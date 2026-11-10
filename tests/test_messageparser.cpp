#include "mime/MessageParser.h"
#include "ui/MessageView.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

using namespace boma;

static QByteArray fx(const char* name) {
    QFile f(QStringLiteral(BOMA_FIXTURES_DIR "/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("fixture mancante: %s", name);
    return f.readAll();
}

class T : public QObject {
    Q_OBJECT
private slots:
    void testPlainMessage() {
        QString err;
        auto m = MessageParser::parse(fx("plain.eml"), &err);
        QVERIFY(err.isEmpty());
        QCOMPARE(m.subject, QStringLiteral("Prova semplice"));
        QVERIFY(m.from.contains(QStringLiteral("mario@example.invalid")));
        QVERIFY(m.from.contains(QStringLiteral("Mario Rossi")));
        QVERIFY(m.to.contains(QStringLiteral("anna@example.invalid")));
        QVERIFY(m.cc.isEmpty());
        QVERIFY(m.date.isValid());
        QCOMPARE(m.date.toUTC(), QDateTime(QDate(2026, 11, 10), QTime(9, 0), QTimeZone::UTC));
        QVERIFY(m.plainText.contains(QStringLiteral("messaggio semplice")));
        QVERIFY(m.html.isEmpty());
        QVERIFY(m.attachments.isEmpty());
        QVERIFY(!m.signedPgp && !m.encryptedPgp);
    }
    void testMultipartAlternativePrefersPlain() {
        auto m = MessageParser::parse(fx("alternative.eml"), nullptr);
        QVERIFY(m.plainText.contains(QStringLiteral("Testo semplice")));
        QVERIFY(m.html.contains(QStringLiteral("<b>HTML</b>")));
        QVERIFY(m.attachments.isEmpty());
        MessageView v;
        v.show(m);
        QVERIFY(v.toPlainText().contains(QStringLiteral("Testo semplice")));
        QVERIFY(!v.toPlainText().contains(QStringLiteral("<b>")));
        // senza testo semplice si usa l'HTML
        ParsedMessage h;
        h.html = m.html;
        v.show(h);
        QVERIFY(v.toPlainText().contains(QStringLiteral("Testo HTML")));
    }
    void testQuotedPrintableAndBase64() {
        auto q = MessageParser::parse(fx("qp_utf8.eml"), nullptr);
        QCOMPARE(q.subject, QString::fromUtf8("Caffè e perché"));
        QVERIFY(q.plainText.contains(QString::fromUtf8("Perché il caffè € 3")));
        QVERIFY(q.plainText.contains(QStringLiteral("spezzata dal trasporto")));
        auto b = MessageParser::parse(fx("b64_latin1.eml"), nullptr);
        QCOMPARE(b.subject, QString::fromUtf8("café").replace(QStringLiteral("é"), QStringLiteral("æ")));
        QVERIFY(b.plainText.startsWith(QString::fromUtf8("Città è ù")));
        auto u = MessageParser::parse(fx("b64_utf8.eml"), nullptr);
        QVERIFY(u.plainText.startsWith(QString::fromUtf8("Perché è così")));
    }
    void testUnknownCharsetFallsBack() {
        QString err;
        auto m = MessageParser::parse(fx("unknown_charset.eml"), &err);
        QVERIFY(!m.plainText.isEmpty());
        QVERIFY(m.plainText.contains(QString::fromUtf8("Perché funziona")));
    }
    void testEmptySubjectAndMissingContentType() {
        QString err;
        auto m = MessageParser::parse(fx("no_subject.eml"), &err);
        QVERIFY(m.subject.isEmpty());
        QVERIFY(m.plainText.contains(QStringLiteral("senza Content-Type")));
        QVERIFY(!m.date.isValid());
        MessageView v;
        v.show(m); // non deve rompersi
    }
    void testAttachmentFilenameWithSpecialCharsIsSafe() {
        const QByteArray raw = fx("attachment_evil.eml");
        auto m = MessageParser::parse(raw, nullptr);
        QCOMPARE(m.attachments.size(), 1);
        const auto a = m.attachments.first();
        QCOMPARE(a.filename, QStringLiteral("passwd.txt"));
        QCOMPARE(a.size, qint64(13));
        QCOMPARE(a.mimeType, QStringLiteral("application/octet-stream"));
        QCOMPARE(MessageParser::attachmentData(raw, a.partIndex), QByteArray("DATI-ALLEGATO"));
        QVERIFY(MessageParser::attachmentData(raw, 99).isEmpty());
        // sanificazione diretta
        for (const QString& bad : {QStringLiteral("../../etc/pass\"wd\n.txt"), QStringLiteral("..\\..\\x\\y.txt"),
                                   QStringLiteral("/etc/passwd"), QStringLiteral(".bashrc"),
                                   QStringLiteral("a\x01\x7f.txt"), QStringLiteral("..")}) {
            const QString s = MessageParser::sanitizeFilename(bad);
            QVERIFY2(!s.isEmpty(), qPrintable(bad));
            QVERIFY2(!s.contains(QLatin1Char('/')) && !s.contains(QLatin1Char('\\')) && !s.contains(QLatin1Char('"')) &&
                         !s.startsWith(QLatin1Char('.')),
                     qPrintable(bad));
            for (QChar c : s) QVERIFY(c.unicode() >= 0x20 && c.unicode() != 0x7f);
        }
        QCOMPARE(MessageParser::sanitizeFilename(QString()), QStringLiteral("allegato"));
        QCOMPARE(MessageParser::sanitizeFilename(QStringLiteral("..")), QStringLiteral("allegato"));
    }
    void testSaveAttachmentNeverOverwrites() {
        const QByteArray raw = fx("attachment_evil.eml");
        QTemporaryDir d;
        QVERIFY(d.isValid());
        const QString dest = d.filePath(QStringLiteral("out.txt"));
        QString err;
        QVERIFY2(MessageParser::saveAttachment(raw, 1, dest, &err), qPrintable(err));
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("DATI-ALLEGATO"));
        f.close();
        // esiste gia': rifiuta e lascia intatto
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("MIO");
        f.close();
        err.clear();
        QVERIFY(!MessageParser::saveAttachment(raw, 1, dest, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("MIO"));
        f.close();
        // collegamento simbolico pendente: non va seguito
        const QString victim = d.filePath(QStringLiteral("victim"));
        QVERIFY(QFile::link(victim, d.filePath(QStringLiteral("sym"))));
        QVERIFY(!MessageParser::saveAttachment(raw, 1, d.filePath(QStringLiteral("sym")), &err));
        QVERIFY(!QFile::exists(victim));
        // indice non valido, cartella inesistente
        QVERIFY(!MessageParser::saveAttachment(raw, 42, d.filePath(QStringLiteral("n.txt")), &err));
        QVERIFY(!MessageParser::saveAttachment(raw, 1, d.filePath(QStringLiteral("no/n.txt")), &err));
        // nessun file temporaneo residuo
        QStringList left = QDir(d.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        left.removeAll(QStringLiteral("out.txt"));
        QVERIFY2(left.isEmpty(), qPrintable(left.join(',')));
    }
    void testDetectsPgpMimeSignedAndEncrypted() {
        auto s = MessageParser::parse(fx("pgp_signed.eml"), nullptr);
        QVERIFY(s.signedPgp);
        QVERIFY(!s.encryptedPgp);
        const QByteArray part = "Content-Type: text/plain; charset=utf-8\n"
                                "Content-Transfer-Encoding: quoted-printable\n\n"
                                "Ciao =C3=A8 firmato.";
        QCOMPARE(s.rawSignedPart, part);
        QVERIFY(s.rawSignature.startsWith("-----BEGIN PGP SIGNATURE-----"));
        QVERIFY(s.rawSignature.contains("-----END PGP SIGNATURE-----"));
        QVERIFY(s.plainText.contains(QString::fromUtf8("Ciao è firmato.")));
        QVERIFY(s.attachments.isEmpty()); // la firma non e' un allegato da mostrare
        // stessa cosa con fine riga CRLF: i byte restano CRLF
        QByteArray crlf = fx("pgp_signed.eml");
        crlf.replace("\n", "\r\n");
        auto c = MessageParser::parse(crlf, nullptr);
        QVERIFY(c.signedPgp);
        QByteArray partCrlf = part;
        partCrlf.replace("\n", "\r\n");
        QCOMPARE(c.rawSignedPart, partCrlf);

        auto e = MessageParser::parse(fx("pgp_encrypted.eml"), nullptr);
        QVERIFY(e.encryptedPgp);
        QVERIFY(!e.signedPgp);
        QVERIFY(e.rawEncrypted.startsWith("-----BEGIN PGP MESSAGE-----"));
        QVERIFY(e.rawEncrypted.contains("-----END PGP MESSAGE-----"));
        QVERIFY(e.plainText.isEmpty());
    }
    void testRemoteImagesAreNotLoaded() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString url = QStringLiteral("http://127.0.0.1:%1/x.png").arg(server.serverPort());
        ParsedMessage p;
        p.html = QStringLiteral("<html><body style=\"background:url(%1)\"><img src=\"%1\">"
                                "<table background=\"%1\"><tr><td>x</td></tr></table>"
                                "<img src=\"http://example.invalid/x.png\"></body></html>").arg(url);
        MessageView v;
        v.resize(400, 300);
        v.show(p);
        QTest::qWait(100);
        v.document()->toHtml();
        QVERIFY(!server.waitForNewConnection(300));
        // richiesta diretta di risorsa: sempre rifiutata
        for (const QString& u : {url, QStringLiteral("https://example.invalid/a.png"),
                                 QStringLiteral("ftp://example.invalid/a"), QStringLiteral("file:///etc/passwd"),
                                 QStringLiteral("cid:abc@x"), QStringLiteral("relativo.png")}) {
            const QVariant r = v.document()->resource(QTextDocument::ImageResource, QUrl(u));
            QVERIFY2(!r.isValid() || r.isNull(), qPrintable(u + QLatin1Char(' ') + QString::fromLatin1(r.typeName())));
        }
        QVERIFY(!server.waitForNewConnection(200));
        // il sorgente sanificato non contiene piu' l'URL remoto
        QVERIFY(!MessageView::sanitizeHtml(p.html).contains(QStringLiteral("127.0.0.1")));
    }
    void testHtmlIsSanitizedAndLinksNotOpened() {
        auto m = MessageParser::parse(fx("html_remote.eml"), nullptr);
        const QString s = MessageView::sanitizeHtml(m.html);
        for (const char* bad : {"<script", "alert(1)", "<iframe", "onload", "onclick", "javascript:", "example.invalid/x.png"})
            QVERIFY2(!s.contains(QLatin1String(bad)), bad);
        QVERIFY(s.contains(QStringLiteral("Ciao")));
        MessageView v;
        v.show(m);
        QVERIFY(v.toPlainText().contains(QStringLiteral("Ciao")));
        QVERIFY(!v.toPlainText().contains(QStringLiteral("alert")));
        // link: nessuna navigazione, solo segnale
        QVERIFY(!v.openLinks());
        QVERIFY(!v.openExternalLinks());
        ParsedMessage p;
        p.plainText = QStringLiteral("x");
        p.subject = QStringLiteral("<b>&evil</b>");
        v.show(p);
        QVERIFY(v.toPlainText().contains(QStringLiteral("<b>&evil</b>")));
        QVERIFY(v.toHtml().contains(QStringLiteral("&lt;b&gt;")));
    }
    void testPlainTextIsEscaped() {
        ParsedMessage p;
        p.plainText = QStringLiteral("<img src=\"http://example.invalid/t.png\">\n  riga   due\n");
        MessageView v;
        v.show(p);
        QVERIFY(v.toPlainText().contains(QStringLiteral("<img src=")));
        QVERIFY(v.toPlainText().contains(QStringLiteral("riga   due")));
    }
    void testMalformedMimeDoesNotCrash() {
        QList<QByteArray> inputs = {
            QByteArray(), QByteArray("\0\0\0", 3), QByteArray("garbage without headers"),
            QByteArray("\xff\xfe\xfd\x00\x01", 5), fx("malformed_boundary.eml"), fx("truncated.eml"),
            QByteArray("Subject: ") + QByteArray(2000000, 'A') + "\n\nbody",
            QByteArray("Content-Type: multipart/mixed; boundary=x\n\n") + QByteArray("--x\n", 4).repeated(5000),
            QByteArray("Content-Type: multipart/mixed; boundary=x\n\n--x\nContent-Type: multipart/mixed; boundary=x\n\n--x\n").repeated(300),
            QByteArray("Content-Type: multipart/signed; protocol=\"application/pgp-signature\"; boundary=b\n\n--b\n"),
            QByteArray("Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=b\n\n--b--\n"),
            QByteArray("Subject: =?utf-8?B?!!!!?= =?x-nope?Q?a?=\n\n"),
        };
        for (int i = 0; i < inputs.size(); ++i) {
            QString err;
            const auto m = MessageParser::parse(inputs[i], &err);
            MessageParser::attachmentData(inputs[i], 0);
            MessageView v;
            v.show(m);
            if (inputs[i].isEmpty()) QVERIFY(!err.isEmpty());
        }
        // la parte troncata resta leggibile
        auto t = MessageParser::parse(fx("truncated.eml"), nullptr);
        QVERIFY(t.plainText.contains(QStringLiteral("Inizio del testo")));
        // input vuoto senza puntatore errore
        MessageParser::parse(QByteArray(), nullptr);
    }
};
QTEST_MAIN(T)
#include "test_messageparser.moc"
