#pragma once
#include "core/ProcessRunner.h"
#include <QHash>
#include <QString>
#include <QStringList>
#include <functional>

namespace boma {

// Importa le etichette Gmail (sola lettura, via IMAP X-GM-LABELS) con lo script Python
// incorporato (gmail_labels.py). Credenziali: le stesse di mbsync (~/.mbsyncrc).
class GmailLabels {
public:
    // Message-ID (senza <>) -> etichette; scarta righe non valide. *done = valore dell'ultima riga {"done":N}.
    static QHash<QString, QStringList> parseOutput(const QByteArray& out, int* done = nullptr);
    static QString script();

    // Asincrono. inboxOnly: solo i messaggi con l'etichetta di sistema \Inbox (sono le chiavi della
    // mappa; serve alla "Posta in arrivo" virtuale). cb(etichette per messaggio, errore): errore vuoto = ok.
    static void fetch(ProcessRunner* runner, const QString& python, const QString& mbsyncConfig,
                      const QString& channel, std::function<void(QHash<QString, QStringList>, QString)> cb,
                      bool inboxOnly = false);
};

} // namespace boma
