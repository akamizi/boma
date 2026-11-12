#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace boma {

// Lettura e modifica mirata di un .msmtprc. La modifica riscrive SOLO le righe
// delle chiavi gestite nel blocco dell'account scelto: commenti, [defaults], gli
// altri account e le chiavi non gestite restano com'erano. Le password in chiaro
// non si gestiscono (resta passwordeval, un comando); la chiave `password` non si
// scrive mai. Le funzioni di testo sono pure; save() scrive sul disco.
class MsmtpConfig {
public:
    // Chiavi modificabili dall'interfaccia.
    static QStringList managedKeys();
    static bool validAccountName(const QString& name);

    // Impostazioni di un account (chiavi in minuscolo -> valore) e di [defaults].
    static QMap<QString, QString> accountSettings(const QByteArray& text, const QString& account);
    static QMap<QString, QString> defaultSettings(const QByteArray& text);
    static bool hasAccount(const QByteArray& text, const QString& account);
    // true se il blocco contiene una chiave `password` in chiaro (sconsigliato).
    static bool hasPlainPassword(const QByteArray& text, const QString& account);

    // Testo aggiornato: per ogni chiave gestita in `values` (valore vuoto = rimuovi).
    // Se l'account non esiste ne aggiunge il blocco in fondo. Vuoto + *error se non valido
    // (nome account, valori su piu' righe, chiavi non gestite).
    static QByteArray applyAccount(const QByteArray& text, const QString& account,
                                   const QMap<QString, QString>& values, QString* error);

    // Scrive `text` in `path`: prima copia il file esistente in `path`.bak, poi sostituisce
    // il file in modo atomico mantenendo i permessi (0600 se nuovo: msmtp lo richiede).
    // Segue un link simbolico scrivendo sul file reale.
    static bool save(const QString& path, const QByteArray& text, QString* error);
};

} // namespace boma
