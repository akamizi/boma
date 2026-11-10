#include "ui/FolderPane.h"

#include <QDir>

namespace boma {

QString FolderPane::folderQuery(const QString& muRoot, const QString& maildirRoot, const QString& folder) {
    QString rel = QDir::cleanPath(QStringLiteral("/") + folder);
    if (!muRoot.isEmpty()) {
        const QString root = QDir::cleanPath(muRoot);
        const QString acc = QDir::cleanPath(maildirRoot);
        if (acc.startsWith(root + QLatin1Char('/')))
            rel = acc.mid(root.size()) + rel;
    }
    rel.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("maildir:\"%1\"").arg(rel);
}

FolderPane::FolderPane(const Config& config, const QString& muRoot, QWidget* parent) : QTreeView(parent) {
    model_ = new QStandardItemModel(this);
    setHeaderHidden(true);
    setEditTriggers(NoEditTriggers);
    setModel(model_);

    const int QueryRole = Qt::UserRole + 1, FolderRole = Qt::UserRole + 2;
    auto* unread = new QStandardItem(tr("Non lette"));
    unread->setData(QString(), QueryRole);
    unread->setData(true, FolderRole);
    model_->appendRow(unread);
    for (const Account& a : config.accounts()) {
        auto* top = new QStandardItem(a.name.isEmpty() ? a.id : a.name);
        top->setData(a.id, Qt::UserRole + 3);
        auto add = [&](const QString& label, const QString& folder) {
            if (folder.trimmed().isEmpty()) return;
            auto* it = new QStandardItem(label);
            it->setData(folderQuery(muRoot, a.maildirRoot, folder), QueryRole);
            it->setData(true, FolderRole);
            top->appendRow(it);
        };
        add(tr("Posta in arrivo"), QStringLiteral("INBOX"));
        add(tr("Inviata"), a.sentFolder);
        add(tr("Bozze"), a.draftsFolder);
        add(tr("Cestino"), a.trashFolder);
        model_->appendRow(top);
    }
    expandAll();
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, [this, FolderRole, QueryRole](const QModelIndex& cur) {
        if (cur.isValid() && cur.data(FolderRole).toBool()) emit querySelected(cur.data(QueryRole).toString());
    });
}

} // namespace boma
