#pragma once
#include "mime/MessageParser.h"
#include "pgp/GpgService.h"
#include <QPushButton>
#include <functional>
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

    // Esito della verifica della firma (badge nell'intestazione). show() lo azzera.
    void setSignatureResult(SignatureStatus status, const QString& signer);
    // Messaggi cifrati: il pulsante "Decifra" chiede conferma e poi emette decryptRequested().
    // Il proprietario chiama gpg e risponde con showDecrypted()/showDecryptError().
    void showDecrypted(const ParsedMessage& m);
    void showDecryptError(const QString& error);
    QPushButton* decryptButton() const { return decryptButton_; }
    const QByteArray& encryptedData() const { return encrypted_; }
    // Sostituibile nei test (default: QMessageBox::question).
    void setConfirmCallback(std::function<bool()> cb) { confirm_ = std::move(cb); }
    // consumed (opzionale): caratteri di input elaborati (< size se troncato per numero di tag).
    static QString sanitizeHtml(const QString& html, qsizetype* consumed = nullptr);

signals:
    void linkActivated(const QUrl& url);
    void decryptRequested();

protected:
    void resizeEvent(QResizeEvent* e) override;
    QVariant loadResource(int type, const QUrl& name) override;

private:
    void render();
    void layoutBar();
    void onDecryptClicked();
    QString badgeHtml() const;

    ParsedMessage msg_;
    SignatureStatus sigStatus_ = SignatureStatus::None;
    QString signer_, decryptError_;
    QByteArray encrypted_;
    bool encryptedShown_ = false;
    QWidget* bar_ = nullptr;
    QPushButton* decryptButton_ = nullptr;
    std::function<bool()> confirm_;
};

} // namespace boma
