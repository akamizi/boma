#include "ui/Theme.h"

#include <QApplication>
#include <QGuiApplication>
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
    const QString sc = s.value(QStringLiteral("appearance/scheme"), QStringLiteral("system")).toString();
    t.scheme = (sc == QLatin1String("light") || sc == QLatin1String("dark")) ? sc : QStringLiteral("system");
    return t;
}

void Theme::save() const {
    QSettings s;
    s.setValue(QStringLiteral("appearance/style"), style);
    s.setValue(QStringLiteral("appearance/scheme"), scheme);
}

void Theme::apply() const {
    // solo stili esistenti: un nome sparito (plugin rimosso) non rompe l'avvio
    if (!style.isEmpty() && QStyleFactory::keys().contains(style, Qt::CaseInsensitive)) QApplication::setStyle(style);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(scheme == QLatin1String("dark")    ? Qt::ColorScheme::Dark
                                                  : scheme == QLatin1String("light") ? Qt::ColorScheme::Light
                                                                                     : Qt::ColorScheme::Unknown);
#endif
}

} // namespace boma
