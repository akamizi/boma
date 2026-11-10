#pragma once
#include "core/MailIndex.h"
#include <QAbstractListModel>
#include <QPointer>
#include <QSet>

namespace boma {

// Modello a caricamento pigro: pagine da 200 righe tramite canFetchMore /
// fetchMore. Query vuota = vista dei non letti. I risultati tardivi di una
// query sostituita (o di un reset) vengono scartati (contatore di generazione).
class MessageListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        FromRole = Qt::UserRole + 1,
        SubjectRole,
        DateRole,
        UnreadRole,
        HasAttachmentRole,
        PathRole,
        AccountIdRole
    };
    static constexpr int PageSize = 200;
    // Finestra massima richiesta a mu (il costo cresce con la profondita').
    static constexpr int kMaxWindow = 5000;

    explicit MessageListModel(MailIndex* index, QObject* parent = nullptr);

    // Azzera il modello e la prossima fetchMore parte dalla prima pagina.
    void setQuery(const QString& query);
    // Come setQuery con la query corrente.
    void reset();
    void setMaxWindow(int n) { m_maxWindow = n; }
    // true se i risultati sono stati troncati dal limite di finestra: la UI
    // puo' suggerire di affinare la ricerca.
    bool truncated() const { return m_truncated; }
    QString query() const { return m_query; }
    bool isLoading() const { return m_loading; }
    const MessageSummary& summary(int row) const { return m_rows.at(row); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

signals:
    // Una pagina e' stata applicata (o e' fallita: error non vuoto).
    void pageLoaded(int added, const QString& error);

private:
    void applyPage(QList<MessageSummary> list, const QString& error, int want, int probe);
    QPointer<MailIndex> m_index;
    QString m_query;
    QList<MessageSummary> m_rows;
    QSet<QString> m_seen;
    int m_offset = 0; // risultati gia' richiesti a mu (incl. duplicati scartati)
    bool m_exhausted = false, m_loading = false, m_truncated = false;
    int m_maxWindow = kMaxWindow;
    quint64 m_generation = 0;
public:
    // Solo per i test: numero di pagine applicate.
    static inline int s_applied = 0;
};

} // namespace boma
