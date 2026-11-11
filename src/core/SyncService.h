#pragma once
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <memory>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include "core/Config.h"
#include "core/ProcessRunner.h"

namespace boma {

// Sincronizzazione con mbsync (`mbsync -V <canale>`), un account alla volta.
// Un solo giro alla volta (stato Running) e un flock su lockPath contro altre
// istanze/processi: un lock occupato e' un errore, non si attende. In caso di
// errore di un account gli altri del giro vengono comunque sincronizzati e il primo errore e' riportato; nessun nuovo tentativo automatico; il prossimo giro e' il timer o
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

    // Arresto per l'uscita dell'applicazione: il mbsync in corso riceve SIGTERM
    // (arresto ordinato; mbsync e' sicuro da interrompere e al prossimo avvio
    // riprende in modo incrementale) e, solo se non esce entro 3 s, SIGKILL.
    // Poi rilascia il lock. Idempotente; dopo l'arresto non parte piu' nulla.
    void shutdown();

    // "Prima sincronizzazione": la Maildir non contiene nemmeno un file di
    // messaggio (radice assente, o solo cartelle vuote, o solo file fuori da
    // cur/ e new/ come .uidvalidity e .mbsyncstate). Vale solo la presenza di
    // UN file in una cartella cur/ o new/: la ricerca si ferma al primo trovato
    // e non percorre mai l'intera Maildir (15 GB). In *examined, se dato, il
    // numero di voci esaminate (cartelle entrate + file visti), per i test.
    static bool maildirHasMessages(const QString& root, int* examined = nullptr);
    static bool isFirstSync(const QString& root, int* examined = nullptr)
    {
        return !maildirHasMessages(root, examined);
    }

    // Percorso del programma (iniettabile nei test con uno script finto).
    void setMbsyncProgram(const QString& program) { m_program = program; }
    void setTimeout(std::chrono::milliseconds t) { m_timeout = t; }
    // Account senza tempo massimo (prima sincronizzazione: 15 GB non stanno in 10
    // minuti; mbsync e' interrompibile e l'utente puo' chiudere). Gli altri hanno
    // m_timeout (il flag vale solo per l'esecuzione in corso e si azzera al suo termine,
    // ok o errore): allo scadere SIGTERM, poi SIGKILL, errore "Tempo scaduto".
    void setNoTimeout(const QString& accountId, bool on)
    {
        if (on) m_noTimeout.insert(accountId); else m_noTimeout.remove(accountId);
    }
    bool hasNoTimeout(const QString& accountId) const { return m_noTimeout.contains(accountId); }

    // Pianificazione: sincronizza tutto ogni intervallo (default 5 minuti).
    void setInterval(std::chrono::milliseconds i) { m_timer.setInterval(i); }
    std::chrono::milliseconds interval() const { return m_timer.intervalAsDuration(); }
    void start() { m_timer.start(); }
    void stop() { m_timer.stop(); }
    bool timerActive() const { return m_timer.isActive(); }

    // Estrae "B: done/total" (o, in mancanza, "C: done/total") da un output
    // di mbsync -V; vero se trovato. Considera l'ultima riga utile.
    static bool parseProgress(const QByteArray& chunk, int* done, int* total);

signals:
    void stateChanged(boma::SyncService::State state, QString accountId, QString message);
    void progress(QString accountId, int done, int total);
    // Per account: avvio ed esito (ok=false: message contiene l'errore).
    void accountStarted(QString accountId);
    void accountFinished(QString accountId, bool ok, QString message);

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
    bool m_shutdown = false;
    QSet<QString> m_noTimeout;
};

} // namespace boma
