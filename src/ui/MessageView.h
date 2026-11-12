#pragma once
#include "mime/MessageParser.h"
#include "pgp/GpgService.h"
#include <QLabel>
#include <QPushButton>
#include <functional>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrl>

namespace boma {

// Vista sicura: nessun contenuto remoto viene mai caricato, i link non si
// aprono da soli (il segnale linkActivated lascia decidere alla UI).
class MessageView : public QTextBrowser {
    Q_OBJECT
public:
    explicit MessageView(QWidget* parent = nullptr);
    void show(const ParsedMessage& m);

    // Preferenza "mostra in HTML quando il messaggio ha anche la parte HTML" (default:
    // testo). Sta in QSettings (view/preferHtml); vale per i messaggi aperti dopo la
    // modifica. Il pulsante nella barra la inverte per il solo messaggio corrente.
    static bool preferHtml();
    static void setPreferHtml(bool on);
    void reloadPreference() { defaultHtml_ = preferHtml(); }
    QPushButton* htmlToggleButton() const { return htmlToggle_; }
    bool showingHtml() const;

    // Esito della verifica della firma (badge nell'intestazione). show() lo azzera.
    void setSignatureResult(SignatureStatus status, const QString& signer);
    // Messaggi cifrati: il pulsante "Decifra" chiede conferma e poi emette decryptRequested().
    // Il proprietario chiama gpg e risponde con showDecrypted()/showDecryptError().
    void showDecrypted(const ParsedMessage& m);
    void showDecryptError(const QString& error);
    QPushButton* decryptButton() const { return decryptButton_; }
    // "Salva allegato...": un'azione per allegato; emette saveAttachmentRequested(i)
    // (indice in message().attachments). Nulla si apre o si salva da solo.
    QToolButton* attachmentButton() const { return attachButton_; }
    const ParsedMessage& message() const { return msg_; }
    // Badge e avvisi sono widget FUORI dal QTextDocument: l'HTML del mittente
    // non puo' disegnarli ne' imitarli.
    QLabel* signatureBadge() const { return badge_; }
    QLabel* decryptErrorLabel() const { return errLabel_; }
    const QByteArray& encryptedData() const { return encrypted_; }
    // Sostituibile nei test (default: QMessageBox::question).
    void setConfirmCallback(std::function<bool()> cb) { confirm_ = std::move(cb); }
    // consumed (opzionale): caratteri di input elaborati (< size se troncato per numero di tag).
    static QString sanitizeHtml(const QString& html, qsizetype* consumed = nullptr);

signals:
    void linkActivated(const QUrl& url);
    void decryptRequested();
    void saveAttachmentRequested(int index);

protected:
    void resizeEvent(QResizeEvent* e) override;
    QVariant loadResource(int type, const QUrl& name) override;

private:
    void render();
    void layoutBar();
    void onDecryptClicked();
    void updateBadge();

    ParsedMessage msg_;
    SignatureStatus sigStatus_ = SignatureStatus::None;
    QString signer_, decryptError_;
    QByteArray encrypted_;
    bool encryptedShown_ = false;
    QWidget* bar_ = nullptr;
    QLabel *badge_ = nullptr, *errLabel_ = nullptr;
    QWidget* encRow_ = nullptr;
    QPushButton* decryptButton_ = nullptr;
    QToolButton* attachButton_ = nullptr;
    QPushButton* htmlToggle_ = nullptr;
    bool defaultHtml_ = false, htmlOverride_ = false, wantHtml_ = false;
    void updateAttachments();
    std::function<bool()> confirm_;
};

} // namespace boma
