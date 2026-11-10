#pragma once

#include "core/Config.h"
#include "core/MailActions.h"
#include "core/MailIndex.h"
#include "core/ProcessRunner.h"
#include "core/SyncService.h"
#include "mime/MessageParser.h"
#include "send/Sender.h"
#include "ui/ComposeWindow.h"
#include "ui/FolderPane.h"
#include "ui/MessageListModel.h"
#include "ui/MessageView.h"
#include "ui/ReplyBuilder.h"
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <functional>

namespace boma {

// Programmi esterni e percorsi (iniettabili nei test).
struct MainWindowOptions {
    QString muProgram = QStringLiteral("mu");
    QString muHome;       // vuoto = predefinito di mu
    QString muRoot;       // radice Maildir di mu (vuoto: [general] muRoot, poi `mu info store`)
    QString mbsyncProgram = QStringLiteral("mbsync");
    QString msmtpProgram = QStringLiteral("msmtp");
    QString gpgProgram;   // vuoto = "gpg"
    QString gnupgHome;    // vuoto = predefinito di gpg
    QString lockPath;     // vuoto = $XDG_RUNTIME_DIR o la cartella temporanea
    bool autoSync = true; // sincronizzazione all'avvio e periodica
};

// Finestra a tre colonne: account/cartelle | elenco | lettura. Tutto il lavoro
// pesante (mu, lettura/analisi del file, gpg, mbsync, msmtp) e' asincrono; i
// risultati di operazioni su un messaggio non piu' selezionato sono scartati
// (contatore di selezione) e ogni callback e' protetto da QPointer.
// L'unico codice che scrive nella Maildir e' MaildirWriter (Inviata/Bozze); le
// azioni sui messaggi passano da MailActions (mu move). Mai cancellazione
// definitiva: "Elimina" sposta nel cestino dell'account, senza conferma (come da
// specifica) ma con pulsante rosso, distinto dalle azioni sicure.
// Limite noto (rinvio del registro): la bozza salvata in Drafts durante la
// scrittura resta in Drafts anche dopo l'invio riuscito.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const Config& config, const MainWindowOptions& options = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    MessageListModel* model() const { return model_; }
    QString muRoot() const { return muRoot_; }
    QListView* listView() const { return list_; }
    QLineEdit* searchEdit() const { return search_; }
    FolderPane* folderPane() const { return folders_; }
    MessageView* messageView() const { return view_; }
    QTextBrowser* setupHelp() const { return help_; }
    QLabel* statusLabel() const { return syncLabel_; }
    QPushButton* newButton() const { return newBtn_; }
    QPushButton* replyButton() const { return replyBtn_; }
    QPushButton* replyAllButton() const { return replyAllBtn_; }
    QPushButton* forwardButton() const { return fwdBtn_; }
    QPushButton* deleteButton() const { return delBtn_; }
    Sender* sender() const { return sender_; }
    SyncService* syncService() const { return sync_; }
    // Finestre di scrittura aperte.
    QList<ComposeWindow*> composers() const;

    // Sostituibili nei test (default: QMessageBox).
    // "Inviare SENZA firma?": true = si'.
    void setConfirmUnsignedCallback(std::function<bool()> cb) { confirmUnsigned_ = std::move(cb); }
    // Avvisi/errori che richiedono l'attenzione dell'utente (titolo, testo).
    void setNoticeCallback(std::function<void(const QString&, const QString&)> cb) { notice_ = std::move(cb); }

public slots:
    void deleteSelected();
    void reply() { compose(ReplyMode::Reply); }
    void replyAll() { compose(ReplyMode::ReplyAll); }
    void forward() { compose(ReplyMode::Forward); }
    void newMessage();
    void syncNow();
    void focusSearch();

signals:
    // Il messaggio e' stato mostrato (segnato come letto solo DOPO questo segnale).
    void messageOpened(const QString& path);

private:
    void resolveMuRoot();
    void applyMuRoot(QString root, const QString& error);
    QString muRoot_, muRootError_;
    bool muRootReliable_ = false, muRootDetected_ = false;
    void onCurrentChanged(const QModelIndex& cur);
    void onLoaded(quint64 token, const MessageSummary& s, const ParsedMessage& pm, const QString& error);
    void markReadAfterOpen(const MessageSummary& s);
    void startVerify(quint64 token, const ParsedMessage& pm);
    void onDecryptRequested();
    void applyQuery(const QString& query);
    void refreshList();
    void onPageLoaded(int added, const QString& error);
    void clearViewer();
    void startLoad(quint64 token, const MessageSummary& s);
    void reindexAndRefresh();
    void onSyncState(SyncService::State st, const QString& accountId, const QString& message);
    void compose(ReplyMode mode);
    ComposeWindow* openCompose(const OutgoingMessage* prefill);
    void startSend(QPointer<ComposeWindow> c, const OutgoingMessage& msg, bool sign, bool encrypt, bool allowUnsigned);
    void onSendResult(QPointer<ComposeWindow> c, const OutgoingMessage& msg, bool sign, bool encrypt, const SendResult& r);
    void showNotice(const QString& title, const QString& text);
    void setHaveMessage(bool on);
    int rowForPath(const QString& path) const;
    Account accountFor(const QString& id) const;

    Config config_;
    MainWindowOptions opts_;
    ProcessRunner* runner_;
    MailIndex* index_;
    MailActions* actions_;
    SyncService* sync_;
    Sender* sender_;
    MessageListModel* model_;
    FolderPane* folders_;
    QListView* list_;
    QLineEdit* search_;
    QTimer* searchTimer_;
    QStackedWidget* stack_;
    MessageView* view_;
    QTextBrowser* help_;
    QPushButton *newBtn_, *replyBtn_, *replyAllBtn_, *fwdBtn_, *delBtn_, *syncBtn_;
    QLabel* syncLabel_;
    QProgressBar* progress_;

    quint64 selToken_ = 0;
    // apertura con debounce (M10): scorrendo l'elenco si analizza solo la riga su cui ci si ferma
    QTimer* openTimer_ = nullptr;
    MessageSummary pendingSummary_;
    quint64 pendingToken_ = 0;
    // azioni in corso (M4)
    bool deleting_ = false, decrypting_ = false;
    // aggiornamento dopo la sincronizzazione (M3): percorso da riselezionare
    QString keepPath_;
    int keepScroll_ = 0;
    bool keepPending_ = false;
    bool haveMsg_ = false;
    MessageSummary curSummary_;
    ParsedMessage curParsed_;
    // firma interna di un messaggio cifrato: puo' arrivare prima o dopo il testo
    bool decrypted_ = false, haveInner_ = false;
    SignatureStatus innerStatus_ = SignatureStatus::None;
    QString innerSigner_;
    QDateTime lastSync_;
    bool syncWasRunning_ = false;

    std::function<bool()> confirmUnsigned_;
    std::function<void(const QString&, const QString&)> notice_;
};

} // namespace boma
