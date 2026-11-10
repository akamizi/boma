#include <QtTest>

#include "ui/MainWindow.h"

class SmokeTest : public QObject {
    Q_OBJECT
private slots:
    void testMainWindowConstructs() {
        boma::MainWindow w;
        QCOMPARE(w.windowTitle(), QString("Boma"));
    }
};

QTEST_MAIN(SmokeTest)
#include "test_smoke.moc"
