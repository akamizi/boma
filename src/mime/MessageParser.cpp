// GMime/GLib prima di Qt: GLib usa un campo chiamato "signals", che la macro Qt romperebbe.
#include <gmime/gmime.h>

#include "mime/MessageParser.h"
#include <QRegularExpression>

#undef signals
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringConverter>
#include <QTemporaryFile>
#include <QTimeZone>
#include <memory>
#include <mutex>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace boma {

namespace {

void initGMime() {
    static std::once_flag once;
    std::call_once(once, [] { g_mime_init(); });
}

struct GDeleter {
    void operator()(gpointer p) const {
        if (p) g_object_unref(p);
    }
};
template <class T> using GRef = std::unique_ptr<T, GDeleter>;

struct GFreeDel {
    void operator()(gpointer p) const { g_free(p); }
};
using GStr = std::unique_ptr<char, GFreeDel>;

QString fromG(const char* s) { return s ? QString::fromUtf8(s).trimmed() : QString(); }

QString addresses(GMimeMessage* msg, GMimeAddressType t) {
    InternetAddressList* l = g_mime_message_get_addresses(msg, t);
    if (!l) return {};
    GStr s(internet_address_list_to_string(l, nullptr, FALSE));
    return fromG(s.get());
}

QString decodeText(const QByteArray& bytes, const QString& charset) {
    const QByteArray cs = charset.trimmed().toLower().toLatin1();
    if (!cs.isEmpty() && cs != "us-ascii" && cs != "ascii" && cs != "utf-8" && cs != "utf8") {
        QStringDecoder dec(cs.constData());
        if (dec.isValid()) {
            const QString out = dec.decode(bytes);
            if (!dec.hasError() || !out.isEmpty()) return out;
        }
    }
    return QString::fromUtf8(bytes); // sostituisce le sequenze non valide
}

// Contenuto decodificato (transfer-encoding rimosso) di una parte foglia.
QByteArray partContent(GMimeObject* obj) {
    GRef<GMimeStream> out(g_mime_stream_mem_new());
    if (GMIME_IS_PART(obj)) {
        GMimeDataWrapper* dw = g_mime_part_get_content(GMIME_PART(obj));
        if (dw) g_mime_data_wrapper_write_to_stream(dw, out.get());
    } else if (GMIME_IS_MESSAGE_PART(obj)) {
        GMimeMessage* inner = g_mime_message_part_get_message(GMIME_MESSAGE_PART(obj));
        if (inner) g_mime_object_write_to_stream(GMIME_OBJECT(inner), nullptr, out.get());
    }
    g_mime_stream_flush(out.get());
    GByteArray* ba = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(out.get()));
    return ba ? QByteArray(reinterpret_cast<const char*>(ba->data), int(ba->len)) : QByteArray();
}

QString mimeTypeOf(GMimeObject* obj) {
    GMimeContentType* ct = g_mime_object_get_content_type(obj);
    if (!ct) return QStringLiteral("application/octet-stream");
    GStr s(g_mime_content_type_get_mime_type(ct));
    return fromG(s.get()).toLower();
}

bool isMultipart(GMimeObject* o) { return GMIME_IS_MULTIPART(o); }

// Byte esatti della prima parte di un multipart come trasmessi (RFC 3156):
// dopo la riga di delimitatore iniziale (fine riga escluso) fino al fine riga che
// precede il delimitatore successivo (escluso). Nessuna rielaborazione.
QByteArray exactFirstPart(const QByteArray& raw, const QByteArray& boundary) {
    if (boundary.isEmpty()) return {};
    const QByteArray delim = "--" + boundary;
    // Se la riga a partire da pos e' un delimitatore, restituisce la fine della
    // riga (indice dopo l'fine riga) e imposta closing; altrimenti -1.
    auto delimAt = [&](qsizetype pos, bool* closing, qsizetype* lineStart) -> qsizetype {
        if (raw.mid(pos, delim.size()) != delim) return -1;
        qsizetype i = pos + delim.size();
        *closing = false;
        if (raw.mid(i, 2) == "--") {
            *closing = true;
            i += 2;
        }
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
        *lineStart = pos;
        if (i >= raw.size()) return raw.size();
        if (raw[i] == '\n') return i + 1;
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') return i + 2;
        return -1;
    };
    qsizetype pos = 0;
    qsizetype start = -1;
    while (pos <= raw.size()) {
        bool closing = false;
        qsizetype ls = 0;
        const qsizetype next = delimAt(pos, &closing, &ls);
        if (next >= 0) {
            if (start < 0) {
                if (closing) return {};
                start = next;
            } else {
                qsizetype end = pos;
                if (end > start && raw[end - 1] == '\n') {
                    --end;
                    if (end > start && raw[end - 1] == '\r') --end;
                }
                return raw.mid(start, end - start);
            }
        }
        const qsizetype nl = raw.indexOf('\n', pos);
        if (nl < 0) break;
        pos = nl + 1;
    }
    return {};
}

constexpr qsizetype kMaxInput = qsizetype(64) * 1024 * 1024;
constexpr int kMaxMultipartMarkers = 500;

// Controllo economico prima di GMime (il cui parser e la cui distruzione sono
// ricorsivi). Limiti: 64 MB di input e al massimo 500 marcatori nel messaggio.
// Il conteggio e' volutamente grossolano: ogni occorrenza della parola
// "multipart" o "rfc822" (senza distinguere maiuscole e senza guardare cosa
// segue, quindi immune a commenti RFC 822 e intestazioni piegate) e' un
// marcatore. Il totale limita anche l'annidamento; sono possibili falsi
// positivi in messaggi con piu' di 500 parti multipart/rfc822.
bool tooBigOrDeep(const QByteArray& raw, QString* why) {
    if (raw.size() > kMaxInput) {
        if (why) *why = QStringLiteral("messaggio troppo grande");
        return true;
    }
    const QByteArray low = raw.toLower();
    int n = 0;
    for (const char* word : {"multipart", "rfc822"}) {
        const qsizetype wl = qsizetype(std::strlen(word));
        for (qsizetype i = low.indexOf(word); i >= 0; i = low.indexOf(word, i + wl))
            if (++n > kMaxMultipartMarkers) {
                if (why) *why = QStringLiteral("troppi livelli MIME annidati");
                return true;
            }
    }
    return false;
}

// Umask del processo letta senza modificarla (umask() e' globale e non thread-safe).
mode_t currentUmask() {
    QFile f(QStringLiteral("/proc/self/status"));
    if (f.open(QIODevice::ReadOnly)) {
        while (!f.atEnd()) {
            const QByteArray l = f.readLine();
            if (l.startsWith("Umask:")) {
                bool ok = false;
                const uint v = l.mid(6).trimmed().toUInt(&ok, 8);
                if (ok && v <= 0777) return mode_t(v);
            }
        }
    }
    return 022;
}

struct Parsed {
    GRef<GMimeStream> stream;
    GRef<GMimeParser> parser;
    GRef<GMimeMessage> msg;
};

bool construct(const QByteArray& raw, Parsed& p, bool prechecked = false) {
    initGMime();
    if (raw.isEmpty()) return false;
    if (!prechecked && tooBigOrDeep(raw, nullptr)) return false;
    p.stream.reset(g_mime_stream_mem_new_with_buffer(raw.constData(), raw.size()));
    if (!p.stream) return false;
    p.parser.reset(g_mime_parser_new_with_stream(p.stream.get()));
    if (!p.parser) return false;
    g_mime_parser_set_format(p.parser.get(), GMIME_FORMAT_MESSAGE);
    p.msg.reset(g_mime_parser_construct_message(p.parser.get(), nullptr));
    return bool(p.msg);
}

constexpr int kMaxDepth = 64; // difesa da annidamenti patologici

QByteArray boundaryOf(GMimeMultipart* mp) {
    const char* b = g_mime_multipart_get_boundary(mp);
    return b ? QByteArray(b) : QByteArray();
}

struct Walker {
    const QByteArray* raw = nullptr;
    ParsedMessage* out = nullptr;
    int nextIndex = 0;
    int wantIndex = -1;
    GMimeObject* found = nullptr;
    bool plainSet = false, htmlSet = false;

    static bool isAttachmentLike(GMimeObject* o, const QString& type) {
        GMimeContentDisposition* cd = g_mime_object_get_content_disposition(o);
        if (cd && g_mime_content_disposition_is_attachment(cd)) return true;
        if (GMIME_IS_PART(o) && g_mime_part_get_filename(GMIME_PART(o))) return true;
        return !(type == QLatin1String("text/plain") || type == QLatin1String("text/html"));
    }

    void leaf(GMimeObject* o, bool hidden) {
        const int idx = nextIndex++;
        if (wantIndex >= 0) {
            if (idx == wantIndex) found = o;
            return;
        }
        if (!out || hidden) return;
        const QString type = mimeTypeOf(o);
        if (isAttachmentLike(o, type)) {
            Attachment a;
            const char* fn = GMIME_IS_PART(o) ? g_mime_part_get_filename(GMIME_PART(o)) : nullptr;
            a.filename = MessageParser::sanitizeFilename(fromG(fn));
            a.mimeType = type;
            a.size = partContent(o).size();
            a.partIndex = idx;
            out->attachments.append(a);
            return;
        }
        const bool isHtml = type == QLatin1String("text/html");
        if ((isHtml ? htmlSet : plainSet)) return;
        (isHtml ? htmlSet : plainSet) = true;
        GMimeContentType* ct = g_mime_object_get_content_type(o);
        const QString charset = ct ? fromG(g_mime_content_type_get_parameter(ct, "charset")) : QString();
        (isHtml ? out->html : out->plainText) = decodeText(partContent(o), charset);
    }

    // hidden: parti (firma, testo cifrato) che non sono allegati da mostrare.
    void walk(GMimeObject* o, int depth, bool hidden) {
        if (!o || depth > kMaxDepth) return;
        if (!isMultipart(o)) {
            leaf(o, hidden);
            return;
        }
        GMimeMultipart* mp = GMIME_MULTIPART(o);
        const int n = g_mime_multipart_get_count(mp);
        const QString type = mimeTypeOf(o);
        GMimeContentType* ct = g_mime_object_get_content_type(o);
        const QString proto = ct ? fromG(g_mime_content_type_get_parameter(ct, "protocol")).toLower() : QString();
        const bool signedPgp = type == QLatin1String("multipart/signed") && proto == QLatin1String("application/pgp-signature");
        const bool encPgp = type == QLatin1String("multipart/encrypted") && proto == QLatin1String("application/pgp-encrypted");
        if (out && wantIndex < 0) {
            if (signedPgp && n >= 2 && !out->signedPgp && !out->encryptedPgp) {
                out->signedPgp = true;
                out->rawSignedPart = raw ? exactFirstPart(*raw, boundaryOf(mp)) : QByteArray();
                GMimeObject* sig = g_mime_multipart_get_part(mp, 1);
                if (sig) out->rawSignature = partContent(sig);
            } else if (encPgp && n >= 2 && !out->signedPgp && !out->encryptedPgp) {
                out->encryptedPgp = true;
                GMimeObject* c = g_mime_multipart_get_part(mp, 1);
                if (c) out->rawEncrypted = partContent(c);
            }
        }
        for (int i = 0; i < n; ++i) {
            const bool h = hidden || (signedPgp && i == 1) || (encPgp && out && wantIndex < 0);
            walk(g_mime_multipart_get_part(mp, i), depth + 1, h);
        }
    }
};

bool findPart(const QByteArray& raw, int partIndex, Parsed& p, GMimeObject** part) {
    *part = nullptr;
    if (partIndex < 0 || !construct(raw, p)) return false;
    Walker w;
    w.wantIndex = partIndex;
    w.walk(g_mime_message_get_mime_part(p.msg.get()), 0, false);
    *part = w.found;
    return w.found != nullptr;
}

} // namespace

ParsedMessage MessageParser::parse(const QByteArray& raw, QString* error) {
    ParsedMessage r;
    Parsed p;
    QString why;
    if (tooBigOrDeep(raw, &why)) {
        if (error) *error = why;
        return r;
    }
    if (!construct(raw, p, true)) {
        if (error) *error = raw.isEmpty() ? QStringLiteral("messaggio vuoto") : QStringLiteral("messaggio non interpretabile");
        return r;
    }
    GMimeMessage* msg = p.msg.get();
    r.subject = fromG(g_mime_message_get_subject(msg));
    r.from = addresses(msg, GMIME_ADDRESS_TYPE_FROM);
    r.to = addresses(msg, GMIME_ADDRESS_TYPE_TO);
    r.cc = addresses(msg, GMIME_ADDRESS_TYPE_CC);
    r.replyTo = addresses(msg, GMIME_ADDRESS_TYPE_REPLY_TO);
    r.messageId = fromG(g_mime_message_get_message_id(msg));
    if (const char* refs = g_mime_object_get_header(GMIME_OBJECT(msg), "References")) {
        static const QRegularExpression idRe(QStringLiteral("<([^<>\\s]+)>"));
        auto it = idRe.globalMatch(QString::fromUtf8(refs));
        while (it.hasNext() && r.references.size() < 200) r.references << it.next().captured(1);
    }
    if (GDateTime* d = g_mime_message_get_date(msg)) {
        const qint64 secs = g_date_time_to_unix(d);
        const int off = int(g_date_time_get_utc_offset(d) / G_USEC_PER_SEC);
        r.date = QDateTime::fromSecsSinceEpoch(secs, QTimeZone::fromSecondsAheadOfUtc(off));
    }
    Walker w;
    w.out = &r;
    w.raw = &raw;
    w.walk(g_mime_message_get_mime_part(msg), 0, false);
    if (error) error->clear();
    return r;
}

QByteArray MessageParser::attachmentData(const QByteArray& raw, int partIndex) {
    Parsed p;
    GMimeObject* part = nullptr;
    if (!findPart(raw, partIndex, p, &part)) return {};
    return partContent(part);
}

bool MessageParser::saveAttachment(const QByteArray& raw, int partIndex, const QString& destPath, QString* error) {
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        return false;
    };
    Parsed p;
    GMimeObject* part = nullptr;
    if (!findPart(raw, partIndex, p, &part)) return fail(QStringLiteral("allegato non trovato"));
    const QByteArray data = partContent(part);
    const QFileInfo fi(destPath);
    if (destPath.isEmpty() || fi.fileName().isEmpty()) return fail(QStringLiteral("percorso non valido"));
    const QByteArray dest = QFile::encodeName(fi.absoluteFilePath());
    // lstat: un collegamento simbolico (anche pendente) conta come esistente
    if (::access(dest.constData(), F_OK) == 0 || QFileInfo(fi.absoluteFilePath()).isSymLink())
        return fail(QStringLiteral("il file esiste gia': %1").arg(destPath));
    QTemporaryFile tmp(fi.absoluteDir().filePath(QStringLiteral(".boma-XXXXXX.part")));
    tmp.setAutoRemove(true);
    if (!tmp.open()) return fail(QStringLiteral("impossibile creare il file temporaneo: %1").arg(tmp.errorString()));
    if (tmp.write(data) != data.size() || !tmp.flush()) return fail(QStringLiteral("scrittura non riuscita: %1").arg(tmp.errorString()));
    // QTemporaryFile crea con 0600: i file salvati rispettano invece la umask.
    ::fchmod(tmp.handle(), 0666 & ~currentUmask());
    ::fsync(tmp.handle());
    const QByteArray src = QFile::encodeName(tmp.fileName());
    // link() non sostituisce mai la destinazione; il file temporaneo viene rimosso dopo.
    if (::link(src.constData(), dest.constData()) == 0) return true;
    int e = errno;
    if (e == EEXIST) return fail(QStringLiteral("il file esiste gia': %1").arg(destPath));
    // filesystem senza hard link: renameat2 con RENAME_NOREPLACE
#ifdef RENAME_NOREPLACE
    if (::renameat2(AT_FDCWD, src.constData(), AT_FDCWD, dest.constData(), RENAME_NOREPLACE) == 0) {
        tmp.setAutoRemove(false);
        return true;
    }
    e = errno; // errore dell'ultimo tentativo
    if (e == EEXIST) return fail(QStringLiteral("il file esiste gia': %1").arg(destPath));
#endif
    return fail(QStringLiteral("salvataggio non riuscito: %1").arg(QString::fromLocal8Bit(std::strerror(e))));
}

QString MessageParser::sanitizeFilename(const QString& name) {
    QString s = name;
    const int cut = qMax(s.lastIndexOf(QLatin1Char('/')), s.lastIndexOf(QLatin1Char('\\')));
    if (cut >= 0) s = s.mid(cut + 1);
    QList<char32_t> cps;
    for (const char32_t c : s.toUcs4()) {
        const auto cat = QChar::category(c);
        if (cat == QChar::Other_Control || cat == QChar::Other_Format || cat == QChar::Other_Surrogate ||
            cat == QChar::Other_NotAssigned || cat == QChar::Separator_Line || cat == QChar::Separator_Paragraph ||
            c == U'"' || c == U'\'' || c == U'\\' || c == U'/')
            continue;
        cps.append(c);
    }
    qsizetype lo = 0, hi = cps.size();
    auto trimSpace = [&] {
        while (lo < hi && QChar::isSpace(cps[lo])) ++lo;
        while (hi > lo && QChar::isSpace(cps[hi - 1])) --hi;
    };
    trimSpace();
    while (lo < hi && cps[lo] == U'.') ++lo;
    trimSpace();
    cps = cps.mid(lo, hi - lo);
    auto cpBytes = [](char32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; };
    constexpr qint64 kMaxBytes = 200;
    qint64 total = 0;
    for (char32_t c : std::as_const(cps)) total += cpBytes(c);
    if (total > kMaxBytes) {
        // conserva l'estensione (se breve), taglia la base su un confine di code point (lineare)
        qsizetype dot = -1;
        for (qsizetype i = cps.size() - 1; i > 0 && cps.size() - i <= 21; --i)
            if (cps[i] == U'.') { dot = i; break; }
        qint64 extBytes = 0;
        for (qsizetype i = dot; dot > 0 && i < cps.size(); ++i) extBytes += cpBytes(cps[i]);
        if (dot <= 0 || extBytes > 20) { dot = cps.size(); extBytes = 0; }
        qint64 acc = 0;
        qsizetype keep = 0;
        while (keep < dot && acc + cpBytes(cps[keep]) + extBytes <= kMaxBytes) acc += cpBytes(cps[keep++]);
        cps = cps.mid(0, keep) + cps.mid(dot);
        lo = 0;
        hi = cps.size();
        trimSpace();
        cps = cps.mid(lo, hi - lo);
    }
    const QString out = QString::fromUcs4(cps.constData(), cps.size());
    return out.isEmpty() ? QStringLiteral("allegato") : out;
}

} // namespace boma
