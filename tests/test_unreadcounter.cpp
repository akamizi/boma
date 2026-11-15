#include <QtTest>
#include "core/MailIndex.h"
#include "core/UnreadCounter.h"
using namespace boma;

// Un "mu" finto: le risposte arrivano dopo un ritardo scelto dal test, cosi' si possono
// sovrapporre i giri. Nessuna GUI, nessun processo.
class T : public QObject {
    Q_OBJECT
    UnreadCounter* c_ = nullptr;
    QHash<QString, int> folders_;        // cartella -> non lette
    QHash<QString, int> singles_;        // query -> non lette
    QString failByMaildir_, failSingle_; // non vuoto = errore
    int byMaildirCalls_ = 0, singleCalls_ = 0, delay_ = 0;
    QStringList singleQueries_;
    QList<QHash<QString, int>> published_;
    QStringList queries_;

    static QString fq(const QString& f) { return QStringLiteral("maildir:\"%1\"").arg(f); }

private slots:
    void init() {
        delete c_;
        folders_ = {{"/a/INBOX", 3}, {"/a/Sent", 1}};
        singles_ = {{QString(), 4}};
        failByMaildir_.clear(); failSingle_.clear();
        byMaildirCalls_ = singleCalls_ = delay_ = 0;
        singleQueries_.clear(); published_.clear();
        queries_ = {QString(), fq("/a/INBOX"), fq("/a/Sent"), fq("/a/Vuota")};
        c_ = new UnreadCounter(this);
        c_->setDebounce(10);
        c_->setMaildirOf([](const QString& q) { return MailIndex::maildirOfQuery(q); });
        c_->setQueries([this] { return queries_; });
        c_->setByMaildir([this](auto done) {
            ++byMaildirCalls_;
            const auto data = folders_; const auto err = failByMaildir_;
            QTimer::singleShot(delay_, [=]() { done(err.isEmpty() ? data : QHash<QString, int>{}, err); });
        });
        c_->setSingle([this](const QString& q, auto done) {
            ++singleCalls_; singleQueries_ << q;
            const int n = singles_.value(q, 0); const auto err = failSingle_;
            QTimer::singleShot(delay_, [=]() { if (err.isEmpty()) done(n, QString()); else done(-1, err); });
        });
        connect(c_, &UnreadCounter::countsReady, this, [this](const QHash<QString, int>& r) { published_ << r; });
    }
    void cleanupTestCase() { delete c_; }

    // Il caso comune: N cartelle = UNA chiamata a mu per tutte, piu' una per "Non lette".
    void testOneCallForAllFolders() {
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        QCOMPARE(byMaildirCalls_, 1);
        QCOMPARE(singleCalls_, 1);
        QCOMPARE(singleQueries_, QStringList{QString()});
        const auto r = published_.first();
        QCOMPARE(r.value(QString()), 4);
        QCOMPARE(r.value(fq("/a/INBOX")), 3);
        QCOMPARE(r.value(fq("/a/Sent")), 1);
        QCOMPARE(r.value(fq("/a/Vuota")), 0); // cartella senza non lette: zero, non "sconosciuto"
        QVERIFY(!c_->busy());
    }
    // Il numero di cartelle non cambia il numero di chiamate a mu.
    void testCallsDoNotGrowWithFolders() {
        for (int i = 0; i < 40; ++i) { queries_ << fq(QString("/a/F%1").arg(i)); folders_.insert(QString("/a/F%1").arg(i), i); }
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        QCOMPARE(byMaildirCalls_, 1);
        QCOMPARE(singleCalls_, 1);
        QCOMPARE(published_.first().value(fq("/a/F7")), 7);
        QCOMPARE(published_.first().size(), 4 + 40);
    }
    // Molte richieste ravvicinate (debounce) = un solo giro.
    void testDebounceCoalescesBursts() {
        for (int i = 0; i < 20; ++i) c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        QTest::qWait(100);
        QCOMPARE(published_.size(), 1);
        QCOMPARE(byMaildirCalls_, 1);
    }
    // Refresh durante un giro: nessun giro in parallelo; ne segue UNO solo, con i dati nuovi,
    // e nel frattempo il risultato precedente non viene toccato.
    void testOverlappingRefreshRunsOneFollowUp() {
        delay_ = 80;
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return byMaildirCalls_ == 1; }, 3000)); // giro 1 in corso
        QVERIFY(c_->busy());
        folders_["/a/INBOX"] = 9; // arriva posta mentre si conta
        for (int i = 0; i < 5; ++i) { c_->schedule(); QTest::qWait(15); } // refresh sovrapposti
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 2; }, 5000));
        QTest::qWait(300);
        QCOMPARE(published_.size(), 2);          // niente giri extra
        QCOMPARE(byMaildirCalls_, 2);            // niente chiamate in parallelo
        QCOMPARE(published_.last().value(fq("/a/INBOX")), 9); // il secondo giro vede i dati nuovi
        QVERIFY(!c_->busy());
    }
    // mu fallisce: al primo giro nessun numero (mai contate); dopo un giro riuscito resta il
    // numero precedente, anche per le query singole; al giro dopo torna tutto aggiornato.
    void testByMaildirFailureKeepsPreviousNumbers() {
        failByMaildir_ = "mu: boom";
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("conteggio non lette fallito: mu: boom"));
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        QVERIFY(!published_.first().contains(fq("/a/INBOX"))); // mai contata: sconosciuta
        QCOMPARE(published_.first().value(QString(), -1), 4);  // "Non lette" e' riuscita
        QVERIFY(!c_->busy()); // il contatore non resta bloccato
        failByMaildir_.clear();
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 2; }, 3000));
        QCOMPARE(published_.last().value(fq("/a/INBOX")), 3);
        // ora un fallimento non cancella i numeri
        folders_["/a/INBOX"] = 99; failByMaildir_ = "mu: di nuovo";
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("conteggio non lette fallito: mu: di nuovo"));
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 3; }, 3000));
        QCOMPARE(published_.last().value(fq("/a/INBOX")), 3);  // il vecchio, non 99 e non sparito
        QCOMPARE(published_.last().value(fq("/a/Sent")), 1);
        QCOMPARE(published_.last().value(fq("/a/Vuota")), 0);
        failByMaildir_.clear();
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 4; }, 3000));
        QCOMPARE(published_.last().value(fq("/a/INBOX")), 99);
    }
    void testSingleFailureKeepsPreviousNumber() {
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        QCOMPARE(published_.last().value(QString()), 4);
        singles_[QString()] = 7; failSingle_ = "mu: giu";
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("conteggio non lette fallito per .*mu: giu"));
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 2; }, 3000));
        QCOMPARE(published_.last().value(QString()), 4);
        QCOMPARE(published_.last().value(fq("/a/INBOX")), 3);
    }
    // Una cartella che sparisce dall'elenco non lascia il suo vecchio numero.
    void testRemovedQueryDropsPreviousNumber() {
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        queries_ = {QString(), fq("/a/INBOX")};
        failByMaildir_ = "mu: boom";
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("conteggio non lette fallito"));
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 2; }, 3000));
        QVERIFY(!published_.last().contains(fq("/a/Sent")));
        QCOMPARE(published_.last().value(fq("/a/INBOX")), 3);
    }
    // Query che non sono una cartella: ognuna con la propria chiamata, in serie (comportamento di prima).
    void testNonFolderQueriesAreCountedOneByOne() {
        queries_ = {QString(), "flag:flagged", fq("/a/INBOX")};
        singles_.insert("flag:flagged", 2);
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        QCOMPARE(singleCalls_, 2);
        QCOMPARE(byMaildirCalls_, 1);
        QCOMPARE(published_.first().value("flag:flagged"), 2);
    }
    void testNoFoldersMeansNoFolderCall() {
        queries_ = {QString()};
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
        QCOMPARE(byMaildirCalls_, 0);
    }
    // reset(): la risposta in ritardo di un giro superato non viene pubblicata e non blocca il successivo.
    void testResetDiscardsRunInFlight() {
        delay_ = 80;
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return byMaildirCalls_ == 1; }, 3000));
        c_->reset();
        QVERIFY(!c_->busy());
        QTest::qWait(250);
        QVERIFY(published_.isEmpty());
        delay_ = 0;
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return published_.size() == 1; }, 3000));
    }
    void testInactiveDoesNothing() {
        c_->setActive(false);
        c_->schedule();
        QTest::qWait(100);
        QVERIFY(published_.isEmpty());
        QCOMPARE(byMaildirCalls_, 0);
    }
    // Il contatore puo' sparire mentre mu sta ancora rispondendo.
    void testDestroyedWhileRunning() {
        delay_ = 50;
        c_->schedule();
        QVERIFY(QTest::qWaitFor([&] { return byMaildirCalls_ == 1; }, 3000));
        delete c_; c_ = nullptr;
        QTest::qWait(150); // le risposte tardive non devono toccare memoria liberata
    }

    // --- funzioni pure usate dal contatore ---
    void testMaildirOfQueryInvertsMaildirQuery() {
        for (const QString& f : {QString("INBOX"), QString("Sub/Cartella con spazi"), QString("[Gmail]/Tutti i messaggi"),
                               QString("virg\"olette"), QString("back\\slash")}) {
            const QString q = MailIndex::maildirQuery("/home/u/.Mail", "/home/u/.Mail/acc", f);
            QVERIFY(!q.isEmpty());
            QCOMPARE(MailIndex::maildirOfQuery(q), "/acc/" + f);
        }
        QCOMPARE(MailIndex::maildirOfQuery(QString()), QString());
        QCOMPARE(MailIndex::maildirOfQuery("flag:unread"), QString());
        QCOMPARE(MailIndex::maildirOfQuery("maildir:\"/a\" AND flag:unread"), QString());
        QCOMPARE(MailIndex::maildirOfQuery("maildir:\"/a\"b\""), QString());
        QCOMPARE(MailIndex::maildirOfQuery("maildir:\""), QString());
    }
    void testParseUnreadByMaildir() {
        const QByteArray out =
            "id1@x /a/INBOX\n"
            "id2@x /a/INBOX\n"
            "id1@x /a/INBOX\n"                       // stesso Message-ID nella stessa cartella: una volta sola
            "id1@x /a/Tutti i messaggi\n"            // stesso Message-ID in un'altra cartella: conta
            " /a/Senza id\n"                         // senza Message-ID: sempre contato
            " /a/Senza id\n"
            "\n"
            "riga-malformata\n";
        const auto r = MailIndex::parseUnreadByMaildir(out);
        QCOMPARE(r.value("/a/INBOX"), 2);
        QCOMPARE(r.value("/a/Tutti i messaggi"), 1);
        QCOMPARE(r.value("/a/Senza id"), 2);
        QCOMPARE(r.size(), 3);
        QVERIFY(MailIndex::parseUnreadByMaildir(QByteArray()).isEmpty());
    }
};

QTEST_MAIN(T)
#include "test_unreadcounter.moc"
