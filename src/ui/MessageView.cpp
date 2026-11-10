#include "ui/MessageView.h"

#include <QImage>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>

namespace boma {

namespace {

const QSet<QString>& blockedTags() {
    static const QSet<QString> s = {QStringLiteral("script"), QStringLiteral("iframe"), QStringLiteral("object"),
                                    QStringLiteral("embed"),  QStringLiteral("applet"), QStringLiteral("link"),
                                    QStringLiteral("base"),   QStringLiteral("meta"),   QStringLiteral("frame"),
                                    QStringLiteral("frameset"), QStringLiteral("noscript")};
    return s;
}

bool isSafeUrlValue(QString v) {
    v.remove(QRegularExpression(QStringLiteral("[\\s\\x00-\\x1f]")));
    return v.startsWith(QLatin1String("data:image/"), Qt::CaseInsensitive);
}

// Riscrive un singolo tag togliendo gli attributi pericolosi.
QString cleanTag(const QString& tag) {
    static const QRegularExpression head(QStringLiteral("^<\\s*(/?)\\s*([A-Za-z][A-Za-z0-9:-]*)(.*?)(/?)\\s*>$"),
                                         QRegularExpression::DotMatchesEverythingOption);
    const auto hm = head.match(tag);
    if (!hm.hasMatch()) return tag.startsWith(QLatin1String("<!")) ? tag : QString();
    const QString name = hm.captured(2).toLower();
    if (blockedTags().contains(name)) return QString();
    if (!hm.captured(1).isEmpty()) return QStringLiteral("</") + name + QLatin1Char('>');
    static const QRegularExpression attr(
        QStringLiteral("([^\\s=/\"'<>]+)(?:\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s\"'>]*))?"));
    QString out = QLatin1Char('<') + name;
    auto it = attr.globalMatch(hm.captured(3));
    while (it.hasNext()) {
        const auto m = it.next();
        const QString an = m.captured(1).toLower();
        QString val = m.captured(2);
        if (val.size() >= 2 && (val.startsWith(QLatin1Char('"')) || val.startsWith(QLatin1Char('\''))))
            val = val.mid(1, val.size() - 2);
        if (an.startsWith(QLatin1String("on"))) continue;
        if ((an == QLatin1String("src") || an == QLatin1String("background") || an == QLatin1String("srcset") ||
             an == QLatin1String("poster") || an == QLatin1String("data") || an == QLatin1String("action") ||
             an == QLatin1String("formaction")) && !isSafeUrlValue(val))
            continue;
        if (an == QLatin1String("href")) {
            QString c = val;
            c.remove(QRegularExpression(QStringLiteral("[\\s\\x00-\\x1f]")));
            if (c.startsWith(QLatin1String("javascript:"), Qt::CaseInsensitive) ||
                c.startsWith(QLatin1String("vbscript:"), Qt::CaseInsensitive))
                continue;
        }
        if (an == QLatin1String("style") && val.contains(QLatin1String("url("), Qt::CaseInsensitive)) continue;
        out += QLatin1Char(' ') + an;
        if (m.captured(2).size() || m.captured(0).contains(QLatin1Char('=')))
            out += QStringLiteral("=\"") + val.toHtmlEscaped().replace(QLatin1Char('"'), QStringLiteral("&quot;")) +
                   QLatin1Char('"');
    }
    if (!hm.captured(4).isEmpty()) out += QLatin1Char('/');
    return out + QLatin1Char('>');
}

QVariant dataUrlImage(const QUrl& url) {
    const QByteArray enc = url.toEncoded();
    const int comma = enc.indexOf(',');
    if (comma < 0) return {};
    const QByteArray meta = enc.left(comma).mid(5).toLower(); // dopo "data:"
    if (!meta.startsWith("image/")) return {};
    QByteArray payload = enc.mid(comma + 1);
    payload = meta.endsWith(";base64") ? QByteArray::fromBase64(QByteArray::fromPercentEncoding(payload))
                                       : QByteArray::fromPercentEncoding(payload);
    QImage img;
    if (!img.loadFromData(payload)) return {};
    return img;
}

QString esc(const QString& s) { return s.toHtmlEscaped(); }

// Politica unica: solo immagini data: gia' incluse nel messaggio.
QVariant allowedResource(int type, const QUrl& name) {
    if (type == QTextDocument::ImageResource && name.scheme().compare(QLatin1String("data"), Qt::CaseInsensitive) == 0)
        return dataUrlImage(name);
    return {};
}

// QTextDocument::resource() ha un proprio ripiego che legge file locali: lo
// escludiamo sostituendo il documento.
class SafeDocument : public QTextDocument {
public:
    using QTextDocument::QTextDocument;

protected:
    QVariant loadResource(int type, const QUrl& name) override { return allowedResource(type, name); }
};

} // namespace

MessageView::MessageView(QWidget* parent) : QTextBrowser(parent) {
    setDocument(new SafeDocument(this));
    setOpenLinks(false);
    setOpenExternalLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, &MessageView::linkActivated);
}

QString MessageView::sanitizeHtml(const QString& html) {
    QString s = html;
    s.remove(QRegularExpression(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption));
    s.remove(QRegularExpression(
        QStringLiteral("<\\s*(script|iframe|object|embed|applet|noscript)\\b.*?<\\s*/\\s*\\1\\s*>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption));
    // blocchi di stile che caricano risorse
    s.remove(QRegularExpression(QStringLiteral("<\\s*style\\b[^>]*>(?:(?!<\\s*/\\s*style).)*(?:url\\s*\\(|@import)"
                                               "(?:(?!<\\s*/\\s*style).)*<\\s*/\\s*style\\s*>"),
                                QRegularExpression::DotMatchesEverythingOption |
                                    QRegularExpression::CaseInsensitiveOption));
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    QString out;
    out.reserve(s.size());
    int last = 0;
    auto it = tagRe.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        out += cleanTag(m.captured(0));
        last = m.capturedEnd();
    }
    out += s.mid(last);
    return out;
}

void MessageView::show(const ParsedMessage& m) {
    QString h = QStringLiteral("<div style=\"margin-bottom:8px\">");
    auto line = [&](const QString& label, const QString& v) {
        // intestazioni enormi bloccherebbero il layout di QTextDocument
        constexpr int kMaxHeader = 2000;
        if (!v.isEmpty())
            h += QStringLiteral("<b>%1:</b> %2<br>")
                     .arg(label, esc(v.size() > kMaxHeader ? v.left(kMaxHeader) + QStringLiteral("...") : v));
    };
    line(tr("Da"), m.from);
    line(tr("A"), m.to);
    line(tr("Cc"), m.cc);
    line(tr("Oggetto"), m.subject);
    if (m.date.isValid()) line(tr("Data"), QLocale().toString(m.date, QLocale::LongFormat));
    if (!m.attachments.isEmpty()) {
        QStringList names;
        for (const auto& a : m.attachments) names << a.filename;
        line(tr("Allegati"), names.join(QStringLiteral(", ")));
    }
    h += QStringLiteral("</div><hr>");
    if (!m.plainText.isEmpty())
        h += QStringLiteral("<pre style=\"white-space: pre-wrap; font-family: monospace;\">") + esc(m.plainText) +
             QStringLiteral("</pre>");
    else if (!m.html.isEmpty())
        h += sanitizeHtml(m.html);
    setHtml(h);
}

// Politica di privacy: nessuna risorsa esterna, mai. Solo immagini data: gia'
// incluse nel messaggio. Nessun accesso alla rete ne' al disco.
QVariant MessageView::loadResource(int type, const QUrl& name) {
    return allowedResource(type, name);
}

} // namespace boma
