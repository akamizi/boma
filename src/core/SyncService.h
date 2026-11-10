#pragma once
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <memory>
#include <QQueue>
#include <QString>
#include <QTimer>
#include "core/Config.h"
#include "core/ProcessRunner.h"

namespace boma {

// Sincronizzazione con mbsync (`mbsync -V <canale>`), un account alla volta.
// Un solo giro alla volta (stato Running) e un flock su lockPath contro altre
// istanze/processi: un lock occupato e' un errore, non si attende. In caso di
// errore nessun nuovo tentativo automatico; il prossimo giro e' il timer o
// una richiesta esplicita. Nessuna eccezione se la rete manca: mbsync esce
// con errore e lo stato diventa Failed.
class SyncService : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Running, Failed };
    Q_ENUM(State)

    SyncService(ProcessRunner* runner, const Config& config, QString lockPath,
                QObject* parent = nullptr);
    ~SyncService() override;

    void syncAll();
    void syncAccount(const QString& id);
    State state() const { return m_state; }
    // Almeno un account ha completato la sincronizzazione nel giro corrente/ultimo
    // (anche se un account successivo e' fallito): la posta nuova va indicizzata.
    bool anySynced() const { return m_anySynced; }

    // Percorso del programma (iniettabile nei test con uno script finto).
    void setMbsyncProgram(const QString& program) { m_program = program; }
    void setTimeout(std::chrono::milliseconds t) { m_timeout = t; }

    // Pianificazione: sincronizza tutto ogni intervallo (default 5 minuti).
    void setInterval(std::chrono::milliseconds i) { m_timer.setInterval(i); }
    std::chrono::milliseconds interval() const { return m_timer.intervalAsDuration(); }
    void start() { m_timer.start(); }
    void stop() { m_timer.stop(); }

    // Estrae "B: done/total" (o, in mancanza, "C: done/total") da un output
    // di mbsync -V; vero se trovato. Considera l'ultima riga utile.
    static bool parseProgress(const QByteArray& chunk, int* done, int* total);

signals:
    void stateChanged(boma::SyncService::State state, QString accountId, QString message);
    void progress(QString accountId, int done, int total);

private:
    void begin(const QList<Account>& accounts);
    void next();
    void finish();
    void onOutput(const QString& accountId, const QByteArray& chunk);
    void setState(State s, const QString& accountId = {}, const QString& msg = {});

    ProcessRunner* m_runner;
    Config m_config;
    QString m_lockPath;
    QString m_program = QStringLiteral("mbsync");
    std::chrono::milliseconds m_timeout{std::chrono::minutes(10)};
    QTimer m_timer;
    State m_state = Idle;
    QQueue<Account> m_queue;
    int m_lockFd = -1;
    QPointer<QProcess> m_proc;
    QByteArray m_lineBuf;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    QString m_failedId, m_failedMsg;
    bool m_anySynced = false;
};

} // namespace boma
