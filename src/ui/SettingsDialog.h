#pragma once
#include "core/Config.h"
#include "core/ConfigCheck.h"
#include "pgp/GpgService.h"
#include "ui/Theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QTextBrowser>
#include <QThreadPool>

namespace boma {

// Percorsi e programmi esterni (iniettabili nei test). Vuoti = predefiniti.
struct SettingsEnvironment {
    QString mbsyncConfig;  // ~/.mbsyncrc
    QString msmtpConfig;   // ~/.msmtprc
    QString gpgProgram;    // "gpg"
    QString gnupgHome;
};

// Finestra "Impostazioni": modifica account e identita' del file di Boma e
// controlla (in sola lettura) la coerenza con mbsync, msmtp e gpg. Non scrive mai
// i file degli altri programmi. Lavora su una copia: result() e' la nuova
// configurazione, il salvataggio e' di chi chiama.
// L'identificativo di un account/identita' esistente non si cambia (e' il nome
// della sezione e il legame con le altre voci); solo quelli nuovi sono modificabili.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const Config& config, const SettingsEnvironment& env = {}, QWidget* parent = nullptr);
    ~SettingsDialog() override;

    Config result() const;
    // I controlli (file e gpg) sono asincroni: emesso a fine verifica.
    QTextBrowser* resultView() const { return results_; }
    QPushButton* checkButton() const { return checkBtn_; }
    QListWidget* accountList() const { return accountList_; }
    QListWidget* identityList() const { return identityList_; }
    QPushButton* addAccountButton() const { return addAccount_; }
    QPushButton* addIdentityButton() const { return addIdentity_; }
    QLineEdit* accountIdEdit() const { return accId_; }
    QLineEdit* accountNameEdit() const { return accName_; }
    QLineEdit* rootEdit() const { return accRoot_; }
    QComboBox* channelCombo() const { return accChannel_; }
    QLineEdit* identityIdEdit() const { return idId_; }
    QLineEdit* addressEdit() const { return idAddress_; }
    QComboBox* msmtpCombo() const { return idMsmtp_; }
    QComboBox* keyCombo() const { return idKey_; }
    QPushButton* msmtpButton() const { return msmtpBtn_; }
    QComboBox* styleCombo() const { return themeStyle_; }
    QComboBox* schemeCombo() const { return themeScheme_; }
    QCheckBox* preferHtmlBox() const { return preferHtml_; }
    QLabel* errorLabel() const { return error_; }
    QDialogButtonBox* buttons() const { return buttons_; }
    bool checksRunning() const { return checking_; }
    bool keysLoaded() const { return keysDone_; }

signals:
    void checksFinished();

public slots:
    void accept() override;

private:
    void showAccount(int row);
    void showIdentity(int row);
    void readAccountForm();
    void readIdentityForm();
    void refreshAccountList(int select);
    void refreshIdentityList(int select);
    void addAccount();
    void removeAccount();
    void addIdentity();
    void removeIdentity();
    void runChecks();
    void loadKeys();
    void fillKeyCombo();
    void setFormsEnabled();

    QList<Account> accounts_;
    QList<Identity> identities_;
    QSet<QString> newAccounts_, newIdentities_;
    Config base_;
    SettingsEnvironment env_;
    QStringList channels_, msmtpAccounts_;
    QString msmtpPath_;
    void editMsmtp();
    bool haveMbsyncRc_ = false, haveMsmtpRc_ = false;
    QList<SecretKeyInfo> keys_;
    bool keysDone_ = false, checking_ = false;

    int curAccount_ = -1, curIdentity_ = -1; // indici in accounts_/identities_
    QList<int> identityRows_;                 // riga della lista -> indice in identities_
    bool loading_ = false;

    QListWidget *accountList_, *identityList_;
    QPushButton *addAccount_, *removeAccount_, *addIdentity_, *removeIdentity_, *checkBtn_;
    QLineEdit *accId_, *accName_, *accRoot_, *accSent_, *accDrafts_, *accTrash_, *accArchive_;
    QComboBox* accChannel_;
    QLineEdit *idId_, *idAddress_, *idName_;
    QPlainTextEdit* idSignature_;
    QComboBox *idMsmtp_, *idKey_;
    QCheckBox* idSign_;
    QPushButton* msmtpBtn_;
    QComboBox *themeStyle_, *themeScheme_;
    QCheckBox* preferHtml_ = nullptr;
    QTextBrowser* results_;
    QLabel* error_;
    QDialogButtonBox* buttons_;
    // Dichiarato per ultimo: alla distruzione attende i lavori gpg in corso.
    QThreadPool pool_;
};

} // namespace boma
