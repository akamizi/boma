#include "core/FolderCleaner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace boma {

QString FolderCleaner::validate(const QString& folderDir, const QString& accountRoot) {
    if (folderDir.isEmpty() || accountRoot.isEmpty()) return QStringLiteral("Percorso non definito.");
    const QString root = QFileInfo(accountRoot).canonicalFilePath();
    const QString dir = QFileInfo(folderDir).canonicalFilePath();
    if (root.isEmpty() || dir.isEmpty()) return QStringLiteral("La cartella non esiste.");
    if (dir == root || !dir.startsWith(root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/')))
        return QStringLiteral("La cartella non e' sotto la Maildir dell'account.");
    if (!QFileInfo(dir + QStringLiteral("/cur")).isDir()) return QStringLiteral("Non e' una cartella Maildir (manca cur/).");
    return {};
}

static QStringList files(const QString& dir) {
    QStringList out;
    for (const char* sub : {"cur", "new"}) {
        const QDir d(dir + QLatin1Char('/') + QLatin1String(sub));
        for (const QFileInfo& fi : d.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot))
            out << fi.absoluteFilePath();
    }
    return out;
}

int FolderCleaner::count(const QString& folderDir) { return int(files(folderDir).size()); }

int FolderCleaner::removeAll(const QString& folderDir, const QString& accountRoot, int* failed, QString* error) {
    if (failed) *failed = 0;
    const QString why = validate(folderDir, accountRoot);
    if (!why.isEmpty()) {
        if (error) *error = why;
        return 0;
    }
    int removed = 0, bad = 0;
    for (const QString& f : files(QFileInfo(folderDir).canonicalFilePath())) {
        if (QFile::remove(f)) ++removed;
        else ++bad;
    }
    if (failed) *failed = bad;
    if (bad && error) *error = QStringLiteral("%1 file non rimossi.").arg(bad);
    return removed;
}

} // namespace boma
