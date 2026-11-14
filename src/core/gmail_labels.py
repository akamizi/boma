# Boma: legge le etichette Gmail (X-GM-LABELS) in SOLA LETTURA e le scrive su stdout,
# una riga JSON per messaggio: {"id": "<message-id senza <>>", "labels": ["a", "b"]}.
# Ultima riga: {"done": N}. In caso di errore: messaggio su stderr, codice di uscita != 0.
#
# Uso:  gmail_labels.py CONFIG CANALE            importa (CONFIG = ~/.mbsyncrc)
#       gmail_labels.py --show-account CONFIG CANALE   host/utente/porta, senza password
#       gmail_labels.py --selftest
#
# Le credenziali sono quelle di mbsync: host e utente da CONFIG, password dal suo PassCmd
# (o Pass), eseguito qui dentro. La password non viene mai stampata ne' salvata.
# Si usa la mailbox in sola lettura (EXAMINE) e BODY.PEEK: nulla viene modificato sul server.
import base64
import imaplib
import json
import re
import shlex
import subprocess
import sys
import time
from email.parser import BytesHeaderParser

CHUNK = 200
PAUSE = 0.3          # secondi tra una richiesta e l'altra
RETRIES = 4          # tentativi dopo OVERQUOTA
RETRY_WAIT = 90      # secondi


def _unquote(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] == '"':
        v = v[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return v


def parse_config(text, channel):
    """Ricava dal testo di .mbsyncrc l'account IMAP del canale."""
    sections, cur = [], None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, val = line.partition(" ")
        key = key.strip()
        if key in ("IMAPAccount", "IMAPStore", "MaildirStore", "Channel", "Group", "Master"):
            cur = {"_type": key, "_name": _unquote(val)}
            sections.append(cur)
        elif cur is not None:
            cur.setdefault(key.lower(), _unquote(val))

    def find(kind, name):
        for s in sections:
            if s["_type"] == kind and s["_name"] == name:
                return s
        raise ValueError("%s '%s' non trovato in .mbsyncrc" % (kind, name))

    ch = find("Channel", channel)
    far = ch.get("far", "")
    m = re.match(r"^:([^:]+):", far)
    if not m:
        raise ValueError("il canale '%s' non ha un lato remoto (Far)" % channel)
    store = find("IMAPStore", m.group(1))
    acc = find("IMAPAccount", store.get("account", ""))
    tls = (acc.get("tlstype") or acc.get("ssltype") or "IMAPS").upper()
    port = int(acc.get("port") or (993 if tls == "IMAPS" else 143))
    return {
        "host": acc.get("host", ""),
        "user": acc.get("user", ""),
        "port": port,
        "tls": tls,
        "passcmd": acc.get("passcmd", ""),
        "pass": acc.get("pass", ""),
    }


def get_password(acc):
    if acc.get("passcmd"):
        cmd = acc["passcmd"]
        if cmd.startswith("+"):
            cmd = cmd[1:]
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("PassCmd fallito (codice %d)" % r.returncode)
        return r.stdout.rstrip("\r\n")
    if acc.get("pass"):
        return acc["pass"]
    raise RuntimeError("nessuna password configurata in .mbsyncrc (Pass/PassCmd)")


def decode_mutf7(s):
    """Nomi di mailbox/etichetta IMAP: UTF-7 modificato (RFC 3501)."""

    def rep(m):
        if m.group(1) == "":
            return "&"
        b = m.group(1).replace(",", "/")
        b += "=" * (-len(b) % 4)
        return base64.b64decode(b).decode("utf-16-be")

    return re.sub(r"&([^-]*)-", rep, s)


def tokenize_list(s, start):
    """Legge '(tok tok ...)' a partire da s[start]=='('; ritorna (lista, indice dopo ')')."""
    assert s[start] == "("
    i, out = start + 1, []
    while i < len(s):
        c = s[i]
        if c == ")":
            return out, i + 1
        if c == " ":
            i += 1
        elif c == '"':
            j, buf = i + 1, []
            while j < len(s) and s[j] != '"':
                if s[j] == "\\" and j + 1 < len(s):
                    j += 1
                buf.append(s[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
        else:
            j = i
            while j < len(s) and s[j] not in ' )':
                j += 1
            out.append(s[i:j])
            i = j
    raise ValueError("lista non chiusa")


def parse_labels(blob):
    """Etichette da un frammento di risposta FETCH (str). Le etichette di sistema (\\Inbox...) si scartano."""
    k = blob.find("X-GM-LABELS (")
    if k < 0:
        return []
    toks, _ = tokenize_list(blob, k + len("X-GM-LABELS "))
    out = []
    for t in toks:
        if t.startswith("\\") or t.upper() == "NIL":
            continue
        d = decode_mutf7(t).strip()
        if d and d not in out:
            out.append(d)
    return out


def parse_fetch(data):
    """Risposta di imaplib.fetch -> [(message-id, [etichette])] per i messaggi con etichette."""
    recs = []
    for item in data:
        if isinstance(item, tuple):
            recs.append([item[0].decode("utf-8", "replace"), item[1], ""])
        elif isinstance(item, bytes) and recs and item.strip() != b")":
            recs[-1][2] += item.decode("utf-8", "replace")
    out = []
    for prefix, header, rest in recs:
        labels = parse_labels(prefix + " " + rest)
        if not labels:
            continue
        mid = BytesHeaderParser().parsebytes(header).get("Message-ID", "")
        mid = " ".join(str(mid).split()).strip().strip("<>").strip()
        if mid:
            out.append((mid, labels))
    return out


def parse_message_ids(data):
    """Risposta FETCH di soli header Message-ID -> [Message-ID senza <>]."""
    out = []
    for item in data:
        if isinstance(item, tuple):
            mid = BytesHeaderParser().parsebytes(item[1]).get("Message-ID", "")
            mid = " ".join(str(mid).split()).strip().strip("<>").strip()
            if mid:
                out.append(mid)
    return out


def parse_list_line(b):
    """Riga di LIST -> (flags, nome decodificato) oppure None."""
    line = b.decode("utf-8", "replace") if isinstance(b, bytes) else str(b)
    m = re.match(r'^\((?P<flags>[^)]*)\) (?:"(?P<sep>[^"]*)"|NIL) (?P<name>.+)$', line)
    if not m:
        return None
    return m.group("flags"), _unquote(m.group("name"))


# Etichette che ricalcano cartelle di sistema (importate da vecchie caselle): non sono tag utili.
IGNORED = {"sent", "trash", "drafts", "draft", "junk", "spam", "archive", "queue", "posta inviata", "inbox"}


def user_labels(boxes):
    """Nomi (grezzi, come li vuole il server) delle etichette dell'utente: niente cartelle di sistema."""
    out = []
    for b in boxes or []:
        r = parse_list_line(b)
        if not r:
            continue
        flags, name = r
        if "\\Noselect" in flags or name.lower() in IGNORED or name.startswith("[Gmail]") or name.startswith("[Google Mail]"):
            continue
        out.append(name)
    return out


def find_all_mail(conn):
    typ, boxes = conn.list()
    for b in boxes or []:
        r = parse_list_line(b)
        if r and "\\All" in r[0]:
            return r[1]
    return "[Gmail]/All Mail"


def quote(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def run(config_path, channel, out=sys.stdout):
    with open(config_path, encoding="utf-8", errors="replace") as f:
        acc = parse_config(f.read(), channel)
    if acc["tls"] not in ("IMAPS", "STARTTLS"):
        raise RuntimeError("connessione non cifrata rifiutata (TLSType %s)" % acc["tls"])
    password = get_password(acc)
    state = {"conn": None, "box": None}

    def connect():
        try:
            if state["conn"]:
                state["conn"].logout()
        except Exception:
            pass
        if acc["tls"] == "IMAPS":
            conn = imaplib.IMAP4_SSL(acc["host"], acc["port"])
        else:
            conn = imaplib.IMAP4(acc["host"], acc["port"])
            conn.starttls()
        if "X-GM-EXT-1" not in " ".join(conn.capabilities):
            raise RuntimeError("il server non e' Gmail (manca X-GM-EXT-1)")
        conn.login(acc["user"], password)
        state["conn"] = conn
        if state["box"] is None:
            typ, boxes = conn.list()
            state["names"] = user_labels(boxes)
            state["box"] = find_all_mail(conn)
        typ, _ = conn.select(quote(state["box"]), readonly=True)
        if typ != "OK":
            raise RuntimeError("cartella %s non apribile" % state["box"])

    def retrying(fn):
        for k in range(RETRIES + 1):
            try:
                return fn(state["conn"])
            except (imaplib.IMAP4.error, OSError) as e:
                quota = "OVERQUOTA" in str(e).upper()
                if k == RETRIES:
                    raise
                time.sleep(RETRY_WAIT if quota else 5)
                if not quota:  # connessione caduta: si riparte da capo (sola lettura, nulla da ripristinare)
                    connect()

    connect()
    try:
        # Una ricerca per etichetta e poi solo gli header dei messaggi trovati: molto meno
        # traffico che leggere tutta la cartella (Gmail limita la banda: OVERQUOTA).
        found = []
        for name in state["names"]:
            typ, res = retrying(lambda c: c.search(None, "X-GM-LABELS", quote(name)))
            if typ == "OK" and res and res[0]:
                found.append((len(res[0].split()), name, res[0].split()))
        found.sort(key=lambda t: t[0])  # prima le piccole: se si viene interrotti, resta il piu' utile
        done = 0
        partial = False
        try:
            for _, name, ids in found:
                label = decode_mutf7(name)
                for a in range(0, len(ids), CHUNK):
                    part = b",".join(ids[a:a + CHUNK]).decode()
                    typ, data = retrying(lambda c: c.fetch(part, "(BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])"))
                    if typ != "OK":
                        raise RuntimeError("FETCH fallito")
                    for mid in parse_message_ids(data):
                        out.write(json.dumps({"id": mid, "labels": [label]}, ensure_ascii=False) + "\n")
                        done += 1
                    out.flush()
                    time.sleep(PAUSE)
        except (imaplib.IMAP4.error, OSError) as e:
            if done == 0:
                raise
            partial = True  # limite di Gmail o rete: si tiene quanto gia' letto
            sys.stderr.write("Importazione parziale (%s): riprova piu' tardi\n" % e)
        out.write(json.dumps({"done": done, "partial": partial}) + "\n")
    finally:
        try:
            state["conn"].logout()
        except Exception:
            pass


def selftest():
    cfg = """
IMAPAccount lavoro
Host imap.gmail.com
User me@example.org
PassCmd "echo secret"
TLSType IMAPS

IMAPStore lavoro-remote
Account lavoro

Channel lavoro
Far :lavoro-remote:
Near :lavoro-local:
"""
    a = parse_config(cfg, "lavoro")
    assert (a["host"], a["user"], a["port"], a["tls"]) == ("imap.gmail.com", "me@example.org", 993, "IMAPS"), a
    assert a["passcmd"] == "echo secret"
    assert get_password(a) == "secret"
    try:
        parse_config(cfg, "nope")
        raise SystemExit("atteso errore")
    except ValueError:
        pass
    assert decode_mutf7("Citt&AOA-") == "Città"
    assert decode_mutf7("A &- B") == "A & B"
    assert parse_labels('1 (X-GM-LABELS ("\\\\Inbox" Lavoro "Con spazi" "a \\"q\\" b" "Citt&AOA-") UID 3') == [
        "Lavoro", "Con spazi", 'a "q" b', "Città"], parse_labels
    assert parse_labels("1 (UID 3)") == []
    data = [
        (b'1 (X-GM-LABELS ("\\\\Inbox" Lavoro) BODY[HEADER.FIELDS (MESSAGE-ID)] {30}', b"Message-ID: <a@x>\r\n\r\n"),
        b")",
        (b"2 (BODY[HEADER.FIELDS (MESSAGE-ID)] {40}", b"Message-ID:\r\n <b@x>\r\n\r\n"),
        b' X-GM-LABELS ("Banca" "Tasse"))',
        (b'3 (X-GM-LABELS ("\\\\Sent") BODY[HEADER.FIELDS (MESSAGE-ID)] {30}', b"Message-ID: <c@x>\r\n\r\n"),
        b")",
    ]
    assert parse_fetch(data) == [("a@x", ["Lavoro"]), ("b@x", ["Banca", "Tasse"])], parse_fetch(data)
    assert parse_message_ids([(b"1 (BODY[HEADER.FIELDS (MESSAGE-ID)] {30}", b"Message-ID: <a@x>\r\n\r\n"), b")"]) == ["a@x"]
    assert user_labels([b'(\\HasNoChildren) "/" "INBOX"', b'(\\HasNoChildren) "/" "Sent"', b'(\\HasNoChildren) "/" "Banca"',
                        b'(\\HasNoChildren \\Sent) "/" "[Gmail]/Posta inviata"', b'(\\HasChildren) "/" "A/B"',
                        b'(\\Noselect) "/" "[Gmail]"']) == ["Banca", "A/B"]
    assert find_all_mail.__name__
    print("ok")


if __name__ == "__main__":
    try:
        if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
            selftest()
        elif len(sys.argv) == 4 and sys.argv[1] == "--show-account":
            with open(sys.argv[2], encoding="utf-8", errors="replace") as f:
                acc = parse_config(f.read(), sys.argv[3])
            acc.pop("pass", None)
            acc["passcmd"] = bool(acc["passcmd"])
            print(json.dumps(acc))
        elif len(sys.argv) == 3:
            run(sys.argv[1], sys.argv[2])
        else:
            sys.exit("uso: gmail_labels.py CONFIG CANALE | --show-account CONFIG CANALE | --selftest")
    except SystemExit:
        raise
    except Exception as e:  # nessun dettaglio sensibile: solo il messaggio
        sys.stderr.write("%s\n" % e)
        sys.exit(1)
