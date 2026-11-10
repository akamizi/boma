#include "core/Config.h"

#include <QDir>
#include <QFile>
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
