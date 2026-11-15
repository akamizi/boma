#include "core/SendController.h"
#include <QMetaObject>

namespace boma {

SendController::State& SendController::stateFor(QObject* window) {
    auto it = state_.find(window);
    if (it == state_.end()) {
        it = state_.insert(window, State{});
        // lo stato segue la vita della finestra
        connect(window, &QObject::destroyed, this, [this](QObject* o) { state_.remove(o); });
    }
    return it.value();
}

void SendController::start(const Surface& s, const OutgoingMessage& msg, bool sign, bool encrypt, bool allowUnsigned) {
    QObject* w = s.window.data();
    if (!w) return;
    State& st = stateFor(w);
    if (st.sending || st.sent) return; // mai un secondo invio
    st.sending = true;
    if (s.setSendEnabled) s.setSendEnabled(false);
    if (s.setStatus) s.setStatus(tr("Invio in corso..."));
    emit statusMessage(tr("Invio in corso..."), 0);
    QPointer<SendController> self(this);
    send_(
        msg, sign, encrypt,
        [self, s, msg, sign, encrypt](SendResult r) {
            if (!self) return;
            self->onResult(s, msg, sign, encrypt, r);
        },
        allowUnsigned);
}

void SendController::onResult(const Surface& s, const OutgoingMessage& msg, bool sign, bool encrypt, const SendResult& r) {
    const bool keepLocked = r.kind == SendResult::NeedsConfirmationUnsigned;
    // Se la finestra di scrittura e' stata chiusa durante l'invio il messaggio non e'
    // piu' "nella finestra": il testo dell'avviso lo dice.
    const QString closedNote = tr("\n\nLa finestra di scrittura e' stata chiusa durante l'invio: il messaggio non e' piu' disponibile qui.");
    const bool alive = !s.window.isNull();
    if (alive && !keepLocked) {
        state_[s.window.data()].sending = false;
        if (r.kind != SendResult::Sent) {
            if (s.setSendEnabled) s.setSendEnabled(true);
            if (s.setStatus) s.setStatus(QString());
        }
    }
    switch (r.kind) {
    case SendResult::Sent:
        emit statusMessage(r.message.isEmpty() ? tr("Inviato") : tr("Inviato (avviso: %1)").arg(r.message), 15000);
        if (!r.message.isEmpty()) emit notice(tr("Inviato, con un avviso"), r.message);
        if (alive && r.copyFailed) {
            // I4: il messaggio E' partito ma la copia in Inviata manca: la finestra resta
            // aperta (il testo non va perso) ma segnata come inviata, mai reinviabile.
            state_[s.window.data()].sent = true;
            if (s.setSendEnabled) s.setSendEnabled(false);
            if (s.setStatus)
                s.setStatus(r.fallbackPath.isEmpty()
                                ? tr("INVIATO. Copia in Inviata non salvata: copia il testo prima di chiudere.")
                                : tr("INVIATO. Copia in Inviata non salvata; copia di emergenza: %1").arg(r.fallbackPath));
            // Se anche la copia di emergenza e' fallita il testo esiste solo qui: la
            // finestra resta "modificata", cosi' chiuderla chiede conferma.
            if (!r.fallbackPath.isEmpty() && s.markClean) s.markClean();
        } else if (alive) {
            if (s.markClean) s.markClean();
            if (s.close) s.close();
        }
        break;
    case SendResult::NeedsConfirmationUnsigned:
        emit statusMessage(QString(), 0);
        // Il Sender resta "occupato" fino al ritorno di questo callback: domanda ed
        // eventuale nuovo invio (allowUnsigned) partono subito dopo, dal ciclo eventi.
        // La finestra di scrittura resta bloccata (nessun invio doppio) fino alla risposta.
        QMetaObject::invokeMethod(
            this,
            [this, s, msg, sign, encrypt]() {
                const bool yes = confirmUnsigned_ ? confirmUnsigned_() : false;
                if (!s.window.isNull()) state_[s.window.data()].sending = false;
                if (yes) {
                    start(s, msg, sign, encrypt, true);
                } else {
                    if (!s.window.isNull()) {
                        if (s.setSendEnabled) s.setSendEnabled(true);
                        if (s.setStatus) s.setStatus(QString());
                    }
                    emit statusMessage(tr("Invio annullato: nulla e' stato inviato"), 8000);
                }
            },
            Qt::QueuedConnection);
        break;
    case SendResult::MissingKeys:
        emit statusMessage(QString(), 0);
        emit notice(tr("Chiavi mancanti"),
                    tr("Nulla e' stato inviato: manca una chiave pubblica utilizzabile per:\n\n%1").arg(r.missing.join(QLatin1Char('\n'))) +
                        (alive ? QString() : closedNote));
        break;
    case SendResult::SmtpFailed:
        emit statusMessage(QString(), 0);
        emit notice(tr("Invio non riuscito"),
                    r.message + (alive ? tr("\n\nIl messaggio resta nella finestra di scrittura e non viene ripetuto automaticamente. "
                                            "Se l'esito e' incerto controlla la cartella Inviata prima di riprovare.")
                                       : tr("\n\nNon viene ripetuto automaticamente. Se l'esito e' incerto controlla la cartella Inviata.") + closedNote));
        break;
    case SendResult::SignFailed:
    case SendResult::EncryptFailed:
    case SendResult::BuildFailed:
    case SendResult::Busy:
        emit statusMessage(QString(), 0);
        emit notice(r.kind == SendResult::Busy ? tr("Invio in corso") : tr("Invio non riuscito"),
                    r.kind == SendResult::Busy ? tr("Un altro invio e' in corso: riprova tra poco. Nulla e' stato inviato.")
                                               : r.message + (alive ? tr("\n\nNulla e' stato inviato; il messaggio resta nella finestra di scrittura.")
                                                                    : tr("\n\nNulla e' stato inviato.") + closedNote));
        break;
    }
}

} // namespace boma
