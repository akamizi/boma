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
        AccountIdRole,
        FlaggedRole,
        SizeRole,
        CheckedRole,
        ThreadSizeRole,     // numero di messaggi della conversazione (riga capo, solo se > 1)
        ThreadExpandedRole, // conversazione aperta
        ThreadLevelRole     // 0 = riga principale, 1 = messaggio dentro la conversazione aperta
    };
    static constexpr int PageSize = 200;
    // Finestra massima richiesta a mu (il costo cresce con la profondita').
    static constexpr int kMaxWindow = 5000;

    explicit MessageListModel(MailIndex* index, QObject* parent = nullptr);

    // Cambia l'indice (nullptr = nessun account configurato) e azzera il modello.
    void setIndex(MailIndex* index) { m_index = index; reset(); }
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
    const MessageSummary& summary(int row) const { return m_all.at(m_vis.at(row).idx); }
    // Aggiornamenti locali dopo un'azione riuscita (senza rileggere da mu):
    // la riga risulta letta (e col nuovo percorso se il file e' stato spostato)...
    void markReadLocal(int row, const QString& newPath = {});
    // Come sopra / come removeAt, ma per percorso: valgono anche per messaggi nascosti
    // dentro una conversazione chiusa.
    void markReadPath(const QString& path, const QString& newPath = {});
    void removePath(const QString& path);
    // ...oppure e' tolta dall'elenco (messaggio spostato nel cestino).
    void removeAt(int row);

    // Raggruppamento per conversazione (Message-ID/References), solo tra i messaggi gia'
    // caricati. Una conversazione chiusa e' una riga (il primo messaggio in ordine di elenco)
    // col numero dei messaggi; aperta mostra tutti i messaggi. Spuntare la riga chiusa
    // seleziona tutta la conversazione. Predefinito: spento.
    void setThreaded(bool on);
    bool threaded() const { return m_threaded; }
    void toggleExpanded(int row);
    bool hasPath(const QString& path) const;

    // Selezione multipla (casella a sinistra di ogni riga), per percorso.
    int checkedCount() const { return int(m_checked.size()); }
    QList<MessageSummary> checkedSummaries() const;
    void clearChecked();
    bool setData(const QModelIndex& idx, const QVariant& v, int role) override;
    Qt::ItemFlags flags(const QModelIndex& idx) const override;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

signals:
    void checkedChanged(int count);
    // Una pagina e' stata applicata (o e' fallita: error non vuoto).
    void pageLoaded(int added, const QString& error);

private:
    struct Group {
        QList<int> members; // indici in m_all, nell'ordine dell'elenco
        QString gid;        // chiave stabile (il Message-ID minore)
        int unread = 0;
    };
    struct Row {
        int group = 0, idx = 0, level = 0;
        QString id; // identita' stabile della riga (Message-ID o percorso)
        int count = 0, unread = 0;
        bool expanded = false;
    };
    static QString keyOf(const MessageSummary& m) { return m.messageId.isEmpty() ? m.path : m.messageId; }
    void rebuildGroups();
    QList<Row> computeVis() const;
    void applyVis(QList<Row> nv);
    bool rowChecked(const Row& r) const;
    void applyPage(QList<MessageSummary> list, const QString& error, int want, int probe);
    QPointer<MailIndex> m_index;
    QString m_query;
    QList<MessageSummary> m_all; // tutti i messaggi caricati
    QList<Group> m_groups;
    QList<Row> m_vis;            // righe visibili
    QSet<QString> m_expanded;    // gid delle conversazioni aperte
    bool m_threaded = false;
    QSet<QString> m_seen, m_checked;
    int m_offset = 0; // risultati gia' richiesti a mu (incl. duplicati scartati)
    bool m_exhausted = false, m_loading = false, m_truncated = false;
    int m_maxWindow = kMaxWindow;
    quint64 m_generation = 0;
public:
    // Solo per i test: numero di pagine applicate.
    static inline int s_applied = 0;
};

} // namespace boma
