#pragma once
#include <QString>
#include <functional>

namespace boma {

// Notifica del desktop (org.freedesktop.Notifications via D-Bus). Se il servizio non c'e'
// la notifica e' solo un comodo, mai un requisito: non si blocca nulla, ma l'esito
// negativo lascia un qWarning (quello di "servizio assente" una volta sola).
class Notifier {
public:
    // done("") = consegnata al servizio, altrimenti il motivo dell'insuccesso.
    using Transport = std::function<void(const QString& title, const QString& escapedBody, int timeoutMs,
                                         std::function<void(const QString& error)> done)>;

    static void show(const QString& title, const QString& body);
    // Il corpo ammette markup: testo che arriva da fuori (mittente, oggetto) va neutralizzato.
    static QString escapeBody(const QString& body);
    // Per i test: sostituisce l'invio D-Bus (vuoto = D-Bus).
    static void setTransport(Transport t);
};

} // namespace boma
