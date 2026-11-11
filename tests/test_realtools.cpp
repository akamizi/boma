// Integrazione con i VERI mu e mbsync, solo su dati sintetici dentro una
// QTemporaryDir sotto la cartella corrente: MUHOME, rc di mbsync, HOME e cache
// sono tutti nella cartella temporanea. Canale mbsync Maildir -> Maildir (il
// "server" e' una seconda Maildir sintetica). Saltato se mu o mbsync mancano.
#include <QtTest>
#include "core/MailActions.h"
#include "core/MailIndex.h"
using namespace boma;

namespace {

bool writeFile(const QString& p, const QByteArray& data)
{
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(data) == data.size();
}

QByteArray message(const QString& id, const QString& subject)
{
    return QStringLiteral("From: a@example.org\nTo: b@example.org\nSubject: %1\nMessage-ID: <%2@example.org>\n"
                          "Date: Mon, 1 Jan 2024 10:00:00 +0000\n\nciao\n")
        .arg(subject, id)
        .toUtf8();
}

QStringList filesIn(const QString& folder)
{
    QStringList out;
    for (const char* sub : {"cur", "new"})
        for (const QString& f : QDir(folder + "/" + sub).entryList(QDir::Files))
            out << folder + "/" + sub + "/" + f;
    return out;
}

} // namespace

class T : public QObject {
    Q_OBJECT
    QTemporaryDir td{QDir::currentPath() + "/realtoolsXXXXXX"};
    QString far, mail, acc, muhome, rc;

    struct Run { int code = -1; QString out; };
    Run run(const QString& prog, const QStringList& args, int ms = 60000) {
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(prog, args);
        Run r;
        if (!p.waitForFinished(ms)) { p.kill(); p.waitForFinished(); return r; }
        r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        r.out = QString::fromUtf8(p.readAll());
        return r;
    }
    Run mbsync() { return run("mbsync", {"-c", rc, "-a"}); }
    bool reindex(MailIndex& idx) {
        bool done = false, ok = false; QString err;
        idx.reindex("acc", [&](bool o, QString e) { ok = o; err = e; done = true; });
        if (!QTest::qWaitFor([&] { return done; }, 60000)) return false;
        if (!ok) qWarning() << err;
        return ok;
    }
    QList<MessageSummary> query(MailIndex& idx, const QString& q, bool unread = false) {
        bool done = false; QList<MessageSummary> l; QString err;
        auto cb = [&](QList<MessageSummary> r, QString e) { l = r; err = e; done = true; };
        if (unread) idx.unread(0, 100, cb); else idx.search(q, 0, 100, cb);
        (void)QTest::qWaitFor([&] { return done; }, 60000);
        if (!err.isEmpty()) qWarning() << err;
        return l;
    }

private slots:
    void initTestCase() {
        if (QStandardPaths::findExecutable("mu").isEmpty() || QStandardPaths::findExecutable("mbsync").isEmpty())
            QSKIP("mu o mbsync non installati");
        QVERIFY(td.isValid());
        const QString home = td.path() + "/home";
        QDir().mkpath(home);
        // Nessun file dell'utente letto o scritto: HOME e cartelle XDG nella cartella temporanea.
        qputenv("HOME", home.toUtf8());
        qputenv("XDG_CACHE_HOME", (home + "/.cache").toUtf8());
        qputenv("XDG_CONFIG_HOME", (home + "/.config").toUtf8());
        qputenv("XDG_DATA_HOME", (home + "/.local/share").toUtf8());
        qputenv("XDG_STATE_HOME", (home + "/.local/state").toUtf8());
        qunsetenv("MUHOME");
        far = td.path() + "/far";
        mail = td.path() + "/mail";
        acc = mail + "/acc";
        muhome = td.path() + "/muhome";
        rc = td.path() + "/mbsyncrc";
        for (const char* f : {"INBOX", "Trash", "Drafts", "Sent"})
            for (const char* s : {"cur", "new", "tmp"}) QVERIFY(QDir().mkpath(far + "/" + f + "/" + s));
        QVERIFY(QDir().mkpath(acc));
        // un messaggio letto e uno non letto nella posta in arrivo "remota"
        QVERIFY(writeFile(far + "/INBOX/cur/1700000000.1.host:2,S", message("letto", "messaggio letto")));
        QVERIFY(writeFile(far + "/INBOX/cur/1700000001.2.host:2,", message("nonletto", "messaggio non letto")));
        // una bozza (D, mai S) e un messaggio in Inviata senza S
        QVERIFY(writeFile(far + "/Drafts/cur/1700000002.3.host:2,D", message("bozza", "bozza")));
        QVERIFY(writeFile(far + "/Sent/cur/1700000003.4.host:2,", message("inviato", "inviato")));
        QVERIFY(writeFile(rc, QStringLiteral(
            "MaildirStore far\nPath %1/\nInbox %1/INBOX\nSubFolders Verbatim\n\n"
            "MaildirStore near\nPath %2/\nInbox %2/INBOX\nSubFolders Verbatim\n\n"
            "Channel acc\nFar :far:\nNear :near:\nPatterns *\nCreate Both\nExpunge Both\nSyncState *\n")
            .arg(far, acc).toUtf8()));
        const Run s = mbsync();
        QVERIFY2(s.code == 0, qPrintable(s.out));
        QCOMPARE(filesIn(acc + "/INBOX").size(), 2);
        const Run i = run("mu", {"init", "--muhome=" + muhome, "--maildir=" + mail});
        QVERIFY2(i.code == 0, qPrintable(i.out));
    }

    // C1 + I2: Elimina sposta con nome nuovo (senza ,U= della cartella di origine) e
    // bandiera S; il successivo mbsync esce 0 e il messaggio arriva nel Cestino remoto.
    void testDeleteThenSyncReachesFarTrash() {
        MailIndex idx(nullptr, "mu", muhome);
        Account a; a.id = "acc"; a.maildirRoot = acc; a.trashFolder = "Trash"; a.draftsFolder = "Drafts"; a.sentFolder = "Sent";
        idx.setAccounts({a});
        idx.setMuRoot(mail);
        QVERIFY(reindex(idx));
        const auto found = query(idx, "msgid:letto@example.org");
        QCOMPARE(found.size(), 1);
        const QString src = found.first().path;
        QVERIFY2(src.contains(",U="), qPrintable(src)); // file assegnato da mbsync

        MailActions actions(nullptr, "mu");
        actions.setMuHome(muhome);
        actions.setMuRoot(mail);
        bool done = false, ok = false; QString err;
        actions.moveToTrash(src, a, [&](bool o, QString e) { ok = o; err = e; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(ok, qPrintable(err));
        QVERIFY(!QFileInfo::exists(src));
        const QStringList inTrash = filesIn(acc + "/Trash");
        QCOMPARE(inTrash.size(), 1);
        const QString trashName = inTrash.first();

        const Run s = mbsync();
        const Run s2 = mbsync(); // anche le volte successive
        const QStringList farTrash = filesIn(far + "/Trash");
        qInfo().noquote() << "mbsync dopo Elimina:" << s.code << s.out.trimmed() << "| poi:" << s2.code
                          << "| cestino remoto:" << farTrash.size() << "| in arrivo remota:" << filesIn(far + "/INBOX").size();
        QVERIFY2(s.code == 0, qPrintable(s.out));
        QVERIFY2(!s.out.contains("beyond highest assigned UID") && !s.out.contains("duplicate UID"), qPrintable(s.out));
        QVERIFY2(s2.code == 0, qPrintable(s2.out));
        // remoto: nel cestino, non piu' nella posta in arrivo
        QCOMPARE(farTrash.size(), 1);
        QVERIFY2(!trashName.contains(",U="), qPrintable(trashName));
        QVERIFY2(trashName.endsWith(":2,S"), qPrintable(trashName)); // letto resta letto
        QFile f(farTrash.first());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("Message-ID: <letto@example.org>"));
        for (const QString& p : filesIn(far + "/INBOX")) {
            QFile g(p);
            QVERIFY(g.open(QIODevice::ReadOnly));
            QVERIFY2(!g.readAll().contains("<letto@example.org>"), qPrintable(p));
        }
        QCOMPARE(filesIn(far + "/INBOX").size(), 1);

        // indice reale dopo la sincronizzazione: il messaggio e' nel cestino, letto
        QVERIFY(reindex(idx));
        const auto after = query(idx, "msgid:letto@example.org");
        QCOMPARE(after.size(), 1);
        QVERIFY(after.first().path.startsWith(acc + "/Trash/"));
        QVERIFY(!after.first().unread);
        QCOMPARE(query(idx, "msgid:letto@example.org AND flag:unread").size(), 0);
    }

    // I2: "Non lette" non mostra Cestino, Bozze ne' Inviata.
    void testUnreadExcludesTrashDraftsSent() {
        MailIndex idx(nullptr, "mu", muhome);
        Account a; a.id = "acc"; a.maildirRoot = acc; a.trashFolder = "Trash"; a.draftsFolder = "Drafts"; a.sentFolder = "Sent";
        idx.setAccounts({a});
        idx.setMuRoot(mail);
        QVERIFY(reindex(idx));
        // senza esclusioni mu li considera non letti
        QCOMPARE(query(idx, "msgid:bozza@example.org AND flag:unread").size(), 1);
        QCOMPARE(query(idx, "msgid:inviato@example.org AND flag:unread").size(), 1);
        // un messaggio non letto finito nel cestino (senza S)
        const auto nl = query(idx, "msgid:nonletto@example.org");
        QCOMPARE(nl.size(), 1);
        const Run m = run("mu", {"move", "--muhome=" + muhome, nl.first().path, "/acc/Trash", "--change-name"});
        QVERIFY2(m.code == 0, qPrintable(m.out));
        QVERIFY(reindex(idx));
        QCOMPARE(query(idx, "msgid:nonletto@example.org AND flag:unread").size(), 1);

        const auto unread = query(idx, {}, true);
        for (const auto& s : unread) {
            QVERIFY2(!s.path.startsWith(acc + "/Trash/"), qPrintable(s.path));
            QVERIFY2(!s.path.startsWith(acc + "/Drafts/"), qPrintable(s.path));
            QVERIFY2(!s.path.startsWith(acc + "/Sent/"), qPrintable(s.path));
        }
        QCOMPARE(unread.size(), 0);
    }
};
QTEST_MAIN(T)
#include "test_realtools.moc"
