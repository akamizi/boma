#include "core/MailIndex.h"
#include "ui/FirstRunDialog.h"
#include "ui/MainWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <csignal>
#include <sys/types.h>

using namespace boma;

namespace {

QString writeScript(const QString& dir, const QString& name, const QByteArray& body) {
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("script non scrivibile");
    f.write(body);
    f.close();
    f.setPermissions(QFile::permissions(path) | QFile::ExeOwner);
    return path;
}

void writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("file non scrivibile: %s", qPrintable(path));
    f.write(data);
}

QByteArray readFile(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class T : public QObject {
    Q_OBJECT
    // sotto la directory di lavoro: /tmp puo' essere montato noexec
    QTemporaryDir root_{QDir::currentPath() + QStringLiteral("/frt-XXXXXX")};
    QString dir_, mu_, mbsync_;

    QString f(const QString& n) const { return dir_ + QLatin1Char('/') + n; }
    QString mail(const QString& ch) const { return f(QStringLiteral("mail/") + ch); }
    bool has(const QString& n) const { return QFile::exists(f(n)); }
    void touch(const QString& n) { writeFile(f(n), "x"); }
    QStringList lines(const QString& n) const {
        return QString::fromUtf8(readFile(f(n))).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }
    int runs(const QString& ch) const { return int(lines(QStringLiteral("mbsync.log")).count(ch)); }
    int totalRuns() const { return int(lines(QStringLiteral("mbsync.log")).size()); }
    int files(const QString& ch) const {
        int n = 0;
        QDirIterator it(mail(ch), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { it.next(); ++n; }
        return n;
    }
    static bool alive(const QString& pidFile) {
        const int pid = QString::fromUtf8(readFile(pidFile)).trimmed().toInt();
        return pid > 0 && ::kill(pid_t(pid), 0) == 0;
    }
    bool pidAlive(const QString& ch) const { return alive(f(QStringLiteral("pid.") + ch)); }

    void resetEnv() {
        QDir(f(QStringLiteral("mail"))).removeRecursively();
        QDir(f(QStringLiteral("muhome"))).removeRecursively();
        const auto l = QDir(dir_).entryList({QStringLiteral("gate.*"), QStringLiteral("fail.*"), QStringLiteral("partial.*"),
                                             QStringLiteral("pid.*"), QStringLiteral("term.*")}, QDir::Files);
        for (const QString& n : l) QFile::remove(f(n));
        for (const char* n : {"mbsync.log", "mu.log", "seq.log", "overlap.log", "busy", "lock"}) QFile::remove(f(QLatin1String(n)));
    }
    void emptyFolders(const QString& ch) {
        for (const char* s : {"INBOX/cur", "INBOX/new", "INBOX/tmp", "Sent/cur", "Sent/new"})
            QDir().mkpath(mail(ch) + QLatin1Char('/') + QLatin1String(s));
        touch(QStringLiteral("mail/%1/INBOX/.uidvalidity").arg(ch));
    }
    void oneMessage(const QString& ch) { writeFile(mail(ch) + QStringLiteral("/INBOX/cur/old1:2,S"), "originale\n"); }

    Config makeConfig() {
        const QString p = f(QStringLiteral("config.ini"));
        writeFile(p, QStringLiteral("[account:a]\nname=Conto A\nmaildirRoot=%1/mail/a\nmbsyncChannel=a\n"
                                    "[account:b]\nname=Conto B\nmaildirRoot=%1/mail/b\nmbsyncChannel=b\n"
                                    "[identity:a1]\naccountId=a\naddress=alice@example.test\nmsmtpAccount=acca\n"
                                    "[identity:b1]\naccountId=b\naddress=bob@example.org\nmsmtpAccount=accb\n")
                      .arg(dir_).toUtf8());
        QString err;
        Config c = Config::load(p, &err);
        if (!err.isEmpty()) qFatal("config: %s", qPrintable(err));
        return c;
    }
    MainWindowOptions opts() const {
        MainWindowOptions o;
        o.muProgram = mu_;
        o.muHome = f(QStringLiteral("muhome"));
        o.mbsyncProgram = mbsync_;
        o.lockPath = f(QStringLiteral("lock"));
        o.muRoot = f(QStringLiteral("mail"));
        o.autoSync = true;
        return o;
    }
    static FirstRunDialog* dlg(MainWindow& w) { return w.firstRunDialog(); }
    bool waitStatus(MainWindow& w, const QString& id, const QString& st, int ms = 10000) {
        return QTest::qWaitFor([&] { return dlg(w) && dlg(w)->statusText(id) == st; }, ms);
    }

private slots:
    void initTestCase() {
        QVERIFY(root_.isValid());
        dir_ = root_.path();
        mu_ = writeScript(dir_, QStringLiteral("mu"),
                          "#!/bin/sh\nd=$(dirname \"$0\")\nprintf '%s\\n' \"$*\" >> \"$d/mu.log\"\n"
                          "case \"$1\" in\n find) echo '[]';;\n info) exit 1;;\n"
                          " index) echo mu-start >> \"$d/seq.log\"; sleep 0.3; echo mu-end >> \"$d/seq.log\"; exit 0;;\n *) exit 0;;\nesac\n");
        // mbsync finto: gate.<canale> = resta in attesa finche' il file esiste; fail.<canale> = errore
        // di rete; partial.<canale> = crea due messaggi poi cade. Segnala (overlap.log) due esecuzioni sovrapposte.
        mbsync_ = writeScript(dir_, QStringLiteral("mbsync"),
                              "#!/bin/sh\nd=$(dirname \"$0\")\nch=\"$2\"\n"
                              "echo \"$ch\" >> \"$d/mbsync.log\"\necho mb-start >> \"$d/seq.log\"\n"
                              "[ -f \"$d/busy\" ] && echo \"$ch\" >> \"$d/overlap.log\"\n"
                              "echo \"$ch\" > \"$d/busy\"\necho $$ > \"$d/pid.$ch\"\n"
                              "trap 'echo term > \"$d/term.$ch\"; rm -f \"$d/busy\"; exit 143' TERM\n"
                              "echo 'C: 0/1  B: 1/3'\n"
                              "while [ -f \"$d/gate.$ch\" ]; do sleep 0.05; done\n"
                              "if [ -f \"$d/fail.$ch\" ]; then echo 'mbsync: network unreachable' >&2; rm -f \"$d/busy\"; exit 1; fi\n"
                              "m=\"$d/mail/$ch\"\nmkdir -p \"$m/INBOX/cur\" \"$m/INBOX/new\" \"$m/INBOX/tmp\"\n"
                              "if [ -f \"$d/partial.$ch\" ]; then echo uno > \"$m/INBOX/cur/p1:2,S\"; echo due > \"$m/INBOX/cur/p2:2,S\";"
                              " echo 'mbsync: connection lost' >&2; rm -f \"$d/busy\"; exit 1; fi\n"
                              "echo nuovo > \"$m/INBOX/cur/n.$$:2,S\"\necho 'C: 1/1  B: 3/3'\nrm -f \"$d/busy\"\nexit 0\n");
    }
    void init() { resetEnv(); }
    void cleanup() { // nessun mbsync finto deve sopravvivere a un test
        for (const char* ch : {"a", "b"}) QTRY_VERIFY_WITH_TIMEOUT(!pidAlive(QLatin1String(ch)), 5000);
        QVERIFY2(!has(QStringLiteral("overlap.log")), "due mbsync sovrapposti");
    }

    // ---- rilevamento ----
    void testDetectionDefinition() {
        const QString r = f(QStringLiteral("det"));
        int ex = 0;
        QVERIFY(SyncService::isFirstSync(r + QStringLiteral("/assente"), &ex)); // radice assente
        QDir().mkpath(r + QStringLiteral("/INBOX/cur"));
        QDir().mkpath(r + QStringLiteral("/INBOX/new"));
        QDir().mkpath(r + QStringLiteral("/INBOX/tmp"));
        QDir().mkpath(r + QStringLiteral("/.Sub/cur"));
        writeFile(r + QStringLiteral("/INBOX/.uidvalidity"), "1");
        writeFile(r + QStringLiteral("/INBOX/tmp/t1"), "x");   // tmp/ non conta
        writeFile(r + QStringLiteral("/INBOX/cur/.nascosto"), "x"); // file nascosto non conta
        QVERIFY(SyncService::isFirstSync(r));                  // solo cartelle vuote
        writeFile(r + QStringLiteral("/.Sub/cur/m1:2,S"), "m");   // sottocartella Maildir++
        QVERIFY(!SyncService::isFirstSync(r));
        QFile::remove(r + QStringLiteral("/.Sub/cur/m1:2,S"));
        QVERIFY(SyncService::isFirstSync(r));
        writeFile(r + QStringLiteral("/INBOX/new/m2"), "m");
        QVERIFY(!SyncService::isFirstSync(r));
        QDir(r).removeRecursively();
    }
    void testDetectionStopsAtFirstFile() {
        const QString r = f(QStringLiteral("det2"));
        QDir().mkpath(r + QStringLiteral("/INBOX/cur"));
        for (int i = 0; i < 3000; ++i) writeFile(r + QStringLiteral("/INBOX/cur/m%1:2,S").arg(i), "m");
        for (int i = 0; i < 200; ++i) QDir().mkpath(r + QStringLiteral("/F%1/cur").arg(i));
        int ex = 0;
        QVERIFY(SyncService::maildirHasMessages(r, &ex));
        // Nessuna enumerazione completa: la ricerca si ferma al primo file (non dipende dal tempo).
        QVERIFY2(ex <= 3, qPrintable(QString::number(ex)));
        QDir(r).removeRecursively();
    }

    // ---- avvio guidato ----
    void testEmptyMaildirTriggersFirstRunFlow() {
        emptyFolders(QStringLiteral("a")); // cartelle vuote: prima sincronizzazione
        // b: radice assente
        touch(QStringLiteral("gate.a"));
        touch(QStringLiteral("gate.b"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(QTest::qWaitFor([&] { return dlg(w) && dlg(w)->isVisible(); }, 10000));
        QCOMPARE(dlg(w)->accountIds(), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("In corso")));
        QCOMPARE(dlg(w)->statusText(QStringLiteral("b")), QStringLiteral("In attesa"));
        QVERIFY(QTest::qWaitFor([&] { return dlg(w)->progressText(QStringLiteral("a")) == QStringLiteral("1/3"); }, 10000));
        // contatori onesti: barra occupata (range 0,0), mai una percentuale inventata
        QCOMPARE(dlg(w)->bar(QStringLiteral("a"))->maximum(), 0);
        QVERIFY(dlg(w)->noteLabel()->text().contains(QStringLiteral("15 GB")));
        QVERIFY(w.firstRunButton()->isVisibleTo(&w));
        QVERIFY(w.statusLabel()->text().contains(QStringLiteral("1/3")));
        QCOMPARE(runs(QStringLiteral("b")), 0); // un account alla volta
        QFile::remove(f(QStringLiteral("gate.a")));
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Completata")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("In corso")));
        QFile::remove(f(QStringLiteral("gate.b")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
        QTRY_VERIFY_WITH_TIMEOUT(w.syncService()->state() == SyncService::Idle, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(QString::fromUtf8(readFile(f(QStringLiteral("mu.log")))).contains(QStringLiteral("index")), 10000);
        QVERIFY(files(QStringLiteral("a")) >= 1 && files(QStringLiteral("b")) >= 1);
    }
    void testNonEmptyMaildirIsNotFirstRun() {
        emptyFolders(QStringLiteral("a"));
        oneMessage(QStringLiteral("a"));
        oneMessage(QStringLiteral("b"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QTRY_VERIFY_WITH_TIMEOUT(totalRuns() == 2 && w.syncService()->state() == SyncService::Idle, 10000);
        QVERIFY(!w.firstRunDialog());
        QVERIFY(!w.firstRunButton()->isVisibleTo(&w));
    }
    void testOnlyEmptyAccountGetsDialog() {
        oneMessage(QStringLiteral("a"));
        touch(QStringLiteral("gate.b"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("In corso")));
        QCOMPARE(dlg(w)->accountIds(), QStringList{QStringLiteral("b")});
        QFile::remove(f(QStringLiteral("gate.b")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
        QTRY_VERIFY_WITH_TIMEOUT(runs(QStringLiteral("a")) == 1 && w.syncService()->state() == SyncService::Idle, 10000);
    }
    void testInterruptedFirstSyncResumesOnNextStart() {
        // Prima esecuzione: mbsync crea due messaggi e cade (a). Boma non scrive nella Maildir.
        touch(QStringLiteral("partial.a"));
        oneMessage(QStringLiteral("b")); // b e' gia' piena: il dialogo riguarda solo a
        {
            MainWindow w(makeConfig(), opts());
            w.show();
            QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Errore")));
            QVERIFY(dlg(w)->errorText(QStringLiteral("a")).contains(QStringLiteral("connection lost")));
        }
        QCOMPARE(files(QStringLiteral("a")), 2);
        const QByteArray p1 = readFile(mail(QStringLiteral("a")) + QStringLiteral("/INBOX/cur/p1:2,S"));
        const QByteArray p2 = readFile(mail(QStringLiteral("a")) + QStringLiteral("/INBOX/cur/p2:2,S"));
        QCOMPARE(p1, QByteArray("uno\n"));
        const int before = runs(QStringLiteral("a"));
        // Riavvio: ormai la Maildir non e' vuota (mbsync riprende da solo, e' incrementale):
        // sincronizzazione normale, senza dialogo, che parte e completa.
        QFile::remove(f(QStringLiteral("partial.a")));
        {
            MainWindow w(makeConfig(), opts());
            w.show();
            QTRY_VERIFY_WITH_TIMEOUT(runs(QStringLiteral("a")) == before + 1 && w.syncService()->state() == SyncService::Idle, 10000);
            QVERIFY(!w.firstRunDialog());
            QVERIFY(w.statusLabel()->text().contains(QStringLiteral("Sincronizzato")));
        }
        QCOMPARE(readFile(mail(QStringLiteral("a")) + QStringLiteral("/INBOX/cur/p1:2,S")), p1);
        QCOMPARE(readFile(mail(QStringLiteral("a")) + QStringLiteral("/INBOX/cur/p2:2,S")), p2);
        QVERIFY(files(QStringLiteral("a")) >= 3);
    }
    void testInterruptedWithNoFilesShowsDialogAgainAtNextStart() {
        touch(QStringLiteral("fail.a"));
        touch(QStringLiteral("fail.b"));
        { MainWindow w(makeConfig(), opts()); w.show(); QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Errore"))); }
        QFile::remove(f(QStringLiteral("fail.a")));
        QFile::remove(f(QStringLiteral("fail.b")));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Completata")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
    }
    void testCloseDialogDoesNotStopSync() {
        touch(QStringLiteral("gate.a"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("In corso")));
        QVERIFY(QTest::qWaitFor([&] { return pidAlive(QStringLiteral("a")); }, 5000));
        QTest::mouseClick(dlg(w)->closeButton(), Qt::LeftButton);
        QVERIFY(!dlg(w)->isVisible());
        QTest::qWait(300);
        QCOMPARE(w.syncService()->state(), SyncService::Running);
        QVERIFY(pidAlive(QStringLiteral("a")));
        QVERIFY(!has(QStringLiteral("term.a")));
        // riapribile dalla barra di stato mentre la sincronizzazione prosegue
        QVERIFY(w.firstRunButton()->isVisibleTo(&w));
        w.firstRunButton()->click();
        QVERIFY(dlg(w)->isVisible());
        QCOMPARE(dlg(w)->statusText(QStringLiteral("a")), QStringLiteral("In corso"));
        QTest::mouseClick(dlg(w)->closeButton(), Qt::LeftButton);
        QFile::remove(f(QStringLiteral("gate.a")));
        QTRY_VERIFY_WITH_TIMEOUT(w.syncService()->state() == SyncService::Idle && totalRuns() >= 2, 10000);
        QCOMPARE(dlg(w)->statusText(QStringLiteral("a")), QStringLiteral("Completata"));
        QVERIFY(files(QStringLiteral("a")) >= 1);
    }
    void testErrorAndRetry() {
        touch(QStringLiteral("fail.a"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Errore")));
        QVERIFY(dlg(w)->errorText(QStringLiteral("a")).contains(QStringLiteral("network unreachable")));
        // b prosegue indipendentemente dall'errore di a
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
        QTRY_VERIFY_WITH_TIMEOUT(w.syncService()->state() != SyncService::Running, 10000);
        QVERIFY(dlg(w)->retryButton(QStringLiteral("a"))->isVisibleTo(dlg(w)));
        QTRY_VERIFY_WITH_TIMEOUT(dlg(w)->retryButton(QStringLiteral("a"))->isEnabled(), 10000); // dopo mu index
        QVERIFY(!dlg(w)->retryButton(QStringLiteral("b"))->isVisibleTo(dlg(w)));
        const int before = runs(QStringLiteral("a"));
        QFile::remove(f(QStringLiteral("fail.a")));
        touch(QStringLiteral("gate.a"));
        dlg(w)->retryButton(QStringLiteral("a"))->click();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("In corso")));
        QVERIFY(!dlg(w)->retryButton(QStringLiteral("a"))->isVisibleTo(dlg(w)));
        QFile::remove(f(QStringLiteral("gate.a")));
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Completata")));
        QCOMPARE(runs(QStringLiteral("a")), before + 1);
    }
    void testRetryWhileRunningNeverStartsSecondSync() {
        touch(QStringLiteral("fail.a"));
        touch(QStringLiteral("gate.b"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Errore")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("In corso")));
        // b e' in corso: Riprova di a e' disattivato e, anche se emesso, non avvia nulla
        QVERIFY(!dlg(w)->retryButton(QStringLiteral("a"))->isEnabled());
        emit dlg(w)->retryRequested(QStringLiteral("a"));
        QTest::qWait(300);
        QCOMPARE(runs(QStringLiteral("a")), 1);
        QFile::remove(f(QStringLiteral("gate.b")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
    }
    void testOfflineStartShowsErrorAndStaysUsable() {
        touch(QStringLiteral("fail.a"));
        touch(QStringLiteral("fail.b"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Errore")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Errore")));
        QVERIFY(dlg(w)->errorText(QStringLiteral("b")).contains(QStringLiteral("network unreachable")));
        QVERIFY(w.statusLabel()->text().contains(QStringLiteral("ffline")));
        QVERIFY(w.isEnabled() && w.searchEdit()->isEnabled() && w.newButton()->isEnabled());
        // nessun ciclo di tentativi automatici: un solo tentativo per account
        QTest::qWait(1500);
        QCOMPARE(runs(QStringLiteral("a")), 1);
        QCOMPARE(runs(QStringLiteral("b")), 1);
        QVERIFY(!w.syncService()->anySynced());
    }

    void testFirstSyncOutlivesTimeoutAndPeriodicTimerStartsAfterFailure() {
        // I1: nessun tempo massimo per le prime sincronizzazioni
        touch(QStringLiteral("gate.a"));
        {
            MainWindow w(makeConfig(), opts());
            w.syncService()->setTimeout(std::chrono::milliseconds(300));
            w.show();
            QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("In corso")));
            QTest::qWait(1000); // ben oltre il tempo massimo
            QCOMPARE(dlg(w)->statusText(QStringLiteral("a")), QStringLiteral("In corso"));
            QFile::remove(f(QStringLiteral("gate.a")));
            QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Completata")));
            QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
            QVERIFY(!has(QStringLiteral("term.a")));
        }
    }
    void testPeriodicTimerStartsAfterFailedStartupAndTickUpdatesRow() {
        // I2
        resetEnv();
        touch(QStringLiteral("fail.a"));
        touch(QStringLiteral("fail.b"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Errore")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Errore")));
        QTRY_VERIFY_WITH_TIMEOUT(w.syncService()->timerActive(), 5000);
        QFile::remove(f(QStringLiteral("fail.a")));
        QFile::remove(f(QStringLiteral("fail.b")));
        w.syncService()->setInterval(std::chrono::milliseconds(300));
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Completata")));
    }
    void testSyncNowDuringStartupDetectionDoesNotDropFirstAccount() {
        MainWindow w(makeConfig(), opts());
        w.show();
        w.syncNow(); // prima della fine del rilevamento asincrono
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("Completata")));
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
    }
    void testMuIndexNeverOverlapsNextMbsync() {
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("b"), QStringLiteral("Completata")));
        QTRY_VERIFY_WITH_TIMEOUT(w.syncService()->state() == SyncService::Idle && lines(QStringLiteral("seq.log")).count(QStringLiteral("mu-end")) >= 2, 10000);
        bool inMu = false;
        for (const QString& l : lines(QStringLiteral("seq.log"))) {
            if (l == QLatin1String("mu-start")) inMu = true;
            else if (l == QLatin1String("mu-end")) inMu = false;
            else QVERIFY2(!inMu, "mbsync partito durante mu index");
        }
    }

    // ---- vista del dialogo ----
    void testTwoAccountsProgressIndependent() {
        FirstRunDialog d;
        d.setAccounts({{QStringLiteral("a"), QStringLiteral("A")}, {QStringLiteral("b"), QStringLiteral("B")}});
        d.show();
        FirstRunState sa; sa.status = FirstRunState::Running; sa.done = 2; sa.total = 7;
        FirstRunState sb; sb.status = FirstRunState::Failed; sb.error = QStringLiteral("rete assente");
        d.setState(QStringLiteral("a"), sa);
        d.setState(QStringLiteral("b"), sb);
        QCOMPARE(d.progressText(QStringLiteral("a")), QStringLiteral("2/7"));
        QCOMPARE(d.progressText(QStringLiteral("b")), QString());
        QCOMPARE(d.statusText(QStringLiteral("a")), QStringLiteral("In corso"));
        QCOMPARE(d.statusText(QStringLiteral("b")), QStringLiteral("Errore"));
        QCOMPARE(d.errorText(QStringLiteral("b")), QStringLiteral("rete assente"));
        QVERIFY(d.errorText(QStringLiteral("a")).isEmpty());
        sa.done = 5;
        d.setState(QStringLiteral("a"), sa);
        QCOMPARE(d.progressText(QStringLiteral("a")), QStringLiteral("5/7"));
        QCOMPARE(d.statusText(QStringLiteral("b")), QStringLiteral("Errore"));
        d.setState(QStringLiteral("zzz"), sa); // account ignoto: ignorato
    }

    // ---- ciclo di vita ----
    void testDestroyDialogMidSyncNoCrashAndSyncContinues() {
        touch(QStringLiteral("gate.a"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("In corso")));
        delete dlg(w);
        QVERIFY(!w.firstRunDialog());
        QTest::qWait(200); // arrivano progressi/segnali con il dialogo distrutto
        QVERIFY(pidAlive(QStringLiteral("a")));
        QVERIFY(!has(QStringLiteral("term.a")));
        // il pulsante lo ricrea con lo stato corrente
        w.firstRunButton()->click();
        QVERIFY(w.firstRunDialog() && w.firstRunDialog()->isVisible());
        QCOMPARE(dlg(w)->statusText(QStringLiteral("a")), QStringLiteral("In corso"));
        delete dlg(w);
        QFile::remove(f(QStringLiteral("gate.a")));
        QTRY_VERIFY_WITH_TIMEOUT(w.syncService()->state() == SyncService::Idle && files(QStringLiteral("a")) >= 1 && files(QStringLiteral("b")) >= 1, 10000);
    }
    void testDestroyWindowMidSyncTerminatesMbsyncGracefully() {
        touch(QStringLiteral("gate.a"));
        {
            auto* w = new MainWindow(makeConfig(), opts());
            w->show();
            QVERIFY(waitStatus(*w, QStringLiteral("a"), QStringLiteral("In corso")));
            QVERIFY(QTest::qWaitFor([&] { return pidAlive(QStringLiteral("a")); }, 5000));
            delete w; // nessun crash; il processo riceve SIGTERM
        }
        QVERIFY(has(QStringLiteral("term.a"))); // gestore di SIGTERM eseguito: arresto ordinato, non SIGKILL
        QVERIFY(!pidAlive(QStringLiteral("a")));
        QCOMPARE(runs(QStringLiteral("b")), 0); // nulla riparte dopo la chiusura
    }
    void testQuittingTerminatesMbsync() {
        touch(QStringLiteral("gate.a"));
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitStatus(w, QStringLiteral("a"), QStringLiteral("In corso")));
        QVERIFY(QTest::qWaitFor([&] { return pidAlive(QStringLiteral("a")); }, 5000));
        QMetaObject::invokeMethod(qApp, "aboutToQuit", Qt::DirectConnection);
        QVERIFY(has(QStringLiteral("term.a")));
        QVERIFY(!pidAlive(QStringLiteral("a")));
        QTest::qWait(300);
        QCOMPARE(runs(QStringLiteral("b")), 0);
    }
};

QTEST_MAIN(T)
#include "test_firstrun.moc"
