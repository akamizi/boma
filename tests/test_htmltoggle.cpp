#include <QtTest>
#include <QBuffer>
#include <QImage>
#include <QTcpServer>
#include <QTcpSocket>
#include "mime/MessageParser.h"
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
    static QByteArray png() {
        QImage img(2, 2, QImage::Format_RGB32);
        img.fill(Qt::red);
        QByteArray out;
        QBuffer b(&out);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        return out;
    }
    void testParserCollectsInlineImagesByContentId() {
        const QByteArray raw =
            "From: a@x\nTo: b@x\nSubject: s\nMIME-Version: 1.0\n"
            "Content-Type: multipart/related; boundary=\"r\"\n\n"
            "--r\nContent-Type: text/html\n\n<p>ciao <img src=\"cid:LogoUno\"></p>\n"
            "--r\nContent-Type: image/png\nContent-ID: <LogoUno>\nContent-Transfer-Encoding: base64\n\n" +
            png().toBase64() + "\n--r--\n";
        const ParsedMessage m = MessageParser::parse(raw);
        QVERIFY(m.inlineImages.contains("logouno"));
        QVERIFY(!m.inlineImages.value("logouno").isEmpty());
        MessageView v;
        v.show(m);
        QVERIFY(v.document()->resource(QTextDocument::ImageResource, QUrl("cid:LogoUno")).isValid());
        QVERIFY(!v.document()->resource(QTextDocument::ImageResource, QUrl("cid:altro")).isValid());
    }
    void testRemoteImagesOnlyAfterExplicitClick() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int requests = 0;
        const QByteArray body = png();
        connect(&server, &QTcpServer::newConnection, this, [&]() {
            while (QTcpSocket* c = server.nextPendingConnection()) {
                connect(c, &QTcpSocket::readyRead, c, [c, &requests, body]() {
                    c->readAll();
                    ++requests;
                    c->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: " +
                             QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    c->disconnectFromHost();
                });
            }
        });
        const QString url = QStringLiteral("http://127.0.0.1:%1/x.png").arg(server.serverPort());
        ParsedMessage m;
        m.html = QStringLiteral("<p>x<img src=\"%1\"></p>").arg(url);
        MessageView v;
        v.resize(400, 300);
        v.show(m);
        QVERIFY(!v.remoteImagesButton()->isHidden());
        QTest::qWait(300);
        QCOMPARE(requests, 0);                       // nessuna richiesta da sola
        QVERIFY(!v.document()->toHtml().contains("127.0.0.1"));
        v.remoteImagesButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(v.remoteImagesLoaded(), 10000);
        QCOMPARE(requests, 1);
        QVERIFY(v.document()->resource(QTextDocument::ImageResource, QUrl(url)).isValid());
        QVERIFY(v.remoteImagesButton()->isHidden());
        v.show(m);                                   // un altro messaggio: di nuovo bloccate
        QVERIFY(!v.document()->resource(QTextDocument::ImageResource, QUrl(url)).isValid());
        QVERIFY(!v.remoteImagesButton()->isHidden());
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
