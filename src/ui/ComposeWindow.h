#pragma once
#include "core/Config.h"
#include "mime/MessageBuilder.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextEdit>
#include <functional>

namespace boma {

// Finestra di scrittura. Non invia (Task 9: ascolta sendRequested) e non
// gestisce PGP: espone solo i pulsanti Firma/Cifra.
// Le bozze sono scritte con MaildirWriter in Bozze dell'account proprietario
// dell'identita' scelta; ogni salvataggio crea un nuovo file (mai
// sovrascrive). Il Bcc non e' mai nel messaggio costruito, quindi non e'
// conservato nella bozza.
class ComposeWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ComposeWindow(const Config& config, QWidget* parent = nullptr);

    // Contenuto iniziale (risposta/inoltro); l'identita' e' cercata per id, poi per indirizzo.
    // Dopo il prefill la finestra e' "pulita".
    void setPrefill(const OutgoingMessage& m);
    OutgoingMessage currentMessage() const;
    void addAttachment(const QString& path);

    // Costruisce e salva la bozza. false con draftError() in caso di errore.
    bool saveDraft();
    bool isDirty() const { return dirty_; }
    void markClean() { dirty_ = false; }
    // Chiamata prima di chiudere con modifiche non salvate: true = scarta.
    // Sostituibile nei test (default: QMessageBox::question).
    void setConfirmCallback(std::function<bool()> cb) { confirm_ = std::move(cb); }

    // Elenco separato da virgole, rispettando virgolette e <>.
    static QStringList splitAddresses(const QString& text);

    QComboBox* identityCombo() const { return identity_; }
    QLineEdit* toEdit() const { return to_; }
    QLineEdit* ccEdit() const { return cc_; }
    QLineEdit* bccEdit() const { return bcc_; }
    QLineEdit* subjectEdit() const { return subject_; }
    QPlainTextEdit* bodyEdit() const { return plain_; }
    QTextEdit* richEdit() const { return rich_; }
    class RichTextToolBar* formatBar() const { return fmtBar_; }
    QCheckBox* htmlCheck() const { return html_; }
    QListWidget* attachmentList() const { return attachments_; }
    QPushButton* signButton() const { return sign_; }
    QPushButton* encryptButton() const { return encrypt_; }
    QPushButton* saveDraftButton() const { return saveDraft_; }
    QPushButton* sendButton() const { return send_; }
    QPushButton* closeButton() const { return close_; }
    QLabel* statusLabel() const { return status_; }

signals:
    void sendRequested(const boma::OutgoingMessage& msg, bool sign, bool encrypt);
    void draftError(const QString& error);
    void draftSaved(const QString& path);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void onIdentityChanged();
    void onHtmlToggled(bool on);
    void onAddAttachment();
    void onRemoveAttachment();
    void onSend();
    void touch() { dirty_ = true; }
    Identity currentIdentity() const;

    Config config_;
    QList<Identity> identities_;
    QComboBox* identity_ = nullptr;
    QLineEdit *to_ = nullptr, *cc_ = nullptr, *bcc_ = nullptr, *subject_ = nullptr;
    QStackedWidget* bodyStack_ = nullptr;
    QPlainTextEdit* plain_ = nullptr;
    QTextEdit* rich_ = nullptr;
    class RichTextToolBar* fmtBar_ = nullptr;
    QCheckBox* html_ = nullptr;
    QListWidget* attachments_ = nullptr;
    QPushButton *sign_ = nullptr, *encrypt_ = nullptr, *saveDraft_ = nullptr, *send_ = nullptr, *close_ = nullptr;
    QLabel* status_ = nullptr;
    bool dirty_ = false;
    bool saveFailed_ = false;
    bool loading_ = false;
    QString inReplyTo_;      // dal prefill (risposte): conservati nel messaggio
    QStringList references_;
    std::function<bool()> confirm_;
};

} // namespace boma
