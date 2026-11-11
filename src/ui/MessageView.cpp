#include "ui/MessageView.h"
#include <QAction>
#include <QMenu>

#include <QImage>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
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

    bar_ = new QWidget(this);
    auto* lay = new QVBoxLayout(bar_);
    lay->setContentsMargins(4, 2, 4, 2);
    lay->setSpacing(2);
    badge_ = new QLabel(bar_);
    badge_->setTextFormat(Qt::PlainText); // il firmatario e' testo, mai HTML
    badge_->setWordWrap(true);
    badge_->hide();
    lay->addWidget(badge_);
    encRow_ = new QWidget(bar_);
    auto* row = new QHBoxLayout(encRow_);
    row->setContentsMargins(0, 0, 0, 0);
    auto* encLabel = new QLabel(tr("Messaggio cifrato"), encRow_);
    row->addWidget(encLabel, 1);
    decryptButton_ = new QPushButton(tr("Decifra"), encRow_);
    row->addWidget(decryptButton_);
    encRow_->hide();
    lay->addWidget(encRow_);
    attachButton_ = new QToolButton(bar_);
    attachButton_->setText(tr("Salva allegato..."));
    attachButton_->setPopupMode(QToolButton::InstantPopup);
    attachButton_->setMenu(new QMenu(attachButton_));
    attachButton_->hide();
    lay->addWidget(attachButton_, 0, Qt::AlignLeft);
    errLabel_ = new QLabel(bar_);
    errLabel_->setTextFormat(Qt::PlainText);
    errLabel_->setWordWrap(true);
    errLabel_->setStyleSheet(QStringLiteral("color:#c62828"));
    errLabel_->hide();
    lay->addWidget(errLabel_);
    bar_->hide();
    connect(decryptButton_, &QPushButton::clicked, this, &MessageView::onDecryptClicked);
}

void MessageView::resizeEvent(QResizeEvent* e) {
    QTextBrowser::resizeEvent(e);
    layoutBar();
}

void MessageView::layoutBar() {
    updateBadge();
    errLabel_->setText(decryptError_);
    errLabel_->setVisible(!decryptError_.isEmpty());
    encRow_->setVisible(encryptedShown_);
    attachButton_->setVisible(!encryptedShown_ && !msg_.attachments.isEmpty());
    const bool any = encryptedShown_ || badge_->isVisibleTo(bar_) || errLabel_->isVisibleTo(bar_) ||
                     attachButton_->isVisibleTo(bar_);
    int h = 0;
    if (any) {
        bar_->layout()->activate();
        h = bar_->layout()->hasHeightForWidth() ? bar_->layout()->totalHeightForWidth(width())
                                                : bar_->sizeHint().height();
    }
    setViewportMargins(0, h, 0, 0);
    bar_->setGeometry(0, 0, width(), h);
    bar_->setVisible(any);
}

void MessageView::onDecryptClicked() {
    const bool yes = confirm_ ? confirm_()
                              : QMessageBox::question(this, tr("Decifra"),
                                                      tr("Decifrare il messaggio? Ti verrà chiesta la password della chiave.")) ==
                                    QMessageBox::Yes;
    if (!yes) return;
    decryptButton_->setEnabled(false); // niente doppie richieste
    emit decryptRequested();
}

void MessageView::setSignatureResult(SignatureStatus status, const QString& signer) {
    sigStatus_ = status;
    signer_ = signer;
    layoutBar();
}

void MessageView::showDecrypted(const ParsedMessage& m) {
    // La verifica di un'eventuale firma interna non e' prevista nella v1.
    encrypted_.clear();
    encryptedShown_ = false;
    decryptError_.clear();
    sigStatus_ = SignatureStatus::None;
    signer_.clear();
    msg_ = m;
    msg_.encryptedPgp = false;
    updateAttachments();
    render();
    layoutBar();
}

void MessageView::showDecryptError(const QString& error) {
    decryptError_ = error.isEmpty() ? tr("Decifrazione non riuscita.") : error;
    decryptButton_->setEnabled(true);
    render(); // nessun testo parziale: si mostra solo l'errore
    layoutBar();
}

namespace {
// Indirizzo e-mail (minuscolo) di "Nome <a@b>" o "a@b"; vuoto se non riconoscibile.
QString addressOf(const QString& s) {
    const int lt = s.lastIndexOf(QLatin1Char('<'));
    const int gt = s.lastIndexOf(QLatin1Char('>'));
    QString a = (lt >= 0 && gt > lt) ? s.mid(lt + 1, gt - lt - 1) : s;
    a = a.trimmed().toLower();
    if (a.count(QLatin1Char('@')) != 1 || a.contains(QLatin1Char(' ')) || a.startsWith(QLatin1Char('@')) ||
        a.endsWith(QLatin1Char('@')))
        return {};
    return a;
}
} // namespace

void MessageView::updateBadge() {
    const char* green = "color:#1a7f37;font-weight:bold"; // solo per Valid
    QString text, style;
    if (msg_.pgpNested) {
        // I1: parte firmata/cifrata annidata accanto a parti non protette. Qualunque
        // esito della verifica riguarda solo QUELLA parte: mai il badge verde.
        text = QStringLiteral("\u26A0 ") +
               tr("Firmato o cifrato solo in parte: il testo mostrato non e' verificato");
        if (sigStatus_ != SignatureStatus::None && !signer_.isEmpty())
            text += QStringLiteral(" (") + tr("firma di una parte: ") + signer_ + QLatin1Char(')');
        badge_->setText(text);
        badge_->setStyleSheet(QStringLiteral("color:#b26a00;font-weight:bold"));
        badge_->setVisible(true);
        return;
    }
    switch (sigStatus_) {
    case SignatureStatus::None:
        break;
    case SignatureStatus::Valid: {
        // Verde solo se l'indirizzo del firmatario coincide con quello del mittente
        // (Da): una firma valida di un altro non deve far sembrare autentico il mittente.
        const QString sAddr = addressOf(signer_), fAddr = addressOf(msg_.from);
        if (!sAddr.isEmpty() && sAddr == fAddr) {
            text = QStringLiteral("\u2713 ") + tr("Firma valida: ") + signer_;
            style = QString::fromLatin1(green);
        } else {
            text = QStringLiteral("\u26A0 ") + tr("Firma valida di %1, ma il mittente (Da) e' %2")
                                                     .arg(signer_, fAddr.isEmpty() ? msg_.from : fAddr);
            style = QStringLiteral("color:#b26a00;font-weight:bold");
        }
        break;
    }
    case SignatureStatus::Invalid:
        text = QStringLiteral("\u2717 ") + tr("Firma non valida");
        style = QStringLiteral("color:#c62828;font-weight:bold");
        break;
    case SignatureStatus::UnknownKey:
        text = QStringLiteral("? ") + tr("Firma di una chiave sconosciuta");
        style = QStringLiteral("color:#b26a00;font-weight:bold");
        break;
    case SignatureStatus::ExpiredKey:
        text = QStringLiteral("\u26A0 ") + tr("Chiave del firmatario scaduta");
        style = QStringLiteral("color:#b26a00;font-weight:bold");
        break;
    case SignatureStatus::RevokedKey:
        text = QStringLiteral("\u26A0 ") + tr("Chiave del firmatario revocata");
        style = QStringLiteral("color:#c62828;font-weight:bold");
        break;
    }
    badge_->setText(text);
    badge_->setStyleSheet(style);
    badge_->setVisible(!text.isEmpty());
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
    msg_ = m;
    sigStatus_ = SignatureStatus::None;
    signer_.clear();
    decryptError_.clear();
    encrypted_ = m.encryptedPgp ? m.rawEncrypted : QByteArray();
    encryptedShown_ = m.encryptedPgp;
    decryptButton_->setEnabled(true);
    updateAttachments();
    render();
    layoutBar();
}

void MessageView::updateAttachments() {
    QMenu* menu = attachButton_->menu();
    menu->clear();
    for (int i = 0; i < msg_.attachments.size(); ++i) {
        const Attachment& a = msg_.attachments.at(i);
        // testo semplice: niente mnemonici "&" dal nome scelto dal mittente
        QString label = a.filename;
        label.replace(QLatin1Char('&'), QStringLiteral("&&"));
        QAction* act = menu->addAction(QStringLiteral("%1 (%2)").arg(label, QLocale().formattedDataSize(a.size)));
        connect(act, &QAction::triggered, this, [this, i]() { emit saveAttachmentRequested(i); });
    }
}

void MessageView::render() {
    const ParsedMessage& m = msg_;
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
    if (encryptedShown_) {
        // il corpo cifrato non si mostra mai
    } else if (!m.plainText.isEmpty()) {
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
