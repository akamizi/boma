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

// Indirizzo nudo (minuscolo) di un elemento "Nome <a@b>" o "a@b".
QString bareAddress(const QString& item);

} // namespace boma
