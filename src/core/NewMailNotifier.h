#pragma once
#include "core/MailIndex.h"
#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

namespace boma {

// Decide quali messaggi non letti sono "nuovi" e li notifica.
//
// Invece di ricordare ogni Message-ID visto (insieme che cresce senza limite) tiene un
// *watermark*: la data piu' recente gia' notificata e i Message-ID con esattamente quella
// data. Si chiedono a mu solo le non lette con data >= watermark, dalla piu' vecchia: il
// numero di messaggi per giro e' limitato (kBatch) senza perdere nulla, perche' i restanti
// escono al giro dopo. Un messaggio che torna "non letto" dopo essere stato letto ha una
// data piu' vecchia del watermark e non viene notificato di nuovo.
//
// La prima lettura riuscita fissa solo il punto di partenza (nessuna notifica per le non
// lette gia' presenti). Se mu fallisce il watermark non si muove, quindi al giro successivo
// nulla e' perso; dopo un errore si riprova da soli (kMaxRetries volte).
//
// Limite noto: la data e' quella dell'intestazione Date del messaggio, non quella di arrivo:
// un messaggio che arriva con ritardo e con una data anteriore al watermark non e' notificato.
class NewMailNotifier : public QObject {
    Q_OBJECT
public:
    using Fetch = std::function<void(const QDateTime& since, int limit, SearchCallback cb)>;
    using Newest = std::function<void(SearchCallback cb)>;
    using Show = std::function<void(const QString& title, const QString& body)>;

    static constexpr int kBatch = 200;
    static constexpr int kMaxRetries = 3;

    explicit NewMailNotifier(QObject* parent = nullptr);
    void setFetch(Fetch f) { fetch_ = std::move(f); }
    void setNewest(Newest f) { newest_ = std::move(f); }
    void setShow(Show s) { show_ = std::move(s); }
    // Se restituisce false il watermark avanza comunque ma non si mostra nulla.
    void setEnabled(std::function<bool()> e) { enabled_ = std::move(e); }
    void setRetryInterval(int ms) { retry_.setInterval(ms); }

    bool initialized() const { return init_; }
    QDateTime watermark() const { return mark_; }
    int watermarkIds() const { return int(ids_.size()); }

public slots:
    // Da chiamare quando l'indice e' cambiato (e una volta all'avvio per il punto di partenza).
    void check();

private:
    void onBaseline(const QList<MessageSummary>& list, const QString& err);
    void onBatch(const QList<MessageSummary>& list, const QString& err);
    void failed(const QString& err);
    void finish();
    static QString keyOf(const MessageSummary& m) { return m.messageId.isEmpty() ? m.path : m.messageId; }

    Fetch fetch_;
    Newest newest_;
    Show show_;
    std::function<bool()> enabled_;
    QDateTime mark_;
    QSet<QString> ids_;
    bool init_ = false, busy_ = false, again_ = false;
    bool absorbing_ = false; // il prossimo lotto non notifica i messaggi con la data del punto di partenza
    int retries_ = 0;
    QTimer retry_;
};

} // namespace boma
