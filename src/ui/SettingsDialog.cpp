#include "ui/SettingsDialog.h"

#include "core/MsmtpConfig.h"
#include "ui/MsmtpDialog.h"
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPointer>
#include <QSplitter>
#include <QVBoxLayout>

namespace boma {

SettingsDialog::SettingsDialog(const Config& config, const SettingsEnvironment& env, QWidget* parent)
    : QDialog(parent), accounts_(config.accounts()), identities_(config.identities()), base_(config), env_(env) {
    setWindowTitle(tr("Impostazioni"));
    resize(900, 720);
    pool_.setMaxThreadCount(1);

    const QString mbRc = env_.mbsyncConfig.isEmpty() ? ConfigCheck::mbsyncConfigPath() : env_.mbsyncConfig;
    const QString msRc = env_.msmtpConfig.isEmpty() ? ConfigCheck::msmtpConfigPath() : env_.msmtpConfig;
    channels_ = ConfigCheck::mbsyncChannels(ConfigCheck::readFile(mbRc, &haveMbsyncRc_));
    msmtpPath_ = msRc.isEmpty() ? QDir::homePath() + QStringLiteral("/.msmtprc") : msRc;
    msmtpAccounts_ = ConfigCheck::msmtpAccounts(ConfigCheck::readFile(msRc, &haveMsmtpRc_));

    auto* root = new QVBoxLayout(this);
    auto* split = new QSplitter(Qt::Horizontal, this);
    root->addWidget(split, 1);

    // ---- sinistra: account ----
    auto* left = new QWidget(split);
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->addWidget(new QLabel(tr("Account"), left));
    accountList_ = new QListWidget(left);
    ll->addWidget(accountList_, 1);
    auto* lb = new QHBoxLayout;
    addAccount_ = new QPushButton(tr("Aggiungi"), left);
    removeAccount_ = new QPushButton(tr("Rimuovi"), left);
    lb->addWidget(addAccount_);
    lb->addWidget(removeAccount_);
    ll->addLayout(lb);

    // ---- destra ----
    auto* right = new QWidget(split);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);

    auto* accBox = new QGroupBox(tr("Account (mbsync)"), right);
    auto* af = new QFormLayout(accBox);
    accId_ = new QLineEdit(accBox);
    accId_->setToolTip(tr("Nome interno: non si cambia dopo il primo salvataggio."));
    accName_ = new QLineEdit(accBox);
    accRoot_ = new QLineEdit(accBox);
    auto* browse = new QPushButton(tr("Scegli..."), accBox);
    auto* rootRow = new QHBoxLayout;
    rootRow->addWidget(accRoot_, 1);
    rootRow->addWidget(browse);
    accChannel_ = new QComboBox(accBox);
    accChannel_->setEditable(true);
    accChannel_->addItems(channels_);
    accChannel_->setToolTip(haveMbsyncRc_ ? tr("Canali letti da .mbsyncrc") : tr("File di mbsync non trovato: scrivi il nome del canale."));
    accSent_ = new QLineEdit(accBox);
    accDrafts_ = new QLineEdit(accBox);
    accTrash_ = new QLineEdit(accBox);
    af->addRow(tr("Identificativo"), accId_);
    af->addRow(tr("Nome visualizzato"), accName_);
    af->addRow(tr("Radice Maildir"), rootRow);
    af->addRow(tr("Canale mbsync"), accChannel_);
    af->addRow(tr("Cartella Inviata"), accSent_);
    af->addRow(tr("Cartella Bozze"), accDrafts_);
    af->addRow(tr("Cartella Cestino"), accTrash_);
    rl->addWidget(accBox);

    auto* idBox = new QGroupBox(tr("Identita' dell'account (msmtp e PGP)"), right);
    auto* il = new QVBoxLayout(idBox);
    identityList_ = new QListWidget(idBox);
    identityList_->setMaximumHeight(90);
    il->addWidget(identityList_);
    auto* ib = new QHBoxLayout;
    addIdentity_ = new QPushButton(tr("Aggiungi identita'"), idBox);
    removeIdentity_ = new QPushButton(tr("Rimuovi identita'"), idBox);
    ib->addWidget(addIdentity_);
    ib->addWidget(removeIdentity_);
    ib->addStretch(1);
    il->addLayout(ib);
    auto* idf = new QFormLayout;
    idId_ = new QLineEdit(idBox);
    idAddress_ = new QLineEdit(idBox);
    idName_ = new QLineEdit(idBox);
    idSignature_ = new QPlainTextEdit(idBox);
    idSignature_->setMaximumHeight(60);
    idMsmtp_ = new QComboBox(idBox);
    idMsmtp_->setEditable(true);
    idMsmtp_->addItems(msmtpAccounts_);
    idMsmtp_->setToolTip(haveMsmtpRc_ ? tr("Account letti da .msmtprc") : tr("File di msmtp non trovato: scrivi il nome dell'account."));
    idKey_ = new QComboBox(idBox);
    idKey_->setEditable(true);
    idKey_->lineEdit()->setPlaceholderText(tr("impronta o indirizzo della chiave (vuoto = senza PGP)"));
    idSign_ = new QCheckBox(tr("Firma di default"), idBox);
    idf->addRow(tr("Identificativo"), idId_);
    idf->addRow(tr("Indirizzo"), idAddress_);
    idf->addRow(tr("Nome"), idName_);
    idf->addRow(tr("Firma testuale"), idSignature_);
    msmtpBtn_ = new QPushButton(tr("Server di invio..."), idBox);
    msmtpBtn_->setToolTip(tr("Modifica host, porta, TLS e utente di questo account in ~/.msmtprc"));
    auto* msRow = new QHBoxLayout;
    msRow->addWidget(idMsmtp_, 1);
    msRow->addWidget(msmtpBtn_);
    idf->addRow(tr("Account msmtp"), msRow);
    idf->addRow(tr("Chiave PGP"), idKey_);
    idf->addRow(QString(), idSign_);
    il->addLayout(idf);
    rl->addWidget(idBox);

    auto* chkRow = new QHBoxLayout;
    checkBtn_ = new QPushButton(tr("Verifica"), right);
    checkBtn_->setToolTip(tr("Controlla mbsync, msmtp, le cartelle e le chiavi PGP (sola lettura)"));
    chkRow->addWidget(checkBtn_);
    chkRow->addStretch(1);
    rl->addLayout(chkRow);
    results_ = new QTextBrowser(right);
    results_->setMinimumHeight(110);
    rl->addWidget(results_, 1);

    auto* themeRow = new QHBoxLayout;
    themeStyle_ = new QComboBox(right);
    themeStyle_->addItem(tr("Predefinito"), QString());
    for (const QString& k : Theme::availableStyles()) themeStyle_->addItem(k, k);
    themeScheme_ = new QComboBox(right);
    themeScheme_->addItem(tr("Segui il sistema"), QStringLiteral("system"));
    themeScheme_->addItem(tr("Chiaro"), QStringLiteral("light"));
    themeScheme_->addItem(tr("Scuro"), QStringLiteral("dark"));
    themeScheme_->setEnabled(Theme::schemeSupported());
    if (!Theme::schemeSupported()) themeScheme_->setToolTip(tr("Richiede Qt 6.8 o successivo"));
    {
        const Theme cur = Theme::load();
        themeStyle_->setCurrentIndex(qMax(0, themeStyle_->findData(cur.style)));
        themeScheme_->setCurrentIndex(qMax(0, themeScheme_->findData(cur.scheme)));
    }
    themeRow->addWidget(new QLabel(tr("Aspetto: stile"), right));
    themeRow->addWidget(themeStyle_);
    themeRow->addWidget(new QLabel(tr("colori"), right));
    themeRow->addWidget(themeScheme_);
    themeRow->addStretch(1);
    rl->addLayout(themeRow);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(1, 1);

    error_ = new QLabel(this);
    error_->setStyleSheet(QStringLiteral("color:#a4262c;"));
    error_->setWordWrap(true);
    root->addWidget(error_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    connect(accountList_, &QListWidget::currentRowChanged, this, &SettingsDialog::showAccount);
    connect(identityList_, &QListWidget::currentRowChanged, this, &SettingsDialog::showIdentity);
    connect(addAccount_, &QPushButton::clicked, this, &SettingsDialog::addAccount);
    connect(removeAccount_, &QPushButton::clicked, this, &SettingsDialog::removeAccount);
    connect(addIdentity_, &QPushButton::clicked, this, &SettingsDialog::addIdentity);
    connect(removeIdentity_, &QPushButton::clicked, this, &SettingsDialog::removeIdentity);
    connect(checkBtn_, &QPushButton::clicked, this, &SettingsDialog::runChecks);
    connect(msmtpBtn_, &QPushButton::clicked, this, &SettingsDialog::editMsmtp);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Radice Maildir"), accRoot_->text());
        if (!d.isEmpty()) accRoot_->setText(d);
    });
    for (QLineEdit* e : {accId_, accName_, accRoot_, accSent_, accDrafts_, accTrash_})
        connect(e, &QLineEdit::textChanged, this, &SettingsDialog::readAccountForm);
    connect(accChannel_, &QComboBox::editTextChanged, this, &SettingsDialog::readAccountForm);
    for (QLineEdit* e : {idId_, idAddress_, idName_})
        connect(e, &QLineEdit::textChanged, this, &SettingsDialog::readIdentityForm);
    connect(idSignature_, &QPlainTextEdit::textChanged, this, &SettingsDialog::readIdentityForm);
    connect(idMsmtp_, &QComboBox::editTextChanged, this, &SettingsDialog::readIdentityForm);
    connect(idKey_, &QComboBox::editTextChanged, this, &SettingsDialog::readIdentityForm);
    connect(idSign_, &QCheckBox::toggled, this, &SettingsDialog::readIdentityForm);
    // scegliendo una chiave dall'elenco si scrive l'impronta, non la descrizione
    connect(idKey_, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        const QString fpr = idKey_->itemData(i).toString();
        if (!fpr.isEmpty()) idKey_->setEditText(fpr);
    });

    refreshAccountList(accounts_.isEmpty() ? -1 : 0);
    setFormsEnabled();
    loadKeys();
}

SettingsDialog::~SettingsDialog() = default;

Config SettingsDialog::result() const {
    Config c = base_;
    c.setAccounts(accounts_);
    c.setIdentities(identities_);
    return c;
}

void SettingsDialog::setFormsEnabled() {
    const bool haveAcc = curAccount_ >= 0;
    const bool haveId = curIdentity_ >= 0;
    for (QWidget* w : QList<QWidget*>{accName_, accRoot_, accChannel_, accSent_, accDrafts_, accTrash_, addIdentity_, removeAccount_})
        w->setEnabled(haveAcc);
    accId_->setEnabled(haveAcc && newAccounts_.contains(accounts_.value(curAccount_).id));
    for (QWidget* w : QList<QWidget*>{idAddress_, idName_, idSignature_, idMsmtp_, msmtpBtn_, idKey_, idSign_, removeIdentity_})
        w->setEnabled(haveId);
    idId_->setEnabled(haveId && newIdentities_.contains(identities_.value(curIdentity_).id));
}

void SettingsDialog::refreshAccountList(int select) {
    QSignalBlocker b(accountList_);
    accountList_->clear();
    for (const Account& a : accounts_)
        accountList_->addItem(a.name.isEmpty() ? a.id : QStringLiteral("%1 (%2)").arg(a.name, a.id));
    b.unblock();
    if (select >= 0 && select < accounts_.size()) accountList_->setCurrentRow(select);
    else showAccount(-1);
}

void SettingsDialog::refreshIdentityList(int select) {
    QSignalBlocker b(identityList_);
    identityList_->clear();
    identityRows_.clear();
    if (curAccount_ >= 0) {
        const QString acc = accounts_.at(curAccount_).id;
        for (int i = 0; i < identities_.size(); ++i)
            if (identities_.at(i).accountId == acc) {
                identityRows_ << i;
                const Identity& id = identities_.at(i);
                identityList_->addItem(QStringLiteral("%1 <%2>").arg(id.displayName.isEmpty() ? id.id : id.displayName, id.address));
            }
    }
    b.unblock();
    if (select >= 0 && select < identityRows_.size()) identityList_->setCurrentRow(select);
    else showIdentity(-1);
}

void SettingsDialog::showAccount(int row) {
    loading_ = true;
    curAccount_ = row;
    if (row >= 0 && row < accounts_.size()) {
        const Account& a = accounts_.at(row);
        accId_->setText(a.id);
        accName_->setText(a.name);
        accRoot_->setText(a.maildirRoot);
        accChannel_->setEditText(a.mbsyncChannel);
        accSent_->setText(a.sentFolder);
        accDrafts_->setText(a.draftsFolder);
        accTrash_->setText(a.trashFolder);
    } else {
        curAccount_ = -1;
        for (QLineEdit* e : {accId_, accName_, accRoot_, accSent_, accDrafts_, accTrash_}) e->clear();
        accChannel_->setEditText(QString());
    }
    loading_ = false;
    refreshIdentityList(identityRows_.isEmpty() && curAccount_ < 0 ? -1 : 0);
    setFormsEnabled();
}

void SettingsDialog::showIdentity(int row) {
    loading_ = true;
    curIdentity_ = (row >= 0 && row < identityRows_.size()) ? identityRows_.at(row) : -1;
    if (curIdentity_ >= 0) {
        const Identity& i = identities_.at(curIdentity_);
        idId_->setText(i.id);
        idAddress_->setText(i.address);
        idName_->setText(i.displayName);
        idSignature_->setPlainText(i.signature);
        idMsmtp_->setEditText(i.msmtpAccount);
        idKey_->setEditText(i.pgpKeyId);
        idSign_->setChecked(i.signByDefault);
    } else {
        for (QLineEdit* e : {idId_, idAddress_, idName_}) e->clear();
        idSignature_->clear();
        idMsmtp_->setEditText(QString());
        idKey_->setEditText(QString());
        idSign_->setChecked(true);
    }
    loading_ = false;
    setFormsEnabled();
}

void SettingsDialog::readAccountForm() {
    if (loading_ || curAccount_ < 0) return;
    Account& a = accounts_[curAccount_];
    const QString oldId = a.id;
    a.id = accId_->text().trimmed();
    a.name = accName_->text();
    a.maildirRoot = accRoot_->text();
    a.mbsyncChannel = accChannel_->currentText();
    a.sentFolder = accSent_->text();
    a.draftsFolder = accDrafts_->text();
    a.trashFolder = accTrash_->text();
    if (a.id != oldId) { // solo un account nuovo ha l'id modificabile: le sue identita' lo seguono
        newAccounts_.remove(oldId);
        newAccounts_.insert(a.id);
        for (Identity& i : identities_)
            if (i.accountId == oldId) i.accountId = a.id;
    }
    QSignalBlocker b(accountList_);
    if (QListWidgetItem* it = accountList_->item(curAccount_))
        it->setText(a.name.isEmpty() ? a.id : QStringLiteral("%1 (%2)").arg(a.name, a.id));
}

void SettingsDialog::readIdentityForm() {
    if (loading_ || curIdentity_ < 0) return;
    Identity& i = identities_[curIdentity_];
    const QString oldId = i.id;
    i.id = idId_->text().trimmed();
    i.address = idAddress_->text();
    i.displayName = idName_->text();
    i.signature = idSignature_->toPlainText();
    i.msmtpAccount = idMsmtp_->currentText();
    i.pgpKeyId = idKey_->currentText();
    i.signByDefault = idSign_->isChecked();
    if (i.id != oldId) {
        newIdentities_.remove(oldId);
        newIdentities_.insert(i.id);
    }
    const int row = identityRows_.indexOf(curIdentity_);
    QSignalBlocker b(identityList_);
    if (QListWidgetItem* it = identityList_->item(row))
        it->setText(QStringLiteral("%1 <%2>").arg(i.displayName.isEmpty() ? i.id : i.displayName, i.address));
}

void SettingsDialog::addAccount() {
    QSet<QString> used;
    for (const Account& a : accounts_) used.insert(a.id);
    QString id;
    for (int n = 1; used.contains(id = QStringLiteral("account%1").arg(n)); ++n) {}
    Account a;
    a.id = id;
    a.name = tr("Nuovo account");
    a.mbsyncChannel = id;
    a.sentFolder = QStringLiteral("Sent");
    a.draftsFolder = QStringLiteral("Drafts");
    a.trashFolder = QStringLiteral("Trash");
    accounts_.append(a);
    newAccounts_.insert(id);
    refreshAccountList(int(accounts_.size()) - 1);
}

void SettingsDialog::removeAccount() {
    if (curAccount_ < 0) return;
    const QString id = accounts_.at(curAccount_).id;
    accounts_.removeAt(curAccount_);
    newAccounts_.remove(id);
    for (int i = int(identities_.size()) - 1; i >= 0; --i)
        if (identities_.at(i).accountId == id) identities_.removeAt(i);
    curIdentity_ = -1;
    refreshAccountList(accounts_.isEmpty() ? -1 : qMin(curAccount_, int(accounts_.size()) - 1));
}

void SettingsDialog::addIdentity() {
    if (curAccount_ < 0) return;
    QSet<QString> used;
    for (const Identity& i : identities_) used.insert(i.id);
    QString id;
    for (int n = 1; used.contains(id = QStringLiteral("identita%1").arg(n)); ++n) {}
    Identity i;
    i.id = id;
    i.accountId = accounts_.at(curAccount_).id;
    identities_.append(i);
    newIdentities_.insert(id);
    refreshIdentityList(int(identityRows_.size())); // la nuova e' l'ultima della lista dell'account
}

void SettingsDialog::removeIdentity() {
    if (curIdentity_ < 0) return;
    const int row = identityRows_.indexOf(curIdentity_);
    newIdentities_.remove(identities_.at(curIdentity_).id);
    identities_.removeAt(curIdentity_);
    curIdentity_ = -1;
    refreshIdentityList(qMax(0, qMin(row, int(identityRows_.size()) - 2)));
}

// Il file di msmtp si scrive subito (non al "Salva" di Boma): e' un file a parte.
void SettingsDialog::editMsmtp() {
    const QString name = idMsmtp_->currentText().trimmed();
    if (!MsmtpConfig::validAccountName(name)) {
        error_->setText(tr("Scrivi prima un nome valido per l'account msmtp (lettere, cifre, . _ -)."));
        return;
    }
    error_->clear();
    MsmtpDialog dlg(msmtpPath_, name, this);
    if (dlg.exec() != QDialog::Accepted) return;
    bool ok = false;
    msmtpAccounts_ = ConfigCheck::msmtpAccounts(ConfigCheck::readFile(msmtpPath_, &ok));
    haveMsmtpRc_ = ok;
    const QString cur = idMsmtp_->currentText();
    loading_ = true;
    idMsmtp_->clear();
    idMsmtp_->addItems(msmtpAccounts_);
    idMsmtp_->setEditText(cur);
    loading_ = false;
}

void SettingsDialog::accept() {
    const QStringList problems = result().validate();
    if (!problems.isEmpty()) {
        error_->setText(problems.join(QStringLiteral("\n")));
        return;
    }
    error_->clear();
    Theme t;
    t.style = themeStyle_->currentData().toString();
    t.scheme = themeScheme_->currentData().toString();
    t.save();
    t.apply(); // subito, anche se poi il salvataggio degli account fallisse
    QDialog::accept();
}

void SettingsDialog::loadKeys() {
    QPointer<SettingsDialog> self(this);
    const QString prog = env_.gpgProgram, home = env_.gnupgHome;
    pool_.start([self, prog, home]() {
        GpgService g;
        if (!prog.isEmpty()) g.setGpgProgram(prog);
        if (!home.isEmpty()) g.setGnupgHome(home);
        QList<SecretKeyInfo> keys;
        g.listSecretKeys(&keys);
        if (!self) return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, keys]() {
                if (!self) return;
                self->keys_ = keys;
                self->keysDone_ = true;
                self->fillKeyCombo();
            },
            Qt::QueuedConnection);
    });
}

void SettingsDialog::fillKeyCombo() {
    const bool was = loading_;
    loading_ = true; // l'elenco cambia, non il valore scritto dall'utente
    const QString cur = idKey_->currentText();
    idKey_->clear();
    for (const SecretKeyInfo& k : keys_)
        idKey_->addItem(QStringLiteral("%1  [%2]").arg(k.uids.value(0), k.fingerprint.right(16)), k.fingerprint);
    idKey_->setEditText(cur);
    loading_ = was;
}

void SettingsDialog::runChecks() {
    if (checking_) return;
    checking_ = true;
    checkBtn_->setEnabled(false);
    results_->setHtml(tr("Verifica in corso..."));

    // controlli sui file: subito (sola lettura, veloci)
    auto fmt = [](const CheckIssue& c) {
        const QString mark = c.level == CheckIssue::Ok ? QStringLiteral("<span style='color:#107c10'>&#10003;</span>")
                             : c.level == CheckIssue::Warning ? QStringLiteral("<span style='color:#9d5d00'>&#9888;</span>")
                                                              : QStringLiteral("<span style='color:#a4262c'>&#10007;</span>");
        return QStringLiteral("%1 %2<br>").arg(mark, c.text.toHtmlEscaped());
    };
    QString html;
    for (const Account& a : accounts_) {
        html += QStringLiteral("<b>%1</b><br>").arg((a.name.isEmpty() ? a.id : a.name).toHtmlEscaped());
        for (const CheckIssue& c : ConfigCheck::checkAccount(a, channels_, haveMbsyncRc_)) html += fmt(c);
        for (const Identity& i : identities_) {
            if (i.accountId != a.id) continue;
            html += QStringLiteral("<i>%1</i><br>").arg(i.address.toHtmlEscaped());
            for (const CheckIssue& c : ConfigCheck::checkIdentity(i, msmtpAccounts_, haveMsmtpRc_)) html += fmt(c);
        }
    }

    // chiavi e cache di gpg-agent: fuori dal thread GUI
    QList<QPair<QString, QString>> keyIds; // (indirizzo, chiave)
    for (const Identity& i : identities_)
        if (!i.pgpKeyId.trimmed().isEmpty()) keyIds.append({i.address, i.pgpKeyId.trimmed()});
    QPointer<SettingsDialog> self(this);
    const QString prog = env_.gpgProgram, home = env_.gnupgHome;
    pool_.start([self, prog, home, keyIds, html, fmt]() {
        GpgService g;
        if (!prog.isEmpty()) g.setGpgProgram(prog);
        if (!home.isEmpty()) g.setGnupgHome(home);
        QString out = html;
        for (const auto& [addr, key] : keyIds) {
            QString err;
            const QString fpr = g.resolveSecretKey(key, &err);
            CheckIssue c;
            if (fpr.isEmpty()) {
                c.level = CheckIssue::Error;
                c.text = QCoreApplication::translate("SettingsDialog", "%1: chiave PGP \"%2\" non utilizzabile: %3").arg(addr, key, err);
            } else {
                c.level = CheckIssue::Ok;
                c.text = QCoreApplication::translate("SettingsDialog", "%1: chiave PGP trovata (%2).").arg(addr, fpr.right(16));
            }
            out += fmt(c);
        }
        const GpgAgentInfo agent = GpgService::checkAgentCache();
        CheckIssue c;
        if (!agent.available) {
            c.level = CheckIssue::Warning;
            c.text = QCoreApplication::translate("SettingsDialog", "gpg-agent: impostazioni non leggibili (gpgconf non risponde).");
        } else if (agent.ttlIs300) {
            c.level = CheckIssue::Ok;
            c.text = QCoreApplication::translate("SettingsDialog", "gpg-agent: la password resta in cache 5 minuti.");
        } else {
            c.level = CheckIssue::Warning;
            c.text = QCoreApplication::translate("SettingsDialog", "gpg-agent: cache di %1 s (massimo %2 s); consigliato 300 s in gpg-agent.conf.")
                         .arg(agent.defaultCacheTtl).arg(agent.maxCacheTtl);
        }
        out += QStringLiteral("<b>GnuPG</b><br>") + fmt(c);
        if (!self) return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, out]() {
                if (!self) return;
                self->results_->setHtml(out);
                self->checking_ = false;
                self->checkBtn_->setEnabled(true);
                emit self->checksFinished();
            },
            Qt::QueuedConnection);
    });
}

} // namespace boma
