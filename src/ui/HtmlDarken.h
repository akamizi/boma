#pragma once
#include <QString>

namespace boma {

// Adatta un HTML GIA' ripulito (MessageView::sanitizeHtml) a una palette scura: i colori di
// testo, sfondo e bordo (attributi bgcolor/color/text/link, proprieta' CSS in style="" e in
// <style>) vengono rimappati invertendo la luminosita' e conservando tinta e saturazione.
// Non si tocca il testo dei messaggi, ne' le immagini.
// Se il messaggio dichiara uno sfondo gia' scuro il testo e' lasciato com'e'.
// changed (opzionale): true se l'HTML restituito differisce dall'originale.
QString darkenHtml(const QString& sanitizedHtml, bool* changed = nullptr);

} // namespace boma
