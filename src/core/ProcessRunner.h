#pragma once
#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <chrono>
#include <functional>

namespace boma {

struct ProcessResult {
    int exitCode = -1;
    bool crashed = false;
    bool timedOut = false;
    QByteArray out, err;
};

// Esegue un programma esterno in modo asincrono (mai bloccante per la GUI).
// "done" viene chiamato esattamente una volta, anche se il programma non
// parte (exitCode -1, messaggio in err). "onOutput", se presente, riceve lo
// stdout e lo stderr man mano che arrivano.
class ProcessRunner : public QObject {
    Q_OBJECT
public:
    using OutputCallback = std::function<void(const QByteArray&)>;
    explicit ProcessRunner(QObject* parent = nullptr) : QObject(parent) {}
    // Allo scadere del tempo massimo (timeout > 0; 0 = nessun limite) il figlio
    // riceve SIGTERM e, se non esce entro il periodo di tolleranza, SIGKILL.
    void setKillGrace(std::chrono::milliseconds g) { m_grace = g; }

    // Restituisce il processo avviato (nullo dopo la distruzione) per consentire
    // a chi chiama di terminarlo.
    QProcess* run(const QString& program, const QStringList& args,
             std::chrono::milliseconds timeout,
             std::function<void(ProcessResult)> done,
             OutputCallback onOutput = nullptr);
private:
    std::chrono::milliseconds m_grace{5000};
};

} // namespace boma
