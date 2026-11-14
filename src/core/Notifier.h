#pragma once
#include <QString>

namespace boma {

// Notifica del desktop (org.freedesktop.Notifications via D-Bus). Se il servizio non c'e'
// non succede nulla: la notifica e' solo un comodo, mai un requisito.
class Notifier {
public:
    static void show(const QString& title, const QString& body);
};

} // namespace boma
