#include <QtTest>
#include <QApplication>
#include <QStandardPaths>
#include "ui/MessageView.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    static ParsedMessage both() {
        ParsedMessage m;
        m.plainText = "Solo testo QQQ";
        m.html = "<p>Versione html ZZZ</p>";
        return m;
    }
private slots:
    void init() { MessageView::setPreferHtml(false); }
    void testDefaultIsTextAndToggleSwitchesView() {
        MessageView v;
        v.resize(500, 400);
        v.show(both());
        QVERIFY(v.toPlainText().contains("QQQ"));
        QVERIFY(!v.toPlainText().contains("ZZZ"));
        QVERIFY(!v.showingHtml());
        QVERIFY(!v.htmlToggleButton()->isHidden());
        v.htmlToggleButton()->click();
        QVERIFY(v.toPlainText().contains("ZZZ"));
        QVERIFY(!v.toPlainText().contains("QQQ"));
        QVERIFY(v.showingHtml());
        v.htmlToggleButton()->click();
        QVERIFY(v.toPlainText().contains("QQQ"));
    }
    void testToggleResetsOnNextMessage() {
        MessageView v;
        v.show(both());
        v.htmlToggleButton()->click();
        v.show(both());
        QVERIFY(!v.showingHtml());
    }
    void testPreferenceMakesHtmlTheDefault() {
        MessageView::setPreferHtml(true);
        MessageView v;
        v.show(both());
        QVERIFY(v.showingHtml());
        QVERIFY(v.toPlainText().contains("ZZZ"));
        MessageView::setPreferHtml(false);
        v.reloadPreference();
        v.show(both());
        QVERIFY(!v.showingHtml());
    }
    void testNoToggleWhenOnlyOneFormat() {
        MessageView v;
        ParsedMessage t; t.plainText = "solo testo";
        v.show(t);
        QVERIFY(v.htmlToggleButton()->isHidden());
        ParsedMessage h; h.html = "<p>solo html HHH</p>";
        v.show(h);
        QVERIFY(v.htmlToggleButton()->isHidden());
        QVERIFY(v.toPlainText().contains("HHH"));
    }
};
int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName("BomaTest");
    QApplication::setApplicationName("BomaTest");
    T t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_htmltoggle.moc"
