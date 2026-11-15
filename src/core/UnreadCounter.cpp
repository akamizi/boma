#include "core/UnreadCounter.h"
#include <QDebug>
#include <QPointer>
#include <memory>

namespace boma {

UnreadCounter::UnreadCounter(QObject* parent) : QObject(parent), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    timer_->setInterval(300);
    connect(timer_, &QTimer::timeout, this, &UnreadCounter::run);
}

void UnreadCounter::schedule() {
    if (!active_) return;
    timer_->start();
}

void UnreadCounter::reset() {
    timer_->stop();
    ++gen_; // le risposte del giro in corso diventano orfane e vengono ignorate
    running_ = false;
    dirty_ = false;
}

void UnreadCounter::run() {
    if (!active_ || !queries_) return;
    if (running_) { // un giro e' in corso: ne segue un altro con i dati nuovi
        dirty_ = true;
        return;
    }
    running_ = true;
    dirty_ = false;
    const quint64 gen = ++gen_;

    // Cartelle (raggruppate) e altre query (una alla volta).
    QHash<QString, QString> folderOf; // query -> cartella
    QStringList others;
    current_ = queries_();
    for (const QString& q : current_) {
        const QString f = maildirOf_ ? maildirOf_(q) : QString();
        if (f.isEmpty() || !byMaildir_) others << q; else folderOf.insert(q, f);
    }

    auto results = std::make_shared<QHash<QString, int>>();
    QPointer<UnreadCounter> self(this);

    // Seconda fase: le query singole, in serie.
    auto step = std::make_shared<std::function<void(int)>>();
    *step = [self, gen, others, results, step](int i) {
        if (!self || gen != self->gen_) return;
        if (i >= others.size() || !self->single_) {
            self->finish(gen, *results);
            return;
        }
        const QString q = others.at(i);
        self->single_(q, [self, gen, q, results, step, i](int n, QString err) {
            if (!self || gen != self->gen_) return;
            if (n >= 0) results->insert(q, n);
            else qWarning().noquote() << "conteggio non lette fallito per" << q << ":" << err;
            (*step)(i + 1);
        });
    };

    if (folderOf.isEmpty()) {
        (*step)(0);
        return;
    }
    byMaildir_([self, gen, folderOf, results, step](QHash<QString, int> byFolder, QString err) {
        if (!self || gen != self->gen_) return;
        if (!err.isEmpty()) {
            // nessun numero per le cartelle in questo giro (restano "sconosciute")
            qWarning().noquote() << "conteggio non lette fallito:" << err;
        } else {
            for (auto it = folderOf.cbegin(); it != folderOf.cend(); ++it)
                results->insert(it.key(), byFolder.value(it.value(), 0));
        }
        (*step)(0);
    });
}

void UnreadCounter::finish(quint64 gen, const QHash<QString, int>& results) {
    if (gen != gen_) return;
    running_ = false;
    // Se mu ha fallito per una query, resta l'ultimo numero noto (meglio vecchio di qualche
    // secondo che sparito); non si inventa nulla per le query mai contate.
    QHash<QString, int> merged = results;
    for (const QString& q : current_)
        if (!merged.contains(q) && last_.contains(q)) merged.insert(q, last_.value(q));
    last_ = merged;
    emit countsReady(merged);
    if (dirty_) schedule();
}

} // namespace boma
