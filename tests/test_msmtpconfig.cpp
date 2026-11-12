#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include "core/MsmtpConfig.h"
#include "ui/MsmtpDialog.h"
using namespace boma;

static void only600(const QString& p) {
    const auto perm = QFileInfo(p).permissions();
    QVERIFY(perm.testFlag(QFile::ReadOwner) && perm.testFlag(QFile::WriteOwner));
    QVERIFY(!(perm & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther | QFile::ExeOwner)));
}

static const QByteArray kRc =
    "# mio file\ndefaults\nauth on\ntls on\ntls_starttls on\nlogfile ~/.msmtp.log\n\n"
    "account personale\nhost smtp.a.it\nport 587\nfrom a@a.it\nuser a@a.it\npasswordeval cat ~/.pw\n\n"
    "account lavoro\nhost smtp.l.it\n# nota\nuser l@l.it\n\naccount default : personale\n";

class T : public QObject {
    Q_OBJECT
    QTemporaryDir dir{QDir::currentPath() + "/msm-XXXXXX"};
private slots:
    void testReadSettings() {
        const auto a = MsmtpConfig::accountSettings(kRc, "personale");
        QCOMPARE(a.value("host"), QString("smtp.a.it"));
        QCOMPARE(a.value("passwordeval"), QString("cat ~/.pw"));
        QVERIFY(!a.contains("tls")); // quello e' in [defaults]
        QCOMPARE(MsmtpConfig::defaultSettings(kRc).value("tls"), QString("on"));
        QVERIFY(MsmtpConfig::hasAccount(kRc, "lavoro"));
        QVERIFY(!MsmtpConfig::hasAccount(kRc, "zzz"));
    }
    void testEditChangesOnlyThatBlock() {
        QString err;
        const QByteArray out = MsmtpConfig::applyAccount(kRc, "lavoro", {{"host", "nuovo.l.it"}, {"port", "465"}, {"user", ""}}, &err);
        QVERIFY2(!out.isNull(), qPrintable(err));
        const auto l = MsmtpConfig::accountSettings(out, "lavoro");
        QCOMPARE(l.value("host"), QString("nuovo.l.it"));
        QCOMPARE(l.value("port"), QString("465"));
        QVERIFY(!l.contains("user"));
        QVERIFY(out.contains("# nota"));    // commenti intatti
        QVERIFY(out.contains("# mio file"));
        QCOMPARE(MsmtpConfig::accountSettings(out, "personale"), MsmtpConfig::accountSettings(kRc, "personale"));
        QCOMPARE(MsmtpConfig::defaultSettings(out), MsmtpConfig::defaultSettings(kRc));
        QVERIFY(out.contains("account default : personale"));
        // senza modifiche il testo e' identico
        QCOMPARE(MsmtpConfig::applyAccount(kRc, "personale", {{"host", "smtp.a.it"}}, nullptr), kRc);
    }
    void testNewAccountIsAppended() {
        const QByteArray out = MsmtpConfig::applyAccount(kRc, "nuovo", {{"host", "smtp.n.it"}, {"passwordeval", "pass show n"}}, nullptr);
        QVERIFY(MsmtpConfig::hasAccount(out, "nuovo"));
        QCOMPARE(MsmtpConfig::accountSettings(out, "nuovo").value("passwordeval"), QString("pass show n"));
        QVERIFY(out.startsWith(kRc.left(kRc.size() - 1))); // il resto del file non cambia
    }
    void testRejectsBadInput() {
        QString err;
        QVERIFY(MsmtpConfig::applyAccount(kRc, "a b", {}, &err).isNull());
        QVERIFY(MsmtpConfig::applyAccount(kRc, "personale", {{"host", "x\nhost evil"}}, &err).isNull());
        QVERIFY(MsmtpConfig::applyAccount(kRc, "personale", {{"password", "segreta"}}, &err).isNull()); // mai password in chiaro
        QVERIFY(MsmtpConfig::applyAccount(kRc, "default", {}, &err).isNull());
    }
    void testSaveKeepsPermissionsAndBackup() {
        const QString p = dir.filePath("msmtprc");
        { QFile f(p); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(kRc); }
        QFile::setPermissions(p, QFile::ReadOwner | QFile::WriteOwner);
        QString err;
        QVERIFY2(MsmtpConfig::save(p, "account x\nhost h\n", &err), qPrintable(err));
        only600(p);
        QFile b(p + ".bak"); QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), kRc);
        only600(p + ".bak");
        // file nuovo: 0600
        const QString n = dir.filePath("nuovo");
        QVERIFY(MsmtpConfig::save(n, "account y\n", &err));
        only600(n);
    }
    void testDialogWritesFile() {
        const QString p = dir.filePath("dlg");
        { QFile f(p); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(kRc); }
        MsmtpDialog d(p, "personale");
        QCOMPARE(d.hostEdit()->text(), QString("smtp.a.it"));
        d.hostEdit()->setText("altro.a.it");
        d.portEdit()->setText("99999");
        d.accept(); // porta non valida: niente scrittura
        QVERIFY(!d.errorLabel()->text().isEmpty());
        QVERIFY(!QFile::exists(p + ".bak"));
        d.portEdit()->setText("465");
        d.accept();
        QFile f(p); QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray out = f.readAll();
        QCOMPARE(MsmtpConfig::accountSettings(out, "personale").value("host"), QString("altro.a.it"));
        QCOMPARE(MsmtpConfig::accountSettings(out, "personale").value("port"), QString("465"));
        QVERIFY(!out.contains("evil"));
    }
};
QTEST_MAIN(T)
#include "test_msmtpconfig.moc"
