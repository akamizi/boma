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

bool isWs(QChar c) { return c.isSpace() || c.unicode() < 0x20; }

// Nome del tag con scansione manuale (nessuna regex con backtracking).
// Restituisce il nome in minuscolo (vuoto se non e' un tag) e la posizione dopo il nome.
QString tagName(const QString& tag, bool* closing, qsizetype* after) {
    qsizetype i = 1;
    const qsizetype n = tag.size();
    while (i < n && isWs(tag[i])) ++i;
    *closing = i < n && tag[i] == QLatin1Char('/');
    if (*closing) ++i;
    while (i < n && isWs(tag[i])) ++i;
    const qsizetype st = i;
    if (i >= n || !tag[i].isLetter() || tag[i].unicode() > 127) return {};
    while (i < n && tag[i].unicode() < 128 && (tag[i].isLetterOrNumber() || tag[i] == QLatin1Char(':') || tag[i] == QLatin1Char('-')))
        ++i;
    *after = i;
    return tag.mid(st, i - st).toLower();
}

// Riscrive un singolo tag togliendo gli attributi pericolosi. Tempo lineare.
QString cleanTag(const QString& tag) {
    bool closing = false;
    qsizetype i = 0;
    const QString name = tagName(tag, &closing, &i);
    if (name.isEmpty() || blockedTags().contains(name)) return QString();
    if (closing) return QStringLiteral("</") + name + QLatin1Char('>');
    const qsizetype end = tag.size() - 1; // posizione del '>' finale
    bool selfClose = false;
    QString out = QLatin1Char('<') + name;
    while (i < end) {
        while (i < end && (isWs(tag[i]) || tag[i] == QLatin1Char('/'))) {
            selfClose = tag[i] == QLatin1Char('/');
            ++i;
        }
        if (i >= end) break;
        selfClose = false;
        const qsizetype ns = i;
        while (i < end && !isWs(tag[i]) && tag[i] != QLatin1Char('=') && tag[i] != QLatin1Char('/') &&
               tag[i] != QLatin1Char('"') && tag[i] != QLatin1Char('\'') && tag[i] != QLatin1Char('<'))
            ++i;
        if (i == ns) { ++i; continue; } // carattere isolato inatteso
        const QString an = tag.mid(ns, i - ns).toLower();
        qsizetype j = i;
        while (j < end && isWs(tag[j])) ++j;
        QString val;
        bool hasValue = false;
        if (j < end && tag[j] == QLatin1Char('=')) {
            hasValue = true;
            ++j;
            while (j < end && isWs(tag[j])) ++j;
            if (j < end && (tag[j] == QLatin1Char('"') || tag[j] == QLatin1Char('\''))) {
                const QChar q = tag[j];
                const qsizetype vs = ++j;
                while (j < end && tag[j] != q) ++j;
                val = tag.mid(vs, j - vs);
                if (j < end) ++j;
            } else {
                const qsizetype vs = j;
                while (j < end && !isWs(tag[j]) && tag[j] != QLatin1Char('>')) ++j;
                val = tag.mid(vs, j - vs);
            }
            i = j;
        }
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
        if (hasValue)
            out += QStringLiteral("=\"") + val.toHtmlEscaped().replace(QLatin1Char('"'), QStringLiteral("&quot;")) +
                   QLatin1Char('"');
    }
    if (selfClose) out += QLatin1Char('/');
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

// Righe lunghissime senza spazi rendono quadratico il layout di Qt: ogni 200
// caratteri consecutivi senza spazio inseriamo un U+200B (invisibile).
QString softWrap(QStringView t, bool protectEntities = true) {
    QString out;
    out.reserve(t.size() + t.size() / 200 + 1);
    int run = 0;
    int ent = 0; // caratteri rimasti in una possibile entita' &...; (mai spezzata)
    for (const QChar c : t) {
        if (protectEntities && c == QLatin1Char('&') && ent == 0) ent = 13; // una '&' non rinnova la finestra
        const bool inside = ent > 0; // anche il ';' finale appartiene all'entita'
        if (c == QLatin1Char(';') || c.isSpace()) ent = 0;
        else if (ent > 0) --ent;
        if (c.isSpace()) run = 0;
        else if (++run > 200 && (!inside || run > 213)) { // al piu' 13 caratteri di rinvio
            out += QChar(0x200B);
            run = 1;
        }
        out += c;
    }
    return out;
}

// Qui l'escape avviene dopo softWrap: nessuna protezione delle entita'.
QString esc(const QString& s) { return softWrap(s, false).toHtmlEscaped(); }

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

// Scansione in un solo passaggio (tempo lineare: ogni ricerca avanza sempre).
QString MessageView::sanitizeHtml(const QString& s, qsizetype* consumed) {
    constexpr int kMaxTags = 20000; // oltre, il layout non e' piu' limitato: si tronca
    int tags = 0;
    if (consumed) *consumed = s.size();
    constexpr int kMaxTag = 20000;
    static const QSet<QString> dropBlock = {QStringLiteral("script"), QStringLiteral("iframe"),
                                            QStringLiteral("object"), QStringLiteral("embed"),
                                            QStringLiteral("applet"), QStringLiteral("noscript")};
    QString out;
    out.reserve(s.size());
    const qsizetype n = s.size();
    qsizetype i = 0;
    qsizetype gt = -1; // ultima posizione di '>' trovata (mai dietro i)
    bool noGt = false;
    while (i < n) {
        const qsizetype lt = s.indexOf(QLatin1Char('<'), i);
        if (lt < 0) {
            out += softWrap(QStringView(s).mid(i));
            break;
        }
        out += softWrap(QStringView(s).mid(i, lt - i));
        const QChar nx = lt + 1 < n ? s[lt + 1] : QChar();
        if (!(nx.isLetter() || nx == QLatin1Char('/') || nx == QLatin1Char('!') || nx == QLatin1Char('?'))) {
            out += QLatin1String("&lt;");
            i = lt + 1;
            continue;
        }
        if (QStringView(s).mid(lt, 4) == QLatin1String("<!--")) {
            const qsizetype e = s.indexOf(QLatin1String("-->"), lt + 4);
            if (e < 0) break; // commento non chiuso: scarta il resto
            i = e + 3;
            continue;
        }
        if (!noGt && gt < lt) {
            gt = s.indexOf(QLatin1Char('>'), lt);
            if (gt < 0) noGt = true;
        }
        if (noGt || gt - lt > kMaxTag) {
            out += QLatin1String("&lt;");
            i = lt + 1;
            continue;
        }
        if (++tags > kMaxTags) {
            if (consumed) *consumed = lt;
            break;
        }
        const QString tag = s.mid(lt, gt - lt + 1);
        i = gt + 1;
        if (tag.startsWith(QLatin1String("<!"))) {
            if (tag.startsWith(QLatin1String("<!doctype"), Qt::CaseInsensitive)) out += tag;
            continue;
        }
        bool isClosing = false;
        qsizetype afterName = 0;
        const QString name = tagName(tag, &isClosing, &afterName);
        if (!name.isEmpty() && !isClosing &&
            (dropBlock.contains(name) || name == QLatin1String("style"))) {
            const qsizetype close = s.indexOf(QLatin1String("</") + name, i, Qt::CaseInsensitive);
            if (close < 0) break; // blocco non chiuso: scarta il resto
            qsizetype end = s.indexOf(QLatin1Char('>'), close);
            end = end < 0 ? n : end + 1;
            if (name == QLatin1String("style")) {
                const QStringView body = QStringView(s).mid(i, close - i);
                if (!body.contains(QLatin1String("url("), Qt::CaseInsensitive) &&
                    !body.contains(QLatin1String("@import"), Qt::CaseInsensitive) &&
                    !body.contains(QLatin1String("expression("), Qt::CaseInsensitive)) {
                    out += cleanTag(tag) + body + QLatin1String("</style>");
                }
            }
            i = end;
            continue;
        }
        out += cleanTag(tag);
    }
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
    // Limite di visualizzazione (il layout di QTextDocument non regge corpi enormi).
    constexpr int kMaxBody = 1000000;
    auto note = [&](const QString& shown) {
        return QStringLiteral("<p><i>Messaggio troncato: %1 byte mostrati</i></p>").arg(shown.toUtf8().size());
    };
    if (!m.plainText.isEmpty()) {
        const bool cut = m.plainText.size() > kMaxBody;
        const QString t = cut ? m.plainText.left(kMaxBody) : m.plainText;
        h += QStringLiteral("<pre style=\"white-space: pre-wrap; font-family: monospace;\">") + esc(t) +
             QStringLiteral("</pre>");
        if (cut) h += note(t);
    } else if (!m.html.isEmpty()) {
        const bool cut = m.html.size() > kMaxBody;
        const QString t = cut ? m.html.left(kMaxBody) : m.html;
        qsizetype used = 0;
        h += sanitizeHtml(t, &used);
        if (cut || used < t.size()) h += note(t.left(used));
    }
    setHtml(h);
}

// Politica di privacy: nessuna risorsa esterna, mai. Solo immagini data: gia'
// incluse nel messaggio. Nessun accesso alla rete ne' al disco.
QVariant MessageView::loadResource(int type, const QUrl& name) {
    return allowedResource(type, name);
}

} // namespace boma
