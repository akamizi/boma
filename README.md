# Boma

Client email desktop per Linux, scritto in C++20 con Qt6 Widgets.

Boma non è un client email "monolitico": è un'**interfaccia grafica che coordina
programmi Unix esistenti e collaudati**, ognuno incaricato di una sola cosa. Boma
non reimplementa IMAP, SMTP, l'indicizzazione o la crittografia: lancia i
programmi giusti come processi separati e mostra il risultato.

Stato: prima versione completa nelle funzioni, ma non ancora collaudata sul campo.
Tutte le funzioni descritte sotto esistono nel codice e la suite di test passa
(16 eseguibili di test con `ctest`). Finora però tutto è stato provato **solo con
posta sintetica e con programmi finti** al posto di `mbsync`, `mu`, `msmtp` e `gpg`;
le verifiche con posta, server e chiavi reali sono in corso. Non ci sono ancora
misure di prestazioni su una casella reale: gli obiettivi di velocità e memoria
indicati sotto sono obiettivi di progetto, non risultati misurati.

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
- **Leggero e veloce** (obiettivo, da confermare su posta reale): ricerca sotto i
  200 ms su 15 GB di posta, avvio sotto 1 secondo, meno di 150 MB di memoria a
  riposo. Per questo niente QML né
  QtWebEngine: l'HTML dei messaggi è reso da `QTextBrowser`, senza caricare
  immagini remote.
- **Copia locale completa**: tutta la posta è sul disco, anche quella vecchia,
  leggibile offline. Il server IMAP resta la copia di riferimento, quindi la
  stessa posta si legge anche dal telefono.

## Chi fa che cosa

| Compito | Programma | Come lo usa Boma |
|---|---|---|
| Sincronizzazione IMAP bidirezionale | **mbsync** (pacchetto `isync`) | Il download periodico è del **cron** (`mbsync` seguito da `mu index`): Boma non sincronizza da solo. Il pulsante «Aggiorna» (F5) lancia `mbsync -V <canale>` per ogni account, uno alla volta, e poi `mu index`. Se il cron è in corso `mbsync` trova il canale bloccato e Boma lo segnala senza trattarlo come errore di rete. Le password IMAP le fornisce `PassCmd` nel suo file di configurazione, Boma non le vede. |
| Archivio dei messaggi | **Maildir** (un file per messaggio) | Copia locale; Boma ci scrive direttamente solo per Inviata e Bozze (vedi sotto). |
| Indice e ricerca | **mu** (pacchetto `mu4e`) | `mu index` dopo ogni sincronizzazione, `mu find` per le ricerche e per le non lette, `mu move` per spostare i messaggi nel cestino e per segnarli come letti quando si aprono. |
| Invio | **msmtp** | Un account msmtp per ogni identità; le credenziali le recupera `passwordeval` nella sua configurazione. |
| Analisi e costruzione dei messaggi MIME | **GMime 3** (libreria) | Intestazioni, parti, allegati e firme; messaggi in uscita. |
| Firma, cifratura, verifica, decifratura | **GnuPG** (`gpg`) con **gpg-agent** e **pinentry-qt** | Formato PGP/MIME. La password della chiave la chiede pinentry e la conserva gpg-agent per il tempo che hai configurato (consigliati 5 minuti, vedi «Configurazione»); Boma non la vede mai. |
| Interfaccia | **Qt6 Widgets** | Tre colonne (cartelle, elenco, lettura) con barra di ricerca; all'avvio mostra le non lette. Cartelle mostrate per ogni account: Posta in arrivo, Inviata, Bozze, Cestino. |
| Filtri in ingresso | **Sieve** sul server | Non è compito di Boma. In Boma "filtrare" significa cercare. |

L'unico punto in cui Boma scrive direttamente nel Maildir è la copia dei messaggi
inviati (Inviata) e delle bozze (Bozze), con la consegna Maildir standard: file
scritto e sincronizzato su disco in `tmp/`, poi pubblicato in `cur/` con `link()`
(che non sostituisce mai un file esistente) e infine rimosso da `tmp/` con
`unlink()`. Tutto il resto passa da `mbsync` e `mu`.

## Funzioni della prima versione

- Due account IMAP completamente indipendenti, con copia locale completa
  (scaricata dal cron con `mbsync`, non da Boma).
- Elenco per messaggio (senza conversazioni), vista delle non lette all'avvio,
  ricerca veloce.
- Testo semplice, HTML visualizzabile senza immagini remote, allegati
  («Salva allegato...»: nome ripulito, proposto in Scaricati; un file esistente
  non viene mai sovrascritto).
- I link si aprono solo dopo una conferma che mostra la destinazione reale (i
  domini internazionali in forma ASCII `xn--`), e solo `http`, `https` e
  `mailto`.
- Più identità per account (indirizzo, firma, chiave PGP, account msmtp). La copia
  inviata va nella cartella Inviata dell'account a cui appartiene l'identità,
  con l'intestazione `Bcc` (che invece non viene mai trasmessa). Se Inviata non
  è una Maildir scrivibile l'invio è rifiutato prima di partire (e all'avvio un
  avviso segnala Inviata, Bozze o Cestino non validi). Se il messaggio parte ma la
  copia in Inviata fallisce, Boma ne salva una copia di emergenza `.eml` in
  `~/.local/share/Boma/non-salvati/` e la finestra di scrittura resta aperta,
  segnata come INVIATO e non reinviabile.
- Chiudere Boma passa da ogni finestra di scrittura aperta (che chiede conferma se
  ci sono modifiche) ed è rifiutato mentre un messaggio è in fase di invio.
- OpenPGP: vedi la sezione dedicata più sotto.
- Cancellazione = spostamento nel cestino (pulsante «Elimina», tasto Canc), senza
  conferma ma recuperabile dal cestino. La cancellazione definitiva automatica non
  è prevista. Vedi «Cancellare i messaggi» per il limite importante.

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
dove sono. Senza file di Boma l'applicazione parte e mostra una pagina di aiuto
con un esempio minimo.

### mbsync, msmtp, gpg-agent

- `~/.mbsyncrc`: account IMAP, canali e cartelle da sincronizzare con `mbsync`
  (Maildir locale), con `PassCmd` per la password. Il nome del canale va indicato
  in `mbsyncChannel` nella configurazione di Boma. Per ogni account usa una
  radice Maildir diversa (ad esempio `~/Mail/personale/` e `~/Mail/lavoro/`).
- `~/.msmtprc`: un account SMTP per ogni identità, con `passwordeval`. Boma
  invoca `msmtp -a <account> --read-envelope-from -- <destinatari>`.
- `~/.gnupg/gpg-agent.conf`: per tenere la password della chiave in cache al
  massimo 5 minuti, con pinentry grafico (adatta il percorso di `pinentry-qt`):

```
default-cache-ttl 300
max-cache-ttl 300
pinentry-program /usr/bin/pinentry-qt
```

  Boma non legge né impone questi valori: senza di essi vale la cache predefinita
  di gpg-agent (10 minuti, fino a 2 ore). Dopo la modifica: `gpgconf --kill gpg-agent`.

### Finestra Impostazioni

Il pulsante «Impostazioni» (Ctrl+,) modifica il **solo file di Boma**: account,
identità, cartelle speciali, canale mbsync, account msmtp e chiave PGP. Il
pulsante «Verifica» controlla in **sola lettura** che il canale esista in
`~/.mbsyncrc`, l'account in `~/.msmtprc`, le cartelle Inviata/Bozze/Cestino, che
la chiave sia utilizzabile con `gpg` e la cache di gpg-agent. Boma non scrive mai
i file di mbsync, msmtp o gpg.

Nella stessa finestra si sceglie l'**aspetto**: stile Qt (Fusion, Windows, ...)
e schema colori (sistema, chiaro, scuro; lo schema richiede Qt 6.8 o successivo).
È una preferenza dell'interfaccia, salvata a parte in `~/.config/Boma/Boma.conf`
e applicata subito.

Al salvataggio la configurazione è applicata subito, senza riavviare. Il file
precedente resta in `config.ini.bak`; i commenti del file non sono conservati
(le chiavi di `[general]`, come `muRoot`, sì). L'identificativo di un account o
di un'identità esistente non si cambia. Le Impostazioni si rifiutano di aprirsi
durante un invio, un aggiornamento o con finestre di scrittura aperte (queste
conservano la configurazione con cui sono nate).

### File di Boma

Posizione: `boma/config.ini` nella cartella di configurazione XDG, cioè di norma
`~/.config/boma/config.ini` (è l'unico percorso, non c'è un'opzione per
cambiarlo). Formato INI. Nessuna password nel file. Il tilde `~` è espanso in
`maildirRoot` e `muRoot`.

`[general]` (opzionale)

- `muRoot=`: radice Maildir di `mu` (vedi «Cancellare i messaggi»). Se assente
  viene letta da `mu info store`.

`[account:<id>]`: uno per account IMAP

- `name=` nome mostrato (predefinito: l'id)
- `maildirRoot=` cartella Maildir locale dell'account
- `mbsyncChannel=` canale `mbsync` (predefinito: l'id)
- `sentFolder=`, `draftsFolder=`, `trashFolder=` cartelle speciali, relative a
  `maildirRoot` (predefiniti: `Sent`, `Drafts`, `Trash`)

`[identity:<id>]`: una o più per account

- `accountId=` obbligatorio: l'account a cui appartiene
- `address=` obbligatorio; `displayName=` nome mostrato
- `signature=` firma testuale; la sequenza di due caratteri `\n` diventa un a capo
- `pgpKeyId=` identificativo o impronta della chiave PGP
- `msmtpAccount=` nome dell'account in `~/.msmtprc`
- `signByDefault=` `true` (predefinito) o `false`

Le identità senza `address` o con `accountId` sconosciuto vengono scartate e
l'errore compare nella barra di stato all'avvio. Le chiavi non riconosciute sono
ignorate in silenzio.

Esempio con due account indipendenti, un'identità ciascuno:

```ini
[general]
muRoot=~/Mail

[account:personale]
name=Personale
maildirRoot=~/Mail/personale
mbsyncChannel=personale
sentFolder=Sent
draftsFolder=Drafts
trashFolder=Trash

[account:lavoro]
name=Lavoro
maildirRoot=~/Mail/lavoro
mbsyncChannel=lavoro
sentFolder=Sent
draftsFolder=Drafts
trashFolder=Trash

[identity:personale]
accountId=personale
address=nome@example.org
displayName=Nome Cognome
signature=Nome
pgpKeyId=0123456789ABCDEF0123456789ABCDEF01234567
msmtpAccount=personale

[identity:lavoro]
accountId=lavoro
address=nome.cognome@example.com
displayName=Nome Cognome
msmtpAccount=lavoro
signByDefault=false
```

## Scorciatoie da tastiera

Finestra principale (i tasti semplici non scattano mentre si scrive nel campo di
ricerca):

| Tasto | Azione |
|---|---|
| Ctrl+F oppure `/` | vai alla ricerca (campo vuoto = non lette) |
| F5 | aggiorna ora (`mbsync` + `mu index`) |
| Ctrl+N | nuovo messaggio |
| Ctrl+, | apri le Impostazioni |
| Ctrl+Q | chiudi Boma (come la X della finestra: chiede se c'è un invio o una bozza aperta) |
| `R` / Maiusc+`R` | rispondi / rispondi a tutti |
| `F` | inoltra |
| Canc | sposta nel cestino |

La finestra di scrittura non ha scorciatoie proprie: Firma, Cifra, Salva bozza,
Invia e Chiudi sono pulsanti.

## Aggiornamento della posta

Boma non sincronizza né all'avvio né a intervalli: lo fa il cron (consigliato:
`mbsync -a && mu index`). All'avvio mostra quello che l'indice di `mu` contiene.
«Aggiorna» (F5) esegue `mbsync -V <canale>` per ogni account, uno alla volta
(limite di 10 minuti ciascuno), poi `mu index` e rilegge l'elenco. Un errore su un
account non ferma gli altri; nulla viene ritentato da solo. Se il cron sta già
sincronizzando, la barra di stato lo dice. Alla chiusura il `mbsync` in corso
riceve SIGTERM (SIGKILL dopo 3 secondi): al riavvio riprende, perché è
incrementale.

## Cancellare i messaggi

«Elimina» (o Canc) sposta il messaggio nella cartella cestino dell'account con
`mu move`; la sincronizzazione successiva propaga lo spostamento sul server. Non
c'è conferma, ma il messaggio resta nel cestino.

**Limite importante**: `mu` ragiona con cartelle relative alla *sua* radice
Maildir, e Boma deve conoscerla con certezza prima di spostare qualcosa. Finché
non è nota, Elimina è disattivata (compare un avviso e nulla viene spostato). La
radice viene presa, in quest'ordine, da `muRoot=` in `[general]` e da
`mu info store` (all'avvio, in modo asincrono). Se `mu info store` fallisce e
`muRoot=` non è impostato, le viste per cartella restano utilizzabili con una
radice stimata (il genitore comune delle Maildir degli account) e un avviso nella
barra di stato, ma Elimina resta disattivata. Per risolvere: imposta in
`[general]` la radice Maildir con cui è stato creato l'indice di `mu` (di solito
la cartella che contiene le Maildir degli account, nell'esempio `~/Mail`), oppure
correggi l'indice di `mu`.

Il messaggio aperto viene segnato come letto (`mu move --flags=-N+S`) dopo la
visualizzazione; anche questo si propaga al server alla sincronizzazione
successiva.

## OpenPGP

Formato PGP/MIME, tramite `gpg`; la password della chiave la chiede pinentry e
non passa mai da Boma.

- **Firma**: attiva di default per ogni identità (`signByDefault`), disattivabile
  nella finestra di scrittura con il pulsante «Firma». Se la firma è richiesta ma
  l'identità non ha `pgpKeyId`, Boma non invia: chiede «Inviare SENZA firma?»
  (risposta predefinita No) e invia senza firma solo dopo un sì esplicito. Se la
  firma fallisce l'invio si ferma, non parte nulla e il messaggio resta nella
  finestra di scrittura. Se invece si disattiva a mano il pulsante «Firma», il
  messaggio parte senza firma e senza altre domande.
- **Cifratura**: solo su richiesta (pulsante «Cifra»). Prima si verifica che ogni
  destinatario abbia una chiave pubblica utilizzabile; se ne manca una l'invio è
  bloccato e si elencano i destinatari senza chiave. Un messaggio per cui è stata
  chiesta la cifratura non viene mai inviato in chiaro. Se la chiave
  dell'identità è risolvibile il messaggio è cifrato anche per il mittente,
  altrimenti la copia in Inviata non sarà leggibile da lui.
- **Verifica**: per i messaggi firmati la firma è controllata all'apertura (senza
  scaricare chiavi dalla rete). La spunta verde con il firmatario compare solo se
  la firma copre l'intero corpo del messaggio e se l'indirizzo del firmatario è
  quello del mittente (Da); una firma valida di un altro indirizzo mostra
  firmatario, mittente e un avviso. Se la parte firmata o cifrata è solo una
  parte del messaggio (accanto a testo non protetto) compare «Firmato o cifrato
  solo in parte» e mai il verde.
- **Decifratura**: per i messaggi cifrati Boma chiede conferma prima di
  decifrare (e a quel punto pinentry chiede la password).
- Un invio non riuscito non viene ripetuto da solo; se l'esito di `msmtp` è
  incerto (tempo scaduto) l'avviso invita a controllare Inviata prima di
  riprovare.

## Limiti noti

- La cartella della posta in arrivo è assunta chiamarsi `INBOX` (relativa alla
  radice dell'account).
- Dopo l'invio la bozza resta nella cartella Bozze: va eliminata a mano.
- Il cestino non viene mai svuotato in automatico.
- Niente conversazioni, niente ricerche salvate, nessuna gestione delle chiavi
  (si usa `gpg`).
- Inoltrare non include gli allegati del messaggio originale.
- `mbsync` non è lanciato in un proprio gruppo di processi: all'uscita il segnale
  arriva al processo `mbsync`; non è verificato che raggiunga anche i suoi figli
  (ad esempio `PassCmd` o `ssh`) con un `mbsync` reale.
- Con la cifratura, l'oggetto e il numero dei destinatari restano visibili
  (limite del formato PGP/MIME).
- Se all'avvio `mu info store` fallisce, la radice di `mu` non viene rilevata di
  nuovo finché Boma non viene riavviato.
- Boma non ha un blocco proprio: se «Aggiorna» e il cron si sovrappongono è `mbsync`
  stesso a rifiutare il secondo, e Boma lo segnala.
- Gli `mu move` (segna come letto, Elimina) possono avvenire mentre il cron sta
  eseguendo `mbsync`: `mbsync` li riprende alla sincronizzazione successiva.
- Se un altro `mu` tiene occupato l'indice (ad esempio un `mu index` lungo),
  segna come letto, Elimina e l'aggiornamento dell'indice dopo l'invio riprovano
  per al massimo 2 minuti, poi mostrano un errore: nulla viene spostato.
- Il confronto firmatario/mittente usa l'identità che `gpg` riporta per la firma
  (l'UID principale della chiave): una chiave con più indirizzi può dare un avviso
  anche se il mittente è uno dei suoi altri indirizzi.
- La copia di emergenza di un inviato (se Inviata fallisce) va spostata a mano in
  Inviata; Boma non la ricarica da solo.
- Provato finora solo con posta sintetica e programmi finti; le verifiche con
  posta, server e chiavi reali sono in corso e non ci sono ancora misure di
  prestazioni su una casella vera.
