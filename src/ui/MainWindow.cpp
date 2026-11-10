#include "ui/MainWindow.h"

namespace boma {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Boma"));
    resize(1100, 700);
}

} // namespace boma
