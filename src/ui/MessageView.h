#pragma once
#include "mime/MessageParser.h"
#include <QTextBrowser>
#include <QUrl>

namespace boma {

// Vista sicura: nessun contenuto remoto viene mai caricato, i link non si
// aprono da soli (il segnale linkActivated lascia decidere alla UI).
class MessageView : public QTextBrowser {
    Q_OBJECT
public:
    explicit MessageView(QWidget* parent = nullptr);
    void show(const ParsedMessage& m);
    static QString sanitizeHtml(const QString& html);

signals:
    void linkActivated(const QUrl& url);

protected:
    QVariant loadResource(int type, const QUrl& name) override;
};

} // namespace boma
