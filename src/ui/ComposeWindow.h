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
#include <QTemporaryDir>
#include <QTextEdit>
#include <functional>
#include <memory>

namespace boma {

// Finestra di scrittura. Non invia (Task 9: ascolta sendRequested) e non
// gestisce PGP: espone solo i pulsanti Firma/Cifra.
// Le bozze sono scritte con MaildirWriter in Bozze dell'account proprietario
// dell'identita' scelta, in modalita' BuildMode::Draft: il Bcc e i destinatari non
// analizzabili sono conservati (intestazioni Bcc e X-Boma-Raw-*), la firma non e'
// aggiunta. Ogni salvataggio SOSTITUISCE la bozza della finestra: il nuovo file si scrive
// per primo e solo dopo si rimuove il vecchio. Una bozza riaperta (setDraftPath) viene
// sostituita allo stesso modo, quindi non si accumulano copie.
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
    // Ogni salvataggio SOSTITUISCE la bozza precedente di questa finestra (la vecchia si
    // rimuove solo dopo aver scritto la nuova: mai nessuna copia in meno del necessario).
    // Percorso dell'ultima bozza scritta (vuoto se mai salvata o gia' rimossa).
    QString draftPath() const { return draftPath_; }
    // Lega la finestra a una bozza esistente (riaperta per modificarla): il prossimo
    // salvataggio la sostituisce e l'invio riuscito la rimuove. Da chiamare dopo setPrefill.
    void setDraftPath(const QString& path);
    // Registra `path` come origine della finestra SENZA legarla: la bozza originale non sara'
    // mai sostituita ne' rimossa (serve per le bozze non riproducibili fedelmente), ma la
    // stessa bozza non si riapre in una seconda finestra.
    void markOpenedFrom(const QString& path);
    // Vero se `path` e' (o e' stata, in questa finestra) la bozza legata alla finestra.
    // Confronta la parte unica del nome (mbsync rinomina i file cambiando i suffissi).
    bool ownsDraft(const QString& path) const;
    // Identita' stabile di una bozza: cartella Maildir + parte unica del nome file.
    static QString draftKey(const QString& path);
    // La finestra possiede la cartella temporanea con gli allegati estratti: viene
    // eliminata alla chiusura (qualunque via), dopo l'invio e alla distruzione.
    void adoptTempDir(std::shared_ptr<QTemporaryDir> dir) { tempDir_ = std::move(dir); }
    // Riferimento condiviso: chi ha un invio pendente lo tiene finche' non si risolve, cosi'
    // la chiusura della finestra non cancella gli allegati prima del nuovo tentativo.
    std::shared_ptr<QTemporaryDir> tempDir() const { return tempDir_; }
    // Rimuove la bozza di questa finestra (dopo un invio riuscito con copia in Inviata).
    bool discardDraft();
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
    // La cartella Bozze e' cambiata su disco (bozza scritta, vecchia o inviata rimossa):
    // chi ha una vista di Bozze puo' rileggerla.
    void draftsChanged();

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
    QString draftPath_;
    QStringList draftKeys_; // chiavi di tutte le bozze legate (la corrente e le sostituite)
    std::shared_ptr<QTemporaryDir> tempDir_;
    bool loading_ = false;
    QString inReplyTo_;      // dal prefill (risposte): conservati nel messaggio
    QStringList references_;
    std::function<bool()> confirm_;
};

} // namespace boma
