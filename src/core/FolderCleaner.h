#pragma once
#include <QString>
#include <QStringList>

namespace boma {

// Svuotamento DEFINITIVO di una cartella Maildir. E' l'unico punto in cui Boma
// cancella file di posta, solo su richiesta esplicita dell'utente (menu "Svuota la
// casella", dopo conferma). Garanzie: la cartella deve stare sotto la radice Maildir
// dell'account (percorsi canonici, link simbolici risolti), non essere la radice
// stessa e contenere cur/; si cancellano solo file regolari direttamente in cur/ e
// new/ (mai tmp/, sottocartelle o link). Le funzioni sono sincrone: chiamarle da un
// thread di lavoro.
class FolderCleaner {
public:
    // Controlla la cartella; vuoto = ok, altrimenti il motivo del rifiuto.
    static QString validate(const QString& folderDir, const QString& accountRoot);
    // Numero di messaggi (file in cur/ e new/).
    static int count(const QString& folderDir);
    // Cancella; ritorna quanti file sono stati rimossi. *failed = quelli non rimossi.
    static int removeAll(const QString& folderDir, const QString& accountRoot, int* failed, QString* error);
};

} // namespace boma
