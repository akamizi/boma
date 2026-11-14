#include "core/Notifier.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QVariantMap>

namespace boma {

void Notifier::show(const QString& title, const QString& body) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    QDBusInterface n(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                     QStringLiteral("org.freedesktop.Notifications"), bus);
    if (!n.isValid()) return;
    // il testo del corpo puo' contenere markup: il mittente/oggetto arrivano da fuori, quindi si neutralizza
    QString safe = body;
    safe.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;"));
    n.asyncCall(QStringLiteral("Notify"), QStringLiteral("Boma"), uint(0), QStringLiteral("mail-unread"), title, safe,
                QStringList(), QVariantMap(), int(8000));
}

} // namespace boma
