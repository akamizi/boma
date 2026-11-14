#include "core/MailWatcher.h"
#include <QDir>
#include <QFileInfo>

namespace boma {

namespace {
void collect(const QString& dir, int depth, QStringList& out) {
    if (depth > 8) return;
    const QDir d(dir);
    if (d.exists(QStringLiteral("new")) && d.exists(QStringLiteral("cur"))) out << d.filePath(QStringLiteral("new"));
    for (const QString& sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (sub == QLatin1String("new") || sub == QLatin1String("cur") || sub == QLatin1String("tmp")) continue;
        collect(d.filePath(sub), depth + 1, out);
    }
}
} // namespace

MailWatcher::MailWatcher(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    m_timer.setInterval(4000); // la posta arriva a gruppi: si attende che si plachi
    connect(&m_fsw, &QFileSystemWatcher::directoryChanged, this, [this]() { m_timer.start(); });
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        rescan(); // cartelle nuove create nel frattempo
        emit newMail();
    });
}

QStringList MailWatcher::findNewDirs(const QString& root) {
    QStringList out;
    if (!root.isEmpty()) collect(QDir::cleanPath(root), 0, out);
    return out;
}

void MailWatcher::setRoots(const QStringList& roots) {
    m_roots = roots;
    rescan();
}

void MailWatcher::rescan() {
    QStringList want;
    for (const QString& r : m_roots) want << findNewDirs(r);
    const QStringList have = m_fsw.directories();
    QStringList stale;
    for (const QString& h : have)
        if (!want.contains(h)) stale << h;
    if (!stale.isEmpty()) m_fsw.removePaths(stale);
    QStringList add;
    for (const QString& w : want)
        if (!have.contains(w)) add << w;
    if (!add.isEmpty()) m_fsw.addPaths(add);
}

} // namespace boma
