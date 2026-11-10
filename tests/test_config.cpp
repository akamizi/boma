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
};

QTEST_APPLESS_MAIN(TestConfig)
#include "test_config.moc"
