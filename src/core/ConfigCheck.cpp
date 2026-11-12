#include "core/ConfigCheck.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace boma {

static QStringList wordsAfter(const QByteArray& rc, const QByteArray& keyword) {
    QStringList out;
    for (const QByteArray& raw : rc.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith('#') || line.size() <= keyword.size()) continue;
        if (line.left(keyword.size()).toLower() != keyword) continue;
        const char sep = line.at(keyword.size());
        if (sep != ' ' && sep != '\t') continue;
        const QString name = QString::fromUtf8(line.mid(keyword.size()).trimmed());
        if (!name.isEmpty() && !out.contains(name)) out << name;
    }
    return out;
}

QStringList ConfigCheck::mbsyncChannels(const QByteArray& rc) {
    QStringList out = wordsAfter(rc, "channel");
    for (const QString& g : wordsAfter(rc, "group"))
        if (!out.contains(g)) out << g;
    return out;
}

QStringList ConfigCheck::msmtpAccounts(const QByteArray& rc) {
    QStringList out;
    for (const QString& w : wordsAfter(rc, "account")) {
        // "account nome" oppure "account nome : base": conta solo il primo nome
        QString name = w.section(QLatin1Char(':'), 0, 0).trimmed();
        if (name.isEmpty() || name == QLatin1String("default")) continue;
        if (!out.contains(name)) out << name;
    }
    return out;
}

static QString firstExisting(const QStringList& c) {
    for (const QString& p : c)
        if (QFileInfo::exists(p)) return p;
    return {};
}

QString ConfigCheck::mbsyncConfigPath() {
    const QString h = QDir::homePath();
    return firstExisting({h + QStringLiteral("/.mbsyncrc"),
                          QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/isyncrc")});
}

QString ConfigCheck::msmtpConfigPath() {
    const QString h = QDir::homePath();
    return firstExisting({h + QStringLiteral("/.msmtprc"),
                          QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/msmtp/config")});
}

QByteArray ConfigCheck::readFile(const QString& path, bool* ok) {
    QFile f(path);
    const bool o = !path.isEmpty() && f.open(QIODevice::ReadOnly);
    if (ok) *ok = o;
    return o ? f.read(4 * 1024 * 1024) : QByteArray();
}

QList<CheckIssue> ConfigCheck::checkAccount(const Account& a, const QStringList& channels, bool haveRc) {
    QList<CheckIssue> out;
    auto add = [&](CheckIssue::Level l, const QString& t) { out.append({l, t}); };
    if (!haveRc)
        add(CheckIssue::Warning, QCoreApplication::translate("ConfigCheck", "File di mbsync non trovato (~/.mbsyncrc): canale non verificabile."));
    else if (a.mbsyncChannel.trimmed().isEmpty())
        add(CheckIssue::Error, QCoreApplication::translate("ConfigCheck", "Canale mbsync non indicato."));
    else if (channels.contains(a.mbsyncChannel.trimmed()))
        add(CheckIssue::Ok, QCoreApplication::translate("ConfigCheck", "Canale mbsync \"%1\" presente.").arg(a.mbsyncChannel.trimmed()));
    else
        add(CheckIssue::Error, QCoreApplication::translate("ConfigCheck", "Il canale mbsync \"%1\" non e' definito in .mbsyncrc.").arg(a.mbsyncChannel.trimmed()));

    const QString root = a.maildirRoot.trimmed();
    if (root.isEmpty()) {
        add(CheckIssue::Error, QCoreApplication::translate("ConfigCheck", "Radice Maildir non indicata."));
        return out;
    }
    if (!QFileInfo(root).isDir()) {
        add(CheckIssue::Warning, QCoreApplication::translate("ConfigCheck", "La cartella %1 non esiste ancora (la crea mbsync alla prima sincronizzazione).").arg(root));
        return out;
    }
    add(CheckIssue::Ok, QCoreApplication::translate("ConfigCheck", "Cartella Maildir presente."));
    const QList<QPair<QString, QString>> specials{{QCoreApplication::translate("ConfigCheck", "Inviata"), a.sentFolder},
                                                  {QCoreApplication::translate("ConfigCheck", "Bozze"), a.draftsFolder},
                                                  {QCoreApplication::translate("ConfigCheck", "Cestino"), a.trashFolder}};
    for (const auto& [label, f] : specials) {
        if (f.trimmed().isEmpty()) {
            add(CheckIssue::Error, QCoreApplication::translate("ConfigCheck", "Cartella %1 non indicata.").arg(label));
            continue;
        }
        const QDir d(QDir(root).filePath(f));
        if (d.exists(QStringLiteral("cur")) && d.exists(QStringLiteral("new")) && d.exists(QStringLiteral("tmp")))
            continue;
        add(CheckIssue::Warning, QCoreApplication::translate("ConfigCheck", "La cartella %1 (%2) non e' una Maildir valida (servono cur, new, tmp).").arg(label, f));
    }
    return out;
}

QList<CheckIssue> ConfigCheck::checkIdentity(const Identity& i, const QStringList& msmtp, bool haveRc) {
    QList<CheckIssue> out;
    auto add = [&](CheckIssue::Level l, const QString& t) { out.append({l, t}); };
    if (i.address.trimmed().isEmpty())
        add(CheckIssue::Error, QCoreApplication::translate("ConfigCheck", "Indirizzo non indicato."));
    const QString acc = i.msmtpAccount.trimmed();
    if (acc.isEmpty())
        add(CheckIssue::Error, QCoreApplication::translate("ConfigCheck", "Account msmtp non indicato: l'invio non e' possibile."));
    else if (!haveRc)
        add(CheckIssue::Warning, QCoreApplication::translate("ConfigCheck", "File di msmtp non trovato (~/.msmtprc): account non verificabile."));
    else if (msmtp.contains(acc))
        add(CheckIssue::Ok, QCoreApplication::translate("ConfigCheck", "Account msmtp \"%1\" presente.").arg(acc));
    else
        add(CheckIssue::Error, QCoreApplication::translate("ConfigCheck", "L'account msmtp \"%1\" non e' definito in .msmtprc.").arg(acc));
    if (i.signByDefault && i.pgpKeyId.trimmed().isEmpty())
        add(CheckIssue::Warning, QCoreApplication::translate("ConfigCheck", "Firma attiva ma nessuna chiave PGP: i messaggi partono senza firma (con conferma)."));
    return out;
}

} // namespace boma
