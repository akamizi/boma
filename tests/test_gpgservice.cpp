#include "pgp/GpgService.h"
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <csignal>
#include <sys/stat.h>

using namespace boma;

namespace {

// GNUPGHOME sempre esplicito: mai ~/.gnupg ne' l'agent dell'utente.
struct GpgRun {
    int code = -1;
    QByteArray out, err;
};

GpgRun runGpg(const QString& home, const QStringList& args, const QByteArray& in = {}) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GNUPGHOME"), home);
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("gpg"), args);
    p.write(in);
    p.closeWriteChannel();
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished();
    }
    return {p.exitCode(), p.readAllStandardOutput(), p.readAllStandardError()};
}

void killAgent(const QString& home) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GNUPGHOME"), home);
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("gpgconf"), {QStringLiteral("--kill"), QStringLiteral("gpg-agent")});
    p.waitForFinished(10000);
}

QString writeScript(const QDir& dir, const QString& name, const QByteArray& body) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("script non scrivibile");
    f.write(body);
    f.close();
    f.setPermissions(QFile::permissions(path) | QFile::ExeOwner);
    return path;
}

struct VerifyOut {
    bool done = false;
    SignatureStatus st = SignatureStatus::None;
    QString who;
};
struct DecOut {
    bool done = false, ok = false;
    QByteArray plain;
    QString err;
};

VerifyOut doVerify(GpgService& s, const QByteArray& data, const QByteArray& sig) {
    VerifyOut o;
    s.verify(data, sig, [&o](SignatureStatus st, QString who) { o = {true, st, who}; });
    QTest::qWaitFor([&o] { return o.done; }, 60000);
    return o;
}
DecOut doDecrypt(GpgService& s, const QByteArray& enc) {
    DecOut o;
    s.decrypt(enc, [&o](bool ok, QByteArray p, QString e) { o = {true, ok, p, e}; });
    QTest::qWaitFor([&o] { return o.done; }, 60000);
    return o;
}

} // namespace

class T : public QObject {
    Q_OBJECT
    QTemporaryDir root_{QDir::currentPath() + QStringLiteral("/gpgt-XXXXXX")};
    QString home_, homeEmpty_, homePw_, homeR_;
    QStringList homes_;
    QByteArray data_ = "Content-Type: text/plain\r\n\r\nCiao mondo \r\n  riga con spazi finali   \r\n";
    QByteArray sigGood_, sigExpired_, sigRevoked_;
    QByteArray dataExpired_ = "dato firmato dalla chiave scaduta\r\n";
    QByteArray dataRevoked_ = "dato firmato dalla chiave revocata\r\n";

    QString newHome(const QString& name) {
        const QString h = root_.filePath(name);
        QDir().mkpath(h);
        chmod(h.toLocal8Bit().constData(), 0700);
        QFile f(h + QStringLiteral("/gpg-agent.conf"));
        f.open(QIODevice::WriteOnly);
        f.write("pinentry-program /bin/false\n");
        f.close();
        homes_ << h;
        return h;
    }
    void gen(const QString& home, const QString& uid, const QStringList& extra = {}, const QString& expire = QStringLiteral("never")) {
        QStringList a = extra;
        a << QStringLiteral("--batch") << QStringLiteral("--passphrase") << QString() << QStringLiteral("--quick-gen-key")
          << uid << QStringLiteral("default") << QStringLiteral("default") << expire;
        const GpgRun r = runGpg(home, a);
        if (r.code != 0) qFatal("generazione chiave fallita: %s", r.err.constData());
    }
    QByteArray sign(const QString& home, const QString& uid, const QByteArray& data, const QStringList& extra = {}) {
        QStringList a = extra;
        a << QStringLiteral("--batch") << QStringLiteral("-u") << uid << QStringLiteral("--detach-sign");
        const GpgRun r = runGpg(home, a, data);
        if (r.code != 0 || r.out.isEmpty()) qFatal("firma fallita: %s", r.err.constData());
        return r.out;
    }

private slots:
    void initTestCase() {
        QVERIFY2(root_.isValid(), "directory temporanea non creata");
        chmod(root_.path().toLocal8Bit().constData(), 0700);
        qInfo() << "GNUPGHOME di prova sotto" << root_.path();
        home_ = newHome(QStringLiteral("h"));
        homeEmpty_ = newHome(QStringLiteral("e"));
        gen(home_, QStringLiteral("Alice Test <alice@example.test>"));
        sigGood_ = sign(home_, QStringLiteral("alice@example.test"), data_);

        // chiave scaduta: creata e usata nel 2020 con scadenza a 1 giorno
        const QStringList fake{QStringLiteral("--faked-system-time"), QStringLiteral("20200101T000000")};
        gen(home_, QStringLiteral("Edna Expired <edna@example.test>"), fake, QStringLiteral("1d"));
        sigExpired_ = sign(home_, QStringLiteral("edna@example.test"), dataExpired_,
                           {QStringLiteral("--faked-system-time"), QStringLiteral("20200101T120000")});

        // chiave revocata
        gen(home_, QStringLiteral("Rob Revoked <rob@example.test>"));
        sigRevoked_ = sign(home_, QStringLiteral("rob@example.test"), dataRevoked_);
        const QString rev = root_.filePath(QStringLiteral("rev.asc"));
        GpgRun r = runGpg(home_, {QStringLiteral("--no-tty"), QStringLiteral("--yes"), QStringLiteral("--command-fd"),
                                  QStringLiteral("0"), QStringLiteral("--output"), rev, QStringLiteral("--gen-revoke"),
                                  QStringLiteral("rob@example.test")},
                          "y\n0\n\ny\n");
        QVERIFY2(r.code == 0, r.err.constData());
        r = runGpg(home_, {QStringLiteral("--batch"), QStringLiteral("--import"), rev});
        QVERIFY2(r.code == 0, r.err.constData());

        // chiave con password + pinentry finto
        homePw_ = newHome(QStringLiteral("p"));
        const QString pe = writeScript(QDir(root_.path()), QStringLiteral("pinentry-fake"),
                                       "#!/bin/sh\n"
                                       "echo called >> '" + root_.filePath(QStringLiteral("pinentry.called")).toUtf8() + "'\n"
                                       "echo 'OK Pleased to meet you'\n"
                                       "while read cmd rest; do\n"
                                       "  case \"$cmd\" in\n"
                                       "    GETPIN) echo 'D secretpw'; echo OK;;\n"
                                       "    BYE) echo OK; exit 0;;\n"
                                       "    *) echo OK;;\n"
                                       "  esac\n"
                                       "done\n");
        QFile f(homePw_ + QStringLiteral("/gpg-agent.conf"));
        f.open(QIODevice::WriteOnly);
        f.write("pinentry-program " + pe.toUtf8() + "\n");
        f.close();
        r = runGpg(homePw_, {QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"), QStringLiteral("loopback"),
                             QStringLiteral("--passphrase-fd"), QStringLiteral("0"), QStringLiteral("--quick-gen-key"),
                             QStringLiteral("Pia Password <pia@example.test>"), QStringLiteral("default"),
                             QStringLiteral("default"), QStringLiteral("never")},
                   "secretpw\n");
        QVERIFY2(r.code == 0, r.err.constData());
    }
    void cleanupTestCase() {
        for (const QString& h : std::as_const(homes_)) killAgent(h);
    }

    void testValidSignature() {
        GpgService s;
        s.setGnupgHome(home_);
        const auto o = doVerify(s, data_, sigGood_);
        QCOMPARE(o.st, SignatureStatus::Valid);
        QVERIFY(o.who.contains(QStringLiteral("alice@example.test")));
    }
    void testTamperedDataIsInvalid() {
        GpgService s;
        s.setGnupgHome(home_);
        QByteArray d = data_;
        d.replace("Ciao", "Ciaa");
        QCOMPARE(doVerify(s, d, sigGood_).st, SignatureStatus::Invalid);
        // un solo byte di differenza nei fine riga conta (byte esatti)
        QByteArray lf = data_;
        lf.replace("\r\n", "\n");
        QCOMPARE(doVerify(s, lf, sigGood_).st, SignatureStatus::Invalid);
    }
    void testTamperedSignatureIsInvalid() {
        GpgService s;
        s.setGnupgHome(home_);
        QByteArray sg = sigGood_;
        sg[sg.size() - 5] = static_cast<char>(sg[sg.size() - 5] ^ 0x55);
        QCOMPARE(doVerify(s, data_, sg).st, SignatureStatus::Invalid);
        QCOMPARE(doVerify(s, data_, QByteArray("spazzatura")).st, SignatureStatus::Invalid);
        QCOMPARE(doVerify(s, data_, QByteArray()).st, SignatureStatus::Invalid);
    }
    void testUnknownKey() {
        GpgService s;
        s.setGnupgHome(homeEmpty_);
        const auto o = doVerify(s, data_, sigGood_);
        QCOMPARE(o.st, SignatureStatus::UnknownKey);
        QVERIFY(!o.who.isEmpty()); // identificativo della chiave
    }
    void testExpiredKey() {
        GpgService s;
        s.setGnupgHome(home_);
        QCOMPARE(doVerify(s, dataExpired_, sigExpired_).st, SignatureStatus::ExpiredKey);
    }
    void testRevokedKey() {
        GpgService s;
        s.setGnupgHome(home_);
        QCOMPARE(doVerify(s, dataRevoked_, sigRevoked_).st, SignatureStatus::RevokedKey);
    }
    void testGpgMissingIsInvalidNotValid() {
        GpgService s(QStringLiteral("/nonexistent/gpg"));
        QCOMPARE(doVerify(s, data_, sigGood_).st, SignatureStatus::Invalid);
        const auto d = doDecrypt(s, "x");
        QVERIFY(!d.ok);
        QVERIFY(d.plain.isEmpty());
        QVERIFY(!d.err.isEmpty());
    }

    void testClassifyStatusLines_data() {
        QTest::addColumn<QByteArray>("status");
        QTest::addColumn<int>("code");
        QTest::addColumn<int>("expected");
        auto E = [](SignatureStatus s) { return static_cast<int>(s); };
        QTest::newRow("good") << QByteArray("[GNUPG:] GOODSIG AB Foo <f@x>\n[GNUPG:] VALIDSIG AB\n") << 0 << E(SignatureStatus::Valid);
        QTest::newRow("good but exit 2") << QByteArray("[GNUPG:] GOODSIG AB Foo\n") << 2 << E(SignatureStatus::Invalid);
        QTest::newRow("bad beats good") << QByteArray("[GNUPG:] GOODSIG A x\n[GNUPG:] BADSIG B y\n") << 1 << E(SignatureStatus::Invalid);
        QTest::newRow("revoked with validsig") << QByteArray("[GNUPG:] REVKEYSIG A x\n[GNUPG:] VALIDSIG A\n") << 0 << E(SignatureStatus::RevokedKey);
        QTest::newRow("expkeysig") << QByteArray("[GNUPG:] EXPKEYSIG A x\n[GNUPG:] VALIDSIG A\n") << 0 << E(SignatureStatus::ExpiredKey);
        QTest::newRow("expsig") << QByteArray("[GNUPG:] EXPSIG A x\n") << 1 << E(SignatureStatus::ExpiredKey);
        QTest::newRow("errsig") << QByteArray("[GNUPG:] ERRSIG A 1 8 00 1 9 01\n[GNUPG:] NO_PUBKEY A\n") << 2 << E(SignatureStatus::UnknownKey);
        QTest::newRow("nodata") << QByteArray("[GNUPG:] NODATA 3\n") << 2 << E(SignatureStatus::Invalid);
        QTest::newRow("empty") << QByteArray() << 0 << E(SignatureStatus::Invalid);
        QTest::newRow("human text only") << QByteArray("gpg: Good signature from \"Foo\"\n") << 0 << E(SignatureStatus::Invalid);
        QTest::newRow("forged text") << QByteArray("gpg: [GNUPG:] GOODSIG A x\n") << 0 << E(SignatureStatus::Invalid);
    }
    void testClassifyStatusLines() {
        QFETCH(QByteArray, status);
        QFETCH(int, code);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(GpgService::classifyVerifyStatus(status, code)), expected);
    }

    void testDecryptRoundTrip() {
        const QByteArray plain = "Oggetto: segreto\r\n\r\nTesto riservato \xc3\xa8\r\n";
        const GpgRun e = runGpg(home_, {QStringLiteral("--batch"), QStringLiteral("--trust-model"), QStringLiteral("always"),
                                        QStringLiteral("-r"), QStringLiteral("alice@example.test"), QStringLiteral("--encrypt")},
                                plain);
        QCOMPARE(e.code, 0);
        GpgService s;
        s.setGnupgHome(home_);
        const auto o = doDecrypt(s, e.out);
        QVERIFY2(o.ok, qPrintable(o.err));
        QCOMPARE(o.plain, plain);
        QVERIFY(o.err.isEmpty());
    }
    void testDecryptWithPassphraseKeyUsesPinentryViaAgent() {
        // --batch non impedisce a gpg-agent di interrogare pinentry
        const QByteArray plain = "messaggio per chiave protetta";
        const GpgRun e = runGpg(homePw_, {QStringLiteral("--batch"), QStringLiteral("--trust-model"), QStringLiteral("always"),
                                          QStringLiteral("-r"), QStringLiteral("pia@example.test"), QStringLiteral("--encrypt")},
                                plain);
        QCOMPARE(e.code, 0);
        QFile::remove(root_.filePath(QStringLiteral("pinentry.called")));
        GpgService s;
        s.setGnupgHome(homePw_);
        const auto o = doDecrypt(s, e.out);
        QVERIFY2(o.ok, qPrintable(o.err));
        QCOMPARE(o.plain, plain);
        QVERIFY(QFile::exists(root_.filePath(QStringLiteral("pinentry.called"))));
    }
    void testDecryptWrongRecipientFailsWithoutPlain() {
        const GpgRun e = runGpg(home_, {QStringLiteral("--batch"), QStringLiteral("--trust-model"), QStringLiteral("always"),
                                        QStringLiteral("-r"), QStringLiteral("alice@example.test"), QStringLiteral("--encrypt")},
                                "x");
        GpgService s;
        s.setGnupgHome(homeEmpty_); // senza la chiave segreta
        const auto o = doDecrypt(s, e.out);
        QVERIFY(!o.ok);
        QVERIFY(o.plain.isEmpty());
        QVERIFY(!o.err.isEmpty());
    }
    void testDecryptCancelledByUserReportsError() {
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        QVERIFY(d.isValid());
        const QString fake = writeScript(QDir(d.path()), QStringLiteral("gpg"),
                                         "#!/bin/sh\n"
                                         "cat >/dev/null\n"
                                         "printf 'testo parziale'\n"
                                         "echo '[GNUPG:] ENC_TO AAAA 1 0' >&2\n"
                                         "echo '[GNUPG:] ERROR pkdecrypt_failed 83886179' >&2\n"
                                         "echo '[GNUPG:] DECRYPTION_FAILED' >&2\n"
                                         "echo 'gpg: decryption failed: operation cancelled' >&2\n"
                                         "exit 2\n");
        GpgService s(fake);
        const auto o = doDecrypt(s, "-----BEGIN PGP MESSAGE-----");
        QVERIFY(!o.ok);
        QVERIFY(o.plain.isEmpty());
        QVERIFY(!o.err.isEmpty());
        QVERIFY(!o.err.contains(QStringLiteral("parziale")));
    }
    void testDecryptOkayWithNonZeroExitIsFailure() {
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        const QString fake = writeScript(QDir(d.path()), QStringLiteral("gpg"),
                                         "#!/bin/sh\ncat >/dev/null\nprintf 'x'\n"
                                         "echo '[GNUPG:] DECRYPTION_OKAY' >&2\nexit 2\n");
        GpgService s(fake);
        const auto o = doDecrypt(s, "a");
        QVERIFY(!o.ok);
        QVERIFY(o.plain.isEmpty());
    }

    void testNoPassphraseIsEverPassedOnCommandLine() {
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        QVERIFY(d.isValid());
        const QString argsFile = d.filePath(QStringLiteral("args.txt"));
        const QString stdinFile = d.filePath(QStringLiteral("stdin.bin"));
        const QString fake = writeScript(QDir(d.path()), QStringLiteral("gpg"),
                                         "#!/bin/sh\n"
                                         "for a in \"$@\"; do printf '%s\\n' \"$a\" >> '" + argsFile.toUtf8() + "'; done\n"
                                         "echo ---- >> '" + argsFile.toUtf8() + "'\n"
                                         "cat > '" + stdinFile.toUtf8() + "'\n"
                                         "echo '[GNUPG:] GOODSIG AA Fake' >&2\n"
                                         "echo '[GNUPG:] DECRYPTION_OKAY' >&2\n"
                                         "echo '[GNUPG:] END_DECRYPTION' >&2\n"
                                         "exit 0\n");
        GpgService s(fake);
        const auto v = doVerify(s, data_, sigGood_);
        QCOMPARE(v.st, SignatureStatus::Valid);
        // i dati firmati arrivano a gpg byte per byte
        QFile in(stdinFile);
        QVERIFY(in.open(QIODevice::ReadOnly));
        QCOMPARE(in.readAll(), data_);
        in.close();
        const auto o = doDecrypt(s, "cifrato");
        QVERIFY(o.ok);
        QFile f(argsFile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray args = f.readAll();
        QVERIFY(args.contains("--status-fd"));
        QVERIFY(args.contains("--verify"));
        QVERIFY(args.contains("--decrypt"));
        QVERIFY2(!args.contains("passphrase"), args.constData());
        QVERIFY2(!args.contains("loopback"), args.constData());
        QVERIFY2(!args.contains("pinentry-mode"), args.constData());
        QVERIFY2(!args.contains("--homedir"), args.constData());
    }

    void testDestroyWhileRunningKillsChildAndNeverCallsBack() {
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        const QString pidFile = d.filePath(QStringLiteral("pid"));
        const QString fake = writeScript(QDir(d.path()), QStringLiteral("gpg"),
                                         "#!/bin/sh\necho $$ > '" + pidFile.toUtf8() + "'\nexec sleep 30\n");
        bool called = false;
        {
            auto* s = new GpgService(fake);
            s->decrypt("x", [&called](bool, QByteArray, QString) { called = true; });
            s->verify("x", "y", [&called](SignatureStatus, QString) { called = true; });
            QTRY_VERIFY(QFile::exists(pidFile));
            QTest::qWait(200);
            delete s;
        }
        QFile f(pidFile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const pid_t pid = f.readAll().trimmed().toInt();
        QVERIFY(pid > 1);
        QTest::qWait(100);
        QVERIFY(::kill(pid, 0) == -1 && errno == ESRCH);
        QVERIFY(!called);
    }

    void testParseGpgconfOptions() {
        const QByteArray real =
            "default-cache-ttl:24:0:expire cached PINs after N seconds:3:3:N:600::300\n"
            "default-cache-ttl-ssh:24:1:ssh:3:3:N:1800::\n"
            "max-cache-ttl:24:2:set max:3:3:N:7200::300\n"
            "max-cache-ttl-ssh:24:2:ssh:3:3:N:7200::\n";
        auto i = GpgService::parseGpgconfOptions(real);
        QVERIFY(i.available);
        QCOMPARE(i.defaultCacheTtl, 300);
        QCOMPARE(i.maxCacheTtl, 300);
        QVERIFY(i.ttlIs300);

        // l'ultimo campo e' il valore: 300 / 999999 non e' "tutti 300"
        i = GpgService::parseGpgconfOptions("default-cache-ttl:24:0:d:3:3:N:600::300\nmax-cache-ttl:24:2:d:3:3:N:7200::999999\n");
        QVERIFY(i.available);
        QCOMPARE(i.maxCacheTtl, 999999);
        QVERIFY(!i.ttlIs300);

        // valori vuoti = predefiniti (600 s e 2 h)
        i = GpgService::parseGpgconfOptions("default-cache-ttl:24:0:d:3:3:N:600::\nmax-cache-ttl:24:2:d:3:3:N:7200::\n");
        QVERIFY(i.available);
        QCOMPARE(i.defaultCacheTtl, 600);
        QCOMPARE(i.maxCacheTtl, 7200);
        QVERIFY(!i.ttlIs300);

        // opzioni mancanti
        i = GpgService::parseGpgconfOptions("max-cache-ttl:24:2:d:3:3:N:7200::300\n");
        QVERIFY(i.available);
        QCOMPARE(i.defaultCacheTtl, 600);
        QVERIFY(!i.ttlIs300);
        i = GpgService::parseGpgconfOptions("pinentry-program:8:0:x:...\n");
        QVERIFY(!i.available);
        QVERIFY(!i.ttlIs300);

        // spazzatura
        for (const QByteArray& g : {QByteArray(), QByteArray("\xff\x00::\n:::"), QByteArray("default-cache-ttl"),
                                    QByteArray("default-cache-ttl:24:0:d:3:3:N:600::abc\nmax-cache-ttl:::::::::-5\n")}) {
            i = GpgService::parseGpgconfOptions(g);
            QVERIFY(!i.ttlIs300);
        }
    }
    void testCheckAgentCacheDoesNotCrash() {
        // mai la configurazione reale dell'utente: GNUPGHOME temporaneo
        const QString h = newHome(QStringLiteral("c"));
        const QByteArray old = qgetenv("GNUPGHOME");
        const bool had = qEnvironmentVariableIsSet("GNUPGHOME");
        qputenv("GNUPGHOME", h.toLocal8Bit());
        const GpgAgentInfo i = GpgService::checkAgentCache();
        if (had) qputenv("GNUPGHOME", old); else qunsetenv("GNUPGHOME");
        if (i.available) {
            QVERIFY(i.defaultCacheTtl >= 0 && i.maxCacheTtl >= 0);
            QCOMPARE(i.defaultCacheTtl, 600); // nessuna configurazione: predefiniti
            QCOMPARE(i.maxCacheTtl, 7200);
        }
    }

    void testSignedAndEncryptedFromUnknownSignerIsReadable() {
        // destinatario con chiave propria; Alice (non importata dal destinatario) firma e cifra
        homeR_ = newHome(QStringLiteral("r"));
        gen(homeR_, QStringLiteral("Ron Recipient <ron@example.test>"));
        const GpgRun pub = runGpg(homeR_, {QStringLiteral("--export"), QStringLiteral("ron@example.test")});
        QVERIFY(!pub.out.isEmpty());
        QCOMPARE(runGpg(home_, {QStringLiteral("--batch"), QStringLiteral("--import")}, pub.out).code, 0);
        const QByteArray plain = "Oggetto: firmato e cifrato\r\n\r\ncorpo\r\n";
        const GpgRun e = runGpg(home_, {QStringLiteral("--batch"), QStringLiteral("--trust-model"), QStringLiteral("always"),
                                        QStringLiteral("-u"), QStringLiteral("alice@example.test"), QStringLiteral("-r"),
                                        QStringLiteral("ron@example.test"), QStringLiteral("--sign"), QStringLiteral("--encrypt")},
                                plain);
        QCOMPARE(e.code, 0);

        GpgService s;
        s.setGnupgHome(homeR_);
        DecOut o;
        VerifyOut inner;
        s.decrypt(e.out, [&o](bool ok, QByteArray p, QString er) { o = {true, ok, p, er}; },
                  [&inner](SignatureStatus st, QString who) { inner = {true, st, who}; });
        QTest::qWaitFor([&o] { return o.done; }, 60000);
        QVERIFY2(o.ok, qPrintable(o.err));
        QCOMPARE(o.plain, plain);
        QVERIFY(inner.done);
        QCOMPARE(inner.st, SignatureStatus::UnknownKey); // mai Valid

        // dopo l'importazione della chiave di Alice la firma interna e' Valid
        const GpgRun apub = runGpg(home_, {QStringLiteral("--export"), QStringLiteral("alice@example.test")});
        QCOMPARE(runGpg(homeR_, {QStringLiteral("--batch"), QStringLiteral("--import")}, apub.out).code, 0);
        o = {};
        inner = {};
        s.decrypt(e.out, [&o](bool ok, QByteArray p, QString er) { o = {true, ok, p, er}; },
                  [&inner](SignatureStatus st, QString who) { inner = {true, st, who}; });
        QTest::qWaitFor([&o] { return o.done; }, 60000);
        QVERIFY2(o.ok, qPrintable(o.err));
        QCOMPARE(inner.st, SignatureStatus::Valid);

        // testo cifrato alterato: errore, nessun testo
        for (int pos : {e.out.size() / 2, e.out.size() - 3}) {
            QByteArray bad = e.out;
            bad[pos] = static_cast<char>(bad[pos] ^ 0x40);
            const auto t = doDecrypt(s, bad);
            QVERIFY(!t.ok);
            QVERIFY(t.plain.isEmpty());
            QVERIFY(!t.err.isEmpty());
        }
        // troncato
        const auto tr = doDecrypt(s, e.out.left(e.out.size() - 20));
        QVERIFY(!tr.ok);
        QVERIFY(tr.plain.isEmpty());
    }

    void testDecryptStatusRule_data() {
        QTest::addColumn<QString>("status");
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("ok");
        const QString core = QStringLiteral("[GNUPG:] DECRYPTION_OKAY\n[GNUPG:] END_DECRYPTION\n");
        QTest::newRow("clean") << core << 0 << true;
        QTest::newRow("errsig exit 2") << core + QStringLiteral("[GNUPG:] ERRSIG A 1 8 00 1 9 X\n[GNUPG:] NO_PUBKEY A\n") << 2 << true;
        QTest::newRow("badsig exit 1") << core + QStringLiteral("[GNUPG:] BADSIG A x\n") << 1 << true;
        QTest::newRow("expkeysig exit 1") << core + QStringLiteral("[GNUPG:] EXPKEYSIG A x\n") << 1 << true;
        QTest::newRow("revkeysig exit 1") << core + QStringLiteral("[GNUPG:] REVKEYSIG A x\n") << 1 << true;
        QTest::newRow("other error exit 2") << core + QStringLiteral("[GNUPG:] ERROR something 17\n") << 2 << false;
        QTest::newRow("sig plus other error") << core + QStringLiteral("[GNUPG:] ERRSIG A 1 8 00 1 9 X\n[GNUPG:] ERROR foo 5\n") << 2 << false;
        QTest::newRow("exit 2 no status reason") << core << 2 << false;
        QTest::newRow("no end_decryption") << QStringLiteral("[GNUPG:] DECRYPTION_OKAY\n") << 0 << false;
        QTest::newRow("no okay") << QStringLiteral("[GNUPG:] END_DECRYPTION\n") << 0 << false;
        QTest::newRow("failed") << core + QStringLiteral("[GNUPG:] DECRYPTION_FAILED\n") << 0 << false;
        QTest::newRow("badmdc") << core + QStringLiteral("[GNUPG:] BADMDC\n") << 0 << false;
        QTest::newRow("nodata with sig") << core + QStringLiteral("[GNUPG:] NODATA 1\n[GNUPG:] ERRSIG A 1 8 00 1 9 X\n") << 2 << false;
        QTest::newRow("cancel") << core + QStringLiteral("[GNUPG:] ERROR pkdecrypt_failed 83886179\n") << 2 << false;
    }
    void testDecryptStatusRule() {
        QFETCH(QString, status);
        QFETCH(int, code);
        QFETCH(bool, ok);
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        QVERIFY(d.isValid());
        QByteArray body = "#!/bin/sh\ncat >/dev/null\nprintf 'PLAIN'\n";
        for (const QString& l : status.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            body += "echo '" + l.toUtf8() + "' >&2\n";
        body += "exit " + QByteArray::number(code) + "\n";
        GpgService s(writeScript(QDir(d.path()), QStringLiteral("gpg"), body));
        const auto o = doDecrypt(s, "x");
        QCOMPARE(o.ok, ok);
        if (ok) QCOMPARE(o.plain, QByteArray("PLAIN"));
        else {
            QVERIFY(o.plain.isEmpty());
            QVERIFY(!o.err.isEmpty());
        }
    }

    void testPlaintextOverflowKillsGpgAndFails() {
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        const QString pidFile = d.filePath(QStringLiteral("pid"));
        const QString fake = writeScript(QDir(d.path()), QStringLiteral("gpg"),
                                         "#!/bin/sh\necho $$ > '" + pidFile.toUtf8() + "'\n"
                                         "head -c 100000 /dev/zero\nexec sleep 30\n");
        GpgService s(fake);
        s.setMaxPlaintextBytes(1000);
        const auto o = doDecrypt(s, "x");
        QVERIFY(!o.ok);
        QVERIFY(o.plain.isEmpty());
        QCOMPARE(o.err, QStringLiteral("Messaggio decifrato troppo grande"));
        QFile f(pidFile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const pid_t pid = f.readAll().trimmed().toInt();
        QTest::qWait(100);
        QVERIFY(::kill(pid, 0) == -1 && errno == ESRCH);
    }
    void testStatusOverflowFails() {
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        const QString fake = writeScript(QDir(d.path()), QStringLiteral("gpg"),
                                         "#!/bin/sh\ncat >/dev/null\nprintf 'ok'\n"
                                         "head -c 100000 /dev/zero | tr '\\0' 'a' >&2\n"
                                         "echo '[GNUPG:] DECRYPTION_OKAY' >&2\necho '[GNUPG:] END_DECRYPTION' >&2\n");
        GpgService s(fake);
        s.setMaxStatusBytes(1000);
        const auto o = doDecrypt(s, "x");
        QVERIFY(!o.ok);
        QVERIFY(o.plain.isEmpty());
        QVERIFY(!o.err.isEmpty());
        // verify: stdout enorme -> Invalid
        const auto v = doVerify(s, "d", "s");
        QCOMPARE(v.st, SignatureStatus::Invalid);
    }

    void testStartFailureMessageAndQueuedCallback() {
        GpgService s(QStringLiteral("/nonexistent/gpg"));
        bool called = false, inside = true;
        DecOut o;
        s.decrypt("x", [&](bool ok, QByteArray p, QString e) { called = true; o = {true, ok, p, e}; });
        inside = called; // il callback non gira dentro decrypt()
        QVERIFY(!inside);
        QTRY_VERIFY(called);
        QVERIFY(!o.ok);
        QVERIFY2(o.err.contains(QStringLiteral("No such file")), qPrintable(o.err));
    }

    void testInlineSignedMessageAsDetachedSignatureIsNotValid() {
        const GpgRun r = runGpg(home_, {QStringLiteral("--batch"), QStringLiteral("-u"), QStringLiteral("alice@example.test"),
                                        QStringLiteral("--sign")},
                                data_);
        QCOMPARE(r.code, 0);
        GpgService s;
        s.setGnupgHome(home_);
        QVERIFY(doVerify(s, data_, r.out).st != SignatureStatus::Valid);
        QVERIFY(doVerify(s, "altro contenuto", r.out).st != SignatureStatus::Valid);
    }

    void testMultipleSignaturesWorstWins() {
        QTemporaryDir d(QDir::currentPath() + QStringLiteral("/fk-XXXXXX"));
        auto run = [&](const QByteArray& lines, int code) {
            GpgService s(writeScript(QDir(d.path()), QStringLiteral("gpg"),
                                     "#!/bin/sh\ncat >/dev/null\n" + lines + "exit " + QByteArray::number(code) + "\n"));
            return doVerify(s, "d", "s").st;
        };
        QCOMPARE(run("echo '[GNUPG:] GOODSIG A Uno' >&2\necho '[GNUPG:] BADSIG B Due' >&2\n", 1), SignatureStatus::Invalid);
        QCOMPARE(run("echo '[GNUPG:] GOODSIG A Uno' >&2\necho '[GNUPG:] ERRSIG B 1 8 00 1 9 X' >&2\n", 2),
                 SignatureStatus::UnknownKey);
        QCOMPARE(run("echo '[GNUPG:] GOODSIG A Uno' >&2\necho '[GNUPG:] GOODSIG B Due' >&2\n", 0), SignatureStatus::Valid);
    }
};

QTEST_MAIN(T)
#include "test_gpgservice.moc"
