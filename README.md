# Boma

Client email desktop per Linux, scritto in C++20 con Qt6 Widgets.

Boma non è un client email "monolitico": è un'**interfaccia grafica che coordina
programmi Unix esistenti e collaudati**, ognuno incaricato di una sola cosa. Boma
non reimplementa IMAP, SMTP, l'indicizzazione o la crittografia: lancia i
programmi giusti come processi separati e mostra il risultato.

Stato: in sviluppo (prima versione). Le funzioni descritte sotto sono l'obiettivo
della v1; man mano che vengono realizzate il README viene aggiornato.

## Spirito del progetto

- **Filosofia Unix**: tanti programmi piccoli, ognuno fa bene una sola cosa; il
  client li mette insieme con un'interfaccia grafica. Ogni programma è
  sostituibile, perché ci si parla solo con file di testo e righe di comando.
- **Su misura**: poche funzioni, scelte per un uso reale, aggiunte solo quando
  servono davvero. Niente calendario, contatti, plugin o temi.
- **Non perdere né corrompere mai la posta**: è il criterio che prevale su tutti
  gli altri. In caso di dubbio Boma si ferma e avvisa, senza "riparare" in
  silenzio. Cancellare significa spostare nel cestino sul server; Boma non
  elimina mai definitivamente la posta in automatico.
- **Leggero e veloce**: ricerca sotto i 200 ms su 15 GB di posta, avvio sotto 1
  secondo, meno di 150 MB di memoria a riposo. Per questo niente QML né
  QtWebEngine: l'HTML dei messaggi è reso da `QTextBrowser`, senza caricare
  immagini remote.
- **Copia locale completa**: tutta la posta è sul disco, anche quella vecchia,
  leggibile offline. Il server IMAP resta la copia di riferimento, quindi la
  stessa posta si legge anche dal telefono.

## Chi fa che cosa

| Compito | Programma | Come lo usa Boma |
|---|---|---|
| Sincronizzazione IMAP bidirezionale | **mbsync** (pacchetto `isync`) | `mbsync -V <canale>` come processo asincrono: all'avvio, a intervalli e su richiesta; mai due insieme (blocco con `flock`). Le password IMAP le fornisce `PassCmd` nel suo file di configurazione, Boma non le vede. |
| Archivio dei messaggi | **Maildir** (un file per messaggio) | Copia locale; Boma ci scrive direttamente solo per Inviata e Bozze (vedi sotto). |
| Indice e ricerca | **mu** (pacchetto `mu4e`) | `mu index` dopo ogni sincronizzazione, `mu find` per le ricerche e per le non lette, `mu move` per spostare i messaggi (ad esempio nel cestino). |
| Invio | **msmtp** | Un account msmtp per ogni identità; le credenziali le recupera `passwordeval` nella sua configurazione. |
| Analisi e costruzione dei messaggi MIME | **GMime 3** (libreria) | Intestazioni, parti, allegati e firme; messaggi in uscita. |
| Firma, cifratura, verifica, decifratura | **GnuPG** (`gpg`) con **gpg-agent** e **pinentry-qt** | Formato PGP/MIME. La password della chiave la chiede pinentry e la conserva gpg-agent per 5 minuti (`default-cache-ttl`); Boma non la vede mai. |
| Interfaccia | **Qt6 Widgets** | Tre colonne (cartelle, elenco, lettura) con barra di ricerca; all'avvio mostra le non lette. |
| Filtri in ingresso | **Sieve** sul server | Non è compito di Boma. In Boma "filtrare" significa cercare. |

L'unico punto in cui Boma scrive direttamente nel Maildir è la copia dei messaggi
inviati (Inviata) e delle bozze (Bozze), con la consegna Maildir standard (file
scritto in `tmp/` e poi spostato con `rename`); tutto il resto passa da `mbsync`
e `mu`.

## Funzioni della prima versione

- Due account IMAP completamente indipendenti, con copia locale completa
  (la prima sincronizzazione è lunga: ha avanzamento visibile e riprende dopo
  un'interruzione).
- Elenco per messaggio (senza conversazioni), vista delle non lette all'avvio,
  ricerca veloce.
- Testo semplice, HTML visualizzabile senza immagini remote, allegati.
- Più identità per account (indirizzo, firma, chiave PGP, account msmtp). La copia
  inviata va nella cartella Inviata dell'account a cui appartiene l'identità.
- OpenPGP: firma attiva di default (disattivabile), cifratura su richiesta; spunta
  verde per le firme valide; per i messaggi cifrati si chiede conferma prima di
  decifrare. Boma non invia mai in chiaro in silenzio se manca una chiave, e se la
  firma fallisce l'invio si ferma e la bozza si conserva.
- Cancellazione = spostamento nel cestino sul server. La cancellazione definitiva
  automatica non è prevista.

Non previsto nella v1: conversazioni, ricerche salvate, gestione delle chiavi
(si usa direttamente `gpg`), cancellazione automatica del cestino.

## Requisiti

- CMake >= 3.20 e un compilatore C++20
- Qt6 (Widgets, Test): `qt6-base-devel`
- GMime 3: `gmime-devel`
- Programmi a runtime: `mbsync` (`isync`), `mu` (`mu4e`), `msmtp`, `gnupg`,
  `pinentry-qt`

Piattaforma di riferimento: Void Linux (glibc, x86_64).

## Build e test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/boma
```

## Configurazione

Boma non sostituisce i file di configurazione dei programmi che usa; li lascia
dove sono.

- `~/.mbsyncrc`: account IMAP, canali e cartelle da sincronizzare con `mbsync`
  (Maildir locale), con `PassCmd` per la password.
- `~/.msmtprc`: account SMTP usati da `msmtp` per l'invio, con `passwordeval`.
- `~/.gnupg/gpg-agent.conf`: per tenere la password della chiave in cache al
  massimo 5 minuti:

```
default-cache-ttl 300
max-cache-ttl 300
```

- File di Boma (INI), con account (`[account:<id>]`) e identità
  (`[identity:<id>]`): per ogni account la cartella Maildir, il canale `mbsync` e
  le cartelle Inviata, Bozze e Cestino; per ogni identità l'indirizzo, il nome, la
  firma testuale, la chiave PGP e l'account `msmtp`.
