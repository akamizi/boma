#include "mime/MessageParser.h"
#include "pgp/GpgService.h"
#include "send/Sender.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <sys/stat.h>

using namespace boma;

namespace {

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

QByteArray readFile(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

SendResult doSend(Sender& s, const OutgoingMessage& m, bool sign, bool enc, bool allowUnsigned = false) {
    SendResult out;
    bool done = false;
    s.send(m, sign, enc, [&](SendResult r) { out = r; done = true; }, allowUnsigned);
    if (!QTest::qWaitFor([&] { return done; }, 120000)) qFatal("send: callback non arrivato");
    return out;
}

constexpr const char* kBody = "Corpo di prova riservato 7731";

} // namespace

class T : public QObject {
    Q_OBJECT
    QTemporaryDir root_{QDir::currentPath() + QStringLiteral("/sndt-XXXXXX")};
    QString home_, homePw_, homeAmb_, bin_, wrapper_, failGpg_;
    QList<Account> accounts_;
    Identity idA_, idB_, idNoKey_, idPia_;
    int n_ = 0;

    // Ambiente per un singolo test: msmtp e mu finti, Maildir sintetiche.
    struct Env {
        QString dir, msmtp, mu, gpg;
    };

    QString newHome(const QString& name, const QByteArray& agentConf = "pinentry-program /bin/false\n") {
        const QString h = root_.filePath(name);
        QDir().mkpath(h);
        chmod(h.toLocal8Bit().constData(), 0700);
        QFile f(h + QStringLiteral("/gpg-agent.conf"));
        f.open(QIODevice::WriteOnly);
        f.write(agentConf);
        f.close();
        return h;
    }
    void gen(const QString& home, const QString& uid, const QString& expire = QStringLiteral("never"),
             const QStringList& extra = {}) {
        QStringList a = extra;
        a << QStringLiteral("--batch") << QStringLiteral("--passphrase") << QString() << QStringLiteral("--quick-gen-key")
          << uid << QStringLiteral("default") << QStringLiteral("default") << expire;
        const GpgRun r = runGpg(home, a);
        if (r.code != 0) qFatal("generazione chiave fallita: %s", r.err.constData());
    }
    void maildir(const QString& path) {
        for (const char* s : {"cur", "new", "tmp"}) QDir().mkpath(path + QLatin1Char('/') + QLatin1String(s));
    }

    Env newEnv() {
        Env e;
        e.dir = root_.filePath(QStringLiteral("env%1").arg(++n_));
        QDir().mkpath(e.dir);
        QDir d(e.dir);
        e.msmtp = writeScript(d, QStringLiteral("msmtp"),
                              "#!/bin/sh\n"
                              "d=$(dirname \"$0\")\n"
                              "echo run >> \"$d/count\"\n"
                              "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"$d/argv\"\n"
                              "cat > \"$d/stdin\"\n"
                              "[ -f \"$d/sleep\" ] && exec sleep 30\n"
                              "[ -f \"$d/fail\" ] && { echo 'msmtp: authentication failed' >&2; exit 1; }\n"
                              "exit 0\n");
        e.mu = writeScript(d, QStringLiteral("mu"), "#!/bin/sh\nd=$(dirname \"$0\")\nprintf '%s\\n' \"$*\" >> \"$d/mu.log\"\nexit 0\n");
        e.gpg = writeScript(d, QStringLiteral("gpgwrap"),
                            "#!/bin/sh\nd=$(dirname \"$0\")\nprintf '%s\\n' \"$*\" >> \"$d/gpg.log\"\nexec gpg \"$@\"\n");
        // maildir degli account a e b sotto root_/mail (ricreate a ogni test)
        QDir(root_.filePath(QStringLiteral("mail"))).removeRecursively();
        for (const char* acc : {"a", "b"}) maildir(root_.filePath(QStringLiteral("mail/%1/Sent").arg(QLatin1String(acc))));
        return e;
    }
    std::unique_ptr<Sender> makeSender(const Env& e, const QString& home, bool wrapGpg = true) {
        auto s = std::make_unique<Sender>();
        s->setAccounts(accounts_);
        s->setMsmtpProgram(e.msmtp);
        s->setMuProgram(e.mu);
        s->setMuHome(e.dir + QStringLiteral("/muhome"));
        s->gpg().setGpgProgram(wrapGpg ? e.gpg : QStringLiteral("gpg"));
        s->gpg().setGnupgHome(home);
        return s;
    }
    OutgoingMessage msgFor(const Identity& id, const QStringList& to, const QStringList& bcc = {}) {
        OutgoingMessage m;
        m.identity = id;
        m.to = to;
        m.bcc = bcc;
        m.subject = QStringLiteral("Prova invio");
        m.plainText = QString::fromLatin1(kBody);
        return m;
    }
    QStringList files(const QString& dir) { return QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot); }
    QString sentDir(const char* acc) { return root_.filePath(QStringLiteral("mail/%1/Sent/cur").arg(QLatin1String(acc))); }

private slots:
    void initTestCase() {
        QVERIFY2(root_.isValid(), "directory temporanea non creata");
        chmod(root_.path().toLocal8Bit().constData(), 0700);
        home_ = newHome(QStringLiteral("h"));
        gen(home_, QStringLiteral("Alice Test <alice@example.test>"));
        gen(home_, QStringLiteral("Bob Test <bob@example.test>"));
        gen(home_, QStringLiteral("Malice Decoy <malice@example.test>"));
        gen(home_, QStringLiteral("Alice Evil <alice@example.test.evil>"));
        gen(home_, QStringLiteral("Edna Expired <edna@example.test>"), QStringLiteral("1d"),
            {QStringLiteral("--faked-system-time"), QStringLiteral("20200101T000000")});

        homeAmb_ = newHome(QStringLiteral("amb"));
        gen(homeAmb_, QStringLiteral("Alice Uno <alice@example.test>"));
        gen(homeAmb_, QStringLiteral("Alice Due <alice@example.test>"));
        gen(homeAmb_, QStringLiteral("Bob Test <bob@example.test>"));

        // chiave protetta da password, pinentry che fallisce (annullamento)
        homePw_ = newHome(QStringLiteral("p"));
        GpgRun r = runGpg(homePw_, {QStringLiteral("--batch"), QStringLiteral("--pinentry-mode"), QStringLiteral("loopback"),
                                    QStringLiteral("--passphrase-fd"), QStringLiteral("0"), QStringLiteral("--quick-gen-key"),
                                    QStringLiteral("Pia Password <pia@example.test>"), QStringLiteral("default"),
                                    QStringLiteral("default"), QStringLiteral("never")},
                          "secretpw\n");
        QVERIFY2(r.code == 0, r.err.constData());

        accounts_ << Account{QStringLiteral("a"), QStringLiteral("A"), root_.filePath(QStringLiteral("mail/a")), QStringLiteral("a"),
                             QStringLiteral("Sent"), QStringLiteral("Drafts"), QStringLiteral("Trash")}
                  << Account{QStringLiteral("b"), QStringLiteral("B"), root_.filePath(QStringLiteral("mail/b")), QStringLiteral("b"),
                             QStringLiteral("Sent"), QStringLiteral("Drafts"), QStringLiteral("Trash")};
        auto id = [](const char* i, const char* acc, const char* addr, const char* key, const char* ms) {
            Identity x;
            x.id = QLatin1String(i);
            x.accountId = QLatin1String(acc);
            x.address = QLatin1String(addr);
            x.displayName = QStringLiteral("Mittente");
            x.pgpKeyId = QLatin1String(key);
            x.msmtpAccount = QLatin1String(ms);
            return x;
        };
        idA_ = id("ia", "a", "alice@example.test", "alice@example.test", "acc-a");
        idB_ = id("ib", "b", "alice@example.test", "alice@example.test", "acc-b");
        idNoKey_ = id("in", "a", "alice@example.test", "", "acc-a");
        idPia_ = id("ip", "a", "pia@example.test", "pia@example.test", "acc-a");
    }
    void cleanupTestCase() {
        killAgent(home_);
        killAgent(homePw_);
        killAgent(homeAmb_);
    }

    // ---- GpgService (parte di scrittura) ----
    void testUsableKeyAddressesParsing() {
        const QByteArray c =
            "tru::1:1:1:0:3:1:5\n"
            "pub:u:255:22:AAAA:1:::u:::scESC:::::::\nuid:u::::1::H::Ok Uno <Ok@Example.Test>:::::::\nuid:r::::1::H::Rev <rev@example.test>:::::::\n"
            "sub:u:255:18:BBBB:1::::::e:::::\n"
            "pub:e:255:22:CCCC:1:::-:::scESC:::::::\nuid:e::::1::H::Exp <exp@example.test>:::::::\nsub:e:255:18:DDDD:1::::::e:::::\n"
            "pub:r:255:22:EEEE:1:::-:::scESC:::::::\nuid:r::::1::H::Rev2 <rev2@example.test>:::::::\n"
            "pub:-:255:22:FFFF:1:::-:::scESD:::::::\nuid:-::::1::H::Dis <dis@example.test>:::::::\nsub:-:255:18:GGGG:1::::::e:::::\n"
            "pub:-:255:22:HHHH:1:::-:::scSC:::::::\nuid:-::::1::H::Solofirma <sign@example.test>:::::::\n"
            "pub:-:255:22:IIII:1:::-:::scESC:::::::\nuid:-::::1::H::Sub Scaduta <subexp@example.test>:::::::\nsub:e:255:18:JJJJ:1::::::e:::::\n"
            "pub:-:255:22:KKKK:1:::-:::scESC:::::::\nuid:-::::1::H::Nome \\x3a Strano <colon@example.test>:::::::\nsub:-:255:18:LLLL:1::::::e:::::\n";
        const QSet<QString> u = GpgService::usableKeyAddresses(c);
        QVERIFY(u.contains(QStringLiteral("ok@example.test")));
        QVERIFY(u.contains(QStringLiteral("colon@example.test")));
        for (const char* bad : {"rev@example.test", "exp@example.test", "rev2@example.test", "dis@example.test",
                                "sign@example.test", "subexp@example.test"})
            QVERIFY2(!u.contains(QLatin1String(bad)), bad);
    }
    void testMissingKeysWithRealGpg() {
        GpgService g;
        g.setGnupgHome(home_);
        const QStringList miss = g.missingKeys({QStringLiteral("ALICE@Example.Test"), QStringLiteral("bob@example.test"),
                                                QStringLiteral("edna@example.test"), QStringLiteral("carol@example.test"),
                                                QStringLiteral("-x@example.test"), QStringLiteral("bo@example.test")});
        QCOMPARE(miss, (QStringList{QStringLiteral("edna@example.test"), QStringLiteral("carol@example.test"),
                                    QStringLiteral("-x@example.test"), QStringLiteral("bo@example.test")}));
        // gpg inesistente: tutto mancante
        GpgService bad(QStringLiteral("/nonexistent/gpg"));
        QCOMPARE(bad.missingKeys({QStringLiteral("alice@example.test")}), QStringList{QStringLiteral("alice@example.test")});
    }
    void testGpgSignMicalgAndBadKeyId() {
        GpgService g;
        g.setGnupgHome(home_);
        struct { bool done = false, ok = false; QByteArray sig; QString micalg, err; } o;
        const QByteArray data = "Content-Type: text/plain\r\n\r\nx\r\n";
        g.sign(data, QStringLiteral("alice@example.test"), [&](bool ok, QByteArray s, QString m, QString e) { o = {true, ok, s, m, e}; });
        QTRY_VERIFY_WITH_TIMEOUT(o.done, 60000);
        QVERIFY2(o.ok, qPrintable(o.err));
        QCOMPARE(o.micalg, QStringLiteral("pgp-sha256"));
        QVERIFY(o.sig.startsWith("-----BEGIN PGP SIGNATURE"));
        o = {};
        g.sign(data, QStringLiteral("--homedir=/tmp"), [&](bool ok, QByteArray s, QString m, QString e) { o = {true, ok, s, m, e}; });
        QTRY_VERIFY(o.done);
        QVERIFY(!o.ok && o.sig.isEmpty());
        o = {};
        g.sign(data, QStringLiteral("nessuno@example.test"), [&](bool ok, QByteArray s, QString m, QString e) { o = {true, ok, s, m, e}; });
        QTRY_VERIFY_WITH_TIMEOUT(o.done, 60000);
        QVERIFY(!o.ok && o.sig.isEmpty() && !o.err.isEmpty());
    }

    // ---- Sender ----
    void testMissingKeyBlocksEncryptedSendAndSendsNothing() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test"), QStringLiteral("carol@example.test")}), true, true);
        QCOMPARE(r.kind, SendResult::MissingKeys);
        QCOMPARE(r.missing, QStringList{QStringLiteral("carol@example.test")});
        QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));          // msmtp mai eseguito
        QVERIFY(!readFile(e.dir + QStringLiteral("/gpg.log")).contains("--encrypt")); // nessuna cifratura
        QVERIFY(!readFile(e.dir + QStringLiteral("/gpg.log")).contains("--detach-sign")); // nemmeno la firma
        QVERIFY(files(sentDir("a")).isEmpty());
        QVERIFY(!s->busy() || QTest::qWaitFor([&] { return !s->busy(); }, 1000));
    }
    void testSignFailureStopsAndKeepsDraft() {
        for (const bool fake : {false, true}) {
            const Env e = newEnv();
            std::unique_ptr<Sender> s;
            OutgoingMessage m;
            if (!fake) { // pinentry che fallisce: firma annullata
                s = makeSender(e, homePw_);
                m = msgFor(idPia_, {QStringLiteral("bob@example.test")});
            } else { // gpg che esce con errore
                s = makeSender(e, home_);
                s->gpg().setGpgProgram(writeScript(QDir(e.dir), QStringLiteral("gpgfail"), "#!/bin/sh\necho 'gpg: agent non raggiungibile' >&2\nexit 2\n"));
                m = msgFor(idA_, {QStringLiteral("bob@example.test")});
            }
            const OutgoingMessage copy = m;
            const SendResult r = doSend(*s, m, true, false, /*allowUnsigned=*/true); // allowUnsigned NON ripiega
            QCOMPARE(r.kind, SendResult::SignFailed);
            QVERIFY(!r.message.isEmpty());
            QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));
            QVERIFY(files(sentDir("a")).isEmpty());
            QCOMPARE(m.to, copy.to);
            QCOMPARE(m.plainText, copy.plainText);
            QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        }
    }
    void testUnsignedSendRequiresExplicitConfirmation() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        const OutgoingMessage m = msgFor(idNoKey_, {QStringLiteral("bob@example.test")});
        SendResult r = doSend(*s, m, true, false);
        QCOMPARE(r.kind, SendResult::NeedsConfirmationUnsigned);
        QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        // anche cifrando + firma richiesta: serve la conferma
        r = doSend(*s, msgFor(idNoKey_, {QStringLiteral("bob@example.test")}), true, true);
        QCOMPARE(r.kind, SendResult::NeedsConfirmationUnsigned);
        QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        // conferma esplicita
        r = doSend(*s, m, true, false, true);
        QCOMPARE(r.kind, SendResult::Sent);
        const QByteArray sent = readFile(e.dir + QStringLiteral("/stdin"));
        QVERIFY(!sent.contains("multipart/signed"));
        QVERIFY(sent.contains(kBody));
    }
    void testSmtpFailureWritesNoSentCopy() {
        const Env e = newEnv();
        QFile(e.dir + QStringLiteral("/fail")).open(QIODevice::WriteOnly);
        auto s = makeSender(e, home_);
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false);
        QCOMPARE(r.kind, SendResult::SmtpFailed);
        QVERIFY(r.message.contains(QStringLiteral("authentication failed")));
        QVERIFY(files(sentDir("a")).isEmpty());
        QVERIFY(!QFile::exists(e.dir + QStringLiteral("/mu.log")));
    }
    void testSentCopyGoesToIdentityAccountSentFolder() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false);
        QCOMPARE(r.kind, SendResult::Sent);
        QVERIFY2(r.message.isEmpty(), qPrintable(r.message));
        QCOMPARE(files(sentDir("a")).size(), 1);
        QVERIFY(files(sentDir("b")).isEmpty());
        QVERIFY(files(sentDir("a")).first().endsWith(QStringLiteral(":2,S")));
        QCOMPARE(readFile(sentDir("a") + QLatin1Char('/') + files(sentDir("a")).first()), readFile(e.dir + QStringLiteral("/stdin")));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(e.dir + QStringLiteral("/mu.log")), 10000);
        QVERIFY(readFile(e.dir + QStringLiteral("/mu.log")).startsWith("index"));
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        r = doSend(*s, msgFor(idB_, {QStringLiteral("bob@example.test")}), false, false);
        QCOMPARE(r.kind, SendResult::Sent);
        QCOMPARE(files(sentDir("a")).size(), 1);
        QCOMPARE(files(sentDir("b")).size(), 1);
    }
    void testMsmtpAccountMatchesIdentity() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        OutgoingMessage m = msgFor(idA_, {QStringLiteral("Bob <bob@example.test>")}, {QStringLiteral("hidden@example.test")});
        m.cc = {QStringLiteral("cc@example.test"), QStringLiteral("BOB@example.test")};
        QCOMPARE(doSend(*s, m, false, false).kind, SendResult::Sent);
        const QList<QByteArray> argv = readFile(e.dir + QStringLiteral("/argv")).split('\n');
        QCOMPARE(argv, (QList<QByteArray>{"-a", "acc-a", "--read-envelope-from", "--", "bob@example.test", "cc@example.test",
                                          "hidden@example.test", ""}));
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        // identita' dell'altro account: -a acc-b
        QCOMPARE(doSend(*s, msgFor(idB_, {QStringLiteral("bob@example.test")}), false, false).kind, SendResult::Sent);
        QVERIFY(readFile(e.dir + QStringLiteral("/argv")).startsWith("-a\nacc-b\n--read-envelope-from\n--\n"));
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        // account msmtp vuoto o con '-' iniziale: mai l'account predefinito per caso
        for (const char* bad : {"", "-C/etc/x", "a b"}) {
            Identity i = idA_;
            i.msmtpAccount = QLatin1String(bad);
            QFile::remove(e.dir + QStringLiteral("/count"));
            QCOMPARE(doSend(*s, msgFor(i, {QStringLiteral("bob@example.test")}), false, false).kind, SendResult::BuildFailed);
            QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));
            QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        }
        QCOMPARE(Sender::msmtpArguments(QStringLiteral("x"), {QStringLiteral("a@b.c")}),
                 (QStringList{QStringLiteral("-a"), QStringLiteral("x"), QStringLiteral("--read-envelope-from"), QStringLiteral("--"),
                              QStringLiteral("a@b.c")}));
    }
    void testBccReachesEnvelopeButNoHeaderOrPart() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        QCOMPARE(doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}, {QStringLiteral("segreto@example.test")}), false, false).kind,
                 SendResult::Sent);
        QVERIFY(readFile(e.dir + QStringLiteral("/argv")).contains("segreto@example.test"));
        const QByteArray sent = readFile(e.dir + QStringLiteral("/stdin"));
        QVERIFY(!sent.contains("segreto@example.test"));
        QVERIFY(!sent.toLower().contains("bcc:"));
        const QByteArray copy = readFile(sentDir("a") + QLatin1Char('/') + files(sentDir("a")).first());
        QVERIFY(!copy.contains("segreto@example.test"));
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        // firmato e cifrato: nemmeno li'
        QCOMPARE(doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}, {QStringLiteral("alice@example.test")}), true, true).kind,
                 SendResult::Sent);
        QVERIFY(!readFile(e.dir + QStringLiteral("/stdin")).toLower().contains("bcc:"));
    }
    void testSignedMessageVerifies() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        QCOMPARE(doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), true, false).kind, SendResult::Sent);
        const QByteArray sent = readFile(e.dir + QStringLiteral("/stdin"));
        QVERIFY(sent.contains("multipart/signed"));
        QVERIFY(sent.contains("micalg=pgp-sha256"));
        QVERIFY(sent.contains("protocol=\"application/pgp-signature\""));
        QVERIFY(sent.contains("Subject: Prova invio"));
        const QByteArray lc = "\n" + sent.toLower();
        QVERIFY(lc.contains("\nmessage-id:") && lc.contains("\ndate:") && lc.contains("\nfrom:") && lc.contains("\nto:"));
        QString err;
        const ParsedMessage pm = MessageParser::parse(sent, &err);
        QVERIFY2(pm.signedPgp, qPrintable(err));
        QVERIFY(!pm.encryptedPgp);
        QVERIFY(pm.plainText.contains(QLatin1String(kBody)));
        QVERIFY(pm.rawSignedPart.contains("\r\n")); // canonicalizzata
        QVERIFY(!pm.rawSignedPart.contains("\n\n")); // nessun LF isolato
        GpgService g;
        g.setGnupgHome(home_);
        SignatureStatus st = SignatureStatus::None;
        QString who;
        bool done = false;
        g.verify(pm.rawSignedPart, pm.rawSignature, [&](SignatureStatus x, QString w) { st = x; who = w; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QCOMPARE(st, SignatureStatus::Valid);
        QVERIFY(who.contains(QStringLiteral("alice@example.test")));
        // alterazione: non piu' valida
        QByteArray t = pm.rawSignedPart;
        t.replace("7731", "7732");
        done = false;
        g.verify(t, pm.rawSignature, [&](SignatureStatus x, QString) { st = x; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QCOMPARE(st, SignatureStatus::Invalid);
        // la copia in Inviata e' identica a cio' che e' stato inviato
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
    }
    void testEncryptedMessageIsPgpMime() {
        for (const bool sign : {true, false}) {
            const Env e = newEnv();
            auto s = makeSender(e, home_);
            QCOMPARE(doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}, {QStringLiteral("alice@example.test")}), sign, true).kind,
                     SendResult::Sent);
            const QByteArray sent = readFile(e.dir + QStringLiteral("/stdin"));
            QVERIFY(sent.contains("multipart/encrypted"));
            QVERIFY(sent.contains("protocol=\"application/pgp-encrypted\""));
            QVERIFY(sent.contains("Version: 1"));
            QVERIFY(sent.contains("application/octet-stream"));
            QVERIFY(!sent.contains(kBody)); // il testo in chiaro non e' mai inviato
            QVERIFY(sent.contains("Subject: Prova invio"));
            const QByteArray lc = "\n" + sent.toLower();
            QVERIFY(lc.contains("\nmessage-id:") && lc.contains("\ndate:"));
            QVERIFY(!sent.contains("multipart/signed"));
            QString err;
            const ParsedMessage pm = MessageParser::parse(sent, &err);
            QVERIFY2(pm.encryptedPgp, qPrintable(err));
            QVERIFY(pm.rawEncrypted.contains("BEGIN PGP MESSAGE"));
            GpgService g;
            g.setGnupgHome(home_);
            bool done = false, ok = false;
            QByteArray plain;
            SignatureStatus inner = SignatureStatus::None;
            QString who;
            g.decrypt(pm.rawEncrypted, [&](bool o, QByteArray p, QString) { ok = o; plain = p; done = true; },
                      [&](SignatureStatus st, QString w) { inner = st; who = w; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
            QVERIFY(ok);
            QVERIFY(plain.startsWith("Content-Type: text/plain"));
            QVERIFY(plain.contains(kBody));
            QVERIFY(plain.contains("\r\n"));
            QVERIFY(!plain.toLower().contains("bcc"));
            QVERIFY(readFile(e.dir + QStringLiteral("/gpg.log")).contains("--throw-keyids"));
            QCOMPARE(readFile(sentDir("a") + QLatin1Char('/') + files(sentDir("a")).first()), sent);
            if (sign) {
                QCOMPARE(inner, SignatureStatus::Valid);
                QVERIFY(who.contains(QStringLiteral("alice@example.test")));
            } else {
                QCOMPARE(inner, SignatureStatus::None);
            }
            // gpg e' stato invocato con una sola esecuzione per firma+cifratura, senza opzioni di password
            const QByteArray log = readFile(e.dir + QStringLiteral("/gpg.log"));
            QVERIFY(!log.contains("--passphrase"));
            QVERIFY(!log.contains("--pinentry-mode"));
            QCOMPARE(log.count(" --encrypt "), 1);
            QCOMPARE(log.count("--detach-sign"), 0);
            QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        }
    }
    void testNoDoubleSend() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        const OutgoingMessage m = msgFor(idA_, {QStringLiteral("bob@example.test")});
        QList<SendResult> res;
        QList<SendResult> inner;
        bool reentrantDone = false;
        s->send(m, false, false, [&](SendResult r) {
            res << r;
            // rientranza dal callback finale: rifiutata
            s->send(m, false, false, [&](SendResult r2) { inner << r2; reentrantDone = true; });
        });
        s->send(m, false, false, [&](SendResult r) { res << r; }); // doppio click
        QTRY_VERIFY_WITH_TIMEOUT(res.size() == 2 && reentrantDone, 60000);
        QCOMPARE(res.value(0).kind, SendResult::Busy); // la seconda chiamata risponde per prima
        QCOMPARE(res.value(1).kind, SendResult::Sent);
        QCOMPARE(inner.value(0).kind, SendResult::Busy);
        QCOMPARE(readFile(e.dir + QStringLiteral("/count")).count("run"), 1);
        QCOMPARE(files(sentDir("a")).size(), 1);
        // dopo la fine il Sender e' di nuovo utilizzabile
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        QCOMPARE(doSend(*s, m, false, false).kind, SendResult::Sent);
    }
    void testMsmtpTimeout() {
        const Env e = newEnv();
        QFile(e.dir + QStringLiteral("/sleep")).open(QIODevice::WriteOnly);
        auto s = makeSender(e, home_);
        s->setMsmtpTimeoutMs(600);
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false);
        QCOMPARE(r.kind, SendResult::SmtpFailed);
        QVERIFY(r.message.contains(QStringLiteral("incerto")));
        QVERIFY(files(sentDir("a")).isEmpty());
    }
    void testSentCopyFailureAfterSuccessReportsSentWithWarning() {
        const Env e = newEnv();
        QDir(root_.filePath(QStringLiteral("mail/a/Sent"))).removeRecursively();
        auto s = makeSender(e, home_);
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false);
        QCOMPARE(r.kind, SendResult::Sent);
        QVERIFY(!r.message.isEmpty());
        QCOMPARE(readFile(e.dir + QStringLiteral("/count")).count("run"), 1); // mai un secondo invio
    }
    void testReindexFailureDoesNotDelayOrChangeResult() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        s->setMuProgram(writeScript(QDir(e.dir), QStringLiteral("mufail"), "#!/bin/sh\nsleep 1\necho boom >&2\nexit 1\n"));
        QSignalSpy spy(s.get(), &Sender::reindexFinished);
        QElapsedTimer t;
        t.start();
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false);
        QVERIFY2(t.elapsed() < 900, "il callback non deve attendere mu index");
        QCOMPARE(r.kind, SendResult::Sent);
        QVERIFY(r.message.isEmpty());
        QCOMPARE(files(sentDir("a")).size(), 1);
        QVERIFY(spy.wait(10000));
        QCOMPARE(spy.at(0).at(0).toBool(), false);
        QVERIFY(spy.at(0).at(1).toString().contains(QStringLiteral("boom")));
    }
    void testDestroyDuringReindexKeepsCopyAndNoCrash() {
        const Env e = newEnv();
        const QString slowMu = writeScript(QDir(e.dir), QStringLiteral("muslow"), "#!/bin/sh\nsleep 1\necho done >> \"$(dirname \"$0\")/mu.done\"\n");
        std::unique_ptr<Sender> s = makeSender(e, home_);
        s->setMuProgram(slowMu);
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false);
        QCOMPARE(r.kind, SendResult::Sent);
        s.reset(); // Sender distrutto mentre mu index gira
        QCOMPARE(files(sentDir("a")).size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(e.dir + QStringLiteral("/mu.done")), 10000); // la reindicizzazione prosegue
    }
    void testCallbackMayDeleteSender() {
        const Env e = newEnv();
        std::unique_ptr<Sender> s = makeSender(e, home_);
        bool called = false;
        Sender* raw = s.get();
        raw->send(msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false, [&](SendResult r) {
            called = true;
            QCOMPARE(r.kind, SendResult::Sent);
            s.reset(); // delete diretto nel callback
        });
        QTRY_VERIFY_WITH_TIMEOUT(called, 60000);
        QTest::qWait(300); // nessun use-after-free
        QVERIFY(!s);
    }
    void testDestroyDuringGpgPhaseNeverCallsBack() {
        const Env e = newEnv();
        bool called = false;
        {
            auto s = makeSender(e, home_);
            // la risoluzione della chiave usa gpg vero, la firma si blocca
            s->gpg().setGpgProgram(writeScript(QDir(e.dir), QStringLiteral("gpgslow"),
                                               "#!/bin/sh\ncase \"$*\" in *--detach-sign*) exec sleep 30;; *) exec gpg \"$@\";; esac\n"));
            s->send(msgFor(idA_, {QStringLiteral("bob@example.test")}), true, false, [&](SendResult) { called = true; });
            QTest::qWait(500);
        }
        QTest::qWait(300);
        QVERIFY(!called);
        QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));
    }
    void testKeyIdResolvedToFingerprintNotDecoy() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        QCOMPARE(doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), true, true).kind, SendResult::Sent);
        const QString log = QString::fromUtf8(readFile(e.dir + QStringLiteral("/gpg.log")));
        static const QRegularExpression lu(QStringLiteral("--local-user ([0-9A-F]{40}) "));
        static const QRegularExpression et(QStringLiteral("--encrypt-to ([0-9A-F]{40}) "));
        const auto m1 = lu.match(log), m2 = et.match(log);
        QVERIFY2(m1.hasMatch() && m2.hasMatch(), qPrintable(log));
        QCOMPARE(m1.captured(1), m2.captured(1));
        QVERIFY(!log.contains(QStringLiteral("malice")));
        QVERIFY(!log.contains(QStringLiteral("--local-user alice")));
        // l'impronta e' quella di Alice (non delle chiavi civetta)
        GpgService g;
        g.setGnupgHome(home_);
        QString err;
        QCOMPARE(g.resolveSecretKey(QStringLiteral("alice@example.test"), &err), m1.captured(1));
        QCOMPARE(g.resolveSecretKey(QStringLiteral("<ALICE@example.test>"), &err), m1.captured(1));
        QVERIFY(g.resolveSecretKey(QStringLiteral("alice@example.test.evil"), &err) != m1.captured(1));
        QVERIFY(g.resolveSecretKey(QStringLiteral("alice"), &err).isEmpty()); // niente corrispondenze parziali
        QVERIFY(g.resolveSecretKey(QStringLiteral("--homedir=/tmp"), &err).isEmpty());
        QCOMPARE(g.resolveSecretKey(m1.captured(1), &err), m1.captured(1));
    }
    void testAmbiguousKeyIdFailsNeverGuesses() {
        const Env e = newEnv();
        auto s = makeSender(e, homeAmb_);
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), true, false);
        QCOMPARE(r.kind, SendResult::SignFailed);
        QVERIFY(r.message.contains(QStringLiteral("Piu' chiavi")));
        QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));
        QVERIFY(!readFile(e.dir + QStringLiteral("/gpg.log")).contains("--detach-sign"));
    }
    void testSecretFingerprintParsing() {
        const QByteArray c =
            "sec:u:255:22:AAAA:1:::u:::scESC:::+::ed25519:::0:\nfpr:::::::::1111111111111111111111111111111111111111:\n"
            "uid:u::::1::H::Uno <uno@example.test>::::::::::0:\nssb:u:255:18:BBBB:1::::::e:::+::cv25519::\n"
            "fpr:::::::::2222222222222222222222222222222222222222:\n"
            "sec:e:255:22:CCCC:1:::u:::scESC:::+::ed25519:::0:\nfpr:::::::::3333333333333333333333333333333333333333:\n"
            "uid:e::::1::H::Uno <uno@example.test>::::::::::0:\n"
            "sec:u:255:22:DDDD:1:::u:::cE:::+::ed25519:::0:\nfpr:::::::::4444444444444444444444444444444444444444:\n"
            "uid:u::::1::H::Uno <uno@example.test>::::::::::0:\n";
        QCOMPARE(GpgService::secretFingerprintsForAddress(c, QStringLiteral("UNO@example.test")),
                 QStringList{QStringLiteral("1111111111111111111111111111111111111111")});
        QVERIFY(GpgService::secretFingerprintsForAddress(c, QStringLiteral("un@example.test")).isEmpty());
    }
    void testSignedWithSignatureHasNoFragileLines() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        OutgoingMessage m = msgFor(idA_, {QStringLiteral("bob@example.test")});
        m.identity.signature = QStringLiteral("Alice");
        m.plainText = QStringLiteral("riga con spazio  \nFrom qui\nfine");
        m.html = QStringLiteral("<p>html </p>");
        QCOMPARE(doSend(*s, m, true, false).kind, SendResult::Sent);
        const QByteArray sent = readFile(e.dir + QStringLiteral("/stdin"));
        const ParsedMessage pm = MessageParser::parse(sent);
        QVERIFY(pm.signedPgp);
        for (const QByteArray& l : pm.rawSignedPart.split('\n')) {
            QByteArray x = l;
            QVERIFY2(x.endsWith('\r') || l == pm.rawSignedPart.split('\n').last(), "riga senza CRLF");
            if (x.endsWith('\r')) x.chop(1);
            QVERIFY2(!x.endsWith(' ') && !x.endsWith('\t'), x.constData());
            QVERIFY2(!x.startsWith("From "), x.constData());
        }
        GpgService g;
        g.setGnupgHome(home_);
        SignatureStatus st = SignatureStatus::None;
        bool done = false;
        g.verify(pm.rawSignedPart, pm.rawSignature, [&](SignatureStatus x, QString) { st = x; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QCOMPARE(st, SignatureStatus::Valid);
        // copia in Inviata == byte inviati
        QCOMPARE(readFile(sentDir("a") + QLatin1Char('/') + files(sentDir("a")).first()), sent);
    }
    void testEncryptOnlyAlsoEncryptsToOwnKeyAndFailureKinds() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        QCOMPARE(doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, true).kind, SendResult::Sent);
        const QByteArray log = readFile(e.dir + QStringLiteral("/gpg.log"));
        QVERIFY(log.contains("--encrypt-to "));
        QVERIFY(!log.contains("--local-user"));
        QVERIFY(log.contains("--throw-keyids"));
        const QByteArray sent = readFile(e.dir + QStringLiteral("/stdin"));
        const QByteArray copy = readFile(sentDir("a") + QLatin1Char('/') + files(sentDir("a")).first());
        QCOMPARE(copy, sent);
        QVERIFY(copy.contains("BEGIN PGP MESSAGE"));
        QVERIFY(!copy.contains(kBody));
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        // gpg che fallisce con INV_RECP: EncryptFailed, anche se la firma era richiesta
        const QString fk = writeScript(QDir(e.dir), QStringLiteral("gpginv"),
                                       "#!/bin/sh\ncase \"$*\" in *--encrypt\\ *) echo '[GNUPG:] INV_RECP 1 x' >&2; exit 2;; *) exec gpg \"$@\";; esac\n");
        s->gpg().setGpgProgram(fk);
        QCOMPARE(doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), true, true).kind, SendResult::EncryptFailed);
    }
    void testMsmtpCrashIsUncertain() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        s->setMsmtpProgram(writeScript(QDir(e.dir), QStringLiteral("msmtpcrash"), "#!/bin/sh\ncat >/dev/null\nkill -9 $$\n"));
        const SendResult r = doSend(*s, msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false);
        QCOMPARE(r.kind, SendResult::SmtpFailed);
        QVERIFY(r.message.contains(QStringLiteral("incerto")));
        QVERIFY(files(sentDir("a")).isEmpty());
    }
    void testHostileRecipientsRejected() {
        const Env e = newEnv();
        auto s = makeSender(e, home_);
        const QList<QStringList> bad{
            {QStringLiteral("-oProxy@example.test")},
            {QStringLiteral("bob@example.test"), QStringLiteral("-X@example.test")},
            {QStringLiteral("büro@example.test")},
            {QStringLiteral("bob@example.test -oProxy@example.test")},
            {},
        };
        for (const QStringList& to : bad) {
            const SendResult r = doSend(*s, msgFor(idA_, to), false, false);
            QVERIFY2(r.kind == SendResult::BuildFailed, qPrintable(to.join('|')));
            QVERIFY(!QFile::exists(e.dir + QStringLiteral("/count")));
            QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        }
        // centinaia di destinatari: argomenti espliciti, tutti presenti
        QStringList many;
        for (int i = 0; i < 300; ++i) many << QStringLiteral("u%1@example.test").arg(i);
        QCOMPARE(doSend(*s, msgFor(idA_, many), false, false).kind, SendResult::Sent);
        QCOMPARE(readFile(e.dir + QStringLiteral("/argv")).split('\n').size(), 4 + 300 + 1);
        QVERIFY(QTest::qWaitFor([&] { return !s->busy(); }, 1000));
        // oltre il limite: rifiutato
        QStringList tooMany;
        for (int i = 0; i < Sender::kMaxRecipients + 1; ++i) tooMany << QStringLiteral("v%1@example.test").arg(i);
        QCOMPARE(doSend(*s, msgFor(idA_, tooMany), false, false).kind, SendResult::BuildFailed);
    }
    void testDestroyWhileSendingNeverCallsBack() {
        const Env e = newEnv();
        QFile(e.dir + QStringLiteral("/sleep")).open(QIODevice::WriteOnly);
        bool called = false;
        {
            auto s = makeSender(e, home_);
            s->send(msgFor(idA_, {QStringLiteral("bob@example.test")}), false, false, [&](SendResult) { called = true; });
            QTest::qWait(300);
        }
        QTest::qWait(300);
        QVERIFY(!called);
    }
};

QTEST_MAIN(T)
#include "test_sender.moc"
