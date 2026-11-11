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
    // Solo con Sent: il messaggio E' partito ma la copia in Inviata non e' stata
    // scritta. fallbackPath = copia di emergenza (.eml) se salvata, altrimenti vuoto.
    bool copyFailed = false;
    QString fallbackPath;
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
// - Prima di msmtp la cartella Inviata dell'account deve essere una Maildir
//   scrivibile (MaildirWriter::checkFolder): altrimenti BuildFailed, nulla e'
//   inviato. Copia in Inviata SOLO se msmtp esce 0; se la copia fallisce
//   comunque (disco pieno, cartella sparita) l'esito e' Sent con copyFailed,
//   un avviso in message e una copia di emergenza in unsavedDir()
//   (fallbackPath): il messaggio inviato non va mai perso. Mai un secondo invio. Il callback arriva subito dopo la consegna;
//   `mu index` parte dopo, a "fuoco e dimentica" su un MailIndex figlio di
//   QCoreApplication (sopravvive al Sender); l'esito e' nel segnale
//   reindexFinished (se il Sender e' vivo) o in un qWarning.
// - Chiave PGP: l'identita' (indirizzo o impronta) e' risolta UNA volta in impronta
//   (GpgService::resolveSecretKey); ambigua/non trovata = SignFailed, mai a caso.
//   Il msmtp "esito incerto" (timeout, crash, kill) non scrive mai la copia.
//   La copia in Inviata contiene l'intestazione Bcc (solo la copia: i byte
//   inviati non la contengono mai), cosi' resta traccia dei destinatari nascosti.
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
    // Base del timeout di msmtp; il timeout effettivo e' base + 1 s per MiB di messaggio (allegati grandi).
    void setMsmtpTimeoutMs(int ms) { msmtpTimeoutMs_ = ms; }
    void setMuProgram(const QString& p) { muProgram_ = p; }
    void setMuHome(const QString& h) { muHome_ = h; }
    // Cartella delle copie di emergenza (default: <AppDataLocation>/non-salvati).
    void setUnsavedDir(const QString& d) { unsavedDir_ = d; }
    QString unsavedDir() const;
    GpgService& gpg() { return gpg_; }
    bool busy() const { return busy_; }

    void send(const OutgoingMessage& msg, bool sign, bool encrypt, std::function<void(SendResult)> done,
              bool allowUnsigned = false);

    // Argomenti esatti di msmtp (testabili).
    static QStringList msmtpArguments(const QString& msmtpAccount, const QStringList& recipients);

signals:
    // Esito della reindicizzazione (a "fuoco e dimentica", dopo il callback di send()). Emesso solo
    // se il Sender e' ancora vivo.
    void reindexFinished(bool ok, QString error);

private:
    void finish(SendResult r);
    void runMsmtp(const QByteArray& message, const QStringList& recipients);
    void storeCopyAndFinish(const QByteArray& message);

    QList<Account> accounts_;
    QString msmtp_ = QStringLiteral("msmtp");
    int msmtpTimeoutMs_ = 120'000;
    GpgService gpg_;
    QString muProgram_ = QStringLiteral("mu"), muHome_;
    ProcessRunner runner_;
    bool busy_ = false;
    Account account_;
    QString msmtpAccount_;
    QByteArray bccHeader_; // solo per la copia in Inviata
    QString unsavedDir_;
    std::function<void(SendResult)> done_;
};

} // namespace boma
