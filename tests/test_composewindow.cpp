#include <QtTest>
#include <QCloseEvent>
#include <QSignalSpy>
#include "core/Config.h"
#include "mime/MessageParser.h"
#include "ui/ComposeWindow.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
    QTemporaryDir dir_{QDir::currentPath() + "/composeXXXXXX"};

    Config makeConfig(bool mkDrafts = true) {
        const QString a = dir_.path() + "/a", b = dir_.path() + "/b";
        if (mkDrafts)
            for (const QString& r : {a, b})
                for (auto s : {"tmp", "new", "cur"}) QDir().mkpath(r + "/Drafts/" + s);
        const QString ini = dir_.filePath("config.ini");
        QFile f(ini);
        f.open(QIODevice::WriteOnly);
        f.write(("[account:a]\nname=A\nmaildirRoot=" + a + "\n[account:b]\nname=B\nmaildirRoot=" + b + "\n"
                 "[identity:a1]\naccountId=a\naddress=a1@a.it\ndisplayName=Alfa\nsignature=Ciao A\nsignByDefault=true\n"
                 "[identity:b1]\naccountId=b\naddress=b1@b.it\nsignByDefault=false\n").toUtf8());
        f.close();
        return Config::load(ini);
    }
    static int count(const QString& folder) {
        return QDir(folder + "/cur").entryList(QDir::Files).size() + QDir(folder + "/new").entryList(QDir::Files).size();
    }
private slots:
    void init() {
        QDir(dir_.path()).removeRecursively();
        QDir().mkpath(dir_.path());
    }
    void testIdentityChangeReevaluatesSign() {
        ComposeWindow w(makeConfig());
        QCOMPARE(w.identityCombo()->count(), 2);
        QVERIFY(w.identityCombo()->itemText(0).contains("Alfa <a1@a.it>"));
        QVERIFY(w.signButton()->isCheckable() && w.encryptButton()->isCheckable());
        QVERIFY(w.signButton()->isChecked());
        w.identityCombo()->setCurrentIndex(1);
        QVERIFY(!w.signButton()->isChecked());
        w.identityCombo()->setCurrentIndex(0);
        QVERIFY(w.signButton()->isChecked());
    }
    void testSaveDraftWritesOneFileInRightAccount() {
        ComposeWindow w(makeConfig());
        w.identityCombo()->setCurrentIndex(1);
        w.toEdit()->setText("x@y.it, \"Rossi, Anna\" <anna@y.it>");
        w.subjectEdit()->setText("Bozza");
        w.bodyEdit()->setPlainText("testo");
        QVERIFY(w.isDirty());
        QSignalSpy err(&w, &ComposeWindow::draftError);
        QVERIFY(w.saveDraft());
        QCOMPARE(err.count(), 0);
        QCOMPARE(count(dir_.path() + "/b/Drafts"), 1);
        QCOMPARE(count(dir_.path() + "/a/Drafts"), 0);
        const QString d = dir_.path() + "/b/Drafts/cur";
        const QString name = QDir(d).entryList(QDir::Files).first();
        QVERIFY(name.endsWith(":2,D"));
        QFile f(d + "/" + name);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const ParsedMessage p = MessageParser::parse(f.readAll());
        QCOMPARE(p.subject, QString("Bozza"));
        QVERIFY(p.to.contains("Rossi, Anna"));
        QVERIFY(!w.isDirty());
        // secondo salvataggio: sostituisce la bozza, non ne crea una nuova
        QVERIFY(w.saveDraft());
        QCOMPARE(count(dir_.path() + "/b/Drafts"), 1);
    }
    void testRepeatedSaveKeepsOnlyTheLatestDraft() {
        ComposeWindow w(makeConfig());
        w.toEdit()->setText("x@y.it");
        w.subjectEdit()->setText("v1");
        for (int i = 1; i <= 4; ++i) {
            w.subjectEdit()->setText(QString("v%1").arg(i));
            QVERIFY(w.saveDraft());
            QCOMPARE(count(dir_.path() + "/a/Drafts"), 1);
        }
        const QString d = dir_.path() + "/a/Drafts/cur";
        QFile f(d + "/" + QDir(d).entryList(QDir::Files).first());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(MessageParser::parse(f.readAll()).subject, QString("v4"));
        QCOMPARE(QFileInfo(w.draftPath()).absoluteFilePath(), QFileInfo(f).absoluteFilePath());
    }
    // Se il nuovo salvataggio fallisce la bozza precedente resta intatta.
    void testFailedResaveKeepsPreviousDraft() {
        ComposeWindow w(makeConfig());
        w.subjectEdit()->setText("prima");
        QVERIFY(w.saveDraft());
        const QString before = w.draftPath();
        QVERIFY(QFileInfo::exists(before));
        QVERIFY(QDir(dir_.path() + "/a/Drafts/tmp").removeRecursively()); // la consegna ora fallisce
        w.subjectEdit()->setText("dopo");
        QVERIFY(!w.saveDraft());
        QVERIFY(QFileInfo::exists(before));
        QCOMPARE(w.draftPath(), before);
        QCOMPARE(count(dir_.path() + "/a/Drafts"), 1);
    }
    // La bozza rinominata da mbsync (,U=n e flag diversi) viene comunque sostituita.
    void testResaveReplacesDraftRenamedBySync() {
        ComposeWindow w(makeConfig());
        w.subjectEdit()->setText("uno");
        QVERIFY(w.saveDraft());
        const QFileInfo fi(w.draftPath());
        const QString unique = fi.fileName().left(fi.fileName().indexOf(':'));
        QVERIFY(QFile::rename(fi.filePath(), fi.dir().filePath(unique + ",U=7:2,DS")));
        w.subjectEdit()->setText("due");
        QVERIFY(w.saveDraft());
        QCOMPARE(count(dir_.path() + "/a/Drafts"), 1);
    }
    void testDiscardDraftRemovesFileOnce() {
        ComposeWindow w(makeConfig());
        QVERIFY(w.discardDraft()); // nessuna bozza: nulla da fare
        w.subjectEdit()->setText("x");
        QVERIFY(w.saveDraft());
        QCOMPARE(count(dir_.path() + "/a/Drafts"), 1);
        QVERIFY(w.discardDraft());
        QCOMPARE(count(dir_.path() + "/a/Drafts"), 0);
        QVERIFY(w.draftPath().isEmpty());
    }
    void testDraftFailureEmitsErrorAndBlocksClose() {
        ComposeWindow w(makeConfig(false)); // nessuna cartella Drafts
        w.subjectEdit()->setText("x");
        int asked = 0;
        bool answer = false;
        w.setConfirmCallback([&] { ++asked; return answer; });
        QSignalSpy err(&w, &ComposeWindow::draftError);
        QVERIFY(!w.saveDraft());
        QCOMPARE(err.count(), 1);
        QVERIFY(!w.statusLabel()->text().isEmpty());
        QVERIFY(w.isDirty());
        w.show();
        QVERIFY(!w.close());
        QCOMPARE(asked, 1);
        QVERIFY(w.isVisible());
        answer = true;
        QVERIFY(w.close());
        QCOMPARE(asked, 2);
    }
    void testDraftKeepsBccAndHalfTypedAddress() {
        ComposeWindow w(makeConfig());
        w.toEdit()->setText("ann");
        w.bccEdit()->setText("h@y.it");
        QSignalSpy err(&w, &ComposeWindow::draftError);
        QVERIFY(w.saveDraft());
        QCOMPARE(err.count(), 0);
        const QString d = dir_.path() + "/a/Drafts/cur";
        QFile f(d + "/" + QDir(d).entryList(QDir::Files).first());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray raw = f.readAll();
        QVERIFY(raw.contains("Bcc: h@y.it"));
        QVERIFY(raw.contains("X-Boma-Raw-To: ann"));
        QVERIFY(!raw.contains("-- ")); // nessuna firma nella bozza
        // l'invio resta rigoroso
        QSignalSpy sp(&w, &ComposeWindow::sendRequested);
        w.sendButton()->click();
        QCOMPARE(sp.count(), 0);
    }
    void testTogglesAndIdentityMarkDirty() {
        ComposeWindow w(makeConfig());
        QVERIFY(!w.isDirty());
        w.encryptButton()->setChecked(true);
        QVERIFY(w.isDirty());
        w.markClean();
        w.identityCombo()->setCurrentIndex(1);
        QVERIFY(w.isDirty());
        w.markClean();
        OutgoingMessage m;
        m.identity.id = "a1";
        w.setPrefill(m); // cambia identita' ma il prefill resta pulito
        QVERIFY(!w.isDirty());
    }
    void testCleanWindowClosesWithoutAsking() {
        ComposeWindow w(makeConfig());
        bool asked = false;
        w.setConfirmCallback([&] { asked = true; return false; });
        w.show();
        QVERIFY(w.close());
        QVERIFY(!asked);
    }
    void testBuildErrorEmitsDraftError() {
        ComposeWindow w(makeConfig());
        w.addAttachment("/nonexistent/zz.bin");
        QSignalSpy err(&w, &ComposeWindow::draftError);
        QVERIFY(!w.saveDraft());
        QCOMPARE(err.count(), 1);
        QCOMPARE(count(dir_.path() + "/a/Drafts"), 0);
    }
    void testSendEmitsSignalWithFlags() {
        ComposeWindow w(makeConfig());
        w.toEdit()->setText("x@y.it");
        w.bccEdit()->setText("h@y.it");
        w.encryptButton()->setChecked(true);
        QSignalSpy sp(&w, &ComposeWindow::sendRequested);
        w.sendButton()->click();
        QCOMPARE(sp.count(), 1);
        const auto m = qvariant_cast<OutgoingMessage>(sp.at(0).at(0));
        QCOMPARE(m.to, QStringList{"x@y.it"});
        QCOMPARE(m.bcc, QStringList{"h@y.it"});
        QCOMPARE(m.identity.id, QString("a1"));
        QVERIFY(sp.at(0).at(1).toBool());
        QVERIFY(sp.at(0).at(2).toBool());
        // senza destinatari non emette
        w.toEdit()->clear();
        w.bccEdit()->clear();
        w.sendButton()->click();
        QCOMPARE(sp.count(), 1);
    }
    void testPrefillHtmlAndAttachments() {
        ComposeWindow w(makeConfig());
        OutgoingMessage m;
        m.identity.id = "b1";
        m.to = {"x@y.it"};
        m.subject = "Re: z";
        m.plainText = "citazione";
        w.setPrefill(m);
        QCOMPARE(w.identityCombo()->currentIndex(), 1);
        QCOMPARE(w.subjectEdit()->text(), QString("Re: z"));
        QVERIFY(!w.isDirty());
        w.htmlCheck()->setChecked(true);
        QVERIFY(w.richEdit()->toPlainText().contains("citazione"));
        const OutgoingMessage c = w.currentMessage();
        QVERIFY(c.html.contains("citazione"));
        QVERIFY(c.plainText.contains("citazione"));
        w.addAttachment(dir_.filePath("config.ini"));
        QCOMPARE(w.currentMessage().attachmentPaths.size(), 1);
        QCOMPARE(ComposeWindow::splitAddresses("a@b, \"R, A\" <c@d>,  <e@f>"),
                 (QStringList{"a@b", "\"R, A\" <c@d>", "<e@f>"}));
    }
};

QTEST_MAIN(T)
#include "test_composewindow.moc"
