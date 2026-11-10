#include "ui/ComposeWindow.h"
#include "core/MaildirWriter.h"
#include <QCloseEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

namespace boma {

namespace {
QString identityLabel(const Identity& i) {
    return i.displayName.isEmpty() ? i.address : QStringLiteral("%1 <%2>").arg(i.displayName, i.address);
}
} // namespace

ComposeWindow::ComposeWindow(const Config& config, QWidget* parent) : QMainWindow(parent), config_(config) {
    qRegisterMetaType<OutgoingMessage>();
    setWindowTitle(tr("Nuovo messaggio"));
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    identity_ = new QComboBox(central);
    for (const Account& a : config_.accounts())
        for (const Identity& i : config_.identities(a.id)) {
            identities_.append(i);
            identity_->addItem(identityLabel(i));
        }
    to_ = new QLineEdit(central);
    cc_ = new QLineEdit(central);
    bcc_ = new QLineEdit(central);
    subject_ = new QLineEdit(central);
    auto* form = new QFormLayout;
    form->addRow(tr("Da"), identity_);
    form->addRow(tr("A"), to_);
    form->addRow(tr("Cc"), cc_);
    form->addRow(tr("Ccn"), bcc_);
    form->addRow(tr("Oggetto"), subject_);
    root->addLayout(form);

    html_ = new QCheckBox(tr("HTML"), central);
    root->addWidget(html_);
    bodyStack_ = new QStackedWidget(central);
    plain_ = new QPlainTextEdit(bodyStack_);
    rich_ = new QTextEdit(bodyStack_);
    rich_->setAcceptRichText(true);
    bodyStack_->addWidget(plain_);
    bodyStack_->addWidget(rich_);
    root->addWidget(bodyStack_, 1);

    attachments_ = new QListWidget(central);
    attachments_->setMaximumHeight(90);
    auto* addB = new QPushButton(tr("Aggiungi allegato"), central);
    auto* remB = new QPushButton(tr("Rimuovi allegato"), central);
    auto* attRow = new QHBoxLayout;
    attRow->addWidget(attachments_, 1);
    auto* attBtns = new QVBoxLayout;
    attBtns->addWidget(addB);
    attBtns->addWidget(remB);
    attRow->addLayout(attBtns);
    root->addLayout(attRow);

    sign_ = new QPushButton(tr("Firma"), central);
    sign_->setCheckable(true);
    encrypt_ = new QPushButton(tr("Cifra"), central);
    encrypt_->setCheckable(true);
    saveDraft_ = new QPushButton(tr("Salva bozza"), central);
    send_ = new QPushButton(tr("Invia"), central);
    close_ = new QPushButton(tr("Chiudi"), central);
    status_ = new QLabel(central);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(sign_);
    btnRow->addWidget(encrypt_);
    btnRow->addStretch(1);
    btnRow->addWidget(saveDraft_);
    btnRow->addWidget(send_);
    btnRow->addWidget(close_);
    root->addLayout(btnRow);
    root->addWidget(status_);
    setCentralWidget(central);

    auto guard = [this] { if (!loading_) touch(); };
    connect(to_, &QLineEdit::textChanged, this, guard);
    connect(cc_, &QLineEdit::textChanged, this, guard);
    connect(bcc_, &QLineEdit::textChanged, this, guard);
    connect(subject_, &QLineEdit::textChanged, this, guard);
    connect(plain_, &QPlainTextEdit::textChanged, this, [this] { if (!loading_) touch(); });
    connect(rich_, &QTextEdit::textChanged, this, [this] { if (!loading_) touch(); });
    connect(identity_, &QComboBox::currentIndexChanged, this, &ComposeWindow::onIdentityChanged);
    connect(html_, &QCheckBox::toggled, this, &ComposeWindow::onHtmlToggled);
    connect(addB, &QPushButton::clicked, this, &ComposeWindow::onAddAttachment);
    connect(remB, &QPushButton::clicked, this, &ComposeWindow::onRemoveAttachment);
    connect(saveDraft_, &QPushButton::clicked, this, [this] { saveDraft(); });
    connect(send_, &QPushButton::clicked, this, &ComposeWindow::onSend);
    connect(close_, &QPushButton::clicked, this, &QWidget::close);

    sign_->setChecked(identities_.isEmpty() ? false : identities_.first().signByDefault);
    resize(720, 560);
}

Identity ComposeWindow::currentIdentity() const {
    const int i = identity_->currentIndex();
    return (i >= 0 && i < identities_.size()) ? identities_.at(i) : Identity();
}

void ComposeWindow::onIdentityChanged() {
    sign_->setChecked(currentIdentity().signByDefault);
    touch();
}

void ComposeWindow::onHtmlToggled(bool on) {
    // Il passaggio da HTML a testo perde la formattazione (resta il testo).
    loading_ = true;
    if (on)
        rich_->setPlainText(plain_->toPlainText());
    else
        plain_->setPlainText(rich_->toPlainText());
    bodyStack_->setCurrentWidget(on ? static_cast<QWidget*>(rich_) : static_cast<QWidget*>(plain_));
    loading_ = false;
    touch();
}

void ComposeWindow::onAddAttachment() {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Aggiungi allegati"));
    for (const QString& f : files) addAttachment(f);
}

void ComposeWindow::addAttachment(const QString& path) {
    if (path.isEmpty()) return;
    attachments_->addItem(path);
    touch();
}

void ComposeWindow::onRemoveAttachment() {
    const auto sel = attachments_->selectedItems();
    for (QListWidgetItem* it : sel) delete attachments_->takeItem(attachments_->row(it));
    if (!sel.isEmpty()) touch();
}

QStringList ComposeWindow::splitAddresses(const QString& text) {
    QStringList out;
    QString cur;
    bool quote = false;
    int angle = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('\\') && quote && i + 1 < text.size()) {
            cur += c;
            cur += text[++i];
            continue;
        }
        if (c == QLatin1Char('"')) quote = !quote;
        else if (!quote && c == QLatin1Char('<')) ++angle;
        else if (!quote && c == QLatin1Char('>') && angle > 0) --angle;
        if (c == QLatin1Char(',') && !quote && angle == 0) {
            if (!cur.trimmed().isEmpty()) out << cur.trimmed();
            cur.clear();
        } else
            cur += c;
    }
    if (!cur.trimmed().isEmpty()) out << cur.trimmed();
    return out;
}

OutgoingMessage ComposeWindow::currentMessage() const {
    OutgoingMessage m;
    m.identity = currentIdentity();
    m.to = splitAddresses(to_->text());
    m.cc = splitAddresses(cc_->text());
    m.bcc = splitAddresses(bcc_->text());
    m.subject = subject_->text();
    if (html_->isChecked()) {
        m.plainText = rich_->toPlainText();
        m.html = rich_->toHtml();
    } else {
        m.plainText = plain_->toPlainText();
    }
    for (int i = 0; i < attachments_->count(); ++i) m.attachmentPaths << attachments_->item(i)->text();
    return m;
}

void ComposeWindow::setPrefill(const OutgoingMessage& m) {
    loading_ = true;
    int idx = -1;
    for (int i = 0; i < identities_.size() && idx < 0; ++i)
        if (!m.identity.id.isEmpty() && identities_[i].id == m.identity.id) idx = i;
    for (int i = 0; i < identities_.size() && idx < 0; ++i)
        if (!m.identity.address.isEmpty() && identities_[i].address.compare(m.identity.address, Qt::CaseInsensitive) == 0) idx = i;
    if (idx >= 0) identity_->setCurrentIndex(idx);
    to_->setText(m.to.join(QStringLiteral(", ")));
    cc_->setText(m.cc.join(QStringLiteral(", ")));
    bcc_->setText(m.bcc.join(QStringLiteral(", ")));
    subject_->setText(m.subject);
    const bool wantHtml = !m.html.trimmed().isEmpty();
    html_->setChecked(wantHtml);
    if (wantHtml)
        rich_->setHtml(m.html);
    else
        plain_->setPlainText(m.plainText);
    attachments_->clear();
    for (const QString& p : m.attachmentPaths) attachments_->addItem(p);
    loading_ = false;
    dirty_ = false;
    saveFailed_ = false;
}

bool ComposeWindow::saveDraft() {
    auto fail = [this](const QString& why) {
        saveFailed_ = true;
        status_->setText(tr("Bozza non salvata: %1").arg(why));
        emit draftError(why);
        return false;
    };
    const Identity id = currentIdentity();
    const Account* acc = nullptr;
    const QList<Account> accounts = config_.accounts();
    for (const Account& a : accounts)
        if (a.id == id.accountId) acc = &a;
    if (!acc) return fail(tr("nessuna identita' o account associato"));
    QString err;
    const QByteArray raw = MessageBuilder::build(currentMessage(), &err);
    if (raw.isEmpty()) return fail(err);
    const QString folder = acc->maildirRoot + QLatin1Char('/') + acc->draftsFolder;
    const QString path = MaildirWriter::deliver(folder, raw, QStringLiteral("D"), &err);
    if (path.isEmpty()) return fail(err);
    saveFailed_ = false;
    dirty_ = false;
    status_->setText(tr("Bozza salvata"));
    emit draftSaved(path);
    return true;
}

void ComposeWindow::onSend() {
    const OutgoingMessage m = currentMessage();
    if (MessageBuilder::envelopeRecipients(m).isEmpty()) {
        status_->setText(tr("Nessun destinatario valido"));
        return;
    }
    QString err;
    if (MessageBuilder::build(m, &err).isEmpty()) { // validazione prima di emettere
        status_->setText(tr("Impossibile inviare: %1").arg(err));
        return;
    }
    emit sendRequested(m, sign_->isChecked(), encrypt_->isChecked());
}

void ComposeWindow::closeEvent(QCloseEvent* e) {
    if (!dirty_) {
        e->accept();
        return;
    }
    bool discard;
    if (confirm_) {
        discard = confirm_();
    } else {
        const QString text = saveFailed_
            ? tr("Il salvataggio della bozza e' fallito. Chiudere e perdere il messaggio?")
            : tr("Ci sono modifiche non salvate. Chiudere e perderle?");
        discard = QMessageBox::question(this, tr("Chiudi"), text) == QMessageBox::Yes;
    }
    if (discard) e->accept();
    else e->ignore();
}

} // namespace boma
