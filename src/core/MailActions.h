#pragma once
#include "core/Config.h"
#include "core/ProcessRunner.h"
#include <QObject>
#include <QString>
#include <functional>

namespace boma {

using Callback = std::function<void(bool ok, QString error)>;

// Azioni sui messaggi esistenti: SEMPRE tramite `mu move`. Boma non rinomina,
// non cancella e non tocca mai direttamente i file della Maildir.
class MailActions : public QObject {
    Q_OBJECT
public:
    explicit MailActions(QObject* parent = nullptr, QString muProgram = QStringLiteral("mu"))
        : QObject(parent), m_mu(std::move(muProgram)) {}
    void setMuProgram(const QString& p) { m_mu = p; }

    // Aggiunge la bandiera S (letto) e toglie N (nuovo).
    void markRead(const QString& path, Callback cb);
    // Sposta nella cartella cestino dell'account (relativa a maildirRoot).
    void moveToTrash(const QString& path, const Account& account, Callback cb);

private:
    void runMu(const QStringList& args, Callback cb);
    QString m_mu;
    ProcessRunner m_runner;
};

} // namespace boma
