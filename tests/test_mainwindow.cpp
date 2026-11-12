#include "mime/MessageParser.h"
#include "ui/ComposeWindow.h"
#include "core/MailIndex.h"
#include "ui/MainWindow.h"
#include "ui/ReplyBuilder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QListView>
#include <QPointer>
#include <QProcess>
#include <QStatusBar>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QMenu>
#include <QSignalSpy>
#include <QToolButton>
#include <QTest>
#include <sys/stat.h>

using namespace boma;

namespace {

struct GpgRun {
    int code = -1;
    QByteArray out, err;
};

GpgRun runGpg(const QString& home, const QStringList& args, const QByteArray& in = {}) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GNUPGHOME"), home);
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("gpg"), args);
    p.write(in);
    p.closeWriteChannel();
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished();
    }
    return {p.exitCode(), p.readAllStandardOutput(), p.readAllStandardError()};
}

void killAgent(const QString& home) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GNUPGHOME"), home);
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("gpgconf"), {QStringLiteral("--kill"), QStringLiteral("gpg-agent")});
    p.waitForFinished(10000);
}

QString writeScript(const QString& dir, const QString& name, const QByteArray& body) {
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("script non scrivibile");
    f.write(body);
    f.close();
    f.setPermissions(QFile::permissions(path) | QFile::ExeOwner);
    return path;
}

void writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("file non scrivibile: %s", qPrintable(path));
    f.write(data);
}

QByteArray readFile(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray plainMsg(const QString& from, const QString& to, const QString& subject, const QString& id,
                    const QString& body, const QString& extra = {}) {
    return QStringLiteral("From: %1\nTo: %2\n%3Subject: %4\nMessage-ID: <%5>\nDate: Tue, 10 Nov 2026 10:00:00 +0100\n"
                          "MIME-Version: 1.0\nContent-Type: text/plain; charset=utf-8\n\n%6\n")
        .arg(from, to, extra, subject, id, body)
        .toUtf8();
}

QJsonObject jsonMsg(const QString& path, const QString& id, const QString& from, const QString& subject, bool unread) {
    QJsonObject c{{QStringLiteral(":email"), from}, {QStringLiteral(":name"), from}};
    return QJsonObject{{QStringLiteral(":path"), path},
                       {QStringLiteral(":message-id"), id},
                       {QStringLiteral(":from"), QJsonArray{c}},
                       {QStringLiteral(":subject"), subject},
                       {QStringLiteral(":date-unix"), 1762765200},
                       {QStringLiteral(":flags"), unread ? QJsonArray{QStringLiteral("unread")} : QJsonArray{QStringLiteral("seen")}}};
}

void writeJson(const QString& path, const QList<QJsonObject>& l) {
    QJsonArray a;
    for (const auto& o : l) a.append(o);
    writeFile(path, QJsonDocument(a).toJson());
}

struct Noticed {
    QStringList titles, texts;
};

} // namespace

class T : public QObject {
    Q_OBJECT
    QTemporaryDir root_{QDir::currentPath() + QStringLiteral("/mwt-XXXXXX")};
    QString home_, dir_, mu_, mbsync_, msmtp_, gpgslow_, gpglog_;
    int n_ = 0;
    QString pathA_, pathB_, pathSigned_, pathEnc_;

    QString cfgText(bool signByDefault, bool withB = true) const {
        QString c = QStringLiteral("[account:a]\nname=Conto A\nmaildirRoot=%1/mail/a\nmbsyncChannel=a\n"
                                   "[account:b]\nname=Conto B\nmaildirRoot=%1/mail/b\nmbsyncChannel=b\n"
                                   "[identity:a1]\naccountId=a\naddress=alice@example.test\ndisplayName=Alice\n"
                                   "pgpKeyId=alice@example.test\nmsmtpAccount=acca\nsignByDefault=%2\n"
                                   "[identity:a2]\naccountId=a\naddress=sales@example.test\ndisplayName=Sales\n"
                                   "msmtpAccount=acca\nsignByDefault=false\n"
                                   "[identity:b1]\naccountId=b\naddress=bob@example.org\nmsmtpAccount=accb\nsignByDefault=false\n")
                        .arg(dir_, signByDefault ? QStringLiteral("true") : QStringLiteral("false"));
        Q_UNUSED(withB);
        return c;
    }
    Config makeConfig(bool signByDefault = false) {
        const QString p = dir_ + QStringLiteral("/config%1.ini").arg(++n_);
        writeFile(p, cfgText(signByDefault).toUtf8());
        QString err;
        Config c = Config::load(p, &err);
        if (!err.isEmpty()) qFatal("config: %s", qPrintable(err));
        return c;
    }
    MainWindowOptions opts() const {
        MainWindowOptions o;
        o.muProgram = mu_;
        o.muHome = dir_ + QStringLiteral("/muhome");
        o.mbsyncProgram = mbsync_;
        o.msmtpProgram = msmtp_;
        o.gnupgHome = home_;
        o.unsavedDir = dir_ + QStringLiteral("/non-salvati");
        return o;
    }
    MainWindowOptions optsRoot() const { // radice di mu nota: Elimina abilitato
        MainWindowOptions o = opts();
        o.muRoot = dir_ + QStringLiteral("/mail");
        return o;
    }
    QString log(const QString& name) const { return QString::fromUtf8(readFile(dir_ + QLatin1Char('/') + name)); }
    // Solo le invocazioni di `mu move` (le query di find possono citare le cartelle).
    QString moves() const {
        QStringList out;
        for (const QString& l : log(QStringLiteral("mu.log")).split(QLatin1Char('\n')))
            if (l.startsWith(QLatin1String("move "))) out << l;
        return out.join(QLatin1Char('\n'));
    }
    void resetLogs() {
        for (const char* f : {"mu.log", "count", "argv", "stdin", "mbsync.log", "gpg.log", "slow", "slowinfo", "mbok", "mba", "info.txt"}) QFile::remove(dir_ + QLatin1Char('/') + QLatin1String(f));
    }
    void listUnread(const QList<QJsonObject>& l) { writeJson(dir_ + QStringLiteral("/unread.json"), l); }
    void listSearch(const QList<QJsonObject>& l) { writeJson(dir_ + QStringLiteral("/search.json"), l); }

    static void select(MainWindow& w, int row) {
        w.listView()->setCurrentIndex(w.model()->index(row, 0));
    }
    static bool waitRows(MainWindow& w, int n) {
        return QTest::qWaitFor([&] { return w.model()->rowCount() == n; }, 10000);
    }
    static ComposeWindow* firstComposer(MainWindow& w) {
        const auto l = w.composers();
        return l.isEmpty() ? nullptr : l.first();
    }

    QByteArray signedEml() {
        const QByteArray part = "Content-Type: text/plain; charset=utf-8\r\n\r\nCorpo firmato 5511";
        GpgRun r = runGpg(home_, {"--batch", "--armor", "--detach-sign", "-u", "alice@example.test"}, part);
        if (r.code != 0) qFatal("firma: %s", r.err.constData());
        return "From: Alice <alice@example.test>\r\nTo: bob@example.org\r\nSubject: Firmato\r\nMessage-ID: <sig1@x>\r\n"
               "Date: Tue, 10 Nov 2026 10:00:00 +0100\r\nMIME-Version: 1.0\r\n"
               "Content-Type: multipart/signed; protocol=\"application/pgp-signature\"; micalg=pgp-sha512; boundary=\"sig\"\r\n\r\n"
               "--sig\r\n" + part + "\r\n--sig\r\nContent-Type: application/pgp-signature\r\n\r\n" + r.out +
               "\r\n--sig--\r\n";
    }
    QByteArray encryptedEml() {
        const QByteArray inner = "Content-Type: text/plain; charset=utf-8\r\n\r\nSegreto 4242\r\n";
        GpgRun r = runGpg(home_, {"--batch", "--armor", "--trust-model", "always", "--encrypt", "--sign", "-r",
                                  "alice@example.test", "-u", "alice@example.test"}, inner);
        if (r.code != 0) qFatal("cifra: %s", r.err.constData());
        return "From: Alice <alice@example.test>\r\nTo: bob@example.org\r\nSubject: Cifrato\r\nMessage-ID: <enc1@x>\r\n"
               "Date: Tue, 10 Nov 2026 10:00:00 +0100\r\nMIME-Version: 1.0\r\n"
               "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"enc\"\r\n\r\n"
               "--enc\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n\r\n--enc\r\n"
               "Content-Type: application/octet-stream\r\n\r\n" + r.out + "\r\n--enc--\r\n";
    }

private slots:
    void initTestCase() {
        QVERIFY(root_.isValid());
        dir_ = root_.path();
        // GNUPGHOME di prova: mai ~/.gnupg
        home_ = dir_ + QStringLiteral("/gnupg");
        QDir().mkpath(home_);
        chmod(home_.toLocal8Bit().constData(), 0700);
        writeFile(home_ + QStringLiteral("/gpg-agent.conf"), "pinentry-program /bin/false\n");
        GpgRun r = runGpg(home_, {"--batch", "--passphrase", "", "--quick-gen-key", "alice@example.test", "default", "default", "never"});
        QVERIFY2(r.code == 0, r.err.constData());

        mu_ = writeScript(dir_, QStringLiteral("mu"),
                          "#!/bin/sh\nd=$(dirname \"$0\")\nprintf '%s\\n' \"$*\" >> \"$d/mu.log\"\n"
                          "case \"$1\" in\n"
                          " find) case \"$*\" in *flag:unread*) cat \"$d/unread.json\";; *) cat \"$d/search.json\";; esac;;\n"
                          " move) [ -f \"$d/slow\" ] && sleep 1; exit 0;;\n info) [ -f \"$d/slowinfo\" ] && sleep 1; [ -f \"$d/info.txt\" ] && cat \"$d/info.txt\" || exit 1;;\n *) exit 0;;\nesac\n");
        mbsync_ = writeScript(dir_, QStringLiteral("mbsync"),
                              "#!/bin/sh\nd=$(dirname \"$0\")\necho \"$*\" >> \"$d/mbsync.log\"\n"
                              "[ -f \"$d/mbok\" ] && exit 0\n[ -f \"$d/mba\" ] && [ \"$2\" = a ] && exit 0\n"
                              "echo 'mbsync: network unreachable' >&2\nexit 1\n");
        gpglog_ = writeScript(dir_, QStringLiteral("gpglog"), "#!/bin/sh\nd=$(dirname \"$0\")\necho \"$*\" >> \"$d/gpg.log\"\nsleep 1\nexec gpg \"$@\"\n");
        msmtp_ = writeScript(dir_, QStringLiteral("msmtp"),
                             "#!/bin/sh\nd=$(dirname \"$0\")\necho run >> \"$d/count\"\n"
                             "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"$d/argv\"\ncat > \"$d/stdin\"\nexit 0\n");
        gpgslow_ = writeScript(dir_, QStringLiteral("gpgslow"), "#!/bin/sh\nsleep 1\nexec gpg \"$@\"\n");

        const QString a = dir_ + QStringLiteral("/mail/a");
        pathA_ = a + QStringLiteral("/INBOX/new/1.eml");
        pathB_ = a + QStringLiteral("/INBOX/cur/2.eml:2,S");
        pathSigned_ = a + QStringLiteral("/INBOX/cur/3.eml:2,S");
        pathEnc_ = a + QStringLiteral("/INBOX/cur/4.eml:2,S");
        writeFile(pathA_, plainMsg(QStringLiteral("Eve <eve@x.test>"), QStringLiteral("Sales <sales@example.test>, Carol <carol@x.test>"),
                                   QStringLiteral("Oggetto uno"), QStringLiteral("m1@x"), QStringLiteral("Corpo uno 1111"),
                                   QStringLiteral("Cc: alice@example.test, dan@x.test\nBcc: segreto@x.test\n")));
        writeFile(pathB_, plainMsg(QStringLiteral("Frank <frank@x.test>"), QStringLiteral("alice@example.test"),
                                   QStringLiteral("Oggetto due"), QStringLiteral("m2@x"), QStringLiteral("Corpo due 2222")));
        writeFile(pathSigned_, signedEml());
        writeFile(pathEnc_, encryptedEml());
        for (const QString& acc : {a, dir_ + QStringLiteral("/mail/b")})
            for (const char* d : {"Sent", "Drafts", "Trash"})
                for (const char* s : {"cur", "new", "tmp"}) QDir().mkpath(acc + QLatin1Char('/') + QLatin1String(d) + QLatin1Char('/') + QLatin1String(s));
    }
    void cleanupTestCase() { killAgent(home_); }
    void init() {
        resetLogs();
        listUnread({jsonMsg(pathA_, QStringLiteral("m1@x"), QStringLiteral("Eve"), QStringLiteral("Oggetto uno"), true),
                    jsonMsg(pathB_, QStringLiteral("m2@x"), QStringLiteral("Frank"), QStringLiteral("Oggetto due"), true)});
        listSearch({jsonMsg(pathB_, QStringLiteral("m2@x"), QStringLiteral("Frank"), QStringLiteral("Oggetto due"), false)});
    }

    void testSettingsApplyLiveAndSave() {
        MainWindowOptions o = opts();
        o.configPath = dir_ + QStringLiteral("/live.ini");
        MainWindow w(makeConfig(), o);
        w.show();
        QVERIFY(waitRows(w, 2));
        const int rowsBefore = w.folderPane()->model()->rowCount();
        w.setSettingsCallback([](const Config& cur, Config* out) {
            *out = cur;
            QList<Account> a = cur.accounts();
            a.removeLast(); // resta solo "a"
            QList<Identity> ids;
            for (const Identity& i : cur.identities()) if (i.accountId == a.first().id) ids << i;
            out->setAccounts(a);
            out->setIdentities(ids);
            return true;
        });
        w.openSettings();
        const Config saved = Config::load(o.configPath, nullptr);
        QCOMPARE(saved.accounts().size(), 1);
        QVERIFY(QFile::exists(o.configPath));
        QVERIFY(w.folderPane()->model()->rowCount() < rowsBefore); // un account in meno nell'albero
        QVERIFY(w.syncButton()->isEnabled());
    }
    void testSettingsEmptyConfigShowsHelpAndDisablesActions() {
        MainWindowOptions o = opts();
        o.configPath = dir_ + QStringLiteral("/vuota.ini");
        MainWindow w(makeConfig(), o);
        w.show();
        QVERIFY(waitRows(w, 2));
        w.setSettingsCallback([](const Config&, Config* out) { *out = Config(); return true; });
        w.openSettings();
        QVERIFY(w.setupHelp()->isVisibleTo(&w));
        QVERIFY(!w.syncButton()->isEnabled());
        QCOMPARE(w.model()->rowCount(), 0);
    }
    void testSettingsRefusedWhileComposing() {
        MainWindowOptions o = opts();
        o.configPath = dir_ + QStringLiteral("/rifiuto.ini");
        MainWindow w(makeConfig(), o);
        w.show();
        QVERIFY(waitRows(w, 2));
        bool called = false;
        w.setSettingsCallback([&](const Config&, Config*) { called = true; return false; });
        w.newButton()->click();
        QVERIFY(firstComposer(w));
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.openSettings();
        QVERIFY(!called);
        QVERIFY(!QFile::exists(o.configPath));
        QCOMPARE(n.titles.size(), 1);
        firstComposer(w)->close();
    }
    void testFolderPaneListsAllMaildirs() {
        const QString r = dir_ + QStringLiteral("/scan");
        for (const char* d : {"INBOX", "Clienti/Rossi", ".Archivio.2020", "Sent"})
            for (const char* s : {"cur", "new", "tmp"}) QDir().mkpath(r + QLatin1Char('/') + QLatin1String(d) + QLatin1Char('/') + QLatin1String(s));
        QDir().mkpath(r + QStringLiteral("/Vuota")); // non e' una Maildir
        const QStringList f = FolderPane::scanFolders(r);
        QVERIFY(f.contains("Clienti/Rossi"));
        QVERIFY(f.contains(".Archivio.2020"));
        QVERIFY(f.contains("INBOX") && f.contains("Sent"));
        QVERIFY(!f.contains("Vuota"));
        QVERIFY(!f.contains("INBOX/cur"));
        QVERIFY(FolderPane::scanFolders("/nonexistent").isEmpty());
    }
    void testStartsShowingUnread() {
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        QCOMPARE(w.model()->query(), QString());
        QVERIFY(log(QStringLiteral("mu.log")).contains(QStringLiteral("flag:unread")));
        QVERIFY(!w.setupHelp()->isVisibleTo(&w));
        QVERIFY(w.folderPane()->model()->rowCount() >= 2); // account come nodi di primo livello
    }
    void testEmptySearchReturnsToUnread() {
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        w.searchEdit()->setText(QStringLiteral("subject:due"));
        QVERIFY(waitRows(w, 1)); // ricerca (con attesa) e non bloccante
        QCOMPARE(w.model()->query(), QStringLiteral("subject:due"));
        QVERIFY(log(QStringLiteral("mu.log")).contains(QStringLiteral("subject:due")));
        w.searchEdit()->clear();
        QVERIFY(waitRows(w, 2));
        QCOMPARE(w.model()->query(), QString());
    }
    void testSelectingMessageMarksReadAfterOpen() {
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        QVERIFY(!log(QStringLiteral("mu.log")).contains(QStringLiteral("move"))); // niente prima dell'apertura
        bool sawBodyBeforeMark = false;
        connect(&w, &MainWindow::messageOpened, this, [&](const QString&) {
            sawBodyBeforeMark = w.messageView()->toPlainText().contains(QStringLiteral("Corpo uno 1111")) &&
                                !log(QStringLiteral("mu.log")).contains(QStringLiteral("move"));
        });
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return log(QStringLiteral("mu.log")).contains(QStringLiteral("--flags=-N+S")); }, 10000));
        QVERIFY(sawBodyBeforeMark);
        QVERIFY(w.messageView()->toPlainText().contains(QStringLiteral("Corpo uno 1111")));
        QVERIFY(log(QStringLiteral("mu.log")).contains(pathA_));
        // un messaggio gia' letto non viene marcato di nuovo
        resetLogs();
        w.searchEdit()->setText(QStringLiteral("subject:due"));
        QVERIFY(waitRows(w, 1));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        QTest::qWait(300);
        QVERIFY(!log(QStringLiteral("mu.log")).contains(QStringLiteral("move")));
    }
    void testDeleteMovesToTrashAndAsksNoConfirmation() {
        MainWindow w(makeConfig(), optsRoot());
        int asked = 0;
        w.setConfirmUnsignedCallback([&] { ++asked; return true; });
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        // il pulsante e' visivamente distinto dalle azioni sicure
        QVERIFY(w.deleteButton()->styleSheet().contains(QStringLiteral("#")));
        QVERIFY(w.deleteButton()->styleSheet() != w.replyButton()->styleSheet());
        QVERIFY(w.replyButton()->styleSheet().isEmpty());
        w.deleteButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return moves().contains(QStringLiteral("/Trash")); }, 10000));
        QVERIFY(moves().contains(QStringLiteral("%1 /a/Trash").arg(pathB_)));
        QVERIFY(waitRows(w, 1)); // tolto dall'elenco
        QCOMPARE(asked, 0);
        QVERIFY(n.titles.isEmpty()); // nessuna finestra di dialogo
        QVERIFY(QFile::exists(pathB_)); // Boma non cancella mai file
    }
    static QStringList folderQueries(MainWindow& w) {
        QStringList out;
        auto* m = w.folderPane()->itemModel();
        for (int i = 0; i < m->rowCount(); ++i)
            for (int j = 0; j < m->item(i)->rowCount(); ++j) out << m->item(i)->child(j)->data(Qt::UserRole + 1).toString();
        return out;
    }
    void testFolderQueryWithSlashRoot() {
        QCOMPARE(FolderPane::folderQuery(QStringLiteral("/"), QStringLiteral("/m/a"), QStringLiteral("INBOX")),
                 QStringLiteral("maildir:\"/m/a/INBOX\""));
    }
    void testUnreadQueryExcludesSpecialFolders() {
        MailIndex idx;
        Account a; a.id = "a"; a.maildirRoot = "/m/a"; a.trashFolder = "Trash"; a.draftsFolder = "Drafts"; a.sentFolder = "Sent";
        Account b; b.id = "b"; b.maildirRoot = "/altrove/b"; b.trashFolder = "Cestino"; // fuori dalla radice: nessuna esclusione
        idx.setAccounts({a, b});
        QCOMPARE(idx.unreadQuery(), QStringLiteral("flag:unread AND NOT flag:trashed")); // radice ignota
        idx.setMuRoot("/m");
        QCOMPARE(idx.unreadQuery(), QStringLiteral("flag:unread AND NOT flag:trashed AND NOT maildir:\"/a/Trash\" "
                                                   "AND NOT maildir:\"/a/Drafts\" AND NOT maildir:\"/a/Sent\""));
        // la finestra principale passa la radice all'indice: la vista "Non lette" esclude le cartelle speciali
        MainWindow w(makeConfig(), optsRoot());
        QVERIFY(QTest::qWaitFor([&] { return log(QStringLiteral("mu.log")).contains(QStringLiteral("flag:unread")); }, 10000));
        QVERIFY(log(QStringLiteral("mu.log")).contains(QStringLiteral("AND NOT maildir:\"/a/Trash\" AND NOT maildir:\"/a/Drafts\"")));
    }
    void testParseStoreRoot() {
        const QByteArray t = "+---+---+\n| property | value |\n+---+---+\n| maildir           | /x/y/mail/   |\n"
                             "| database-path | /x/h |\n";
        QCOMPARE(MailIndex::parseStoreRoot(t), QStringLiteral("/x/y/mail"));
        QCOMPARE(MailIndex::parseStoreRoot("niente"), QString());
    }
    void testMuRootFromInfoStoreGivesFolderQueriesAndTrash() {
        writeFile(dir_ + QStringLiteral("/info.txt"), QStringLiteral("| maildir | %1/mail |\n").arg(dir_).toUtf8());
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(QTest::qWaitFor([&] { return folderQueries(w).contains(QStringLiteral("maildir:\"/a/INBOX\"")); }, 10000));
        QVERIFY(folderQueries(w).contains(QStringLiteral("maildir:\"/b/INBOX\"")));
        QVERIFY(!w.statusBar()->currentMessage().contains(QStringLiteral("radice")));
        QFile::remove(dir_ + QStringLiteral("/info.txt"));
    }
    void testMuRootFromConfigOverridesInfoStore() {
        writeFile(dir_ + QStringLiteral("/info.txt"), QStringLiteral("| maildir | /altrove |\n").toUtf8());
        const QString p = dir_ + QStringLiteral("/configroot.ini");
        writeFile(p, ("[general]\nmuRoot=" + dir_ + "/mail\n" + cfgText(false)).toUtf8());
        QString err;
        Config c = Config::load(p, &err);
        QCOMPARE(c.muRoot(), dir_ + QStringLiteral("/mail"));
        MainWindow w(c, opts());
        w.show();
        QVERIFY(QTest::qWaitFor([&] { return folderQueries(w).contains(QStringLiteral("maildir:\"/a/INBOX\"")); }, 10000));
        QFile::remove(dir_ + QStringLiteral("/info.txt"));
    }
    void testMuRootFallsBackToCommonParentWithWarning() {
        MainWindow w(makeConfig(), opts()); // info.txt assente: mu info store fallisce
        w.show();
        QVERIFY(QTest::qWaitFor([&] { return folderQueries(w).contains(QStringLiteral("maildir:\"/a/INBOX\"")); }, 10000));
        QVERIFY(w.statusBar()->currentMessage().contains(QStringLiteral("radice")));
    }
    void testAccountOutsideMuRootIsRefused() {
        MainWindowOptions o = opts();
        o.muRoot = dir_ + QStringLiteral("/mail/a"); // l'account b sta fuori
        MainWindow w(makeConfig(), o);
        w.show();
        QVERIFY(waitRows(w, 2));
        QVERIFY(!folderQueries(w).filter(QStringLiteral("/b/")).size());
        QVERIFY(folderQueries(w).contains(QStringLiteral("maildir:\"/INBOX\"")));
    }
    static int countLines(const QString& text, const QString& needle) {
        int n = 0;
        for (const QString& l : text.split(QLatin1Char('\n')))
            if (l.contains(needle)) ++n;
        return n;
    }
    void testDeleteDoesNotOpenAdjacentRow() {
        MainWindow w(makeConfig(), optsRoot());
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return log(QStringLiteral("mu.log")).contains(QStringLiteral("--flags=-N+S")); }, 10000));
        resetLogs();
        w.deleteButton()->click();
        QVERIFY(waitRows(w, 1));
        QTest::qWait(600);
        QCOMPARE(countLines(log(QStringLiteral("mu.log")), QStringLiteral("--flags=-N+S")), 0); // il vicino resta non letto
        QVERIFY(w.model()->summary(0).unread);
        QVERIFY(!w.deleteButton()->isEnabled());
        QVERIFY(!w.listView()->currentIndex().isValid());
        QVERIFY(!w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")));
    }
    void testDeleteInFlightGuard() {
        writeFile(dir_ + QStringLiteral("/slow"), "x");
        MainWindow w(makeConfig(), optsRoot());
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        w.deleteButton()->click();
        QVERIFY(!w.deleteButton()->isEnabled());
        w.deleteSelected(); // anche dalla scorciatoia: nessun secondo spostamento
        QVERIFY(waitRows(w, 1));
        QCOMPARE(countLines(moves(), QStringLiteral("/a/Trash")), 1);
    }
    void testDecryptInFlightGuard() {
        listUnread({jsonMsg(pathEnc_, QStringLiteral("enc1@x"), QStringLiteral("Alice"), QStringLiteral("Cifrato"), false)});
        MainWindowOptions o = opts();
        o.gpgProgram = gpglog_;
        MainWindow w(makeConfig(), o);
        w.show();
        QVERIFY(waitRows(w, 1));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->decryptButton()->isVisibleTo(w.messageView()); }, 10000));
        w.messageView()->setConfirmCallback([] { return true; });
        w.messageView()->decryptButton()->click();
        w.messageView()->decryptButton()->click();
        emit w.messageView()->decryptRequested();
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Segreto 4242")); }, 30000));
        QCOMPARE(countLines(log(QStringLiteral("gpg.log")), QStringLiteral("decrypt")), 1);
    }
    void testSelectionSurvivesSyncRefreshAndClearsWhenGone() {
        writeFile(dir_ + QStringLiteral("/mbok"), "x");
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        resetLogs();
        writeFile(dir_ + QStringLiteral("/mbok"), "x");
        // la riga selezionata cambia posizione: dopo l'aggiornamento resta quella
        listUnread({jsonMsg(pathA_, QStringLiteral("m1@x"), QStringLiteral("Eve"), QStringLiteral("Oggetto uno"), true),
                    jsonMsg(pathSigned_, QStringLiteral("s@x"), QStringLiteral("Z"), QStringLiteral("Altro"), true),
                    jsonMsg(pathB_, QStringLiteral("m2@x"), QStringLiteral("Frank"), QStringLiteral("Oggetto due"), true)});
        w.syncNow();
        QVERIFY(waitRows(w, 3));
        QVERIFY(QTest::qWaitFor([&] { return w.listView()->currentIndex().isValid(); }, 5000));
        QCOMPARE(w.listView()->currentIndex().row(), 2);
        QVERIFY(w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222"))); // senza richiuderlo
        QVERIFY(w.deleteButton()->isEnabled());
        // il messaggio sparisce dall'elenco: il visore si svuota
        listUnread({jsonMsg(pathA_, QStringLiteral("m1@x"), QStringLiteral("Eve"), QStringLiteral("Oggetto uno"), true)});
        w.syncNow();
        QVERIFY(waitRows(w, 1));
        QVERIFY(QTest::qWaitFor([&] { return !w.deleteButton()->isEnabled(); }, 5000));
        QVERIFY(!w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")));
    }
    void testFailedSyncOfOneAccountStillReindexes() {
        writeFile(dir_ + QStringLiteral("/mba"), "x"); // a riesce, b fallisce
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        resetLogs();
        writeFile(dir_ + QStringLiteral("/mba"), "x");
        w.syncNow();
        QVERIFY(QTest::qWaitFor([&] { return log(QStringLiteral("mu.log")).contains(QStringLiteral("index")); }, 10000));
        QVERIFY(w.statusLabel()->text().contains(QStringLiteral("non riuscito")));
    }
    void testFullyFailedSyncDoesNotReindex() {
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        resetLogs();
        w.syncNow();
        QVERIFY(QTest::qWaitFor([&] { return w.statusLabel()->text().contains(QStringLiteral("non riuscito")); }, 10000));
        QTest::qWait(300);
        QVERIFY(!log(QStringLiteral("mu.log")).contains(QStringLiteral("index")));
    }
    void testFastScrollingOpensOnlyTheLastRow() {
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        int opened = 0;
        connect(&w, &MainWindow::messageOpened, this, [&](const QString&) { ++opened; });
        select(w, 0);
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        QTest::qWait(400);
        QCOMPARE(opened, 1);
        QVERIFY(!w.messageView()->toPlainText().contains(QStringLiteral("Corpo uno 1111")));
    }
    void testFolderSelectionRunsFolderQuery() {
        writeFile(dir_ + QStringLiteral("/info.txt"), QStringLiteral("| maildir | %1/mail |\n").arg(dir_).toUtf8());
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(QTest::qWaitFor([&] { return folderQueries(w).contains(QStringLiteral("maildir:\"/a/INBOX\"")); }, 10000));
        auto* m = w.folderPane()->itemModel();
        QStandardItem* inbox = m->item(1)->child(0); // primo account, Posta in arrivo
        QCOMPARE(inbox->data(Qt::UserRole + 1).toString(), QStringLiteral("maildir:\"/a/INBOX\""));
        w.folderPane()->setCurrentIndex(inbox->index());
        QVERIFY(QTest::qWaitFor([&] { return w.model()->query() == QStringLiteral("maildir:\"/a/INBOX\""); }, 10000));
        QVERIFY(QTest::qWaitFor([&] { return log(QStringLiteral("mu.log")).contains(QStringLiteral("maildir:\"/a/INBOX\"")); }, 10000));
        // il nodo account non lancia nulla; "Non lette" torna alla vista dei non letti
        w.folderPane()->setCurrentIndex(m->item(0)->index());
        QVERIFY(QTest::qWaitFor([&] { return w.model()->query().isEmpty(); }, 10000));
    }
    void testShortcuts() {
        MainWindow w(makeConfig(), optsRoot());
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QVERIFY(waitRows(w, 2));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.replyButton()->isEnabled(); }, 10000));
        auto composers = [&] { return w.composers().size(); };
        // le finestre di scrittura rubano il fuoco: la finestra principale deve essere attiva
        auto key = [&](Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) {
            w.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&w));
            w.listView()->setFocus();
            QTest::keyClick(w.listView(), k, m);
        };
        key(Qt::Key_R);
        QTRY_COMPARE(composers(), 1);
        QCOMPARE(firstComposer(w)->subjectEdit()->text(), QStringLiteral("Re: Oggetto due"));
        qDeleteAll(w.composers());
        key(Qt::Key_R, Qt::ShiftModifier);
        QTRY_COMPARE(composers(), 1);
        qDeleteAll(w.composers());
        key(Qt::Key_F);
        QTRY_COMPARE(composers(), 1);
        QCOMPARE(firstComposer(w)->subjectEdit()->text(), QStringLiteral("Fwd: Oggetto due"));
        qDeleteAll(w.composers());
        key(Qt::Key_N, Qt::ControlModifier);
        QTRY_COMPARE(composers(), 1);
        qDeleteAll(w.composers());
        key(Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(w.searchEdit()->hasFocus());
        key(Qt::Key_F5);
        QTRY_VERIFY(log(QStringLiteral("mbsync.log")).contains(QStringLiteral("-V")));
        key(Qt::Key_Delete);
        QTRY_VERIFY(moves().contains(QStringLiteral("/a/Trash")));
    }
    void testReplyBuilderSanitisesAndCaps() {
        const Config cfg = makeConfig();
        ParsedMessage m;
        m.subject = QStringLiteral("Ciao\r\nBcc: spia@x.test");
        m.from = QStringLiteral("Eve\r\nX: y <eve@x.test>");
        m.to = QStringLiteral("alice@example.test");
        m.plainText = QString(400 * 1024, QLatin1Char('x'));
        const OutgoingMessage r = buildReply(m, QStringLiteral("a"), cfg, ReplyMode::Reply);
        QVERIFY(!r.subject.contains(QLatin1Char('\n')) && !r.subject.contains(QLatin1Char('\r')));
        QCOMPARE(r.subject, QStringLiteral("Re: Ciao Bcc: spia@x.test"));
        for (const QString& a : r.to) QVERIFY(!a.contains(QLatin1Char('\n')));
        QVERIFY(r.plainText.contains(QStringLiteral("[citazione troncata]")));
        QVERIFY(r.plainText.size() < 300 * 1024);
        const OutgoingMessage f = buildReply(m, QStringLiteral("a"), cfg, ReplyMode::Forward);
        QVERIFY(!f.subject.contains(QLatin1Char('\n')));
        QVERIFY(f.plainText.contains(QStringLiteral("[citazione troncata]")));
        // allegati: nota visibile nell'inoltro
        ParsedMessage a;
        a.subject = QStringLiteral("x");
        a.plainText = QStringLiteral("corpo");
        a.attachments << Attachment() << Attachment();
        QVERIFY(buildReply(a, QStringLiteral("a"), cfg, ReplyMode::Forward).plainText.contains(QStringLiteral("allegati originali non inclusi: 2")));
        QVERIFY(!buildReply(a, QStringLiteral("a"), cfg, ReplyMode::Reply).plainText.contains(QStringLiteral("allegati originali")));
    }
    void testDeleteRefusedWhileMuRootIsBeingDetected() {
        writeFile(dir_ + QStringLiteral("/info.txt"), QStringLiteral("| maildir | %1/mail |\n").arg(dir_).toUtf8());
        writeFile(dir_ + QStringLiteral("/slowinfo"), "x");
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        QVERIFY(w.folderPane()->itemModel()->item(1)->rowCount() == 0); // radice ancora ignota
        w.deleteButton()->click();
        QVERIFY(w.statusBar()->currentMessage().contains(QStringLiteral("Cancella disponibile dopo il rilevamento della radice mu")));
        QTest::qWait(200);
        QVERIFY(!moves().contains(QStringLiteral("Trash")));
        QCOMPARE(w.model()->rowCount(), 2);
        QVERIFY(QTest::qWaitFor([&] { return !folderQueries(w).isEmpty(); }, 10000));
        w.deleteButton()->click();
        QVERIFY(waitRows(w, 1));
        QVERIFY(moves().contains(QStringLiteral("/a/Trash")));
    }
    void testDeleteRefusedInFallbackMode() {
        MainWindow w(makeConfig(), opts()); // mu info store fallisce
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        QVERIFY(QTest::qWaitFor([&] { return !folderQueries(w).isEmpty(); }, 10000)); // cartelle ancora disponibili
        QVERIFY(w.statusBar()->currentMessage().contains(QStringLiteral("radice")));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        w.deleteButton()->click();
        QVERIFY(!n.texts.isEmpty());
        QVERIFY(n.texts.first().contains(QStringLiteral("muRoot=")));
        QTest::qWait(200);
        QVERIFY(!moves().contains(QStringLiteral("Trash")));
        QCOMPARE(w.model()->rowCount(), 2);
    }
    void testDeleteKeepsUnrelatedSelectionMadeDuringMove() {
        writeFile(dir_ + QStringLiteral("/slow"), "x");
        MainWindow w(makeConfig(), optsRoot());
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 1); // pathB_
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        w.deleteButton()->click();
        select(w, 0); // l'utente sceglie un'altra riga durante lo spostamento
        QVERIFY(waitRows(w, 1));
        QVERIFY(w.listView()->currentIndex().isValid());
        QCOMPARE(w.model()->summary(w.listView()->currentIndex().row()).path, pathA_);
    }
    void testDeleteRefusedWhenAccountOutsideMuRoot() {
        MainWindowOptions o = opts();
        o.muRoot = dir_ + QStringLiteral("/mail/b"); // il messaggio e' dell'account a
        MainWindow w(makeConfig(), o);
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        w.deleteButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return !n.texts.isEmpty(); }, 10000));
        QVERIFY(n.texts.first().contains(QStringLiteral("radice di mu")));
        QVERIFY(!moves().contains(QStringLiteral("Trash")));
        QVERIFY(w.deleteButton()->isEnabled()); // di nuovo utilizzabile
        QCOMPARE(w.model()->rowCount(), 2);
    }
    void testStartDoesNotRunMbsyncAndListIsShown() {
        QElapsedTimer t;
        t.start();
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY2(t.elapsed() < 3000, "avvio bloccante");
        QVERIFY(waitRows(w, 2)); // l'elenco c'e' subito, senza sincronizzare
        QTest::qWait(300);
        QVERIFY(!QFile::exists(dir_ + QStringLiteral("/mbsync.log")) || log(QStringLiteral("mbsync.log")).isEmpty());
        QVERIFY(!log(QStringLiteral("mu.log")).contains(QStringLiteral("index")));
    }
    void testStartsWithEmptyConfigShowsSetupHelp() {
        const QString p = dir_ + QStringLiteral("/vuota.ini");
        writeFile(p, "");
        MainWindow w(Config::load(p), opts());
        w.show();
        QVERIFY(w.setupHelp()->isVisibleTo(&w));
        QVERIFY(w.setupHelp()->toPlainText().contains(QStringLiteral("config.ini")));
        QVERIFY(!w.messageView()->isVisibleTo(&w));
        QTest::qWait(200);
        QVERIFY(!QFile::exists(dir_ + QStringLiteral("/mu.log")) || !log(QStringLiteral("mu.log")).contains(QStringLiteral("find")));
    }
    void testSignedMessageShowsBadgeWithRealGpg() {
        listUnread({jsonMsg(pathSigned_, QStringLiteral("sig1@x"), QStringLiteral("Alice"), QStringLiteral("Firmato"), false)});
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 1));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->signatureBadge()->isVisibleTo(w.messageView()); }, 20000));
        QVERIFY2(w.messageView()->signatureBadge()->text().contains(QStringLiteral("Firma valida")),
                 qPrintable(w.messageView()->signatureBadge()->text()));
        QVERIFY(w.messageView()->toPlainText().contains(QStringLiteral("Corpo firmato 5511")));
    }
    void testEncryptedRequiresConfirmationThenDecrypts() {
        listUnread({jsonMsg(pathEnc_, QStringLiteral("enc1@x"), QStringLiteral("Alice"), QStringLiteral("Cifrato"), false)});
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 1));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->decryptButton()->isVisibleTo(w.messageView()); }, 10000));
        QVERIFY(!w.messageView()->toPlainText().contains(QStringLiteral("Segreto 4242")));
        bool yes = false;
        w.messageView()->setConfirmCallback([&] { return yes; });
        w.messageView()->decryptButton()->click(); // rifiutato
        QTest::qWait(300);
        QVERIFY(!w.messageView()->toPlainText().contains(QStringLiteral("Segreto 4242")));
        yes = true;
        w.messageView()->decryptButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Segreto 4242")); }, 30000));
        // firma interna: il badge compare anche per firmato+cifrato (in qualunque ordine di arrivo)
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->signatureBadge()->isVisibleTo(w.messageView()); }, 10000));
        QVERIFY(w.messageView()->signatureBadge()->text().contains(QStringLiteral("Firma valida")));
    }
    // I1: un vecchio messaggio firmato annidato sotto un testo dell'attaccante (o
    // dentro un cifrato) non deve mai dare il badge verde sul testo non firmato.
    QByteArray nestedSignedEntity() {
        const QByteArray part = "Content-Type: text/plain; charset=utf-8\r\n\r\nVecchio testo firmato 7001";
        GpgRun r = runGpg(home_, {"--batch", "--armor", "--detach-sign", "-u", "alice@example.test"}, part);
        if (r.code != 0) qFatal("firma: %s", r.err.constData());
        return "Content-Type: multipart/mixed; boundary=\"mix\"\r\n\r\n"
               "--mix\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nTESTO DELL'ATTACCANTE 6660\r\n"
               "--mix\r\nContent-Type: multipart/signed; protocol=\"application/pgp-signature\"; micalg=pgp-sha512; boundary=\"sig\"\r\n\r\n"
               "--sig\r\n" + part + "\r\n--sig\r\nContent-Type: application/pgp-signature\r\n\r\n" + r.out +
               "\r\n--sig--\r\n\r\n--mix--\r\n";
    }
    void testNestedSignedPartNeverGivesGreenBadge() {
        const QString p = dir_ + QStringLiteral("/mail/a/INBOX/cur/nested.eml:2,S");
        writeFile(p, "From: Alice <alice@example.test>\r\nTo: bob@example.org\r\nSubject: Replay\r\nMessage-ID: <nest@x>\r\n"
                     "Date: Tue, 10 Nov 2026 10:00:00 +0100\r\nMIME-Version: 1.0\r\n" + nestedSignedEntity());
        listUnread({jsonMsg(p, QStringLiteral("nest@x"), QStringLiteral("Alice"), QStringLiteral("Replay"), false)});
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 1));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("6660")); }, 10000));
        QTest::qWait(1500); // tempo per un'eventuale verifica
        QLabel* b = w.messageView()->signatureBadge();
        QVERIFY2(!b->text().contains(QStringLiteral("Firma valida")), qPrintable(b->text()));
        QVERIFY(!b->styleSheet().contains(QStringLiteral("#1a7f37")));
        QVERIFY2(b->isVisibleTo(w.messageView()) && b->text().contains(QStringLiteral("parte")), qPrintable(b->text()));
    }
    void testEncryptedWithNestedSignedPartNeverGivesGreenBadge() {
        GpgRun r = runGpg(home_, {"--batch", "--armor", "--trust-model", "always", "--encrypt", "-r", "alice@example.test"},
                          nestedSignedEntity());
        QVERIFY2(r.code == 0, r.err.constData());
        const QString p = dir_ + QStringLiteral("/mail/a/INBOX/cur/nestenc.eml:2,S");
        writeFile(p, "From: Alice <alice@example.test>\r\nTo: bob@example.org\r\nSubject: Cifrato\r\nMessage-ID: <nestenc@x>\r\n"
                     "Date: Tue, 10 Nov 2026 10:00:00 +0100\r\nMIME-Version: 1.0\r\n"
                     "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"enc\"\r\n\r\n"
                     "--enc\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n\r\n--enc\r\n"
                     "Content-Type: application/octet-stream\r\n\r\n" + r.out + "\r\n--enc--\r\n");
        listUnread({jsonMsg(p, QStringLiteral("nestenc@x"), QStringLiteral("Alice"), QStringLiteral("Cifrato"), false)});
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 1));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->decryptButton()->isVisibleTo(w.messageView()); }, 10000));
        w.messageView()->setConfirmCallback([] { return true; });
        w.messageView()->decryptButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("6660")); }, 30000));
        QTest::qWait(1500);
        QLabel* b = w.messageView()->signatureBadge();
        QVERIFY2(!b->text().contains(QStringLiteral("Firma valida")), qPrintable(b->text()));
        QVERIFY(!b->styleSheet().contains(QStringLiteral("#1a7f37")));
    }
    void testStaleVerifyResultIsDropped() {
        listUnread({jsonMsg(pathSigned_, QStringLiteral("sig1@x"), QStringLiteral("Alice"), QStringLiteral("Firmato"), false),
                    jsonMsg(pathB_, QStringLiteral("m2@x"), QStringLiteral("Frank"), QStringLiteral("Oggetto due"), false)});
        MainWindowOptions o = opts();
        o.gpgProgram = gpgslow_; // la verifica impiega ~1 s
        MainWindow w(makeConfig(), o);
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo firmato 5511")); }, 10000));
        select(w, 1);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")); }, 10000));
        QTest::qWait(2500); // arriva il risultato (superato) della prima verifica
        QVERIFY(!w.messageView()->signatureBadge()->isVisibleTo(w.messageView()));
        QVERIFY(w.messageView()->toPlainText().contains(QStringLiteral("Corpo due 2222")));
    }
    void testReplyBuilder() {
        const Config cfg = makeConfig();
        ParsedMessage m = MessageParser::parse(readFile(pathA_));
        QVERIFY(m.messageId == QStringLiteral("m1@x"));
        // Re: non si accumula
        m.subject = QStringLiteral("RE: Re: Oggetto uno");
        OutgoingMessage r = buildReply(m, QStringLiteral("a"), cfg, ReplyMode::Reply);
        QCOMPARE(r.subject, QStringLiteral("Re: Oggetto uno"));
        m.subject = QStringLiteral("Oggetto uno");
        r = buildReply(m, QStringLiteral("a"), cfg, ReplyMode::Reply);
        QCOMPARE(r.subject, QStringLiteral("Re: Oggetto uno"));
        QVERIFY(r.plainText.contains(QStringLiteral("> Corpo uno 1111")));
        QCOMPARE(r.to.size(), 1);
        QVERIFY(r.to.first().contains(QStringLiteral("eve@x.test")));
        QVERIFY(r.cc.isEmpty());
        QVERIFY(r.bcc.isEmpty());
        QCOMPARE(r.inReplyToMessageId, QStringLiteral("m1@x"));
        QVERIFY(r.referencesIds.contains(QStringLiteral("m1@x")));
        // identita': quella a cui era indirizzato il messaggio (sales), non la prima
        QCOMPARE(r.identity.id, QStringLiteral("a2"));
        // Rispondi a tutti: niente identita' proprie, niente duplicati, Bcc mai
        const QString all = buildReply(m, QStringLiteral("a"), cfg, ReplyMode::ReplyAll).to.join(QLatin1Char(',')) + QLatin1Char('|') +
                            buildReply(m, QStringLiteral("a"), cfg, ReplyMode::ReplyAll).cc.join(QLatin1Char(','));
        QVERIFY(all.contains(QStringLiteral("eve@x.test")));
        QVERIFY(all.contains(QStringLiteral("carol@x.test")));
        QVERIFY(all.contains(QStringLiteral("dan@x.test")));
        QVERIFY2(!all.contains(QStringLiteral("sales@example.test")), qPrintable(all));
        QVERIFY(!all.contains(QStringLiteral("alice@example.test")));
        QVERIFY(!all.contains(QStringLiteral("segreto@x.test")));
        // Inoltra
        m.subject = QStringLiteral("Fwd: Oggetto uno");
        OutgoingMessage f = buildReply(m, QStringLiteral("a"), cfg, ReplyMode::Forward);
        QCOMPARE(f.subject, QStringLiteral("Fwd: Oggetto uno"));
        QVERIFY(f.to.isEmpty());
        QVERIFY(f.inReplyToMessageId.isEmpty());
        QVERIFY(f.plainText.contains(QStringLiteral("Corpo uno 1111")));
        // nessuna corrispondenza: prima identita' dell'account del messaggio
        ParsedMessage o = m;
        o.to = QStringLiteral("altro@x.test");
        o.cc.clear();
        QCOMPARE(buildReply(o, QStringLiteral("b"), cfg, ReplyMode::Reply).identity.id, QStringLiteral("b1"));
        QCOMPARE(buildReply(o, QStringLiteral("a"), cfg, ReplyMode::Reply).identity.id, QStringLiteral("a1"));
        // messaggio scritto da me: la risposta va ai destinatari originali
        ParsedMessage mine = m;
        mine.from = QStringLiteral("Alice <alice@example.test>");
        mine.to = QStringLiteral("zed@x.test");
        mine.cc.clear();
        r = buildReply(mine, QStringLiteral("a"), cfg, ReplyMode::Reply);
        QVERIFY(r.to.join(QLatin1Char(',')).contains(QStringLiteral("zed@x.test")));
    }
    void testReplyButtonsOpenComposerWithPrefill() {
        MainWindow w(makeConfig(), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.replyButton()->isEnabled(); }, 10000));
        w.replyButton()->click();
        ComposeWindow* c = firstComposer(w);
        QVERIFY(c);
        QCOMPARE(c->subjectEdit()->text(), QStringLiteral("Re: Oggetto uno"));
        QVERIFY(c->toEdit()->text().contains(QStringLiteral("eve@x.test")));
        QVERIFY(c->bodyEdit()->toPlainText().contains(QStringLiteral("> Corpo uno 1111")));
        QCOMPARE(c->currentMessage().inReplyToMessageId, QStringLiteral("m1@x"));
        QVERIFY(c->currentMessage().bcc.isEmpty());
        w.newButton()->click();
        QCOMPARE(w.composers().size(), 2);
        w.replyAllButton()->click();
        w.forwardButton()->click();
        QCOMPARE(w.composers().size(), 4);
    }
    void testSendSentClosesComposeAndShowsStatus() {
        MainWindow w(makeConfig(false), opts());
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.replyButton()->isEnabled(); }, 10000));
        w.replyButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        c->show();
        QVERIFY(!c->signButton()->isChecked());
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return c.isNull(); }, 20000));
        QVERIFY(w.statusLabel()->text().contains(QStringLiteral("Inviato")) || w.statusBar()->currentMessage().contains(QStringLiteral("Inviato")));
        QCOMPARE(log(QStringLiteral("count")).count(QStringLiteral("run")), 1);
        QVERIFY(log(QStringLiteral("argv")).contains(QStringLiteral("--")));
        QVERIFY(!log(QStringLiteral("argv")).contains(QStringLiteral("segreto@x.test"))); // Bcc del messaggio originale mai
    }
    // I4: Inviata non valida segnalata all'avvio (account gia' sincronizzato).
    void testInvalidSpecialFolderWarnsAtStartup() {
        const QString p = dir_ + QStringLiteral("/config-bad%1.ini").arg(++n_);
        writeFile(p, cfgText(false).replace(QStringLiteral("mbsyncChannel=a\n"),
                                            QStringLiteral("mbsyncChannel=a\nsentFolder=Sent Messages\n")).toUtf8());
        QString err;
        const Config cfg = Config::load(p, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        Noticed n;
        MainWindow w(cfg, opts());
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(QTest::qWaitFor([&] { return !n.titles.isEmpty(); }, 10000));
        QCOMPARE(n.titles.first(), QStringLiteral("Cartelle non valide"));
        QVERIFY2(n.texts.first().contains(QStringLiteral("Sent Messages")), qPrintable(n.texts.first()));
        QVERIFY(!n.texts.first().contains(QStringLiteral("Drafts\"")));
        // configurazione corretta: nessun avviso
        Noticed m;
        MainWindow ok(makeConfig(), opts());
        ok.setNoticeCallback([&](const QString& t, const QString& x) { m.titles << t; m.texts << x; });
        ok.show();
        QVERIFY(waitRows(ok, 2));
        QTest::qWait(500);
        QVERIFY2(m.titles.isEmpty(), qPrintable(m.texts.join('|')));
    }
    // I4: inviato ma copia in Inviata fallita: la finestra resta aperta, segnata come
    // inviata (non reinviabile), con il percorso della copia di emergenza.
    void testSentButCopyFailedKeepsComposerMarkedSent() {
        const QString sent = dir_ + QStringLiteral("/mail/a/Sent");
        MainWindowOptions o = opts();
        o.msmtpProgram = writeScript(dir_, QStringLiteral("msmtpbreak"),
                                     ("#!/bin/sh\nd=$(dirname \"$0\")\necho run >> \"$d/count\"\ncat > \"$d/stdin\"\nrm -rf '" +
                                      sent.toUtf8() + "'\nexit 0\n"));
        MainWindow w(makeConfig(false), o);
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        w.newButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        c->toEdit()->setText(QStringLiteral("bob@x.test"));
        c->bodyEdit()->setPlainText(QStringLiteral("Testo importante 8080"));
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return !n.titles.isEmpty(); }, 20000));
        QTest::qWait(300);
        for (const char* d : {"cur", "new", "tmp"}) QDir().mkpath(sent + QLatin1Char('/') + QLatin1String(d)); // per gli altri test
        QVERIFY(c); // non chiusa
        QVERIFY(!c->sendButton()->isEnabled());
        QVERIFY2(c->statusLabel()->text().contains(QStringLiteral("INVIATO")), qPrintable(c->statusLabel()->text()));
        QVERIFY(c->bodyEdit()->toPlainText().contains(QStringLiteral("8080")));
        const QStringList saved = QDir(dir_ + QStringLiteral("/non-salvati")).entryList(QDir::Files);
        QCOMPARE(saved.size(), 1);
        QVERIFY(c->statusLabel()->text().contains(saved.first()));
        QVERIFY(n.texts.first().contains(saved.first()));
        QVERIFY(readFile(dir_ + QStringLiteral("/non-salvati/") + saved.first()).contains("Testo importante 8080"));
        // nessun secondo invio, nemmeno forzando il segnale
        emit c->sendRequested(c->currentMessage(), false, false);
        c->sendButton()->setEnabled(true);
        c->sendButton()->click();
        QTest::qWait(500);
        QCOMPARE(log(QStringLiteral("count")).count(QStringLiteral("run")), 1);
        // chiudere non chiede conferma (gia' inviato)
        c->setConfirmCallback([] { return false; });
        c->close();
        QVERIFY(QTest::qWaitFor([&] { return c.isNull(); }, 2000));
        QDir(dir_ + QStringLiteral("/non-salvati")).removeRecursively();
    }
    // Doppio guasto: inviato, copia in Inviata fallita E copia di emergenza fallita.
    // Il testo esiste solo nella finestra: chiuderla deve chiedere conferma.
    void testSentCopyAndFallbackFailedKeepsComposerDirty() {
        const QString sent = dir_ + QStringLiteral("/mail/a/Sent");
        const QString blocker = dir_ + QStringLiteral("/non-una-cartella");
        writeFile(blocker, "file, non cartella");
        MainWindowOptions o = opts();
        o.unsavedDir = blocker + QStringLiteral("/non-salvati");
        o.msmtpProgram = writeScript(dir_, QStringLiteral("msmtpbreak2"),
                                     ("#!/bin/sh\nd=$(dirname \"$0\")\ncat > \"$d/stdin\"\nrm -rf '" +
                                      sent.toUtf8() + "'\nexit 0\n"));
        MainWindow w(makeConfig(false), o);
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        w.newButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        c->toEdit()->setText(QStringLiteral("bob@x.test"));
        c->bodyEdit()->setPlainText(QStringLiteral("Testo unico 9090"));
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return !n.titles.isEmpty(); }, 20000));
        QTest::qWait(300);
        for (const char* d : {"cur", "new", "tmp"}) QDir().mkpath(sent + QLatin1Char('/') + QLatin1String(d)); // per gli altri test
        QVERIFY(c);
        QVERIFY(!c->sendButton()->isEnabled());
        QVERIFY2(c->statusLabel()->text().contains(QStringLiteral("INVIATO")), qPrintable(c->statusLabel()->text()));
        QVERIFY(!QFileInfo::exists(o.unsavedDir));
        int asked = 0;
        c->setConfirmCallback([&] { ++asked; return false; });
        QVERIFY(!c->close());
        QCOMPARE(asked, 1);
        QVERIFY(c);
        QVERIFY(c->bodyEdit()->toPlainText().contains(QStringLiteral("9090")));
        c->setConfirmCallback([&] { ++asked; return true; });
        c->close();
        QVERIFY(QTest::qWaitFor([&] { return c.isNull(); }, 2000));
        QCOMPARE(asked, 2);
        QFile::remove(blocker);
    }
    // I6: chiudere la finestra principale passa da ogni finestra di scrittura (che
    // chiede conferma se ci sono modifiche): se una rifiuta, nulla si chiude.
    void testMainCloseAsksEveryComposer() {
        MainWindow w(makeConfig(false), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        w.newButton()->click();
        w.newButton()->click();
        QCOMPARE(w.composers().size(), 2);
        QPointer<ComposeWindow> clean = w.composers().at(0), dirty = w.composers().at(1);
        dirty->bodyEdit()->setPlainText(QStringLiteral("non perdermi"));
        int asked = 0;
        bool answer = false;
        dirty->setConfirmCallback([&] { ++asked; return answer; });
        QVERIFY(!w.close());
        QCOMPARE(asked, 1);
        QVERIFY(w.isVisible());
        QVERIFY(dirty);
        QVERIFY(dirty->isVisible());
        answer = true;
        QVERIFY(w.close());
        QCOMPARE(asked, 2);
        QVERIFY(QTest::qWaitFor([&] { return dirty.isNull() && clean.isNull(); }, 2000));
    }
    // I6: mai chiudere durante un invio (msmtp in corso).
    void testMainCloseRefusedWhileSending() {
        MainWindowOptions o = opts();
        o.msmtpProgram = writeScript(dir_, QStringLiteral("msmtpslow"),
                                     "#!/bin/sh\nd=$(dirname \"$0\")\ncat > \"$d/stdin\"\nsleep 2\necho run >> \"$d/count\"\nexit 0\n");
        MainWindow w(makeConfig(false), o);
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        const int before = log(QStringLiteral("count")).count(QStringLiteral("run"));
        w.newButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        c->toEdit()->setText(QStringLiteral("bob@x.test"));
        c->bodyEdit()->setPlainText(QStringLiteral("in viaggio"));
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return w.sender()->busy(); }, 5000));
        QVERIFY(!w.close());
        QVERIFY(w.isVisible());
        QVERIFY(!n.titles.isEmpty());
        QVERIFY2(n.texts.last().contains(QStringLiteral("invio")), qPrintable(n.texts.last()));
        QVERIFY(QTest::qWaitFor([&] { return !w.sender()->busy(); }, 20000));
        QCOMPARE(log(QStringLiteral("count")).count(QStringLiteral("run")), before + 1);
        QVERIFY(QTest::qWaitFor([&] { return c.isNull(); }, 5000));
        QVERIFY(w.close());
    }
    // I5: "Salva allegato..." salva i byte decodificati nel file scelto; un file
    // esistente non viene mai sovrascritto (errore chiaro, contenuto intatto).
    void testSaveAttachmentNeverOverwrites() {
        const QString p = dir_ + QStringLiteral("/mail/a/INBOX/cur/5.eml:2,S");
        writeFile(p, "From: Gina <gina@x.test>\nTo: alice@example.test\nSubject: Con allegato\nMessage-ID: <m5@x>\n"
                     "MIME-Version: 1.0\nContent-Type: multipart/mixed; boundary=\"b\"\n\n"
                     "--b\nContent-Type: text/plain\n\nVedi allegato 5555\n"
                     "--b\nContent-Type: application/octet-stream; name=\"../../etc/rapporto.bin\"\n"
                     "Content-Disposition: attachment; filename=\"../../etc/rapporto.bin\"\n"
                     "Content-Transfer-Encoding: base64\n\nAAECA2NpYW8=\n--b--\n");
        listUnread({jsonMsg(p, QStringLiteral("m5@x"), QStringLiteral("Gina"), QStringLiteral("Con allegato"), false)});
        MainWindow w(makeConfig(), opts());
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        QStringList suggested;
        const QString out = dir_ + QStringLiteral("/scaricati");
        QDir().mkpath(out);
        QString answer = out + QStringLiteral("/rapporto.bin");
        w.setSaveFileCallback([&](const QString& s) { suggested << s; return answer; });
        w.show();
        QVERIFY(waitRows(w, 1));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("5555")); }, 10000));
        QToolButton* b = w.messageView()->attachmentButton();
        QVERIFY(b->isVisibleTo(w.messageView()));
        QCOMPARE(b->menu()->actions().size(), 1);
        b->menu()->actions().first()->trigger();
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(answer); }, 5000));
        QCOMPARE(suggested.size(), 1);
        QVERIFY2(suggested.first().endsWith(QStringLiteral("/rapporto.bin")) && !suggested.first().contains(QStringLiteral("..")),
                 qPrintable(suggested.first()));
        QCOMPARE(readFile(answer), QByteArray("\x00\x01\x02\x03" "ciao", 8));
        // secondo salvataggio sullo stesso file: rifiutato, contenuto intatto
        writeFile(answer, "MIO");
        b->menu()->actions().first()->trigger();
        QVERIFY(QTest::qWaitFor([&] { return !n.titles.isEmpty(); }, 5000));
        QVERIFY2(n.texts.last().contains(QStringLiteral("esiste")), qPrintable(n.texts.last()));
        QCOMPARE(readFile(answer), QByteArray("MIO"));
        // annullato: niente
        answer.clear();
        b->menu()->actions().first()->trigger();
        QTest::qWait(300);
        QCOMPARE(QDir(out).entryList(QDir::Files).size(), 1);
        QDir(out).removeRecursively();
        QFile::remove(p);
    }
    // Durante la finestra (modale) di scelta del file la selezione puo' cambiare:
    // si salva comunque l'allegato del messaggio da cui e' partita la richiesta.
    void testSaveAttachmentUsesMessageChosenBeforeDialog() {
        auto mk = [](const char* id, const char* subj, const char* b64) {
            return QByteArray("From: Gina <gina@x.test>\nTo: alice@example.test\nSubject: ") + subj +
                   "\nMessage-ID: <" + id + ">\nMIME-Version: 1.0\nContent-Type: multipart/mixed; boundary=\"b\"\n\n"
                   "--b\nContent-Type: text/plain\n\nTesto " + subj + "\n"
                   "--b\nContent-Type: application/octet-stream; name=\"x.bin\"\n"
                   "Content-Disposition: attachment; filename=\"x.bin\"\n"
                   "Content-Transfer-Encoding: base64\n\n" + b64 + "\n--b--\n";
        };
        const QString p1 = dir_ + QStringLiteral("/mail/a/INBOX/cur/61.eml:2,S");
        const QString p2 = dir_ + QStringLiteral("/mail/a/INBOX/cur/62.eml:2,S");
        writeFile(p1, mk("m61@x", "primo6161", "UFJJTU8="));    // "PRIMO"
        writeFile(p2, mk("m62@x", "secondo6262", "U0VDT05ETw==")); // "SECONDO"
        listUnread({jsonMsg(p1, QStringLiteral("m61@x"), QStringLiteral("Gina"), QStringLiteral("primo6161"), false),
                    jsonMsg(p2, QStringLiteral("m62@x"), QStringLiteral("Gina"), QStringLiteral("secondo6262"), false)});
        MainWindow w(makeConfig(), opts());
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        const QString out = dir_ + QStringLiteral("/scaricati2");
        QDir().mkpath(out);
        const QString answer = out + QStringLiteral("/x.bin");
        w.setSaveFileCallback([&](const QString&) {
            // mentre la finestra e' aperta l'utente passa all'altro messaggio
            select(w, 1);
            (void)QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("secondo6262")); }, 10000);
            return answer;
        });
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.messageView()->toPlainText().contains(QStringLiteral("primo6161")); }, 10000));
        w.messageView()->attachmentButton()->menu()->actions().first()->trigger();
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(answer); }, 10000));
        QVERIFY(w.messageView()->toPlainText().contains(QStringLiteral("secondo6262")));
        QCOMPARE(readFile(answer), QByteArray("PRIMO"));
        QDir(out).removeRecursively();
        QFile::remove(p1);
        QFile::remove(p2);
    }
    // I5: i link si aprono solo dopo conferma che mostra la destinazione reale, e
    // solo http, https e mailto.
    void testLinksNeedConfirmationAndSafeScheme() {
        MainWindow w(makeConfig(), opts());
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        QList<QUrl> asked, opened;
        bool yes = false;
        w.setConfirmLinkCallback([&](const QUrl& u, const QString& text) {
            asked << u;
            n.texts << text;
            return yes;
        });
        w.setOpenUrlCallback([&](const QUrl& u) { opened << u; });
        w.show();
        QVERIFY(waitRows(w, 2));
        emit w.messageView()->linkActivated(QUrl(QStringLiteral("https://banca.example/accesso")));
        QCOMPARE(asked.size(), 1);
        QVERIFY(n.texts.last().contains(QStringLiteral("https://banca.example/accesso")));
        QVERIFY(opened.isEmpty()); // rifiutato
        yes = true;
        emit w.messageView()->linkActivated(QUrl(QStringLiteral("https://xn--pypal-4ve.example/")));
        QCOMPARE(opened.size(), 1);
        // dominio internazionale: la conferma mostra la forma ASCII (punycode)
        emit w.messageView()->linkActivated(QUrl(QString::fromUtf8("https://p\xd0\xb0ypal.example/")));
        QVERIFY2(n.texts.last().contains(QStringLiteral("xn--")), qPrintable(n.texts.last()));
        emit w.messageView()->linkActivated(QUrl(QStringLiteral("mailto:bob@x.test")));
        QCOMPARE(opened.size(), 3);
        const int askedBefore = asked.size();
        for (const char* bad : {"file:///etc/passwd", "javascript:alert(1)", "smb://host/share", "relativo.html", "data:text/html,x"}) {
            emit w.messageView()->linkActivated(QUrl(QString::fromLatin1(bad)));
        }
        QCOMPARE(asked.size(), askedBefore); // nemmeno chiesto
        QCOMPARE(opened.size(), 3);
        QVERIFY(n.titles.contains(QStringLiteral("Link non aperto")));
    }
    // Dopo l'invio e l'indicizzazione riuscita l'elenco viene ricaricato.
    void testListRefreshedAfterReindexAfterSend() {
        MainWindow w(makeConfig(false), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        QSignalSpy spy(w.sender(), &Sender::reindexFinished);
        w.newButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        c->toEdit()->setText(QStringLiteral("bob@x.test"));
        c->bodyEdit()->setPlainText(QStringLiteral("ciao"));
        resetLogs();
        c->sendButton()->click();
        QVERIFY(spy.wait(20000));
        QVERIFY(spy.first().at(0).toBool());
        QVERIFY(QTest::qWaitFor([&] {
            const QString l = log(QStringLiteral("mu.log"));
            const int i = l.indexOf(QStringLiteral("index"));
            return i >= 0 && l.indexOf(QStringLiteral("find"), i) > i;
        }, 5000));
        QVERIFY(waitRows(w, 2));
    }
    void testSendUnsignedNeedsExplicitConfirmation() {
        MainWindow w(makeConfig(true), opts());
        int asked = 0;
        bool answer = false;
        w.setConfirmUnsignedCallback([&] { ++asked; return answer; });
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.replyButton()->isEnabled(); }, 10000));
        w.replyButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        // identita' sales (a2) senza chiave PGP: la firma e' attiva solo se la identita' lo chiede
        c->identityCombo()->setCurrentIndex(c->identityCombo()->findData(QStringLiteral("a2")) >= 0
                                                ? c->identityCombo()->findData(QStringLiteral("a2")) : 1);
        c->signButton()->setChecked(true);
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return asked == 1; }, 20000));
        QTest::qWait(500);
        QVERIFY(!QFile::exists(dir_ + QStringLiteral("/count"))); // risposto "no": nulla inviato
        QVERIFY(!c.isNull());
        QVERIFY(c->sendButton()->isEnabled());
        answer = true;
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return c.isNull(); }, 20000));
        QCOMPARE(asked, 2);
        QCOMPARE(log(QStringLiteral("count")).count(QStringLiteral("run")), 1);
    }
    void testSendMissingKeysShowsListAndSendsNothing() {
        MainWindow w(makeConfig(false), opts());
        Noticed n;
        w.setNoticeCallback([&](const QString& t, const QString& x) { n.titles << t; n.texts << x; });
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.replyButton()->isEnabled(); }, 10000));
        w.replyButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        c->encryptButton()->setChecked(true);
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return !n.texts.isEmpty(); }, 20000));
        QVERIFY2(n.texts.first().contains(QStringLiteral("eve@x.test")), qPrintable(n.texts.first()));
        QVERIFY(!QFile::exists(dir_ + QStringLiteral("/count")));
        QVERIFY(!c.isNull()); // la finestra di scrittura resta aperta
        QVERIFY(c->sendButton()->isEnabled());
    }
    void testDoubleClickSendsOnce() {
        MainWindow w(makeConfig(false), opts());
        w.show();
        QVERIFY(waitRows(w, 2));
        select(w, 0);
        QVERIFY(QTest::qWaitFor([&] { return w.replyButton()->isEnabled(); }, 10000));
        w.replyButton()->click();
        QPointer<ComposeWindow> c = firstComposer(w);
        QVERIFY(c);
        c->sendButton()->click();
        c->sendButton()->click();
        c->sendButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return c.isNull(); }, 20000));
        QTest::qWait(500);
        QCOMPARE(log(QStringLiteral("count")).count(QStringLiteral("run")), 1);
    }
    void testDestroyDuringAsyncWorkDoesNotCrash() {
        listUnread({jsonMsg(pathSigned_, QStringLiteral("sig1@x"), QStringLiteral("Alice"), QStringLiteral("Firmato"), false)});
        MainWindowOptions o = opts();
        o.gpgProgram = gpgslow_;
        {
            auto w = std::make_unique<MainWindow>(makeConfig(false), o);
            w->show();
            QVERIFY(waitRows(*w, 1));
            select(*w, 0);
            QVERIFY(QTest::qWaitFor([&] { return w->messageView()->toPlainText().contains(QStringLiteral("Corpo firmato 5511")); }, 10000));
            w->searchEdit()->setText(QStringLiteral("x"));
            w.reset(); // verifica, ricerca e lettura ancora in corso
        }
        QTest::qWait(1800);
        QVERIFY(true);
    }
};

QTEST_MAIN(T)
#include "test_mainwindow.moc"
