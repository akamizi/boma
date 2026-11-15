#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>

namespace boma {

// Conteggio dei messaggi non letti per cartella, mostrato nel pannello delle cartelle.
//
// - schedule() raggruppa le richieste (debounce): molte chiamate ravvicinate = un solo giro.
// - Un solo giro alla volta: se ne arriva una durante un giro, ne segue un altro con i dati
//   nuovi (coalescenza); mentre il giro e' in corso i numeri precedenti restano visibili.
// - Le query `maildir:"..."` si contano TUTTE con una sola chiamata a mu (byMaildir), la cui
//   durata non dipende dal numero di cartelle; ogni altra query (per esempio quella vuota
//   della vista "Non lette", che ha esclusioni e dedup proprie) si conta con una chiamata a
//   parte (single), una alla volta.
// - Il risultato di un giro e' pubblicato in un colpo solo (countsReady). Se mu fallisce per
//   una query (con un qWarning) resta il suo ultimo numero noto; una query mai contata con
//   successo non compare ("sconosciuta": nessun numero).
// - reset() scarta il giro in corso (per esempio quando cambia l'insieme delle cartelle).
class UnreadCounter : public QObject {
    Q_OBJECT
public:
    // Una chiamata a mu per tutte le cartelle: cartella ("/a/INBOX") -> non lette.
    using ByMaildir = std::function<void(std::function<void(QHash<QString, int>, QString)> done)>;
    // Una chiamata a mu per una query: cb(-1, errore) se fallisce.
    using Single = std::function<void(const QString& query, std::function<void(int, QString)> done)>;
    // Inverso di MailIndex::maildirQuery (vuota = non e' una query per cartella).
    using MaildirOf = std::function<QString(const QString& query)>;

    explicit UnreadCounter(QObject* parent = nullptr);
    void setByMaildir(ByMaildir f) { byMaildir_ = std::move(f); }
    void setSingle(Single f) { single_ = std::move(f); }
    void setMaildirOf(MaildirOf f) { maildirOf_ = std::move(f); }
    // Le query da contare, lette all'inizio di ogni giro (l'elenco delle cartelle puo' cambiare).
    void setQueries(std::function<QStringList()> f) { queries_ = std::move(f); }
    // Falso = schede inattive: schedule() non fa nulla (nessuna configurazione).
    void setActive(bool on) { active_ = on; }
    void setDebounce(int ms) { timer_->setInterval(ms); }

    void schedule();
    void reset();
    bool busy() const { return running_; }

signals:
    // query -> conteggio, per le sole query contate con successo.
    void countsReady(const QHash<QString, int>& byQuery);

private:
    void run();
    void finish(quint64 gen, const QHash<QString, int>& results);
    QTimer* timer_;
    ByMaildir byMaildir_;
    Single single_;
    MaildirOf maildirOf_;
    std::function<QStringList()> queries_;
    bool active_ = true, running_ = false, dirty_ = false;
    quint64 gen_ = 0;
    QStringList current_;        // query del giro in corso
    QHash<QString, int> last_;   // ultimo risultato pubblicato
};

} // namespace boma
