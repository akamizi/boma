#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QFile>
#include <QDir>
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
    void testSyncStartsAtMostOneAtATime() {
        QString log = dir.filePath("log1");
        QString p = writeScript(dir.filePath("m1"), "echo \"$@\" >> " + log + "\nsleep 0.3\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c); s.setMbsyncProgram(p);
        QSignalSpy st(&s, &SyncService::stateChanged);
        s.syncAll(); s.syncAll();
        QCOMPARE(s.state(), SyncService::Running);
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Idle, 10000);
        QCOMPARE(countLines(log), 2); // due account, un solo giro
        QCOMPARE(st.first().first().toInt(), int(SyncService::Running));
        QVERIFY(s.anySynced());
    }
    void testQuickModeUsesPullNew() {
        QString log = dir.filePath("logq");
        QString p = writeScript(dir.filePath("mq"), "echo \"$@\" >> " + log + "\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c); s.setMbsyncProgram(p);
        s.setQuick(true);
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Idle, 10000);
        QFile f(log); QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("--pull-new -V chA"));
    }
    void testFailureReportsFirstErrorAndDoesNotRetry() {
        QString log = dir.filePath("log2");
        QString p = writeScript(dir.filePath("m2"), "echo x >> " + log + "\necho boom >&2\nexit 1");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c); s.setMbsyncProgram(p);
        QSignalSpy st(&s, &SyncService::stateChanged);
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(countLines(log), 2, 10000); // b viene sincronizzato comunque
        QTest::qWait(300);
        QCOMPARE(countLines(log), 2);
        QCOMPARE(st.last().at(1).toString(), QString("a"));
        QVERIFY(st.last().at(2).toString().contains("boom"));
        QVERIFY(!s.busyElsewhere());
        QVERIFY(!s.anySynced());
    }
    void testLockedChannelIsReportedAsBusyElsewhere() {
        QString p = writeScript(dir.filePath("m3"), "echo 'Error: channel chA is locked' >&2\nexit 1");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c); s.setMbsyncProgram(p);
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        QVERIFY(s.busyElsewhere());
        QVERIFY(SyncService::isLockError("Error: channel x is locked"));
        QVERIFY(!SyncService::isLockError("Network is unreachable"));
        s.setMbsyncProgram(writeScript(dir.filePath("m3b"), "exit 0"));
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Idle, 10000);
        QVERIFY(!s.busyElsewhere());
    }
    void testWithoutNetworkAndMissingProgramDoNotCrash() {
        QString p = writeScript(dir.filePath("m4"), "echo 'mbsync: could not connect: Network is unreachable' >&2\nexit 1");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c); s.setMbsyncProgram(p);
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        s.setMbsyncProgram("/nonexistent/mbsync");
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
    }
    void testFailureOfOneAccountDoesNotStarveTheOther() {
        QString log = dir.filePath("log10");
        QString p = writeScript(dir.filePath("m10"), "echo $2 >> " + log + "\n[ \"$2\" = chA ] && { echo boom >&2; exit 1; }\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c); s.setMbsyncProgram(p);
        for (int round = 1; round <= 2; ++round) {
            s.syncAll();
            QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
            QCOMPARE(countLines(log), round * 2);
            QVERIFY(s.anySynced());
        }
    }
    void testDestroyWhileRunningIsSafe() {
        QString log = dir.filePath("log6");
        QString p = writeScript(dir.filePath("m6"), "echo x >> " + log + "\nsleep 5\nexit 1");
        ProcessRunner r; Config c = cfg();
        auto* s = new SyncService(&r, c);
        s->setMbsyncProgram(p);
        s->syncAll();
        QTRY_VERIFY_WITH_TIMEOUT(countLines(log) == 1, 5000);
        delete s;
        QTest::qWait(500);
        QCOMPARE(countLines(log), 1); // il giro non prosegue
    }
    void testTimeoutSendsSigtermFirst() {
        QString mark = dir.filePath("term8");
        QString p = writeScript(dir.filePath("m8"),
            "trap 'echo term > " + mark + "; exit 143' TERM\ni=0\nwhile [ $i -lt 100 ]; do sleep 0.05; i=$((i+1)); done\nexit 0");
        ProcessRunner r; Config c = cfg();
        SyncService s(&r, c); s.setMbsyncProgram(p);
        s.setTimeout(std::chrono::milliseconds(300));
        QSignalSpy st(&s, &SyncService::stateChanged);
        s.syncAll();
        QTRY_COMPARE_WITH_TIMEOUT(s.state(), SyncService::Failed, 10000);
        QVERIFY(st.last().at(2).toString().contains("Tempo scaduto"));
        QVERIFY(QFile::exists(mark)); // SIGTERM, non SIGKILL
    }
};
QTEST_MAIN(T)
#include "test_syncservice.moc"
