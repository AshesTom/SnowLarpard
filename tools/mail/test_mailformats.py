# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/mail/mailformats.py against the rules in docs/mail-reverse.md and, when the
DVD's Mail.app is unpacked (.local/media/re/mail), against its own files.

    python3 -m unittest tools/mail/test_mailformats.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mailformats as mf  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MAIL = ROOT / ".local/media/re/mail/Applications/Mail.app/Contents/Resources"
HAVE_MAIL = (MAIL / "MainWindow.toolbar").exists()


class OldPlist(unittest.TestCase):
    def test_syntax(self):
        v = mf.parse_oldplist('{ a = (b, "c d", {e = f;}); /* x */ g = "h\\"i"; // y\n }')
        self.assertEqual(v, {"a": ["b", "c d", {"e": "f"}], "g": 'h"i'})

    def test_errors(self):
        for bad in ("{a = b", "(a, b", '"abc', "{a b;}"):
            with self.assertRaises(mf.PlistError):
                mf.parse_oldplist(bad)


class Emlx(unittest.TestCase):
    MSG = b"From: a@example.com\r\nSubject: hi\r\n\r\nbody\r\n"

    def test_round_trip(self):
        meta = {"flags": mf.encode_flags(read=True, flagged=True, attachments=2, priority=3),
                "date-sent": 1258000000, "subject": "hi"}
        data = mf.write_emlx(self.MSG, meta)
        self.assertTrue(data.startswith(b"%d\n" % len(self.MSG)))
        msg, back = mf.read_emlx(data)
        self.assertEqual(msg, self.MSG)
        self.assertEqual(back, meta)

    def test_message_may_contain_plist_text(self):
        msg = b"Subject: x\r\n\r\n<?xml version=\"1.0\"?><plist></plist>\r\n"
        self.assertEqual(mf.read_emlx(mf.write_emlx(msg, {"flags": 0}))[0], msg)

    def test_bad(self):
        with self.assertRaises(ValueError):
            mf.read_emlx(b"no length\nx")
        with self.assertRaises(ValueError):
            mf.read_emlx(b"100\nshort")


class Flags(unittest.TestCase):
    def test_imap_table(self):
        v = mf.flags_from_imap(["\\Seen", "\\FLAGGED", "$Forwarded", "NotJunk", "unknown"])
        self.assertEqual(v, 0x1 | 0x10 | 0x100 | 0x2000000)

    def test_fields(self):
        d = mf.decode_flags(mf.encode_flags(answered=True, attachments=5, priority=1))
        self.assertTrue(d["answered"])
        self.assertFalse(d["read"])
        self.assertEqual((d["attachments"], d["priority"]), (5, 1))

    def test_unknown_attachment_counts(self):
        for n in (0x3F, 0x3D):
            self.assertEqual(mf.decode_flags(n << 10)["attachments"], 0)


class Paths(unittest.TestCase):
    def test_layout(self):
        acct = mf.account_dirname("IMAP", "me", "mail.example.com")
        self.assertEqual(acct, "IMAP-me@mail.example.com")
        box = mf.mailbox_path("/home/u/Library/Mail", acct, ["Work", "Clients"], "imapmbox")
        self.assertEqual(str(box), "/home/u/Library/Mail/IMAP-me@mail.example.com/Work/Clients.imapmbox")
        self.assertEqual(mf.message_path(box, 42).name, "42.emlx")
        self.assertEqual(mf.message_path(box, 42, "partial").name, "42.partial.emlx")
        self.assertEqual(mf.message_path(box, 42, "empty").parent.name, "Messages")


class Headers(unittest.TestCase):
    def test_levels(self):
        keys = ["from", "x-mailer", "received"]
        self.assertEqual(mf.headers_to_display(keys, 0), [])
        self.assertEqual(mf.headers_to_display(keys, 1)[:3], ["resent-from", "from", "date"])
        full = mf.headers_to_display(keys, 2)
        self.assertIn("x-mailer", full)
        self.assertEqual(full.count("from"), 1)
        self.assertEqual(mf.headers_to_display(keys, 3, [r"x-.*"]),
                         ["x-mail-rss-source-name", "x-mail-rss-author", "x-mailer"])


@unittest.skipUnless(HAVE_MAIL, "Mail.app from the DVD is not unpacked in .local/media/re/mail")
class DvdFiles(unittest.TestCase):
    def test_every_toolbar_parses_and_resolves(self):
        files = sorted(MAIL.glob("*.toolbar"))
        self.assertEqual(len(files), 6)
        for f in files:
            default, items = mf.load_toolbar(f)
            self.assertTrue(default, f.name)
            for ident in default:
                if ident.startswith("NSToolbar"):
                    continue
                self.assertIn(ident, items, f"{f.name}: {ident}")

    def test_toolbar_images_exist(self):
        _, items = mf.load_toolbar(MAIL / "MainWindow.toolbar")
        names = {i["image"] for i in items.values() if i.get("image", "empty") != "empty"}
        names |= {s["image"] for i in items.values() for s in i.get("segments", []) if "image" in s}
        missing = [n for n in names if not list(MAIL.glob(n + ".*"))]
        self.assertEqual(missing, [])

    def test_main_window_default_set(self):
        default, items = mf.load_toolbar(MAIL / "MainWindow.toolbar")
        self.assertEqual(default[0], "checkNewMail:")
        self.assertEqual(int(items["Search"]["tag"]), mf.SEARCH_TAG)
        self.assertEqual([s["identifier"] for s in items["reply_replyAll_forward"]["segments"]],
                         ["replyMessage:", "replyAllMessage:", "forwardMessage:"])

    def test_toolbar_labels_are_localized(self):
        strings = mf.read_strings(MAIL / "English.lproj/Localizable.strings")
        for f in sorted(MAIL.glob("*.toolbar")):
            _, items = mf.load_toolbar(f)
            titles = {i["title"] for i in items.values() if "title" in i}
            titles |= {s["title"] for i in items.values() for s in i.get("segments", []) if "title" in s}
            self.assertEqual(sorted(t for t in titles if t not in strings), [], f.name)

    def test_assistant_resources(self):
        strings = mf.read_strings(MAIL / "English.lproj/Assistant.strings")
        for key in ("TITLE_FOR_WELCOME", "DESCRIPTION_FOR_WELCOME", "DESCRIPTION_FOR_ADD_ACCOUNT"):
            self.assertIn(key, strings)
        self.assertIn("\n\n", strings["DESCRIPTION_FOR_WELCOME"])   # two paragraphs
        for name in ("Mail_Large", "InvalidDataIcon", "Caution"):
            self.assertTrue(list(MAIL.glob(name + ".*")), name)

    def test_assistant_geometry(self):
        import render_assistant as ra
        # the page's origin in the window: TransparentImageView + NSBox + its content view
        self.assertEqual(ra.PAGE, (141, 62))
        # the Welcome title (page 12, 314, 285 x 18) lands 32 px under the title bar's bottom
        x, y, w, h = ra.page(12, 314, 285, 18)
        self.assertEqual((x, y - ra.TITLEBAR), (153, 22))
        # Continue (456, 12) sits 44 px above the window's bottom
        self.assertEqual(ra.flip(456, 12, 108, 32)[1] + 32, ra.TITLEBAR + ra.H - 12)

    def test_sidebar_icon_files(self):
        for name in ("InMailbox", "DraftsMailbox", "SentMailbox", "ToBeDeliveredMailbox", "TrashMailbox",
                     "JunkMailbox", "junkmail", "NotesMailbox", "ToDoMailbox", "RSSMailbox",
                     "GenericAccount", "InMailboxLarge", "OutMailboxLarge"):
            self.assertTrue(list(MAIL.glob(name + ".*")), name)
        for system_drawn in ("NormalMailbox", "SmartFolderSmall", "LocalAccount", "AOSAccount"):
            self.assertFalse(list(MAIL.glob(system_drawn + ".*")), system_drawn)


if __name__ == "__main__":
    unittest.main()
