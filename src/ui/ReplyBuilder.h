#pragma once
#include "core/Config.h"
#include "mime/MessageBuilder.h"
#include "mime/MessageParser.h"

namespace boma {

enum class ReplyMode { Reply, ReplyAll, Forward };

// Prepara il contenuto iniziale di una risposta/inoltro (per ComposeWindow::setPrefill).
// - Oggetto: "Re: "/"Fwd: " senza accumulare prefissi esistenti.
// - Citazione con "> ". In Forward il testo e' inserito dopo un'intestazione
//   "Messaggio inoltrato" (gli allegati originali non sono inoltrati).
// - Reply: a Reply-To (o Da); se il Da e' una identita' propria, ai destinatari
//   originali. ReplyAll: aggiunge To e Cc originali, SENZA le identita' proprie
//   (di tutti gli account) e senza duplicati. Il Bcc non compare mai (il
//   ParsedMessage non lo contiene).
// - Identita': dell'account del messaggio; quella il cui indirizzo e' tra
//   To/Cc, altrimenti la prima dell'account, altrimenti la prima in assoluto.
// - In-Reply-To = Message-ID dell'originale; References = catena + Message-ID.
OutgoingMessage buildReply(const ParsedMessage& m, const QString& accountId, const Config& config, ReplyMode mode);

// Contenuto iniziale per riaprire in modifica una bozza salvata (ComposeWindow::setPrefill).
// - Destinatari: A/Cc/Ccn dalle intestazioni, piu' quelli che GMime non sa interpretare
//   (rawTo/rawCc/rawBcc), cosi' nessun destinatario si perde.
// - Identita': quella il cui indirizzo e' il Da della bozza; se non c'e' (bozza di un altro
//   client) la prima dell'account, e *identityMatched e' false.
// - attachmentPaths: file gia' estratti dal chiamante (la bozza non contiene percorsi).
// - In-Reply-To e' l'ultimo elemento di References (e' cosi' che MessageBuilder li scrive).
OutgoingMessage draftToOutgoing(const ParsedMessage& draft, const QString& accountId, const Config& config,
                                const QStringList& attachmentPaths, bool* identityMatched = nullptr);

// Indirizzo nudo (minuscolo) di un elemento "Nome <a@b>" o "a@b".
QString bareAddress(const QString& item);

} // namespace boma
