#include "ui/MessageView.h"
#include <QStandardPaths>
#include <QApplication>
#include <QPushButton>
#include <QStandardPaths>
#include <QApplication>
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
        v.setSignatureResult(static_cast<SignatureStatus>(status), QStringLiteral("Alice <a@example.test>"));
        QLabel* b = v.signatureBadge();
        if (marker.isEmpty()) {
            QVERIFY(!b->isVisibleTo(&v));
            QVERIFY(b->text().isEmpty());
        } else {
            QVERIFY(b->isVisibleTo(&v));
            QVERIFY2(b->text().contains(marker), qPrintable(b->text()));
        }
        QCOMPARE(b->styleSheet().contains(QStringLiteral("1a7f37")), green);
        if (status == int(SignatureStatus::Valid)) QVERIFY(b->text().contains(QStringLiteral("\u2713")));
        // nel documento non c'e' mai nulla del badge
        QVERIFY(!v.toPlainText().contains(QStringLiteral("Firma")));
        QVERIFY(!hasGreen(v.toHtml()));
        QVERIFY(v.toPlainText().contains(QStringLiteral("corpo del messaggio")));
    }
    // Firma valida ma di un'identita' diversa dal mittente (Da): firmatario mostrato,
    // avviso e mai il verde.
    void testSignerDifferentFromSenderWarns() {
        MessageView v;
        ParsedMessage m = msg();
        m.from = QStringLiteral("Banca <servizio@banca.example>");
        v.show(m);
        v.setSignatureResult(SignatureStatus::Valid, QStringLiteral("Mallory <mallory@evil.example>"));
        QLabel* b = v.signatureBadge();
        QVERIFY(b->isVisibleTo(&v));
        QVERIFY2(b->text().contains(QStringLiteral("mallory@evil.example")), qPrintable(b->text()));
        QVERIFY2(b->text().contains(QStringLiteral("servizio@banca.example")), qPrintable(b->text()));
        QVERIFY2(!b->text().contains(QStringLiteral("\u2713")), qPrintable(b->text()));
        QVERIFY(!b->styleSheet().contains(QStringLiteral("1a7f37")));
        // stesso indirizzo (maiuscole diverse, nomi diversi): verde
        m.from = QStringLiteral("Alice R. <Alice@Example.test>");
        v.show(m);
        v.setSignatureResult(SignatureStatus::Valid, QStringLiteral("Alice <alice@example.test>"));
        QVERIFY(b->styleSheet().contains(QStringLiteral("1a7f37")));
        // uid senza <>: solo indirizzo
        m.from = QStringLiteral("alice@example.test");
        v.show(m);
        v.setSignatureResult(SignatureStatus::Valid, QStringLiteral("alice@example.test"));
        QVERIFY(b->styleSheet().contains(QStringLiteral("1a7f37")));
        // firmatario senza indirizzo riconoscibile: nessun verde
        v.show(m);
        v.setSignatureResult(SignatureStatus::Valid, QStringLiteral("Alice"));
        QVERIFY(!b->styleSheet().contains(QStringLiteral("1a7f37")));
        QVERIFY(b->text().contains(QStringLiteral("Alice")));
    }
    void testSenderHtmlCannotForgeBadge() {
        MessageView v;
        ParsedMessage m = msg();
        m.plainText.clear();
        m.html = QStringLiteral("<p style=\"color:#1a7f37\">&#10003; Firma valida: Banca</p>");
        v.show(m);
        QVERIFY(!v.signatureBadge()->isVisibleTo(&v));
        QVERIFY(v.signatureBadge()->text().isEmpty());
        v.setSignatureResult(SignatureStatus::Invalid, QString());
        QVERIFY(!v.signatureBadge()->styleSheet().contains(QStringLiteral("1a7f37")));
    }
    void testSignerIsPlainTextAndShowResetsBadge() {
        MessageView v;
        v.show(msg());
        v.setSignatureResult(SignatureStatus::Valid, QStringLiteral("<b>X</b><img src=x> %1 &amp;"));
        QCOMPARE(v.signatureBadge()->textFormat(), Qt::PlainText);
        QVERIFY(v.signatureBadge()->text().contains(QStringLiteral("<b>X</b><img src=x> %1 &amp;")));
        v.show(msg());
        QVERIFY(!v.signatureBadge()->isVisibleTo(&v));
        QVERIFY(v.signatureBadge()->text().isEmpty());
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
        QCOMPARE(v.decryptErrorLabel()->text(), QStringLiteral("Decifrazione annullata."));
        QVERIFY(v.decryptErrorLabel()->isVisibleTo(&v));
        QVERIFY(!v.toPlainText().contains(QStringLiteral("Decifrazione annullata.")));
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

int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true); // QSettings di prova, non quelle dell'utente
    QApplication app(argc, argv);
    QApplication::setOrganizationName("BomaTest");
    QApplication::setApplicationName("BomaTest");
    T t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_messageview_pgp.moc"
