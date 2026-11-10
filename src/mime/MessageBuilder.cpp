// GMime/GLib prima di Qt: GLib usa un campo chiamato "signals", che la macro Qt romperebbe.
#include <gmime/gmime.h>

#include "mime/MessageBuilder.h"

#undef signals
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocumentFragment>
#include <QUuid>
#include <memory>
#include <mutex>

#include "mime/MessageParser.h"

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

QString oneLine(QString s);
QString oneLineRaw(const QString& s) { return oneLine(s); }

bool hasControl(const QString& s) {
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u < 0xa0) || u == 0x2028 || u == 0x2029) return true;
    }
    return false;
}

bool validAddr(const QString& a) {
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?$"));
    return re.match(a).hasMatch();
}

struct Mailbox {
    QString name, addr;
};

// Un elemento deve essere esattamente UN mailbox valido (niente liste, gruppi, a-capo).
bool parseMailbox(const QString& item, Mailbox* out, QString* err) {
    initGMime();
    auto fail = [&](const QString& why) {
        if (err) *err = QStringLiteral("Indirizzo non valido \"%1\": %2").arg(item.left(80), why);
        return false;
    };
    if (hasControl(item)) return fail(QStringLiteral("contiene caratteri di controllo"));
    if (item.trimmed().isEmpty()) return fail(QStringLiteral("vuoto"));
    const QByteArray u8 = item.trimmed().toUtf8();
    GRef<InternetAddressList> l(internet_address_list_parse(nullptr, u8.constData()));
    if (!l || internet_address_list_length(l.get()) != 1) return fail(QStringLiteral("deve essere un solo indirizzo"));
    InternetAddress* ia = internet_address_list_get_address(l.get(), 0);
    if (!ia || !INTERNET_ADDRESS_IS_MAILBOX(ia)) return fail(QStringLiteral("non e' un indirizzo singolo"));
    const char* a = internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(ia));
    const char* n = internet_address_get_name(ia);
    Mailbox mb;
    mb.addr = a ? QString::fromUtf8(a).trimmed() : QString();
    mb.name = n ? QString::fromUtf8(n).trimmed() : QString();
    if (!validAddr(mb.addr)) return fail(QStringLiteral("formato dell'indirizzo non accettato"));
    if (hasControl(mb.name)) return fail(QStringLiteral("nome con caratteri di controllo"));
    // Un nome che contiene un secondo indirizzo tra <> indica un tentativo di confusione.
    if (mb.name.contains(QLatin1Char('<')) || mb.name.contains(QLatin1Char('>')))
        return fail(QStringLiteral("nome con caratteri < >"));
    *out = mb;
    return true;
}

bool parseList(const QStringList& items, QList<Mailbox>* out, QString* err) {
    for (const QString& it : items) {
        Mailbox mb;
        if (!parseMailbox(it, &mb, err)) return false;
        out->append(mb);
    }
    return true;
}

// Modalita' bozza: gli elementi non analizzabili vengono restituiti in *raw.
void parseListLenient(const QStringList& items, QList<Mailbox>* out, QStringList* raw) {
    for (const QString& it : items) {
        Mailbox mb;
        if (parseMailbox(it, &mb, nullptr)) out->append(mb);
        else if (!it.trimmed().isEmpty()) raw->append(oneLineRaw(it));
    }
}

// Id con o senza <>; vuoto = invalido.
QString normaliseId(const QString& id, QString* err) {
    QString s = id.trimmed();
    if (s.startsWith(QLatin1Char('<')) && s.endsWith(QLatin1Char('>'))) s = s.mid(1, s.size() - 2);
    bool ok = !s.isEmpty() && !hasControl(s);
    for (const QChar c : s)
        if (c.unicode() > 0x7e || c.isSpace() || c == QLatin1Char('<') || c == QLatin1Char('>')) ok = false;
    if (!ok) {
        if (err) *err = QStringLiteral("Message-ID non valido: \"%1\"").arg(id.left(80));
        return {};
    }
    return s;
}

QString oneLine(QString s) {
    QString r;
    bool lastCtl = false;
    for (const QChar c : s) {
        const bool ctl = c.unicode() < 0x20 || c.unicode() == 0x7f || c == QChar(0x2028) || c == QChar(0x2029);
        if (ctl) {
            if (!lastCtl) r += QLatin1Char(' ');
        } else
            r += c;
        lastCtl = ctl;
    }
    return r.trimmed();
}

QString normaliseNewlines(QString s) {
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return s;
}

GMimeObject* makeText(const QByteArray& data, const char* subtype) {
    bool ascii = true, longLine = false;
    int run = 0;
    for (const char c : data) {
        if (uchar(c) >= 0x80 || c == '\0') ascii = false;
        run = (c == '\n') ? 0 : run + 1;
        if (run > 900) longLine = true;
    }
    GMimeTextPart* tp = g_mime_text_part_new_with_subtype(subtype);
    GMimePart* part = GMIME_PART(tp);
    g_mime_object_set_content_type_parameter(GMIME_OBJECT(part), "charset", "utf-8");
    GRef<GMimeStream> st(g_mime_stream_mem_new_with_buffer(data.constData(), size_t(data.size())));
    GRef<GMimeDataWrapper> dw(g_mime_data_wrapper_new_with_stream(st.get(), GMIME_CONTENT_ENCODING_DEFAULT));
    g_mime_part_set_content(part, dw.get());
    g_mime_part_set_content_encoding(part, (ascii && !longLine) ? GMIME_CONTENT_ENCODING_7BIT
                                                                : GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE);
    return GMIME_OBJECT(tp);
}

QString htmlSignature(const QString& sig) {
    QString h = sig.toHtmlEscaped();
    h.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    return QStringLiteral("<div>-- <br/>") + h + QStringLiteral("</div>");
}

} // namespace

QByteArray MessageBuilder::build(const OutgoingMessage& m, QString* error, BuildMode mode) {
    const bool draft = mode == BuildMode::Draft;
    initGMime();
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QByteArray();
    };

    // Mittente
    Mailbox from;
    from.addr = m.identity.address.trimmed();
    from.name = m.identity.displayName.trimmed();
    if (hasControl(from.name)) return fail(QStringLiteral("Nome del mittente con caratteri di controllo"));
    if (!validAddr(from.addr)) return fail(QStringLiteral("Indirizzo del mittente non valido: \"%1\"").arg(from.addr.left(80)));
    QList<Mailbox> to, cc, bcc;
    QString err;
    QStringList rawTo, rawCc, rawBcc;
    if (draft) {
        parseListLenient(m.to, &to, &rawTo);
        parseListLenient(m.cc, &cc, &rawCc);
        parseListLenient(m.bcc, &bcc, &rawBcc);
    } else if (!parseList(m.to, &to, &err) || !parseList(m.cc, &cc, &err) || !parseList(m.bcc, &bcc, &err)) {
        return fail(err);
    }

    // Identificativi di risposta
    QStringList refs;
    QString replyId;
    if (!m.inReplyToMessageId.trimmed().isEmpty()) {
        replyId = normaliseId(m.inReplyToMessageId, &err);
        if (replyId.isEmpty()) return fail(err);
    }
    for (const QString& r : m.referencesIds) {
        const QString id = normaliseId(r, &err);
        if (id.isEmpty()) return fail(err);
        if (!refs.contains(id)) refs.append(id);
    }
    if (!replyId.isEmpty()) {
        refs.removeAll(replyId);
        refs.append(replyId);
    }
    while (refs.size() > 20) refs.removeFirst();

    // Allegati: controlli di dimensione prima di leggere
    struct Att {
        QString path;
        qint64 size;
    };
    QList<Att> atts;
    qint64 total = 0;
    for (const QString& p : m.attachmentPaths) {
        const QFileInfo fi(p);
        if (!fi.exists() || !fi.isFile() || !fi.isReadable())
            return fail(QStringLiteral("Allegato non leggibile: %1").arg(p));
        if (fi.size() > kMaxAttachmentBytes)
            return fail(QStringLiteral("Allegato troppo grande (oltre 25 MiB): %1").arg(p));
        total += fi.size();
        if (total > kMaxTotalAttachmentBytes) return fail(QStringLiteral("Allegati troppo grandi (oltre 40 MiB in totale)"));
        atts.append({p, fi.size()});
    }

    // Corpo
    QString plain = normaliseNewlines(m.plainText);
    QString html = normaliseNewlines(m.html);
    const bool hasHtml = !html.trimmed().isEmpty();
    if (plain.trimmed().isEmpty() && hasHtml) plain = QTextDocumentFragment::fromHtml(html).toPlainText();
    plain = normaliseNewlines(plain);
    const QString sig = normaliseNewlines(m.identity.signature).trimmed();
    // Gia' firmato: il corpo termina con un blocco "-- " che contiene la firma.
    bool alreadySigned = false;
    if (!sig.isEmpty()) {
        const int d = plain.lastIndexOf(QStringLiteral("\n-- \n"));
        const QString tail = d >= 0 ? plain.mid(d + 5) : (plain.startsWith(QStringLiteral("-- \n")) ? plain.mid(4) : QString());
        alreadySigned = !tail.isNull() && tail.trimmed() == sig;
    }
    if (!draft && !sig.isEmpty() && !alreadySigned) {
        while (plain.endsWith(QLatin1Char('\n'))) plain.chop(1);
        plain += (plain.isEmpty() ? QString() : QStringLiteral("\n\n")) + QStringLiteral("-- \n") + sig + QLatin1Char('\n');
        if (hasHtml) {
            const int b = html.lastIndexOf(QStringLiteral("</body>"), -1, Qt::CaseInsensitive);
            html.insert(b >= 0 ? b : html.size(), htmlSignature(sig));
        }
    } else if (!plain.isEmpty() && !plain.endsWith(QLatin1Char('\n'))) {
        plain += QLatin1Char('\n');
    }

    GRef<GMimeMessage> msg(g_mime_message_new(TRUE));
    const QByteArray fromName = from.name.toUtf8(), fromAddr = from.addr.toUtf8();
    g_mime_message_add_mailbox(msg.get(), GMIME_ADDRESS_TYPE_FROM, fromName.isEmpty() ? nullptr : fromName.constData(),
                               fromAddr.constData());
    auto addAll = [&](const QList<Mailbox>& l, GMimeAddressType t) {
        for (const Mailbox& mb : l) {
            const QByteArray n = mb.name.toUtf8(), a = mb.addr.toUtf8();
            g_mime_message_add_mailbox(msg.get(), t, n.isEmpty() ? nullptr : n.constData(), a.constData());
        }
    };
    addAll(to, GMIME_ADDRESS_TYPE_TO);
    addAll(cc, GMIME_ADDRESS_TYPE_CC);
    // Send: nessun Bcc, i destinatari nascosti restano solo nella busta.
    if (draft) {
        addAll(bcc, GMIME_ADDRESS_TYPE_BCC);
        auto raw = [&](const char* h, const QStringList& l) {
            if (!l.isEmpty())
                g_mime_object_set_header(GMIME_OBJECT(msg.get()), h, l.join(QStringLiteral(", ")).toUtf8().constData(), "utf-8");
        };
        raw("X-Boma-Raw-To", rawTo);
        raw("X-Boma-Raw-Cc", rawCc);
        raw("X-Boma-Raw-Bcc", rawBcc);
    }

    g_mime_message_set_subject(msg.get(), oneLine(m.subject).toUtf8().constData(), "utf-8");

    GDateTime* now = g_date_time_new_now_local();
    g_mime_message_set_date(msg.get(), now);
    g_date_time_unref(now);

    const QString domain = from.addr.mid(from.addr.lastIndexOf(QLatin1Char('@')) + 1);
    const QString mid = QStringLiteral("%1.%2.%3@%4")
                            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
                            .arg(QDateTime::currentMSecsSinceEpoch())
                            .arg(QRandomGenerator::global()->generate(), 0, 16)
                            .arg(domain.isEmpty() ? QStringLiteral("boma.invalid") : domain);
    g_mime_message_set_message_id(msg.get(), mid.toUtf8().constData());

    if (!replyId.isEmpty()) {
        g_mime_object_set_header(GMIME_OBJECT(msg.get()), "In-Reply-To", ("<" + replyId + ">").toUtf8().constData(), "utf-8");
    }
    if (!refs.isEmpty()) {
        QStringList br;
        for (const QString& r : std::as_const(refs)) br << "<" + r + ">";
        g_mime_object_set_header(GMIME_OBJECT(msg.get()), "References", br.join(QLatin1Char(' ')).toUtf8().constData(), "utf-8");
    }
    g_mime_object_set_header(GMIME_OBJECT(msg.get()), "MIME-Version", "1.0", nullptr);

    GMimeObject* top = makeText(plain.toUtf8(), "plain");
    if (hasHtml) {
        GMimeMultipart* alt = g_mime_multipart_new_with_subtype("alternative");
        g_mime_multipart_add(alt, top);
        g_object_unref(top);
        GMimeObject* h = makeText(html.toUtf8(), "html");
        g_mime_multipart_add(alt, h);
        g_object_unref(h);
        top = GMIME_OBJECT(alt);
    }
    if (!atts.isEmpty()) {
        GMimeMultipart* mixed = g_mime_multipart_new_with_subtype("mixed");
        g_mime_multipart_add(mixed, top);
        g_object_unref(top);
        top = GMIME_OBJECT(mixed);
        QMimeDatabase db;
        for (const Att& a : std::as_const(atts)) {
            QFile f(a.path);
            if (!f.open(QIODevice::ReadOnly)) {
                g_object_unref(top);
                return fail(QStringLiteral("Allegato non leggibile: %1").arg(a.path));
            }
            const QByteArray data = f.read(a.size + 1); // lettura limitata: il file puo' crescere
            if (data.size() != a.size || f.error() != QFileDevice::NoError) {
                g_object_unref(top);
                return fail(QStringLiteral("Lettura dell'allegato incompleta: %1").arg(a.path));
            }
            const QString mt = db.mimeTypeForFile(a.path).name();
            const QStringList parts = mt.split(QLatin1Char('/'));
            const QByteArray t = parts.value(0, "application").toLatin1(), s = parts.value(1, "octet-stream").toLatin1();
            GMimePart* ap = g_mime_part_new_with_type(t.constData(), s.constData());
            GRef<GMimeStream> st(g_mime_stream_mem_new_with_buffer(data.constData(), size_t(data.size())));
            GRef<GMimeDataWrapper> dw(g_mime_data_wrapper_new_with_stream(st.get(), GMIME_CONTENT_ENCODING_DEFAULT));
            g_mime_part_set_content(ap, dw.get());
            g_mime_part_set_content_encoding(ap, GMIME_CONTENT_ENCODING_BASE64);
            QString fn = MessageParser::sanitizeFilename(QFileInfo(a.path).fileName());
            if (fn.isEmpty()) fn = QStringLiteral("allegato");
            g_mime_object_set_disposition(GMIME_OBJECT(ap), GMIME_DISPOSITION_ATTACHMENT);
            g_mime_part_set_filename(ap, fn.toUtf8().constData());
            g_mime_multipart_add(mixed, GMIME_OBJECT(ap));
            g_object_unref(ap);
        }
    }
    g_mime_message_set_mime_part(msg.get(), top);
    g_object_unref(top);

    GRef<GMimeStream> out(g_mime_stream_mem_new());
    g_mime_object_write_to_stream(GMIME_OBJECT(msg.get()), nullptr, out.get());
    g_mime_stream_flush(out.get());
    GByteArray* ba = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(out.get()));
    if (!ba || ba->len == 0) return fail(QStringLiteral("Costruzione del messaggio fallita"));
    return QByteArray(reinterpret_cast<const char*>(ba->data), int(ba->len));
}

QStringList MessageBuilder::envelopeRecipients(const OutgoingMessage& m) {
    QStringList out;
    QSet<QString> seen;
    for (const QStringList* l : {&m.to, &m.cc, &m.bcc})
        for (const QString& it : *l) {
            Mailbox mb;
            if (!parseMailbox(it, &mb, nullptr)) continue;
            if (seen.contains(mb.addr.toLower())) continue;
            seen.insert(mb.addr.toLower());
            out.append(mb.addr);
        }
    return out;
}

} // namespace boma
