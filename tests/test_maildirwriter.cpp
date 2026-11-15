#include <QtTest>
#include "core/MaildirWriter.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    static void mk(const QString& base) {
        for (auto s : {"tmp", "new", "cur"})
            QVERIFY(QDir().mkpath(base + "/" + s));
    }
private slots:
    void testDeliverCreatesFileAtomicallyAndNeverOverwrites() {
        QTemporaryDir td(QDir::currentPath() + "/maildirXXXXXX");
        QVERIFY(td.isValid());
        const QString folder = td.path() + "/Sent";
        mk(folder);
        const QByteArray msg = "From: a@b.c\nSubject: x\n\nbody\n";
        QSet<QString> seen;
        for (int i = 0; i < 1000; ++i) {
            QString err;
            const QString p = MaildirWriter::deliver(folder, msg, "SR", &err);
            QVERIFY2(!p.isEmpty(), qPrintable(err));
            QVERIFY(p.endsWith(":2,RS"));
            QVERIFY(!seen.contains(p));
            seen.insert(p);
        }
        QCOMPARE(seen.size(), 1000);
        const auto files = QDir(folder + "/cur").entryList(QDir::Files);
        QCOMPARE(files.size(), 1000);
        for (const QString& f : files) {
            QFile file(folder + "/cur/" + f);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), msg);
        }
        QVERIFY(QDir(folder + "/tmp").entryList(QDir::Files).isEmpty());
        QVERIFY(QDir(folder + "/new").entryList(QDir::Files).isEmpty());
    }
    void testDeliverToMissingFolderFailsWithoutCreatingJunk() {
        QTemporaryDir td(QDir::currentPath() + "/maildirXXXXXX");
        QString err;
        QVERIFY(MaildirWriter::deliver(td.path() + "/Nope", "x", "S", &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(QDir(td.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        // cartella con solo alcune sottocartelle: niente creato
        QVERIFY(QDir().mkpath(td.path() + "/Half/cur"));
        err.clear();
        QVERIFY(MaildirWriter::deliver(td.path() + "/Half", "x", "S", &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(!QFileInfo::exists(td.path() + "/Half/tmp"));
        QVERIFY(!QFileInfo::exists(td.path() + "/Half/new"));
        QVERIFY(QDir(td.path() + "/Half/cur").entryList(QDir::Files).isEmpty());
    }
    void testDiscardRemovesDeliveredMessage() {
        QTemporaryDir td(QDir::currentPath() + "/maildirXXXXXX");
        const QString folder = td.path() + "/Drafts";
        mk(folder);
        QString err;
        const QString a = MaildirWriter::deliver(folder, "x", "D", &err);
        const QString b = MaildirWriter::deliver(folder, "y", "D", &err);
        QVERIFY(MaildirWriter::discard(a, &err));
        QVERIFY(!QFileInfo::exists(a));
        QVERIFY(QFileInfo::exists(b)); // solo quello indicato
        QVERIFY(MaildirWriter::discard(a, &err)); // gia' assente: non e' un errore
        QVERIFY(QFileInfo::exists(b));
    }
    // mbsync rinomina il file (aggiunge ,U=n e cambia i flag): la bozza si trova ancora.
    void testDiscardFindsFileRenamedBySync() {
        QTemporaryDir td(QDir::currentPath() + "/maildirXXXXXX");
        const QString folder = td.path() + "/Drafts";
        mk(folder);
        QString err;
        const QString a = MaildirWriter::deliver(folder, "x", "D", &err);
        const QString other = MaildirWriter::deliver(folder, "y", "D", &err);
        const QFileInfo fi(a);
        const QString unique = fi.fileName().left(fi.fileName().indexOf(':'));
        QVERIFY(QFile::rename(a, folder + "/cur/" + unique + ",U=42:2,DS"));
        QVERIFY(MaildirWriter::discard(a, &err));
        QCOMPARE(QDir(folder + "/cur").entryList(QDir::Files).size(), 1);
        QVERIFY(QFileInfo::exists(other));
    }
    void testDiscardRefusesPathsOutsideMaildir() {
        QTemporaryDir td(QDir::currentPath() + "/maildirXXXXXX");
        QFile f(td.path() + "/importante.txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QString err;
        QVERIFY(!MaildirWriter::discard(f.fileName(), &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(f.exists());
        QVERIFY(!MaildirWriter::discard(QString(), &err));
    }
    void testDeliverRejectsInvalidFlags() {
        QTemporaryDir td(QDir::currentPath() + "/maildirXXXXXX");
        const QString folder = td.path() + "/Sent";
        mk(folder);
        for (const QString& f : {QString("S/"), QString("a,b"), QString("S1"), QString("S R"), QString("\xc3\xa8")}) {
            QString err;
            QVERIFY2(MaildirWriter::deliver(folder, "x", f, &err).isEmpty(), qPrintable(f));
            QVERIFY(!err.isEmpty());
        }
        for (auto s : {"tmp", "new", "cur"})
            QVERIFY(QDir(folder + "/" + s).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        QString err;
        const QString p = MaildirWriter::deliver(folder, "x", "RSR", &err);
        QVERIFY2(p.endsWith(":2,RS"), qPrintable(p + err));
    }
    void testDeliverRetriesOnNameCollision() {
        QTemporaryDir td(QDir::currentPath() + "/maildirXXXXXX");
        const QString folder = td.path() + "/Sent";
        mk(folder);
        { QFile f(folder + "/cur/fixed:2,S"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("ORIGINALE"); }
        int calls = 0;
        MaildirWriter::setNameGeneratorForTest([&]() { return calls++ == 0 ? QString("fixed") : QString("other%1").arg(calls); });
        QString err;
        const QString p = MaildirWriter::deliver(folder, "nuovo", "S", &err);
        MaildirWriter::setNameGeneratorForTest(nullptr);
        QVERIFY2(!p.isEmpty(), qPrintable(err));
        QVERIFY(!p.endsWith("/fixed:2,S"));
        QFile o(folder + "/cur/fixed:2,S"); QVERIFY(o.open(QIODevice::ReadOnly));
        QCOMPARE(o.readAll(), QByteArray("ORIGINALE"));
        QFile n(p); QVERIFY(n.open(QIODevice::ReadOnly));
        QCOMPARE(n.readAll(), QByteArray("nuovo"));
        QVERIFY(QDir(folder + "/tmp").entryList(QDir::Files).isEmpty());
    }
};
QTEST_MAIN(T)
#include "test_maildirwriter.moc"
