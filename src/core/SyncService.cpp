#include "core/SyncService.h"

namespace boma {

SyncService::SyncService(ProcessRunner* runner, const Config& config, QObject* parent)
    : QObject(parent), m_runner(runner), m_config(config)
{
}

SyncService::~SyncService() { shutdown(); }

void SyncService::shutdown()
{
    // Nessuna callback deve toccare un oggetto distrutto; il figlio in corso
    // viene fermato con SIGTERM (poi SIGKILL se non risponde).
    m_shutdown = true;
    *m_alive = false;
    m_queue.clear();
    if (m_proc) {
        m_proc->disconnect();
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->terminate();
            if (!m_proc->waitForFinished(3000)) {
                m_proc->kill();
                m_proc->waitForFinished(2000);
            }
        }
    }
}

bool SyncService::isLockError(const QString& text)
{
    return text.contains(QLatin1String("lock"), Qt::CaseInsensitive);
}

void SyncService::syncAll()
{
    const QList<Account> accounts = m_config.accounts();
    if (m_shutdown || m_state == Running || accounts.isEmpty())
        return;
    m_queue.clear();
    for (const Account& a : accounts)
        m_queue.enqueue(a);
    m_failedId.clear();
    m_failedMsg.clear();
    m_anySynced = false;
    m_busyElsewhere = false;
    setState(Running);
    next();
}

void SyncService::next()
{
    if (m_queue.isEmpty()) {
        finish();
        return;
    }
    const Account acc = m_queue.dequeue();
    auto alive = m_alive;
    m_proc = m_runner->run(
        m_program, m_quick ? QStringList{QStringLiteral("--pull-new"), QStringLiteral("-V"), acc.mbsyncChannel}
                : QStringList{QStringLiteral("-V"), acc.mbsyncChannel}, m_timeout,
        [this, alive, acc](ProcessResult r) {
            if (!*alive)
                return;
            if (r.timedOut || r.crashed || r.exitCode != 0) {
                QString detail = QString::fromUtf8(r.err).trimmed();
                if (detail.isEmpty())
                    detail = QString::fromUtf8(r.out).trimmed().section('\n', -1);
                const bool locked = !r.timedOut && !r.crashed && isLockError(detail);
                const QString failedMsg = r.timedOut ? tr("Tempo scaduto")
                              : r.crashed ? tr("mbsync terminato in modo anomalo")
                              : locked ? tr("un altro mbsync (ad esempio il cron) e' gia' in corso")
                                       : tr("mbsync ha restituito %1: %2").arg(r.exitCode).arg(detail);
                if (m_failedId.isEmpty()) { // si riporta il primo errore del giro
                    m_failedId = acc.id;
                    m_failedMsg = failedMsg;
                    m_busyElsewhere = locked;
                } else if (!locked) {
                    m_busyElsewhere = false;
                }
            } else {
                m_anySynced = true;
            }
            next(); // un errore non impedisce di sincronizzare gli account restanti
        });
}

void SyncService::finish()
{
    if (m_failedId.isEmpty())
        setState(Idle);
    else
        setState(Failed, m_failedId, m_failedMsg);
}

void SyncService::setState(State s, const QString& accountId, const QString& msg)
{
    m_state = s;
    emit stateChanged(s, accountId, msg);
}

} // namespace boma
