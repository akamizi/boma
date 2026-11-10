#include <QtTest>
#include "core/MailIndex.h"
#include "ui/MessageListModel.h"
using namespace boma;

namespace {
const int N = 2000;

struct Result { bool done = false; QList<MessageSummary> list; QString error; };

Result runSearch(MailIndex& idx, const QString& q, int off, int lim, bool unread = false)
{
    Result r;
    auto cb = [&r](QList<MessageSummary> l, QString e) { r.list = l; r.error = e; r.done = true; };
    if (unread) idx.unread(off, lim, cb); else idx.search(q, off, lim, cb);
    (void)QTest::qWaitFor([&] { return r.done; }, 60000);
    return r;
}
}

class T : public QObject {
    Q_OBJECT
    QTemporaryDir td{QDir::currentPath() + "/indexXXXXXX"};
    QString mail, muhome;
    MailIndex* idx = nullptr;
private slots:
    void initTestCase() {
        QVERIFY(td.isValid());
        mail = td.path() + "/mail";
        muhome = td.path() + "/muhome";
        QProcess gen;
        gen.start("python3", {QString(BOMA_FIXTURES_DIR) + "/make_maildir.py", mail, QString::number(N)});
        QVERIFY(gen.waitForFinished(120000));
        QCOMPARE(gen.exitCode(), 0);
        QProcess init;
        init.start("mu", {"init", "--muhome=" + muhome, "--maildir=" + mail});
        QVERIFY(init.waitForFinished(60000));
        QCOMPARE(init.exitCode(), 0);
        idx = new MailIndex(this, "mu", muhome);
        Account a; a.id = "acc"; a.maildirRoot = mail;
        idx->setAccounts({a});
        bool done = false, ok = false; QString err;
        idx->reindex("acc", [&](bool o, QString e) { ok = o; err = e; done = true; });
        QTest::qWaitFor([&] { return done; }, 120000);
        QVERIFY2(ok, qPrintable(err));
    }
    void testUnreadReturnsOnlyUnread() {
        auto r = runSearch(*idx, {}, 0, 5000, true);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.list.size(), (N + 2) / 3);
        for (const auto& m : r.list) {
            QVERIFY(m.unread);
            QCOMPARE(m.accountId, QString("acc"));
        }
        // piu' recente per primo
        QVERIFY(r.list.first().date >= r.list.last().date);
    }
    void testSearchBySubjectAndSender() {
        auto r = runSearch(*idx, "subject:numero1234", 0, 50);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.list.size(), 1);
        QCOMPARE(r.list[0].subject, QString("Oggetto numero1234 argomento%1").arg(1234 % 13));
        QCOMPARE(r.list[0].messageId, QString("msg1234@gen.example.org"));
        auto s = runSearch(*idx, "from:Mittente5", 0, 5000);
        int expected = 0;
        for (int i = 0; i < N; ++i) if (i % 97 == 5) ++expected;
        QCOMPARE(s.list.size(), expected);
        QVERIFY(s.list[0].from.startsWith("Mittente5"));
        auto none = runSearch(*idx, "subject:inesistentexyz", 0, 50);
        QVERIFY(none.error.isEmpty());
        QVERIFY(none.list.isEmpty());
        auto att = runSearch(*idx, "flag:attach", 0, 5000);
        for (const auto& m : att.list) QVERIFY(m.hasAttachment);
        QCOMPARE(att.list.size(), (N + 6) / 7);
    }
    void testPagingReturnsAllWithoutDuplicates() {
        QSet<QString> seen;
        const int page = 300;
        int total = 0;
        for (int off = 0; off < N + page; off += page) {
            auto r = runSearch(*idx, "date:2000..", off, page);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            for (const auto& m : r.list) {
                QVERIFY2(!seen.contains(m.path), "duplicato");
                seen.insert(m.path);
            }
            total += r.list.size();
            if (r.list.size() < page) break;
        }
        QCOMPARE(total, N);
        QCOMPARE(seen.size(), N);
    }
    void testBadQueryReportsErrorOrEmptyNeverCrash() {
        // mu e' tollerante con le query malformate: o errore o nessun risultato, mai crash.
        for (const char* q : {"(", "subject:\"aperta", "date:xx..", "and or"}) {
            auto r = runSearch(*idx, q, 0, 10);
            QVERIFY(r.done);
        }
        // una query che sembra un'opzione arriva a mu dopo "--", come elemento unico
        const QString argvFile = td.path() + "/argv.txt";
        MailIndex rec(nullptr, fakeMu("murec", "printf '%s\\n' \"$@\" > '" + argvFile + "'; echo '[]'"), muhome);
        auto opt = runSearch(rec, "--maxnum=1", 0, 10);
        QVERIFY(opt.error.isEmpty());
        QFile af(argvFile); QVERIFY(af.open(QIODevice::ReadOnly));
        const QStringList argv = QString::fromUtf8(af.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(argv.count("--maxnum=1"), 1);
        const int qi = argv.indexOf("--maxnum=1");
        QVERIFY(qi > 0);
        QCOMPARE(argv[qi - 1], QString("--"));
        QCOMPARE(qi, argv.size() - 1);
        auto normal = runSearch(*idx, "date:2000..", 0, 10);
        QCOMPARE(normal.list.size(), 10);
        // database inesistente: errore riportato
        MailIndex bad(nullptr, "mu", td.path() + "/nessuno");
        auto r = runSearch(bad, "subject:x", 0, 10);
        QVERIFY(!r.error.isEmpty());
        QVERIFY(r.list.isEmpty());
        // mu assente: errore riportato
        MailIndex missing(nullptr, td.path() + "/non-esiste-mu", muhome);
        auto r2 = runSearch(missing, "x", 0, 10);
        QVERIFY(!r2.error.isEmpty());
        // paginazione non valida
        auto r3 = runSearch(*idx, "x", 0, 0);
        QVERIFY(!r3.error.isEmpty());
    }
    void testModelLoadsLazilyInPages() {
        MessageListModel m(idx);
        m.setQuery("date:2000..");
        QCOMPARE(m.rowCount(), 0);
        QVERIFY(m.canFetchMore({}));
        int guard = 0;
        while (m.canFetchMore({}) && guard++ < 50) {
            const int before = m.rowCount();
            QSignalSpy spy(&m, &MessageListModel::pageLoaded);
            m.fetchMore({});
            QVERIFY(!m.canFetchMore({})); // una sola richiesta alla volta
            QVERIFY(spy.wait(60000));
            QVERIFY(m.rowCount() - before <= MessageListModel::PageSize);
        }
        QCOMPARE(m.rowCount(), N);
        QSet<QString> paths;
        for (int i = 0; i < m.rowCount(); ++i) paths.insert(m.data(m.index(i), MessageListModel::PathRole).toString());
        QCOMPARE(paths.size(), N);
        QVERIFY(m.data(m.index(0), MessageListModel::DateRole).toDateTime().isValid());
        QVERIFY(!m.data(m.index(0), MessageListModel::FromRole).toString().isEmpty());
    }
    void testModelEmptyQueryIsUnreadView() {
        MessageListModel m(idx);
        m.setQuery({});
        QSignalSpy spy(&m, &MessageListModel::pageLoaded);
        m.fetchMore({});
        QVERIFY(spy.wait(60000));
        QCOMPARE(m.rowCount(), MessageListModel::PageSize);
        for (int i = 0; i < m.rowCount(); ++i)
            QVERIFY(m.data(m.index(i), MessageListModel::UnreadRole).toBool());
    }
    void testModelDiscardsSupersededQuery() {
        MessageListModel m(idx);
        m.setQuery("subject:numero1234");
        m.fetchMore({});                 // richiesta in volo
        m.setQuery("subject:numero77");  // la supera
        QSignalSpy spy(&m, &MessageListModel::pageLoaded);
        m.fetchMore({});
        QVERIFY(spy.wait(60000));
        QTest::qWait(300);               // lascia arrivare l'eventuale risultato tardivo
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.data(m.index(0), MessageListModel::SubjectRole).toString().left(17), QString("Oggetto numero77 "));
    }
    QString fakeMu(const QString& name, const QString& body) {
        const QString p = td.path() + "/" + name;
        QFile f(p); f.open(QIODevice::WriteOnly);
        f.write("#!/bin/sh\n" + body.toUtf8() + "\n"); f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
        return p;
    }
    void testExitCode2Handling() {
        MailIndex empty(nullptr, fakeMu("mu2a", "exit 2"), muhome);
        auto a = runSearch(empty, "x", 0, 10);
        QVERIFY(a.error.isEmpty()); QVERIFY(a.list.isEmpty());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("nessuna"));
        MailIndex nomatch(nullptr, fakeMu("mu2b", "echo 'nessuna corrispondenza' >&2; exit 2"), muhome);
        auto b = runSearch(nomatch, "x", 0, 10);
        QVERIFY(b.error.isEmpty()); QVERIFY(b.list.isEmpty());
        // stderr con codice 2: sempre vuoto, avviso non fatale
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("parse error: boom"));
        MailIndex perr(nullptr, fakeMu("mu2c", "echo 'parse error: boom' >&2; exit 2"), muhome);
        auto c = runSearch(perr, "x", 0, 10);
        QVERIFY(c.error.isEmpty()); QVERIFY(c.list.isEmpty());
    }
    void testModelDestroyedWhileQueryInFlight() {
        const QString marker = td.path() + "/slow-done";
        QFile::remove(marker);
        MailIndex slow(nullptr, fakeMu("mus", "sleep 1; touch '" + marker + "'; echo '[]'"), muhome);
        auto* m = new MessageListModel(&slow);
        m->setQuery("x");
        m->fetchMore({});
        const int before = MessageListModel::s_applied;
        delete m;
        // il processo finisce e la callback tardiva arriva a modello distrutto
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(marker); }, 10000));
        QTest::qWait(500);
        QCOMPARE(MessageListModel::s_applied, before); // nessuna scrittura nel modello distrutto
    }
    void testMaxWindowTruncates() {
        MessageListModel m(idx);
        m.setMaxWindow(500);
        m.setQuery("date:2000..");
        while (m.canFetchMore({})) {
            QSignalSpy spy(&m, &MessageListModel::pageLoaded);
            m.fetchMore({});
            QVERIFY(spy.wait(60000));
        }
        QCOMPARE(m.rowCount(), 500);
        QVERIFY(m.truncated());
        MessageListModel small(idx);
        small.setQuery("subject:numero77");
        QSignalSpy spy(&small, &MessageListModel::pageLoaded);
        small.fetchMore({});
        QVERIFY(spy.wait(60000));
        QVERIFY(!small.truncated());
    }
    void testTruncatedOnlyWhenRowsHidden() {
        // subject:argomento5 ha esattamente 154 risultati su 2000
        MessageListModel exact(idx);
        exact.setMaxWindow(154);
        exact.setQuery("subject:argomento5");
        while (exact.canFetchMore({})) {
            QSignalSpy spy(&exact, &MessageListModel::pageLoaded);
            exact.fetchMore({});
            QVERIFY(spy.wait(60000));
        }
        QCOMPARE(exact.rowCount(), 154);
        QVERIFY(!exact.truncated());
        MessageListModel cut(idx);
        cut.setMaxWindow(153);
        cut.setQuery("subject:argomento5");
        while (cut.canFetchMore({})) {
            QSignalSpy spy(&cut, &MessageListModel::pageLoaded);
            cut.fetchMore({});
            QVERIFY(spy.wait(60000));
        }
        QCOMPARE(cut.rowCount(), 153);
        QVERIFY(cut.truncated());
    }
    void testModelErrorAllowsRetry() {
        MailIndex bad(nullptr, "mu", td.path() + "/nessuno");
        MessageListModel m(&bad);
        m.setQuery("x");
        QSignalSpy spy(&m, &MessageListModel::pageLoaded);
        m.fetchMore({});
        QVERIFY(spy.wait(60000));
        QVERIFY(m.canFetchMore({}));
        bad.setMuHome(muhome);
        m.fetchMore({});
        QVERIFY(spy.wait(60000));
        QVERIFY(spy.last().at(1).toString().isEmpty());
    }
    void testModelReportsError() {
        MailIndex bad(nullptr, "mu", td.path() + "/nessuno");
        MessageListModel m(&bad);
        m.setQuery("x");
        QSignalSpy spy(&m, &MessageListModel::pageLoaded);
        m.fetchMore({});
        QVERIFY(spy.wait(60000));
        QVERIFY(!spy.first().at(1).toString().isEmpty());
    }
};
QTEST_MAIN(T)
#include "test_mailindex.moc"
