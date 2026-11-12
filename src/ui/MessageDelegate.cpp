#include "ui/MessageDelegate.h"
#include "ui/MessageListModel.h"

#include <QApplication>
#include <QLocale>
#include <QPainter>
#include <QStyleOptionButton>

namespace boma {

QString MessageDelegate::formatSize(qint64 b) {
    if (b < 1024) return QStringLiteral("%1 B").arg(b);
    if (b < 1024 * 1024) return QStringLiteral("%1 KiB").arg(qMax<qint64>(1, (b + 512) / 1024));
    return QStringLiteral("%1 MiB").arg(b / (1024.0 * 1024.0), 0, 'f', 1);
}

QString MessageDelegate::formatDate(const QDateTime& d, const QDateTime& now) {
    if (!d.isValid()) return {};
    const qint64 s = d.secsTo(now);
    if (s >= 0 && s < 60) return QObject::tr("adesso");
    if (s >= 60 && s < 3600) return QObject::tr("%1 min fa").arg(s / 60);
    if (s >= 3600 && s < 6 * 3600) return QObject::tr("%1 ore fa").arg(s / 3600);
    const QLocale loc;
    if (d.date() == now.date()) return QObject::tr("oggi alle %1").arg(d.time().toString(QStringLiteral("HH:mm")));
    if (d.date().daysTo(now.date()) == 1) return QObject::tr("ieri alle %1").arg(d.time().toString(QStringLiteral("HH:mm")));
    if (d.date().year() == now.date().year())
        return QStringLiteral("%1 %2, %3").arg(d.date().day()).arg(loc.monthName(d.date().month(), QLocale::ShortFormat)).arg(d.time().toString(QStringLiteral("HH:mm")));
    return loc.toString(d.date(), QLocale::ShortFormat);
}

QRect MessageDelegate::checkRect(const QRect& r) {
    return QRect(r.left() + 12, r.center().y() - 9, 18, 18);
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem& o, const QModelIndex&) const {
    const int h = o.fontMetrics.height();
    return {200, h * 2 + 18};
}

void MessageDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& i) const {
    p->save();
    const bool unread = i.data(MessageListModel::UnreadRole).toBool();
    const bool sel = opt.state & QStyle::State_Selected;
    const QPalette& pal = opt.palette;
    QRect r = opt.rect;
    if (sel) p->fillRect(r, pal.highlight());
    else if (unread) {
        QColor c = pal.color(QPalette::Highlight);
        c.setAlpha(pal.color(QPalette::Base).lightness() < 128 ? 55 : 35);
        p->fillRect(r, c);
    }
    if (unread) p->fillRect(QRect(r.left(), r.top(), 4, r.height()), pal.color(QPalette::Highlight));
    p->setPen(pal.color(QPalette::Mid));
    p->drawLine(r.bottomLeft(), r.bottomRight());

    const QColor fg = sel ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text);
    const QColor dim = sel ? fg : pal.color(QPalette::PlaceholderText);
    QFont bold = opt.font;
    bold.setBold(unread);
    const QFontMetrics fm(opt.font), fb(bold);
    const int pad = 10, h = fm.height();
    const QRect in = r.adjusted(pad + 4 + 28, 6, -pad, -6);
    {
        QStyleOptionButton cb;
        cb.rect = checkRect(r);
        cb.state = QStyle::State_Enabled | (i.data(MessageListModel::CheckedRole).toBool() ? QStyle::State_On : QStyle::State_Off);
        cb.palette = pal;
        const QWidget* w = opt.widget;
        (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_CheckBox, &cb, p, w);
    }
    const int l1 = in.top(), l2 = in.top() + h + 4;

    // linea 1: data (e stella) a destra, mittente a sinistra
    const bool flagged = i.data(MessageListModel::FlaggedRole).toBool();
    const bool attach = i.data(MessageListModel::HasAttachmentRole).toBool();
    const int iconW = h + 2;
    const QString date = formatDate(i.data(MessageListModel::DateRole).toDateTime());
    p->setFont(opt.font);
    p->setPen(dim);
    const int right = in.right() - iconW;
    const int dateW = fm.horizontalAdvance(date);
    p->drawText(QRect(right - dateW, l1, dateW, h), Qt::AlignRight | Qt::AlignVCenter, date);
    if (flagged) {
        p->setPen(QColor(0xf5, 0xa6, 0x23));
        p->drawText(QRect(in.right() - iconW + 2, l1, iconW, h), Qt::AlignCenter, QStringLiteral("★"));
    }
    p->setFont(bold);
    p->setPen(fg);
    p->drawText(QRect(in.left(), l1, right - dateW - in.left() - 8, h), Qt::AlignLeft | Qt::AlignVCenter,
                fb.elidedText(i.data(MessageListModel::FromRole).toString(), Qt::ElideRight, right - dateW - in.left() - 8));

    // linea 2: dimensione (e graffetta) a destra, oggetto a sinistra
    const QString size = formatSize(i.data(MessageListModel::SizeRole).toLongLong());
    const int sizeW = fm.horizontalAdvance(size);
    p->setFont(opt.font);
    p->setPen(dim);
    p->drawText(QRect(right - sizeW, l2, sizeW, h), Qt::AlignRight | Qt::AlignVCenter, size);
    if (attach) p->drawText(QRect(in.right() - iconW + 2, l2, iconW, h), Qt::AlignCenter, QStringLiteral("📎"));
    p->setFont(bold);
    p->setPen(fg);
    const int sw = right - sizeW - in.left() - 8;
    p->drawText(QRect(in.left(), l2, sw, h), Qt::AlignLeft | Qt::AlignVCenter,
                fb.elidedText(i.data(MessageListModel::SubjectRole).toString(), Qt::ElideRight, sw));
    p->restore();
}

} // namespace boma
