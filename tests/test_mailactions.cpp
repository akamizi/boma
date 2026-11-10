#include <QtTest>
#include "core/MailActions.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    static int countFiles(const QString& root) {
        int n = 0;
        QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { it.next(); ++n; }
        return n;
    }
    static QString script(const QString& dir, const QString& name, const QString& body) {
        const QString p = dir + "/" + name;
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write("#!/bin/sh\n" + body.toUtf8() + "\n");
        f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
        return p;
    }
    struct Env {
        QTemporaryDir td{QDir::currentPath() + "/actionsXXXXXX"};
        QString mail, msg;
        Account acc;
        void init() {
            mail = td.path() + "/mail";
            for (auto s : {"INBOX/cur", "INBOX/new", "INBOX/tmp", "Trash/cur", "Trash/new", "Trash/tmp"})
                QDir().mkpath(mail + "/" + s);
            msg = mail + "/INBOX/cur/m1:2,S";
            QFile f(msg); f.open(QIODevice::WriteOnly); f.write("Subject: x\n\nhi\n"); f.close();
            acc.maildirRoot = mail; acc.trashFolder = "Trash";
        }
    };
private slots:
    void testMoveToTrashKeepsMessageCount() {
        Env e; e.init();
        // finto mu che esegue davvero lo spostamento: mu move <src> /Trash
        const QString mu = script(e.td.path(), "mu",
            "[ \"$1\" = move ] || exit 2\n"
            "mv \"$2\" \"" + e.mail + "$3/cur/\"");
        MailActions a(nullptr, mu);
        const int before = countFiles(e.mail);
        bool done = false, ok = false; QString err;
        a.moveToTrash(e.msg, e.acc, [&](bool o, QString m) { ok = o; err = m; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(countFiles(e.mail), before);
        QVERIFY(!QFileInfo::exists(e.msg));
        QCOMPARE(QDir(e.mail + "/Trash/cur").entryList(QDir::Files).size(), 1);
    }
    void testMoveToTrashFailureLeavesOriginalIntact() {
        Env e; e.init();
        const QString mu = script(e.td.path(), "mu", "echo boom >&2; exit 1");
        MailActions a(nullptr, mu);
        const int before = countFiles(e.mail);
        bool done = false, ok = true; QString err;
        a.moveToTrash(e.msg, e.acc, [&](bool o, QString m) { ok = o; err = m; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!ok);
        QVERIFY(err.contains("boom"));
        QVERIFY(QFileInfo::exists(e.msg));
        QCOMPARE(countFiles(e.mail), before);
        QFile f(e.msg); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("Subject: x\n\nhi\n"));
    }
    void testMarkReadUsesFlagsArgument() {
        Env e; e.init();
        const QString log = e.td.path() + "/args.txt";
        const QString mu = script(e.td.path(), "mu", "echo \"$@\" > \"" + log + "\"");
        MailActions a(nullptr, mu);
        bool done = false, ok = false; QString err;
        a.markRead(e.msg, [&](bool o, QString m) { ok = o; err = m; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(ok);
        QFile f(log); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(f.readAll()).trimmed(), "move " + e.msg + " --flags=-N+S");
        QVERIFY(QFileInfo::exists(e.msg));
    }
    void testMoveToTrashRefusesPathOutsideMaildirRoot() {
        Env e; e.init();
        const QString log = e.td.path() + "/calls.txt";
        const QString mu = script(e.td.path(), "mu", "echo x >> \"" + log + "\"");
        MailActions a(nullptr, mu);
        const QString outside = e.td.path() + "/other/INBOX/cur/m";
        QDir().mkpath(e.td.path() + "/other/INBOX/cur");
        { QFile f(outside); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
        for (const QString& p : {outside, e.mail + "/../other/INBOX/cur/m", QString("relativo/m"), QString(e.mail + "-x/m")}) {
            bool done = false, ok = true; QString err;
            a.moveToTrash(p, e.acc, [&](bool o, QString m) { ok = o; err = m; done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
            QVERIFY2(!ok, qPrintable(p));
            QVERIFY(!err.isEmpty());
        }
        QVERIFY(!QFileInfo::exists(log));
        QVERIFY(QFileInfo::exists(outside));
        // markRead richiede percorso assoluto
        bool done = false, ok = true;
        a.markRead("-rf", [&](bool o, QString) { ok = o; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!ok);
        QVERIFY(!QFileInfo::exists(log));
    }
    void testMoveToTrashRefusesEmptyOrDotDotTrashFolder() {
        Env e; e.init();
        const QString log = e.td.path() + "/calls.txt";
        const QString mu = script(e.td.path(), "mu", "echo x >> \"" + log + "\"");
        MailActions a(nullptr, mu);
        for (const QString& f : {QString(), QString("  "), QString("/"), QString(".."), QString("../x"),
                                 QString("a/../../b"), QString("/../etc")}) {
            Account acc = e.acc; acc.trashFolder = f;
            bool done = false, ok = true; QString err;
            a.moveToTrash(e.msg, acc, [&](bool o, QString m) { ok = o; err = m; done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
            QVERIFY2(!ok, qPrintable(f));
            QVERIFY(!err.isEmpty());
        }
        QVERIFY(!QFileInfo::exists(log));
        // forma valida con slash superflui: accettata e normalizzata
        const QString log2 = e.td.path() + "/args.txt";
        const QString mu2 = script(e.td.path(), "mu2", "echo \"$@\" > \"" + log2 + "\"");
        MailActions b(nullptr, mu2);
        Account acc = e.acc; acc.trashFolder = "//Trash/";
        bool done = false, ok = false;
        b.moveToTrash(e.msg, acc, [&](bool o, QString) { ok = o; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(ok);
        QFile lf(log2); QVERIFY(lf.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(lf.readAll()).trimmed(), "move " + e.msg + " /Trash");
    }
    void testNoActionEverDeletesFiles() {
        Env e; e.init();
        const int before = countFiles(e.mail);
        // mu finto che fallisce, mancante, o sposta: il totale non cala mai
        const QStringList programs = {
            script(e.td.path(), "mu1", "exit 1"),
            e.td.path() + "/inesistente",
            script(e.td.path(), "mu2", "mv \"$2\" \"" + e.mail + "$3/cur/\"")};
        for (const QString& p : programs) {
            MailActions a(nullptr, p);
            bool done = false;
            a.markRead(e.msg, [&](bool, QString) { done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
            QVERIFY(countFiles(e.mail) >= before);
            done = false;
            const QString cur = QDir(e.mail + "/INBOX/cur").entryList(QDir::Files).value(0);
            a.moveToTrash(cur.isEmpty() ? e.msg : e.mail + "/INBOX/cur/" + cur, e.acc,
                          [&](bool, QString) { done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
            QVERIFY(countFiles(e.mail) >= before);
        }
    }
};
QTEST_MAIN(T)
#include "test_mailactions.moc"
