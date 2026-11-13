#pragma once
#include <QAction>
#include <QTextCharFormat>
#include <QToolBar>
#include <functional>

class QTextEdit;
class QFontComboBox;
class QComboBox;

namespace boma {

// Barra di formattazione per un QTextEdit (editor WYSIWYG della composizione HTML):
// grassetto, corsivo, sottolineato, barrato, dimensione, colore, elenchi, allineamento,
// collegamento, rimozione formattazione. Le azioni hanno objectName stabili (per i test).
// I collegamenti ammessi sono solo http, https e mailto.
class RichTextToolBar : public QToolBar {
    Q_OBJECT
public:
    explicit RichTextToolBar(QTextEdit* edit, QWidget* parent = nullptr);
    // Sostituibile nei test: restituisce l'URL da applicare (vuoto = annulla).
    void setLinkPrompt(std::function<QString(const QString& current)> p) { linkPrompt_ = std::move(p); }
    static bool isAllowedLink(const QString& url);

private:
    void syncState();
    void applyFormat(const QTextCharFormat& f);
    QAction* addToggle(const QString& text, const QString& name, const QString& tip);
    QTextEdit* edit_;
    QAction *bold_, *italic_, *underline_, *strike_, *bullets_, *numbers_;
    QAction *left_, *center_, *right_;
    QComboBox* size_;
    std::function<QString(const QString&)> linkPrompt_;
};

} // namespace boma
