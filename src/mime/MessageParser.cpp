// GMime/GLib prima di Qt: GLib usa un campo chiamato "signals", che la macro Qt romperebbe.
#include <gmime/gmime.h>

#include "mime/MessageParser.h"

#undef signals
#include <QDir>
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

QByteArray serialize(GMimeObject* obj, bool crlf) {
    GRef<GMimeStream> out(g_mime_stream_mem_new());
    GMimeFormatOptions* opts = g_mime_format_options_new();
    g_mime_format_options_set_newline_format(opts, crlf ? GMIME_NEWLINE_FORMAT_DOS : GMIME_NEWLINE_FORMAT_UNIX);
    g_mime_object_write_to_stream(obj, opts, out.get());
    g_mime_format_options_free(opts);
    g_mime_stream_flush(out.get());
    GByteArray* ba = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(out.get()));
    return ba ? QByteArray(reinterpret_cast<const char*>(ba->data), int(ba->len)) : QByteArray();
}

struct Parsed {
    GRef<GMimeStream> stream;
    GRef<GMimeParser> parser;
    GRef<GMimeMessage> msg;
};

bool construct(const QByteArray& raw, Parsed& p) {
    initGMime();
    if (raw.isEmpty()) return false;
    p.stream.reset(g_mime_stream_mem_new_with_buffer(raw.constData(), raw.size()));
    if (!p.stream) return false;
    p.parser.reset(g_mime_parser_new_with_stream(p.stream.get()));
    if (!p.parser) return false;
    g_mime_parser_set_format(p.parser.get(), GMIME_FORMAT_MESSAGE);
    p.msg.reset(g_mime_parser_construct_message(p.parser.get(), nullptr));
    return bool(p.msg);
}

constexpr int kMaxDepth = 64; // difesa da annidamenti patologici

struct Walker {
    bool rawCrlf = false;
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
                out->rawSignedPart = serialize(g_mime_multipart_get_part(mp, 0), rawCrlf);
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
    if (!construct(raw, p)) {
        if (error) *error = raw.isEmpty() ? QStringLiteral("messaggio vuoto") : QStringLiteral("messaggio non interpretabile");
        return r;
    }
    GMimeMessage* msg = p.msg.get();
    r.subject = fromG(g_mime_message_get_subject(msg));
    r.from = addresses(msg, GMIME_ADDRESS_TYPE_FROM);
    r.to = addresses(msg, GMIME_ADDRESS_TYPE_TO);
    r.cc = addresses(msg, GMIME_ADDRESS_TYPE_CC);
    if (GDateTime* d = g_mime_message_get_date(msg)) {
        const qint64 secs = g_date_time_to_unix(d);
        const int off = int(g_date_time_get_utc_offset(d) / G_USEC_PER_SEC);
        r.date = QDateTime::fromSecsSinceEpoch(secs, QTimeZone::fromSecondsAheadOfUtc(off));
    }
    Walker w;
    w.out = &r;
    w.rawCrlf = raw.contains("\r\n");
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
    ::fsync(tmp.handle());
    const QByteArray src = QFile::encodeName(tmp.fileName());
    // link() non sostituisce mai la destinazione; il file temporaneo viene rimosso dopo.
    if (::link(src.constData(), dest.constData()) == 0) return true;
    const int e = errno;
    if (e == EEXIST) return fail(QStringLiteral("il file esiste gia': %1").arg(destPath));
    // filesystem senza hard link: renameat2 con RENAME_NOREPLACE
#ifdef RENAME_NOREPLACE
    if (::renameat2(AT_FDCWD, src.constData(), AT_FDCWD, dest.constData(), RENAME_NOREPLACE) == 0) {
        tmp.setAutoRemove(false);
        return true;
    }
    if (errno == EEXIST) return fail(QStringLiteral("il file esiste gia': %1").arg(destPath));
#endif
    return fail(QStringLiteral("salvataggio non riuscito: %1").arg(QString::fromLocal8Bit(std::strerror(e))));
}

QString MessageParser::sanitizeFilename(const QString& name) {
    QString s = name;
    const int cut = qMax(s.lastIndexOf(QLatin1Char('/')), s.lastIndexOf(QLatin1Char('\\')));
    if (cut >= 0) s = s.mid(cut + 1);
    QString out;
    for (const QChar c : s) {
        const auto cat = c.category();
        if (cat == QChar::Other_Control || cat == QChar::Other_Format || cat == QChar::Other_Surrogate ||
            cat == QChar::Other_NotAssigned || c == QLatin1Char('"') || c == QLatin1Char('\'') ||
            c == QLatin1Char('\\') || c == QLatin1Char('/') || c == QChar::LineSeparator ||
            c == QChar::ParagraphSeparator)
            continue;
        out.append(c);
    }
    out = out.trimmed();
    while (out.startsWith(QLatin1Char('.'))) out.remove(0, 1);
    out = out.trimmed();
    if (out.size() > 200) out = out.left(200).trimmed();
    return out.isEmpty() ? QStringLiteral("allegato") : out;
}

} // namespace boma
