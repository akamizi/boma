#include <QApplication>

#include "core/Config.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include <QStatusBar>

#include "core/Version.h"

int main(int argc, char *argv[]) {
    int code = 0;
    if (boma::handleCommandLine(argc, argv, &code)) return code; // prima di QApplication: niente display
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
