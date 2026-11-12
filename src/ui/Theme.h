#pragma once
#include <QString>
#include <QStringList>

namespace boma {

// Aspetto dell'applicazione: stile Qt (Fusion, Windows, ...) e schema colori
// (sistema/chiaro/scuro). Preferenza dell'interfaccia, non della posta: sta in
// QSettings (~/.config/Boma/Boma.conf), separata dal config.ini degli account.
struct Theme {
    QString style;                              // vuoto = predefinito della piattaforma
    QString scheme = QStringLiteral("system"); // "system" | "light" | "dark"

    static QStringList availableStyles();
    static Theme load();
    void save() const;
    // Applica a QApplication. Lo schema richiede Qt 6.8; con Qt piu' vecchio e' ignorato.
    void apply() const;
    static bool schemeSupported();
};

} // namespace boma
