#pragma once
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QQueue>
#include <QString>
#include <chrono>
#include <memory>
#include "core/Config.h"
#include "core/ProcessRunner.h"

namespace boma {

// "Aggiorna ora": esegue `mbsync -V <canale>` per ogni account, uno alla volta.
// La sincronizzazione periodica NON e' compito di Boma (la fa il cron); questa
// classe serve solo alla richiesta esplicita dell'utente. Un solo giro alla volta
// (stato Running). Non c'e' un blocco proprio: mbsync ne ha uno suo sul canale, e
// se il cron e' in corso mbsync esce con un errore di blocco, che qui diventa
// busyElsewhere() (non e' un errore di rete). Un errore su un account non ferma gli
// altri; il primo errore e' riportato; nessun nuovo tentativo automatico.
class SyncService : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Running, Failed };
    Q_ENUM(State)

    SyncService(ProcessRunner* runner, const Config& config, QObject* parent = nullptr);
    ~SyncService() override;

    void syncAll();
    State state() const { return m_state; }
    // Almeno un account ha completato la sincronizzazione nel giro corrente/ultimo
    // (anche se un account successivo e' fallito): la posta nuova va indicizzata.
    bool anySynced() const { return m_anySynced; }
    // Il giro e' fallito perche' un altro mbsync (cron) teneva il canale bloccato.
    bool busyElsewhere() const { return m_busyElsewhere; }

    // Arresto per l'uscita dell'applicazione: il mbsync in corso riceve SIGTERM
    // (mbsync e' sicuro da interrompere e riprende in modo incrementale) e, solo se
    // non esce entro 3 s, SIGKILL. Idempotente; dopo l'arresto non parte piu' nulla.
    void shutdown();

    // Vero se l'errore di mbsync indica un canale bloccato da un altro mbsync.
    static bool isLockError(const QString& text);

    // Percorso del programma (iniettabile nei test con uno script finto).
    void setMbsyncProgram(const QString& program) { m_program = program; }
    // Tempo massimo per account: allo scadere SIGTERM, poi SIGKILL.
    void setTimeout(std::chrono::milliseconds t) { m_timeout = t; }

signals:
    void stateChanged(boma::SyncService::State state, QString accountId, QString message);

private:
    void next();
    void finish();
    void setState(State s, const QString& accountId = {}, const QString& msg = {});

    ProcessRunner* m_runner;
    Config m_config;
    QString m_program = QStringLiteral("mbsync");
    std::chrono::milliseconds m_timeout{std::chrono::minutes(10)};
    State m_state = Idle;
    QQueue<Account> m_queue;
    QPointer<QProcess> m_proc;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    QString m_failedId, m_failedMsg;
    bool m_anySynced = false;
    bool m_busyElsewhere = false;
    bool m_shutdown = false;
};

} // namespace boma
