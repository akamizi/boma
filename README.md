# Boma

Client email desktop per Linux in Qt6 Widgets (C++20). Usa `mbsync` per la
sincronizzazione IMAP, `mu` per l'indicizzazione, `msmtp` per l'invio e GMime
per il parsing dei messaggi. Niente QML né QtWebEngine.

Stato: scheletro iniziale (finestra vuota); il resto verrà documentato man mano.

## Requisiti

- CMake >= 3.20 e un compilatore C++20
- Qt6 (Widgets, Test): `qt6-base-devel`
- GMime 3: `gmime-devel`
- Strumenti a runtime: `mbsync` (isync), `mu`, `msmtp`, `gnupg`, `pinentry-qt`

## Build

```sh
cmake -S . -B build
cmake --build build
./build/boma
```

## Configurazione

- `~/.mbsyncrc`: definisce account IMAP, canali e cartelle da sincronizzare
  con `mbsync` (Maildir locale).
- `~/.msmtprc`: definisce gli account SMTP usati da `msmtp` per l'invio.
- `~/.gnupg/gpg-agent.conf`: per limitare la permanenza delle password in cache
  a 5 minuti:

```
default-cache-ttl 300
max-cache-ttl 300
```
