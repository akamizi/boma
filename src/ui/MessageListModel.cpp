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
    m_rows.clear();
    m_seen.clear();
    m_offset = 0;
    m_exhausted = false;
    m_loading = false;
    endResetModel();
}

int MessageListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant MessageListModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_rows.size())
        return {};
    const MessageSummary& m = m_rows.at(idx.row());
    switch (role) {
    case Qt::DisplayRole:
    case SubjectRole: return m.subject;
    case FromRole: return m.from;
    case DateRole: return m.date;
    case UnreadRole: return m.unread;
    case HasAttachmentRole: return m.hasAttachment;
    case PathRole: return m.path;
    case AccountIdRole: return m.accountId;
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
    m_loading = true;
    const quint64 gen = m_generation;
    auto cb = [this, gen](QList<MessageSummary> list, QString error) {
        if (gen != m_generation)
            return; // risultato di una query superata
        m_loading = false;
        if (!error.isEmpty()) {
            m_exhausted = true;
            emit pageLoaded(0, error);
            return;
        }
        m_offset += int(list.size());
        if (list.size() < PageSize)
            m_exhausted = true;
        QList<MessageSummary> fresh;
        for (const MessageSummary& s : list) {
            if (m_seen.contains(s.path))
                continue;
            m_seen.insert(s.path);
            fresh.append(s);
        }
        if (!fresh.isEmpty()) {
            beginInsertRows({}, int(m_rows.size()), int(m_rows.size() + fresh.size() - 1));
            m_rows += fresh;
            endInsertRows();
        }
        emit pageLoaded(int(fresh.size()), QString());
    };
    if (m_query.trimmed().isEmpty())
        m_index->unread(m_offset, PageSize, cb);
    else
        m_index->search(m_query, m_offset, PageSize, cb);
}

} // namespace boma
