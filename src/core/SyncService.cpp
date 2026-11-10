#include "core/SyncService.h"

#include <QRegularExpression>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace boma {

SyncService::SyncService(ProcessRunner* runner, const Config& config, QString lockPath,
                         QObject* parent)
    : QObject(parent), m_runner(runner), m_config(config), m_lockPath(std::move(lockPath))
{
    m_timer.setInterval(std::chrono::minutes(5));
    connect(&m_timer, &QTimer::timeout, this, &SyncService::syncAll);
}

SyncService::~SyncService()
{
    if (m_lockFd >= 0)
        ::close(m_lockFd);
}

void SyncService::syncAll() { begin(m_config.accounts()); }

void SyncService::syncAccount(const QString& id)
{
    for (const Account& a : m_config.accounts()) {
        if (a.id == id) {
            begin({a});
            return;
        }
    }
    if (m_state != Running)
        setState(Failed, id, tr("Account sconosciuto: %1").arg(id));
}

void SyncService::begin(const QList<Account>& accounts)
{
    if (m_state == Running || accounts.isEmpty())
        return;

    m_lockFd = ::open(m_lockPath.toLocal8Bit().constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (m_lockFd < 0 || ::flock(m_lockFd, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = m_lockFd >= 0;
        if (m_lockFd >= 0) {
            ::close(m_lockFd);
            m_lockFd = -1;
        }
        setState(Failed, {},
                 busy ? tr("Sincronizzazione gia' in corso in un altro processo")
                      : tr("Impossibile creare il file di blocco %1").arg(m_lockPath));
        return;
    }

    m_queue.clear();
    for (const Account& a : accounts)
        m_queue.enqueue(a);
    m_failedId.clear();
    m_failedMsg.clear();
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
    m_runner->run(
        m_program, {QStringLiteral("-V"), acc.mbsyncChannel}, m_timeout,
        [this, acc](ProcessResult r) {
            if (r.timedOut || r.crashed || r.exitCode != 0) {
                if (m_failedId.isEmpty()) {
                    m_failedId = acc.id;
                    QString detail = QString::fromUtf8(r.err).trimmed();
                    if (detail.isEmpty())
                        detail = QString::fromUtf8(r.out).trimmed().section('\n', -1);
                    m_failedMsg = r.timedOut ? tr("Tempo scaduto")
                                  : r.crashed ? tr("mbsync terminato in modo anomalo")
                                              : tr("mbsync ha restituito %1: %2")
                                                    .arg(r.exitCode).arg(detail);
                }
            }
            next();
        },
        [this, acc](const QByteArray& chunk) {
            int d = 0, t = 0;
            if (parseProgress(chunk, &d, &t))
                emit progress(acc.id, d, t);
        });
}

void SyncService::finish()
{
    if (m_lockFd >= 0) {
        ::close(m_lockFd); // rilascia il flock
        m_lockFd = -1;
    }
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

bool SyncService::parseProgress(const QByteArray& chunk, int* done, int* total)
{
    static const QRegularExpression re(QStringLiteral(R"(([BC]):\s*(\d+)/(\d+))"));
    bool found = false;
    const QString text = QString::fromUtf8(chunk);
    for (const QString& line : text.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                          Qt::SkipEmptyParts)) {
        QRegularExpressionMatchIterator it = re.globalMatch(line);
        int bd = -1, bt = 0, cd = -1, ct = 0;
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.captured(1) == QLatin1String("B")) {
                bd = m.captured(2).toInt();
                bt = m.captured(3).toInt();
            } else {
                cd = m.captured(2).toInt();
                ct = m.captured(3).toInt();
            }
        }
        if (bd >= 0) {
            *done = bd; *total = bt; found = true;
        } else if (cd >= 0) {
            *done = cd; *total = ct; found = true;
        }
    }
    return found;
}

} // namespace boma
