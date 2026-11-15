#include "core/NewMailNotifier.h"
#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <algorithm>

namespace boma {

NewMailNotifier::NewMailNotifier(QObject* parent) : QObject(parent) {
    retry_.setSingleShot(true);
    retry_.setInterval(30000);
    connect(&retry_, &QTimer::timeout, this, &NewMailNotifier::check);
}

void NewMailNotifier::check() {
    if (busy_) { // un giro e' in corso: ne segue un altro con i dati nuovi
        again_ = true;
        return;
    }
    if (!fetch_ || !newest_) return;
    retry_.stop();
    busy_ = true;
    QPointer<NewMailNotifier> self(this);
    if (!init_) {
        newest_([self](QList<MessageSummary> l, QString e) {
            if (self) self->onBaseline(l, e);
        });
    } else {
        fetch_(mark_, kBatch, [self](QList<MessageSummary> l, QString e) {
            if (self) self->onBatch(l, e);
        });
    }
}

void NewMailNotifier::failed(const QString& err) {
    qWarning().noquote() << "Controllo della posta nuova non riuscito:" << err;
    // il watermark non si e' mosso: il prossimo giro recupera quanto e' arrivato nel frattempo
    if (retries_ < kMaxRetries) {
        ++retries_;
        retry_.start();
    }
    busy_ = false;
}

void NewMailNotifier::finish() {
    busy_ = false;
    if (again_) {
        again_ = false;
        check();
    }
}

void NewMailNotifier::onBaseline(const QList<MessageSummary>& list, const QString& err) {
    if (!err.isEmpty()) return failed(err);
    retries_ = 0;
    init_ = true;
    if (!list.isEmpty() && list.first().date.isValid()) {
        mark_ = list.first().date;
        ids_.clear();
        ids_.insert(keyOf(list.first()));
        // altri messaggi con la stessa data (al secondo) non sono nella lista da uno: si
        // ritroverebbero come "nuovi". Un giro subito dopo li assorbe in silenzio.
        again_ = true;
        absorbing_ = true;
    } else {
        mark_ = QDateTime(); // nessuna non letta: tutto cio' che arrivera' e' nuovo
    }
    finish();
}

void NewMailNotifier::onBatch(const QList<MessageSummary>& list, const QString& err) {
    if (!err.isEmpty()) return failed(err);
    retries_ = 0;
    const QDateTime base = mark_;
    QList<MessageSummary> fresh;
    for (const MessageSummary& m : list) {
        if (!m.date.isValid()) continue;
        if (mark_.isValid()) {
            if (m.date < mark_) continue;
            if (m.date == mark_ && ids_.contains(keyOf(m))) continue;
        }
        fresh << m;
    }
    if (!fresh.isEmpty()) {
        std::stable_sort(fresh.begin(), fresh.end(),
                         [](const MessageSummary& a, const MessageSummary& b) { return a.date < b.date; });
        const QDateTime newest = fresh.last().date;
        if (!mark_.isValid() || newest > mark_) {
            mark_ = newest;
            ids_.clear();
        }
        for (const MessageSummary& m : fresh)
            if (m.date == mark_) ids_.insert(keyOf(m));
        // un lotto pieno: ce ne sono altri oltre il limite, si prosegue subito
        if (list.size() >= kBatch) again_ = true;
        // subito dopo il punto di partenza i messaggi con la sua stessa data (al secondo) erano
        // gia' presenti: si assorbono in silenzio
        if (absorbing_)
            fresh.erase(std::remove_if(fresh.begin(), fresh.end(),
                                       [&](const MessageSummary& m) { return m.date == base; }),
                        fresh.end());
        if (!fresh.isEmpty() && show_ && (!enabled_ || enabled_())) {
            const MessageSummary& last = fresh.last();
            if (fresh.size() == 1)
                show_(last.from, last.subject);
            else
                show_(tr("%1 nuovi messaggi").arg(fresh.size()), last.from + QStringLiteral(": ") + last.subject);
        }
    }
    absorbing_ = false;
    finish();
}

} // namespace boma
