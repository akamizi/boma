#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include "core/TagStore.h"

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
};
QTEST_MAIN(T)
#include "test_tagstore.moc"
