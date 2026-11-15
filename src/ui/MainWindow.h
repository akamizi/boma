#pragma once
#include <QUrl>

#include "core/Config.h"
#include "core/MailActions.h"
#include "core/MailIndex.h"
#include "core/ProcessRunner.h"
#include "core/MailWatcher.h"
#include "core/SyncService.h"
#include "core/TagStore.h"
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
#include <QMap>
#include <QMainWindow>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <functional>

namespace boma {

class NewMailNotifier;
class UnreadCounter;
class SendController;

// Programmi esterni e percorsi (iniettabili nei test).
struct MainWindowOptions {
    QString muProgram = QStringLiteral("mu");
    QString muHome;       // vuoto = predefinito di mu
    QString muRoot;       // radice Maildir di mu (vuoto: [general] muRoot, poi `mu info store`)
    QString mbsyncProgram = QStringLiteral("mbsync");
    QString msmtpProgram = QStringLiteral("msmtp");
    QString gpgProgram;   // vuoto = "gpg"
    QString gnupgHome;    // vuoto = predefinito di gpg
    QString configPath;   // file di Boma da salvare dalle Impostazioni (vuoto = Config::defaultPath())
    QString mbsyncConfig; // ~/.mbsyncrc per i controlli delle Impostazioni (vuoto = predefinito)
    QString msmtpConfig;  // ~/.msmtprc (vuoto = predefinito)
    QString unsavedDir;   // copie di emergenza degli inviati (vuoto = default di Sender)
    bool watchMail = true; // osserva le cartelle "new" per la posta scaricata da altri programmi
    QString tagsFile;     // file JSON delle etichette (vuoto = <dati app>/tags.json)
};

// Finestra a tre colonne: account/cartelle | elenco | lettura. Tutto il lavoro
// pesante (mu, lettura/analisi del file, gpg, mbsync, msmtp) e' asincrono; i
// risultati di operazioni su un messaggio non piu' selezionato sono scartati
// (contatore di selezione) e ogni callback e' protetto da QPointer.
// L'unico codice che scrive nella Maildir e' MaildirWriter (Inviata/Bozze); le
// azioni sui messaggi passano da MailActions (mu move). Mai cancellazione
// definitiva: "Elimina" sposta nel cestino dell'account, senza conferma (come da
// specifica) ma con pulsante rosso, distinto dalle azioni sicure.
// Bozze: da Bozze un messaggio si riapre in ComposeWindow (doppio clic, Invio, pulsante
// Modifica). Il salvataggio sostituisce la stessa bozza e l'invio riuscito (con copia in
// Inviata) la rimuove; se la copia in Inviata fallisce la bozza resta. Una bozza non si
// apre in due finestre. Gli allegati si estraggono in una cartella temporanea eliminata
// alla chiusura della finestra.
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
    QPushButton* syncButton() const { return syncBtn_; }
    QPushButton* replyButton() const { return replyBtn_; }
    QPushButton* replyAllButton() const { return replyAllBtn_; }
    QPushButton* forwardButton() const { return fwdBtn_; }
    // Visibile solo nella cartella Bozze; riapre la bozza selezionata nel composer.
    QPushButton* editButton() const { return editBtn_; }
    // Riapre una bozza (anche per i test). Se e' gia' aperta porta in primo piano la finestra.
    void openDraft(const MessageSummary& s);
    bool isDraftSummary(const MessageSummary& s) const;
    QPushButton* deleteButton() const { return delBtn_; }
    // Operazioni sui messaggi selezionati con le caselle (barra sopra l'elenco).
    QWidget* bulkBar() const { return bulkBar_; }
    void markCheckedRead();
    void deleteChecked();
    // Barra sopra l'elenco: agisce sui messaggi spuntati (Ordina sempre attivo).
    QWidget* listBar() const { return listBar_; }
    void moveChecked(const QString& accountId, const QString& folder, const QString& label);
    void archiveChecked();
    void setCheckedFlags(const QString& add, const QString& remove, const QString& label);
    void setListSort(const QString& field, bool descending);
    Sender* sender() const { return sender_; }
    SyncService* syncService() const { return sync_; }
    // Finestre di scrittura aperte.
    QList<ComposeWindow*> composers() const;

    // Sostituibili nei test (default: QMessageBox).
    // "Inviare SENZA firma?": true = si'.
    // Comandi del menu delle cartelle. Usano la query/percorso della cartella scelta.
    void markFolderRead(const QString& accountId, const QString& query, const QString& label);
    void emptyFolder(const QString& accountId, const QString& dir, const QString& label);
    // Conferma delle azioni distruttive (titolo, testo); sostituibile nei test.
    void setConfirmDestructiveCallback(std::function<bool(const QString&, const QString&)> cb) { confirmDestructive_ = std::move(cb); }
    bool bulkBusy() const { return bulkBusy_; }
    void setConfirmUnsignedCallback(std::function<bool()> cb) { confirmUnsigned_ = std::move(cb); }
    // Avvisi/errori che richiedono l'attenzione dell'utente (titolo, testo).
    void setNoticeCallback(std::function<void(const QString&, const QString&)> cb) { notice_ = std::move(cb); }
    // "Salva allegato": riceve il percorso proposto, restituisce quello scelto (vuoto = annulla).
    // Default: QFileDialog (senza la sua conferma di sovrascrittura: non si sovrascrive mai).
    void setSaveFileCallback(std::function<QString(const QString&)> cb) { saveFile_ = std::move(cb); }
    // Conferma prima di aprire un link (url, testo con la destinazione reale): true = apri.
    void setConfirmLinkCallback(std::function<bool(const QUrl&, const QString&)> cb) { confirmLink_ = std::move(cb); }
    // Apertura effettiva (default: QDesktopServices::openUrl).
    void setOpenUrlCallback(std::function<void(const QUrl&)> cb) { openUrl_ = std::move(cb); }

public slots:
    void deleteSelected();
    void reply() { compose(ReplyMode::Reply); }
    void replyAll() { compose(ReplyMode::ReplyAll); }
    void forward() { compose(ReplyMode::Forward); }
    void newMessage();
    void syncNow();
    // Aggiorna un solo account (id vuoto = tutti).
    void syncAccount(const QString& accountId);
    QToolButton* syncMenuButton() const { return syncMenuBtn_; }
    void focusSearch();
    // Finestra Impostazioni: salva il file di Boma e applica la nuova configurazione
    // a caldo. Rifiutata durante un invio, una sincronizzazione o con finestre di
    // scrittura aperte (conservano la configurazione con cui sono nate).
    void openSettings();
    // Applica una configurazione senza riavviare (usata da openSettings).
    void applyConfig(const Config& config);
public:
    QPushButton* settingsButton() const { return settingsBtn_; }
    // Sostituibile nei test: riceve la configurazione attuale e restituisce la nuova
    // (Config vuota con ok=false = annullato).
    void setSettingsCallback(std::function<bool(const Config& current, Config* out)> cb) { settingsCb_ = std::move(cb); }
public slots:

protected:
    // Mai chiudere durante un invio; ogni finestra di scrittura aperta viene chiusa
    // (e chiede conferma se ha modifiche): se una rifiuta, nulla si chiude.
    void closeEvent(QCloseEvent* e) override;

signals:
    // Il messaggio e' stato mostrato (segnato come letto solo DOPO questo segnale).
    void messageOpened(const QString& path);

private:
    void checkSpecialFolders();
    void resolveMuRoot();
    void applyMuRoot(QString root, const QString& error);
    QString muRoot_, muRootError_;
    bool muRootReliable_ = false, muRootDetected_ = false;
    void onCurrentChanged(const QModelIndex& cur);
    void onLoaded(quint64 token, const MessageSummary& s, const ParsedMessage& pm, const QByteArray& raw,
                  const QString& error);
    void saveAttachment(int index);
    void openLink(const QUrl& url);
    void markReadAfterOpen(const MessageSummary& s);
    void startVerify(quint64 token, const ParsedMessage& pm);
    void onDecryptRequested();
    void applyQuery(const QString& query);
    // Sostituisce i termini tag:nome / tag:"nome con spazi" con una query msgid: equivalente.
    QString expandTagQuery(const QString& query) const;
    void setCheckedTag(const QString& tag, bool on);
    void buildTagMenu(QMenu* menu);
    void importGmailLabels(const QString& channel, const QString& name);
    bool gmailBusy_ = false;
    void refreshList();
    void onPageLoaded(int added, const QString& error);
    void clearViewer();
    void startLoad(quint64 token, const MessageSummary& s);
    void runReindex();
    void reindexAndRefresh(std::function<void()> done = {});
    // La cartella Bozze e' cambiata su disco (salvataggio, sostituzione, scarto dopo l'invio):
    // `mu index` (incrementale) e, se la vista mostra le Bozze, rilettura dell'elenco.
    void draftsChanged();
    bool viewingDrafts() const;
    void onSyncState(SyncService::State st, const QString& accountId, const QString& message);
    void compose(ReplyMode mode);
    ComposeWindow* openCompose(const OutgoingMessage* prefill);
    void updateDraftUi();
    void editSelectedDraft();
    QPushButton* editBtn_ = nullptr;
    QSet<QString> openingDrafts_; // chiavi delle bozze in lettura (evita doppie aperture)
    SendController* sendCtl_ = nullptr; // flusso di invio (logica in core/SendController)
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
    TagStore* tags_ = nullptr;
    MailWatcher* watcher_ = nullptr;
    void watchMail();
    void setupNewMailNotifier();
    NewMailNotifier* newMail_ = nullptr;
    FolderPane* folders_;
    QListView* list_;
    QLineEdit* search_;
    QTimer* searchTimer_;
    QStackedWidget* stack_;
    MessageView* view_;
    QTextBrowser* help_;
    QWidget* bulkBar_ = nullptr;
    QWidget* listBar_ = nullptr;
    QToolButton* syncMenuBtn_ = nullptr;
    void setSyncEnabled(bool on);
    QList<QWidget*> checkedOnly_; // pulsanti attivi solo con messaggi spuntati
    void bulkApply(const QString& title, const QList<MessageSummary>& items,
                   std::function<void(const MessageSummary&, Callback)> op, bool removesFromList,
                   const QString& doneText);
    bool moveGuard(const QString& title);
    QLabel* bulkLabel_ = nullptr;
    QPushButton *newBtn_, *replyBtn_, *replyAllBtn_, *fwdBtn_, *delBtn_, *syncBtn_, *settingsBtn_;
    std::function<bool(const Config&, Config*)> settingsCb_;
    void updateEmptyState();
    QLabel* syncLabel_;
    QProgressBar* progress_;

    quint64 selToken_ = 0;
    // apertura con debounce (M10): scorrendo l'elenco si analizza solo la riga su cui ci si ferma
    QTimer* openTimer_ = nullptr;
    UnreadCounter* unread_ = nullptr; // conteggio dei non letti per cartella
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
    QByteArray curRaw_; // byte da cui e' stato analizzato curParsed_ (in chiaro dopo Decifra)
    // firma interna di un messaggio cifrato: puo' arrivare prima o dopo il testo
    bool decrypted_ = false, haveInner_ = false;
    SignatureStatus innerStatus_ = SignatureStatus::None;
    QString innerSigner_;
    QDateTime lastSync_;
    bool reindexing_ = false;
    QTimer* draftsTimer_ = nullptr;
    bool bulkBusy_ = false;
    std::function<bool(const QString&, const QString&)> confirmDestructive_;
    bool bulkAllowed();
    bool eventFilter(QObject* o, QEvent* e) override;

    std::function<bool()> confirmUnsigned_;
    std::function<void(const QString&, const QString&)> notice_;
    std::function<QString(const QString&)> saveFile_;
    std::function<bool(const QUrl&, const QString&)> confirmLink_;
    std::function<void(const QUrl&)> openUrl_;
};

} // namespace boma
