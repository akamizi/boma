#pragma once
#include <QByteArray>
#include <QString>

namespace boma {

// Unico punto in cui Boma scrive direttamente nella Maildir (Inviata, Bozze).
// Protocollo Maildir: file unico in tmp/, fsync, poi link in cur/ senza mai
// sovrascrivere. Restituisce il percorso finale, o stringa vuota con *error.
class MaildirWriter {
public:
    static QString deliver(const QString& maildirFolder, const QByteArray& message,
                           QString flags, QString* error);
};

} // namespace boma
