#include "ui/FolderPane.h"

#include "core/MailIndex.h"

#include <QAction>
#include <QDir>
#include <QHash>
#include <functional>
#include <QFileInfo>
#include <QSet>

namespace boma {

QString FolderPane::folderQuery(const QString& muRoot, const QString& maildirRoot, const QString& folder) {
    return MailIndex::maildirQuery(muRoot, maildirRoot, folder);
}

// Tutte le Maildir (cartelle con cur/new/tmp) sotto la radice dell'account, come
// percorsi relativi: annidate (SubFolders Verbatim) o in stile Maildir++ (".A.B").
// Non entra mai in cur/new/tmp e limita profondita' e numero.
QStringList FolderPane::scanFolders(const QString& root) {
    QStringList out;
    const QDir base(root);
    if (root.trimmed().isEmpty() || !base.exists()) return out;
    auto isMaildir = [](const QDir& d) { return QFileInfo(d.filePath(QStringLiteral("cur"))).isDir(); };
    auto walk = [&](auto&& self, const QString& rel, int depth) -> void {
        if (depth > 8 || out.size() >= 2000) return;
        const QDir d(base.filePath(rel));
        const QStringList subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name | QDir::IgnoreCase);
        for (const QString& n : subs) {
            if (n == QLatin1String("cur") || n == QLatin1String("new") || n == QLatin1String("tmp")) continue;
            const QString r = rel.isEmpty() ? n : rel + QLatin1Char('/') + n;
            if (isMaildir(QDir(base.filePath(r)))) out << r;
            self(self, r, depth + 1);
        }
    };
    walk(walk, QString(), 0);
    return out;
}

FolderPane::FolderPane(const Config& config, QWidget* parent) : QTreeView(parent), config_(config) {
    model_ = new QStandardItemModel(this);
    setHeaderHidden(true);
    setEditTriggers(NoEditTriggers);
    setModel(model_);
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
        if (cur.isValid() && cur.data(Qt::UserRole + 2).toBool()) emit querySelected(cur.data(Qt::UserRole + 1).toString());
    });
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QMenu* m = contextMenuFor(indexAt(p));
        if (!m) return;
        m->exec(viewport()->mapToGlobal(p));
        delete m;
    });
    setMuRoot(QString());
}

QMenu* FolderPane::contextMenuFor(const QModelIndex& index) {
    if (!index.isValid()) return nullptr;
    const QString dir = index.data(Qt::UserRole + 6).toString();
    const QString acc = index.data(Qt::UserRole + 7).toString();
    if (dir.isEmpty() || acc.isEmpty()) return nullptr;
    const QString q = index.data(Qt::UserRole + 1).toString();
    const QString label = index.data(Qt::UserRole + 5).toString();
    auto* m = new QMenu(this);
    QAction* read = m->addAction(tr("Segna tutti come letti"));
    read->setObjectName(QStringLiteral("markAllRead"));
    connect(read, &QAction::triggered, this, [=, this]() { emit markAllReadRequested(acc, q, dir, label); });
    m->addSeparator();
    QAction* empty = m->addAction(tr("Svuota la casella (cancellazione definitiva)..."));
    empty->setObjectName(QStringLiteral("emptyFolder"));
    connect(empty, &QAction::triggered, this, [=, this]() { emit emptyFolderRequested(acc, q, dir, label); });
    return m;
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
        // Albero: "A/B" annidato e ".A.B" (Maildir++) diventano A > B. I nodi
        // intermedi che non sono Maildir non sono selezionabili.
        QHash<QString, QStandardItem*> nodes;
        auto add = [&](const QString& label, const QString& folder) {
            if (muRoot.isEmpty() || folder.trimmed().isEmpty()) return;
            const QString q = folderQuery(muRoot, a.maildirRoot, folder);
            if (q.isEmpty()) return;
            QStringList seg;
            for (const QString& part : folder.split(QLatin1Char('/'), Qt::SkipEmptyParts))
                seg += part.startsWith(QLatin1Char('.')) ? part.split(QLatin1Char('.'), Qt::SkipEmptyParts) : QStringList{part};
            if (seg.isEmpty()) return;
            QStandardItem* parent = top;
            QString key;
            for (int i = 0; i < seg.size(); ++i) {
                key += QLatin1Char('\x1f') + seg.at(i);
                QStandardItem*& n = nodes[key];
                if (!n) {
                    n = new QStandardItem(seg.at(i));
                    parent->appendRow(n);
                }
                parent = n;
            }
            parent->setText(label.isEmpty() ? seg.last() : label);
            parent->setData(q, QueryRole);
            parent->setData(true, FolderRole);
            parent->setData(QDir::cleanPath(QDir(a.maildirRoot).filePath(folder.trimmed())), Qt::UserRole + 6);
            parent->setData(a.id, Qt::UserRole + 7);
        };
        add(tr("Posta in arrivo"), QStringLiteral("INBOX"));
        add(tr("Inviata"), a.sentFolder);
        add(tr("Bozze"), a.draftsFolder);
        add(tr("Cestino"), a.trashFolder);
        if (!muRoot.isEmpty()) { // le altre cartelle presenti su disco
            QSet<QString> shown{QStringLiteral("INBOX"), a.sentFolder, a.draftsFolder, a.trashFolder};
            for (const QString& f : scanFolders(a.maildirRoot))
                if (!shown.contains(f)) add(QString(), f);
        }
        model_->appendRow(top);
    }
    // etichetta di base di ogni nodo (il testo mostrato puo' avere il conteggio)
    std::function<void(QStandardItem*)> mark = [&](QStandardItem* it) {
        it->setData(it->text(), Qt::UserRole + 5);
        for (int i = 0; i < it->rowCount(); ++i) mark(it->child(i));
    };
    for (int i = 0; i < model_->rowCount(); ++i) mark(model_->item(i));
    expandAll();
}

QStringList FolderPane::countableQueries() const {
    QStringList out;
    std::function<void(QStandardItem*)> walk = [&](QStandardItem* it) {
        if (it->data(Qt::UserRole + 2).toBool()) out << it->data(Qt::UserRole + 1).toString();
        for (int i = 0; i < it->rowCount(); ++i) walk(it->child(i));
    };
    for (int i = 0; i < model_->rowCount(); ++i) walk(model_->item(i));
    return out;
}

void FolderPane::setUnreadCounts(const QHash<QString, int>& byQuery) {
    // -1 = sconosciuto
    std::function<int(QStandardItem*)> apply = [&](QStandardItem* it) -> int {
        int sum = 0;
        bool known = false;
        for (int i = 0; i < it->rowCount(); ++i) {
            const int c = apply(it->child(i));
            if (c >= 0) { sum += c; known = true; }
        }
        int n = -1;
        if (it->data(Qt::UserRole + 2).toBool()) {
            const QString q = it->data(Qt::UserRole + 1).toString();
            n = byQuery.contains(q) ? byQuery.value(q) : -1;
        } else if (known) {
            n = sum;
        }
        const QString base = it->data(Qt::UserRole + 5).toString();
        it->setText(n > 0 ? QStringLiteral("%1 (%2)").arg(base).arg(n) : base);
        QFont f = it->font();
        f.setBold(n > 0);
        it->setFont(f);
        return n;
    };
    for (int i = 0; i < model_->rowCount(); ++i) apply(model_->item(i));
}

} // namespace boma
