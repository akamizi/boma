#include "ui/FolderPane.h"

#include <QDir>

namespace boma {

QString FolderPane::folderQuery(const QString& muRoot, const QString& maildirRoot, const QString& folder) {
    QString rel = QDir::cleanPath(QStringLiteral("/") + folder);
    const QString root = QDir::cleanPath(muRoot);
    const QString acc = QDir::cleanPath(maildirRoot);
    if (acc == root) {
        // account = radice di mu
    } else if (acc.startsWith(root + QLatin1Char('/'))) {
        rel = acc.mid(root.size()) + rel;
    } else {
        return {};
    }
    rel.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("maildir:\"%1\"").arg(rel);
}

FolderPane::FolderPane(const Config& config, QWidget* parent) : QTreeView(parent), config_(config) {
    model_ = new QStandardItemModel(this);
    setHeaderHidden(true);
    setEditTriggers(NoEditTriggers);
    setModel(model_);
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
        if (cur.isValid() && cur.data(Qt::UserRole + 2).toBool()) emit querySelected(cur.data(Qt::UserRole + 1).toString());
    });
    setMuRoot(QString());
}

void FolderPane::setMuRoot(const QString& muRoot) {
    const int QueryRole = Qt::UserRole + 1, FolderRole = Qt::UserRole + 2;
    QSignalBlocker b(selectionModel());
    model_->clear();
    auto* unread = new QStandardItem(tr("Non lette"));
    unread->setData(QString(), QueryRole);
    unread->setData(true, FolderRole);
    model_->appendRow(unread);
    for (const Account& a : config_.accounts()) {
        auto* top = new QStandardItem(a.name.isEmpty() ? a.id : a.name);
        top->setData(a.id, Qt::UserRole + 3);
        auto add = [&](const QString& label, const QString& folder) {
            if (muRoot.isEmpty() || folder.trimmed().isEmpty()) return;
            const QString q = folderQuery(muRoot, a.maildirRoot, folder);
            if (q.isEmpty()) return;
            auto* it = new QStandardItem(label);
            it->setData(q, QueryRole);
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
}

} // namespace boma
