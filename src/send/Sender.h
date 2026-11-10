#pragma once
#include "core/Config.h"
#include "core/MailIndex.h"
#include "core/ProcessRunner.h"
#include "mime/MessageBuilder.h"
#include "pgp/GpgService.h"
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>

namespace boma {

struct SendResult {
    enum Kind {
        Sent,                      // msmtp ha accettato il messaggio (message puo' contenere un avviso)
        NeedsConfirmationUnsigned, // firma richiesta ma l'identita' non ha chiave PGP: nulla e' stato inviato
        MissingKeys,               // cifratura richiesta, chiavi mancanti (in "missing"): nulla e' stato inviato
        SignFailed,                // firma non riuscita: nulla e' stato inviato
        SmtpFailed,                // msmtp fallito/scaduto: nessuna copia in Inviata
        // aggiunte rispetto al brief (additive)
        EncryptFailed,             // cifratura non riuscita: nulla e' stato inviato
        BuildFailed,               // messaggio/destinatari/identita' non validi: nulla e' stato inviato
        Busy                       // un altro invio e' in corso su questo Sender: nulla e' stato inviato
    } kind = SmtpFailed;
    QStringList missing;
    QString message;
};

// Invio di un messaggio: costruzione -> (cifratura) -> firma/cifratura PGP/MIME
// -> msmtp -> copia in Inviata -> reindicizzazione.
//
// Regole di sicurezza (vedi anche i test):
// - Il messaggio e' SEMPRE ricostruito da OutgoingMessage con BuildMode::Send:
//   Bcc non compare in nessuna intestazione; i destinatari della busta
//   (to+cc+bcc) sono passati a msmtp come argomenti espliciti.
//   Comando: msmtp -a <identity.msmtpAccount> --read-envelope-from -- <dest...>
//   (NIENTE -t: -t leggerebbe i destinatari dalle intestazioni e perderebbe
//   Bcc). Il messaggio e' su stdin. Account msmtp e destinatari sono validati
//   (niente '-' iniziale, solo ASCII, "--" prima dei destinatari). Un account
//   msmtp vuoto e' un errore: mai l'account predefinito per caso.
// - Cifratura richiesta + una chiave mancante: MissingKeys, nessun gpg di
//   cifratura, nessun msmtp. Mai in chiaro in silenzio.
// - Firma richiesta e fallita (agent assente, pinentry annullato, timeout):
//   SignFailed, nulla inviato. Sender non cancella mai nulla: chi chiama tiene
//   il proprio OutgoingMessage (la bozza).
// - Inviare senza firma e' possibile solo con allowUnsigned=true, che la UI
//   imposta DOPO che l'utente ha confermato la risposta
//   NeedsConfirmationUnsigned (identita' senza chiave PGP e firma richiesta).
//   Una firma FALLITA non ripiega mai da sola sull'invio senza firma
//   (SignFailed sempre, anche con allowUnsigned): se l'utente vuole inviare
//   comunque, la UI richiama send() con sign=false in modo esplicito.
// - Copia in Inviata SOLO se msmtp esce 0, nella cartella Inviata dell'account
//   dell'identita'; se la copia fallisce l'esito e' Sent con un avviso in
//   message (mai un secondo invio). Poi `mu index`; un suo errore e' un avviso.
//   La copia non contiene Bcc (come cio' che e' stato inviato).
// - Un solo invio alla volta per Sender: un send() durante un invio (anche
//   dentro il callback finale) riceve Busy e non invia nulla. Lo stato "occupato"
//   termina dopo il ritorno del callback finale.
// - Il callback e' sempre consegnato in coda al ciclo eventi, mai dentro
//   send(); distruggere il Sender termina i processi figli e non richiama nulla
//   (se msmtp era gia' partito l'esito e' incerto: succede solo alla chiusura).
// - Limiti: 500 destinatari, solo indirizzi ASCII (niente SMTPUTF8), msmtp 120 s
//   (poi kill: SmtpFailed "esito incerto", nessuna copia), stdout/stderr di
//   msmtp max 1 MiB.
class Sender : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxRecipients = 500;

    explicit Sender(QObject* parent = nullptr);
    ~Sender() override;

    void setAccounts(const QList<Account>& a) { accounts_ = a; }
    void setMsmtpProgram(const QString& p) { msmtp_ = p; }
    void setMsmtpTimeoutMs(int ms) { msmtpTimeoutMs_ = ms; } // per i test
    void setMuProgram(const QString& p) { index_.setMuProgram(p); }
    void setMuHome(const QString& h) { index_.setMuHome(h); }
    GpgService& gpg() { return gpg_; }
    bool busy() const { return busy_; }

    void send(const OutgoingMessage& msg, bool sign, bool encrypt, std::function<void(SendResult)> done,
              bool allowUnsigned = false);

    // Argomenti esatti di msmtp (testabili).
    static QStringList msmtpArguments(const QString& msmtpAccount, const QStringList& recipients);

private:
    void finish(SendResult r);
    void runMsmtp(const QByteArray& message, const QStringList& recipients);
    void storeCopyAndFinish(const QByteArray& message);

    QList<Account> accounts_;
    QString msmtp_ = QStringLiteral("msmtp");
    int msmtpTimeoutMs_ = 120'000;
    GpgService gpg_;
    MailIndex index_;
    ProcessRunner runner_;
    bool busy_ = false;
    Account account_;
    QString msmtpAccount_;
    std::function<void(SendResult)> done_;
};

} // namespace boma
