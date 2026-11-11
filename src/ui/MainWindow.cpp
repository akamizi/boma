#include "ui/MainWindow.h"

#include "ui/ReplyBuilder.h"

#include <QCoreApplication>
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

QString defaultLockPath() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (d.isEmpty()) d = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return d + QStringLiteral("/boma-sync.lock");
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
    sync_ = new SyncService(runner_, config_, opts_.lockPath.isEmpty() ? defaultLockPath() : opts_.lockPath, this);
    sync_->setMbsyncProgram(opts_.mbsyncProgram);
    sender_ = new Sender(this);
    sender_->setAccounts(config_.accounts());
    sender_->setMsmtpProgram(opts_.msmtpProgram);
    sender_->setMuProgram(opts_.muProgram);
    sender_->setMuHome(opts_.muHome);
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
    syncBtn_ = new QPushButton(tr("Sincronizza"), central);
    syncBtn_->setToolTip(tr("Sincronizza ora (F5)"));
    searchRow->addWidget(search_, 1);
    searchRow->addWidget(syncBtn_);
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
    firstBtn_ = new QPushButton(tr("Prima sincronizzazione..."), this);
    firstBtn_->setFlat(true);
    firstBtn_->setToolTip(tr("Mostra l'avanzamento della prima sincronizzazione"));
    firstBtn_->hide();
    statusBar()->addPermanentWidget(firstBtn_);
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
    connect(newBtn_, &QPushButton::clicked, this, &MainWindow::newMessage);
    connect(replyBtn_, &QPushButton::clicked, this, &MainWindow::reply);
    connect(replyAllBtn_, &QPushButton::clicked, this, &MainWindow::replyAll);
    connect(fwdBtn_, &QPushButton::clicked, this, &MainWindow::forward);
    connect(delBtn_, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(syncBtn_, &QPushButton::clicked, this, &MainWindow::syncNow);
    connect(sync_, &SyncService::stateChanged, this, &MainWindow::onSyncState);
    connect(firstBtn_, &QPushButton::clicked, this, &MainWindow::showFirstRunDialog);
    connect(sync_, &SyncService::accountStarted, this, [this](const QString& id) {
        if (!firstStates_.contains(id)) return;
        FirstRunState st;
        st.status = FirstRunState::Running;
        setFirstState(id, st);
        syncLabel_->setText(tr("Prima sincronizzazione di %1...").arg(accountFor(id).name));
    });
    connect(sync_, &SyncService::accountFinished, this, [this](const QString& id, bool ok, const QString& msg) {
        if (!firstStates_.contains(id)) return;
        FirstRunState st;
        st.status = ok ? FirstRunState::Done : FirstRunState::Failed;
        st.error = msg;
        setFirstState(id, st);
        if (ok) sync_->setNoTimeout(id, false); // finita la prima sync, valgono i tempi normali
    });
    connect(sync_, &SyncService::progress, this, [this](const QString& id, int done, int total) {
        if (firstStates_.contains(id)) {
            // contatori di mbsync (cartelle/canali), non dati: solo testo "N/M", barra occupata
            FirstRunState st = firstStates_.value(id);
            st.status = FirstRunState::Running;
            st.done = done;
            st.total = total;
            setFirstState(id, st);
            syncLabel_->setText(tr("Prima sincronizzazione di %1: %2/%3").arg(accountFor(id).name).arg(done).arg(total));
            return;
        }
        progress_->setRange(0, total > 0 ? total : 0);
        progress_->setValue(done);
    });
    // All'uscita il mbsync in corso e' fermato con SIGTERM (mai lasciato orfano);
    // il contesto `this` scollega la callback se la finestra sparisce prima.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { sync_->shutdown(); });
    connect(sender_, &Sender::reindexFinished, this, [this](bool ok, const QString& err) {
        if (!ok)
            statusBar()->showMessage(tr("Attenzione: aggiornamento dell'indice dopo l'invio non riuscito: %1").arg(err));
    });

    // scorciatoie: i tasti semplici non scattano mentre si scrive nel campo di ricerca
    auto sc = [this](const QKeySequence& k, auto slot) {
        auto* s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, slot);
    };
    sc(QKeySequence(QKeySequence::Find), &MainWindow::focusSearch);
    sc(QKeySequence(Qt::Key_Slash), &MainWindow::focusSearch);
    sc(QKeySequence(Qt::Key_Delete), &MainWindow::deleteSelected);
    sc(QKeySequence(Qt::Key_R), &MainWindow::reply);
    sc(QKeySequence(Qt::SHIFT | Qt::Key_R), &MainWindow::replyAll);
    sc(QKeySequence(Qt::Key_F), &MainWindow::forward);
    sc(QKeySequence(QKeySequence::New), &MainWindow::newMessage);
    sc(QKeySequence(Qt::Key_F5), &MainWindow::syncNow);

    // ---- avvio: nessun lavoro sincrono ----
    resolveMuRoot();
    if (config_.isEmpty()) {
        help_->setHtml(tr("<h2>Benvenuto in Boma</h2><p>Nessun account configurato.</p>"
                          "<p>Crea il file <b>%1</b> con almeno un account e una identit&agrave;, ad esempio:</p>"
                          "<pre>[account:personale]\nname=Personale\nmaildirRoot=~/Mail/personale\n"
                          "mbsyncChannel=personale\nsentFolder=Sent\ndraftsFolder=Drafts\ntrashFolder=Trash\n\n"
                          "[identity:principale]\naccountId=personale\naddress=io@example.org\n"
                          "displayName=Il mio nome\nmsmtpAccount=personale</pre>"
                          "<p>Le password non vanno nel file: le gestiscono mbsync (PassCmd) e msmtp (passwordeval).</p>")
                          .arg(Config::defaultPath().toHtmlEscaped()));
        stack_->setCurrentWidget(help_);
        syncLabel_->setText(tr("Nessun account configurato"));
        search_->setEnabled(false);
        syncBtn_->setEnabled(false);
        newBtn_->setEnabled(false);
    } else {
        stack_->setCurrentWidget(view_);
        applyQuery(QString());
        if (opts_.autoSync) {
            startupPending_ = true;
            QTimer::singleShot(0, this, &MainWindow::startupSync);
        }
    }
}

MainWindow::~MainWindow() {
    // Prima di distruggere i figli (ProcessRunner ucciderebbe con SIGKILL):
    // il mbsync in corso riceve SIGTERM, resta sicuro e riprende al prossimo avvio.
    if (sync_) sync_->shutdown();
}

// ---- prima sincronizzazione ----

void MainWindow::startupSync() {
    if (config_.isEmpty()) return;
    // Controllo asincrono ed economico: per ogni account si cerca UN file di messaggio e
    // ci si ferma al primo trovato (mai una visita dell'intera Maildir da 15 GB).
    const QList<Account> accounts = config_.accounts();
    offThread<QStringList>(
        this,
        [accounts]() {
            QStringList ids;
            for (const Account& a : accounts)
                if (SyncService::isFirstSync(a.maildirRoot)) ids << a.id;
            return ids;
        },
        [this](QStringList ids) { beginStartupQueue(ids); });
}

void MainWindow::beginStartupQueue(const QStringList& firstIds) {
    startupPending_ = false;
    if (firstIds.isEmpty()) {
        sync_->start();
        syncNow();
        return;
    }
    firstIds_ = firstIds;
    firstStates_.clear();
    for (const QString& id : firstIds) firstStates_.insert(id, FirstRunState());
    queue_ = firstIds;
    for (const Account& a : config_.accounts())
        if (!firstIds.contains(a.id)) queue_ << a.id; // gli altri, dopo, senza dialogo
    queueActive_ = true;
    // nessun tempo massimo per i sync di avvio (15 GB): mbsync e' interrompibile
    for (const QString& id : std::as_const(queue_)) sync_->setNoTimeout(id, true);
    ensureFirstRunDialog()->show();
    updateFirstRunUi();
    runNextQueued();
}

// Un account alla volta tramite SyncService (che serializza e prende il flock);
// un errore su un account non ferma gli altri, ne' si riprova da soli.
void MainWindow::runNextQueued() {
    if (queue_.isEmpty()) {
        queueActive_ = false;
        queueCur_.clear();
        sync_->start(); // anche dopo errori: il tick riprova e aggiorna lo stato (un tentativo per intervallo)
        updateFirstRunUi();
        return;
    }
    queueCur_ = queue_.takeFirst();
    sync_->syncAccount(queueCur_);
}

void MainWindow::retryFirstRun(const QString& id) {
    // Mai due sincronizzazioni insieme: se ne e' in corso una (o la coda iniziale) non si fa nulla.
    if (!firstStates_.contains(id) || queueActive_ || sync_->state() == SyncService::Running) return;
    queueCur_ = id;
    sync_->setNoTimeout(id, true);
    sync_->syncAccount(id);
}

void MainWindow::setFirstState(const QString& id, const FirstRunState& st) {
    firstStates_[id] = st;
    if (firstRun_) firstRun_->setState(id, st);
    updateFirstRunUi();
}

void MainWindow::updateFirstRunUi() {
    bool allDone = !firstStates_.isEmpty();
    for (const FirstRunState& s : std::as_const(firstStates_))
        allDone = allDone && s.status == FirstRunState::Done;
    firstBtn_->setVisible(!firstStates_.isEmpty() && !allDone);
    if (firstRun_) firstRun_->setRetryAllowed(!queueActive_ && sync_->state() != SyncService::Running);
}

FirstRunDialog* MainWindow::ensureFirstRunDialog() {
    if (!firstRun_) {
        auto* d = new FirstRunDialog(this);
        QList<QPair<QString, QString>> rows;
        for (const QString& id : std::as_const(firstIds_)) rows.append({id, accountFor(id).name});
        d->setAccounts(rows);
        for (auto it = firstStates_.cbegin(); it != firstStates_.cend(); ++it) d->setState(it.key(), it.value());
        connect(d, &FirstRunDialog::retryRequested, this, &MainWindow::retryFirstRun);
        firstRun_ = d;
        updateFirstRunUi();
    }
    return firstRun_;
}

void MainWindow::showFirstRunDialog() {
    if (firstStates_.isEmpty()) return;
    FirstRunDialog* d = ensureFirstRunDialog();
    d->show();
    d->raise();
    d->activateWindow();
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
            const QByteArray raw = f.read(kMaxMessageBytes + 1);
            l.pm = MessageParser::parse(raw, &l.error);
            return l;
        },
        [this, token, s](Loaded l) { onLoaded(token, s, l.pm, l.error); });
}

void MainWindow::onLoaded(quint64 token, const MessageSummary& s, const ParsedMessage& pm, const QString& error) {
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
        // mu sposta i file di new/ in cur/: ririsolve il percorso (sola lettura)
        QString newPath;
        if (s.path.contains(QStringLiteral("/new/"))) {
            const int slash = s.path.lastIndexOf(QLatin1Char('/'));
            const QString base = s.path.mid(slash + 1).section(QLatin1Char(':'), 0, 0);
            const QString curDir = s.path.left(s.path.lastIndexOf(QStringLiteral("/new/"))) + QStringLiteral("/cur");
            const QStringList found = QDir(curDir).entryList({base + QLatin1Char('*')}, QDir::Files);
            if (!found.isEmpty()) newPath = curDir + QLatin1Char('/') + found.first();
        }
        const int row = self->rowForPath(s.path);
        if (row >= 0) self->model_->markReadLocal(row, newPath);
        if (self->curSummary_.path == s.path) {
            self->curSummary_.unread = false;
            if (!newPath.isEmpty()) self->curSummary_.path = newPath;
        }
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
                    self->decrypted_ = true;
                    self->curParsed_ = inner;
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
    // durante il rilevamento iniziale e la coda di avvio la sincronizzazione e' gia' pilotata
    if (config_.isEmpty() || startupPending_ || queueActive_) return;
    statusBar()->clearMessage();
    sync_->syncAll();
}

void MainWindow::onSyncState(SyncService::State st, const QString& accountId, const QString& message) {
    bool reindexing = false;
    QPointer<MainWindow> selfp(this);
    const std::function<void()> cont = [selfp]() {
        if (selfp && selfp->queueActive_ && selfp->sync_->state() != SyncService::Running) selfp->runNextQueued();
    };
    switch (st) {
    case SyncService::Running:
        syncWasRunning_ = true;
        syncLabel_->setText(accountId.isEmpty() ? tr("Sincronizzazione in corso...")
                                                 : tr("Sincronizzazione in corso: %1").arg(accountId));
        progress_->setRange(0, 0);
        progress_->show();
        break;
    case SyncService::Idle:
        progress_->hide();
        if (opts_.autoSync && !queueActive_ && !sync_->timerActive()) sync_->start(); // dopo un errore iniziale, al primo successo
        lastSync_ = QDateTime::currentDateTime();
        syncLabel_->setText(tr("Sincronizzato alle %1").arg(lastSync_.toString(QStringLiteral("HH:mm"))));
        if (syncWasRunning_) {
            syncWasRunning_ = false;
            reindexing = true;
            reindexAndRefresh(queueActive_ ? cont : std::function<void()>());
        }
        break;
    case SyncService::Failed:
        // un account fallito non deve nascondere la posta scaricata dagli altri
        if (syncWasRunning_ && sync_->anySynced()) {
            reindexing = true;
            reindexAndRefresh(queueActive_ ? cont : std::function<void()>());
        }
        syncWasRunning_ = false;
        progress_->hide();
        // senza rete mbsync esce con errore: nessun messaggio ripetuto, solo lo stato
        syncLabel_->setText(tr("Offline o sincronizzazione non riuscita%1%2")
                                .arg(lastSync_.isValid() ? tr(" (ultima sincronizzazione %1)").arg(lastSync_.toString(QStringLiteral("HH:mm")))
                                                         : tr(" (mai sincronizzato)"),
                                     message.isEmpty() ? QString() : QStringLiteral(": ") + message.left(160)));
        syncLabel_->setToolTip(message);
        break;
    }
    if (st == SyncService::Failed) {
        // errore senza account (lock occupato, ...): va all'account che si stava avviando
        if (accountId.isEmpty() && firstStates_.contains(queueCur_)) {
            FirstRunState fs;
            fs.status = FirstRunState::Failed;
            fs.error = message;
            setFirstState(queueCur_, fs);
        }
    }
    if (st != SyncService::Running && queueActive_) {
        if (reindexing) {
            // il prossimo mbsync parte solo a mu index finito (niente sovrapposizioni): vedi cont
        } else {
            runNextQueued();
        }
    }
    updateFirstRunUi();
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
    if (!c || c->property("bomaSending").toBool()) return; // mai un secondo invio
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
        if (c) {
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
    if (!warning.isEmpty()) statusBar()->showMessage(warning);
}

} // namespace boma
