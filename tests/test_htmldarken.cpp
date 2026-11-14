#include <QtTest>
#include <QApplication>
#include <QStandardPaths>
#include "ui/HtmlDarken.h"
#include "ui/MessageView.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
private slots:
    void testLightBackgroundBecomesDark() {
        bool ch = false;
        const QString out = darkenHtml("<body bgcolor=\"#ffffff\"><p style=\"color:#000000\">Ciao colore: #fff</p></body>", &ch);
        QVERIFY(ch);
        QVERIFY(!out.contains("bgcolor=\"#ffffff\""));
        QVERIFY(out.contains("bgcolor=\"#1a1a1a\"")); // bianco -> L 0.1
        QVERIFY(out.contains("color:#ebebeb"));
        QVERIFY(out.contains("Ciao colore: #fff")); // il testo non si tocca
    }
    void testNamedRgbAndStyleBlock() {
        const QString out = darkenHtml("<style>td{background-color:white;color:rgb(0,0,0)}</style><td>x</td>");
        QVERIFY(out.contains("background-color:#1a1a1a"));
        QVERIFY(out.contains("color:#ebebeb"));
    }
    void testAlreadyDarkIsUntouched() {
        bool ch = true;
        const QString in = "<table bgcolor=\"#101010\"><td style=\"color:#eeeeee\">x</td></table>";
        QCOMPARE(darkenHtml(in, &ch), in);
        QVERIFY(!ch);
    }
    void testSmallDarkDetailDoesNotBlockAdaptation() {
        bool ch = false;
        const QString h = "<style>#m>b{background-color:#000000}</style><body bgcolor=\"#ffffff\"><table style=\"background:#ffffff\"><tr><td style=\"background-color:#f3f3f3\">x</td></tr></table></body>";
        darkenHtml(h, &ch);
        QVERIFY(ch);
    }
    void testNoColorsNoChange() {
        bool ch = true;
        QCOMPARE(darkenHtml("<p>ciao</p>", &ch), QString("<p>ciao</p>"));
        QVERIFY(!ch);
    }
    void testViewAdaptsOnlyWithDarkPalette() {
        ParsedMessage m;
        m.html = "<body bgcolor=\"#ffffff\"><p>ZZZ</p></body>";
        MessageView light;
        QPalette pl; pl.setColor(QPalette::Base, Qt::white); light.setPalette(pl);
        light.show(m);
        QVERIFY(light.darkToggleButton()->isHidden());
        QVERIFY(!light.darkAdapted());
        MessageView dark;
        QPalette pd; pd.setColor(QPalette::Base, QColor(30, 30, 30)); dark.setPalette(pd);
        dark.show(m);
        QVERIFY(dark.darkAdapted());
        QVERIFY(!dark.darkToggleButton()->isHidden());
        dark.darkToggleButton()->click();
        QVERIFY(!dark.darkAdapted());
        dark.show(m); // si ripristina al messaggio successivo
        QVERIFY(dark.darkAdapted());
    }
};
int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    T t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_htmldarken.moc"
