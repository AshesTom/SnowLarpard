# SPDX-License-Identifier: LGPL-2.1-or-later
"""Mail 4 (10.6) formats, as recovered in docs/mail-reverse.md. No Apple data here: the DVD's
files are read at run time from .local (tools/extract.py, or Mail.pkg unpacked into
.local/media/re/mail).

- old-style (NeXT) plists: Mail's *.toolbar files (section 4)
- .strings files (UTF-16 or UTF-8)
- .emlx: "<length>\\n" + RFC 822 message + XML plist (section 7)
- the message flags integer (section 7)
- ~/Library/Mail paths (section 8)
"""
import plistlib
import re
from pathlib import Path

# ---------------------------------------------------------------- old-style plists

class PlistError(ValueError):
    pass


def parse_oldplist(text):
    """Parse a NeXT/OpenStep plist: dictionaries, arrays, quoted and bare strings, // and /* */."""
    i, n = 0, len(text)

    def ws():
        nonlocal i
        while i < n:
            if text[i].isspace():
                i += 1
            elif text.startswith("//", i):
                j = text.find("\n", i)
                i = n if j < 0 else j + 1
            elif text.startswith("/*", i):
                j = text.find("*/", i + 2)
                if j < 0:
                    raise PlistError("unterminated comment")
                i = j + 2
            else:
                break

    bare = re.compile(r"[A-Za-z0-9_$+/:.\-]+")

    def value():
        nonlocal i
        ws()
        if i >= n:
            raise PlistError("unexpected end")
        c = text[i]
        if c == "{":
            i += 1
            d = {}
            while True:
                ws()
                if i < n and text[i] == "}":
                    i += 1
                    return d
                k = value()
                ws()
                if i >= n or text[i] != "=":
                    raise PlistError(f"'=' expected at {i}")
                i += 1
                d[k] = value()
                ws()
                if i < n and text[i] == ";":
                    i += 1
        if c == "(":
            i += 1
            a = []
            while True:
                ws()
                if i < n and text[i] == ")":
                    i += 1
                    return a
                a.append(value())
                ws()
                if i < n and text[i] == ",":
                    i += 1
        if c == '"':
            i += 1
            out = []
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    i += 1
                    out.append({"n": "\n", "t": "\t"}.get(text[i], text[i]))
                else:
                    out.append(text[i])
                i += 1
            if i >= n:
                raise PlistError("unterminated string")
            i += 1
            return "".join(out)
        m = bare.match(text, i)
        if not m:
            raise PlistError(f"unexpected {c!r} at {i}")
        i = m.end()
        return m.group(0)

    v = value()
    ws()
    if i != n:
        raise PlistError(f"trailing data at {i}")
    return v


def read_strings(path):
    """A .strings file as a dict (UTF-16 with BOM, or UTF-8)."""
    raw = Path(path).read_bytes()
    if raw[:2] in (b"\xfe\xff", b"\xff\xfe"):
        text = raw.decode("utf-16")
    else:
        text = raw.decode("utf-8")
    d = parse_oldplist("{" + text + "}") if not text.lstrip().startswith("{") else parse_oldplist(text)
    return d


# ---------------------------------------------------------------- toolbars (section 4)

FLEXIBLE_SPACE = "NSToolbarFlexibleSpaceItem"
SEARCH_TAG = 100


def load_toolbar(path):
    """(default set, {identifier: item}) of a Mail *.toolbar file. Items inside segmented groups
    stay in their group's "segments"; each also appears on its own when the file lists it so."""
    d = parse_oldplist(Path(path).read_text(encoding="utf-8", errors="replace"))
    items = {}
    for group in d.get("toolbar array", []):
        for item in group if isinstance(group, list) else [group]:
            items.setdefault(item["identifier"], item)
    return d.get("default set", []), items


# ---------------------------------------------------------------- .emlx (section 7)

def write_emlx(message: bytes, metadata: dict) -> bytes:
    """+[Library _writeEmlxFile:...]: decimal length, '\\n', the message, the XML plist."""
    return b"%d\n" % len(message) + message + plistlib.dumps(metadata, fmt=plistlib.FMT_XML)


def read_emlx(data: bytes):
    """(message, metadata dict) of an .emlx file's bytes."""
    nl = data.find(b"\n")
    if nl <= 0 or not data[:nl].strip().isdigit():
        raise ValueError("not an emlx file: no length line")
    length = int(data[:nl])
    start = nl + 1
    if start + length > len(data):
        raise ValueError("emlx truncated")
    message = data[start:start + length]
    rest = data[start + length:]
    meta = plistlib.loads(rest) if rest.strip() else {}
    return message, meta


# ---------------------------------------------------------------- flags (section 7)

FLAG_BITS = {
    "read": 0x1, "deleted": 0x2, "answered": 0x4, "flagged": 0x10, "draft": 0x40,
    "forwarded": 0x100, "redirected": 0x200, "junk": 0x1000000, "not_junk": 0x2000000,
    "junk_recorded": 0x20000000, "color_on_text": 0x40000000,
}
IMAP_FLAGS = {  # _IMAPMessageFlagsTable, case-insensitive
    "\\seen": 0x1, "\\deleted": 0x2, "\\answered": 0x4, "\\flagged": 0x10, "\\draft": 0x40,
    "$forwarded": 0x100, "forwarded": 0x100, "redirected": 0x200, "$junk": 0x1000000,
    "junk": 0x1000000, "$notjunk": 0x2000000, "notjunk": 0x2000000, "junkrecorded": 0x20000000,
}


def flags_from_imap(names):
    """_MFMessageFlagsFromArray."""
    v = 0
    for name in names:
        v |= IMAP_FLAGS.get(name.lower(), 0)
    return v


def decode_flags(v):
    out = {k: bool(v & b) for k, b in FLAG_BITS.items()}
    n = (v & 0xFC00) >> 10
    out["attachments"] = 0 if n in (0x3F, 0x3D) else n          # -[Message numberOfAttachments]
    out["priority"] = (v & 0x70000) >> 16                          # 0 = not known yet
    return out


def encode_flags(attachments=0, priority=0, **bits):
    v = 0
    for k, on in bits.items():
        if on:
            v |= FLAG_BITS[k]
    return v | (min(attachments, 0x3C) & 0x3F) << 10 | (priority & 7) << 16


# ---------------------------------------------------------------- paths (section 8)

MAILBOX_EXTENSION = {"local": "mbox", "IMAP": "imapmbox", "EWS": "ewsmbox", "RSS": "rssmbox"}


def account_dirname(account_type, user, host):
    """defaultPathNameForAccountWithHostname:username: — "%@-%@@%@"."""
    return f"{account_type}-{user}@{host}"


def mailbox_path(root, account_dir, components, extension):
    """The folder names from the account down, the extension on the last one."""
    p = Path(root) / account_dir
    for c in components[:-1]:
        p /= c
    return p / f"{components[-1]}.{extension}"


def message_filename(message_id, kind="full"):
    """_filenameForMessageID: 1 = partial, 2 = empty, else whole."""
    return f"{message_id}" + {"partial": ".partial.emlx", "empty": ".empty.emlx"}.get(kind, ".emlx")


def message_path(mailbox, message_id, kind="full"):
    return Path(mailbox) / "Messages" / message_filename(message_id, kind)


# ---------------------------------------------------------------- headers (section 11)

DEFAULT_HEADERS = ["resent-from", "from", "date", "resent-to", "to", "resent-cc", "cc", "bcc",
                   "subject", "reply-to", "Attachments", "x-mail-rss-source-name", "x-mail-rss-author"]


def headers_to_display(message_keys, level, custom_patterns=()):
    """-[Defaults headersToDisplayFromHeaderKeys:headerDetailLevel:]."""
    if level == 0:
        return []
    if level == 1:
        return list(DEFAULT_HEADERS)
    out = []
    for key in DEFAULT_HEADERS + list(message_keys):
        if key in out:
            continue
        if level == 2 or any(re.fullmatch(p, key) for p in custom_patterns):
            out.append(key)
    return out
