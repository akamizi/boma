#include "ui/MainWindow.h"

#include "ui/ReplyBuilder.h"
#include "core/MaildirWriter.h"
#include "ui/SettingsDialog.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThreadPool>
#include <QVBoxLayout>
#include <memory>

namespace boma {

namespace {

constexpr qint64 kMaxMessageBytes = 64LL * 1024 * 1024; // come MessageParser

struct Loaded {
    ParsedMessage pm;
    QByteArray raw;
    QString error;
};

// Esegue work() su un thread del pool e consegna il risultato sul thread della
// GUI, solo se la finestra esiste ancora (QPointer controllato sul thread GUI).
template <class R, class W, class D>
void offThread(MainWindow* self, W work, D deliver) {
    QPointer<MainWindow> p(self);
    QObject* app = QCoreApplication::instance();
    QThreadPool::globalInstance()->start([p, app, work = std::move(work), deliver = std::move(deliver)]() {
        auto res = std::make_shared<R>(work());
        QMetaObject::invokeMethod(
            app, [p, res, deliver]() {
                if (p) deliver(std::move(*res));
            },
            Qt::QueuedConnection);
    });
}

} // namespace

MainWindow::MainWindow(const Config& config, const MainWindowOptions& options, QWidget* parent)
    : QMainWindow(parent), config_(config), opts_(options) {
    setWindowTitle(QStringLiteral("Boma"));
    resize(1200, 720);

    qRegisterMetaType<boma::OutgoingMessage>();
    runner_ = new ProcessRunner(this);
    index_ = new MailIndex(this, opts_.muProgram, opts_.muHome);
    index_->setAccounts(config_.accounts());
    actions_ = new MailActions(this, opts_.muProgram);
    actions_->setMuHome(opts_.muHome);
    sync_ = new SyncService(runner_, config_, this);
    sync_->setMbsyncProgram(opts_.mbsyncProgram);
    sender_ = new Sender(this);
    sender_->setAccounts(config_.accounts());
    sender_->setMsmtpProgram(opts_.msmtpProgram);
    sender_->setMuProgram(opts_.muProgram);
    sender_->setMuHome(opts_.muHome);
    if (!opts_.unsavedDir.isEmpty()) sender_->setUnsavedDir(opts_.unsavedDir);
    if (!opts_.gpgProgram.isEmpty()) sender_->gpg().setGpgProgram(opts_.gpgProgram);
    if (!opts_.gnupgHome.isEmpty()) sender_->gpg().setGnupgHome(opts_.gnupgHome);
    model_ = new MessageListModel(config_.isEmpty() ? nullptr : index_, this);

    // ---- interfaccia ----
    auto* central = new QWidget(this);
    auto* top = new QVBoxLayout(central);
    top->setContentsMargins(4, 4, 4, 4);
    auto* searchRow = new QHBoxLayout;
    search_ = new QLineEdit(central);
    search_->setPlaceholderText(tr("Cerca (vuoto = non lette)   [Ctrl+F o /]"));
    search_->setClearButtonEnabled(true);
    syncBtn_ = new QPushButton(tr("Aggiorna"), central);
    syncBtn_->setToolTip(tr("Scarica la posta e aggiorna l'elenco (F5)"));
    settingsBtn_ = new QPushButton(tr("Impostazioni"), central);
    settingsBtn_->setToolTip(tr("Account, identita', mbsync, msmtp e PGP (Ctrl+,)"));
    searchRow->addWidget(search_, 1);
    searchRow->addWidget(syncBtn_);
    searchRow->addWidget(settingsBtn_);
    top->addLayout(searchRow);

    auto* split = new QSplitter(central);
    folders_ = new FolderPane(config_, split);
    list_ = new QListView(split);
    list_->setModel(model_);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* right = new QWidget(split);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    auto* btnRow = new QHBoxLayout;
    newBtn_ = new QPushButton(tr("Nuovo"), right);
    replyBtn_ = new QPushButton(tr("Rispondi"), right);
    replyAllBtn_ = new QPushButton(tr("Rispondi a tutti"), right);
    fwdBtn_ = new QPushButton(tr("Inoltra"), right);
    delBtn_ = new QPushButton(tr("Elimina"), right);
    delBtn_->setToolTip(tr("Sposta nel cestino (Canc). Senza conferma, ma recuperabile dal cestino."));
    // azione distruttiva (anche se recuperabile): visivamente distinta
    delBtn_->setStyleSheet(QStringLiteral("QPushButton{background-color:#fde7e9;color:#a4262c;border:1px solid #d13438;"
                                          "border-radius:3px;padding:3px 10px;}"
                                          "QPushButton:disabled{background-color:#f5f5f5;color:#999;border:1px solid #ccc;}"));
    btnRow->addWidget(newBtn_);
    btnRow->addWidget(replyBtn_);
    btnRow->addWidget(replyAllBtn_);
    btnRow->addWidget(fwdBtn_);
    btnRow->addStretch(1);
    btnRow->addSpacing(24);
    btnRow->addWidget(delBtn_);
    rl->addLayout(btnRow);
    stack_ = new QStackedWidget(right);
    view_ = new MessageView(stack_);
    help_ = new QTextBrowser(stack_);
    help_->setOpenLinks(false);
    help_->setObjectName(QStringLiteral("setupHelp"));
    stack_->addWidget(view_);
    stack_->addWidget(help_);
    rl->addWidget(stack_, 1);

    split->addWidget(folders_);
    split->addWidget(list_);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 4);
    top->addWidget(split, 1);
    setCentralWidget(central);

    syncLabel_ = new QLabel(tr("Non ancora sincronizzato"), this);
    progress_ = new QProgressBar(this);
    progress_->setMaximumWidth(160);
    progress_->setTextVisible(false);
    progress_->hide();
    statusBar()->addPermanentWidget(syncLabel_);
    statusBar()->addPermanentWidget(progress_);

    setHaveMessage(false);

    // ---- collegamenti ----
    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(250); // attesa: una ricerca per pausa di digitazione
    connect(search_, &QLineEdit::textChanged, searchTimer_, qOverload<>(&QTimer::start));
    connect(searchTimer_, &QTimer::timeout, this, [this] { applyQuery(search_->text().trimmed()); });
    connect(folders_, &FolderPane::querySelected, this, [this](const QString& q) {
        QSignalBlocker b(search_);
        search_->clear();
        searchTimer_->stop();
        applyQuery(q);
    });
    openTimer_ = new QTimer(this);
    openTimer_->setSingleShot(true);
    openTimer_->setInterval(100);
    connect(openTimer_, &QTimer::timeout, this, [this] {
        if (pendingToken_ == selToken_) startLoad(pendingToken_, pendingSummary_);
    });
    connect(model_, &MessageListModel::pageLoaded, this, &MainWindow::onPageLoaded);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur) { onCurrentChanged(cur); });
    connect(view_, &MessageView::decryptRequested, this, &MainWindow::onDecryptRequested);
    connect(view_, &MessageView::saveAttachmentRequested, this, &MainWindow::saveAttachment);
    connect(view_, &MessageView::linkActivated, this, &MainWindow::openLink);
    connect(newBtn_, &QPushButton::clicked, this, &MainWindow::newMessage);
    connect(replyBtn_, &QPushButton::clicked, this, &MainWindow::reply);
    connect(replyAllBtn_, &QPushButton::clicked, this, &MainWindow::replyAll);
    connect(fwdBtn_, &QPushButton::clicked, this, &MainWindow::forward);
    connect(delBtn_, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(syncBtn_, &QPushButton::clicked, this, &MainWindow::syncNow);
    connect(settingsBtn_, &QPushButton::clicked, this, &MainWindow::openSettings);
    connect(sync_, &SyncService::stateChanged, this, &MainWindow::onSyncState);
    // All'uscita il mbsync in corso e' fermato con SIGTERM (mai lasciato orfano);
    // il contesto `this` scollega la callback se la finestra sparisce prima.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { sync_->shutdown(); });
    connect(sender_, &Sender::reindexFinished, this, [this](bool ok, const QString& err) {
        if (!ok) {
            statusBar()->showMessage(tr("Attenzione: aggiornamento dell'indice dopo l'invio non riuscito: %1").arg(err));
            return;
        }
        refreshList(); // il messaggio in Inviata e' ora cercabile (selezione mantenuta)
    });

    // scorciatoie: i tasti semplici non scattano mentre si scrive nel campo di ricerca
    auto sc = [this](const QKeySequence& k, auto slot) {
        auto* s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, slot);
    };
    // Ctrl+Q chiude la finestra: passa da closeEvent (invio in corso, bozze aperte).
    sc(QKeySequence(QKeySequence::Quit), [this]() { close(); });
    sc(QKeySequence(QKeySequence::Find), &MainWindow::focusSearch);
    sc(QKeySequence(Qt::Key_Slash), &MainWindow::focusSearch);
    sc(QKeySequence(Qt::Key_Delete), &MainWindow::deleteSelected);
    sc(QKeySequence(Qt::Key_R), &MainWindow::reply);
    sc(QKeySequence(Qt::SHIFT | Qt::Key_R), &MainWindow::replyAll);
    sc(QKeySequence(Qt::Key_F), &MainWindow::forward);
    sc(QKeySequence(QKeySequence::New), &MainWindow::newMessage);
    sc(QKeySequence(Qt::Key_F5), &MainWindow::syncNow);
    sc(QKeySequence(Qt::CTRL | Qt::Key_Comma), &MainWindow::openSettings);

    // ---- avvio: nessun lavoro sincrono ----
    resolveMuRoot();
    updateEmptyState();
    if (!config_.isEmpty()) {
        applyQuery(QString());
        checkSpecialFolders();
    }
}

void MainWindow::updateEmptyState() {
    const bool empty = config_.isEmpty();
    if (empty) {
        help_->setHtml(tr("<h2>Benvenuto in Boma</h2><p>Nessun account configurato.</p>"
                          "<p>Premi <b>Impostazioni</b> (Ctrl+,) per aggiungere un account e una identit&agrave;, "
                          "oppure crea a mano il file <b>%1</b>, ad esempio:</p>"
                          "<pre>[account:personale]\nname=Personale\nmaildirRoot=~/Mail/personale\n"
                          "mbsyncChannel=personale\nsentFolder=Sent\ndraftsFolder=Drafts\ntrashFolder=Trash\n\n"
                          "[identity:principale]\naccountId=personale\naddress=io@example.org\n"
                          "displayName=Il mio nome\nmsmtpAccount=personale</pre>"
                          "<p>Le password non vanno nel file: le gestiscono mbsync (PassCmd) e msmtp (passwordeval).</p>")
                          .arg(Config::defaultPath().toHtmlEscaped()));
        stack_->setCurrentWidget(help_);
        syncLabel_->setText(tr("Nessun account configurato"));
    } else {
        stack_->setCurrentWidget(view_);
        if (syncLabel_->text() == tr("Nessun account configurato")) syncLabel_->setText(tr("Non ancora sincronizzato"));
    }
    search_->setEnabled(!empty);
    syncBtn_->setEnabled(!empty);
    newBtn_->setEnabled(!empty);
}

void MainWindow::applyConfig(const Config& c) {
    config_ = c;
    index_->setAccounts(c.accounts());
    sender_->setAccounts(c.accounts());
    sync_->setConfig(c);
    folders_->setConfig(c);
    actions_->setMuRoot(QString()); // finche' la radice non e' riconfermata, Elimina resta disattivato
    muRootDetected_ = false;
    muRootReliable_ = false;
    muRootError_.clear();
    muRoot_.clear();
    folders_->setMuRoot(QString());
    model_->setIndex(c.isEmpty() ? nullptr : index_);
    clearViewer();
    resolveMuRoot();
    updateEmptyState();
    if (!c.isEmpty()) {
        applyQuery(search_->text().trimmed());
        checkSpecialFolders();
    }
}

void MainWindow::openSettings() {
    if (sender_->busy()) {
        showNotice(tr("Impostazioni"), tr("Un messaggio e' in fase di invio: attendi la fine dell'invio."));
        return;
    }
    if (sync_->state() == SyncService::Running || reindexing_) {
        showNotice(tr("Impostazioni"), tr("Un aggiornamento e' in corso: attendi che finisca."));
        return;
    }
    if (!composers().isEmpty()) {
        showNotice(tr("Impostazioni"), tr("Chiudi prima le finestre di scrittura aperte: conservano la configurazione con cui sono state aperte."));
        return;
    }
    Config nc;
    if (settingsCb_) {
        if (!settingsCb_(config_, &nc)) return;
    } else {
        const SettingsEnvironment env{opts_.mbsyncConfig, opts_.msmtpConfig, opts_.gpgProgram, opts_.gnupgHome};
        SettingsDialog dlg(config_, env, this);
        if (dlg.exec() != QDialog::Accepted) return;
        nc = dlg.result();
    }
    QString err;
    const QString path = opts_.configPath.isEmpty() ? Config::defaultPath() : opts_.configPath;
    if (!nc.save(path, &err)) {
        showNotice(tr("Impostazioni"), tr("Salvataggio non riuscito: %1").arg(err));
        return;
    }
    applyConfig(nc);
    statusBar()->showMessage(tr("Impostazioni salvate"), 5000);
}

MainWindow::~MainWindow() {
    // Prima di distruggere i figli (ProcessRunner ucciderebbe con SIGKILL):
    // il mbsync in corso riceve SIGTERM, resta sicuro e riprende al prossimo avvio.
    if (sync_) sync_->shutdown();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (sender_ && sender_->busy()) {
        e->ignore();
        showNotice(tr("Invio in corso"),
               tr("Un messaggio e' in fase di invio. Attendi la fine dell'invio prima di chiudere Boma."));
        return;
    }
    QList<QPointer<ComposeWindow>> open;
    for (ComposeWindow* c : composers())
        open << c;
    for (const QPointer<ComposeWindow>& c : open) {
        if (c && !c->close()) {
            e->ignore();
            c->raise();
            c->activateWindow();
            return;
        }
    }
    QMainWindow::closeEvent(e);
}

// I4: Inviata/Bozze/Cestino devono essere Maildir valide (MaildirWriter e mu
// move non creano mai cartelle). Controllo fuori dal thread GUI; un account
// ancora vuoto (prima sincronizzazione: le cartelle le crea mbsync) e' saltato.
// L'invio da un account con Inviata non valida e' comunque rifiutato da Sender.
void MainWindow::checkSpecialFolders() {
    const QList<Account> accounts = config_.accounts();
    QPointer<MainWindow> self(this);
    offThread<QStringList>(
        this,
        [accounts]() {
            QStringList bad;
            for (const Account& a : accounts) {
                if (a.maildirRoot.isEmpty()) continue;
                const QList<QPair<QString, QString>> folders{{QObject::tr("Inviata (sentFolder)"), a.sentFolder},
                                                              {QObject::tr("Bozze (draftsFolder)"), a.draftsFolder},
                                                              {QObject::tr("Cestino (trashFolder)"), a.trashFolder}};
                for (const auto& [label, f] : folders) {
                    const QString why = MaildirWriter::checkFolder(f.isEmpty() ? QString() : QDir(a.maildirRoot).filePath(f));
                    if (!why.isEmpty())
                        bad << QObject::tr("%1 - %2 \"%3\": %4").arg(a.name.isEmpty() ? a.id : a.name, label, f, why);
                }
            }
            return bad;
        },
        [self](QStringList bad) {
            if (!self || bad.isEmpty()) return;
            self->statusBar()->showMessage(tr("Cartelle dell'account non valide: vedi l'avviso"), 30000);
            const QString title = tr("Cartelle non valide");
            const QString text = tr("Queste cartelle non sono Maildir utilizzabili:\n\n%1\n\n"
                                    "Finche' non le correggi nella configurazione l'invio da quell'account e' bloccato "
                                    "(nulla parte senza la copia in Inviata), le bozze non si salvano ed Elimina non funziona.")
                                     .arg(bad.join(QLatin1Char('\n')));
            if (self->notice_) {
                self->notice_(title, text);
                return;
            }
            // Avviso NON modale: all'avvio non deve fermare il ciclo di eventi
            // (sincronizzazione, prima sincronizzazione, elenco).
            auto* box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Ok, self.data());
            box->setTextFormat(Qt::PlainText);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setModal(false);
            box->show();
        });
}

QList<ComposeWindow*> MainWindow::composers() const {
    return findChildren<ComposeWindow*>(QString(), Qt::FindDirectChildrenOnly);
}

void MainWindow::setHaveMessage(bool on) {
    haveMsg_ = on;
    replyBtn_->setEnabled(on);
    replyAllBtn_->setEnabled(on);
    fwdBtn_->setEnabled(on);
    delBtn_->setEnabled(on && !deleting_);
}

void MainWindow::focusSearch() {
    if (!search_->isEnabled()) return;
    search_->setFocus();
    search_->selectAll();
}

Account MainWindow::accountFor(const QString& id) const {
    for (const Account& a : config_.accounts())
        if (a.id == id) return a;
    return {};
}

int MainWindow::rowForPath(const QString& path) const {
    for (int i = 0; i < model_->rowCount(); ++i)
        if (model_->summary(i).path == path) return i;
    return -1;
}

void MainWindow::showNotice(const QString& title, const QString& text) {
    if (notice_)
        notice_(title, text);
    else
        QMessageBox::warning(this, title, text);
}

// ---- elenco e ricerca ----

void MainWindow::applyQuery(const QString& query) {
    keepPending_ = false;
    model_->setQuery(query); // generazione: i risultati della query precedente sono scartati
    if (model_->canFetchMore({})) model_->fetchMore({});
}

// Dopo la sincronizzazione: l'elenco viene riletto ma la selezione (e lo scorrimento)
// restano. La selezione non emette segnali durante il reset: il messaggio aperto non
// viene riletto ne' richiuso; se dopo la prima pagina non c'e' piu', il visore si svuota.
void MainWindow::refreshList() {
    keepPending_ = haveMsg_ && !curSummary_.path.isEmpty();
    keepPath_ = curSummary_.path;
    keepScroll_ = list_->verticalScrollBar()->value();
    {
        QSignalBlocker b(list_->selectionModel());
        model_->reset();
    }
    if (model_->canFetchMore({})) model_->fetchMore({});
}

void MainWindow::onPageLoaded(int, const QString& error) {
    if (!keepPending_) return;
    keepPending_ = false;
    if (!error.isEmpty()) { // l'elenco e' stato azzerato: il visore non deve restare incoerente
        clearViewer();
        return;
    }
    const int row = rowForPath(keepPath_);
    if (row < 0) {
        clearViewer();
        return;
    }
    {
        QSignalBlocker b(list_->selectionModel());
        list_->selectionModel()->setCurrentIndex(model_->index(row, 0), QItemSelectionModel::ClearAndSelect);
    }
    list_->verticalScrollBar()->setValue(keepScroll_);
    curSummary_ = model_->summary(row);
}

void MainWindow::clearViewer() {
    ++selToken_;
    openTimer_->stop();
    decrypted_ = haveInner_ = false;
    setHaveMessage(false);
    view_->show(ParsedMessage());
}

void MainWindow::reindexAndRefresh(std::function<void()> done) {
    // nuova posta: aggiorna l'indice di mu (asincrono) e poi l'elenco;
    // done (se dato) parte quando mu index ha finito, ok o no.
    QPointer<MainWindow> self(this);
    index_->reindex(QString(), [self, done = std::move(done)](bool ok, QString err) {
        if (!self) return;
        if (done) QTimer::singleShot(0, self, done);
        if (!ok)
            self->statusBar()->showMessage(tr("Aggiornamento dell'indice non riuscito: %1").arg(err), 10000);
        else
            self->refreshList();
    });
}

// ---- lettura ----

void MainWindow::onCurrentChanged(const QModelIndex& cur) {
    const quint64 token = ++selToken_; // invalida tutto cio' che riguarda la selezione precedente
    decrypted_ = haveInner_ = false;
    innerStatus_ = SignatureStatus::None;
    setHaveMessage(false);
    openTimer_->stop();
    if (!cur.isValid() || cur.row() >= model_->rowCount()) return;
    // Debounce: scorrendo velocemente si analizza solo la riga su cui ci si ferma.
    pendingSummary_ = model_->summary(cur.row());
    pendingToken_ = token;
    openTimer_->start();
}

void MainWindow::startLoad(quint64 token, const MessageSummary& s) {
    // Scelta: lettura e analisi del singolo messaggio nel pool di thread (il file
    // puo' arrivare a 64 MiB): la GUI non attende mai il disco ne' GMime.
    offThread<Loaded>(
        this,
        [path = s.path]() {
            Loaded l;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                l.error = QObject::tr("Impossibile leggere il file: %1").arg(f.errorString());
                return l;
            }
            if (f.size() > kMaxMessageBytes) {
                l.error = QObject::tr("Messaggio troppo grande per essere mostrato");
                return l;
            }
            l.raw = f.read(kMaxMessageBytes + 1);
            l.pm = MessageParser::parse(l.raw, &l.error);
            return l;
        },
        [this, token, s](Loaded l) { onLoaded(token, s, l.pm, l.raw, l.error); });
}

void MainWindow::onLoaded(quint64 token, const MessageSummary& s, const ParsedMessage& pm, const QByteArray& raw,
                          const QString& error) {
    if (token != selToken_) return; // selezione superata
    if (!error.isEmpty()) {
        ParsedMessage e;
        e.subject = s.subject;
        e.from = s.from;
        e.plainText = tr("Impossibile aprire il messaggio: %1").arg(error);
        view_->show(e);
        return;
    }
    curSummary_ = s;
    curParsed_ = pm;
    curRaw_ = raw;
    view_->show(pm);
    setHaveMessage(true);
    emit messageOpened(s.path);
    markReadAfterOpen(s); // DOPO la visualizzazione
    startVerify(token, pm);
}

void MainWindow::markReadAfterOpen(const MessageSummary& s) {
    if (!s.unread) return;
    QPointer<MainWindow> self(this);
    actions_->markRead(s.path, [self, s](bool ok, QString err) {
        if (!self) return;
        if (!ok) {
            self->statusBar()->showMessage(tr("Impossibile segnare come letto: %1").arg(err), 10000);
            return;
        }
        auto apply = [self, s](const QString& newPath) {
            if (!self) return;
            const int row = self->rowForPath(s.path);
            if (row >= 0) self->model_->markReadLocal(row, newPath);
            if (self->curSummary_.path == s.path) {
                self->curSummary_.unread = false;
                if (!newPath.isEmpty()) self->curSummary_.path = newPath;
            }
        };
        if (!s.path.contains(QStringLiteral("/new/"))) {
            apply(QString());
            return;
        }
        // mu sposta i file di new/ in cur/: ririsolve il percorso (sola lettura).
        // La lettura di cur/ (migliaia di file) avviene fuori dal thread GUI.
        offThread<QString>(
            self.data(),
            [path = s.path]() {
                const int slash = path.lastIndexOf(QLatin1Char('/'));
                const QString base = path.mid(slash + 1).section(QLatin1Char(':'), 0, 0);
                const QString curDir = path.left(path.lastIndexOf(QStringLiteral("/new/"))) + QStringLiteral("/cur");
                const QStringList found = QDir(curDir).entryList({base + QLatin1Char('*')}, QDir::Files);
                return found.isEmpty() ? QString() : curDir + QLatin1Char('/') + found.first();
            },
            apply);
    });
}

void MainWindow::startVerify(quint64 token, const ParsedMessage& pm) {
    if (!pm.signedPgp) return;
    QPointer<MainWindow> self(this);
    sender_->gpg().verify(pm.rawSignedPart, pm.rawSignature, [self, token](SignatureStatus st, QString signer) {
        if (!self || token != self->selToken_) return; // messaggio non piu' selezionato
        self->view_->setSignatureResult(st, signer);
    });
}

void MainWindow::onDecryptRequested() {
    if (!haveMsg_ || !curParsed_.encryptedPgp) return;
    if (decrypting_) {
        statusBar()->showMessage(tr("Decifratura gia' in corso"), 5000);
        return;
    }
    decrypting_ = true; // un solo gpg --decrypt alla volta
    const quint64 token = selToken_;
    QPointer<MainWindow> self(this);
    sender_->gpg().decrypt(
        view_->encryptedData(),
        [self, token](bool ok, QByteArray plain, QString error) {
            if (!self) return;
            if (!ok) self->decrypting_ = false;
            if (token != self->selToken_) {
                self->decrypting_ = false;
                return;
            }
            if (!ok) {
                self->view_->showDecryptError(error);
                return;
            }
            // il testo in chiaro puo' essere grande: analisi fuori dal thread GUI
            offThread<Loaded>(
                self.data(),
                [plain = std::move(plain)]() {
                    Loaded l;
                    l.pm = MessageParser::parse(plain, &l.error);
                    l.raw = plain;
                    return l;
                },
                [self, token](Loaded l) {
                    self->decrypting_ = false;
                    if (token != self->selToken_) return;
                    if (!l.error.isEmpty()) {
                        self->view_->showDecryptError(tr("Contenuto decifrato non interpretabile: %1").arg(l.error));
                        return;
                    }
                    ParsedMessage inner = l.pm;
                    const ParsedMessage& o = self->curParsed_;
                    if (inner.from.isEmpty()) inner.from = o.from;
                    if (inner.to.isEmpty()) inner.to = o.to;
                    if (inner.cc.isEmpty()) inner.cc = o.cc;
                    if (inner.subject.isEmpty()) inner.subject = o.subject;
                    if (!inner.date.isValid()) inner.date = o.date;
                    inner.messageId = o.messageId;
                    inner.references = o.references;
                    inner.replyTo = o.replyTo;
                    // I1: se il cifrato era annidato accanto a parti in chiaro, anche
                    // il contenuto decifrato resta "solo in parte" (mai badge verde).
                    if (o.pgpNested) inner.pgpNested = true;
                    self->decrypted_ = true;
                    self->curParsed_ = inner;
                    self->curRaw_ = l.raw; // allegati: indici riferiti al contenuto decifrato
                    self->curParsed_.encryptedPgp = false;
                    self->view_->showDecrypted(inner);
                    if (self->haveInner_) self->view_->setSignatureResult(self->innerStatus_, self->innerSigner_);
                    self->startVerify(token, inner); // PGP/MIME firmato dentro il cifrato
                });
        },
        [self, token](SignatureStatus st, QString signer) {
            if (!self || token != self->selToken_) return;
            self->haveInner_ = true;
            self->innerStatus_ = st;
            self->innerSigner_ = signer;
            if (self->decrypted_) self->view_->setSignatureResult(st, signer); // altrimenti dopo showDecrypted
        });
}

// ---- allegati e link ----

void MainWindow::saveAttachment(int index) {
    if (!haveMsg_ || index < 0 || index >= curParsed_.attachments.size()) return;
    const Attachment a = curParsed_.attachments.at(index);
    // Byte del messaggio e indice della parte catturati PRIMA della finestra modale:
    // durante la scelta del file la selezione (o un aggiornamento) puo' cambiare
    // curRaw_, e l'indice si applicherebbe ai byte di un altro messaggio.
    const QByteArray raw = curRaw_;
    const int part = a.partIndex;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) dir = QDir::homePath();
    const QString proposed = dir + QLatin1Char('/') + MessageParser::sanitizeFilename(a.filename);
    // La conferma di sovrascrittura di QFileDialog e' disattivata: saveAttachment non
    // sovrascrive mai e un file esistente produce un errore chiaro.
    const QString dest = saveFile_ ? saveFile_(proposed)
                                   : QFileDialog::getSaveFileName(this, tr("Salva allegato"), proposed, QString(), nullptr,
                                                                  QFileDialog::DontConfirmOverwrite);
    if (dest.isEmpty()) return;
    QPointer<MainWindow> self(this);
    // scrittura (fino a 64 MiB, fsync) fuori dal thread GUI
    offThread<QString>(
        this,
        [raw, part, dest]() {
            QString err;
            if (!MessageParser::saveAttachment(raw, part, dest, &err) && err.isEmpty())
                err = QObject::tr("salvataggio non riuscito");
            return err;
        },
        [self, dest](QString err) {
            if (!err.isEmpty()) {
                self->showNotice(tr("Allegato non salvato"),
                                 tr("L'allegato non e' stato salvato in %1: %2.\nNessun file esistente e' stato modificato.")
                                     .arg(dest, err));
                return;
            }
            self->statusBar()->showMessage(tr("Allegato salvato: %1").arg(dest), 10000);
        });
}

void MainWindow::openLink(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || !(scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
                            scheme == QLatin1String("mailto"))) {
        showNotice(tr("Link non aperto"),
                   tr("Boma apre solo link http, https e mailto. Destinazione: %1")
                       .arg(QString::fromLatin1(url.toEncoded().left(500))));
        return;
    }
    // Destinazione reale, in ASCII: un dominio internazionale appare in punycode
    // (xn--...), cosi' un dominio che imita un altro con lettere simili si vede.
    const QString target = QString::fromLatin1(url.toEncoded(QUrl::FullyEncoded).left(2000));
    QString text = tr("Aprire questo indirizzo?\n\n%1").arg(target);
    if (scheme != QLatin1String("mailto")) {
        const QString host = QString::fromLatin1(QUrl::toAce(url.host()));
        if (!host.isEmpty()) text += tr("\n\nSito: %1").arg(host);
    }
    bool yes;
    if (confirmLink_) {
        yes = confirmLink_(url, text);
    } else {
        QMessageBox box(QMessageBox::Question, tr("Apri link"), text, QMessageBox::Yes | QMessageBox::No, this);
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::No);
        yes = box.exec() == QMessageBox::Yes;
    }
    if (!yes) return;
    if (openUrl_) openUrl_(url);
    else QDesktopServices::openUrl(url);
}

// ---- azioni ----

void MainWindow::deleteSelected() {
    if (!haveMsg_ || deleting_) return; // un solo spostamento alla volta
    if (!muRootDetected_ && !config_.isEmpty()) {
        statusBar()->showMessage(tr("Cancella disponibile dopo il rilevamento della radice mu"), 8000);
        return;
    }
    if (!muRootReliable_) {
        showNotice(tr("Elimina"),
                   tr("La radice Maildir di mu non e' stata determinata in modo affidabile (%1): per sicurezza nulla viene spostato.\n\n"
                      "Imposta muRoot= nella sezione [general] della configurazione di Boma "
                      "(o correggi `mu info store`).").arg(muRootError_.isEmpty() ? tr("errore sconosciuto") : muRootError_));
        return;
    }
    const MessageSummary s = curSummary_;
    const Account acc = accountFor(s.accountId);
    if (acc.id.isEmpty()) {
        showNotice(tr("Elimina"), tr("Account del messaggio non riconosciuto: non viene spostato nulla."));
        return;
    }
    deleting_ = true;
    delBtn_->setEnabled(false);
    QPointer<MainWindow> self(this);
    actions_->moveToTrash(s.path, acc, [self, s](bool ok, QString err) {
        if (!self) return;
        self->deleting_ = false;
        self->delBtn_->setEnabled(self->haveMsg_);
        if (!ok) {
            self->showNotice(tr("Elimina"), tr("Impossibile spostare nel cestino: %1").arg(err));
            return;
        }
        const int row = self->rowForPath(s.path);
        if (row >= 0) {
            // Se l'utente ha scelto un'altra riga durante lo spostamento, quella resta.
            const bool wasCurrent = self->list_->currentIndex().isValid() && self->list_->currentIndex().row() == row;
            if (wasCurrent) {
                // la riga vicina NON deve aprirsi (e risultare letta) da sola
                {
                    QSignalBlocker b(self->list_->selectionModel());
                    self->model_->removeAt(row);
                    self->list_->selectionModel()->clear();
                }
                self->clearViewer();
            } else {
                self->model_->removeAt(row);
            }
        }
        self->statusBar()->showMessage(tr("Spostato nel cestino"), 5000);
    });
}

void MainWindow::syncNow() {
    // La sincronizzazione periodica e' del cron; qui solo "Aggiorna ora":
    // mbsync e poi mu index (reindexAndRefresh), uno alla volta.
    if (config_.isEmpty() || sync_->state() == SyncService::Running || reindexing_) return;
    statusBar()->clearMessage();
    sync_->syncAll();
}

void MainWindow::onSyncState(SyncService::State st, const QString&, const QString& message) {
    switch (st) {
    case SyncService::Running:
        syncBtn_->setEnabled(false);
        syncLabel_->setText(tr("Aggiornamento in corso..."));
        progress_->setRange(0, 0);
        progress_->show();
        break;
    case SyncService::Idle:
        progress_->hide();
        lastSync_ = QDateTime::currentDateTime();
        syncLabel_->setText(tr("Aggiornato alle %1").arg(lastSync_.toString(QStringLiteral("HH:mm"))));
        syncLabel_->setToolTip(QString());
        runReindex();
        break;
    case SyncService::Failed:
        progress_->hide();
        // un account fallito non deve nascondere la posta scaricata dagli altri
        if (sync_->anySynced()) runReindex();
        else syncBtn_->setEnabled(true);
        if (sync_->busyElsewhere())
            syncLabel_->setText(tr("Sincronizzazione gia' in corso altrove (cron): riprova tra poco"));
        else
            syncLabel_->setText(tr("Aggiornamento non riuscito%1%2")
                                    .arg(lastSync_.isValid() ? tr(" (ultimo %1)").arg(lastSync_.toString(QStringLiteral("HH:mm")))
                                                             : QString(),
                                         message.isEmpty() ? QString() : QStringLiteral(": ") + message.left(160)));
        syncLabel_->setToolTip(message);
        break;
    }
}

void MainWindow::runReindex() {
    reindexing_ = true;
    syncBtn_->setEnabled(false);
    QPointer<MainWindow> self(this);
    reindexAndRefresh([self]() {
        if (!self) return;
        self->reindexing_ = false;
        self->syncBtn_->setEnabled(true);
    });
}

// ---- scrittura e invio ----

void MainWindow::newMessage() {
    if (config_.isEmpty()) return;
    openCompose(nullptr);
}

void MainWindow::compose(ReplyMode mode) {
    if (!haveMsg_) return;
    const OutgoingMessage m = buildReply(curParsed_, curSummary_.accountId, config_, mode);
    openCompose(&m);
}

ComposeWindow* MainWindow::openCompose(const OutgoingMessage* prefill) {
    auto* c = new ComposeWindow(config_, this);
    c->setAttribute(Qt::WA_DeleteOnClose);
    if (prefill) c->setPrefill(*prefill);
    QPointer<ComposeWindow> pc(c);
    connect(c, &ComposeWindow::sendRequested, this,
            [this, pc](const OutgoingMessage& msg, bool sign, bool encrypt) { startSend(pc, msg, sign, encrypt, false); });
    c->show();
    return c;
}

void MainWindow::startSend(QPointer<ComposeWindow> c, const OutgoingMessage& msg, bool sign, bool encrypt, bool allowUnsigned) {
    if (!c || c->property("bomaSending").toBool() || c->property("bomaSent").toBool()) return; // mai un secondo invio
    c->setProperty("bomaSending", true);
    c->sendButton()->setEnabled(false);
    c->statusLabel()->setText(tr("Invio in corso..."));
    statusBar()->showMessage(tr("Invio in corso..."));
    QPointer<MainWindow> self(this);
    sender_->send(
        msg, sign, encrypt,
        [self, c, msg, sign, encrypt](SendResult r) {
            if (!self) return;
            self->onSendResult(c, msg, sign, encrypt, r);
        },
        allowUnsigned);
}

void MainWindow::onSendResult(QPointer<ComposeWindow> c, const OutgoingMessage& msg, bool sign, bool encrypt, const SendResult& r) {
    const bool keepLocked = r.kind == SendResult::NeedsConfirmationUnsigned;
    // Se la finestra di scrittura e' stata chiusa durante l'invio il messaggio non e'
    // piu' "nella finestra": il testo dell'avviso lo dice.
    const QString closedNote = tr("\n\nLa finestra di scrittura e' stata chiusa durante l'invio: il messaggio non e' piu' disponibile qui.");
    if (c && !keepLocked) {
        c->setProperty("bomaSending", false);
        if (r.kind != SendResult::Sent) {
            c->sendButton()->setEnabled(true);
            c->statusLabel()->clear();
        }
    }
    switch (r.kind) {
    case SendResult::Sent:
        statusBar()->showMessage(r.message.isEmpty() ? tr("Inviato") : tr("Inviato (avviso: %1)").arg(r.message), 15000);
        if (!r.message.isEmpty()) showNotice(tr("Inviato, con un avviso"), r.message);
        if (c && r.copyFailed) {
            // I4: il messaggio E' partito ma la copia in Inviata manca: la finestra resta
            // aperta (il testo non va perso) ma segnata come inviata, mai reinviabile.
            c->setProperty("bomaSent", true);
            c->sendButton()->setEnabled(false);
            c->statusLabel()->setText(r.fallbackPath.isEmpty()
                                          ? tr("INVIATO. Copia in Inviata non salvata: copia il testo prima di chiudere.")
                                          : tr("INVIATO. Copia in Inviata non salvata; copia di emergenza: %1").arg(r.fallbackPath));
            // Se anche la copia di emergenza e' fallita il testo esiste solo qui: la
            // finestra resta "modificata", cosi' chiuderla chiede conferma.
            if (!r.fallbackPath.isEmpty()) c->markClean();
        } else if (c) {
            c->markClean();
            c->close();
        }
        break;
    case SendResult::NeedsConfirmationUnsigned:
        statusBar()->clearMessage();
        // Il Sender resta "occupato" fino al ritorno di questo callback: domanda ed
        // eventuale nuovo invio (allowUnsigned) partono subito dopo, dal ciclo eventi.
        // La finestra di scrittura resta bloccata (nessun invio doppio) fino alla risposta.
        QMetaObject::invokeMethod(
            this,
            [this, c, msg, sign, encrypt]() {
                const bool yes = confirmUnsigned_
                                     ? confirmUnsigned_()
                                     : QMessageBox::question(this, tr("Firma non disponibile"),
                                                             tr("L'identita' scelta non ha una chiave PGP.\n\nInviare SENZA firma?"),
                                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
                if (c) c->setProperty("bomaSending", false);
                if (yes) {
                    startSend(c, msg, sign, encrypt, true);
                } else {
                    if (c) {
                        c->sendButton()->setEnabled(true);
                        c->statusLabel()->clear();
                    }
                    statusBar()->showMessage(tr("Invio annullato: nulla e' stato inviato"), 8000);
                }
            },
            Qt::QueuedConnection);
        break;
    case SendResult::MissingKeys:
        statusBar()->clearMessage();
        showNotice(tr("Chiavi mancanti"),
                   tr("Nulla e' stato inviato: manca una chiave pubblica utilizzabile per:\n\n%1").arg(r.missing.join(QLatin1Char('\n'))) +
                       (c ? QString() : closedNote));
        break;
    case SendResult::SmtpFailed:
        statusBar()->clearMessage();
        showNotice(tr("Invio non riuscito"),
                   r.message + (c ? tr("\n\nIl messaggio resta nella finestra di scrittura e non viene ripetuto automaticamente. "
                                       "Se l'esito e' incerto controlla la cartella Inviata prima di riprovare.")
                                  : tr("\n\nNon viene ripetuto automaticamente. Se l'esito e' incerto controlla la cartella Inviata.") + closedNote));
        break;
    case SendResult::SignFailed:
    case SendResult::EncryptFailed:
    case SendResult::BuildFailed:
    case SendResult::Busy:
        statusBar()->clearMessage();
        showNotice(r.kind == SendResult::Busy ? tr("Invio in corso") : tr("Invio non riuscito"),
                   r.kind == SendResult::Busy ? tr("Un altro invio e' in corso: riprova tra poco. Nulla e' stato inviato.")
                                              : r.message + (c ? tr("\n\nNulla e' stato inviato; il messaggio resta nella finestra di scrittura.")
                                                             : tr("\n\nNulla e' stato inviato.") + closedNote));
        break;
    }
}

} // namespace boma

namespace boma {

// Le cartelle di mu sono relative alla sua radice Maildir, non a quella di un
// account: priorita' opzione > [general] muRoot > `mu info store` > (con avviso)
// il genitore comune delle Maildir degli account.
void MainWindow::resolveMuRoot()
{
    if (config_.isEmpty()) return;
    if (!opts_.muRoot.isEmpty()) return applyMuRoot(opts_.muRoot, QString());
    if (!config_.muRoot().isEmpty()) return applyMuRoot(config_.muRoot(), QString());
    QPointer<MainWindow> self(this);
    index_->storeRoot([self](QString root, QString err) {
        if (!self) return;
        if (!root.isEmpty()) return self->applyMuRoot(root, QString());
        self->applyMuRoot(QString(), err);
    });
}

void MainWindow::applyMuRoot(QString root, const QString& error)
{
    QString warning;
    if (root.isEmpty()) {
        QStringList common;
        bool first = true;
        for (const Account& a : config_.accounts()) {
            QStringList parts = QDir::cleanPath(a.maildirRoot).split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (!parts.isEmpty()) parts.removeLast(); // genitore della Maildir
            if (first) {
                common = parts;
                first = false;
                continue;
            }
            int n = 0;
            while (n < common.size() && n < parts.size() && common[n] == parts[n]) ++n;
            common = common.mid(0, n);
        }
        root = QLatin1Char('/') + common.join(QLatin1Char('/'));
        warning = tr("Radice Maildir di mu non determinata (%1): uso %2. Impostare [general] muRoot nella configurazione.")
                      .arg(error, root);
    }
    muRoot_ = root;
    muRootReliable_ = warning.isEmpty();
    muRootError_ = error;
    muRootDetected_ = true;
    // Elimina solo con radice affidabile; l'euristica serve alle sole viste delle cartelle
    actions_->setMuRoot(muRootReliable_ ? root : QString());
    folders_->setMuRoot(root);
    // "Non lette" esclude Cestino/Bozze/Inviata (cartelle relative alla radice di mu).
    // Con una radice dedotta male le esclusioni non trovano nulla: nessun danno.
    const bool changed = index_->muRoot() != root;
    index_->setMuRoot(root);
    if (changed && model_->query().isEmpty()) refreshList();
    if (!warning.isEmpty()) statusBar()->showMessage(warning);
}

} // namespace boma
