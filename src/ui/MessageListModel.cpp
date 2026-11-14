#include "ui/MessageListModel.h"

namespace boma {

MessageListModel::MessageListModel(MailIndex* index, QObject* parent)
    : QAbstractListModel(parent), m_index(index) {}

void MessageListModel::setQuery(const QString& query)
{
    m_query = query;
    reset();
}

void MessageListModel::reset()
{
    beginResetModel();
    ++m_generation;
    m_all.clear();
    m_groups.clear();
    m_vis.clear();
    m_expanded.clear();
    m_seen.clear();
    const bool hadChecked = !m_checked.isEmpty();
    m_checked.clear();
    m_offset = 0;
    m_exhausted = false;
    m_truncated = false;
    m_loading = false;
    endResetModel();
    if (hadChecked) emit checkedChanged(0);
}

// Raggruppa per conversazione: due messaggi sono nella stessa se uno cita (References) un
// Message-ID dell'altro, direttamente o tramite altri. Union-find su stringhe, lineare.
void MessageListModel::rebuildGroups()
{
    m_groups.clear();
    const int n = int(m_all.size());
    if (!m_threaded) {
        m_groups.reserve(n);
        for (int i = 0; i < n; ++i) {
            Group g;
            g.members << i;
            g.gid = keyOf(m_all.at(i));
            g.unread = m_all.at(i).unread ? 1 : 0;
            m_groups << g;
        }
        return;
    }
    QHash<QString, int> node;
    QList<int> parent;
    auto id = [&](const QString& k) {
        auto it = node.find(k);
        if (it != node.end()) return it.value();
        const int v = int(parent.size());
        parent << v;
        node.insert(k, v);
        return v;
    };
    auto find = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    QList<int> own(n);
    for (int i = 0; i < n; ++i) {
        const MessageSummary& m = m_all.at(i);
        own[i] = id(keyOf(m));
        for (const QString& r : m.references) {
            const int a = find(own[i]), b = find(id(r));
            if (a != b) parent[b] = a;
        }
    }
    QHash<int, int> groupOf;
    for (int i = 0; i < n; ++i) {
        const int root = find(own[i]);
        auto it = groupOf.find(root);
        int g;
        if (it == groupOf.end()) {
            g = int(m_groups.size());
            groupOf.insert(root, g);
            m_groups << Group();
        } else {
            g = it.value();
        }
        Group& gr = m_groups[g];
        gr.members << i;
        const QString k = keyOf(m_all.at(i));
        if (gr.gid.isEmpty() || k < gr.gid) gr.gid = k;
        if (m_all.at(i).unread) ++gr.unread;
    }
}

QList<MessageListModel::Row> MessageListModel::computeVis() const
{
    QList<Row> v;
    v.reserve(m_all.size());
    for (int g = 0; g < m_groups.size(); ++g) {
        const Group& gr = m_groups.at(g);
        const int cnt = int(gr.members.size());
        const bool open = cnt > 1 && m_expanded.contains(gr.gid);
        for (int p = 0; p < (open ? cnt : 1); ++p) {
            Row r;
            r.group = g;
            r.idx = gr.members.at(p);
            r.level = p == 0 ? 0 : 1;
            r.id = keyOf(m_all.at(r.idx));
            r.count = p == 0 ? cnt : 0;
            r.unread = gr.unread;
            r.expanded = open;
            v << r;
        }
    }
    return v;
}

// Porta m_vis a nv emettendo il minimo di segnali: la vista non perde selezione e
// posizione quando arriva una pagina o si apre/chiude una conversazione.
void MessageListModel::applyVis(QList<Row> nv)
{
    const QList<Row> old = m_vis;
    auto same = [](const Row& a, const Row& b) { return a.id == b.id && a.level == b.level; };
    const int on = int(old.size()), nn = int(nv.size());
    int d = 0;
    while (d < on && d < nn && same(old[d], nv[d])) ++d;
    if (nn > on) {
        const int k = nn - on;
        bool tailOk = true;
        for (int i = d; i < on && tailOk; ++i) tailOk = same(old[i], nv[i + k]);
        if (tailOk) {
            beginInsertRows({}, d, d + k - 1);
            m_vis = std::move(nv);
            endInsertRows();
            // righe preesistenti (capo di conversazione) con dati cambiati
            if (d > 0) emit dataChanged(index(0), index(d - 1));
            return;
        }
    } else if (nn < on) {
        const int k = on - nn;
        bool tailOk = true;
        for (int i = d + k; i < on && tailOk; ++i) tailOk = same(old[i], nv[i - k]);
        if (tailOk) {
            beginRemoveRows({}, d, d + k - 1);
            m_vis = std::move(nv);
            endRemoveRows();
            if (nn > 0) emit dataChanged(index(0), index(nn - 1));
            return;
        }
    } else {
        m_vis = std::move(nv);
        if (nn > 0) emit dataChanged(index(0), index(nn - 1));
        return;
    }
    beginResetModel();
    m_vis = std::move(nv);
    endResetModel();
}

void MessageListModel::setThreaded(bool on)
{
    if (m_threaded == on) return;
    beginResetModel();
    m_threaded = on;
    m_expanded.clear();
    rebuildGroups();
    m_vis = computeVis();
    endResetModel();
}

void MessageListModel::toggleExpanded(int row)
{
    if (row < 0 || row >= m_vis.size() || m_vis.at(row).level != 0) return;
    const Group& g = m_groups.at(m_vis.at(row).group);
    if (g.members.size() < 2) return;
    if (!m_expanded.remove(g.gid)) m_expanded.insert(g.gid);
    applyVis(computeVis());
}

bool MessageListModel::hasPath(const QString& path) const
{
    return m_seen.contains(path);
}

void MessageListModel::markReadLocal(int row, const QString& newPath)
{
    if (row < 0 || row >= m_vis.size())
        return;
    markReadPath(m_all.at(m_vis.at(row).idx).path, newPath);
}

void MessageListModel::markReadPath(const QString& path, const QString& newPath)
{
    int i = -1;
    for (int k = 0; k < m_all.size(); ++k)
        if (m_all.at(k).path == path) { i = k; break; }
    if (i < 0)
        return;
    m_all[i].unread = false;
    if (!newPath.isEmpty() && newPath != path) {
        m_seen.remove(path);
        if (m_checked.remove(path)) m_checked.insert(newPath);
        m_all[i].path = newPath;
        m_seen.insert(newPath);
    }
    rebuildGroups();
    applyVis(computeVis());
}

void MessageListModel::removeAt(int row)
{
    if (row < 0 || row >= m_vis.size())
        return;
    removePath(m_all.at(m_vis.at(row).idx).path);
}

void MessageListModel::removePath(const QString& path)
{
    int i = -1;
    for (int k = 0; k < m_all.size(); ++k)
        if (m_all.at(k).path == path) { i = k; break; }
    if (i < 0)
        return;
    const bool wasChecked = m_checked.remove(path);
    m_seen.remove(path);
    m_all.removeAt(i);
    rebuildGroups();
    applyVis(computeVis());
    if (wasChecked) emit checkedChanged(int(m_checked.size()));
    // le pagine successive si spostano di una riga
    if (m_offset > 0)
        --m_offset;
}

QList<MessageSummary> MessageListModel::checkedSummaries() const
{
    QList<MessageSummary> out;
    for (const MessageSummary& m : m_all)
        if (m_checked.contains(m.path)) out << m;
    return out;
}

void MessageListModel::clearChecked()
{
    if (m_checked.isEmpty()) return;
    m_checked.clear();
    if (!m_vis.isEmpty()) emit dataChanged(index(0), index(int(m_vis.size()) - 1), {CheckedRole});
    emit checkedChanged(0);
}

// Riga capo di una conversazione chiusa: rappresenta tutti i suoi messaggi.
bool MessageListModel::rowChecked(const Row& r) const
{
    const Group& g = m_groups.at(r.group);
    if (r.level == 0 && g.members.size() > 1 && !r.expanded) {
        for (int i : g.members)
            if (!m_checked.contains(m_all.at(i).path)) return false;
        return true;
    }
    return m_checked.contains(m_all.at(r.idx).path);
}

bool MessageListModel::setData(const QModelIndex& idx, const QVariant& v, int role)
{
    if (role != CheckedRole || !idx.isValid() || idx.row() >= m_vis.size()) return false;
    const Row& r = m_vis.at(idx.row());
    const Group& g = m_groups.at(r.group);
    QList<int> targets;
    if (r.level == 0 && g.members.size() > 1 && !r.expanded) targets = g.members;
    else targets << r.idx;
    for (int i : targets) {
        const QString& p = m_all.at(i).path;
        if (v.toBool()) m_checked.insert(p);
        else m_checked.remove(p);
    }
    // le righe di una conversazione aperta mostrano la propria casella: aggiorna tutto
    emit dataChanged(index(0), index(int(m_vis.size()) - 1), {CheckedRole});
    emit checkedChanged(int(m_checked.size()));
    return true;
}

Qt::ItemFlags MessageListModel::flags(const QModelIndex& idx) const
{
    return QAbstractListModel::flags(idx);
}

int MessageListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_vis.size());
}

QVariant MessageListModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_vis.size())
        return {};
    const Row& r = m_vis.at(idx.row());
    const MessageSummary& m = m_all.at(r.idx);
    const bool collapsedHead = r.level == 0 && r.count > 1 && !r.expanded;
    switch (role) {
    case Qt::DisplayRole:
    case SubjectRole: return m.subject;
    case FromRole: return m.from;
    case DateRole: return m.date;
    case UnreadRole: return collapsedHead ? r.unread > 0 : m.unread;
    case HasAttachmentRole: return m.hasAttachment;
    case PathRole: return m.path;
    case AccountIdRole: return m.accountId;
    case FlaggedRole: return m.flagged;
    case SizeRole: return m.size;
    case CheckedRole: return rowChecked(r);
    case ThreadSizeRole: return r.level == 0 && r.count > 1 ? r.count : 0;
    case ThreadExpandedRole: return r.expanded;
    case ThreadLevelRole: return r.level;
    }
    return {};
}

QHash<int, QByteArray> MessageListModel::roleNames() const
{
    auto r = QAbstractListModel::roleNames();
    r[FromRole] = "from";
    r[SubjectRole] = "subject";
    r[DateRole] = "date";
    r[UnreadRole] = "unread";
    r[HasAttachmentRole] = "hasAttachment";
    r[PathRole] = "path";
    r[AccountIdRole] = "accountId";
    return r;
}

bool MessageListModel::canFetchMore(const QModelIndex& parent) const
{
    return !parent.isValid() && m_index && !m_exhausted && !m_loading;
}

void MessageListModel::fetchMore(const QModelIndex& parent)
{
    if (!canFetchMore(parent))
        return;
    const int want = qMin(int(PageSize), m_maxWindow - m_offset);
    if (want <= 0) {
        m_exhausted = true;
        m_truncated = true;
        return;
    }
    m_loading = true;
    const quint64 gen = m_generation;
    // Ultima pagina della finestra: una riga di sonda per sapere se altre
    // righe restano nascoste (truncated() solo se davvero vero).
    const int probe = (m_offset + want >= m_maxWindow) ? 1 : 0;
    QPointer<MessageListModel> self(this);
    auto cb = [self, gen, want, probe](QList<MessageSummary> list, QString error) {
        if (!self || gen != self->m_generation)
            return; // modello distrutto o risultato di una query superata
        self->applyPage(std::move(list), error, want, probe);
    };
    if (m_query.trimmed().isEmpty())
        m_index->unread(m_offset, want + probe, cb);
    else
        m_index->search(m_query, m_offset, want + probe, cb);
}

void MessageListModel::applyPage(QList<MessageSummary> list, const QString& error, int want, int probe)
{
    ++s_applied;
    {
        m_loading = false;
        if (!error.isEmpty()) {
            // non definitivo: fetchMore puo' ritentare
            emit pageLoaded(0, error);
            return;
        }
        bool hidden = false;
        if (probe && list.size() > want) {
            hidden = true;
            list = list.mid(0, want);
        }
        m_offset += int(list.size());
        if (hidden) {
            m_exhausted = true;
            m_truncated = true;
        } else if (list.size() < want || m_offset >= m_maxWindow) {
            m_exhausted = true;
        }
        QList<MessageSummary> fresh;
        for (const MessageSummary& s : list) {
            if (m_seen.contains(s.path))
                continue;
            m_seen.insert(s.path);
            fresh.append(s);
        }
        if (!fresh.isEmpty()) {
            m_all += fresh;
            rebuildGroups();
            applyVis(computeVis());
        }
        emit pageLoaded(int(fresh.size()), QString());
    }
}

} // namespace boma
