#pragma once

#include <QMainWindow>

namespace boma {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
};

} // namespace boma
