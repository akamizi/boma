#include <QtTest>
#include "core/ProcessRunner.h"
using namespace boma;
using namespace std::chrono_literals;

class T : public QObject {
    Q_OBJECT
private slots:
    void testRunnerCapturesOutputAndExitCode() {
        ProcessRunner r; bool got = false; ProcessResult res;
        r.run("sh", {"-c", "echo hi; echo err >&2; exit 3"}, 5s,
              [&](ProcessResult x) { res = x; got = true; });
        QTRY_VERIFY_WITH_TIMEOUT(got, 5000);
        QCOMPARE(res.exitCode, 3);
        QVERIFY(!res.timedOut && !res.crashed);
        QCOMPARE(res.out, QByteArray("hi\n"));
        QCOMPARE(res.err, QByteArray("err\n"));
    }
    void testRunnerTimesOut() {
        ProcessRunner r; bool got = false; ProcessResult res;
        r.run("sleep", {"10"}, 200ms, [&](ProcessResult x) { res = x; got = true; });
        QTRY_VERIFY_WITH_TIMEOUT(got, 5000);
        QVERIFY(res.timedOut);
    }
    void testRunnerMissingProgramReportsFailure() {
        ProcessRunner r; bool got = false; ProcessResult res;
        r.run("/nonexistent/prog", {}, 1s, [&](ProcessResult x) { res = x; got = true; });
        QTRY_VERIFY_WITH_TIMEOUT(got, 5000);
        QVERIFY(res.exitCode != 0);
    }
};
QTEST_MAIN(T)
#include "test_processrunner.moc"
