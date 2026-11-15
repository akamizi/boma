#include "core/Notifier.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QVariantMap>

namespace boma {

namespace {
Notifier::Transport& transport() {
    static Notifier::Transport t;
    return t;
}

void sendDBus(const QString& title, const QString& body, int timeoutMs, const std::function<void(const QString&)>& done) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return done(QStringLiteral("bus di sessione D-Bus non disponibile"));
    QDBusInterface n(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                     QStringLiteral("org.freedesktop.Notifications"), bus);
    if (!n.isValid()) return done(QStringLiteral("servizio di notifiche assente"));
    QDBusPendingCall call = n.asyncCall(QStringLiteral("Notify"), QStringLiteral("Boma"), uint(0),
                                        QStringLiteral("mail-unread"), title, body, QStringList(), QVariantMap(), timeoutMs);
    auto* w = new QDBusPendingCallWatcher(call);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, [w, done](QDBusPendingCallWatcher*) {
        QDBusPendingReply<uint> reply = *w;
        done(reply.isError() ? reply.error().message() : QString());
        w->deleteLater();
    });
}
} // namespace

QString Notifier::escapeBody(const QString& body) { return body.toHtmlEscaped(); }

void Notifier::setTransport(Transport t) { transport() = std::move(t); }

void Notifier::show(const QString& title, const QString& body) {
    auto done = [](const QString& error) {
        static bool warnedUnavailable = false;
        if (error.isEmpty()) return;
        // il servizio assente e' normale su molti desktop: se ne avvisa una volta sola
        const bool unavailable = error.contains(QLatin1String("assente")) || error.contains(QLatin1String("non disponibile"));
        if (unavailable) {
            if (warnedUnavailable) return;
            warnedUnavailable = true;
        }
        qWarning().noquote() << "Notifica del desktop non riuscita:" << error;
    };
    const QString safe = escapeBody(body);
    if (transport()) return transport()(title, safe, 8000, done);
    sendDBus(title, safe, 8000, done);
}

} // namespace boma
