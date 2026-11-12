#include "core/MsmtpConfig.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace boma {

namespace {

struct Line {
    QByteArray raw;
    QString key, value; // chiave in minuscolo; vuoti per commenti/righe vuote
};

QList<Line> split(const QByteArray& text) {
    QList<Line> out;
    QByteArray t = text;
    QList<QByteArray> parts = t.split('\n');
    if (!parts.isEmpty() && parts.last().isEmpty()) parts.removeLast(); // niente riga vuota finale fittizia
    for (const QByteArray& p : parts) {
        Line l;
        l.raw = p;
        const QString s = QString::fromUtf8(p).trimmed();
        if (!s.isEmpty() && !s.startsWith(QLatin1Char('#'))) {
            const int i = s.indexOf(QRegularExpression(QStringLiteral("\\s")));
            l.key = (i < 0 ? s : s.left(i)).toLower();
            l.value = i < 0 ? QString() : s.mid(i).trimmed();
        }
        out << l;
    }
    return out;
}

// Intervallo [begin,end) del blocco: account "name" oppure defaults ("" = defaults).
bool findBlock(const QList<Line>& ls, const QString& account, bool defaults, int* begin, int* end) {
    int b = -1;
    for (int i = 0; i < ls.size(); ++i) {
        const Line& l = ls.at(i);
        const bool isAcc = l.key == QLatin1String("account");
        const bool isDef = l.key == QLatin1String("defaults");
        if (b >= 0 && (isAcc || isDef)) {
            *begin = b;
            *end = i;
            return true;
        }
        if (b < 0) {
            if (defaults && isDef) b = i;
            else if (!defaults && isAcc && l.value.section(QLatin1Char(':'), 0, 0).trimmed() == account) b = i;
        }
    }
    if (b < 0) return false;
    *begin = b;
    *end = ls.size();
    return true;
}

QMap<QString, QString> readBlock(const QList<Line>& ls, int b, int e) {
    QMap<QString, QString> m;
    for (int i = b + 1; i < e; ++i)
        if (!ls.at(i).key.isEmpty() && !m.contains(ls.at(i).key)) m.insert(ls.at(i).key, ls.at(i).value);
    return m;
}

} // namespace

QStringList MsmtpConfig::managedKeys() {
    return {QStringLiteral("host"), QStringLiteral("port"), QStringLiteral("tls"), QStringLiteral("tls_starttls"),
            QStringLiteral("auth"), QStringLiteral("user"), QStringLiteral("from"), QStringLiteral("passwordeval")};
}

bool MsmtpConfig::validAccountName(const QString& n) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return re.match(n).hasMatch() && n != QLatin1String("default");
}

QMap<QString, QString> MsmtpConfig::accountSettings(const QByteArray& text, const QString& account) {
    const QList<Line> ls = split(text);
    int b, e;
    return findBlock(ls, account, false, &b, &e) ? readBlock(ls, b, e) : QMap<QString, QString>();
}

QMap<QString, QString> MsmtpConfig::defaultSettings(const QByteArray& text) {
    const QList<Line> ls = split(text);
    int b, e;
    return findBlock(ls, QString(), true, &b, &e) ? readBlock(ls, b, e) : QMap<QString, QString>();
}

bool MsmtpConfig::hasAccount(const QByteArray& text, const QString& account) {
    const QList<Line> ls = split(text);
    int b, e;
    return findBlock(ls, account, false, &b, &e);
}

bool MsmtpConfig::hasPlainPassword(const QByteArray& text, const QString& account) {
    return accountSettings(text, account).contains(QStringLiteral("password"));
}

QByteArray MsmtpConfig::applyAccount(const QByteArray& text, const QString& account,
                                     const QMap<QString, QString>& values, QString* error) {
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        return QByteArray();
    };
    if (!validAccountName(account)) return fail(QStringLiteral("Nome account msmtp non valido (ammessi lettere, cifre, . _ -)."));
    const QStringList managed = managedKeys();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!managed.contains(it.key())) return fail(QStringLiteral("Chiave non gestita: %1").arg(it.key()));
        if (it.value().contains(QLatin1Char('\n')) || it.value().contains(QLatin1Char('\r')))
            return fail(QStringLiteral("Il valore di %1 deve stare su una riga.").arg(it.key()));
    }
    QList<Line> ls = split(text);
    int b, e;
    auto makeLine = [](const QString& k, const QString& v) {
        Line l;
        l.key = k;
        l.value = v;
        l.raw = QStringLiteral("%1 %2").arg(k, -14).arg(v).toUtf8();
        return l;
    };
    if (!findBlock(ls, account, false, &b, &e)) {
        if (!ls.isEmpty() && !ls.last().raw.trimmed().isEmpty()) ls << Line();
        Line h;
        h.key = QStringLiteral("account");
        h.value = account;
        h.raw = "account        " + account.toUtf8();
        ls << h;
        b = int(ls.size()) - 1;
        e = int(ls.size());
    }
    for (const QString& k : managed) {
        if (!values.contains(k)) continue;
        const QString v = values.value(k).trimmed();
        int found = -1;
        for (int i = b + 1; i < e; ++i)
            if (ls.at(i).key == k) { found = i; break; }
        if (found >= 0) {
            if (v.isEmpty()) { ls.removeAt(found); --e; }
            else if (ls.at(found).value != v) ls[found] = makeLine(k, v);
        } else if (!v.isEmpty()) {
            int at = e; // dopo l'ultima riga non vuota del blocco
            while (at > b + 1 && ls.at(at - 1).raw.trimmed().isEmpty()) --at;
            ls.insert(at, makeLine(k, v));
            ++e;
        }
    }
    QByteArray out;
    for (const Line& l : ls) out += l.raw + '\n';
    return out;
}

bool MsmtpConfig::save(const QString& path, const QByteArray& text, QString* error) {
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        return false;
    };
    if (path.isEmpty()) return fail(QStringLiteral("Percorso di .msmtprc non definito."));
    QString target = path;
    const QFileInfo fi(path);
    if (fi.exists() && fi.isSymLink()) target = fi.canonicalFilePath();
    const QFileInfo ti(target);
    QFile::Permissions perms = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (ti.exists()) {
        perms = ti.permissions();
        const QString bak = target + QStringLiteral(".bak");
        QFile::remove(bak);
        if (!QFile::copy(target, bak)) return fail(QStringLiteral("Impossibile creare la copia di sicurezza %1.").arg(bak));
        QFile::setPermissions(bak, perms);
    }
    QSaveFile f(target);
    if (!f.open(QIODevice::WriteOnly)) return fail(f.errorString());
    f.write(text);
    f.setPermissions(perms);
    if (!f.commit()) return fail(f.errorString());
    return true;
}

} // namespace boma
