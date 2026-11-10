#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace boma {

struct Attachment {
    QString filename;  // gia' sanificato: componente singolo, mai vuoto
    QString mimeType;
    qint64 size = 0;   // dimensione decodificata
    int partIndex = -1;
};

struct ParsedMessage {
    QString from, to, cc, subject;
    QDateTime date;
    QString plainText, html;
    QList<Attachment> attachments;
    bool signedPgp = false, encryptedPgp = false;
    // PGP/MIME firmato: byte esatti della prima parte come trasmessa e firma
    // decodificata. PGP/MIME cifrato: contenuto del testo cifrato.
    // Boma non verifica ne' decifra qui.
    QByteArray rawSignedPart, rawSignature;
    QByteArray rawEncrypted;
};

// Non solleva mai e non va in crash su input arbitrari (vuoto, spazzatura,
// troncato). *error e' valorizzato se l'input non e' interpretabile.
// partIndex = posizione (da 0) delle parti foglia non multipart, in ordine di
// visita in profondita'.
class MessageParser {
public:
    static ParsedMessage parse(const QByteArray& raw, QString* error = nullptr);
    static QByteArray attachmentData(const QByteArray& raw, int partIndex);
    // Mai sovrascrive: fallisce se destPath esiste. Scrive in un file
    // temporaneo nella stessa cartella e lo pubblica con link/rename atomico.
    static bool saveAttachment(const QByteArray& raw, int partIndex, const QString& destPath,
                               QString* error = nullptr);
    // Nome sicuro da usare come proposta di salvataggio.
    static QString sanitizeFilename(const QString& name);
};

} // namespace boma
