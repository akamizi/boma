#include "send/Sender.h"

#include "core/MaildirWriter.h"

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

namespace boma {

namespace {

constexpr qint64 kMaxMsmtpOutput = 1024 * 1024;

// LF -> CRLF, idempotente (CRLF e CR isolati diventano CRLF).
QByteArray toCrlf(const QByteArray& in) {
    QByteArray s = in;
    s.replace("\r\n", "\n");
    s.replace('\r', '\n');
    s.replace("\n", "\r\n");
    return s;
}

struct Split {
    QByteArray outerHeaders; // CRLF, ogni riga termina con CRLF, senza riga vuota
    QByteArray inner;        // entita' MIME canonica (CRLF): intestazioni Content-* + riga vuota + corpo
    bool hasBcc = false;
};

// Separa le intestazioni del messaggio costruito: tutto cio' che e' Content-*
// appartiene all'entita' protetta; il resto (From/To/Subject/Date/Message-ID/
// MIME-Version...) resta fuori.
bool splitMessage(const QByteArray& built, Split* out) {
    QByteArray s = built;
    s.replace("\r\n", "\n");
    const int end = s.indexOf("\n\n");
    if (end < 0) return false;
    const QByteArray head = s.left(end), body = s.mid(end + 2);
    QList<QByteArray> groups;
    for (const QByteArray& l : head.split('\n')) {
        if (!l.isEmpty() && (l[0] == ' ' || l[0] == '\t') && !groups.isEmpty()) groups.last() += "\n" + l;
        else groups.append(l);
    }
    QByteArray outer, inner;
    for (const QByteArray& g : std::as_const(groups)) {
        if (g.isEmpty()) continue;
        const QByteArray name = g.left(g.indexOf(':')).trimmed().toLower();
        if (name == "bcc") out->hasBcc = true;
        if (name.startsWith("content-")) inner += g + "\n";
        else outer += g + "\n";
    }
    if (inner.isEmpty()) return false;
    out->outerHeaders = toCrlf(outer);
    out->inner = toCrlf(inner + "\n" + body);
    return true;
}

QByteArray newBoundary(const QByteArray& avoid) {
    for (;;) {
        QByteArray b = "=-boma-";
        for (int i = 0; i < 3; ++i) b += QByteArray::number(QRandomGenerator::global()->generate64(), 16);
        if (!avoid.contains(b)) return b;
    }
}

QByteArray assembleSigned(const Split& sp, const QByteArray& signedPart, const QByteArray& sig, const QString& micalg,
                          const QByteArray& boundary) {
    QByteArray m = sp.outerHeaders;
    m += "Content-Type: multipart/signed; micalg=" + micalg.toLatin1() + ";\r\n\tprotocol=\"application/pgp-signature\"; boundary=\"" +
         boundary + "\"\r\n\r\n";
    m += "This is an OpenPGP/MIME signed message (RFC 4880 and 3156)\r\n";
    m += "--" + boundary + "\r\n" + signedPart + "\r\n";
    m += "--" + boundary + "\r\n";
    m += "Content-Type: application/pgp-signature; name=\"signature.asc\"\r\n"
         "Content-Description: OpenPGP digital signature\r\n"
         "Content-Disposition: attachment; filename=\"signature.asc\"\r\n\r\n";
    m += toCrlf(sig);
    if (!m.endsWith("\r\n")) m += "\r\n";
    m += "--" + boundary + "--\r\n";
    return m;
}

QByteArray assembleEncrypted(const Split& sp, const QByteArray& armored, const QByteArray& boundary) {
    QByteArray m = sp.outerHeaders;
    m += "Content-Type: multipart/encrypted;\r\n\tprotocol=\"application/pgp-encrypted\"; boundary=\"" + boundary + "\"\r\n\r\n";
    m += "This is an OpenPGP/MIME encrypted message (RFC 4880 and 3156)\r\n";
    m += "--" + boundary + "\r\n"
         "Content-Type: application/pgp-encrypted\r\n"
         "Content-Description: PGP/MIME version identification\r\n\r\n"
         "Version: 1\r\n\r\n";
    m += "--" + boundary + "\r\n"
         "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n"
         "Content-Description: OpenPGP encrypted message\r\n"
         "Content-Disposition: inline; filename=\"encrypted.asc\"\r\n\r\n";
    m += toCrlf(armored);
    if (!m.endsWith("\r\n")) m += "\r\n";
    m += "--" + boundary + "--\r\n";
    return m;
}

bool safeMsmtpAccount(const QString& a) {
    if (a.isEmpty() || a.size() > 200 || a.startsWith(QLatin1Char('-'))) return false;
    for (const QChar c : a) {
        const ushort u = c.unicode();
        if (u <= 0x20 || u >= 0x7f) return false;
    }
    return true;
}

} // namespace

Sender::Sender(QObject* parent) : QObject(parent), gpg_(this) {}
Sender::~Sender() = default;

QStringList Sender::msmtpArguments(const QString& msmtpAccount, const QStringList& recipients) {
    QStringList a{QStringLiteral("-a"), msmtpAccount, QStringLiteral("--read-envelope-from"), QStringLiteral("--")};
    a += recipients;
    return a;
}

void Sender::finish(SendResult r) {
    QTimer::singleShot(0, this, [self = QPointer<Sender>(this), r = std::move(r), cb = std::move(done_)]() {
        if (cb) cb(r);
        if (self) self->busy_ = false; // dopo il ritorno del callback (che puo' aver distrutto il Sender)
    });
    done_ = nullptr;
}

void Sender::send(const OutgoingMessage& msg, bool sign, bool encrypt, std::function<void(SendResult)> done,
                  bool allowUnsigned) {
    if (busy_) {
        QTimer::singleShot(0, this, [done]() {
            if (!done) return;
            SendResult r;
            r.kind = SendResult::Busy;
            r.message = tr("Un invio e' gia' in corso.");
            done(r);
        });
        return;
    }
    busy_ = true;
    bccHeader_.clear();
    done_ = std::move(done);
    auto fail = [this](SendResult::Kind k, const QString& m, const QStringList& missing = {}) {
        SendResult r;
        r.kind = k;
        r.message = m;
        r.missing = missing;
        finish(std::move(r));
    };

    // Account proprietario dell'identita' e cartella Inviata.
    bool found = false;
    for (const Account& a : std::as_const(accounts_))
        if (a.id == msg.identity.accountId) { account_ = a; found = true; break; }
    if (!found || account_.maildirRoot.isEmpty() || account_.sentFolder.isEmpty())
        return fail(SendResult::BuildFailed, tr("Account dell'identita' o cartella Inviata non configurati."));
    // Mai inviare se la copia in Inviata non potra' essere scritta (cartella
    // inesistente, non Maildir, non scrivibile): nulla e' ancora partito.
    if (const QString why = MaildirWriter::checkFolder(QDir(account_.maildirRoot).filePath(account_.sentFolder));
        !why.isEmpty())
        return fail(SendResult::BuildFailed,
                    tr("Cartella Inviata dell'account non utilizzabile (%1). Nulla e' stato inviato: "
                       "correggi sentFolder nella configurazione.").arg(why));
    msmtpAccount_ = msg.identity.msmtpAccount.trimmed();
    if (!safeMsmtpAccount(msmtpAccount_))
        return fail(SendResult::BuildFailed, tr("Account msmtp dell'identita' non configurato o non valido."));

    QString err;
    const QByteArray built = MessageBuilder::build(msg, &err, BuildMode::Send);
    if (built.isEmpty()) return fail(SendResult::BuildFailed, err.isEmpty() ? tr("Costruzione del messaggio fallita.") : err);

    const QStringList rcpts = MessageBuilder::envelopeRecipients(msg);
    if (rcpts.isEmpty()) return fail(SendResult::BuildFailed, tr("Nessun destinatario."));
    if (rcpts.size() > kMaxRecipients)
        return fail(SendResult::BuildFailed, tr("Troppi destinatari (massimo %1).").arg(kMaxRecipients));
    for (const QString& r : rcpts)
        if (!GpgService::isSafeAddress(r)) // ASCII, niente '-' iniziale, niente caratteri strani
            return fail(SendResult::BuildFailed, tr("Indirizzo destinatario non utilizzabile: %1").arg(r.left(80)));

    Split sp;
    if (!splitMessage(built, &sp)) return fail(SendResult::BuildFailed, tr("Messaggio costruito non interpretabile."));
    bccHeader_ = MessageBuilder::bccHeader(msg);
    if (sp.hasBcc) return fail(SendResult::BuildFailed, tr("Il messaggio costruito contiene un'intestazione Bcc."));

    const QString keyId = msg.identity.pgpKeyId.trimmed();
    if (sign && keyId.isEmpty()) {
        if (!allowUnsigned)
            return fail(SendResult::NeedsConfirmationUnsigned,
                        tr("L'identita' non ha una chiave PGP: confermare per inviare senza firma."));
        sign = false;
    }
    if (!sign && !encrypt) {
        runMsmtp(built, rcpts);
        return;
    }

    // Controllo delle chiavi (gpg --list-keys / --list-secret-keys, fino a 30 s):
    // fuori dal thread GUI. runList() usa solo un QProcess locale e valori costanti,
    // quindi e' sicuro; il risultato torna sul thread GUI (contesto `this`: se il
    // Sender sparisce il callback non parte, e keyPool_ attende il lavoro in corso).
    // Chiave dell'identita' risolta in impronta (una volta). Firma: obbligatoria. Solo
    // cifratura: se non risolvibile si rinuncia alla copia leggibile (--encrypt-to),
    // senza indovinare.
    auto protect = [this](const Split& sp, const QStringList& rcpts, bool sign, bool encrypt, const QString& fpr) {
    QPointer<Sender> self(this);
    // L'ultimo CRLF appartiene al delimitatore: la parte firmata e' quella senza.
    QByteArray part = sp.inner;
    if (part.endsWith("\r\n")) part.chop(2);

    if (encrypt) {
        gpg_.encrypt(sp.inner, rcpts, sign ? fpr : QString(), fpr,
                     [self, sp, rcpts](bool ok, QByteArray armored, QString e, GpgService::EncryptFailure kind) {
                         if (!self) return;
                         if (!ok) {
                             SendResult r;
                             r.kind = kind == GpgService::EncryptFailure::Sign ? SendResult::SignFailed : SendResult::EncryptFailed;
                             r.message = e;
                             self->finish(std::move(r));
                             return;
                         }
                         self->runMsmtp(assembleEncrypted(sp, armored, newBoundary(armored)), rcpts);
                     });
        return;
    }
    gpg_.sign(part, fpr, [self, sp, part, rcpts](bool ok, QByteArray sig, QString micalg, QString e) {
        if (!self) return;
        if (!ok) {
            SendResult r;
            r.kind = SendResult::SignFailed;
            r.message = e;
            self->finish(std::move(r));
            return;
        }
        self->runMsmtp(assembleSigned(sp, part, sig, micalg, newBoundary(part)), rcpts);
    });
    };

    keyPool_.setMaxThreadCount(1);
    GpgService* gpg = &gpg_;
    keyPool_.start([this, gpg, sp, rcpts, keyId, sign, encrypt, protect]() {
        QStringList missing;
        QString fpr, kerr;
        if (encrypt) missing = gpg->missingKeys(rcpts);
        if (missing.isEmpty() && (sign || (encrypt && !keyId.isEmpty()))) fpr = gpg->resolveSecretKey(keyId, &kerr);
        QMetaObject::invokeMethod(
            this,
            [this, sp, rcpts, sign, encrypt, missing, fpr, kerr, protect]() {
                SendResult r;
                if (encrypt && !missing.isEmpty()) {
                    r.kind = SendResult::MissingKeys;
                    r.message = tr("Manca una chiave di cifratura utilizzabile per: %1").arg(missing.join(QStringLiteral(", ")));
                    r.missing = missing;
                    return finish(std::move(r));
                }
                if (sign && fpr.isEmpty()) {
                    r.kind = SendResult::SignFailed;
                    r.message = kerr;
                    return finish(std::move(r));
                }
                protect(sp, rcpts, sign, encrypt, fpr);
            },
            Qt::QueuedConnection);
    });
}

void Sender::runMsmtp(const QByteArray& message, const QStringList& recipients) {
    QPointer<Sender> self(this);
    auto seen = std::make_shared<qint64>(0);
    auto procPtr = std::make_shared<QPointer<QProcess>>();
    QProcess* p = runner_.run(
        msmtp_, msmtpArguments(msmtpAccount_, recipients),
        std::chrono::milliseconds(msmtpTimeoutMs_ + (message.size() / (1024 * 1024)) * 1000),
        [self, message](ProcessResult r) {
            if (!self) return;
            if (r.exitCode == 0 && !r.crashed && !r.timedOut) {
                self->storeCopyAndFinish(message);
                return;
            }
            SendResult res;
            res.kind = SendResult::SmtpFailed;
            if (r.timedOut || r.crashed) // anche kill per output eccessivo: esito incerto
                res.message = tr("msmtp e' stato interrotto (tempo scaduto o terminazione anomala): l'esito e' incerto, "
                                 "il messaggio potrebbe essere stato inviato. Verificare prima di reinviare.");
            else {
                res.message = QString::fromUtf8(r.err).trimmed().left(500);
                if (res.message.isEmpty()) res.message = tr("msmtp ha restituito l'errore %1.").arg(r.exitCode);
            }
            self->finish(std::move(res));
        },
        [seen, procPtr](const QByteArray& d) {
            *seen += d.size();
            if (*seen > kMaxMsmtpOutput && *procPtr && (*procPtr)->state() != QProcess::NotRunning) (*procPtr)->kill();
        });
    *procPtr = p;
    if (p && p->state() != QProcess::NotRunning) {
        p->write(message);
        p->closeWriteChannel();
    }
}

QString Sender::unsavedDir() const {
    if (!unsavedDir_.isEmpty()) return unsavedDir_;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/non-salvati");
}

void Sender::storeCopyAndFinish(const QByteArray& message) {
    const QString folder = QDir(account_.maildirRoot).filePath(account_.sentFolder);
    // Copia = byte inviati + Bcc (con lo stesso fine riga del messaggio).
    QByteArray copy = message;
    if (!bccHeader_.isEmpty()) {
        const qsizetype nl = message.indexOf('\n');
        const bool crlf = nl > 0 && message.at(nl - 1) == '\r';
        QByteArray h = bccHeader_;
        if (crlf) h.replace("\n", "\r\n");
        copy.prepend(h);
    }
    QString e;
    const QString path = MaildirWriter::deliver(folder, copy, QStringLiteral("S"), &e);
    SendResult r;
    r.kind = SendResult::Sent;
    if (path.isEmpty()) {
        r.copyFailed = true;
        QString fe;
        r.fallbackPath = MaildirWriter::saveFallback(unsavedDir(), copy, &fe);
        r.message = tr("Messaggio INVIATO, ma la copia in Inviata non e' stata salvata: %1").arg(e);
        r.message += r.fallbackPath.isEmpty()
                         ? tr("\nNemmeno la copia di emergenza e' stata salvata (%1).").arg(fe)
                         : tr("\nUna copia e' stata salvata in: %1").arg(r.fallbackPath);
        finish(std::move(r));
        return;
    }
    // Reindicizzazione a "fuoco e dimentica": non trattiene il risultato e sopravvive al Sender.
    auto* idx = new MailIndex(QCoreApplication::instance(), muProgram_, muHome_);
    QPointer<Sender> self(this);
    idx->reindex(account_.id, [idx, self](bool ok, QString err) {
        if (!ok) qWarning().noquote() << "mu index dopo l'invio:" << err;
        if (self) emit self->reindexFinished(ok, err);
        idx->deleteLater();
    });
    finish(std::move(r));
}

} // namespace boma
