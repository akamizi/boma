#include <QtTest>
#include <QApplication>
#include <QStandardPaths>
#include "ui/ComposeWindow.h"
#include "ui/RichTextToolBar.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    static QAction* act(RichTextToolBar* b, const char* n) { return b->findChild<QAction*>(n); }
private slots:
    void testLinkSchemes() {
        QVERIFY(RichTextToolBar::isAllowedLink("https://example.org/x"));
        QVERIFY(RichTextToolBar::isAllowedLink("mailto:a@b.it"));
        QVERIFY(!RichTextToolBar::isAllowedLink("javascript:alert(1)"));
        QVERIFY(!RichTextToolBar::isAllowedLink("file:///etc/passwd"));
    }
    void testBarOnlyInHtmlMode() {
        ComposeWindow w{Config()};
        w.show();
        QVERIFY(!w.formatBar()->isVisible());
        w.htmlCheck()->setChecked(true);
        QVERIFY(w.formatBar()->isVisible());
        w.htmlCheck()->setChecked(false);
        QVERIFY(!w.formatBar()->isVisible());
    }
    void testBoldItalicListAndLinkEndUpInHtml() {
        ComposeWindow w{Config()};
        w.show();
        w.htmlCheck()->setChecked(true);
        QTextEdit* e = w.richEdit();
        RichTextToolBar* b = w.formatBar();
        b->setLinkPrompt([](const QString&) { return QStringLiteral("https://example.org/"); });
        e->setPlainText("grassetto corsivo link");
        QTextCursor c = e->textCursor();
        c.setPosition(0); c.setPosition(9, QTextCursor::KeepAnchor); e->setTextCursor(c);
        act(b, "fmtBold")->trigger();
        c.setPosition(10); c.setPosition(17, QTextCursor::KeepAnchor); e->setTextCursor(c);
        act(b, "fmtItalic")->trigger();
        c.setPosition(18); c.setPosition(22, QTextCursor::KeepAnchor); e->setTextCursor(c);
        act(b, "fmtLink")->trigger();
        const QString html = w.currentMessage().html;
        QVERIFY(html.contains("font-weight"));
        QVERIFY(html.contains("italic"));
        QVERIFY(html.contains("href=\"https://example.org/\""));
        e->selectAll();
        act(b, "fmtBullets")->trigger();
        QVERIFY(w.currentMessage().html.contains("<ul"));
        QCOMPARE(w.currentMessage().plainText.trimmed(), QString("grassetto corsivo link"));
    }
    void testDisallowedLinkIsIgnored() {
        ComposeWindow w{Config()};
        w.htmlCheck()->setChecked(true);
        w.formatBar()->setLinkPrompt([](const QString&) { return QStringLiteral("javascript:alert(1)"); });
        w.richEdit()->setPlainText("x");
        w.richEdit()->selectAll();
        act(w.formatBar(), "fmtLink")->trigger();
        QVERIFY(!w.currentMessage().html.contains("javascript"));
    }
};
int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    T t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_richtext.moc"
