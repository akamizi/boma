#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "core/Config.h"

using namespace boma;

class TestConfig : public QObject {
    Q_OBJECT
    static QString write(const QTemporaryDir& d, const QByteArray& body) {
        QString p = d.filePath("config.ini");
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write(body);
        return p;
    }
private slots:
    void testLoadsTwoAccountsWithIdentities() {
        QTemporaryDir d;
        QString p = write(d,
            "[account:a]\nname=Lavoro\nmaildirRoot=~/Mail/a\nmbsyncChannel=a\n"
            "sentFolder=Sent\ndraftsFolder=Drafts\ntrashFolder=Trash\n"
            "[account:b]\nname=Privato\nmaildirRoot=/home/x/Mail/b\nmbsyncChannel=b\n"
            "[identity:a1]\naccountId=a\naddress=mario@a.it\ndisplayName=Mario\nmsmtpAccount=a\npgpKeyId=ABC\n"
            "[identity:a2]\naccountId=a\naddress=info@a.it\nsignByDefault=false\n"
            "[identity:b1]\naccountId=b\naddress=m@b.it\n");
        QString err;
        Config c = Config::load(p, &err);
        QVERIFY(err.isEmpty());
        QVERIFY(!c.isEmpty());
        QCOMPARE(c.accounts().size(), 2);
        QCOMPARE(c.accounts().at(0).id, QString("a"));
        QCOMPARE(c.accounts().at(0).maildirRoot, QDir::homePath() + "/Mail/a");
        QCOMPARE(c.identities("a").size(), 2);
        QCOMPARE(c.identities("b").size(), 1);
        QCOMPARE(c.identities("b").at(0).address, QString("m@b.it"));
        QVERIFY(c.identities("b").at(0).signByDefault);
        QVERIFY(!c.identities("a").at(1).signByDefault);
        QCOMPARE(c.identityForAddress("MARIO@a.it").id, QString("a1"));
        QVERIFY(c.identityForAddress("nobody@x.it").id.isEmpty());
    }
    void testMissingFileGivesEmptyConfig() {
        QTemporaryDir d;
        QString err;
        Config c = Config::load(d.filePath("nope.ini"), &err);
        QVERIFY(c.isEmpty());
        QVERIFY(err.isEmpty());
    }
    void testIdentityWithUnknownAccountIsRejected() {
        QTemporaryDir d;
        QString p = write(d,
            "[account:a]\nname=A\n[identity:x]\naccountId=zzz\naddress=x@x.it\n"
            "[identity:ok]\naccountId=a\naddress=ok@a.it\n");
        QString err;
        Config c = Config::load(p, &err);
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains("zzz"));
        QCOMPARE(c.identities("a").size(), 1);
        QVERIFY(c.identityForAddress("x@x.it").id.isEmpty());
    }
    void testSignatureWithCommaIsPreserved() {
        QTemporaryDir d;
        QString p = write(d,
            "[account:a]\nname=A\n[identity:i]\naccountId=a\naddress=i@a.it\n"
            "signature=Mario Rossi, Dirigente\n");
        Config c = Config::load(p, nullptr);
        QCOMPARE(c.identities("a").at(0).signature, QString("Mario Rossi, Dirigente"));
    }
    void testSignatureWithEscapedNewlines() {
        QTemporaryDir d;
        QString p = write(d,
            "[account:a]\nname=A\n[identity:i]\naccountId=a\naddress=i@a.it\n"
            "signature=-- \\nMario\\nAkabit\n");
        Config c = Config::load(p, nullptr);
        QCOMPARE(c.identities("a").at(0).signature, QString("-- \nMario\nAkabit"));
    }
    void testSaveMakesBackupOfPreviousFile() {
        QTemporaryDir d;
        QString p = write(d, "[account:a]\nname=Prima\nmaildirRoot=/m/a\nmbsyncChannel=a\n");
        Config c = Config::load(p, nullptr);
        QList<Account> acc = c.accounts();
        acc[0].name = "Dopo";
        c.setAccounts(acc);
        QString err;
        QVERIFY2(c.save(p, &err), qPrintable(err));
        QFile bak(p + ".bak");
        QVERIFY(bak.open(QIODevice::ReadOnly));
        QVERIFY(bak.readAll().contains("Prima"));
        QCOMPARE(Config::load(p, nullptr).accounts().at(0).name, QString("Dopo"));
    }
    void testSaveThenLoadGivesSameData() {
        QTemporaryDir d;
        QString p = d.filePath("config.ini");
        Config c;
        Account a;
        a.id = "a"; a.name = "A"; a.maildirRoot = QDir::homePath() + "/Mail/a"; a.mbsyncChannel = "ca";
        a.sentFolder = "Sent"; a.draftsFolder = "Drafts"; a.trashFolder = "Trash";
        c.setAccounts({a});
        Identity i;
        i.id = "i"; i.accountId = "a"; i.address = "i@a.it"; i.displayName = "I";
        i.signature = "-- \nMario, Akabit"; i.pgpKeyId = "K"; i.msmtpAccount = "m"; i.signByDefault = false;
        c.setIdentities({i});
        QString err;
        QVERIFY2(c.save(p, &err), qPrintable(err));
        QFile f(p); QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("maildirRoot=~/Mail/a")); // sotto la home: scritto con "~"
        Config r = Config::load(p, &err);
        QVERIFY(err.isEmpty());
        QCOMPARE(r.accounts().size(), 1);
        QCOMPARE(r.accounts().at(0).maildirRoot, a.maildirRoot);
        QCOMPARE(r.accounts().at(0).mbsyncChannel, QString("ca"));
        QCOMPARE(r.identities("a").size(), 1);
        const Identity g = r.identities("a").at(0);
        QCOMPARE(g.signature, i.signature);
        QCOMPARE(g.pgpKeyId, QString("K"));
        QCOMPARE(g.msmtpAccount, QString("m"));
        QVERIFY(!g.signByDefault);
    }
    void testSaveKeepsGeneralAndRemovesDeletedAccounts() {
        QTemporaryDir d;
        QString p = write(d,
            "[general]\nmuRoot=/srv/mail\n[account:a]\nname=A\nmaildirRoot=/m/a\nmbsyncChannel=a\n"
            "[account:old]\nname=Old\nmaildirRoot=/m/o\nmbsyncChannel=o\n"
            "[identity:i]\naccountId=a\naddress=i@a.it\n[identity:o]\naccountId=old\naddress=o@o.it\n");
        Config c = Config::load(p, nullptr);
        QList<Account> acc;
        for (const Account& a : c.accounts()) if (a.id != "old") acc << a;
        QList<Identity> ids;
        for (const Identity& i : c.identities()) if (i.accountId != "old") ids << i;
        c.setAccounts(acc);
        c.setIdentities(ids);
        QVERIFY(c.save(p));
        Config r = Config::load(p, nullptr);
        QCOMPARE(r.muRoot(), QString("/srv/mail"));
        QCOMPARE(r.accounts().size(), 1);
        QCOMPARE(r.identities("old").size(), 0);
    }
    void testValidateRejectsBadConfigs() {
        Config c;
        Account a; a.id = "a"; a.maildirRoot = "/m"; a.mbsyncChannel = "c";
        Account dup = a;
        c.setAccounts({a, dup});
        QVERIFY(!c.validate().isEmpty()); // id duplicato
        Account bad = a; bad.id = "a/b";
        c.setAccounts({bad});
        QVERIFY(!c.validate().isEmpty()); // carattere non ammesso
        Account noRoot = a; noRoot.maildirRoot.clear();
        c.setAccounts({noRoot});
        QVERIFY(!c.validate().isEmpty());
        c.setAccounts({a});
        Identity i; i.id = "i"; i.accountId = "zzz"; i.address = "x@y.it";
        c.setIdentities({i});
        QVERIFY(!c.validate().isEmpty()); // account sconosciuto
        i.accountId = "a"; i.address.clear();
        c.setIdentities({i});
        QVERIFY(!c.validate().isEmpty()); // senza indirizzo
        i.address = "x@y.it";
        c.setIdentities({i});
        QVERIFY(c.validate().isEmpty());
        QString err;
        QTemporaryDir d;
        c.setIdentities({});
        c.setAccounts({dup, dup});
        QVERIFY(!c.save(d.filePath("x.ini"), &err));
        QVERIFY(!QFile::exists(d.filePath("x.ini"))); // niente file se non valida
    }
};

QTEST_APPLESS_MAIN(TestConfig)
#include "test_config.moc"
