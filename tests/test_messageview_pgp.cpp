#include "ui/MessageView.h"
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

using namespace boma;

class T : public QObject {
    Q_OBJECT
    static ParsedMessage msg() {
        ParsedMessage m;
        m.from = QStringLiteral("a@example.test");
        m.subject = QStringLiteral("Prova");
        m.plainText = QStringLiteral("corpo del messaggio");
        return m;
    }
    static bool hasGreen(const QString& h) { return h.contains(QStringLiteral("1a7f37")); }

private slots:
    void testBadgePerStatus_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("marker");
        QTest::addColumn<bool>("green");
        QTest::newRow("valid") << int(SignatureStatus::Valid) << QStringLiteral("Firma valida: Alice") << true;
        QTest::newRow("invalid") << int(SignatureStatus::Invalid) << QStringLiteral("Firma non valida") << false;
        QTest::newRow("unknown") << int(SignatureStatus::UnknownKey) << QStringLiteral("Firma di una chiave sconosciuta") << false;
        QTest::newRow("expired") << int(SignatureStatus::ExpiredKey) << QStringLiteral("Chiave del firmatario scaduta") << false;
        QTest::newRow("revoked") << int(SignatureStatus::RevokedKey) << QStringLiteral("Chiave del firmatario revocata") << false;
        QTest::newRow("none") << int(SignatureStatus::None) << QString() << false;
    }
    void testBadgePerStatus() {
        QFETCH(int, status);
        QFETCH(QString, marker);
        QFETCH(bool, green);
        MessageView v;
        v.show(msg());
        v.setSignatureResult(static_cast<SignatureStatus>(status), QStringLiteral("Alice"));
        const QString plain = v.toPlainText();
        const QString html = v.toHtml();
        if (marker.isEmpty()) {
            QVERIFY(!plain.contains(QStringLiteral("Firma")));
            QVERIFY(!plain.contains(QStringLiteral("Chiave del firmatario")));
        } else {
            QVERIFY2(plain.contains(marker), qPrintable(plain));
        }
        QCOMPARE(hasGreen(html), green);
        if (status == int(SignatureStatus::Valid)) QVERIFY(plain.contains(QStringLiteral("✓")));
        QVERIFY(plain.contains(QStringLiteral("corpo del messaggio")));
    }
    void testSignerIsEscapedAndShowResetsBadge() {
        MessageView v;
        v.show(msg());
        v.setSignatureResult(SignatureStatus::Valid, QStringLiteral("<b>X</b><img src=x> %1 &amp;"));
        const QString plain = v.toPlainText();
        QVERIFY(plain.contains(QStringLiteral("<b>X</b><img src=x> %1 &amp;")));
        v.show(msg());
        QVERIFY(!v.toPlainText().contains(QStringLiteral("Firma")));
        QVERIFY(!hasGreen(v.toHtml()));
    }
    void testEncryptedShowsButtonAndNoBody() {
        MessageView v;
        v.resize(400, 300);
        v.show(msg());
        QVERIFY(!v.decryptButton()->isVisibleTo(&v));
        ParsedMessage m = msg();
        m.encryptedPgp = true;
        m.rawEncrypted = "CIFRATO";
        v.show(m);
        QVERIFY(v.decryptButton()->isVisibleTo(&v));
        QVERIFY(v.toPlainText().contains(QStringLiteral("Messaggio cifrato")));
        QVERIFY(!v.toPlainText().contains(QStringLiteral("corpo del messaggio")));
        QCOMPARE(v.encryptedData(), QByteArray("CIFRATO"));
    }
    void testDecryptConfirmAndSignal() {
        MessageView v;
        ParsedMessage m = msg();
        m.encryptedPgp = true;
        m.rawEncrypted = "C";
        v.show(m);
        QSignalSpy spy(&v, &MessageView::decryptRequested);
        bool answer = false;
        int asked = 0;
        v.setConfirmCallback([&] { ++asked; return answer; });
        v.decryptButton()->click();
        QCOMPARE(asked, 1);
        QCOMPARE(spy.count(), 0); // no: nessuna decifrazione
        QVERIFY(v.decryptButton()->isEnabled());
        answer = true;
        v.decryptButton()->click();
        QCOMPARE(spy.count(), 1);
        QVERIFY(!v.decryptButton()->isEnabled()); // niente doppie richieste
    }
    void testDecryptErrorShowsNoText() {
        MessageView v;
        ParsedMessage m = msg();
        m.encryptedPgp = true;
        v.show(m);
        v.setConfirmCallback([] { return true; });
        v.decryptButton()->click();
        v.showDecryptError(QStringLiteral("Decifrazione annullata."));
        QVERIFY(v.toPlainText().contains(QStringLiteral("Decifrazione annullata.")));
        QVERIFY(!v.toPlainText().contains(QStringLiteral("corpo del messaggio")));
        QVERIFY(v.decryptButton()->isEnabled());
    }
    void testShowDecrypted() {
        MessageView v;
        ParsedMessage m = msg();
        m.encryptedPgp = true;
        v.show(m);
        ParsedMessage inner = msg();
        inner.plainText = QStringLiteral("testo in chiaro");
        v.showDecrypted(inner);
        QVERIFY(v.toPlainText().contains(QStringLiteral("testo in chiaro")));
        QVERIFY(!v.decryptButton()->isVisibleTo(&v));
        QVERIFY(v.encryptedData().isEmpty());
    }
};

QTEST_MAIN(T)
#include "test_messageview_pgp.moc"
