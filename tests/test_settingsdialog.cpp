#include <QtTest>
#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include "ui/SettingsDialog.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    QTemporaryDir dir{QDir::currentPath() + "/sdt-XXXXXX"};
    static void put(const QString& p, const QByteArray& b) { QFile f(p); (void)f.open(QIODevice::WriteOnly); f.write(b); }
    SettingsEnvironment env() {
        put(dir.filePath("mbsyncrc"), "Channel personale\nChannel lavoro\n");
        put(dir.filePath("msmtprc"), "account personale\naccount lavoro\n");
        SettingsEnvironment e;
        e.mbsyncConfig = dir.filePath("mbsyncrc");
        e.msmtpConfig = dir.filePath("msmtprc");
        e.gpgProgram = "/nonexistent/gpg";
        return e;
    }
    Config sample() {
        Config c;
        Account a; a.id = "personale"; a.name = "Personale"; a.maildirRoot = dir.path() + "/m/p"; a.mbsyncChannel = "personale";
        a.sentFolder = "Sent"; a.draftsFolder = "Drafts"; a.trashFolder = "Trash";
        c.setAccounts({a});
        Identity i; i.id = "principale"; i.accountId = "personale"; i.address = "io@x.it"; i.displayName = "Io"; i.msmtpAccount = "personale";
        c.setIdentities({i});
        return c;
    }
private slots:
    void testShowsAndReturnsTheSameConfig() {
        SettingsDialog d(sample(), env());
        QCOMPARE(d.accountList()->count(), 1);
        QCOMPARE(d.identityList()->count(), 1);
        QCOMPARE(d.addressEdit()->text(), QString("io@x.it"));
        QCOMPARE(d.channelCombo()->currentText(), QString("personale"));
        QVERIFY(!d.accountIdEdit()->isEnabled()); // account esistente: id fisso
        QVERIFY(!d.identityIdEdit()->isEnabled());
        const Config r = d.result();
        QCOMPARE(r.accounts().size(), 1);
        QCOMPARE(r.identities("personale").at(0).address, QString("io@x.it"));
    }
    void testEditAccountAndIdentity() {
        SettingsDialog d(sample(), env());
        d.accountNameEdit()->setText("Casa");
        d.addressEdit()->setText("nuovo@x.it");
        d.msmtpCombo()->setEditText("lavoro");
        const Config r = d.result();
        QCOMPARE(r.accounts().at(0).name, QString("Casa"));
        QCOMPARE(r.identities("personale").at(0).address, QString("nuovo@x.it"));
        QCOMPARE(r.identities("personale").at(0).msmtpAccount, QString("lavoro"));
        QCOMPARE(d.accountList()->item(0)->text(), QString("Casa (personale)"));
    }
    void testThemeIsSavedOnAccept() {
        SettingsDialog d(sample(), env());
        QVERIFY(d.styleCombo()->count() >= 2);
        d.schemeCombo()->setCurrentIndex(d.schemeCombo()->findData("dark"));
        d.styleCombo()->setCurrentIndex(d.styleCombo()->findData("Fusion"));
        d.accept();
        QCOMPARE(d.result().accounts().size(), 1);
        const Theme t = Theme::load();
        QCOMPARE(t.scheme, QString("dark"));
        QCOMPARE(t.style, QString("Fusion"));
        Theme().save(); // ripristina
    }
    void testChannelAndMsmtpListsComeFromFiles() {
        SettingsDialog d(sample(), env());
        QStringList ch, ms;
        for (int i = 0; i < d.channelCombo()->count(); ++i) ch << d.channelCombo()->itemText(i);
        for (int i = 0; i < d.msmtpCombo()->count(); ++i) ms << d.msmtpCombo()->itemText(i);
        QVERIFY(ch.contains("lavoro") && ch.contains("personale"));
        QVERIFY(ms.contains("lavoro") && ms.contains("personale"));
    }
    void testAddAndRemoveAccountWithIdentities() {
        SettingsDialog d(sample(), env());
        d.addAccountButton()->click();
        QCOMPARE(d.accountList()->count(), 2);
        QVERIFY(d.accountIdEdit()->isEnabled()); // nuovo: id modificabile
        d.accountIdEdit()->setText("lavoro");
        d.rootEdit()->setText(dir.path() + "/m/l");
        d.channelCombo()->setEditText("lavoro");
        d.addIdentityButton()->click();
        QCOMPARE(d.identityList()->count(), 1);
        d.addressEdit()->setText("l@x.it");
        Config r = d.result();
        QCOMPARE(r.accounts().size(), 2);
        QCOMPARE(r.identities("lavoro").size(), 1); // l'identita' segue il nuovo id
        QCOMPARE(r.identities("lavoro").at(0).address, QString("l@x.it"));
        QVERIFY(r.validate().isEmpty());
    }
};
int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true); // QSettings di prova, non quelle dell'utente
    QApplication app(argc, argv);
    QApplication::setOrganizationName("BomaTest");
    QApplication::setApplicationName("BomaTest");
    T t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_settingsdialog.moc"
