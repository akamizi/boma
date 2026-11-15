#include <QtTest>
#include "core/NewMailNotifier.h"
#include "core/Notifier.h"
using namespace boma;

namespace {
const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'800'000'000);

MessageSummary msg(const QString& id, int secs, bool unread = true) {
    MessageSummary m;
    m.messageId = id;
    m.path = "/m/" + id;
    m.from = "mitt-" + id;
    m.subject = "ogg-" + id;
    m.date = t0.addSecs(secs);
    m.unread = unread;
    return m;
}
} // namespace

// Un "mu" finto: elenco di messaggi, di cui si puo' cambiare lo stato di lettura.
class T : public QObject {
    Q_OBJECT
    QList<MessageSummary> all_;
    QString failWith_;
    int fetches_ = 0, newestCalls_ = 0;
    QList<QPair<QString, QString>> shown_;
    bool enabled_ = true;
    NewMailNotifier* n_ = nullptr;

    void fetch(const QDateTime& since, int limit, SearchCallback cb) {
        ++fetches_;
        QList<MessageSummary> out;
        if (failWith_.isEmpty()) {
            for (const auto& m : all_)
                if (m.unread && m.date >= since) out << m;
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.date < b.date; });
            out = out.mid(0, limit);
        }
        const QString err = failWith_;
        QTimer::singleShot(0, [=]() { cb(out, err); });
    }
    void newest(SearchCallback cb) {
        ++newestCalls_;
        QList<MessageSummary> out;
        if (failWith_.isEmpty()) {
            for (const auto& m : all_)
                if (m.unread && (out.isEmpty() || m.date > out.first().date)) out = {m};
        }
        const QString err = failWith_;
        QTimer::singleShot(0, [=]() { cb(out, err); });
    }
    void settle() { QTest::qWait(60); }
    void setRead(const QString& id, bool unread) {
        for (auto& m : all_) if (m.messageId == id) m.unread = unread;
    }

private slots:
    void init() {
        all_.clear(); shown_.clear(); failWith_.clear(); fetches_ = newestCalls_ = 0; enabled_ = true;
        delete n_;
        n_ = new NewMailNotifier(this);
        n_->setRetryInterval(20);
        n_->setFetch([this](const QDateTime& s, int l, SearchCallback cb) { fetch(s, l, std::move(cb)); });
        n_->setNewest([this](SearchCallback cb) { newest(std::move(cb)); });
        n_->setShow([this](const QString& t, const QString& b) { shown_ << qMakePair(t, b); });
        n_->setEnabled([this]() { return enabled_; });
    }

    void testBaselineIsSilent() {
        all_ << msg("a", 10) << msg("b", 20) << msg("c", 30);
        n_->check(); settle();
        QVERIFY(n_->initialized());
        QCOMPARE(n_->watermark(), t0.addSecs(30));
        QVERIFY(shown_.isEmpty());
        n_->check(); settle(); // nulla di nuovo
        QVERIFY(shown_.isEmpty());
    }
    void testSingleAndMultipleNotified() {
        all_ << msg("a", 10);
        n_->check(); settle();
        all_ << msg("b", 20);
        n_->check(); settle();
        QCOMPARE(shown_.size(), 1);
        QCOMPARE(shown_[0].first, QString("mitt-b"));
        QCOMPARE(shown_[0].second, QString("ogg-b"));
        all_ << msg("c", 30) << msg("d", 40);
        n_->check(); settle();
        QCOMPARE(shown_.size(), 2);
        QVERIFY(shown_[1].first.contains("2"));
        QVERIFY(shown_[1].second.contains("mitt-d")); // il piu' recente
        n_->check(); settle();
        QCOMPARE(shown_.size(), 2); // mai due volte
    }
    void testReturningUnreadIsNotRenotified() {
        all_ << msg("a", 10, false) << msg("b", 20, false) << msg("n", 30);
        n_->check(); settle();                // baseline: n
        all_ << msg("x", 40);
        n_->check(); settle();
        QCOMPARE(shown_.size(), 1);
        // "a" era letto, ora torna non letto: data piu' vecchia del watermark
        setRead("a", true);
        setRead("n", true);                    // idem, gia' visto
        n_->check(); settle();
        QCOMPARE(shown_.size(), 1);
        // un messaggio davvero nuovo continua a essere notificato
        all_ << msg("y", 50);
        n_->check(); settle();
        QCOMPARE(shown_.size(), 2);
    }
    void testBeyondOldLimitOf100AndStateStaysSmall() {
        all_ << msg("base", 0);
        n_->check(); settle();
        for (int i = 1; i <= 450; ++i) all_ << msg(QString("m%1").arg(i), 100 + i);
        n_->check();
        QTRY_VERIFY_WITH_TIMEOUT(n_->watermark() == t0.addSecs(550), 3000);
        QVERIFY(fetches_ > 2);                 // piu' lotti, nessuno perso
        int total = 0;
        for (const auto& s : shown_) total += s.first.contains("nuovi") ? s.first.split(' ').first().toInt() : 1;
        QCOMPARE(total, 450);
        QCOMPARE(n_->watermarkIds(), 1);       // lo stato non cresce con il numero di messaggi
    }
    void testSameSecondMessages() {
        all_ << msg("a", 10) << msg("b", 10);  // stessa data: il baseline ne vede uno solo
        n_->check(); settle();
        QVERIFY(shown_.isEmpty());             // l'altro con la stessa data non e' "nuovo"
        all_ << msg("c", 10);                  // arriva dopo, stessa data: e' nuovo
        n_->check(); settle();
        QCOMPARE(shown_.size(), 1);
        QCOMPARE(shown_[0].first, QString("mitt-c"));
        n_->check(); settle();
        QCOMPARE(shown_.size(), 1);
    }
    void testStartWithNoUnread() {
        n_->check(); settle();
        QVERIFY(n_->initialized());
        all_ << msg("a", 10);
        n_->check(); settle();
        QCOMPARE(shown_.size(), 1);
    }
    void testFailureLosesNothingAndRetries() {
        all_ << msg("a", 10);
        n_->check(); settle();
        all_ << msg("b", 20);
        failWith_ = "mu: errore";
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Controllo della posta nuova non riuscito: mu: errore"));
        n_->check(); settle();
        QVERIFY(shown_.isEmpty());
        QCOMPARE(n_->watermark(), t0.addSecs(10)); // fermo
        failWith_.clear();
        QTRY_COMPARE_WITH_TIMEOUT(shown_.size(), 1, 2000); // il nuovo tentativo automatico recupera
        QCOMPARE(shown_[0].second, QString("ogg-b"));
    }
    void testBaselineFailureThenRecovery() {
        all_ << msg("a", 10);
        failWith_ = "mu: giu'";
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("non riuscito"));
        n_->check(); settle();
        QVERIFY(!n_->initialized());
        failWith_.clear();
        QTRY_VERIFY_WITH_TIMEOUT(n_->initialized(), 2000);
        QVERIFY(shown_.isEmpty());             // quella gia' presente resta silenziosa
    }
    void testDisabledStillAdvances() {
        all_ << msg("a", 10);
        n_->check(); settle();
        enabled_ = false;
        all_ << msg("b", 20);
        n_->check(); settle();
        QVERIFY(shown_.isEmpty());
        QCOMPARE(n_->watermark(), t0.addSecs(20));
        enabled_ = true;
        n_->check(); settle();
        QVERIFY(shown_.isEmpty());             // riattivando non arriva la raffica arretrata
    }
    void testOverlappingChecksAreCoalesced() {
        all_ << msg("a", 10);
        n_->check(); settle();
        all_ << msg("b", 20);
        n_->check(); n_->check(); n_->check();
        settle();
        QCOMPARE(shown_.size(), 1);
        QVERIFY(fetches_ <= 3);
    }
    void testNotifierEscapesAndReportsFailure() {
        QString gotTitle, gotBody;
        Notifier::setTransport([&](const QString& t, const QString& b, int, std::function<void(const QString&)> done) {
            gotTitle = t; gotBody = b; done(QString());
        });
        Notifier::show("T <b>", "Mario & <i>Rossi</i> > \"x\"");
        QCOMPARE(gotTitle, QString("T <b>"));
        QCOMPARE(gotBody, QString("Mario &amp; &lt;i&gt;Rossi&lt;/i&gt; &gt; &quot;x&quot;"));
        Notifier::setTransport([&](const QString&, const QString&, int, std::function<void(const QString&)> done) {
            done("rifiutata dal server");
        });
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Notifica del desktop non riuscita: rifiutata dal server"));
        Notifier::show("t", "b");
        Notifier::setTransport({});
    }
    void cleanupTestCase() { Notifier::setTransport({}); }
};
QTEST_MAIN(T)
#include "test_newmailnotifier.moc"
