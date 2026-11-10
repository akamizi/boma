#pragma once
#include <QList>
#include <QString>

namespace boma {

// Formato INI (default ~/.config/boma/config.ini). Nessuna password: quelle
// sono delegate a mbsync (PassCmd) e msmtp (passwordeval).
//
//   [account:<id>]
//   name=...            nome visualizzato
//   maildirRoot=~/Mail/<id>   radice Maildir ("~" espanso)
//   mbsyncChannel=...   canale mbsync da sincronizzare
//   sentFolder=Sent     cartelle speciali (relative alla radice)
//   draftsFolder=Drafts
//   trashFolder=Trash
//
//   [identity:<id>]
//   accountId=<id account>   obbligatorio: ogni identita' appartiene a un solo account
//   address=...  displayName=...  signature=...
//   pgpKeyId=...  msmtpAccount=...
//   signByDefault=true
//
// Identita' con accountId sconosciuto o senza address vengono scartate e
// segnalate in *error.

struct Account {
    QString id, name, maildirRoot, mbsyncChannel, sentFolder, draftsFolder, trashFolder;
};

struct Identity {
    QString id, accountId, address, displayName, signature, pgpKeyId, msmtpAccount;
    bool signByDefault = true;
};

class Config {
public:
    // File assente: configurazione vuota, nessun errore.
    static Config load(const QString& path, QString* error = nullptr);
    static QString defaultPath();

    QList<Account> accounts() const { return m_accounts; }
    QList<Identity> identities(const QString& accountId) const;
    // Confronto case-insensitive; id vuoto se non trovata.
    Identity identityForAddress(const QString& address) const;
    bool isEmpty() const { return m_accounts.isEmpty(); }

private:
    QList<Account> m_accounts;
    QList<Identity> m_identities;
};

} // namespace boma
