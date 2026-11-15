#include <QtTest>
#include "core/SendController.h"
#include "send/Sender.h"
#include "core/MaildirWriter.h"
using namespace boma;

namespace {

// Una "finestra di scrittura" finta: registra che cosa il controller le fa.
struct FakeWin : QObject {
    bool enabled = true, clean = false, closed = false;
    int closes = 0, cleans = 0, discards = 0;
    QString status;
    SendController::Surface surface() {
        SendController::Surface s;
        s.window = this;
        s.setSendEnabled = [this](bool on) { enabled = on; };
        s.setStatus = [this](const QString& t) { status = t; };
        s.markClean = [this]() { clean = true; ++cleans; };
        s.close = [this]() { closed = true; ++closes; };
        s.discardDraft = [this]() { ++discards; };
        return s;
    }
};

OutgoingMessage someMessage() {
    OutgoingMessage m;
    m.to = {QStringLiteral("bob@example.test")};
    m.subject = QStringLiteral("Prova");
    m.plainText = QStringLiteral("testo");
    return m;
}

QString writeScript(const QDir& dir, const QString& name, const QByteArray& body) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("script non scrivibile");
    f.write(body);
    f.close();
    f.setPermissions(QFile::permissions(path) | QFile::ExeOwner);
    return path;
}
QByteArray readFile(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

class T : public QObject {
    Q_OBJECT

    // --- parte 1: Sender finto, esiti scelti dal test ---
    SendController* c_ = nullptr;
    int sendCalls_ = 0;
    bool lastAllowUnsigned_ = false;
    SendResult next_;
    bool hold_ = false;                      // true: il callback si chiama a mano (finish())
    std::function<void(SendResult)> held_;
    QList<QPair<QString, int>> statuses_;
    QList<QPair<QString, QString>> notices_;
    int asked_ = 0;
    bool answer_ = false;

    void finish(const SendResult& r) { auto d = held_; held_ = nullptr; QTimer::singleShot(0, [d, r] { d(r); }); }
    static SendResult res(SendResult::Kind k, const QString& msg = {}) { SendResult r; r.kind = k; r.message = msg; return r; }

    // --- parte 2: Sender vero con msmtp/mu finti (script) ---
    QTemporaryDir root_{QDir::currentPath() + QStringLiteral("/sndc-XXXXXX")};
    QString dir_, msmtp_, mu_;
    QList<Account> accounts_;
    Identity id_, idNoKey_;
    int n_ = 0;
    std::unique_ptr<Sender> sender_;
    std::unique_ptr<SendController> real_;
    QStringList sentFiles() { return QDir(dir_ + "/mail/a/Sent/cur").entryList(QDir::Files); }
    int msmtpRuns() { return readFile(dir_ + "/count").count("run"); }
    void newRealEnv(const QString& msmtpProgram = {}) {
        dir_ = root_.filePath(QStringLiteral("e%1").arg(++n_));
        QDir().mkpath(dir_);
        for (const char* s : {"cur", "new", "tmp"}) QDir().mkpath(dir_ + "/mail/a/Sent/" + s);
        QDir d(dir_);
        msmtp_ = writeScript(d, "msmtp",
                             "#!/bin/sh\nd=$(dirname \"$0\")\necho run >> \"$d/count\"\ncat > \"$d/stdin\"\n"
                             "[ -f \"$d/slow\" ] && sleep 1\n"
                             "[ -f \"$d/fail\" ] && { echo 'msmtp: authentication failed' >&2; exit 1; }\nexit 0\n");
        mu_ = writeScript(d, "mu", "#!/bin/sh\nexit 0\n");
        accounts_ = {Account{"a", "A", dir_ + "/mail/a", "a", "Sent", "Drafts", "Trash"}};
        sender_ = std::make_unique<Sender>();
        sender_->setAccounts(accounts_);
        sender_->setMsmtpProgram(msmtpProgram.isEmpty() ? msmtp_ : msmtpProgram);
        sender_->setMuProgram(mu_);
        sender_->setMuHome(dir_ + "/muhome");
        sender_->setUnsavedDir(dir_ + "/non-salvati");
        real_ = std::make_unique<SendController>([this](const OutgoingMessage& m, bool sg, bool en, std::function<void(SendResult)> done, bool au) {
            sender_->send(m, sg, en, std::move(done), au);
        });
        connect(real_.get(), &SendController::statusMessage, this, [this](const QString& t, int ms) { statuses_ << qMakePair(t, ms); });
        connect(real_.get(), &SendController::notice, this, [this](const QString& t, const QString& x) { notices_ << qMakePair(t, x); });
        real_->setConfirmUnsigned([this] { ++asked_; return answer_; });
    }
    OutgoingMessage realMsg(bool withKey = true) {
        OutgoingMessage m = someMessage();
        m.identity = withKey ? id_ : idNoKey_;
        return m;
    }

private slots:
    void initTestCase() {
        QVERIFY2(root_.isValid(), "directory temporanea non creata");
        auto id = [](const char* i, const char* key) {
            Identity x;
            x.id = QLatin1String(i); x.accountId = QStringLiteral("a"); x.address = QStringLiteral("alice@example.test");
            x.displayName = QStringLiteral("Alice"); x.pgpKeyId = QLatin1String(key); x.msmtpAccount = QStringLiteral("acc-a");
            return x;
        };
        id_ = id("i1", "alice@example.test");
        idNoKey_ = id("i2", "");
    }
    void init() {
        c_ = new SendController([this](const OutgoingMessage&, bool, bool, std::function<void(SendResult)> done, bool au) {
            ++sendCalls_; lastAllowUnsigned_ = au;
            if (hold_) held_ = std::move(done);
            else { const SendResult r = au ? SendResult(res(SendResult::Sent)) : next_; QTimer::singleShot(0, [done, r] { done(r); }); }
        }, this);
        sendCalls_ = 0; asked_ = 0; answer_ = false; hold_ = false; held_ = nullptr; lastAllowUnsigned_ = false;
        next_ = res(SendResult::Sent);
        statuses_.clear(); notices_.clear();
        connect(c_, &SendController::statusMessage, this, [this](const QString& t, int ms) { statuses_ << qMakePair(t, ms); });
        connect(c_, &SendController::notice, this, [this](const QString& t, const QString& x) { notices_ << qMakePair(t, x); });
        c_->setConfirmUnsigned([this] { ++asked_; return answer_; });
    }
    void cleanup() { delete c_; c_ = nullptr; real_.reset(); sender_.reset(); }

    // ================= logica del controller (Sender finto) =================

    void testSuccessLocksThenClosesCleanWindow() {
        FakeWin w; hold_ = true;
        c_->start(w.surface(), someMessage(), false, false);
        QVERIFY(c_->isSending(&w));
        QVERIFY(!w.enabled);                                  // bloccata durante l'invio
        QVERIFY(w.status.contains("Invio in corso"));
        QCOMPARE(statuses_.last().first, QString("Invio in corso..."));
        finish(res(SendResult::Sent));
        QTRY_VERIFY(w.closed);
        QVERIFY(w.clean);                                     // chiusa senza chiedere conferma
        QCOMPARE(statuses_.last().first, QString("Inviato"));
        QVERIFY(notices_.isEmpty());
    }
    // Bozza di partenza: tolta solo a invio riuscito con la copia in Inviata scritta.
    void testDraftDiscardedOnlyAfterSentCopyWritten() {
        FakeWin w;
        c_->start(w.surface(), someMessage(), false, false);
        QTRY_VERIFY(w.closed);
        QCOMPARE(w.discards, 1);
    }
    void testDraftKeptWhenSentCopyFailed() {
        for (const QString fb : {QString("/x/emergenza.eml"), QString()}) {
            FakeWin w; next_ = res(SendResult::Sent, "copia"); next_.copyFailed = true; next_.fallbackPath = fb;
            c_->start(w.surface(), someMessage(), false, false);
            QTRY_VERIFY(c_->isSent(&w));
            QCOMPARE(w.discards, 0);
        }
    }
    void testDraftKeptOnEveryFailureAndOnCancel() {
        for (SendResult::Kind k : {SendResult::SmtpFailed, SendResult::SignFailed, SendResult::EncryptFailed,
                                   SendResult::BuildFailed, SendResult::Busy, SendResult::MissingKeys}) {
            FakeWin w; next_ = res(k, "x");
            const int before = notices_.size();
            c_->start(w.surface(), someMessage(), false, false);
            QTRY_VERIFY(notices_.size() > before);
            QCOMPARE(w.discards, 0);
        }
        FakeWin w; next_ = res(SendResult::NeedsConfirmationUnsigned); answer_ = false;
        c_->start(w.surface(), someMessage(), true, false);
        QTRY_COMPARE(asked_, 1);
        QTRY_VERIFY(w.enabled);
        QCOMPARE(w.discards, 0);
    }
    // Annullamento e poi invio confermato: la bozza si toglie una volta sola, alla fine.
    void testDraftDiscardedOnceAfterConfirmedUnsignedSend() {
        FakeWin w; answer_ = true; next_ = res(SendResult::NeedsConfirmationUnsigned);
        c_->start(w.surface(), someMessage(), true, false);
        QTRY_VERIFY(w.closed);
        QCOMPARE(w.discards, 1);
    }
    // Finestra chiusa durante l'invio riuscito: la bozza va tolta comunque.
    void testDraftDiscardedEvenIfWindowClosedDuringSend() {
        int discards = 0; hold_ = true;
        {
            auto* w = new FakeWin;
            SendController::Surface s = w->surface();
            s.discardDraft = [&discards] { ++discards; };
            c_->start(s, someMessage(), false, false);
            delete w;
        }
        finish(res(SendResult::Sent));
        QTRY_COMPARE(discards, 1);
    }
    void testSuccessWithWarningShowsNotice() {
        FakeWin w; next_ = res(SendResult::Sent, "avviso X");
        c_->start(w.surface(), someMessage(), false, false);
        QTRY_VERIFY(w.closed);
        QCOMPARE(notices_.size(), 1);
        QCOMPARE(notices_[0].second, QString("avviso X"));
        QVERIFY(statuses_.last().first.contains("avviso X"));
    }
    // Il messaggio e' partito ma la copia in Inviata manca: la finestra NON si chiude, non e' reinviabile.
    void testSentButCopyFailedKeepsWindowAndForbidsResend() {
        FakeWin w; next_ = res(SendResult::Sent, "copia non salvata"); next_.copyFailed = true; next_.fallbackPath = "/x/emergenza.eml";
        c_->start(w.surface(), someMessage(), false, false);
        QTRY_VERIFY(c_->isSent(&w));
        QVERIFY(!w.closed);
        QVERIFY(!w.enabled);
        QVERIFY(w.status.contains("INVIATO") && w.status.contains("/x/emergenza.eml"));
        QVERIFY(w.clean);                                     // la copia di emergenza esiste: si puo' chiudere
        c_->start(w.surface(), someMessage(), false, false);  // mai un secondo invio
        QTest::qWait(50);
        QCOMPARE(sendCalls_, 1);
    }
    // Copia di Inviata e di emergenza entrambe fallite: il testo esiste solo qui, la finestra resta "modificata".
    void testSentAndEmergencyCopyFailedStaysDirty() {
        FakeWin w; next_ = res(SendResult::Sent, "x"); next_.copyFailed = true;
        c_->start(w.surface(), someMessage(), false, false);
        QTRY_VERIFY(c_->isSent(&w));
        QVERIFY(!w.closed);
        QVERIFY(!w.clean);
    }
    // Ogni errore: la finestra resta aperta, sbloccata, con il testo; nessuna chiusura, nessun "pulito".
    void testEveryFailureKeepsWindowUsable_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("smtp") << int(SendResult::SmtpFailed);
        QTest::newRow("firma") << int(SendResult::SignFailed);
        QTest::newRow("cifratura") << int(SendResult::EncryptFailed);
        QTest::newRow("costruzione") << int(SendResult::BuildFailed);
        QTest::newRow("occupato") << int(SendResult::Busy);
        QTest::newRow("chiavi") << int(SendResult::MissingKeys);
    }
    void testEveryFailureKeepsWindowUsable() {
        QFETCH(int, kind);
        FakeWin w; next_ = res(SendResult::Kind(kind), "motivo"); next_.missing = {"carol@example.test"};
        c_->start(w.surface(), someMessage(), false, false);
        QTRY_VERIFY(!notices_.isEmpty());
        QVERIFY(w.enabled);
        QVERIFY(w.status.isEmpty());
        QVERIFY(!w.closed);
        QVERIFY(!w.clean);
        QVERIFY(!c_->isSending(&w));
        QVERIFY(!c_->isSent(&w));
        QVERIFY(statuses_.last().first.isEmpty());            // "Invio in corso..." tolto
        // si puo' riprovare
        next_ = res(SendResult::Sent);
        c_->start(w.surface(), someMessage(), false, false);
        QTRY_VERIFY(w.closed);
        QCOMPARE(sendCalls_, 2);
    }
    void testFailureNoticeSaysNothingWasRepeated() {
        FakeWin w; next_ = res(SendResult::SmtpFailed, "authentication failed");
        c_->start(w.surface(), someMessage(), false, false);
        QTRY_VERIFY(!notices_.isEmpty());
        QVERIFY(notices_[0].second.contains("authentication failed"));
        QVERIFY(notices_[0].second.contains("non viene ripetuto"));
        QVERIFY(notices_[0].second.contains("controlla la cartella Inviata"));
    }
    // Doppio invio: finche' il primo e' in corso il secondo e' ignorato (nessuna chiamata, nessun avviso).
    void testDoubleStartWhileSendingIsIgnored() {
        FakeWin w; hold_ = true;
        for (int i = 0; i < 3; ++i) c_->start(w.surface(), someMessage(), false, false);
        QCOMPARE(sendCalls_, 1);
        finish(res(SendResult::Sent));
        QTRY_VERIFY(w.closed);
        QCOMPARE(sendCalls_, 1);
        QVERIFY(notices_.isEmpty());
    }
    void testWindowsAreIndependent() {
        FakeWin a, b; hold_ = true;
        c_->start(a.surface(), someMessage(), false, false);
        c_->start(b.surface(), someMessage(), false, false);
        QCOMPARE(sendCalls_, 2); // ognuna ha il suo stato; l'unicita' tra finestre e' di Sender (Busy)
    }
    void testStartOnClosedWindowDoesNothing() {
        SendController::Surface s; // finestra nulla
        c_->start(s, someMessage(), false, false);
        QCOMPARE(sendCalls_, 0);
    }
    // Annullamento: la domanda sulla firma. No = nulla parte e la finestra e' di nuovo utilizzabile.
    void testUnsignedDeclinedCancelsEverything() {
        FakeWin w; next_ = res(SendResult::NeedsConfirmationUnsigned); answer_ = false;
        c_->start(w.surface(), someMessage(), true, false);
        QTRY_COMPARE(asked_, 1);
        QTRY_VERIFY(w.enabled);
        QVERIFY(w.status.isEmpty());
        QVERIFY(!w.closed);
        QCOMPARE(sendCalls_, 1);
        QCOMPARE(statuses_.last().first, QString("Invio annullato: nulla e' stato inviato"));
        QVERIFY(!c_->isSending(&w));
    }
    void testUnsignedConfirmedResendsWithAllowUnsignedOnce() {
        FakeWin w; answer_ = true; hold_ = false;
        next_ = res(SendResult::NeedsConfirmationUnsigned);
        c_->start(w.surface(), someMessage(), true, false);
        QTRY_COMPARE(asked_, 1);
        // la seconda chiamata (allowUnsigned) riesce
        QTRY_COMPARE(sendCalls_, 2);
        QVERIFY(lastAllowUnsigned_);
        QTRY_VERIFY(w.closed);
    }
    // Mentre la domanda e' aperta la finestra e' bloccata: un altro start non parte.
    void testWindowStaysLockedWhileAsking() {
        FakeWin w; next_ = res(SendResult::NeedsConfirmationUnsigned);
        c_->setConfirmUnsigned([&] {
            ++asked_;
            c_->start(w.surface(), someMessage(), true, false); // clic sul pulsante durante la domanda
            return false;
        });
        c_->start(w.surface(), someMessage(), true, false);
        QTRY_COMPARE(asked_, 1);
        QTest::qWait(50);
        QCOMPARE(sendCalls_, 1);
    }
    void testNoConfirmCallbackMeansNo() {
        FakeWin w; next_ = res(SendResult::NeedsConfirmationUnsigned);
        c_->setConfirmUnsigned(nullptr);
        c_->start(w.surface(), someMessage(), true, false);
        QTRY_VERIFY(w.enabled);
        QCOMPARE(sendCalls_, 1);
    }
    // La finestra chiusa durante l'invio: nessun accesso alla finestra; l'avviso lo dice.
    void testWindowClosedDuringFailedSendSaysSoInTheNotice() {
        auto* w = new FakeWin; hold_ = true;
        c_->start(w->surface(), someMessage(), false, false);
        delete w;
        finish(res(SendResult::SmtpFailed, "boom"));
        QTRY_VERIFY(!notices_.isEmpty());
        QVERIFY(notices_[0].second.contains("finestra di scrittura e' stata chiusa"));
        QVERIFY(notices_[0].second.contains("controlla la cartella Inviata"));
    }
    void testWindowClosedDuringSuccessIsFine() {
        auto* w = new FakeWin; hold_ = true;
        c_->start(w->surface(), someMessage(), false, false);
        delete w;
        finish(res(SendResult::Sent));
        QTRY_COMPARE(statuses_.last().first, QString("Inviato"));
    }
    void testControllerDestroyedDuringSendIsSafe() {
        FakeWin w; hold_ = true;
        c_->start(w.surface(), someMessage(), false, false);
        delete c_; c_ = nullptr;
        // il callback tardivo non deve toccare il controller distrutto
        auto d = held_; held_ = nullptr;
        d(res(SendResult::Sent));
        QVERIFY(!w.closed);
    }

    // ============ con il vero Sender e msmtp/mu finti: che cosa finisce in Inviata ============

    void testRealSuccessOneCopyInSentWindowClosed() {
        newRealEnv(); FakeWin w;
        real_->start(w.surface(), realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(w.closed, 20000);
        QCOMPARE(msmtpRuns(), 1);
        QCOMPARE(sentFiles().size(), 1);
        QVERIFY(w.clean);
        QVERIFY(notices_.isEmpty());
    }
    // msmtp esce con errore: nulla in Inviata, la finestra resta con il testo e si puo' riprovare.
    void testRealMsmtpErrorNoCopyAndNothingLost() {
        newRealEnv(); FakeWin w;
        QVERIFY(QFile(dir_ + "/fail").open(QIODevice::WriteOnly));
        real_->start(w.surface(), realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(!notices_.isEmpty(), 20000);
        QVERIFY(notices_[0].second.contains("authentication failed"));
        QVERIFY(sentFiles().isEmpty());                       // NON in Inviata
        QVERIFY(!w.closed && !w.clean);                       // il testo resta nella finestra
        QVERIFY(w.enabled);
        QCOMPARE(msmtpRuns(), 1);                             // e non e' stato ripetuto da solo
        QTest::qWait(300);
        QCOMPARE(msmtpRuns(), 1);
        // l'utente riprova dopo aver sistemato: parte e viene copiato UNA volta
        QFile::remove(dir_ + "/fail");
        QTRY_VERIFY_WITH_TIMEOUT(!sender_->busy(), 5000);
        real_->start(w.surface(), realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(w.closed, 20000);
        QCOMPARE(msmtpRuns(), 2);
        QCOMPARE(sentFiles().size(), 1);
    }
    // msmtp non esiste: stesso trattamento (nessuna copia, finestra intatta).
    void testRealMsmtpMissingNoCopyAndNothingLost() {
        newRealEnv(QStringLiteral("/nonexistent/msmtp")); FakeWin w;
        real_->start(w.surface(), realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(!notices_.isEmpty(), 20000);
        QVERIFY(sentFiles().isEmpty());
        QVERIFY(!w.closed && !w.clean);
        QVERIFY(w.enabled);
        QVERIFY(!real_->isSent(&w));
        QVERIFY(!notices_[0].second.isEmpty());
    }
    // Bozza di partenza: un file in Bozze, scritto come fa la finestra di scrittura.
    QString makeDraft() {
        for (const char* x : {"cur", "new", "tmp"}) QDir().mkpath(dir_ + "/mail/a/Drafts/" + x);
        QString err;
        const QString p = MaildirWriter::deliver(dir_ + "/mail/a/Drafts", "From: a@b.c\nSubject: Prova\n\ntesto\n", "D", &err);
        if (p.isEmpty()) qFatal("bozza non scritta: %s", qPrintable(err));
        return p;
    }
    int draftsCount() { return QDir(dir_ + "/mail/a/Drafts/cur").entryList(QDir::Files).size(); }
    void testRealSuccessRemovesDraftKeepsOneSentCopy() {
        newRealEnv(); FakeWin w;
        const QString draft = makeDraft();
        auto s = w.surface();
        s.discardDraft = [draft] { MaildirWriter::discard(draft, nullptr); };
        QCOMPARE(draftsCount(), 1);
        real_->start(s, realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(w.closed, 20000);
        QCOMPARE(draftsCount(), 0);                            // bozza sparita
        QCOMPARE(sentFiles().size(), 1);                       // una sola copia in Inviata
    }
    // msmtp ha spedito ma la scrittura in Inviata fallisce: la bozza resta (mai perdere posta).
    void testRealSentCopyFailureKeepsDraft() {
        newRealEnv(); FakeWin w;
        const QString draft = makeDraft();
        QFile b(dir_ + "/breaksent");
        QVERIFY(b.open(QIODevice::WriteOnly));
        b.write((dir_ + "/mail/a/Sent").toUtf8());
        b.close();
        // msmtp finto: dopo aver letto il messaggio distrugge Inviata
        writeScript(QDir(dir_), "msmtp",
                    "#!/bin/sh\nd=$(dirname \"$0\")\necho run >> \"$d/count\"\ncat > \"$d/stdin\"\nrm -rf \"$(cat \"$d/breaksent\")\"\nexit 0\n");
        sender_->setUnsavedDir(dir_ + "/non-salvati");
        auto s = w.surface();
        s.discardDraft = [draft] { MaildirWriter::discard(draft, nullptr); };
        real_->start(s, realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(real_->isSent(&w), 20000);
        QCOMPARE(msmtpRuns(), 1);
        QVERIFY(!QFileInfo::exists(dir_ + "/mail/a/Sent/cur"));
        QCOMPARE(draftsCount(), 1);                            // bozza intatta
        QVERIFY(QFileInfo::exists(draft));
        QVERIFY(!w.closed);
    }
    void testRealMsmtpErrorKeepsDraft() {
        newRealEnv(); FakeWin w;
        const QString draft = makeDraft();
        QVERIFY(QFile(dir_ + "/fail").open(QIODevice::WriteOnly));
        auto s = w.surface();
        s.discardDraft = [draft] { MaildirWriter::discard(draft, nullptr); };
        real_->start(s, realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(!notices_.isEmpty(), 20000);
        QCOMPARE(draftsCount(), 1);
        QVERIFY(sentFiles().isEmpty());
    }
    // Doppio invio (doppio clic): un solo msmtp, una sola copia.
    void testRealDoubleClickSendsOnce() {
        newRealEnv(); FakeWin w;
        QVERIFY(QFile(dir_ + "/slow").open(QIODevice::WriteOnly));
        real_->start(w.surface(), realMsg(), false, false);
        real_->start(w.surface(), realMsg(), false, false);
        real_->start(w.surface(), realMsg(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(w.closed, 20000);
        QTest::qWait(200);
        QCOMPARE(msmtpRuns(), 1);
        QCOMPARE(sentFiles().size(), 1);
        QVERIFY(notices_.isEmpty());
    }
    // Annullamento della firma mancante: msmtp non parte, nulla in Inviata.
    void testRealUnsignedDeclinedRunsNothing() {
        newRealEnv(); FakeWin w; answer_ = false;
        real_->start(w.surface(), realMsg(false), true, false);
        QTRY_COMPARE_WITH_TIMEOUT(asked_, 1, 20000);
        QTRY_VERIFY(w.enabled);
        QCOMPARE(msmtpRuns(), 0);
        QVERIFY(sentFiles().isEmpty());
        QVERIFY(!w.closed);
    }
    void testRealUnsignedConfirmedSendsOnce() {
        newRealEnv(); FakeWin w; answer_ = true;
        real_->start(w.surface(), realMsg(false), true, false);
        QTRY_VERIFY_WITH_TIMEOUT(w.closed, 20000);
        QCOMPARE(asked_, 1);
        QCOMPARE(msmtpRuns(), 1);
        QCOMPARE(sentFiles().size(), 1);
    }
};

QTEST_MAIN(T)
#include "test_sendcontroller.moc"
