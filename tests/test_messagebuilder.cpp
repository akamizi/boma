#include <QtTest>
#include <sys/stat.h>
#include "mime/MessageBuilder.h"
#include "mime/MessageParser.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    static OutgoingMessage base() {
        OutgoingMessage m;
        m.identity.address = "mario@example.org";
        m.identity.displayName = "Mario Rossi";
        m.to = {"Anna <anna@example.com>"};
        m.subject = "Ciao";
        m.plainText = "Corpo del messaggio";
        return m;
    }
    static QByteArray headerPart(const QByteArray& raw) {
        const int i = raw.indexOf("\n\n");
        return i < 0 ? raw : raw.left(i);
    }
    static QString tmpFile(QTemporaryDir& d, const QString& name, const QByteArray& data) {
        const QString p = d.filePath(name);
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write(data);
        return p;
    }
private slots:
    void testPlainOnlyHasNoHtmlPart() {
        QString err;
        const QByteArray raw = MessageBuilder::build(base(), &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        QVERIFY(!raw.contains("multipart/"));
        QVERIFY(!raw.contains("text/html"));
        QVERIFY(raw.contains("MIME-Version: 1.0"));
        QVERIFY(raw.contains("charset=utf-8") || raw.contains("charset=\"utf-8\""));
        QVERIFY(!raw.contains("User-Agent"));
        QVERIFY(!raw.contains("X-Mailer"));
        QVERIFY(raw.contains("Date: "));
    }
    void testHtmlAddsAlternative() {
        OutgoingMessage m = base();
        m.html = "<p>Corpo <b>html</b></p>";
        QString err;
        const QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        QVERIFY(raw.contains("multipart/alternative"));
        QVERIFY(raw.indexOf("text/plain") < raw.indexOf("text/html"));
        const ParsedMessage p = MessageParser::parse(raw);
        QVERIFY(p.plainText.contains("Corpo del messaggio"));
        QVERIFY(p.html.contains("<b>html</b>"));
        // testo vuoto: ricavato dall'HTML
        m.plainText.clear();
        const ParsedMessage q = MessageParser::parse(MessageBuilder::build(m, &err));
        QVERIFY(q.plainText.contains("Corpo html"));
        QVERIFY(!q.plainText.contains("<b>"));
    }
    void testSignatureAppendedPerIdentity() {
        OutgoingMessage m = base();
        m.identity.signature = "Mario\nSito: example.org";
        QString err;
        ParsedMessage p = MessageParser::parse(MessageBuilder::build(m, &err));
        QVERIFY(p.plainText.contains("\n-- \nMario\nSito: example.org"));
        QVERIFY(p.plainText.indexOf("Corpo") < p.plainText.indexOf("-- "));
        m.identity.signature.clear();
        p = MessageParser::parse(MessageBuilder::build(m, &err));
        QVERIFY(!p.plainText.contains("-- "));
    }
    void testAttachmentsAreEncodedAndNamedSafely() {
        QTemporaryDir d;
        const QByteArray data = QByteArray("\x00\x01\xff binary", 10);
        OutgoingMessage m = base();
        m.attachmentPaths = {tmpFile(d, "relazione \xc3\xa8.pdf", data), tmpFile(d, "b.txt", "ciao")};
        QString err;
        const QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        QVERIFY(raw.contains("multipart/mixed"));
        QVERIFY(raw.contains("Content-Transfer-Encoding: base64"));
        const ParsedMessage p = MessageParser::parse(raw);
        QCOMPARE(p.attachments.size(), 2);
        QCOMPARE(p.attachments[0].filename, QString::fromUtf8("relazione \xc3\xa8.pdf"));
        QCOMPARE(p.attachments[1].filename, QString("b.txt"));
        QCOMPARE(MessageParser::attachmentData(raw, p.attachments[0].partIndex), data);
        QVERIFY(!raw.contains(d.path().toUtf8())); // niente percorsi nel messaggio
        QVERIFY(p.plainText.contains("Corpo del messaggio"));
    }
    void testNonAsciiSubjectIsEncoded() {
        OutgoingMessage m = base();
        m.subject = QString::fromUtf8("Perché è già così");
        m.identity.displayName = QString::fromUtf8("Mario Rossì");
        m.plainText = QString::fromUtf8("perché");
        QString err;
        const QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        const QByteArray h = headerPart(raw);
        for (char c : h) QVERIFY(uchar(c) < 0x80);
        QVERIFY(h.contains("=?"));
        const ParsedMessage p = MessageParser::parse(raw);
        QCOMPARE(p.subject, m.subject);
        QVERIFY(p.from.contains(QString::fromUtf8("Rossì")));
        QVERIFY(raw.contains("quoted-printable"));
    }
    void testReplyKeepsInReplyToAndReferences() {
        OutgoingMessage m = base();
        m.inReplyToMessageId = "c@x";
        m.referencesIds = {"<a@x>", "b@x", "<c@x>"};
        QString err;
        QByteArray h = headerPart(MessageBuilder::build(m, &err));
        QVERIFY2(!h.isEmpty(), qPrintable(err));
        QVERIFY(h.contains("In-Reply-To: <c@x>"));
        const int r = h.indexOf("References:");
        QVERIFY(r >= 0);
        const QByteArray ref = h.mid(r).split('\n').first();
        QVERIFY(ref.contains("<a@x>") && ref.contains("<b@x>"));
        QCOMPARE(ref.count("<c@x>"), 1);
        QVERIFY(ref.indexOf("<c@x>") > ref.indexOf("<b@x>"));
        // cap a 20
        m.referencesIds.clear();
        for (int i = 0; i < 30; ++i) m.referencesIds << QString("m%1@x").arg(i);
        h = headerPart(MessageBuilder::build(m, &err));
        QVERIFY(!h.contains("<m0@x>") && !h.contains("<m10@x>"));
        QVERIFY(h.contains("<m11@x>") && h.contains("<m29@x>") && h.contains("<c@x>"));
    }
    void testBccNeverAppearsInHeaders() {
        QTemporaryDir d;
        OutgoingMessage m = base();
        m.bcc = {"Segreto <hidden@example.net>"};
        m.html = "<p>x</p>";
        m.attachmentPaths = {tmpFile(d, "a.txt", "x")};
        QString err;
        const QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        QVERIFY(!raw.toLower().contains("bcc"));
        QVERIFY(!raw.contains("hidden@example.net"));
        QVERIFY(MessageBuilder::envelopeRecipients(m).contains("hidden@example.net"));
    }
    void testEnvelopeRecipientsDeduplicates() {
        OutgoingMessage m = base();
        m.to = {"Anna <anna@example.com>", "b@x.it"};
        m.cc = {"ANNA@example.com", "c@x.it"};
        m.bcc = {"b@x.it", "d@x.it"};
        const QStringList r = MessageBuilder::envelopeRecipients(m);
        QCOMPARE(r, (QStringList{"anna@example.com", "b@x.it", "c@x.it", "d@x.it"}));
    }
    void testHeaderInjectionIsNeutralised() {
        QString err;
        OutgoingMessage m = base();
        m.subject = "x\r\nBcc: evil@x.it";
        QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        QVERIFY(!raw.toLower().contains("\nbcc:"));
        for (const QByteArray& line : headerPart(raw).split('\n')) QVERIFY(!line.toLower().startsWith("bcc"));
        QVERIFY(!headerPart(raw).contains("\nevil")); // nessuna riga iniettata
        QCOMPARE(MessageParser::parse(raw).subject, QString("x Bcc: evil@x.it"));

        m = base();
        m.identity.displayName = "Mario\r\nBcc: evil@x.it";
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
        QVERIFY(!err.isEmpty());

        m = base();
        m.to = {"Anna\r\nBcc: evil@x.it <anna@example.com>"};
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());

        for (const QString& bad : {QString("a@b.it>, evil@x.it"), QString("a@b.it, evil@x.it"),
                                   QString("<a@b.it> <evil@x.it>"), QString("a@b.it\nBcc: e@x.it"),
                                   QString("not an address"), QString("@x.it"), QString("a@"), QString("")}) {
            m = base();
            m.to = {bad};
            err.clear();
            QVERIFY2(MessageBuilder::build(m, &err).isEmpty(), qPrintable(bad));
            QVERIFY(!err.isEmpty());
        }
        m = base();
        m.inReplyToMessageId = "x@y>\r\nBcc: evil@x.it";
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
        m = base();
        m.referencesIds = {"a@b\nBcc: e@x.it"};
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
    }
    void testInvalidAddressNamesIt() {
        OutgoingMessage m = base();
        m.cc = {"rotto@@x.it"};
        QString err;
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
        QVERIFY2(err.contains("rotto@@x.it"), qPrintable(err));
        m = base();
        m.identity.address = "senzachiocciola";
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
    }
    void testQuotedDisplayNameWithCommaIsKept() {
        OutgoingMessage m = base();
        m.to = {"\"Rossi, Anna\" <anna@example.com>"};
        QString err;
        const QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        QVERIFY(MessageParser::parse(raw).to.contains("Rossi, Anna"));
    }
    void testMissingAttachmentFails() {
        OutgoingMessage m = base();
        m.attachmentPaths = {"/nonexistent/zzz.bin"};
        QString err;
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
        QVERIFY2(err.contains("zzz.bin"), qPrintable(err));
        QTemporaryDir d;
        m.attachmentPaths = {d.path()}; // directory
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
    }
    void testOversizeAttachmentRefused() {
        QTemporaryDir d;
        OutgoingMessage m = base();
        QFile f(d.filePath("big.bin"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.resize(MessageBuilder::kMaxAttachmentBytes + 1)); // sparso
        f.close();
        m.attachmentPaths = {f.fileName()};
        QString err;
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
        QVERIFY(err.contains("25"));
        // totale oltre 40 MiB con allegati singoli ammessi
        m.attachmentPaths.clear();
        for (int i = 0; i < 2; ++i) {
            QFile g(d.filePath(QString("p%1.bin").arg(i)));
            QVERIFY(g.open(QIODevice::WriteOnly));
            QVERIFY(g.resize(21LL * 1024 * 1024));
            g.close();
            m.attachmentPaths << g.fileName();
        }
        err.clear();
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
        QVERIFY(err.contains("40"));
    }
    void testMessageIdUniqueAndDomainFromFrom() {
        QSet<QByteArray> ids;
        for (int i = 0; i < 50; ++i) {
            QString err;
            const QByteArray h = headerPart(MessageBuilder::build(base(), &err));
            const int s = h.toLower().indexOf("message-id: <"); // GMime scrive "Message-Id"
            QVERIFY(s >= 0);
            const QByteArray id = h.mid(s).split('\n').first();
            QVERIFY(id.endsWith("@example.org>"));
            QVERIFY(!ids.contains(id));
            ids.insert(id);
        }
        OutgoingMessage m = base();
        m.identity.address.clear();
        QString err;
        // senza indirizzo del mittente la costruzione fallisce
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
    }
    void testDraftKeepsBccSendDoesNot() {
        OutgoingMessage m = base();
        m.bcc = {"Segreto <hidden@example.net>", "anna@example.com"}; // anche in To
        QString err;
        const QByteArray d = MessageBuilder::build(m, &err, BuildMode::Draft);
        QVERIFY2(!d.isEmpty(), qPrintable(err));
        QVERIFY(headerPart(d).contains("\nBcc: ") || headerPart(d).startsWith("Bcc: "));
        QVERIFY(d.contains("hidden@example.net"));
        QVERIFY(headerPart(d).contains("anna@example.com"));
        const QByteArray s = MessageBuilder::build(m, &err, BuildMode::Send);
        QVERIFY(!s.toLower().contains("bcc"));
        QVERIFY(!s.contains("hidden@example.net"));
        // la bozza si rilegge: Bcc ricostruibile, ricostruzione Send lo elimina
        const ParsedMessage p = MessageParser::parse(d);
        QCOMPARE(p.subject, QString("Ciao"));
        QVERIFY(p.to.contains("anna@example.com"));
    }
    void testDraftHasNoSignatureSendAppendsOnce() {
        OutgoingMessage m = base();
        m.identity.signature = "Mario";
        QString err;
        QVERIFY(!MessageBuilder::build(m, &err, BuildMode::Draft).contains("-- "));
        const QString once = MessageParser::parse(MessageBuilder::build(m, &err)).plainText;
        QCOMPARE(once.count("-- \n"), 1);
        // testo gia' firmato (bozza riaperta e firma digitata): nessun duplicato
        m.plainText = once;
        const QString twice = MessageParser::parse(MessageBuilder::build(m, &err)).plainText;
        QCOMPARE(twice.count("-- \n"), 1);
        QCOMPARE(twice.count("Mario"), once.count("Mario"));
        // firma diversa nel corpo: viene aggiunta quella dell'identita'
        m.plainText = "x\n-- \nAltro";
        QCOMPARE(MessageParser::parse(MessageBuilder::build(m, &err)).plainText.count("-- \n"), 2);
    }
    void testHtmlSignatureOnceOnly() {
        OutgoingMessage m = base();
        m.identity.signature = "Mario";
        m.html = "<html><body><p>x</p></body></html>";
        QString err;
        const ParsedMessage p = MessageParser::parse(MessageBuilder::build(m, &err));
        QCOMPARE(p.html.count("Mario"), 1);
        QCOMPARE(p.plainText.count("Mario"), 1);
        const ParsedMessage dr = MessageParser::parse(MessageBuilder::build(m, &err, BuildMode::Draft));
        QVERIFY(!dr.html.contains("Mario"));
    }
    void testLenientDraftKeepsRawRecipients() {
        OutgoingMessage m = base();
        m.to = {"ok@example.com", "mezzo\r\nBcc: evil@x.it"};
        m.cc = {"a@"};
        QString err;
        QVERIFY(MessageBuilder::build(m, &err).isEmpty()); // Send resta rigoroso
        const QByteArray d = MessageBuilder::build(m, &err, BuildMode::Draft);
        QVERIFY2(!d.isEmpty(), qPrintable(err));
        const QByteArray h = headerPart(d);
        QVERIFY(h.contains("To: ok@example.com"));
        QVERIFY(h.contains("X-Boma-Raw-To: mezzo Bcc: evil@x.it"));
        QVERIFY(h.contains("X-Boma-Raw-Cc: a@"));
        for (const QByteArray& line : h.split('\n')) QVERIFY(!line.toLower().startsWith("bcc"));
        // allegato mancante: anche la bozza fallisce
        m.attachmentPaths = {"/nonexistent/q.bin"};
        QVERIFY(MessageBuilder::build(m, &err, BuildMode::Draft).isEmpty());
        QVERIFY(err.contains("q.bin"));
    }
    void testLongLineIsQuotedPrintable() {
        OutgoingMessage m = base();
        m.plainText = QString(998, QLatin1Char('a'));
        QString err;
        const QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        for (const QByteArray& line : raw.split('\n')) QVERIFY(line.size() <= 998);
        QVERIFY(MessageParser::parse(raw).plainText.contains(m.plainText));
    }
    void testFifoAttachmentRefusedWithoutHanging() {
        QTemporaryDir d;
        const QString fifo = d.filePath("f");
        QCOMPARE(mkfifo(fifo.toLocal8Bit().constData(), 0600), 0);
        OutgoingMessage m = base();
        m.attachmentPaths = {fifo};
        QString err;
        QVERIFY(MessageBuilder::build(m, &err).isEmpty());
        QVERIFY(!err.isEmpty());
    }
    void testLargeAttachmentRoundTripsExactly() {
        QTemporaryDir d;
        QByteArray data(200 * 1024, 0);
        for (int i = 0; i < data.size(); ++i) data[i] = char((i * 31 + 7) & 0xff);
        OutgoingMessage m = base();
        m.attachmentPaths = {tmpFile(d, "grande.bin", data)};
        QString err;
        const QByteArray raw = MessageBuilder::build(m, &err);
        QVERIFY2(!raw.isEmpty(), qPrintable(err));
        const ParsedMessage p = MessageParser::parse(raw);
        QCOMPARE(MessageParser::attachmentData(raw, p.attachments[0].partIndex), data);
    }
    void testRoundTripWithParser() {
        QTemporaryDir d;
        OutgoingMessage m = base();
        m.cc = {"c@example.com"};
        m.html = "<p>ciao</p>";
        m.attachmentPaths = {tmpFile(d, "n.txt", "dati")};
        QString err;
        const ParsedMessage p = MessageParser::parse(MessageBuilder::build(m, &err));
        QVERIFY(p.from.contains("mario@example.org") && p.from.contains("Mario Rossi"));
        QVERIFY(p.to.contains("anna@example.com"));
        QVERIFY(p.cc.contains("c@example.com"));
        QCOMPARE(p.subject, QString("Ciao"));
        QVERIFY(p.plainText.contains("Corpo del messaggio"));
        QVERIFY(p.html.contains("ciao"));
        QCOMPARE(p.attachments.size(), 1);
        QCOMPARE(p.attachments[0].filename, QString("n.txt"));
        QVERIFY(p.date.isValid());
    }
};

QTEST_MAIN(T)
#include "test_messagebuilder.moc"
