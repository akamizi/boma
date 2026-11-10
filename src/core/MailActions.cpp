#include "core/MailActions.h"

#include <QDir>
#include <chrono>

namespace boma {

void MailActions::runMu(const QStringList& argsIn, Callback cb)
{
    QStringList args = argsIn;
    if (!m_muHome.isEmpty() && !args.isEmpty())
        args.insert(1, QStringLiteral("--muhome=%1").arg(m_muHome));
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
    if (!QDir::isAbsolutePath(path)) {
        if (cb)
            cb(false, QStringLiteral("Percorso del messaggio non assoluto: %1").arg(path));
        return;
    }
    runMu({QStringLiteral("move"), path, QStringLiteral("--flags=-N+S")}, std::move(cb));
}

void MailActions::moveToTrash(const QString& path, const Account& account, Callback cb)
{
    auto refuse = [&cb](const QString& why) {
        if (cb)
            cb(false, why);
    };
    if (!QDir::isAbsolutePath(path) || !QDir::isAbsolutePath(account.maildirRoot))
        return refuse(QStringLiteral("Percorso del messaggio o radice Maildir non assoluti"));
    const QString root = QDir::cleanPath(account.maildirRoot);
    const QString msg = QDir::cleanPath(path);
    if (!msg.startsWith(root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/')))
        return refuse(QStringLiteral("Il messaggio %1 non e' sotto la Maildir dell'account (%2)")
                          .arg(path, account.maildirRoot));

    const QString trimmed = account.trashFolder.trimmed();
    const QStringList parts = trimmed.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.contains(QStringLiteral("..")) || parts.contains(QStringLiteral(".")))
        return refuse(QStringLiteral("Cartella cestino non valida: '%1'").arg(account.trashFolder));
    // mu vuole la cartella relativa alla radice Maildir di mu, con '/' iniziale.
    // La radice di mu e' OBBLIGATORIA: senza, la cartella di destinazione potrebbe
    // essere sbagliata e non si sposta nulla. "/" non e' accettata come radice.
    if (m_muRoot.isEmpty())
        return refuse(QStringLiteral("Radice Maildir di mu non nota: spostamento rifiutato"));
    const QString mroot = QDir::cleanPath(m_muRoot);
    if (mroot == QLatin1String("/") || !QDir::isAbsolutePath(mroot))
        return refuse(QStringLiteral("Radice Maildir di mu non valida (%1): spostamento rifiutato").arg(m_muRoot));
    QString prefix;
    if (root == mroot) {
        // account = radice di mu
    } else if (root.startsWith(mroot + QLatin1Char('/'))) {
        prefix = root.mid(mroot.size());
    } else {
        return refuse(QStringLiteral("La Maildir dell'account (%1) non e' sotto la radice di mu (%2): spostamento rifiutato")
                          .arg(account.maildirRoot, m_muRoot));
    }
    runMu({QStringLiteral("move"), path, prefix + QLatin1Char('/') + parts.join(QLatin1Char('/'))}, std::move(cb));
}

} // namespace boma
