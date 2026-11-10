#!/usr/bin/env python3
"""Genera una Maildir sintetica e deterministica.

uso: make_maildir.py <dir-maildir> <N>
Ogni terzo messaggio e' non letto (new/), gli altri letti (cur/, flag S);
ogni settimo ha un allegato. Oggetti e mittenti distinti; Message-ID unici.
"""
import os
import sys
from email.message import EmailMessage
from email.utils import format_datetime
from datetime import datetime, timedelta, timezone


def main():
    root, n = sys.argv[1], int(sys.argv[2])
    for d in ("cur", "new", "tmp"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    base = datetime(2024, 1, 1, tzinfo=timezone.utc)
    for i in range(n):
        m = EmailMessage()
        m["From"] = f"Mittente{i % 97} <sender{i % 97}@example.org>"
        m["To"] = "me@example.net"
        m["Subject"] = f"Oggetto numero{i} argomento{i % 13}"
        m["Date"] = format_datetime(base + timedelta(minutes=i))
        m["Message-ID"] = f"<msg{i}@gen.example.org>"
        m.set_content(f"Corpo del messaggio {i}\n")
        if i % 7 == 0:
            m.add_attachment(b"dati%d" % i, maintype="application",
                             subtype="octet-stream", filename=f"all{i}.bin")
        unread = i % 3 == 0
        name = f"{1700000000 + i}.{i}.gen"
        if unread:
            path = os.path.join(root, "new", name)
        else:
            path = os.path.join(root, "cur", name + ":2,S")
        with open(path, "wb") as f:
            f.write(m.as_bytes())


main()
