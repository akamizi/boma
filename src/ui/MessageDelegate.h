#pragma once
#include <QDateTime>
#include <QStyledItemDelegate>

namespace boma {

// Riga a due linee: mittente (grassetto se non letta) e data, oggetto e dimensione;
// stella per i messaggi contrassegnati, graffetta per gli allegati. Le non lette
// hanno sfondo evidenziato e barra laterale.
class MessageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override;
    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override;
    static QString formatDate(const QDateTime& d, const QDateTime& now = QDateTime::currentDateTime());
    static QString formatSize(qint64 bytes);
};

} // namespace boma
