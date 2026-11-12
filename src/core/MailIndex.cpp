#include "core/MailIndex.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <QRegularExpression>
#include <chrono>

namespace boma {

QStringList MailIndex::baseArgs(const QString& sub) const
{
    QStringList a{sub};
    if (!m_muHome.isEmpty())
        a << QStringLiteral("--muhome=%1").arg(m_muHome);
    return a;
}

QString MailIndex::accountFor(const QString& path) const
{
    for (const Account& acc : m_accounts) {
        if (acc.maildirRoot.isEmpty())
            continue;
        const QString root = QDir::cleanPath(acc.maildirRoot);
        if (path.startsWith(root + QLatin1Char('/')))
            return acc.id;
    }
    return {};
}

static QString contactText(const QJsonValue& v)
{
    QStringList parts;
    for (const QJsonValue& c : v.toArray()) {
        const QJsonObject o = c.toObject();
        const QString name = o.value(QStringLiteral(":name")).toString();
        const QString email = o.value(QStringLiteral(":email")).toString();
        parts << (name.isEmpty() ? email : name);
    }
    return parts.join(QStringLiteral(", "));
}

QList<MessageSummary> MailIndex::parse(const QByteArray& json, QString* error) const
{
    QList<MessageSummary> out;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error)
            *error = QStringLiteral("mu: risposta non valida (%1)").arg(pe.errorString());
        return out;
    }
    const QJsonArray arr = doc.array();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        MessageSummary m;
        m.path = o.value(QStringLiteral(":path")).toString();
        if (m.path.isEmpty())
            continue;
        m.messageId = o.value(QStringLiteral(":message-id")).toString();
        m.from = contactText(o.value(QStringLiteral(":from")));
        m.subject = o.value(QStringLiteral(":subject")).toString();
        const QJsonValue d = o.value(QStringLiteral(":date-unix"));
        if (d.isDouble())
            m.date = QDateTime::fromSecsSinceEpoch(qint64(d.toDouble()));
        for (const QJsonValue& f : o.value(QStringLiteral(":flags")).toArray()) {
            const QString s = f.toString();
            if (s == QLatin1String("unread") || s == QLatin1String("new"))
                m.unread = true;
            else if (s == QLatin1String("attach"))
                m.hasAttachment = true;
        }
        m.accountId = accountFor(m.path);
        out.append(m);
    }
    return out;
}

void MailIndex::search(const QString& query, int offset, int limit, SearchCallback cb)
{
    if (limit <= 0 || offset < 0) {
        if (cb)
            cb({}, QStringLiteral("Parametri di paginazione non validi"));
        return;
    }
    // Pagine successive della stessa ricerca: servite dalla copia letta alla prima
    // pagina (offset 0, sempre rifatta da mu), cosi' non si rilegge e rianalizza
    // ogni volta tutto il prefisso (costo quadratico).
    const int need = offset + limit;
    if (offset > 0 && m_cacheValid && m_cacheQuery == query && (m_cacheComplete || m_cache.size() >= need)) {
        QList<MessageSummary> page = m_cache.mid(offset, limit);
        // Sempre asincrono, come la risposta di mu: chi chiama non e' rientrato.
        QTimer::singleShot(0, this, [page = std::move(page), cb = std::move(cb)]() mutable {
            if (cb)
                cb(std::move(page), QString());
        });
        return;
    }
    const int fetch = qMax(need, kCacheChunk);
    if (offset == 0)
        m_cacheValid = false;
    QStringList args = baseArgs(QStringLiteral("find"));
    args << QStringLiteral("--format=json") << QStringLiteral("--skip-dups")
         << QStringLiteral("--sortfield=date") << QStringLiteral("--reverse")
         << QStringLiteral("--maxnum=%1").arg(fetch);
    // "--" : la query non deve mai essere letta come opzione.
    args << QStringLiteral("--") << query;
    m_runner.run(m_mu, args, std::chrono::seconds(60),
                 [this, offset, limit, fetch, query, cb = std::move(cb)](ProcessResult r) {
                     QList<MessageSummary> list;
                     QString err;
                     bool store = false;
                     if (r.crashed || r.timedOut || r.exitCode == -1) {
                         err = QString::fromUtf8(r.err).trimmed();
                         if (err.isEmpty())
                             err = r.timedOut ? QStringLiteral("mu: tempo scaduto")
                                              : QStringLiteral("mu: non eseguibile");
                     } else if (r.exitCode == 2) {
                         // codice 2 = nessuna corrispondenza: SEMPRE risultato vuoto
                         // (nessun confronto di testi dipendenti dalla lingua).
                         // Eventuale stderr: avviso non fatale.
                         const QString e = QString::fromUtf8(r.err).trimmed();
                         if (!e.isEmpty())
                             qWarning().noquote() << "mu find (codice 2):" << e;
                     } else if (r.exitCode != 0) {
                         err = QString::fromUtf8(r.err).trimmed();
                         if (err.isEmpty())
                             err = QStringLiteral("mu: errore (codice %1)").arg(r.exitCode);
                     } else {
                         list = parse(r.out, &err);
                         if (err.isEmpty()) {
                             m_cache = list;
                             m_cacheQuery = query;
                             m_cacheComplete = list.size() < fetch;
                             m_cacheValid = true;
                             store = true;
                             list = list.mid(offset, limit);
                         } else {
                             list.clear();
                         }
                     }
                     if (!store && offset == 0)
                         m_cacheValid = false;
                     if (cb)
                         cb(std::move(list), err);
                 });
}

QString MailIndex::maildirQuery(const QString& muRoot, const QString& maildirRoot, const QString& folder)
{
    if (folder.trimmed().isEmpty() || muRoot.isEmpty())
        return {};
    QString rel = QDir::cleanPath(QStringLiteral("/") + folder.trimmed());
    if (rel == QLatin1String("/"))
        return {};
    const QString root = QDir::cleanPath(muRoot);
    const QString acc = QDir::cleanPath(maildirRoot);
    if (acc == root) {
        // account = radice di mu
    } else if (root == QLatin1String("/") && acc.startsWith(QLatin1Char('/'))) {
        rel = acc + rel;
    } else if (acc.startsWith(root + QLatin1Char('/'))) {
        rel = acc.mid(root.size()) + rel;
    } else {
        return {};
    }
    rel.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("maildir:\"%1\"").arg(rel);
}

QString MailIndex::unreadQuery() const
{
    // Cestino, Bozze (salvate con la sola bandiera D) e Inviata non sono "da leggere".
    QString q = QStringLiteral("flag:unread AND NOT flag:trashed");
    for (const Account& a : m_accounts)
        for (const QString& f : {a.trashFolder, a.draftsFolder, a.sentFolder}) {
            const QString m = maildirQuery(m_muRoot, a.maildirRoot, f);
            if (!m.isEmpty())
                q += QStringLiteral(" AND NOT ") + m;
        }
    return q;
}

void MailIndex::unread(int offset, int limit, SearchCallback cb)
{
    search(unreadQuery(), offset, limit, std::move(cb));
}

void MailIndex::reindex(const QString&, Callback cb)
{
    reindexAttempt(std::move(cb), std::chrono::milliseconds(0), std::chrono::milliseconds(250));
}

void MailIndex::reindexAttempt(Callback cb, std::chrono::milliseconds waited, std::chrono::milliseconds delay)
{
    QPointer<MailIndex> self(this);
    m_runner.run(m_mu, baseArgs(QStringLiteral("index")), std::chrono::minutes(60),
                 [self, waited, delay, cb = std::move(cb)](ProcessResult r) mutable {
                     // 19 = base dati bloccata da un altro mu (spostamento o altra
                     // indicizzazione): si riprova con attesa crescente, al massimo 2 minuti.
                     if (self && r.exitCode == 19 && !r.crashed && !r.timedOut
                         && waited + delay <= std::chrono::milliseconds(120000)) {
                         const auto next = std::min(delay * 2, std::chrono::milliseconds(4000));
                         QTimer::singleShot(delay, self.data(), [self, waited, delay, next, cb = std::move(cb)]() mutable {
                             if (self)
                                 self->reindexAttempt(std::move(cb), waited + delay, next);
                         });
                         return;
                     }
                     const bool ok = r.exitCode == 0 && !r.crashed && !r.timedOut;
                     QString err;
                     if (!ok) {
                         err = QString::fromUtf8(r.err).trimmed();
                         if (err.isEmpty())
                             err = r.timedOut ? QStringLiteral("mu: tempo scaduto")
                                              : QStringLiteral("mu: errore (codice %1)").arg(r.exitCode);
                     }
                     if (cb)
                         cb(ok, err);
                 });
}

QString MailIndex::parseStoreRoot(const QByteArray& out)
{
    static const QRegularExpression re(QStringLiteral("^\\|\\s*maildir\\s*\\|\\s*(.*?)\\s*\\|\\s*$"));
    for (const QString& line : QString::fromUtf8(out).split(QLatin1Char('\n'))) {
        const auto m = re.match(line.trimmed());
        if (m.hasMatch() && !m.captured(1).isEmpty())
            return QDir::cleanPath(m.captured(1));
    }
    return {};
}

void MailIndex::storeRoot(std::function<void(QString, QString)> cb)
{
    QStringList args{QStringLiteral("info"), QStringLiteral("store")};
    if (!m_muHome.isEmpty())
        args << QStringLiteral("--muhome=%1").arg(m_muHome);
    m_runner.run(m_mu, args, std::chrono::seconds(30), [cb = std::move(cb)](ProcessResult r) {
        QString root, err;
        if (r.exitCode == 0 && !r.crashed && !r.timedOut)
            root = parseStoreRoot(r.out);
        if (root.isEmpty()) {
            err = QString::fromUtf8(r.err).trimmed();
            if (err.isEmpty())
                err = QStringLiteral("radice Maildir di mu non determinabile");
        }
        if (cb)
            cb(root, err);
    });
}

} // namespace boma
