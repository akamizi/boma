#include "core/SyncService.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
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

SyncService::~SyncService() { shutdown(); }

void SyncService::shutdown()
{
    // Nessuna callback deve toccare un oggetto distrutto; il figlio in corso
    // viene fermato con SIGTERM (poi SIGKILL se non risponde) prima di
    // rilasciare il lock (niente mbsync concorrenti ne' orfani).
    m_shutdown = true;
    *m_alive = false;
    m_timer.stop();
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
    if (m_lockFd >= 0) {
        ::close(m_lockFd);
        m_lockFd = -1;
    }
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
    if (m_shutdown || m_state == Running || accounts.isEmpty())
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
    m_anySynced = false;
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
    m_lineBuf.clear();
    auto alive = m_alive;
    emit accountStarted(acc.id);
    m_proc = m_runner->run(
        m_program, {QStringLiteral("-V"), acc.mbsyncChannel},
        m_noTimeout.contains(acc.id) ? std::chrono::milliseconds::zero() : m_timeout,
        [this, alive, acc](ProcessResult r) {
            if (!*alive)
                return;
            if (r.timedOut || r.crashed || r.exitCode != 0) {
                QString detail = QString::fromUtf8(r.err).trimmed();
                if (detail.isEmpty())
                    detail = QString::fromUtf8(r.out).trimmed().section('\n', -1);
                m_failedId = acc.id;
                m_failedMsg = r.timedOut ? tr("Tempo scaduto")
                              : r.crashed ? tr("mbsync terminato in modo anomalo")
                                          : tr("mbsync ha restituito %1: %2")
                                                .arg(r.exitCode).arg(detail);
                m_queue.clear(); // al primo errore ci si ferma
                emit accountFinished(acc.id, false, m_failedMsg);
            } else {
                m_anySynced = true;
                emit accountFinished(acc.id, true, {});
            }
            next();
        },
        [this, alive, acc](const QByteArray& chunk) {
            if (*alive)
                onOutput(acc.id, chunk);
        });
}

void SyncService::onOutput(const QString& accountId, const QByteArray& chunk)
{
    // Si interpretano solo righe complete (terminate da \r o \n).
    m_lineBuf += chunk;
    const int cut = qMax(m_lineBuf.lastIndexOf('\n'), m_lineBuf.lastIndexOf('\r'));
    if (cut < 0) {
        if (m_lineBuf.size() > 65536)
            m_lineBuf.clear();
        return;
    }
    const QByteArray complete = m_lineBuf.left(cut + 1);
    m_lineBuf = m_lineBuf.mid(cut + 1);
    int d = 0, t = 0;
    if (parseProgress(complete, &d, &t))
        emit progress(accountId, d, t);
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

namespace {

constexpr int kMaxDepth = 24;

// Vero al primo file (non nascosto) in dir.
bool hasFile(const QString& dir, int* examined)
{
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot);
    if (it.hasNext()) {
        it.next();
        if (examined)
            ++*examined;
        return true;
    }
    return false;
}

bool walk(const QString& dir, int depth, int* examined)
{
    if (depth > kMaxDepth)
        return false;
    if (examined)
        ++*examined;
    // Prima cur/ e new/ di questa cartella, poi le sottocartelle (anche
    // nascoste: Maildir++ usa ".Sotto"); tmp/ e i link simbolici si saltano.
    for (const char* sub : {"cur", "new"})
        if (hasFile(dir + QLatin1Char('/') + QLatin1String(sub), examined))
            return true;
    QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    while (it.hasNext()) {
        const QString child = it.next();
        const QString name = it.fileName();
        if (name == QLatin1String("cur") || name == QLatin1String("new") || name == QLatin1String("tmp"))
            continue;
        if (walk(child, depth + 1, examined))
            return true;
    }
    return false;
}

} // namespace

bool SyncService::maildirHasMessages(const QString& root, int* examined)
{
    if (examined)
        *examined = 0;
    if (root.isEmpty() || !QFileInfo(root).isDir())
        return false;
    return walk(QDir::cleanPath(root), 0, examined);
}

} // namespace boma
