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
from email.parser import BytesHeaderParser

CHUNK = 500


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


def find_all_mail(conn):
    typ, boxes = conn.list()
    for b in boxes or []:
        line = b.decode("utf-8", "replace") if isinstance(b, bytes) else str(b)
        m = re.match(r'^\((?P<flags>[^)]*)\) "(?P<sep>[^"]*)" (?P<name>.+)$', line)
        if m and "\\All" in m.group("flags"):
            return _unquote(m.group("name"))
    return "[Gmail]/All Mail"


def run(config_path, channel, out=sys.stdout):
    with open(config_path, encoding="utf-8", errors="replace") as f:
        acc = parse_config(f.read(), channel)
    if acc["tls"] not in ("IMAPS", "STARTTLS"):
        raise RuntimeError("connessione non cifrata rifiutata (TLSType %s)" % acc["tls"])
    if acc["tls"] == "IMAPS":
        conn = imaplib.IMAP4_SSL(acc["host"], acc["port"])
    else:
        conn = imaplib.IMAP4(acc["host"], acc["port"])
        conn.starttls()
    try:
        caps = " ".join(conn.capabilities)
        if "X-GM-EXT-1" not in caps:
            raise RuntimeError("il server non e' Gmail (manca X-GM-EXT-1)")
        conn.login(acc["user"], get_password(acc))
        box = find_all_mail(conn)
        typ, dat = conn.select('"%s"' % box.replace("\\", "\\\\").replace('"', '\\"'), readonly=True)
        if typ != "OK":
            raise RuntimeError("cartella %s non apribile" % box)
        total = int(dat[0])
        done = 0
        for a in range(1, total + 1, CHUNK):
            b = min(total, a + CHUNK - 1)
            typ, data = conn.fetch("%d:%d" % (a, b), "(X-GM-LABELS BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])")
            if typ != "OK":
                raise RuntimeError("FETCH fallito")
            for mid, labels in parse_fetch(data):
                out.write(json.dumps({"id": mid, "labels": labels}, ensure_ascii=False) + "\n")
                done += 1
            out.flush()
        out.write(json.dumps({"done": done}) + "\n")
    finally:
        try:
            conn.logout()
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
