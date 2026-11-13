#include "core/Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

namespace boma {

static QString expandHome(QString p) {
    if (p == "~" || p.startsWith("~/"))
        p.replace(0, 1, QDir::homePath());
    return p;
}

QString Config::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/boma/config.ini";
}

Config Config::load(const QString& path, QString* error) {
    Config c;
    if (error) error->clear();
    if (!QFile::exists(path)) return c;

    QSettings s(path, QSettings::IniFormat);
    QStringList errors;
    const QStringList groups = s.childGroups();

    // QSettings mappa la sezione [general] sulle chiavi di primo livello.
    {
        QVariant v = s.value("muRoot");
        if (!v.isValid()) v = s.value("general/muRoot");
        c.m_muRoot = expandHome(v.toString());
    }
    for (const QString& g : groups) {
        if (!g.startsWith("account:")) continue;
        s.beginGroup(g);
        Account a;
        a.id = g.mid(8);
        a.name = s.value("name", a.id).toString();
        a.maildirRoot = expandHome(s.value("maildirRoot").toString());
        a.mbsyncChannel = s.value("mbsyncChannel", a.id).toString();
        a.sentFolder = s.value("sentFolder", "Sent").toString();
        a.draftsFolder = s.value("draftsFolder", "Drafts").toString();
        a.trashFolder = s.value("trashFolder", "Trash").toString();
        a.archiveFolder = s.value("archiveFolder", "Archive").toString();
        s.endGroup();
        c.m_accounts.append(a);
    }

    for (const QString& g : groups) {
        if (!g.startsWith("identity:")) continue;
        s.beginGroup(g);
        Identity i;
        i.id = g.mid(9);
        i.accountId = s.value("accountId").toString();
        i.address = s.value("address").toString();
        i.displayName = s.value("displayName").toString();
        {
            // QSettings restituisce una QStringList se il valore contiene virgole
            const QVariant v = s.value("signature");
            QString sig = v.metaType().id() == QMetaType::QStringList
                              ? v.toStringList().join(", ")
                              : v.toString();
            i.signature = sig.replace("\\n", "\n");
        }
        i.pgpKeyId = s.value("pgpKeyId").toString();
        i.msmtpAccount = s.value("msmtpAccount").toString();
        i.signByDefault = s.value("signByDefault", true).toBool();
        s.endGroup();

        bool known = false;
        for (const Account& a : c.m_accounts)
            if (a.id == i.accountId) known = true;
        if (!known) {
            errors << QString("identita' '%1': account sconosciuto '%2'").arg(i.id, i.accountId);
            continue;
        }
        if (i.address.isEmpty()) {
            errors << QString("identita' '%1': indirizzo mancante").arg(i.id);
            continue;
        }
        c.m_identities.append(i);
    }

    if (error) *error = errors.join("; ");
    return c;
}

static QString unexpandHome(const QString& p) {
    const QString h = QDir::homePath();
    if (p == h) return QStringLiteral("~");
    if (p.startsWith(h + QLatin1Char('/'))) return QStringLiteral("~") + p.mid(h.size());
    return p;
}

static bool validName(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return re.match(id).hasMatch();
}

QStringList Config::validate() const {
    QStringList e;
    QSet<QString> accIds, idIds;
    for (const Account& a : m_accounts) {
        if (a.id.isEmpty() || !validName(a.id))
            e << QObject::tr("Account \"%1\": l'identificativo deve contenere solo lettere, cifre, '_', '-' e '.'").arg(a.id);
        else if (accIds.contains(a.id))
            e << QObject::tr("Account \"%1\": identificativo duplicato").arg(a.id);
        accIds.insert(a.id);
        if (a.maildirRoot.trimmed().isEmpty()) e << QObject::tr("Account \"%1\": manca la radice Maildir").arg(a.id);
        if (a.mbsyncChannel.trimmed().isEmpty()) e << QObject::tr("Account \"%1\": manca il canale mbsync").arg(a.id);
    }
    for (const Identity& i : m_identities) {
        if (i.id.isEmpty() || !validName(i.id))
            e << QObject::tr("Identita' \"%1\": l'identificativo deve contenere solo lettere, cifre, '_', '-' e '.'").arg(i.id);
        else if (idIds.contains(i.id))
            e << QObject::tr("Identita' \"%1\": identificativo duplicato").arg(i.id);
        idIds.insert(i.id);
        if (i.address.trimmed().isEmpty()) e << QObject::tr("Identita' \"%1\": manca l'indirizzo").arg(i.id);
        if (!accIds.contains(i.accountId)) e << QObject::tr("Identita' \"%1\": account sconosciuto").arg(i.id);
    }
    return e;
}

bool Config::save(const QString& path, QString* error) const {
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (error) error->clear();
    const QStringList problems = validate();
    if (!problems.isEmpty()) return fail(problems.join(QStringLiteral("; ")));
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return fail(QObject::tr("Impossibile creare la cartella di %1").arg(path));
    if (QFile::exists(path)) {
        const QString bak = path + QStringLiteral(".bak");
        QFile::remove(bak);
        if (!QFile::copy(path, bak)) return fail(QObject::tr("Impossibile salvare la copia %1").arg(bak));
    }
    QSettings s(path, QSettings::IniFormat);
    for (const QString& g : s.childGroups())
        if (g.startsWith(QLatin1String("account:")) || g.startsWith(QLatin1String("identity:"))) s.remove(g);
    for (const Account& a : m_accounts) {
        s.beginGroup(QStringLiteral("account:") + a.id);
        s.setValue(QStringLiteral("name"), a.name);
        s.setValue(QStringLiteral("maildirRoot"), unexpandHome(a.maildirRoot.trimmed()));
        s.setValue(QStringLiteral("mbsyncChannel"), a.mbsyncChannel.trimmed());
        s.setValue(QStringLiteral("sentFolder"), a.sentFolder);
        s.setValue(QStringLiteral("draftsFolder"), a.draftsFolder);
        s.setValue(QStringLiteral("trashFolder"), a.trashFolder);
        s.setValue(QStringLiteral("archiveFolder"), a.archiveFolder);
        s.endGroup();
    }
    for (const Identity& i : m_identities) {
        s.beginGroup(QStringLiteral("identity:") + i.id);
        s.setValue(QStringLiteral("accountId"), i.accountId);
        s.setValue(QStringLiteral("address"), i.address.trimmed());
        s.setValue(QStringLiteral("displayName"), i.displayName);
        QString sig = i.signature;
        sig.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        s.setValue(QStringLiteral("signature"), sig);
        s.setValue(QStringLiteral("pgpKeyId"), i.pgpKeyId.trimmed());
        s.setValue(QStringLiteral("msmtpAccount"), i.msmtpAccount.trimmed());
        s.setValue(QStringLiteral("signByDefault"), i.signByDefault);
        s.endGroup();
    }
    s.sync();
    if (s.status() != QSettings::NoError) return fail(QObject::tr("Scrittura di %1 non riuscita").arg(path));
    return true;
}

QList<Identity> Config::identities(const QString& accountId) const {
    QList<Identity> out;
    for (const Identity& i : m_identities)
        if (i.accountId == accountId) out.append(i);
    return out;
}

Identity Config::identityForAddress(const QString& address) const {
    for (const Identity& i : m_identities)
        if (i.address.compare(address.trimmed(), Qt::CaseInsensitive) == 0) return i;
    return {};
}

} // namespace boma
