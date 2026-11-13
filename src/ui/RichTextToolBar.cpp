#include "ui/RichTextToolBar.h"
#include <QActionGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QInputDialog>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextList>
#include <QUrl>
#include <functional>

namespace boma {

bool RichTextToolBar::isAllowedLink(const QString& url) {
    const QUrl u(url.trimmed(), QUrl::StrictMode);
    if (!u.isValid()) return false;
    const QString s = u.scheme().toLower();
    return s == QLatin1String("http") || s == QLatin1String("https") || s == QLatin1String("mailto");
}

QAction* RichTextToolBar::addToggle(const QString& text, const QString& name, const QString& tip) {
    QAction* a = addAction(text);
    a->setObjectName(name);
    a->setCheckable(true);
    a->setToolTip(tip);
    return a;
}

RichTextToolBar::RichTextToolBar(QTextEdit* edit, QWidget* parent) : QToolBar(parent), edit_(edit) {
    setObjectName(QStringLiteral("richToolBar"));
    setMovable(false);
    setToolButtonStyle(Qt::ToolButtonTextOnly);

    bold_ = addToggle(QStringLiteral("B"), QStringLiteral("fmtBold"), tr("Grassetto (Ctrl+B)"));
    QFont f = widgetForAction(bold_)->font();
    f.setBold(true);
    widgetForAction(bold_)->setFont(f);
    italic_ = addToggle(QStringLiteral("I"), QStringLiteral("fmtItalic"), tr("Corsivo (Ctrl+I)"));
    f = widgetForAction(italic_)->font(); f.setItalic(true); widgetForAction(italic_)->setFont(f);
    underline_ = addToggle(QStringLiteral("U"), QStringLiteral("fmtUnderline"), tr("Sottolineato (Ctrl+U)"));
    f = widgetForAction(underline_)->font(); f.setUnderline(true); widgetForAction(underline_)->setFont(f);
    strike_ = addToggle(QStringLiteral("S"), QStringLiteral("fmtStrike"), tr("Barrato"));
    f = widgetForAction(strike_)->font(); f.setStrikeOut(true); widgetForAction(strike_)->setFont(f);
    addSeparator();

    size_ = new QComboBox(this);
    size_->setObjectName(QStringLiteral("fmtSize"));
    size_->setToolTip(tr("Dimensione del testo"));
    for (int p : {9, 10, 11, 12, 14, 16, 18, 24, 32}) size_->addItem(QString::number(p), p);
    size_->setCurrentText(QString::number(11));
    addWidget(size_);
    QAction* color = addAction(tr("Colore"));
    color->setObjectName(QStringLiteral("fmtColor"));
    color->setToolTip(tr("Colore del testo"));
    addSeparator();

    bullets_ = addToggle(tr("• Elenco"), QStringLiteral("fmtBullets"), tr("Elenco puntato"));
    numbers_ = addToggle(tr("1. Elenco"), QStringLiteral("fmtNumbers"), tr("Elenco numerato"));
    addSeparator();
    auto* grp = new QActionGroup(this);
    left_ = addToggle(tr("Sx"), QStringLiteral("fmtLeft"), tr("Allinea a sinistra"));
    center_ = addToggle(tr("Cx"), QStringLiteral("fmtCenter"), tr("Centra"));
    right_ = addToggle(tr("Dx"), QStringLiteral("fmtRight"), tr("Allinea a destra"));
    for (QAction* a : {left_, center_, right_}) grp->addAction(a);
    addSeparator();
    QAction* link = addAction(tr("Link"));
    link->setObjectName(QStringLiteral("fmtLink"));
    link->setToolTip(tr("Inserisci o modifica un collegamento (http, https, mailto)"));
    QAction* clear = addAction(tr("Pulisci"));
    clear->setObjectName(QStringLiteral("fmtClear"));
    clear->setToolTip(tr("Rimuove la formattazione della selezione"));

    connect(bold_, &QAction::triggered, this, [this](bool on) {
        QTextCharFormat c; c.setFontWeight(on ? QFont::Bold : QFont::Normal); applyFormat(c); });
    connect(italic_, &QAction::triggered, this, [this](bool on) { QTextCharFormat c; c.setFontItalic(on); applyFormat(c); });
    connect(underline_, &QAction::triggered, this, [this](bool on) { QTextCharFormat c; c.setFontUnderline(on); applyFormat(c); });
    connect(strike_, &QAction::triggered, this, [this](bool on) { QTextCharFormat c; c.setFontStrikeOut(on); applyFormat(c); });
    connect(size_, &QComboBox::activated, this, [this](int i) {
        QTextCharFormat c; c.setFontPointSize(size_->itemData(i).toInt()); applyFormat(c); edit_->setFocus(); });
    connect(color, &QAction::triggered, this, [this]() {
        const QColor col = QColorDialog::getColor(edit_->textColor(), this, tr("Colore del testo"));
        if (!col.isValid()) return;
        QTextCharFormat c; c.setForeground(col); applyFormat(c); });
    auto list = [this](QTextListFormat::Style st, QAction* me, QAction* other) {
        QTextCursor cur = edit_->textCursor();
        other->setChecked(false);
        if (!me->isChecked()) {
            QTextBlockFormat bf; bf.setIndent(0);
            cur.beginEditBlock();
            if (cur.currentList()) cur.currentList()->remove(cur.block());
            QTextBlockFormat nb = cur.blockFormat(); nb.setIndent(0); cur.setBlockFormat(nb);
            cur.endEditBlock();
        } else {
            QTextListFormat lf; lf.setStyle(st); cur.createList(lf);
        }
        edit_->setFocus();
    };
    connect(bullets_, &QAction::triggered, this, [=]() { list(QTextListFormat::ListDisc, bullets_, numbers_); });
    connect(numbers_, &QAction::triggered, this, [=]() { list(QTextListFormat::ListDecimal, numbers_, bullets_); });
    connect(left_, &QAction::triggered, this, [this]() { edit_->setAlignment(Qt::AlignLeft); });
    connect(center_, &QAction::triggered, this, [this]() { edit_->setAlignment(Qt::AlignHCenter); });
    connect(right_, &QAction::triggered, this, [this]() { edit_->setAlignment(Qt::AlignRight); });
    connect(link, &QAction::triggered, this, [this]() {
        QTextCursor cur = edit_->textCursor();
        const QString current = edit_->textCursor().charFormat().anchorHref();
        QString url = linkPrompt_ ? linkPrompt_(current)
                                  : QInputDialog::getText(this, tr("Collegamento"), tr("Indirizzo (http, https o mailto):"),
                                                          QLineEdit::Normal, current);
        url = url.trimmed();
        if (url.isEmpty()) {
            if (!current.isEmpty()) { // togli il collegamento
                QTextCharFormat c; c.setAnchor(false); c.setAnchorHref(QString()); c.setFontUnderline(false);
                c.setForeground(edit_->palette().text()); applyFormat(c);
            }
            return;
        }
        if (!isAllowedLink(url)) return;
        QTextCharFormat c;
        c.setAnchor(true); c.setAnchorHref(url); c.setFontUnderline(true); c.setForeground(QColor(0x1a, 0x5f, 0xb4));
        if (!cur.hasSelection()) {
            cur.insertText(url, c);
            edit_->setTextCursor(cur);
        } else applyFormat(c);
    });
    connect(clear, &QAction::triggered, this, [this]() {
        QTextCursor cur = edit_->textCursor();
        QTextCharFormat plain;
        plain.setFontPointSize(11);
        cur.setCharFormat(plain);
        if (cur.hasSelection()) cur.mergeCharFormat(plain);
        edit_->setTextCursor(cur);
        syncState();
    });
    connect(edit_, &QTextEdit::cursorPositionChanged, this, &RichTextToolBar::syncState);
    connect(edit_, &QTextEdit::currentCharFormatChanged, this, &RichTextToolBar::syncState);
}

void RichTextToolBar::applyFormat(const QTextCharFormat& f) {
    QTextCursor cur = edit_->textCursor();
    if (!cur.hasSelection()) cur.select(QTextCursor::WordUnderCursor);
    // Senza selezione e senza parola: cambia il formato di digitazione.
    if (cur.hasSelection()) cur.mergeCharFormat(f);
    edit_->mergeCurrentCharFormat(f);
    edit_->setFocus();
}

void RichTextToolBar::syncState() {
    const QTextCharFormat f = edit_->currentCharFormat();
    bold_->setChecked(f.fontWeight() >= QFont::Bold);
    italic_->setChecked(f.fontItalic());
    underline_->setChecked(f.fontUnderline());
    strike_->setChecked(f.fontStrikeOut());
    QTextList* l = edit_->textCursor().currentList();
    bullets_->setChecked(l && l->format().style() == QTextListFormat::ListDisc);
    numbers_->setChecked(l && l->format().style() == QTextListFormat::ListDecimal);
    const Qt::Alignment a = edit_->alignment();
    left_->setChecked(a & Qt::AlignLeft);
    center_->setChecked(a & Qt::AlignHCenter);
    right_->setChecked(a & Qt::AlignRight);
    const int pt = int(f.fontPointSize());
    if (pt > 0) {
        const int i = size_->findData(pt);
        if (i >= 0) size_->setCurrentIndex(i);
    }
}

} // namespace boma
