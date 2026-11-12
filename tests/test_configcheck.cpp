#include <QtTest>
#include <QTemporaryDir>
#include "core/ConfigCheck.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
private slots:
    void testMbsyncChannelsAndGroups() {
        const QByteArray rc =
            "# Channel commentato\nIMAPAccount a\nChannel personale\nFar :x:\n  channel   Lavoro \n"
            "Group tutti\nChannels personale\nChannelX nonvale\n";
        const QStringList ch = ConfigCheck::mbsyncChannels(rc);
        QVERIFY(ch.contains("personale"));
        QVERIFY(ch.contains("Lavoro"));
        QVERIFY(ch.contains("tutti"));
        QVERIFY(!ch.contains("nonvale"));
        QVERIFY(!ch.contains("commentato"));
    }
    void testMsmtpAccounts() {
        const QByteArray rc =
            "defaults\ntls on\naccount personale\nhost smtp.x\naccount lavoro : personale\n"
            "account default : personale\n# account finto\n";
        const QStringList a = ConfigCheck::msmtpAccounts(rc);
        QCOMPARE(a, (QStringList{"personale", "lavoro"}));
    }
    void testCheckAccountReportsMissingChannel() {
        QTemporaryDir d;
        Account a; a.id = "a"; a.maildirRoot = d.path(); a.mbsyncChannel = "manca";
        a.sentFolder = "Sent"; a.draftsFolder = "Drafts"; a.trashFolder = "Trash";
        auto l = ConfigCheck::checkAccount(a, {"altro"}, true);
        QVERIFY(l.first().level == CheckIssue::Error);
        l = ConfigCheck::checkAccount(a, {"manca"}, true);
        QVERIFY(l.first().level == CheckIssue::Ok);
        l = ConfigCheck::checkAccount(a, {}, false);
        QVERIFY(l.first().level == CheckIssue::Warning); // file mbsync assente: non e' un errore
    }
    void testCheckAccountFolders() {
        QTemporaryDir d;
        Account a; a.id = "a"; a.maildirRoot = d.path(); a.mbsyncChannel = "c";
        a.sentFolder = "Sent"; a.draftsFolder = "Drafts"; a.trashFolder = "Trash";
        auto warnings = [](const QList<CheckIssue>& l) { int n = 0; for (const auto& c : l) n += c.level == CheckIssue::Warning; return n; };
        QCOMPARE(warnings(ConfigCheck::checkAccount(a, {"c"}, true)), 3); // nessuna cartella speciale
        for (const char* f : {"Sent", "Drafts", "Trash"})
            for (const char* s : {"cur", "new", "tmp"}) QDir().mkpath(d.path() + "/" + f + "/" + s);
        QCOMPARE(warnings(ConfigCheck::checkAccount(a, {"c"}, true)), 0);
        a.maildirRoot = d.path() + "/nonesiste";
        const auto l = ConfigCheck::checkAccount(a, {"c"}, true);
        QCOMPARE(warnings(l), 1); // la crea mbsync: solo un avviso
    }
    void testCheckIdentity() {
        Identity i; i.id = "i"; i.address = "a@b.it"; i.msmtpAccount = "x"; i.signByDefault = true;
        auto levels = [](const QList<CheckIssue>& l) { QList<int> r; for (const auto& c : l) r << c.level; return r; };
        // account msmtp mancante + firma senza chiave
        auto l = ConfigCheck::checkIdentity(i, {"y"}, true);
        QVERIFY(levels(l).contains(CheckIssue::Error));
        QVERIFY(levels(l).contains(CheckIssue::Warning));
        i.pgpKeyId = "K";
        l = ConfigCheck::checkIdentity(i, {"x"}, true);
        QCOMPARE(levels(l), (QList<int>{CheckIssue::Ok}));
        i.msmtpAccount.clear();
        QVERIFY(levels(ConfigCheck::checkIdentity(i, {"x"}, true)).contains(CheckIssue::Error));
    }
    void testReadFileMissing() {
        bool ok = true;
        QVERIFY(ConfigCheck::readFile("/nonexistent/file", &ok).isEmpty());
        QVERIFY(!ok);
    }
};
QTEST_APPLESS_MAIN(T)
#include "test_configcheck.moc"
