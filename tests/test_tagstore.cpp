#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include "core/TagStore.h"
#include "core/GmailLabels.h"

using namespace boma;

class T : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString file() const { return dir_.path() + "/sub/tags.json"; }
private slots:
    void init() { QDir(dir_.path()).removeRecursively(); QDir().mkpath(dir_.path()); }
    void testSetAndPersist() {
        {
            TagStore s(file());
            QSignalSpy sp(&s, &TagStore::changed);
            QVERIFY(s.setTag({"a@x", "b@x"}, "  Lavoro  ", true));
            QCOMPARE(sp.count(), 1);
            QVERIFY(s.setTag({"a@x"}, "lavoro", true)); // gia' presente: nessun cambiamento
            QCOMPARE(sp.count(), 1);
            QVERIFY(s.setTag({"a@x"}, "Urgente", true));
        }
        TagStore r(file());
        QCOMPARE(r.tags("a@x"), (QStringList{"Lavoro", "Urgente"}));
        QCOMPARE(r.tags("b@x"), QStringList{"Lavoro"});
        QCOMPARE(r.counts().value("Lavoro"), 2);
        QCOMPARE(r.messageIds("LAVORO"), (QStringList{"a@x", "b@x"}));
    }
    void testRemoveAndEmptyEntryDisappears() {
        TagStore s(file());
        s.setTag({"a@x"}, "t", true);
        s.setTag({"a@x"}, "T", false);
        QVERIFY(s.tags("a@x").isEmpty());
        QVERIFY(s.counts().isEmpty());
    }
    void testRenameMergesAndRemoveTag() {
        TagStore s(file());
        s.setTag({"a@x"}, "uno", true);
        s.setTag({"b@x"}, "due", true);
        s.setTag({"b@x"}, "uno", true);
        QVERIFY(s.renameTag("uno", "due"));
        QCOMPARE(s.tags("a@x"), QStringList{"due"});
        QCOMPARE(s.tags("b@x"), QStringList{"due"});
        QVERIFY(s.removeTag("due"));
        QVERIFY(s.counts().isEmpty());
        QVERIFY(TagStore(file()).counts().isEmpty());
    }
    void testNormalize() {
        QCOMPARE(TagStore::normalize("  a \t b\n"), QString("a b"));
        QCOMPARE(TagStore::normalize(QString(60, 'x')).size(), 40);
    }
    void testCorruptFileIsKeptAside() {
        QDir().mkpath(dir_.path() + "/sub");
        QFile f(file());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{non json");
        f.close();
        TagStore s(file());
        QVERIFY(!s.loadError().isEmpty());
        QVERIFY(QFile::exists(file() + ".corrupt"));
        QVERIFY(s.counts().isEmpty());
    }
    void testUnwritablePathReportsError() {
        TagStore s("/proc/nope/tags.json");
        QVERIFY(!s.setTag({"a@x"}, "t", true));
        QVERIFY(!s.lastError().isEmpty());
        QCOMPARE(s.tags("a@x"), QStringList{"t"}); // lo stato in memoria resta
    }
    void testAddTagsMergesWithoutRemoving() {
        TagStore s(file());
        QVERIFY(s.setTag({"a@x"}, "Mia", true));
        QSignalSpy sp(&s, &TagStore::changed);
        QCOMPARE(s.addTags({{"a@x", {"Mia", "banca"}}, {"b@x", {"Tasse"}}, {"", {"x"}}}), 2);
        QCOMPARE(sp.count(), 1);
        QCOMPARE(s.tags("a@x"), (QStringList{"banca", "Mia"}));
        QCOMPARE(s.addTags({{"a@x", {"BANCA"}}}), 0); // nulla da aggiungere: nessun segnale
        QCOMPARE(sp.count(), 1);
        TagStore r(file());
        QCOMPARE(r.tags("b@x"), QStringList{"Tasse"});
    }
    void testGmailOutputParsing() {
        int done = -1;
        const auto m = GmailLabels::parseOutput(
            "{\"id\":\"a@x\",\"labels\":[\"Lavoro\",\"Citt\\u00e0\"]}\n"
            "garbage\n{\"id\":\"\",\"labels\":[\"x\"]}\n{\"id\":\"b@x\",\"labels\":[]}\n{\"done\":1}\n", &done);
        QCOMPARE(done, 1);
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.value("a@x"), (QStringList{"Lavoro", QString::fromUtf8("Citt\u00e0")}));
    }
    void testEmbeddedScriptSelfTest() {
        QProcess p;
        p.start("python3", {"-I", "-c", GmailLabels::script(), "--selftest"});
        QVERIFY(p.waitForFinished(20000));
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(p.readAllStandardOutput().contains("ok"));
    }
};
QTEST_MAIN(T)
#include "test_tagstore.moc"
