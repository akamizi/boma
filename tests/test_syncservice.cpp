#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QFile>
#include <QDir>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include "core/SyncService.h"
using namespace boma;

static QString writeScript(const QString& path, const QString& body) {
    QFile f(path);
    (void)f.open(QIODevice::WriteOnly);
    f.write(("#!/bin/sh\n" + body + "\n").toUtf8());
    f.close();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

class T : public QObject {
    Q_OBJECT
    // sotto la directory di lavoro: /tmp puo' essere montato noexec
    QTemporaryDir dir{QDir::currentPath() + "/fake-XXXXXX"};
    Config cfg() {
        QString ini = dir.filePath("c.ini");
        QFile f(ini); (void)f.open(QIODevice::WriteOnly);
        f.write("[account:a]\nname=A\nmbsyncChannel=chA\n[account:b]\nname=B\nmbsyncChannel=chB\n");
        f.close();
        return Config::load(ini);
    }
    int countLines(const QString& p) {
        QFile f(p); if (!f.open(QIODevice::ReadOnly)) return 0;
        return f.readAll().count('\n');
    }
private slots:
    void testParseProgress() {
        int d = 0, t = 0;
        QVERIFY(SyncService::parseProgress("C: 1/2  B: 3/4  F: +0/0 *0/0 #0/0", &d, &t));
        QCOMPARE(d, 3); QCOMPARE(t, 4);
        QVERIFY(SyncService::parseProgress("C: 1/2", &d, &t));
        QCOMPARE(d, 1); QCOMPARE(t, 2);
        QVERIFY(!SyncService::parseProgress("nothing", &d, &t));
    }
    void testSyncStartsAtMostOneAtATime() {
        QString log = dir.filePath("log1");
        QString p = writeScript(dir.filePath("m1"), "echo \"$@\" >> " + log + "\necho 'C: 1/2  B: 3/4'\nsleep 0.3\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, dir.filePath("lock1")); s.setMbsyncProgram(p);
        QSignalSpy prog(&s, &SyncService::progress);
        QSignalSpy st(&s, &SyncService::stateChanged);
        s.syncAll(); s.syncAll();
        QCOMPARE(s.state(), SyncService::Running);
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Idle, 10000);
        QCOMPARE(countLines(log), 2); // due account, un solo giro
        QVERIFY(prog.count() >= 1);
        QCOMPARE(prog.first().at(1).toInt(), 3);
        QCOMPARE(st.first().first().toInt(), int(SyncService::Running));
    }
    void testSyncFailureReportsAndDoesNotRetry() {
        QString log = dir.filePath("log2");
        QString p = writeScript(dir.filePath("m2"), "echo x >> " + log + "\necho boom >&2\nexit 1");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, dir.filePath("lock2")); s.setMbsyncProgram(p);
        QSignalSpy st(&s, &SyncService::stateChanged);
        s.syncAccount("a");
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        QTest::qWait(300);
        QCOMPARE(countLines(log), 1);
        QCOMPARE(st.last().at(1).toString(), QString("a"));
        QVERIFY(st.last().at(2).toString().contains("boom"));
    }
    void testLockHeldByAnotherProcessIsReported() {
        QString log = dir.filePath("log3");
        QString p = writeScript(dir.filePath("m3"), "echo x >> " + log);
        QString lock = dir.filePath("lock3");
        int fd = ::open(lock.toLocal8Bit().constData(), O_RDWR | O_CREAT, 0600);
        QCOMPARE(::flock(fd, LOCK_EX | LOCK_NB), 0);
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, lock); s.setMbsyncProgram(p);
        s.syncAll();
        QCOMPARE(s.state(), SyncService::Failed);
        QTest::qWait(200);
        QCOMPARE(countLines(log), 0);
        ::close(fd);
        s.syncAll(); // ora libero
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Idle, 10000);
    }
    void testSyncWithoutNetworkStillAllowsOfflineUse() {
        QString p = writeScript(dir.filePath("m4"), "echo 'mbsync: could not connect: Network is unreachable' >&2\nexit 1");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, dir.filePath("lock4")); s.setMbsyncProgram(p);
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        // missing program too: no crash
        s.setMbsyncProgram("/nonexistent/mbsync");
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
    }
    void testFailureInFirstAccountStopsRound() {
        QString log = dir.filePath("log5");
        QString p = writeScript(dir.filePath("m5"), "echo x >> " + log + "\necho boom >&2\nexit 1");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, dir.filePath("lock5")); s.setMbsyncProgram(p);
        QSignalSpy st(&s, &SyncService::stateChanged);
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        QTest::qWait(300);
        QCOMPARE(countLines(log), 1);
        QCOMPARE(st.last().at(1).toString(), QString("a"));
    }
    void testDestroyWhileRunningIsSafe() {
        QString log = dir.filePath("log6");
        QString p = writeScript(dir.filePath("m6"), "echo x >> " + log + "\nsleep 5\nexit 1");
        ProcessRunner r; Config c = cfg();
        auto* s = new SyncService(&r, c, dir.filePath("lock6"));
        s->setMbsyncProgram(p);
        QSignalSpy st(s, &SyncService::stateChanged);
        s->syncAll();
        QTRY_VERIFY_WITH_TIMEOUT(countLines(log) == 1, 5000);
        delete s;
        QTest::qWait(500);
        // il lock e' libero e nessun figlio e' rimasto
        int fd = ::open(dir.filePath("lock6").toLocal8Bit().constData(), O_RDWR);
        QCOMPARE(::flock(fd, LOCK_EX | LOCK_NB), 0);
        ::close(fd);
    }
    void testProgressSplitAcrossChunksIsNotMisread() {
        QString p = writeScript(dir.filePath("m7"),
            "printf 'C: 1/2  B: 3/'\nsleep 0.3\nprintf '4\\n'\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, dir.filePath("lock7")); s.setMbsyncProgram(p);
        QSignalSpy prog(&s, &SyncService::progress);
        s.syncAccount("a");
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Idle, 10000);
        QCOMPARE(prog.count(), 1);
        QCOMPARE(prog.first().at(1).toInt(), 3);
        QCOMPARE(prog.first().at(2).toInt(), 4);
    }
    void testTimeoutSendsSigtermFirst() {
        QString mark = dir.filePath("term8");
        QString p = writeScript(dir.filePath("m8"),
            "trap 'echo term > " + mark + "; exit 143' TERM\ni=0\nwhile [ $i -lt 100 ]; do sleep 0.05; i=$((i+1)); done\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, dir.filePath("lock8")); s.setMbsyncProgram(p);
        s.setTimeout(std::chrono::milliseconds(300));
        QSignalSpy st(&s, &SyncService::stateChanged);
        s.syncAccount("a");
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        QVERIFY(st.last().at(2).toString().contains("Tempo scaduto"));
        QVERIFY(QFile::exists(mark)); // SIGTERM, non SIGKILL
    }
    void testNoTimeoutAccountOutlivesTimeout() {
        QString p = writeScript(dir.filePath("m9"), "sleep 1\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c, dir.filePath("lock9")); s.setMbsyncProgram(p);
        s.setTimeout(std::chrono::milliseconds(300));
        s.setNoTimeout("a", true);
        s.syncAccount("a");
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Idle, 10000);
        QVERIFY(s.anySynced());
        s.setNoTimeout("a", false); // ora vale il tempo massimo
        s.syncAccount("a");
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
    }
};
QTEST_MAIN(T)
#include "test_syncservice.moc"
