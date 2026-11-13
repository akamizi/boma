#pragma once
#include "core/Config.h"
#include <QStandardItemModel>
#include <QHash>
#include <QMenu>
#include <QString>
#include <QStringList>
#include <QTreeView>

namespace boma {

// Account come nodi di primo livello, cartelle (da Config) sotto. In cima il
// nodo "Non lette" (tutti gli account). La selezione emette la query mu da
// eseguire (asincrona, a cura del chiamante); i nodi account non emettono nulla.
// I conteggi dei non letti li calcola il chiamante (una query mu per cartella) e li
// passa a setUnreadCounts: la cartella con non letti e' in grassetto, "Nome (N)";
// i nodi senza Maildir propria (account, cartelle intermedie) mostrano la somma dei figli.
// Finche' la radice di mu non e' nota (setMuRoot) esiste solo "Non lette" e gli
// account senza cartelle: le query di cartella sarebbero sbagliate.
class FolderPane : public QTreeView {
    Q_OBJECT
public:
    explicit FolderPane(const Config& config, QWidget* parent = nullptr);
    // Nuova configurazione: va seguita da setMuRoot per ricostruire l'albero.
    void setConfig(const Config& config) { config_ = config; }
    // (Ri)costruisce le cartelle con la radice di mu data.
    void setMuRoot(const QString& muRoot);
    QStandardItemModel* itemModel() const { return model_; }
    // Query di tutte le cartelle selezionabili ("" = "Non lette"), per contarne i non letti.
    QStringList countableQueries() const;
    // Aggiorna i conteggi (chiave = query; query mancante = nessun dato, testo senza numero).
    void setUnreadCounts(const QHash<QString, int>& byQuery);
    // Query di una cartella "maildir:\"/<rel>\"", relativa alla radice di mu.
    // Vuota se la Maildir dell'account non e' sotto la radice.
    static QStringList scanFolders(const QString& maildirRoot);
    static QString folderQuery(const QString& muRoot, const QString& maildirRoot, const QString& folder);

    // Menu contestuale di una cartella (Maildir): nullptr per i nodi che non lo sono.
    // Il chiamante lo possiede (parent = this).
    QMenu* contextMenuFor(const QModelIndex& index);

    // Cartelle Maildir di ogni account, come destinazioni di uno spostamento.
    struct FolderTarget {
        QString accountId, accountName;
        QString folder; // relativa a maildirRoot ("INBOX", "Archive/2025")
        QString label;  // percorso mostrato ("Posta in arrivo", "Altre / Sotto")
    };
    QList<FolderTarget> folderTargets() const;

signals:
    void querySelected(const QString& query);
    // accountId, query della cartella, percorso assoluto della cartella, nome mostrato
    void markAllReadRequested(const QString& accountId, const QString& query, const QString& dir, const QString& label);
    void emptyFolderRequested(const QString& accountId, const QString& query, const QString& dir, const QString& label);

private:
    Config config_;
    QStandardItemModel* model_;
};

} // namespace boma
