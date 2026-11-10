#include "pgp/GpgService.h"

#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <memory>

namespace boma {

namespace {

constexpr int kVerifyTimeoutMs = 30'000;
constexpr int kDecryptTimeoutMs = 5 * 60 * 1000; // include l'attesa di pinentry
constexpr int kMaxSigner = 300;

struct StatusLine {
    QByteArray keyword;
    QByteArray rest;
};

QList<StatusLine> parseStatus(const QByteArray& err) {
    QList<StatusLine> out;
    static const QByteArray prefix = "[GNUPG:] ";
    for (const QByteArray& l : err.split('\n')) {
        QByteArray line = l;
        if (line.endsWith('\r')) line.chop(1);
        if (!line.startsWith(prefix)) continue;
        const QByteArray body = line.mid(prefix.size());
        const int sp = body.indexOf(' ');
        out.append({sp < 0 ? body : body.left(sp), sp < 0 ? QByteArray() : body.mid(sp + 1)});
    }
    return out;
}

// "KEYID user id..." -> user id (o KEYID se manca)
QString signerOf(const QByteArray& rest) {
    const int sp = rest.indexOf(' ');
    QString s = QString::fromUtf8(sp < 0 ? rest : rest.mid(sp + 1)).trimmed();
    if (s.isEmpty()) s = QString::fromUtf8(sp < 0 ? rest : rest.left(sp)).trimmed();
    return s.left(kMaxSigner);
}

QString keyIdOf(const QByteArray& rest) {
    const int sp = rest.indexOf(' ');
    return QString::fromUtf8(sp < 0 ? rest : rest.left(sp)).trimmed().left(kMaxSigner);
}

int toSeconds(const QString& v, int dflt) {
    bool ok = false;
    const int n = v.trimmed().toInt(&ok);
    return ok && n >= 0 ? n : dflt;
}

} // namespace

GpgService::GpgService(QObject* parent) : QObject(parent) {}
GpgService::GpgService(const QString& gpgProgram, QObject* parent) : QObject(parent), gpg_(gpgProgram) {}

GpgService::~GpgService() {
    const auto procs = procs_;
    procs_.clear();
    for (QProcess* p : procs) {
        p->disconnect();
        if (p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(2000);
        }
        delete p;
    }
}

void GpgService::start(const QStringList& args, const QByteArray& stdinData, int timeoutMs,
                       std::function<void(int, bool, bool, QByteArray, QByteArray)> done) {
    auto* proc = new QProcess(this);
    procs_.append(proc);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.remove(QStringLiteral("LANGUAGE"));
    if (!home_.isEmpty()) env.insert(QStringLiteral("GNUPGHOME"), home_);
    proc->setProcessEnvironment(env);

    struct State {
        QByteArray out, err;
        bool timedOut = false, finished = false;
    };
    auto st = std::make_shared<State>();
    auto finish = [this, proc, st, done = std::move(done)](int code, bool startFailed) {
        if (st->finished) return;
        st->finished = true;
        st->out += proc->readAllStandardOutput();
        st->err += proc->readAllStandardError();
        procs_.removeOne(proc);
        proc->disconnect();
        proc->deleteLater();
        if (done) done(code, st->timedOut, startFailed, st->out, st->err);
    };
    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, st]() { st->out += proc->readAllStandardOutput(); });
    connect(proc, &QProcess::readyReadStandardError, this, [proc, st]() { st->err += proc->readAllStandardError(); });
    connect(proc, &QProcess::errorOccurred, this, [finish, st](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish(-1, true);
    });
    connect(proc, &QProcess::finished, this, [finish, proc](int code, QProcess::ExitStatus s) {
        finish(s == QProcess::CrashExit ? -1 : code, false);
    });
    auto* t = new QTimer(proc);
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, proc, [proc, st]() {
        if (proc->state() != QProcess::NotRunning) {
            st->timedOut = true;
            proc->kill();
        }
    });
    t->start(timeoutMs);

    proc->start(gpg_, args);
    if (!stdinData.isEmpty()) proc->write(stdinData);
    proc->closeWriteChannel();
}

SignatureStatus GpgService::classifyVerifyStatus(const QByteArray& statusOutput, int exitCode, QString* signer) {
    bool good = false, valid = false, bad = false, unknown = false, expired = false, revoked = false;
    QString sBad, sRev, sExp, sUnk, sGood;
    for (const StatusLine& l : parseStatus(statusOutput)) {
        if (l.keyword == "GOODSIG") { good = true; sGood = signerOf(l.rest); }
        else if (l.keyword == "VALIDSIG") valid = true;
        else if (l.keyword == "BADSIG") { bad = true; sBad = signerOf(l.rest); }
        else if (l.keyword == "REVKEYSIG") { revoked = true; sRev = signerOf(l.rest); }
        else if (l.keyword == "EXPKEYSIG" || l.keyword == "EXPSIG") { expired = true; sExp = signerOf(l.rest); }
        else if (l.keyword == "ERRSIG" || l.keyword == "NO_PUBKEY") { unknown = true; sUnk = keyIdOf(l.rest); }
    }
    auto ret = [&](SignatureStatus s, const QString& who) {
        if (signer) *signer = who;
        return s;
    };
    // Il caso peggiore vince; Valid solo senza alcun segnale negativo e con uscita 0.
    if (bad) return ret(SignatureStatus::Invalid, sBad);
    if (revoked) return ret(SignatureStatus::RevokedKey, sRev);
    if (expired) return ret(SignatureStatus::ExpiredKey, sExp);
    if (unknown) return ret(SignatureStatus::UnknownKey, sUnk);
    if ((good || valid) && exitCode == 0) return ret(SignatureStatus::Valid, sGood);
    return ret(SignatureStatus::Invalid, QString());
}

void GpgService::verify(const QByteArray& signedPart, const QByteArray& signature,
                        std::function<void(SignatureStatus, QString)> done) {
    auto dir = std::make_shared<QTemporaryDir>(); // 0700, rimossa alla fine
    QString sigPath;
    bool wrote = false;
    if (dir->isValid()) {
        sigPath = dir->filePath(QStringLiteral("sig.bin"));
        QFile f(sigPath);
        wrote = f.open(QIODevice::WriteOnly) && f.write(signature) == signature.size();
        f.close();
    }
    if (!wrote) {
        QTimer::singleShot(0, this, [done]() { if (done) done(SignatureStatus::Invalid, QString()); });
        return;
    }
    const QStringList args{QStringLiteral("--batch"), QStringLiteral("--no-tty"), QStringLiteral("--status-fd"),
                           QStringLiteral("2"), QStringLiteral("--verify"), sigPath, QStringLiteral("-")};
    start(args, signedPart, kVerifyTimeoutMs,
          [dir, done](int code, bool timedOut, bool startFailed, QByteArray, QByteArray err) {
              QString who;
              SignatureStatus s = (timedOut || startFailed) ? SignatureStatus::Invalid
                                                            : classifyVerifyStatus(err, code, &who);
              if (s == SignatureStatus::Invalid && (timedOut || startFailed)) who.clear();
              if (done) done(s, who);
          });
}

void GpgService::decrypt(const QByteArray& encrypted,
                         std::function<void(bool, QByteArray, QString)> done) {
    const QStringList args{QStringLiteral("--batch"), QStringLiteral("--no-tty"), QStringLiteral("--status-fd"),
                           QStringLiteral("2"), QStringLiteral("--decrypt")};
    start(args, encrypted, kDecryptTimeoutMs,
          [done](int code, bool timedOut, bool startFailed, QByteArray out, QByteArray err) {
              if (!done) return;
              if (startFailed) {
                  done(false, {}, QObject::tr("Impossibile avviare gpg: %1").arg(QString::fromUtf8(err)));
                  return;
              }
              if (timedOut) {
                  done(false, {}, QObject::tr("Tempo scaduto durante la decifrazione."));
                  return;
              }
              bool okay = false, failed = false, cancelled = false, noData = false;
              for (const StatusLine& l : parseStatus(err)) {
                  if (l.keyword == "DECRYPTION_OKAY") okay = true;
                  else if (l.keyword == "DECRYPTION_FAILED") failed = true;
                  else if (l.keyword == "NODATA") noData = true;
                  else if (l.keyword == "ERROR") {
                      // "ERROR <location> <code>": 83886179 = operazione annullata
                      const QList<QByteArray> p = l.rest.split(' ');
                      bool numOk = false;
                      const uint code = p.size() >= 2 ? p.at(1).trimmed().toUInt(&numOk) : 0;
                      if (numOk && (code & 0xFFFF) == 99) cancelled = true; // GPG_ERR_CANCELED
                  }
              }
              if (okay && !failed && code == 0) {
                  done(true, out, QString());
                  return;
              }
              QString msg;
              if (cancelled) msg = QObject::tr("Decifrazione annullata.");
              else if (noData) msg = QObject::tr("Il messaggio non contiene dati OpenPGP validi.");
              else msg = QObject::tr("Decifrazione non riuscita (chiave segreta mancante, password errata o dati alterati).");
              done(false, QByteArray(), msg); // mai testo parziale
          });
}

GpgAgentInfo GpgService::parseGpgconfOptions(const QByteArray& out) {
    GpgAgentInfo info;
    bool sawDefault = false, sawMax = false;
    for (const QByteArray& l : out.split('\n')) {
        const QList<QByteArray> f = l.trimmed().split(':');
        if (f.size() < 2) continue;
        const QByteArray name = f.at(0);
        const bool isDef = name == "default-cache-ttl", isMax = name == "max-cache-ttl";
        if (!isDef && !isMax) continue;
        // nome:flags:level:desc:type:alt-type:argname:default:argdef:value
        const QString value = f.size() > 9 ? QString::fromUtf8(f.at(9)).trimmed() : QString();
        // valore vuoto = non impostato: default-cache-ttl 600 s, max-cache-ttl 7200 s (2 h)
        if (isDef) { info.defaultCacheTtl = toSeconds(value, 600); sawDefault = true; }
        else { info.maxCacheTtl = toSeconds(value, 7200); sawMax = true; }
    }
    info.available = sawDefault || sawMax;
    info.ttlIs300 = info.available && info.defaultCacheTtl == 300 && info.maxCacheTtl == 300;
    return info;
}

GpgAgentInfo GpgService::checkAgentCache() {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("gpgconf"), {QStringLiteral("--list-options"), QStringLiteral("gpg-agent")});
    if (!p.waitForFinished(3000)) {
        p.kill();
        p.waitForFinished(1000);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return {};
    return parseGpgconfOptions(p.readAllStandardOutput());
}

} // namespace boma
