#include "core/MailActions.h"

#include <chrono>

namespace boma {

void MailActions::runMu(const QStringList& args, Callback cb)
{
    m_runner.run(m_mu, args, std::chrono::seconds(30),
                 [cb = std::move(cb)](ProcessResult r) {
                     const bool ok = r.exitCode == 0 && !r.crashed && !r.timedOut;
                     QString err;
                     if (!ok) {
                         err = QString::fromUtf8(r.err).trimmed();
                         if (err.isEmpty())
                             err = r.timedOut ? QStringLiteral("mu: tempo scaduto")
                                              : QStringLiteral("mu: errore (codice %1)").arg(r.exitCode);
                     }
                     if (cb)
                         cb(ok, err);
                 });
}

void MailActions::markRead(const QString& path, Callback cb)
{
    runMu({QStringLiteral("move"), path, QStringLiteral("--flags=-N+S")}, std::move(cb));
}

void MailActions::moveToTrash(const QString& path, const Account& account, Callback cb)
{
    QString folder = account.trashFolder;
    while (folder.startsWith(QLatin1Char('/')))
        folder.remove(0, 1);
    // mu vuole la cartella relativa alla radice Maildir, con '/' iniziale.
    runMu({QStringLiteral("move"), path, QLatin1Char('/') + folder}, std::move(cb));
}

} // namespace boma
