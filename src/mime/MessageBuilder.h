#pragma once
#include "core/Config.h"
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace boma {

struct OutgoingMessage {
    Identity identity;
    QStringList to, cc, bcc;           // elementi "Nome <a@b>" oppure "a@b"
    QString subject, plainText, html;
    QList<QString> attachmentPaths;
    QString inReplyToMessageId;        // con o senza <>
    QStringList referencesIds;         // catena References precedente (con o senza <>)
};

// Costruisce i byte RFC 5322/MIME con GMime.
// - Line ending: l'output usa LF (formato predefinito di GMime), adatto alla
//   Maildir; msmtp legge da stdin e accetta LF (lo converte in CRLF da se').
// - Bcc non compare MAI nelle intestazioni: i destinatari della busta si
//   ottengono con envelopeRecipients().
// - Nessun User-Agent/X-Mailer (riservatezza).
// - Intestazioni: indirizzi/id con caratteri di controllo o non validi fanno
//   fallire la costruzione (mai inviare a un indirizzo alterato); l'oggetto
//   ha gli a-capo sostituiti da spazi.
// - Allegati: letti solo dai percorsi indicati; mancante/illeggibile = errore.
//   Limiti: 25 MiB per allegato, 40 MiB in totale (limiti tipici dei server).
// - Se html e' non vuoto: multipart/alternative (testo prima). Se il testo e'
//   vuoto, lo si ricava dall'HTML (QTextDocumentFragment::toPlainText).
// - La firma dell'identita' e' aggiunta dopo "-- " (RFC 3676), anche all'HTML.
class MessageBuilder {
public:
    static constexpr qint64 kMaxAttachmentBytes = 25LL * 1024 * 1024;
    static constexpr qint64 kMaxTotalAttachmentBytes = 40LL * 1024 * 1024;

    // Array vuoto con *error valorizzato in caso di fallimento.
    static QByteArray build(const OutgoingMessage& msg, QString* error);
    // to+cc+bcc: indirizzi nudi, senza duplicati (case-insensitive), elementi non validi scartati.
    static QStringList envelopeRecipients(const OutgoingMessage& msg);
};

} // namespace boma

Q_DECLARE_METATYPE(boma::OutgoingMessage)
