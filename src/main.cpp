#include <QApplication>

#include "core/Config.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include <QStatusBar>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Boma"));
    QApplication::setOrganizationName(QStringLiteral("Boma"));
    boma::Theme::load().apply();
    QString error;
    const boma::Config config = boma::Config::load(boma::Config::defaultPath(), &error);
    boma::MainWindow w(config);
    w.show();
    if (!error.isEmpty()) w.statusBar()->showMessage(QObject::tr("Configurazione: %1").arg(error));
    return app.exec();
}
