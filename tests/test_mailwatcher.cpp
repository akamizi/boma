#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "core/MailWatcher.h"

using namespace boma;

class T : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    void mk(const QString& folder) {
        for (const char* s : {"cur", "new", "tmp"}) QVERIFY(QDir().mkpath(dir_.path() + "/" + folder + "/" + s));
    }
    void drop(const QString& folder, const QString& name) {
        QFile f(dir_.path() + "/" + folder + "/new/" + name);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("Subject: x\n\nx\n");
    }
private slots:
    void testFindsNewDirsRecursively() {
        mk("INBOX"); mk("Archive/2024"); QDir().mkpath(dir_.path() + "/NoMaildir/x");
        const QStringList l = MailWatcher::findNewDirs(dir_.path());
        QCOMPARE(l.size(), 2);
        QVERIFY(l.contains(dir_.path() + "/INBOX/new"));
        QVERIFY(l.contains(dir_.path() + "/Archive/2024/new"));
    }
    void testBurstGivesOneSignal() {
        mk("INBOX");
        MailWatcher w;
        w.setDebounce(150);
        w.setRoots({dir_.path()});
        QCOMPARE(w.watchedCount(), 2); // INBOX/new e Archive/2024/new (creata dal test precedente)
        QSignalSpy sp(&w, &MailWatcher::newMail);
        drop("INBOX", "a"); drop("INBOX", "b"); drop("INBOX", "c");
        QTRY_COMPARE_WITH_TIMEOUT(sp.count(), 1, 3000);
        QTest::qWait(400);
        QCOMPARE(sp.count(), 1);
    }
    void testNewFolderIsPickedUp() {
        MailWatcher w;
        w.setDebounce(100);
        w.setRoots({dir_.path()});
        const int before = w.watchedCount();
        QSignalSpy sp(&w, &MailWatcher::newMail);
        mk("Nuova");
        drop("INBOX", "d"); // fa partire il giro che rilegge l'albero
        QTRY_COMPARE_WITH_TIMEOUT(sp.count(), 1, 3000);
        QCOMPARE(w.watchedCount(), before + 1);
        sp.clear();
        drop("Nuova", "e");
        QTRY_COMPARE_WITH_TIMEOUT(sp.count(), 1, 3000);
    }
    void testChangingRootsStopsWatchingOldOnes() {
        MailWatcher w;
        w.setRoots({dir_.path()});
        QVERIFY(w.watchedCount() > 0);
        w.setRoots({});
        QCOMPARE(w.watchedCount(), 0);
    }
};
QTEST_MAIN(T)
#include "test_mailwatcher.moc"
