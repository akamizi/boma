#include <QtTest>
#include "core/MailIndex.h"
using namespace boma;

// Opt-in: BOMA_PERF=1 (genera 50 000 messaggi e li indicizza: minuti).
class T : public QObject {
    Q_OBJECT
    template <class F> static qint64 timed(F f) {
        QElapsedTimer t; t.start(); f(); return t.elapsed();
    }
private slots:
    void testPerfSearchOn50kMessages() {
        if (qEnvironmentVariable("BOMA_PERF") != "1")
            QSKIP("prova di prestazioni disattivata: impostare BOMA_PERF=1");
        const int N = 50000;
        QTemporaryDir td(QDir::currentPath() + "/perfXXXXXX");
        QVERIFY(td.isValid());
        const QString mail = td.path() + "/mail", muhome = td.path() + "/muhome";
        QProcess gen;
        qint64 tGen = timed([&] {
            gen.start("python3", {QString(BOMA_FIXTURES_DIR) + "/make_maildir.py", mail, QString::number(N)});
            gen.waitForFinished(30 * 60 * 1000);
        });
        QCOMPARE(gen.exitCode(), 0);
        QProcess init;
        init.start("mu", {"init", "--muhome=" + muhome, "--maildir=" + mail});
        QVERIFY(init.waitForFinished(60000));
        MailIndex idx(this, "mu", muhome);
        bool done = false, ok = false;
        const qint64 tIndex = timed([&] {
            idx.reindex("x", [&](bool o, QString) { ok = o; done = true; });
            (void)QTest::qWaitFor([&] { return done; }, 60 * 60 * 1000);
        });
        QVERIFY(ok);
        auto measure = [&](const char* label, auto call) -> int {
            bool d = false; QList<MessageSummary> list; QString err;
            const qint64 ms = timed([&] {
                call([&](QList<MessageSummary> l, QString e) { list = l; err = e; d = true; });
                (void)QTest::qWaitFor([&] { return d; }, 120000);
            });
            if (!err.isEmpty()) { qCritical() << err; return -1; }
            qInfo().noquote() << QString("PERF %1: %2 ms (%3 risultati)").arg(label).arg(ms).arg(list.size());
            if (ms > 200) qWarning().noquote() << QString("PERF %1 oltre 200 ms: %2 ms").arg(label).arg(ms);
            return list.size();
        };
        qInfo().noquote() << QString("PERF generazione %1 messaggi: %2 ms; indicizzazione: %3 ms").arg(N).arg(tGen).arg(tIndex);
        QCOMPARE(measure("prima pagina (date:2000..)", [&](auto cb) { idx.search("date:2000..", 0, 200, cb); }), 200);
        QCOMPARE(measure("prima pagina non lette", [&](auto cb) { idx.unread(0, 200, cb); }), 200);
        measure("ricerca subject:numero4242", [&](auto cb) { idx.search("subject:numero4242", 0, 200, cb); });
        measure("ricerca from:Mittente5 (200)", [&](auto cb) { idx.search("from:Mittente5", 0, 200, cb); });
        measure("pagina profonda non lette (offset 5000)", [&](auto cb) { idx.unread(5000, 200, cb); });
    }
};
QTEST_MAIN(T)
#include "test_perf.moc"
