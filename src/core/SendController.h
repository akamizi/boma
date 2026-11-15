#pragma once
#include "mime/MessageBuilder.h"
#include "send/Sender.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>

namespace boma {

// Flusso di invio dal punto di vista della finestra di scrittura: blocca la finestra durante
// l'invio, interpreta l'esito di Sender e decide che cosa mostrare e se chiudere la finestra.
// Non conosce widget: la finestra e' vista attraverso `Surface` (funzioni), cosi' e' testabile
// senza GUI. La consegna vera (msmtp, copia in Inviata) e' di Sender; qui si garantisce che:
// - un messaggio non parta due volte dalla stessa finestra (mentre e' in corso, e mai piu'
//   dopo un invio riuscito);
// - dopo un errore la finestra torni utilizzabile e il testo resti li' (non si chiude mai);
// - la bozza di partenza si tolga solo con la copia in Inviata al sicuro;
// - la finestra si chiuda da sola solo se il messaggio e' partito E la copia in Inviata c'e';
//   se la copia manca la finestra resta aperta, segnata come inviata e non reinviabile.
//
// Uscite verso la finestra principale: statusMessage() (testo vuoto = cancella) e notice().
class SendController : public QObject {
    Q_OBJECT
public:
    // La finestra di scrittura. `window` identifica la finestra e ne segue la vita (nullo =
    // chiusa: nulla viene piu' chiamato su di essa). Le funzioni mancanti sono ignorate.
    struct Surface {
        QPointer<QObject> window;
        std::function<void(bool)> setSendEnabled;
        std::function<void(const QString&)> setStatus; // etichetta di stato; vuoto = svuota
        std::function<void()> markClean;               // il testo non e' piu' "da salvare"
        std::function<void()> close;
        // Rimuove la bozza di partenza. Chiamata SOLO quando il messaggio e' partito E la copia
        // in Inviata e' stata scritta (mai se copyFailed), anche se la finestra e' gia' chiusa.
        std::function<void()> discardDraft;
        // Risorse che il messaggio da inviare usa ancora (es. la cartella temporanea con gli
        // allegati di una bozza riaperta). La Surface e' copiata in ogni passo pendente
        // (invio in corso, domanda "senza firma?", nuovo tentativo): finche' ne esiste uno la
        // risorsa vive anche se la finestra e' stata chiusa, e si libera quando l'ultimo
        // tentativo si risolve (inviato, fallito o annullato).
        std::shared_ptr<void> keepAlive;
    };
    // Invio effettivo (di solito Sender::send).
    using SendFn = std::function<void(const OutgoingMessage&, bool sign, bool encrypt,
                                      std::function<void(SendResult)> done, bool allowUnsigned)>;

    explicit SendController(SendFn send, QObject* parent = nullptr) : QObject(parent), send_(std::move(send)) {}
    // Domanda "inviare senza firma?" (identita' senza chiave PGP). Senza funzione: no.
    void setConfirmUnsigned(std::function<bool()> f) { confirmUnsigned_ = std::move(f); }

    // Avvia l'invio dalla finestra. Ignorato se la finestra e' chiusa, se un invio da
    // questa finestra e' in corso o se e' gia' stato inviato.
    void start(const Surface& surface, const OutgoingMessage& msg, bool sign, bool encrypt, bool allowUnsigned = false);

    bool isSending(const QObject* window) const { return state_.value(window).sending; }
    bool isSent(const QObject* window) const { return state_.value(window).sent; }

signals:
    void statusMessage(const QString& text, int timeoutMs); // text vuoto = cancella il messaggio
    void notice(const QString& title, const QString& text);

private:
    struct State { bool sending = false, sent = false; };
    void onResult(const Surface& s, const OutgoingMessage& msg, bool sign, bool encrypt, const SendResult& r);
    State& stateFor(QObject* window);

    SendFn send_;
    std::function<bool()> confirmUnsigned_;
    QHash<const QObject*, State> state_;
};

} // namespace boma
