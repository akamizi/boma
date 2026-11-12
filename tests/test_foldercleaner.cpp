#include <QtTest>
#include <QTemporaryDir>
#include "core/FolderCleaner.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    QTemporaryDir dir{QDir::currentPath() + "/fc-XXXXXX"};
    static void touch(const QString& p) { QDir().mkpath(QFileInfo(p).path()); QFile f(p); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
    QString root() const { return dir.path() + "/acc"; }
private slots:
    void testRemovesOnlyCurAndNew() {
        const QString f = root() + "/Trash";
        for (int i = 0; i < 3; ++i) touch(f + QString("/cur/m%1:2,S").arg(i));
        touch(f + "/new/n1");
        touch(f + "/tmp/t1");                 // tmp: mai toccato
        touch(f + "/Sotto/cur/s1");           // sottocartella: non toccata
        touch(root() + "/fuori");             // file fuori
        QVERIFY(QFile::link(root() + "/fuori", f + "/cur/link")); // link: non seguito, non rimosso
        QCOMPARE(FolderCleaner::count(f), 4);
        int failed = -1; QString err;
        QCOMPARE(FolderCleaner::removeAll(f, root(), &failed, &err), 4);
        QCOMPARE(failed, 0);
        QVERIFY(QFile::exists(f + "/tmp/t1"));
        QVERIFY(QFile::exists(f + "/Sotto/cur/s1"));
        QVERIFY(QFile::exists(root() + "/fuori"));
        QVERIFY(QFileInfo(f + "/cur/link").isSymLink());
        QVERIFY(QDir(f + "/cur").exists());   // le cartelle restano
    }
    void testRefusesBadFolders() {
        touch(root() + "/Altra/cur/a");
        QVERIFY(!FolderCleaner::validate(root(), root()).isEmpty());                 // la radice stessa
        QVERIFY(!FolderCleaner::validate(dir.path(), root()).isEmpty());             // fuori dalla radice
        QVERIFY(!FolderCleaner::validate(root() + "/Altra/../..", root()).isEmpty()); // percorso con ..
        QDir().mkpath(root() + "/NonMaildir");
        QVERIFY(!FolderCleaner::validate(root() + "/NonMaildir", root()).isEmpty()); // senza cur/
        QVERIFY(!FolderCleaner::validate(root() + "/non/esiste", root()).isEmpty());
        QVERIFY(FolderCleaner::validate(root() + "/Altra", root()).isEmpty());
        int failed; QString err;
        QCOMPARE(FolderCleaner::removeAll(dir.path(), root(), &failed, &err), 0);
        QVERIFY(QFile::exists(root() + "/Altra/cur/a"));
        // link simbolico a una cartella fuori dalla radice
        QVERIFY(QFile::link(dir.path(), root() + "/Esca"));
        QVERIFY(!FolderCleaner::validate(root() + "/Esca", root()).isEmpty());
    }
};
QTEST_APPLESS_MAIN(T)
#include "test_foldercleaner.moc"
