#pragma once
#include "core/Config.h"
#include <QStandardItemModel>
#include <QString>
#include <QTreeView>

namespace boma {

// Account come nodi di primo livello, cartelle (da Config) sotto. In cima il
// nodo "Non lette" (tutti gli account). La selezione emette la query mu da
// eseguire (asincrona, a cura del chiamante); i nodi account non emettono nulla.
// Nessun conteggio dei non letti: richiederebbe una query mu per cartella.
class FolderPane : public QTreeView {
    Q_OBJECT
public:
    // muRoot: radice Maildir di mu; se vuota si assume la radice dell'account
    // (come MailActions). Le cartelle sono "maildir:/<percorso relativo a muRoot>".
    explicit FolderPane(const Config& config, const QString& muRoot = {}, QWidget* parent = nullptr);
    QStandardItemModel* itemModel() const { return model_; }
    // Query di una cartella (vuota per "Non lette"); esposta per i test.
    static QString folderQuery(const QString& muRoot, const QString& maildirRoot, const QString& folder);

signals:
    void querySelected(const QString& query);

private:
    QStandardItemModel* model_;
};

} // namespace boma
