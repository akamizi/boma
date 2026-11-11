#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

namespace boma {

// Unico punto in cui Boma scrive direttamente nella Maildir (Inviata, Bozze).
// Protocollo Maildir: file unico in tmp/, fsync, poi link in cur/ senza mai
// sovrascrivere. Restituisce il percorso finale, o stringa vuota con *error.
class MaildirWriter {
public:
    static QString deliver(const QString& maildirFolder, const QByteArray& message,
                           QString flags, QString* error);
    // Controllo (solo lettura): la cartella e' una Maildir in cui deliver() puo'
    // scrivere (tmp/new/cur presenti, tmp e cur scrivibili). Vuoto = valida,
    // altrimenti il motivo. Non crea mai nulla.
    static QString checkFolder(const QString& maildirFolder);
    // Salvataggio di emergenza fuori dalla Maildir: nuovo file <dir>/<nome>.eml
    // (0600, mai sovrascritto, fsync). Crea dir (0700) se manca. Percorso o vuoto con *error.
    static QString saveFallback(const QString& dir, const QByteArray& message, QString* error);

    // Solo per i test: sostituisce il generatore del nome unico (nullptr = default).
    static void setNameGeneratorForTest(std::function<QString()> gen);
};

} // namespace boma
