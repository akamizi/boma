#include "ui/Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>

namespace boma {

QStringList Theme::availableStyles() { return QStyleFactory::keys(); }

bool Theme::schemeSupported() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    return true;
#else
    return false;
#endif
}

Theme Theme::load() {
    QSettings s;
    Theme t;
    t.style = s.value(QStringLiteral("appearance/style")).toString();
    // AppImage: lo stile Adwaita incluso e' il predefinito (somiglia a GTK), finche' non se ne sceglie un altro
    if (t.style.isEmpty() && qEnvironmentVariableIsSet("APPIMAGE")) {
        const QStringList keys = QStyleFactory::keys();
        for (const QString& k : keys)
            if (k.compare(QLatin1String("Adwaita"), Qt::CaseInsensitive) == 0) t.style = k;
    }
    const QString sc = s.value(QStringLiteral("appearance/scheme"), QStringLiteral("system")).toString();
    t.scheme = (sc == QLatin1String("light") || sc == QLatin1String("dark")) ? sc : QStringLiteral("system");
    return t;
}

void Theme::save() const {
    QSettings s;
    s.setValue(QStringLiteral("appearance/style"), style);
    s.setValue(QStringLiteral("appearance/scheme"), scheme);
}

namespace {
QPalette darkPalette() {
    QPalette p;
    const QColor win(45, 45, 48), base(30, 30, 32), text(228, 228, 228), dis(130, 130, 130), hl(61, 142, 224);
    p.setColor(QPalette::Window, win);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(40, 40, 43));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, win);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, dis);
    p.setColor(QPalette::Highlight, hl);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(110, 170, 255));
    p.setColor(QPalette::Mid, QColor(70, 70, 74));
    p.setColor(QPalette::Dark, QColor(25, 25, 27));
    p.setColor(QPalette::Light, QColor(80, 80, 84));
    for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, r, dis);
    return p;
}
} // namespace

void Theme::apply() const {
    // solo stili esistenti: un nome sparito (plugin rimosso) non rompe l'avvio
    if (!style.isEmpty() && QStyleFactory::keys().contains(style, Qt::CaseInsensitive)) QApplication::setStyle(style);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(scheme == QLatin1String("dark")    ? Qt::ColorScheme::Dark
                                                  : scheme == QLatin1String("light") ? Qt::ColorScheme::Light
                                                                                     : Qt::ColorScheme::Unknown);
#endif
    // Riserva: senza un tema di piattaforma che gestisca lo schema (es. QT_QPA_PLATFORMTHEME=qt5ct
    // con Qt6) la richiesta e' ignorata; applico una palette propria cosi' la scelta ha effetto.
    const bool wantDark = scheme == QLatin1String("dark"), wantLight = scheme == QLatin1String("light");
    if (wantDark || wantLight) {
        const bool isDark = QApplication::palette().color(QPalette::Base).lightness() < 128;
        if (wantDark && !isDark) QApplication::setPalette(darkPalette());
        else if (wantLight && isDark) QApplication::setPalette(QApplication::style()->standardPalette());
    }
}

} // namespace boma
