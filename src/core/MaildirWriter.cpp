#include "core/MaildirWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QDateTime>
#include <QCoreApplication>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace boma {

namespace {
void setError(QString* e, const QString& m) { if (e) *e = m; }

QString errnoText() { return QString::fromLocal8Bit(std::strerror(errno)); }

void fsyncDir(const QByteArray& dir)
{
    const int fd = ::open(dir.constData(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

std::function<QString()>& generator()
{
    static std::function<QString()> g;
    return g;
}

QString uniqueBase()
{
    if (generator())
        return generator()();
    static quint64 counter = 0;
    char hn[256] = {};
    ::gethostname(hn, sizeof hn - 1);
    QString host = QString::fromLocal8Bit(hn);
    host.replace(QLatin1Char('/'), QStringLiteral("\\057")).replace(QLatin1Char(':'), QStringLiteral("\\072"));
    if (host.isEmpty())
        host = QStringLiteral("localhost");
    return QStringLiteral("%1.%2_%3_%4.%5")
        .arg(QDateTime::currentSecsSinceEpoch())
        .arg(::getpid())
        .arg(counter++)
        .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'))
        .arg(host);
}
} // namespace

QString MaildirWriter::checkFolder(const QString& maildirFolder)
{
    if (maildirFolder.isEmpty())
        return QStringLiteral("Cartella Maildir non configurata");
    for (const char* sub : {"tmp", "new", "cur"}) {
        const QString d = maildirFolder + QLatin1Char('/') + QLatin1String(sub);
        if (!QFileInfo(d).isDir())
            return QStringLiteral("Cartella Maildir non valida: manca %1").arg(d);
        if (qstrcmp(sub, "new") != 0 && ::access(QFile::encodeName(d).constData(), W_OK | X_OK) != 0)
            return QStringLiteral("Cartella Maildir non scrivibile: %1 (%2)").arg(d, errnoText());
    }
    return {};
}

QString MaildirWriter::saveFallback(const QString& dir, const QByteArray& message, QString* error)
{
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        setError(error, QStringLiteral("Cartella di emergenza non creabile: %1").arg(dir));
        return {};
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    for (int attempt = 0; attempt < 5; ++attempt) {
        const QString path = dir + QLatin1Char('/') + uniqueBase() + QStringLiteral(".eml");
        const QByteArray p = QFile::encodeName(path);
        const int fd = ::open(p.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            setError(error, QStringLiteral("Impossibile creare %1: %2").arg(path, errnoText()));
            return {};
        }
        qint64 off = 0;
        bool ok = true;
        while (off < message.size()) {
            const ssize_t w = ::write(fd, message.constData() + off, size_t(message.size() - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            off += w;
        }
        if (ok && ::fsync(fd) != 0)
            ok = false;
        const QString why = errnoText();
        ::close(fd);
        if (!ok) {
            ::unlink(p.constData());
            setError(error, QStringLiteral("Scrittura di %1 non riuscita: %2").arg(path, why));
            return {};
        }
        fsyncDir(QFile::encodeName(dir));
        return path;
    }
    setError(error, QStringLiteral("Nome unico non disponibile in %1").arg(dir));
    return {};
}

void MaildirWriter::setNameGeneratorForTest(std::function<QString()> gen) { generator() = std::move(gen); }

QString MaildirWriter::deliver(const QString& maildirFolder, const QByteArray& message,
                               QString flags, QString* error)
{
    const QString tmpDir = maildirFolder + QStringLiteral("/tmp");
    const QString curDir = maildirFolder + QStringLiteral("/cur");
    if (const QString why = checkFolder(maildirFolder); !why.isEmpty()) {
        setError(error, why);
        return {};
    }

    for (const QChar c : std::as_const(flags)) {
        const ushort u = c.unicode();
        if (!((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z'))) {
            setError(error, QStringLiteral("Bandiere Maildir non valide: %1").arg(flags));
            return {};
        }
    }
    std::sort(flags.begin(), flags.end());
    flags.erase(std::unique(flags.begin(), flags.end()), flags.end());
    const QString suffix = QStringLiteral(":2,") + flags;

    // File unico in tmp/ (O_EXCL: non sovrascrive mai).
    QString base, tmpPath;
    int fd = -1;
    for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
        base = uniqueBase();
        tmpPath = tmpDir + QLatin1Char('/') + base;
        fd = ::open(QFile::encodeName(tmpPath).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) {
            setError(error, QStringLiteral("Creazione file in tmp fallita: %1").arg(errnoText()));
            return {};
        }
    }
    if (fd < 0) {
        setError(error, QStringLiteral("Impossibile ottenere un nome unico in tmp"));
        return {};
    }
    const QByteArray tmpEnc = QFile::encodeName(tmpPath);

    auto fail = [&](const QString& msg) {
        ::unlink(tmpEnc.constData()); // solo il nostro file temporaneo
        setError(error, msg);
        return QString();
    };

    const char* p = message.constData();
    qint64 left = message.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, size_t(left));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            const QString m = errnoText();
            ::close(fd);
            return fail(QStringLiteral("Scrittura fallita: %1").arg(m));
        }
        p += n;
        left -= n;
    }
    if (::fsync(fd) != 0) {
        const QString m = errnoText();
        ::close(fd);
        return fail(QStringLiteral("fsync fallito: %1").arg(m));
    }
    if (::close(fd) != 0)
        return fail(QStringLiteral("Chiusura fallita: %1").arg(errnoText()));

    // link() fallisce con EEXIST invece di sovrascrivere: in tal caso nuovo nome.
    QString finalPath;
    for (int attempt = 0; attempt < 100; ++attempt) {
        finalPath = curDir + QLatin1Char('/') + base + suffix;
        if (::link(tmpEnc.constData(), QFile::encodeName(finalPath).constData()) == 0)
            break;
        if (errno != EEXIST)
            return fail(QStringLiteral("Consegna in cur fallita: %1").arg(errnoText()));
        finalPath.clear();
        base = uniqueBase();
    }
    if (finalPath.isEmpty())
        return fail(QStringLiteral("Impossibile ottenere un nome unico in cur"));

    ::unlink(tmpEnc.constData());
    fsyncDir(QFile::encodeName(curDir));
    fsyncDir(QFile::encodeName(tmpDir));
    return finalPath;
}

} // namespace boma
