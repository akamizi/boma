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
| Interfaccia | **Qt6 Widgets** | Tre colonne (cartelle, elenco, lettura) con barra di ricerca; all'avvio mostra le non lette. Cartelle mostrate per ogni account: Posta in arrivo, «Non lette» (solo i non letti dell'account: per Gmail quelli della Posta in arrivo, per gli altri quelli di tutte le cartelle tranne Cestino, Bozze e Inviata), Inviata, Bozze, Cestino. |
| Filtri in ingresso | **Sieve** sul server | Non è compito di Boma. In Boma "filtrare" significa cercare. |

L'unico punto in cui Boma scrive direttamente nel Maildir è la copia dei messaggi
inviati (Inviata) e delle bozze (Bozze), con la consegna Maildir standard: file
scritto e sincronizzato su disco in `tmp/`, poi pubblicato in `cur/` con `link()`
(che non sostituisce mai un file esistente) e infine rimosso da `tmp/` con
`unlink()`. Tutto il resto passa da `mbsync` e `mu`.

## Funzioni della prima versione

- Due account IMAP completamente indipendenti, con copia locale completa
  (scaricata dal cron con `mbsync`, non da Boma).
- Elenco a due righe: mittente e data (relativa: «2 ore fa», «ieri alle 18:02»),
  oggetto e dimensione; stella per i messaggi contrassegnati, graffetta per gli
  allegati; le non lette sono in grassetto, con sfondo e barra laterale evidenziati.
- Barra sopra l'elenco: «Ordina» (data, mittente, oggetto, dimensione; la scelta si
  ricorda) e, sui messaggi spuntati con la casella a sinistra di ogni riga (senza
  aprire il messaggio), «Sposta» (menu delle cartelle dello stesso account),
  «Archivia» (cartella `archiveFolder` dell'account, default `Archive`: deve
  esistere, Boma non la crea), «Elimina» (nel cestino, con conferma) e «Segna»
  (letto, non letto, preferito, togli preferito). Non sostituiscono il pulsante
  «Elimina» del messaggio aperto. Compare anche la riga «N selezionati» con
  «Deseleziona». Tutto passa da `mu move`.
- Etichette: il pulsante «Etichette» sopra l'elenco applica o toglie etichette ai messaggi
  spuntati (anche con «Nuova etichetta…»), con pillole colorate accanto all'oggetto. Si salvano
  solo in un file JSON locale (`~/.local/share/Boma/tags.json`, Message-ID → etichette, scrittura
  atomica): non si scrive nulla nei messaggi né sui server. Si filtra da «Mostra i messaggi con
  l'etichetta» o digitando `tag:nome` / `tag:"nome con spazi"` nella ricerca (combinabile con altri
  termini). Un file illeggibile non viene mai sovrascritto (copia in `tags.json.corrupt`).
  Solo per Gmail: «Importa etichette Gmail da» copia i nomi delle etichette (`X-GM-LABELS`) nelle
  etichette locali, in sola lettura (IMAP `EXAMINE`, `BODY.PEEK`) con un piccolo script Python
  incorporato (`src/core/gmail_labels.py`, richiede `python3`). Host, utente e password vengono da
  `~/.mbsyncrc` (PassCmd); le etichette di sistema (Inbox, Sent...) sono escluse, quelle già presenti
  non vengono mai tolte e Gmail non viene modificato.
  Sempre per Gmail: «Posta in arrivo Gmail: aggiorna da» legge (stessa sola lettura) quali messaggi
  hanno l'etichetta di sistema `\Inbox` e la cartella «Posta in arrivo» dell'account diventa quella
  vista, **senza scaricare INBOX** (niente duplicati con «Tutti i messaggi»). L'elenco è in
  `~/.local/share/Boma/gmail-inbox.json` (Message-ID per account) e viene sostituito a ogni
  aggiornamento, quindi un messaggio archiviato su Gmail esce dalla Posta in arrivo; dopo il primo
  consenso si aggiorna da solo a ogni sincronizzazione. È una vista: non si può spostare nulla
  «dentro» e l'archiviazione dal programma non toglie l'etichetta sul server. Mostra solo gli
  ultimi 1500 messaggi in Posta in arrivo per data di ricezione: la query di mu non regge
  decine di migliaia di ID.
- Conversazioni: i messaggi che si citano a vicenda (Message-ID/References, letti da `mu`)
  sono raggruppati in una riga con il numero dei messaggi; il clic sull'etichetta «N ▸» apre
  e chiude la conversazione, spuntare la riga seleziona tutti i suoi messaggi. Il
  raggruppamento riguarda i messaggi già caricati in elenco (stessa cartella o ricerca) e si
  disattiva da «Ordina → Raggruppa per conversazione».
- Composizione HTML con editor WYSIWYG: spuntando «HTML» compare la barra di
  formattazione (grassetto, corsivo, sottolineato, barrato, dimensione, colore, elenchi,
  allineamento, collegamento, pulisci formattazione). Sono ammessi solo collegamenti
  http, https e mailto. Il messaggio parte multipart/alternative con il testo semplice.
- Messaggi mostrati in testo semplice per default; se esiste anche la parte HTML un
  pulsante nella barra del messaggio alterna testo/HTML, e nelle Impostazioni si puo'
  scegliere l'HTML come predefinito. L'HTML e' sempre ripulito.
  Con un tema scuro i colori chiari dell'HTML (sfondi bianchi) sono adattati invertendo la
  luminosita' e conservando la tinta; il pulsante «Mostra originale» ripristina i colori
  del mittente per quel messaggio. Le immagini non vengono modificate.
  Le immagini incorporate nel messaggio (`cid:`) si vedono; quelle remote (http/https)
  NON si scaricano mai da sole (il mittente saprebbe che e quando leggi): il pulsante
  «Carica immagini remote» le scarica per il solo messaggio aperto (max 5 MiB l'una,
  40 per messaggio, senza cookie).
- Menu contestuale (tasto destro) su ogni cartella: «Segna tutti come letti»
  (tramite `mu move`) e «Svuota la casella» (cancellazione DEFINITIVA dei file in
  `cur/` e `new/`, con conferma che indica il numero di messaggi; con `Expunge`
  attivo in mbsync la cancellazione arriva anche al server al sync successivo).
- Vista delle non lette all'avvio, ricerca veloce. L'elenco raggruppa per conversazione
  (disattivabile: «Ordina → Raggruppa per conversazione»); `mu` lavora comunque per
  messaggio.
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

Non previsto nella v1: ricerche salvate, gestione delle chiavi (si usa direttamente
`gpg`), cancellazione automatica del cestino.

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

### Opzioni da riga di comando

```sh
boma --version   # (o -V) una riga: "boma 0.1.4 (abc123def)"; esce con 0
boma --help      # (o -h) elenco delle opzioni
```

`--version` non apre la finestra, non richiede display né configurazione e non lancia programmi
esterni: serve a verificare la versione installata (per esempio `boma --version | cut -d' ' -f2`).
Il numero viene da `project(boma VERSION ...)` in `CMakeLists.txt`, l'unica fonte. Tra parentesi
c'è il commit del build, se compilato da un checkout git (calcolato al `cmake` di configurazione;
si può fissare con `-DBOMA_COMMIT=...`); nei pacchetti compilati dal tarball c'è solo la versione.

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

Il pulsante «Server di invio...» accanto all'account msmtp modifica **solo il blocco
di quell'account** in `~/.msmtprc` (server, porta, TLS, STARTTLS, autenticazione,
utente, mittente, comando della password). Le altre righe, i commenti, `[defaults]` e gli
altri account restano identici; il file precedente va in `.msmtprc.bak` e i permessi
sono mantenuti (0600 se nuovo). Le password in chiaro non si scrivono mai: si usa
`passwordeval`. A differenza del resto delle Impostazioni, questo file si scrive subito
al «Salva» della sua finestra, non al «Salva» di Boma. `~/.mbsyncrc` resta in sola lettura.

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
| Invio (o doppio clic) | in Bozze: riapre la bozza per modificarla |

La finestra di scrittura non ha scorciatoie proprie: Firma, Cifra, Salva bozza,
Invia e Chiudi sono pulsanti.

### Bozze

Nella cartella Bozze un messaggio si riapre per modificarlo con doppio clic, Invio o il
pulsante **Modifica** (visibile solo in Bozze). Si ritrovano destinatari A, Cc e Ccn,
oggetto, corpo, identità e allegati.

- Salvare la bozza riaperta **sostituisce** quella originale (nessun doppione); dopo
  l'invio riuscito, con la copia in Inviata, la bozza viene rimossa. Se la copia in
  Inviata non riesce la bozza resta.
- La stessa bozza non si apre in due finestre: se è già aperta, viene portata in primo
  piano.
- Gli allegati si estraggono in una cartella temporanea (`boma-bozza-*` in `/tmp`)
  eliminata alla chiusura della finestra, comunque avvenga.
- Se la bozza non è stata creata da Boma (mittente non riconosciuto), contiene
  immagini incorporate, è firmata/cifrata o ha allegati non recuperabili, la finestra
  lo segnala e **non** sostituisce né rimuove l'originale, che resta in Bozze.
- L'invio ricostruisce sempre il messaggio dal contenuto della finestra: i byte della
  bozza non vanno mai a `msmtp`, quindi il Ccn non compare nelle intestazioni inviate.

## Cartelle e non letti

Il pannello delle cartelle mostra tutte le Maildir di ogni account con la loro
gerarchia. Le cartelle con messaggi non letti sono in grassetto con il numero,
«Nome (N)»; account e cartelle intermedie mostrano la somma dei figli. I conteggi
(una query `mu find` per cartella, una alla volta) si aggiornano dopo l'indicizzazione,
quando si legge o si cestina un messaggio e al cambio di configurazione. Il cestino
non conta.

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
- Se l'invio riesce ma la copia in Inviata non si scrive, la bozza resta in Bozze (va eliminata a mano).
- Notifiche di posta nuova: si basano sulla data dell'intestazione `Date`, non su quella di arrivo. Un messaggio
  consegnato in ritardo con una data anteriore all'ultimo messaggio già notificato non viene segnalato
  (compare comunque nell'elenco e tra le non lette).
- Il cestino non viene mai svuotato in automatico.
- Niente ricerche salvate, nessuna gestione delle chiavi (si usa `gpg`). Le conversazioni
  raggruppano solo i messaggi già caricati nell'elenco (stessa cartella o ricerca).
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

## Aggiornamento rapido

Accanto ad «Aggiorna» la freccia ▾ permette di aggiornare un solo account (F5 li
aggiorna tutti).

«Aggiorna ora» usa per default `mbsync --pull-new --push`: dal server scarica solo i
messaggi nuovi (piu' veloce), mentre verso il server invia tutte le modifiche fatte in
Boma (letto, cestino, spostamenti, cancellazioni). Bandiere e cancellazioni fatte
altrove arrivano con la sincronizzazione completa (il cron). Si puo' tornare alla
sincronizzazione completa in Impostazioni.

Attenzione: mbsync sincronizza solo le cartelle che corrispondono a `Patterns` nel
canale. Una cartella locale fuori dai pattern (es. «Trash» o «Archive» di un account Gmail
che sincronizza solo «[Gmail]/Tutti i messaggi») non arriva mai al server.

Posta scaricata da altri programmi (il cron, `goimapnotify`...): Boma osserva le cartelle
`new` dei Maildir configurati (solo le directory, mai i file) e, quando dopo qualche secondo
di quiete ne arriva di nuova, esegue `mu index` e rilegge l'elenco da solo, senza premere
«Aggiorna». Non parte se è già in corso una sincronizzazione di Boma. Le modifiche arrivate
dal server alle sole bandiere (letto/non letto) restano al prossimo aggiornamento.

Quando l'indice di `mu` si aggiorna (dopo «Aggiorna» o dopo posta scaricata da altri) e compaiono messaggi non letti nuovi, Boma mostra una notifica del desktop (D-Bus, `org.freedesktop.Notifications`): mittente e oggetto se è uno solo, altrimenti il numero. Si ricorda solo la data dell'ultimo messaggio notificato (più i Message-ID con quella data), non l'elenco di tutti: un messaggio che torna «non letto» dopo essere stato letto non viene notificato di nuovo, e non c'è limite al numero di messaggi nuovi per giro. Le non lette già presenti all'avvio non vengono notificate. Limite: la data è quella dell'intestazione `Date`, quindi un messaggio che arriva in ritardo con una data anteriore all'ultima notificata non viene segnalato. Se `mu` fallisce non si perde nulla (si riprova da soli e al giro successivo). Si disattiva in Impostazioni; se il servizio di notifiche manca non succede nulla (un avviso sul terminale).

## Licenza

Boma è distribuito con licenza GPL-3.0-or-later (vedi `LICENSE`).

## Installazione su Void Linux (repository xbps)

Il repository binario (solo `x86_64`, glibc) è pubblicato su GitHub Pages a
`https://akamizi.github.io/boma/` ed è firmato.

Prima installazione:

```sh
# 1. aggiungere il repository
echo 'repository=https://akamizi.github.io/boma' | sudo tee /etc/xbps.d/boma.conf

# 2. sincronizzare e installare: alla prima sincronizzazione xbps chiede di importare la
#    chiave pubblica del repository. Verificare che l'impronta sia
#    67:a2:ff:79:04:76:71:8c:19:62:93:79:d2:3f:d9:99 e rispondere "y"
sudo xbps-install -S boma
```

`boma` richiede a runtime `isync` (mbsync), `mu4e` (fornisce `mu`), `msmtp`, `gnupg` e `python3`,
che xbps installa da solo. Dopo l'installazione:

1. configurare mbsync e msmtp come descritto nelle sezioni precedenti (`~/.mbsyncrc`, `~/.msmtprc`);
2. avviare `boma` dal menu delle applicazioni o da terminale: al primo avvio crea
   `~/.config/boma/config.ini`, che si modifica anche da «Impostazioni»;
3. per gli aggiornamenti: `sudo xbps-install -Su`.

### AppImage (senza installare Qt)

Per chi non vuole Qt sul sistema (per esempio con un ambiente tutto GTK) ogni release ha anche un
`Boma-vX.Y.Z-x86_64.AppImage`, costruito da `.github/workflows/appimage.yml` (stesso tag del
repository xbps). Contiene Boma, Qt 6 e lo stile Adwaita, che è lo stile predefinito dell'AppImage
(si cambia in «Impostazioni»; lo schema chiaro/scuro funziona come al solito). Si scarica dalle
release di GitHub, si rende eseguibile (`chmod +x`) e si lancia.

**Non** contiene i programmi che Boma usa: devono essere installati sul sistema
(su Void: `sudo xbps-install isync mu4e msmtp gnupg python3`). Gli aggiornamenti non sono
automatici: si scarica il nuovo file.

### Come si pubblica una nuova versione (per chi mantiene il progetto)

1. Alzare `VERSION` in `project(boma VERSION ...)` di `CMakeLists.txt` e fare il commit.
2. Creare e pubblicare il tag `vX.Y.Z` (corrispondente alla versione): il workflow
   `.github/workflows/xbps.yml` compila il pacchetto in un container Void con il template
   `packaging/void/srcpkgs/boma/template`, lo firma con la chiave del secret `XBPS_PRIVATE_KEY`
   e aggiorna il ramo `gh-pages`. Si può anche lanciare a mano (*Actions → Repository xbps →
   Run workflow*). Lo stesso tag costruisce anche l'AppImage (`appimage.yml`) e lo allega alla release.
3. La chiave privata di firma non è nel repository: se cambia, cambia anche l'impronta qui sopra
   e gli utenti devono importare la nuova chiave.
