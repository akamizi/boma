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

    // signedPart: byte esatti della parte firmata, passati a gpg senza modifiche.
    void verify(const QByteArray& signedPart, const QByteArray& signature,
                std::function<void(SignatureStatus, QString signer)> done);
    // In caso di errore o annullamento: ok=false, plain vuoto, error non vuoto.
    void decrypt(const QByteArray& encrypted,
                 std::function<void(bool ok, QByteArray plain, QString error)> done);

    // Bloccante (timeout breve): da chiamare di rado, non nel percorso di lettura.
    static GpgAgentInfo checkAgentCache();
    // Interpreta l'output di `gpgconf --list-options gpg-agent`.
    static GpgAgentInfo parseGpgconfOptions(const QByteArray& out);

    // Classificazione pura delle righe di stato di --verify (testabile da sola).
    static SignatureStatus classifyVerifyStatus(const QByteArray& statusOutput, int exitCode,
                                                QString* signer = nullptr);

private:
    struct Job;
    void start(const QStringList& args, const QByteArray& stdinData, int timeoutMs,
               std::function<void(int exitCode, bool timedOut, bool startFailed, QByteArray out, QByteArray err)> done);

    QString gpg_ = QStringLiteral("gpg");
    QString home_;
    QList<QProcess*> procs_;
};

} // namespace boma
