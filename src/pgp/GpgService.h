#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <functional>

namespace boma {

enum class SignatureStatus { None, Valid, Invalid, UnknownKey, ExpiredKey, RevokedKey };

struct GpgAgentInfo {
    bool available = false;   // gpgconf ha risposto e le opzioni sono state lette
    int defaultCacheTtl = 600; // secondi (valore effettivo; 600 se non impostato)
    int maxCacheTtl = 7200;    // secondi (valore effettivo; 2 ore se non impostato)
    bool ttlIs300 = false;     // entrambi i valori valgono 300
};

// Verifica e decifrazione OpenPGP tramite il programma esterno "gpg".
// L'esito si ricava SOLO dalle righe di stato leggibili dalla macchina
// ("[GNUPG:] ..." su --status-fd 2: lo stdout resta riservato al testo in
// chiaro), mai dal testo per umani. La password la chiede gpg-agent tramite
// pinentry: Boma non la vede e non la passa mai (nessuna opzione --passphrase*,
// nessun --pinentry-mode loopback). --batch non impedisce a gpg-agent di
// chiamare pinentry (verificato nei test con un pinentry finto).
// Note di progetto:
// - Il timeout di decrypt (5 min) comprende il tempo in cui pinentry attende la password.
// - GOODSIG di una chiave non fidata e' Valid per scelta: non si richiede fiducia (trust).
// - ERRSIG con rc diverso da 9 (non solo "chiave mancante") e' riportato come UnknownKey (mai verde).
// - Piu' firme: vince il caso peggiore (una BADSIG rende tutto Invalid, anche con un'altra GOODSIG).
// - I callback sono sempre consegnati in coda al ciclo eventi, mai dentro verify()/decrypt().
// - Limiti: 128 MiB di testo in chiaro e 1 MiB di stato/stderr; oltre, gpg e' terminato e
//   l'esito e' un errore (nessun dato parziale).
// - Decifrazione riuscita (ok=true) se: DECRYPTION_OKAY e END_DECRYPTION, nessun DECRYPTION_FAILED
//   o BADMDC, e uscita 0 oppure uscita diversa da 0 dovuta solo a stati di firma (ERRSIG,
//   NO_PUBKEY, BADSIG, EXPKEYSIG, EXPSIG, REVKEYSIG): un messaggio firmato e cifrato resta
//   leggibile anche se la firma interna non e' valida. L'esito della firma interna e'
//   esposto dal secondo callback (opzionale) e non e' mai verde se non Valid.
// Il testo in chiaro vive solo in memoria. Distruggere il servizio mentre gpg
// e' in esecuzione termina i processi figli e non richiama alcun callback.
class GpgService : public QObject {
    Q_OBJECT
public:
    explicit GpgService(QObject* parent = nullptr);
    explicit GpgService(const QString& gpgProgram, QObject* parent = nullptr);
    ~GpgService() override;

    void setGpgProgram(const QString& program) { gpg_ = program; }
    // Se non vuoto, impostato come GNUPGHOME dei processi (per i test).
    void setGnupgHome(const QString& dir) { home_ = dir; }
    // Limiti (per i test): byte di stdout (testo in chiaro) e di stato/stderr.
    void setMaxPlaintextBytes(qint64 n) { maxPlain_ = n; }
    void setMaxStatusBytes(qint64 n) { maxStatus_ = n; }

    // signedPart: byte esatti della parte firmata, passati a gpg senza modifiche.
    void verify(const QByteArray& signedPart, const QByteArray& signature,
                std::function<void(SignatureStatus, QString signer)> done);
    // In caso di errore o annullamento: ok=false, plain vuoto, error non vuoto.
    void decrypt(const QByteArray& encrypted,
                 std::function<void(bool ok, QByteArray plain, QString error)> done,
                 std::function<void(SignatureStatus, QString signer)> innerSignature = nullptr);

    // Bloccante (timeout breve): da chiamare di rado, non nel percorso di lettura.
    static GpgAgentInfo checkAgentCache();
    // Interpreta l'output di `gpgconf --list-options gpg-agent`.
    static GpgAgentInfo parseGpgconfOptions(const QByteArray& out);

    // Classificazione pura delle righe di stato di --verify (testabile da sola).
    static SignatureStatus classifyVerifyStatus(const QByteArray& statusOutput, int exitCode,
                                                QString* signer = nullptr);

private:
    struct Result {
        int code = -1;
        bool timedOut = false, startFailed = false, overflow = false;
        QString startError;
        QByteArray out, err;
    };
    void start(const QStringList& args, const QByteArray& stdinData, int timeoutMs, std::function<void(Result)> done);

    QString gpg_ = QStringLiteral("gpg");
    QString home_;
    qint64 maxPlain_ = 128LL * 1024 * 1024;
    qint64 maxStatus_ = 1024 * 1024;
    QList<QProcess*> procs_;
};

} // namespace boma
