# SPDX-License-Identifier: LGPL-2.1-or-later
"""A small IMAP4rev1 + SMTP server on localhost to test Mail.app without a real account: the
commands libcurl sends (CAPABILITY, LOGIN / AUTHENTICATE PLAIN, LIST, SELECT, UID FETCH, UID STORE,
UID MOVE / COPY, EXPUNGE, APPEND; EHLO, AUTH, MAIL, RCPT, DATA). Everything is in memory; a message
sent by SMTP arrives in INBOX. Any user and password are accepted. No TLS: Mail allows a plain
connection only to localhost.

    python3 tools/mail/fakemail.py [--imap 1143] [--smtp 1025] [--log FILE]
"""
import argparse
import asyncio
import base64
import re
import time

MAILBOXES = {"INBOX": [], "Sent": [], "Drafts": [], "Trash": [], "Junk": [], "Work": []}
SPECIAL = {"Sent": "\\Sent", "Drafts": "\\Drafts", "Trash": "\\Trash", "Junk": "\\Junk"}
NEXT_UID = {name: 1 for name in MAILBOXES}
LOG = None


def log(*a):
    if LOG:
        with open(LOG, "a") as f:
            print(*a, file=f)


def add(box, raw, flags=()):
    uid = NEXT_UID[box]
    NEXT_UID[box] += 1
    MAILBOXES[box].append({"uid": uid, "raw": raw if isinstance(raw, bytes) else raw.encode(), "flags": set(flags)})
    return uid


def seed():
    now = time.strftime("%a, %d %b %Y %H:%M:%S +0000", time.gmtime())
    add("INBOX", f"From: Alice Martin <alice@example.com>\r\nTo: me@localhost\r\nSubject: Lunch on Friday?\r\n"
                 f"Date: {now}\r\nMessage-Id: <1@example.com>\r\n\r\nHi,\r\n\r\nShall we have lunch on Friday at noon?\r\n\r\nAlice\r\n")
    add("INBOX", f"From: =?UTF-8?Q?Bob_Le=C3=AFc?= <bob@example.com>\r\nTo: me@localhost\r\n"
                 f"Subject: =?UTF-8?B?UsOpdW5pb24gZGUgbHVuZGk=?=\r\nDate: {now}\r\nMessage-Id: <2@example.com>\r\n"
                 f"MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"XX\"\r\n\r\n"
                 f"--XX\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
                 f"La r=C3=A9union de lundi est d=C3=A9plac=C3=A9e =C3=A0 10 h.\r\n\r\nBob\r\n"
                 f"--XX\r\nContent-Type: application/pdf; name=\"ordre du jour.pdf\"\r\n"
                 f"Content-Disposition: attachment; filename=\"ordre du jour.pdf\"\r\nContent-Transfer-Encoding: base64\r\n\r\n"
                 f"JVBERi0xLjQK\r\n--XX--\r\n", {"\\Seen"})
    add("INBOX", f"From: Carol <carol@example.com>\r\nTo: me@localhost\r\nSubject: Photos\r\nDate: {now}\r\n"
                 f"Message-Id: <3@example.com>\r\nMIME-Version: 1.0\r\nContent-Type: text/html; charset=iso-8859-1\r\n\r\n"
                 f"<html><body><p>Voil\xe0 les photos&nbsp;!</p><p>Carol</p></body></html>\r\n".encode("latin-1"), {"\\Flagged"})
    add("Work", f"From: Dan <dan@example.com>\r\nTo: me@localhost\r\nSubject: Weekly report\r\nDate: {now}\r\n\r\nAttached.\r\n", {"\\Seen"})


# ------------------------------------------------------------------------------------------- IMAP

class Imap:
    def __init__(self, r, w):
        self.r, self.w, self.box = r, w, None

    async def send(self, line):
        log("IMAP <", line[:200] if isinstance(line, str) else line[:200])
        self.w.write((line + "\r\n").encode() if isinstance(line, str) else line)
        await self.w.drain()

    def msgs(self):
        return MAILBOXES.get(self.box, [])

    def by_uids(self, spec):
        msgs = self.msgs()
        out = []
        for part in spec.split(","):
            if ":" in part:
                a, b = part.split(":")
                lo = int(a)
                hi = max([m["uid"] for m in msgs] + [0]) if b == "*" else int(b)
                out += [m for m in msgs if lo <= m["uid"] <= hi]
            else:
                out += [m for m in msgs if m["uid"] == int(part)]
        return out

    async def run(self):
        await self.send("* OK IMAP4rev1 fakemail ready")
        while True:
            line = await self.r.readline()
            if not line:
                return
            line = line.decode(errors="replace").rstrip("\r\n")
            log("IMAP >", line)
            m = re.match(r"(\S+) (\S+)\s*(.*)", line)
            if not m:
                continue
            tag, cmd, arg = m.group(1), m.group(2).upper(), m.group(3)
            if cmd == "UID":
                sub, _, rest = arg.partition(" ")
                await self.uid(tag, sub.upper(), rest)
                continue
            if cmd == "FETCH":   # by sequence numbers: "FETCH 1:* (UID FLAGS)"
                spec, _, what = arg.partition(" ")
                msgs = self.msgs()
                seqs = set()
                for part in spec.split(","):
                    a, _, b = part.partition(":")
                    lo = int(a)
                    hi = len(msgs) if b == "*" else int(b or a)
                    seqs |= set(range(lo, hi + 1))
                if not msgs:
                    await self.send(f"{tag} BAD no messages")
                    continue
                for i, m in enumerate(msgs):
                    if i + 1 in seqs:
                        await self.send(f"* {i + 1} FETCH (UID {m['uid']} FLAGS ({' '.join(sorted(m['flags']))}))")
                await self.send(f"{tag} OK FETCH done")
                continue
            if cmd == "CAPABILITY":
                await self.send("* CAPABILITY IMAP4rev1 AUTH=PLAIN MOVE UIDPLUS")
            elif cmd == "LOGIN":
                pass
            elif cmd == "AUTHENTICATE":
                if len(arg.split()) == 1:
                    await self.send("+ ")
                    await self.r.readline()
            elif cmd == "LIST":
                for name in MAILBOXES:
                    flags = "\\HasNoChildren" + (" " + SPECIAL[name] if name in SPECIAL else "")
                    await self.send(f'* LIST ({flags}) "/" "{name}"')
            elif cmd in ("SELECT", "EXAMINE"):
                self.box = arg.strip().strip('"')
                if self.box not in MAILBOXES:
                    await self.send(f"{tag} NO no such mailbox")
                    continue
                await self.send(f"* {len(self.msgs())} EXISTS")
                await self.send("* OK [UIDVALIDITY 1] UIDs valid")
                await self.send(f"* OK [UIDNEXT {NEXT_UID[self.box]}] next")
                await self.send(f"{tag} OK [READ-WRITE] SELECT done")
                continue
            elif cmd == "EXPUNGE":
                MAILBOXES[self.box][:] = [x for x in self.msgs() if "\\Deleted" not in x["flags"]]
            elif cmd == "APPEND":
                mm = re.match(r'"?([^"]+?)"?\s+(?:\(([^)]*)\)\s+)?\{(\d+)\}', arg)
                box, flags, n = mm.group(1), mm.group(2) or "", int(mm.group(3))
                await self.send("+ Ready for literal data")
                data = await self.r.readexactly(n)
                await self.r.readline()
                uid = add(box, data, flags.split())
                await self.send(f"{tag} OK [APPENDUID 1 {uid}] APPEND done")
                continue
            elif cmd == "LOGOUT":
                await self.send("* BYE")
                await self.send(f"{tag} OK LOGOUT done")
                return
            await self.send(f"{tag} OK {cmd} done")

    async def uid(self, tag, sub, rest):
        if sub == "FETCH":
            spec, _, what = rest.partition(" ")
            for i, m in enumerate(self.msgs()):
                if m not in self.by_uids(spec):
                    continue
                seq = i + 1
                if "BODY" in what.upper():
                    if "PEEK" not in what.upper():
                        m["flags"].add("\\Seen")
                    raw = m["raw"]
                    head = f"* {seq} FETCH (UID {m['uid']} BODY[] {{{len(raw)}}}\r\n".encode()
                    await self.send(head + raw + b")\r\n")
                else:
                    await self.send(f"* {seq} FETCH (UID {m['uid']} FLAGS ({' '.join(sorted(m['flags']))}))")
        elif sub == "STORE":
            spec, op, flags = re.match(r"(\S+) ([+-]?FLAGS)\S* \(([^)]*)\)", rest).groups()
            for m in self.by_uids(spec):
                f = set(flags.split())
                if op.startswith("+"):
                    m["flags"] |= f
                elif op.startswith("-"):
                    m["flags"] -= f
                else:
                    m["flags"] = f
        elif sub in ("MOVE", "COPY"):
            spec, _, dest = rest.partition(" ")
            dest = dest.strip().strip('"')
            for m in self.by_uids(spec):
                add(dest, m["raw"], m["flags"])
                if sub == "MOVE":
                    MAILBOXES[self.box].remove(m)
        await self.send(f"{tag} OK UID {sub} done")


# ------------------------------------------------------------------------------------------- SMTP

async def smtp(r, w):
    async def send(s):
        log("SMTP <", s)
        w.write((s + "\r\n").encode())
        await w.drain()

    await send("220 localhost fakemail ESMTP")
    rcpts = []
    while True:
        line = await r.readline()
        if not line:
            return
        line = line.decode(errors="replace").rstrip("\r\n")
        log("SMTP >", line)
        u = line.upper()
        if u.startswith("EHLO") or u.startswith("HELO"):
            await send("250-localhost")
            await send("250-AUTH PLAIN LOGIN")
            await send("250 8BITMIME")
        elif u.startswith("AUTH"):
            if len(line.split()) == 2:
                await send("334 ")
                await r.readline()
            await send("235 Authenticated")
        elif u.startswith("MAIL FROM"):
            rcpts = []
            await send("250 OK")
        elif u.startswith("RCPT TO"):
            rcpts.append(line)
            await send("250 OK")
        elif u == "DATA":
            await send("354 End data with <CR><LF>.<CR><LF>")
            data = b""
            while True:
                l = await r.readline()
                if l in (b".\r\n", b".\n") or not l:
                    break
                data += l[1:] if l.startswith(b"..") else l
            add("INBOX", data)
            await send("250 OK queued")
        elif u == "QUIT":
            await send("221 Bye")
            return
        else:
            await send("250 OK")


async def main():
    global LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--imap", type=int, default=1143)
    ap.add_argument("--smtp", type=int, default=1025)
    ap.add_argument("--log")
    a = ap.parse_args()
    LOG = a.log
    seed()
    s1 = await asyncio.start_server(lambda r, w: Imap(r, w).run(), "127.0.0.1", a.imap)
    s2 = await asyncio.start_server(smtp, "127.0.0.1", a.smtp)
    print(f"fakemail: IMAP 127.0.0.1:{a.imap}, SMTP 127.0.0.1:{a.smtp}", flush=True)
    async with s1, s2:
        await asyncio.gather(s1.serve_forever(), s2.serve_forever())


if __name__ == "__main__":
    asyncio.run(main())
