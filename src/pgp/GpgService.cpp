#include "pgp/GpgService.h"

#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
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
                       std::function<void(Result)> done) {
    auto* proc = new QProcess(this);
    procs_.append(proc);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.remove(QStringLiteral("LANGUAGE"));
    if (!home_.isEmpty()) env.insert(QStringLiteral("GNUPGHOME"), home_);
    proc->setProcessEnvironment(env);

    auto res = std::make_shared<Result>();
    auto finished = std::make_shared<bool>(false);
    const qint64 maxOut = maxPlain_, maxErr = maxStatus_;
    auto finish = [this, proc, res, finished, done = std::move(done)](int code) {
        if (*finished) return;
        *finished = true;
        res->code = code;
        if (!res->overflow) {
            res->out += proc->readAllStandardOutput();
            res->err += proc->readAllStandardError();
        }
        procs_.removeOne(proc);
        proc->disconnect();
        proc->deleteLater();
        // consegna in coda: mai dentro verify()/decrypt(); annullata se il servizio muore
        QTimer::singleShot(0, this, [done, r = *res]() mutable {
            if (done) done(std::move(r));
        });
    };
    auto overflow = [proc, res]() {
        res->overflow = true;
        res->out.clear();
        res->err.clear();
        proc->kill();
    };
    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, res, maxOut, overflow]() {
        const QByteArray d = proc->readAllStandardOutput();
        if (res->overflow) return;
        res->out += d;
        if (res->out.size() > maxOut) overflow();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [proc, res, maxErr, overflow]() {
        const QByteArray d = proc->readAllStandardError();
        if (res->overflow) return;
        res->err += d;
        if (res->err.size() > maxErr) overflow();
    });
    connect(proc, &QProcess::errorOccurred, this, [proc, res, finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            res->startFailed = true;
            res->startError = proc->errorString();
            finish(-1);
        }
    });
    connect(proc, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus s) {
        finish(s == QProcess::CrashExit ? -1 : code);
    });
    auto* t = new QTimer(proc);
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, proc, [proc, res]() {
        if (proc->state() != QProcess::NotRunning) {
            res->timedOut = true;
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
    start(args, signedPart, kVerifyTimeoutMs, [dir, done](Result r) {
        QString who;
        SignatureStatus s = SignatureStatus::Invalid;
        if (!r.timedOut && !r.startFailed && !r.overflow) s = classifyVerifyStatus(r.err, r.code, &who);
        if (done) done(s, who);
    });
}

void GpgService::decrypt(const QByteArray& encrypted, std::function<void(bool, QByteArray, QString)> done,
                         std::function<void(SignatureStatus, QString)> innerSignature) {
    const QStringList args{QStringLiteral("--batch"), QStringLiteral("--no-tty"), QStringLiteral("--status-fd"),
                           QStringLiteral("2"), QStringLiteral("--decrypt")};
    start(args, encrypted, kDecryptTimeoutMs, [done, innerSignature](Result r) {
        if (!done) return;
        if (r.startFailed) {
            done(false, {}, QObject::tr("Impossibile avviare gpg: %1").arg(r.startError));
            return;
        }
        if (r.timedOut) {
            done(false, {}, QObject::tr("Tempo scaduto durante la decifrazione."));
            return;
        }
        if (r.overflow) {
            done(false, {}, QObject::tr("Messaggio decifrato troppo grande"));
            return;
        }
        bool okay = false, endDec = false, failed = false, cancelled = false, noData = false, otherError = false,
             sigClass = false;
        for (const StatusLine& l : parseStatus(r.err)) {
            const QByteArray& k = l.keyword;
            if (k == "DECRYPTION_OKAY") okay = true;
            else if (k == "END_DECRYPTION") endDec = true;
            else if (k == "DECRYPTION_FAILED" || k == "BADMDC") failed = true;
            else if (k == "NODATA") noData = true;
            else if (k == "ERRSIG" || k == "NO_PUBKEY" || k == "BADSIG" || k == "EXPKEYSIG" || k == "EXPSIG" ||
                     k == "REVKEYSIG")
                sigClass = true;
            else if (k == "ERROR") {
                otherError = true;
                // "ERROR <location> <code>": codice & 0xFFFF == 99: GPG_ERR_CANCELED
                const QList<QByteArray> p = l.rest.split(' ');
                bool numOk = false;
                const uint code = p.size() >= 2 ? p.at(1).trimmed().toUInt(&numOk) : 0;
                if (numOk && (code & 0xFFFF) == 99) cancelled = true;
            }
        }
        const bool exitOk = r.code == 0 || (sigClass && !otherError && !noData);
        if (okay && endDec && !failed && exitOk) {
            if (innerSignature) {
                QString who;
                const SignatureStatus s = sigClass || r.err.contains("GOODSIG") || r.err.contains("VALIDSIG")
                                              ? classifyVerifyStatus(r.err, r.code, &who)
                                              : SignatureStatus::None;
                innerSignature(s, who);
            }
            done(true, r.out, QString());
            return;
        }
        QString msg;
        if (cancelled) msg = QObject::tr("Decifrazione annullata.");
        else if (noData) msg = QObject::tr("Il messaggio non contiene dati OpenPGP validi.");
        else msg = QObject::tr("Decifrazione non riuscita (chiave segreta mancante, password errata o dati alterati).");
        done(false, QByteArray(), msg); // mai testo parziale
    });
}

namespace {

constexpr int kSignTimeoutMs = 5 * 60 * 1000; // include l'attesa di pinentry
constexpr int kListKeysTimeoutMs = 30'000;

QString micalgFor(int hashAlgo) {
    switch (hashAlgo) {
    case 1: return QStringLiteral("pgp-md5");
    case 2: return QStringLiteral("pgp-sha1");
    case 3: return QStringLiteral("pgp-ripemd160");
    case 8: return QStringLiteral("pgp-sha256");
    case 9: return QStringLiteral("pgp-sha384");
    case 10: return QStringLiteral("pgp-sha512");
    case 11: return QStringLiteral("pgp-sha224");
    default: return QString();
    }
}

bool statusCancelled(const QList<StatusLine>& st) {
    for (const StatusLine& l : st) {
        if (l.keyword != "ERROR") continue;
        const QList<QByteArray> p = l.rest.split(' ');
        bool ok = false;
        const uint code = p.size() >= 2 ? p.at(1).trimmed().toUInt(&ok) : 0;
        if (ok && (code & 0xFFFF) == 99) return true;
    }
    return false;
}

QString startProblem(const GpgService*, bool startFailed, const QString& startError, bool timedOut, bool overflow,
                     const QString& what) {
    if (startFailed) return QObject::tr("Impossibile avviare gpg: %1").arg(startError);
    if (timedOut) return QObject::tr("Tempo scaduto durante %1.").arg(what);
    if (overflow) return QObject::tr("Risposta di gpg troppo grande.");
    return QString();
}

// uid "Nome <a@b>" (con escape \xNN di --with-colons) -> "a@b" minuscolo
QString emailOfUid(const QByteArray& raw) {
    QByteArray b;
    for (int i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size() && raw[i + 1] == 'x') {
            bool ok = false;
            const int v = raw.mid(i + 2, 2).toInt(&ok, 16);
            if (ok) { b.append(char(v)); i += 3; continue; }
        }
        b.append(raw[i]);
    }
    const QString dec = QString::fromUtf8(b);
    const int lt = dec.lastIndexOf(QLatin1Char('<')), gt = dec.lastIndexOf(QLatin1Char('>'));
    QString addr;
    if (lt >= 0 && gt > lt) addr = dec.mid(lt + 1, gt - lt - 1);
    else if (dec.contains(QLatin1Char('@')) && !dec.contains(QLatin1Char(' '))) addr = dec;
    return addr.trimmed().toLower();
}

} // namespace

bool GpgService::isSafeAddress(const QString& a) {
    if (a.isEmpty() || a.size() > 320 || a.startsWith(QLatin1Char('-'))) return false;
    for (const QChar c : a) {
        const ushort u = c.unicode();
        if (u <= 0x20 || u >= 0x7f) return false;
        if (c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('"') || c == QLatin1Char('\\') ||
            c == QLatin1Char(','))
            return false;
    }
    return a.count(QLatin1Char('@')) == 1 && !a.startsWith(QLatin1Char('@')) && !a.endsWith(QLatin1Char('@'));
}

bool GpgService::isSafeKeyId(const QString& k) {
    if (k.isEmpty() || k.size() > 320 || !(k.at(0).isLetterOrNumber() && k.at(0).unicode() < 0x7f)) return false;
    for (const QChar c : k) {
        const ushort u = c.unicode();
        const bool ok = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == QLatin1Char('@') ||
                        c == QLatin1Char('.') || c == QLatin1Char('_') || c == QLatin1Char('+') || c == QLatin1Char('-') ||
                        c == QLatin1Char('<') || c == QLatin1Char('>');
        if (!ok) return false;
    }
    return true;
}

void GpgService::sign(const QByteArray& mimeBody, const QString& keyId,
                      std::function<void(bool, QByteArray, QString, QString)> done) {
    if (!isSafeKeyId(keyId)) {
        QTimer::singleShot(0, this, [done]() { if (done) done(false, {}, {}, QObject::tr("Identificativo della chiave di firma non valido.")); });
        return;
    }
    const QStringList args{QStringLiteral("--batch"), QStringLiteral("--no-tty"), QStringLiteral("--status-fd"),
                           QStringLiteral("2"), QStringLiteral("--armor"), QStringLiteral("--digest-algo"),
                           QStringLiteral("SHA256"), QStringLiteral("--local-user"), keyId,
                           QStringLiteral("--detach-sign")};
    start(args, mimeBody, kSignTimeoutMs, [this, done](Result r) {
        if (!done) return;
        const QString prob = startProblem(this, r.startFailed, r.startError, r.timedOut, r.overflow, QObject::tr("la firma"));
        if (!prob.isEmpty()) { done(false, {}, {}, prob); return; }
        const QList<StatusLine> st = parseStatus(r.err);
        QString micalg;
        bool created = false;
        for (const StatusLine& l : st)
            if (l.keyword == "SIG_CREATED") {
                // "<tipo> <algo chiave> <algo hash> <classe> <tempo> <fpr>"
                const QList<QByteArray> f = l.rest.split(' ');
                bool ok = false;
                const int h = f.size() >= 3 ? f.at(2).toInt(&ok) : 0;
                if (ok) { micalg = micalgFor(h); created = !micalg.isEmpty(); }
            }
        if (r.code == 0 && created && r.out.contains("BEGIN PGP SIGNATURE")) {
            done(true, r.out, micalg, QString());
            return;
        }
        done(false, {}, {},
             statusCancelled(st) ? QObject::tr("Firma annullata.")
                                 : QObject::tr("Firma non riuscita (chiave segreta mancante, agent non disponibile o password errata)."));
    });
}

void GpgService::encrypt(const QByteArray& mimeBody, const QStringList& recipients, const QString& signKeyId,
                         std::function<void(bool, QByteArray, QString)> done) {
    encrypt(mimeBody, recipients, signKeyId, signKeyId,
            [done](bool ok, QByteArray a, QString e, EncryptFailure) { if (done) done(ok, std::move(a), std::move(e)); });
}

void GpgService::encrypt(const QByteArray& mimeBody, const QStringList& recipients, const QString& signKeyId,
                         const QString& encryptToKey,
                         std::function<void(bool, QByteArray, QString, EncryptFailure)> done) {
    auto fail = [this, done](const QString& m, EncryptFailure k) {
        QTimer::singleShot(0, this, [done, m, k]() { if (done) done(false, {}, m, k); });
    };
    if (recipients.isEmpty()) return fail(QObject::tr("Nessun destinatario per la cifratura."), EncryptFailure::Encrypt);
    const bool signToo = !signKeyId.isEmpty();
    if (signToo && !isSafeKeyId(signKeyId))
        return fail(QObject::tr("Identificativo della chiave di firma non valido."), EncryptFailure::Sign);
    if (!encryptToKey.isEmpty() && !isSafeKeyId(encryptToKey))
        return fail(QObject::tr("Identificativo della chiave personale non valido."), EncryptFailure::Encrypt);
    QStringList args{QStringLiteral("--batch"), QStringLiteral("--no-tty"), QStringLiteral("--status-fd"),
                     QStringLiteral("2"), QStringLiteral("--armor"), QStringLiteral("--trust-model"),
                     QStringLiteral("always"), QStringLiteral("--throw-keyids"),
                     QStringLiteral("--no-auto-key-locate"), QStringLiteral("--no-auto-key-retrieve")};
    if (signToo)
        args << QStringLiteral("--digest-algo") << QStringLiteral("SHA256") << QStringLiteral("--local-user") << signKeyId
             << QStringLiteral("--sign");
    if (!encryptToKey.isEmpty()) args << QStringLiteral("--encrypt-to") << encryptToKey;
    args << QStringLiteral("--encrypt");
    for (const QString& r : recipients) {
        if (!isSafeAddress(r))
            return fail(QObject::tr("Destinatario non valido per la cifratura: %1").arg(r.left(80)), EncryptFailure::Encrypt);
        args << QStringLiteral("--recipient") << QLatin1Char('<') + r + QLatin1Char('>');
    }
    start(args, mimeBody, kSignTimeoutMs, [this, done, signToo](Result r) {
        if (!done) return;
        const QString prob = startProblem(this, r.startFailed, r.startError, r.timedOut, r.overflow, QObject::tr("la cifratura"));
        if (!prob.isEmpty()) { done(false, {}, prob, signToo ? EncryptFailure::Sign : EncryptFailure::Encrypt); return; }
        const QList<StatusLine> st = parseStatus(r.err);
        bool begin = false, end = false, created = false, invalid = false;
        for (const StatusLine& l : st) {
            if (l.keyword == "BEGIN_ENCRYPTION") begin = true;
            else if (l.keyword == "END_ENCRYPTION") end = true;
            else if (l.keyword == "SIG_CREATED") created = true;
            else if (l.keyword == "INV_RECP" || l.keyword == "NO_RECP") invalid = true;
        }
        if (r.code == 0 && begin && end && !invalid && (!signToo || created) && r.out.contains("BEGIN PGP MESSAGE")) {
            done(true, r.out, QString(), EncryptFailure::None);
            return;
        }
        const bool cancelled = statusCancelled(st);
        // Problema di firma solo se la firma era richiesta e non e' dovuta a un destinatario non valido.
        const EncryptFailure kind = (signToo && !invalid && !created) ? EncryptFailure::Sign : EncryptFailure::Encrypt;
        done(false, {},
             cancelled ? QObject::tr("Operazione annullata (password non fornita).")
             : invalid ? QObject::tr("Chiave del destinatario non utilizzabile.")
             : kind == EncryptFailure::Sign ? QObject::tr("Firma non riuscita.")
                                            : QObject::tr("Cifratura non riuscita."),
             kind);
    });
}

QSet<QString> GpgService::usableKeyAddresses(const QByteArray& colons) {
    QSet<QString> out;
    bool inKey = false, pubOk = false, encOk = false;
    QStringList uids;
    auto bad = [](const QByteArray& v) { return v == "e" || v == "r" || v == "i" || v == "d" || v == "n"; };
    auto flush = [&]() {
        if (inKey && pubOk && encOk)
            for (const QString& a : std::as_const(uids)) out.insert(a);
        uids.clear();
        inKey = pubOk = encOk = false;
    };
    for (const QByteArray& line : colons.split('\n')) {
        const QList<QByteArray> f = line.split(':');
        if (f.size() < 2) continue;
        const QByteArray& t = f.at(0);
        const QByteArray st = f.at(1).left(1);
        const QByteArray caps = f.size() > 11 ? f.at(11) : QByteArray();
        if (t == "pub") {
            flush();
            inKey = true;
            pubOk = !bad(st) && !caps.contains('D');
            encOk = caps.contains('e');
        } else if (t == "sub" && inKey) {
            if (!bad(st) && caps.contains('e')) encOk = true;
        } else if (t == "uid" && inKey) {
            if (st != "r" && st != "e" && st != "i" && st != "n" && f.size() > 9) {
                const QString a = emailOfUid(f.at(9));
                if (!a.isEmpty()) uids << a;
            }
        }
    }
    flush();
    return out;
}

bool GpgService::runList(const QStringList& extra, QByteArray* result) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.remove(QStringLiteral("LANGUAGE"));
    if (!home_.isEmpty()) env.insert(QStringLiteral("GNUPGHOME"), home_);
    p.setProcessEnvironment(env);
    QStringList args{QStringLiteral("--batch"), QStringLiteral("--no-tty"), QStringLiteral("--no-auto-key-locate"),
                     QStringLiteral("--with-colons")};
    args += extra;
    p.start(gpg_, args);
    QByteArray out;
    QElapsedTimer t;
    t.start();
    bool tooBig = false;
    p.closeWriteChannel();
    while (p.state() != QProcess::NotRunning && t.elapsed() < kListKeysTimeoutMs) {
        p.waitForReadyRead(100);
        out += p.readAllStandardOutput();
        p.readAllStandardError();
        if (out.size() > maxPlain_) { tooBig = true; break; }
    }
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(2000);
        return false;
    }
    out += p.readAllStandardOutput();
    const bool ok = !tooBig && out.size() <= maxPlain_ && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (ok && result) *result = out;
    return ok;
}

QStringList GpgService::missingKeys(const QStringList& addresses) {
    QStringList missing;
    QSet<QString> usable;
    QByteArray out;
    const bool ok = runList({QStringLiteral("--list-keys")}, &out);
    if (ok) usable = usableKeyAddresses(out);
    for (const QString& a : addresses) {
        const QString k = a.trimmed().toLower();
        if (!ok || !isSafeAddress(k) || !usable.contains(k)) missing << a.trimmed();
    }
    return missing;
}

QStringList GpgService::secretFingerprintsForAddress(const QByteArray& colons, const QString& address) {
    QStringList out;
    const QString want = address.trimmed().toLower();
    bool inKey = false, ok = false, haveFpr = false, match = false;
    QString fpr;
    auto bad = [](const QByteArray& v) { return v == "e" || v == "r" || v == "i" || v == "d" || v == "n"; };
    auto flush = [&]() {
        if (inKey && ok && match && !fpr.isEmpty() && !out.contains(fpr)) out << fpr;
        inKey = ok = haveFpr = match = false;
        fpr.clear();
    };
    for (const QByteArray& line : colons.split('\n')) {
        const QList<QByteArray> f = line.split(':');
        if (f.size() < 2) continue;
        const QByteArray& t = f.at(0);
        const QByteArray st = f.at(1).left(1);
        if (t == "sec") {
            flush();
            inKey = true;
            const QByteArray caps = f.size() > 11 ? f.at(11) : QByteArray();
            ok = !bad(st) && !caps.contains('D') && (caps.contains('s') || caps.contains('S'));
        } else if (t == "fpr" && inKey && !haveFpr) {
            haveFpr = true;
            fpr = f.size() > 9 ? QString::fromLatin1(f.at(9)) : QString();
        } else if (t == "uid" && inKey && st != "r" && st != "e" && st != "i" && f.size() > 9) {
            if (emailOfUid(f.at(9)) == want) match = true;
        }
    }
    flush();
    return out;
}

QString GpgService::resolveSecretKey(const QString& keyId, QString* error) {
    auto fail = [&](const QString& m) { if (error) *error = m; return QString(); };
    const QString k = keyId.trimmed();
    static const QRegularExpression hexId(QStringLiteral("^(0x)?[0-9A-Fa-f]{16,40}$"));
    if (hexId.match(k).hasMatch()) return k; // fingerprint / id lungo: nessuna ambiguita' di indirizzo
    QString addr = k;
    if (addr.startsWith(QLatin1Char('<')) && addr.endsWith(QLatin1Char('>'))) addr = addr.mid(1, addr.size() - 2);
    if (!isSafeAddress(addr)) return fail(QObject::tr("Chiave di firma non riconoscibile: usare l'impronta o l'indirizzo."));
    QByteArray out;
    if (!runList({QStringLiteral("--list-secret-keys")}, &out)) return fail(QObject::tr("Impossibile leggere le chiavi segrete con gpg."));
    const QStringList fprs = secretFingerprintsForAddress(out, addr);
    if (fprs.isEmpty()) return fail(QObject::tr("Nessuna chiave segreta utilizzabile per %1.").arg(addr));
    if (fprs.size() > 1)
        return fail(QObject::tr("Piu' chiavi segrete per %1: indicare l'impronta nell'identita'.").arg(addr));
    return fprs.first();
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
