#pragma once
#include "core/Config.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace boma {

// Controlli di coerenza tra la configurazione di Boma e i file di mbsync e msmtp.
// Solo lettura: Boma non scrive mai ~/.mbsyncrc ne' ~/.msmtprc. Le funzioni di
// analisi sono pure (testo in ingresso) per poterle provare senza file.
struct CheckIssue {
    enum Level { Ok, Warning, Error } level = Ok;
    QString text;
};

class ConfigCheck {
public:
    // Nomi dei canali (e dei gruppi) definiti in un .mbsyncrc.
    static QStringList mbsyncChannels(const QByteArray& rc);
    // Nomi degli account definiti in un .msmtprc (le righe "account default : x" sono alias).
    static QStringList msmtpAccounts(const QByteArray& rc);
    // File di configurazione effettivi (primo esistente tra i candidati; vuoto se nessuno).
    static QString mbsyncConfigPath();
    static QString msmtpConfigPath();
    // Contenuto del file, o vuoto se manca/illeggibile (*ok dice se e' stato letto).
    static QByteArray readFile(const QString& path, bool* ok = nullptr);

    // haveRc=false: file di mbsync non trovato (il canale non si puo' verificare).
    static QList<CheckIssue> checkAccount(const Account& a, const QStringList& channels, bool haveRc);
    static QList<CheckIssue> checkIdentity(const Identity& i, const QStringList& msmtpAccounts, bool haveRc);
};

} // namespace boma
