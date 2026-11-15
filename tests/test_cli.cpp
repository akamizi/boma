#include <QtTest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include "core/Version.h"

using namespace boma;

class T : public QObject {
    Q_OBJECT
    // Ambiente spoglio: nessun display, HOME vuota, PATH senza mu/msmtp/mbsync.
    static QProcessEnvironment bareEnv(const QString& home) {
        QProcessEnvironment e;
        e.insert(QStringLiteral("HOME"), home);
        e.insert(QStringLiteral("PATH"), home); // cartella vuota
        return e;
    }
    static QString run(const QStringList& args, int* code, QString* err, const QString& home) {
        QProcess p;
        p.setProcessEnvironment(bareEnv(home));
        p.start(QStringLiteral(BOMA_EXE), args);
        if (!p.waitForFinished(15000)) { p.kill(); *code = -99; return {}; }
        *code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        if (err) *err = QString::fromUtf8(p.readAllStandardError());
        return QString::fromUtf8(p.readAllStandardOutput());
    }
private slots:
    void testVersionMatchesCMakeAndNeedsNoDisplay() {
        QTemporaryDir home;
        for (const char* opt : {"--version", "-V"}) {
            int code = -1;
            QString err;
            const QString out = run({QString::fromLatin1(opt)}, &code, &err, home.path());
            QCOMPARE(code, 0);
            QVERIFY2(err.isEmpty(), qPrintable(err));
            QCOMPARE(out, versionLine() + QLatin1Char('\n')); // una riga sola
            QVERIFY(out.startsWith(QStringLiteral("boma ") + QStringLiteral(BOMA_VERSION)));
        }
        QCOMPARE(versionNumber(), QStringLiteral(BOMA_VERSION));
        // non ha creato configurazione ne' cartelle in HOME
        QVERIFY(QDir(home.path()).isEmpty());
    }
    void testHelpListsOnlyRealOptions() {
        QTemporaryDir home;
        int code = -1;
        const QString out = run({QStringLiteral("--help")}, &code, nullptr, home.path());
        QCOMPARE(code, 0);
        QCOMPARE(out, helpText());
        QVERIFY(out.contains(QStringLiteral("--version")));
    }
    void testVersionLineFormat() {
        const QRegularExpression re(QStringLiteral("^boma \\d+\\.\\d+\\.\\d+( \\([0-9a-f]{7,40}\\))?$"));
        QVERIFY2(re.match(versionLine()).hasMatch(), qPrintable(versionLine()));
    }
};
QTEST_APPLESS_MAIN(T)
#include "test_cli.moc"
