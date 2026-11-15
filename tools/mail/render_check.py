# SPDX-License-Identifier: LGPL-2.1-or-later
"""Draws Mail's viewer from what docs/mail-reverse.md recovered, with your DVD's own images and
fonts, to check the numbers by eye. Not the port: a visual test bench.

    python3 tools/mail/render_check.py [OUT_DIR]     (default .local/media/re/mail/render)

Recovered from Mail's code (exact): toolbar contents and order, segment groups, tags and labels;
sidebar row height (11 pt + 9), icons per mailbox type, unread badges (font, size, shape,
colours); list columns (ids, widths), thread bands, deleted messages in dark grey, unread in bold;
address atoms (shape, sizes, colours). Not recovered (approximations, drawn hatched in the
legend): the toolbar's bezels and gradient, the source list's background and group-row style,
the window frame.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import cairo

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mailformats as mf  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RES = ROOT / ".local/media/re/mail/Applications/Mail.app/Contents/Resources"
FONTS = ROOT / ".local/assets/System/Library/Fonts"


# ---------------------------------------------------------------- fonts: Lucida Grande from the DVD

def use_dvd_fonts():
    conf = Path(tempfile.gettempdir()) / f"sl-mail-fonts-{os.getuid()}.conf"
    conf.write_text(f"""<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig><include ignore_missing="yes">/etc/fonts/fonts.conf</include><dir>{FONTS}</dir></fontconfig>""")
    os.environ["FONTCONFIG_FILE"] = str(conf)
    out = subprocess.run(["fc-list", ":family=Lucida Grande"], capture_output=True, text=True).stdout
    return bool(out.strip())


def font(cr, size, bold=False, family="Lucida Grande"):
    cr.select_font_face(family, cairo.FONT_SLANT_NORMAL,
                        cairo.FONT_WEIGHT_BOLD if bold else cairo.FONT_WEIGHT_NORMAL)
    cr.set_font_size(size * 96 / 72 * 0.75)  # points at 72 dpi, as the Mac draws them


def text(cr, x, baseline, s, rgb=(0, 0, 0)):
    cr.set_source_rgb(*[c / 255 for c in rgb])
    cr.move_to(x, baseline)
    cr.show_text(s)


def width(cr, s):
    return cr.text_extents(s).x_advance


def image(name):
    for ext in (".png", ".tiff", ".tif"):
        p = RES / (name + ext)
        if p.exists():
            if ext == ".png":
                return cairo.ImageSurface.create_from_png(str(p))
            from PIL import Image  # tiff
            im = Image.open(p).convert("RGBA")
            tmp = Path(tempfile.gettempdir()) / f"sl-mail-{name}.png"
            im.save(tmp)
            return cairo.ImageSurface.create_from_png(str(tmp))
    return None


def paint(cr, surf, x, y, w=None, h=None):
    if surf is None:
        return
    cr.save()
    cr.translate(x, y)
    if w and h:
        cr.scale(w / surf.get_width(), h / surf.get_height())
    cr.set_source_surface(surf, 0, 0)
    cr.paint()
    cr.restore()


def hatch(cr, x, y, w, h):
    """Marks an area whose look was not recovered."""
    cr.save()
    cr.rectangle(x, y, w, h)
    cr.clip()
    cr.set_source_rgba(1, 0, 0, 0.12)
    cr.set_line_width(1)
    for i in range(int(-h), int(w), 8):
        cr.move_to(x + i, y + h)
        cr.line_to(x + i + h, y)
    cr.stroke()
    cr.restore()


def rgb(c):
    return [v / 255 for v in c]


# ---------------------------------------------------------------- 1. toolbar (section 4)

def render_toolbar(out, strings):
    default, items = mf.load_toolbar(RES / "MainWindow.toolbar")
    W, H = 900, 58
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    cr = cairo.Context(s)
    g = cairo.LinearGradient(0, 0, 0, H)        # approximation: 10.6's toolbar gradient is CoreUI's
    g.add_color_stop_rgb(0, *rgb((232, 232, 232)))
    g.add_color_stop_rgb(1, *rgb((207, 207, 207)))
    cr.set_source(g)
    cr.paint()
    hatch(cr, 0, 0, W, 4)
    font(cr, 11)

    def segments(item):
        return item.get("segments") or [item]

    # widths: 29 px glyphs in 32 px segments; labels under them
    entries = []
    for ident in default:
        if ident == mf.FLEXIBLE_SPACE:
            entries.append(("flex", None, 0))
        elif int(items[ident].get("tag", 0)) == mf.SEARCH_TAG:
            entries.append(("search", items[ident], 180))
        else:
            segs = segments(items[ident])
            labels = [strings.get(x.get("title", ""), x.get("title", "")) for x in segs]
            # a segment is as wide as its glyph or its label (approximation of NSToolbarItemGroup)
            segw = [max(32, round(width(cr, label)) + 8) for label in labels]
            entries.append(("item", (items[ident], segw), sum(segw)))
    fixed = sum(e[2] for e in entries) + 8 * (len(entries) + 1)
    flex = max(0, (W - fixed) / max(1, sum(1 for e in entries if e[0] == "flex")))
    x = 8
    for kind, item, w in entries:
        if kind == "flex":
            x += flex
            continue
        if kind == "search":
            cr.set_source_rgb(1, 1, 1)
            cr.new_path()
            cr.arc(x + 11, 22, 11, 1.5708, 4.7124)
            cr.arc(x + w - 11, 22, 11, -1.5708, 1.5708)
            cr.close_path()
            cr.fill_preserve()
            cr.set_source_rgb(*rgb((140, 140, 140)))
            cr.set_line_width(1)
            cr.stroke()
            hatch(cr, x, 11, w, 22)
            label = strings.get(item["title"], item["title"])
            text(cr, x + (w - width(cr, label)) / 2, 51, label)
            x += w + 8
            continue
        item, segw = item
        segs = segments(item)
        # the bezel: 32 px per glyph, centred over each segment's column (approximation)
        cr.set_line_width(1)
        sx = x
        for i, sg in enumerate(segs):
            gx = sx + (segw[i] - 32) / 2
            cr.new_path()
            cr.rectangle(gx + 0.5, 10.5, 31, 23)
            g = cairo.LinearGradient(0, 10, 0, 34)
            g.add_color_stop_rgb(0, *rgb((252, 252, 252)))
            g.add_color_stop_rgb(1, *rgb((221, 221, 221)))
            cr.set_source(g)
            cr.fill_preserve()
            cr.set_source_rgb(*rgb((120, 120, 120)))
            cr.stroke()
            paint(cr, image(sg["image"]), gx + 1.5, 12)
            label = strings.get(sg.get("title", ""), sg.get("title", ""))
            text(cr, sx + (segw[i] - width(cr, label)) / 2, 51, label)
            sx += segw[i]
        x += w + 8
    s.write_to_png(str(out))


# ---------------------------------------------------------------- 2. sidebar (section 9)

BADGE = {  # (badge, text) by state; tint 1 = Blue
    ("normal", "blue"): ((133, 151, 187), (255, 255, 255)),
    ("normal", "graphite"): ((137, 152, 169), (255, 255, 255)),
    ("inactive", None): ((154, 154, 154), (255, 255, 255)),
    ("selected-focused", "blue"): ((255, 255, 255), (56, 115, 191)),
    ("selected-focused", "graphite"): ((255, 255, 255), (97, 117, 139)),
    ("selected", "blue"): ((255, 255, 255), (136, 153, 188)),
    ("selected", "graphite"): ((255, 255, 255), (148, 162, 178)),
    ("selected-inactive", None): ((255, 255, 255), (159, 159, 159)),
}
MAILBOX_FONT = 11.0                 # +[NSFont(MailAdditions) mailboxListFont] default
ROW = max(20.0, MAILBOX_FONT + 9)   # _setOutlineViewRowHeight


def badge(cr, right, cy, count, state, tint):
    """MailboxListCell drawBadgeCenteredInRect: / badgeWidth."""
    fill, ink = BADGE[(state, tint)]
    font(cr, MAILBOX_FONT, bold=True, family="Helvetica")
    label = f"{count:,}"
    ext = cr.text_extents(label)
    h = cr.font_extents()[2]                        # the text's line height
    sample = label if count > 9 else "99"
    w = cr.text_extents(sample).x_advance + (h * 0.5 + 0.999) // 1 + 1
    hh = h // 1
    x, y = right - w, cy - hh / 2
    r = hh / 2
    cr.new_sub_path()
    cr.arc(x + r, y + r, r, 1.5708, 4.7124)
    cr.arc(x + w - r, y + r, r, -1.5708, 1.5708)
    cr.close_path()
    cr.set_source_rgb(*rgb(fill))
    cr.fill()
    text(cr, x + (w - ext.x_advance) / 2, cy + ext.height / 2, label, ink)
    return w


def render_sidebar(out, strings):
    rows = [
        ("group", "MAILBOXES"),
        ("box", "InMailbox", "Inbox", 12), ("box", "DraftsMailbox", "Drafts", 0),
        ("box", "SentMailbox", "Sent", 0), ("box", "TrashMailbox", "Trash", 0),
        ("box", "JunkMailbox", "Junk", 3),
        ("group", "REMINDERS"),
        ("box", "NotesMailbox", "Notes", 0), ("box", "ToDoMailbox", "To Do", 1),
        ("group", "RSS"),
        ("box", "RSSMailbox", "Apple Hot News", 128),
        ("group", "IMAP"),
        ("box", "GenericAccount", "INBOX", 0),
    ]
    W = 190
    H = int(ROW * (len(rows) + 1)) + 23
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    cr = cairo.Context(s)
    cr.set_source_rgb(*rgb((214, 221, 229)))   # approximation: AppKit's source-list background
    cr.paint()
    y = ROW / 2
    selected = 1
    for i, r in enumerate(rows):
        top = y + i * ROW
        if r[0] == "group":
            hatch(cr, 0, top, W, ROW)
            font(cr, 11, bold=True)
            text(cr, 8, top + 14, r[1], (98, 112, 128))
            continue
        _, icon, name, count = r
        if i == selected:
            g = cairo.LinearGradient(0, top, 0, top + ROW)
            g.add_color_stop_rgb(0, *rgb((92, 147, 213)))
            g.add_color_stop_rgb(1, *rgb((22, 92, 197)))
            cr.rectangle(0, top, W, ROW)
            cr.set_source(g)
            cr.fill()
            hatch(cr, 0, top, W, ROW)
        paint(cr, image(icon), 18, top + (ROW - 16) / 2)
        font(cr, MAILBOX_FONT, bold=(i == selected))
        text(cr, 40, top + 14, name, (255, 255, 255) if i == selected else (0, 0, 0))
        if count:
            badge(cr, W - 8, top + ROW / 2, count,
                  "selected-focused" if i == selected else "normal", "blue")
    # MailboxPaneBottomView: 23 px at the bottom (nib)
    cr.rectangle(0, H - 23, W, 23)
    cr.set_source_rgb(*rgb((230, 230, 230)))
    cr.fill()
    hatch(cr, 0, H - 23, W, 23)
    s.write_to_png(str(out))


# ---------------------------------------------------------------- 3. message list (sections 5, 10)

COLUMNS = [(0, "", 40), (9, "", 20), (5, "From", 150), (2, "Subject", 240), (3, "Date Received", 120)]
THREAD = (205, 220, 243)                                    # threadBackgroundColor
LIGHTER = tuple(round(c + 0.55 * (255 - c)) for c in THREAD)  # lighterThreadBackgroundColor


def render_list(out):
    msgs = [  # (kind, unread, deleted, from, subject, date)
        ("msg", True, False, "Alice", "Lunch on Friday?", "Today 12:04"),
        ("thread", False, False, "Bob", "Release notes (3)", "Today 10:12"),
        ("child", False, False, "Bob", "Release notes", "Today 10:12"),
        ("child", True, False, "Carol", "Re: Release notes", "Today 10:40"),
        ("child", False, False, "Dan", "Re: Release notes", "Today 11:02"),
        ("msg", False, True, "Eve", "Deleted, shown with Show Deleted", "Yesterday"),
        ("msg", False, False, "Frank", "Weekly report", "Yesterday"),
    ]
    W = sum(c[2] for c in COLUMNS)
    s0 = cairo.ImageSurface(cairo.FORMAT_ARGB32, 1, 1)
    c0 = cairo.Context(s0)
    font(c0, 12, bold=True)
    fe = c0.font_extents()
    row = round(fe[2] + 2)            # updateTableViewRowHeight: "Afgy" in the bold font
    H = 17 + row * len(msgs)
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    cr = cairo.Context(s)
    cr.set_source_rgb(1, 1, 1)
    cr.paint()
    # header (approximation: AppKit's table header)
    cr.rectangle(0, 0, W, 17)
    cr.set_source_rgb(*rgb((236, 236, 236)))
    cr.fill()
    hatch(cr, 0, 0, W, 17)
    font(cr, 11)
    x = 0
    for _, title, w in COLUMNS:
        text(cr, x + 4, 13, title)
        x += w
    # thread bands: rows 1..4 (thread line + its open messages)
    top = 17
    cr.rectangle(COLUMNS[0][2], top + row, W - COLUMNS[0][2], row * 4)
    cr.set_source_rgb(*rgb(LIGHTER))
    cr.fill()
    cr.rectangle(0, top + row, COLUMNS[0][2], row * 4)
    cr.set_source_rgb(*rgb(THREAD))
    cr.fill()
    for i, (kind, unread, deleted, frm, subj, date) in enumerate(msgs):
        y = top + i * row
        ink = (64, 64, 64) if deleted else (0, 0, 0)          # darkGrayColor
        if unread:
            cr.arc(20, y + row / 2, 4.5, 0, 6.2832)          # approximation: the unread dot
            cr.set_source_rgb(*rgb((80, 130, 220)))
            cr.fill()
        if kind == "thread":
            font(cr, 11)
            text(cr, 47, y + row - 5, "▾", (90, 90, 90))
        x = 0
        for cid, _, w in COLUMNS:
            if cid in (5, 2, 3):
                value = {5: frm, 2: subj, 3: date}[cid]
                font(cr, 12, bold=unread)
                indent = 12 if kind == "child" and cid == 5 else 0
                text(cr, x + 4 + indent, y + row - 5, value, ink)
            x += w
    s.write_to_png(str(out))
    return row


# ---------------------------------------------------------------- 4. address atoms (section 13)

ATOM = {  # (border, fill)
    "normal": ((164, 189, 236), (222, 231, 248)),
    "inactive": ((215, 215, 215), (244, 244, 244)),
    "spotlighted": ((109, 149, 224), (187, 206, 241)),
    "selected": ((89, 139, 236), None),
    "selected-inactive": ((145, 145, 145), None),
}


def pill(cr, x, y, w, h, color):
    """_drawAtomPartsWithRect: a round-capped line as thick as the rect is high."""
    cr.set_line_width(h)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.move_to(x + h / 2, y + h / 2)
    cr.line_to(x + w - h / 2, y + h / 2)
    cr.set_source_rgb(*rgb(color))
    cr.stroke()


def atom(cr, x, y, name, state):
    font(cr, 12)
    w = round(width(cr, name) + 30 + 4)         # cellSize: name + 28 + 2, + 4 left margin
    h = 17
    bx, bw = x + 4, w - 4 - 2                    # atomBoundsRectForCellFrame:
    border, fill = ATOM[state]
    pill(cr, bx, y, bw, h, border)
    if fill:
        pill(cr, bx + 1, y + 1, bw - 2, h - 2, fill)
    selected = state.startswith("selected")
    text(cr, bx + 9, y + 13, name, (255, 255, 255) if selected else (0, 0, 0))
    return w


def render_atoms(out):
    W, H = 760, 34
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    cr = cairo.Context(s)
    cr.set_source_rgb(1, 1, 1)
    cr.paint()
    x = 6
    for state in ATOM:
        x += atom(cr, x, 8, f"Alice ({state})", state) + 6
    s.write_to_png(str(out))


def render_badges(out):
    W, H = 640, 30
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    cr = cairo.Context(s)
    cr.set_source_rgb(*rgb((214, 221, 229)))
    cr.paint()
    x = 10
    for (state, tint) in BADGE:
        if state.startswith("selected"):
            cr.rectangle(x - 4, 0, 72, H)
            cr.set_source_rgb(*rgb((40, 100, 200) if state != "selected-inactive" else (160, 160, 160)))
            cr.fill()
        w = badge(cr, x + 60, H / 2, 7 if state == "normal" else 42, state, tint)
        x += 78
    s.write_to_png(str(out))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / ".local/media/re/mail/render"
    if not (RES / "MainWindow.toolbar").exists():
        sys.exit(f"{RES}: unpack Mail.pkg from your DVD first (see docs/mail-reverse.md)")
    out.mkdir(parents=True, exist_ok=True)
    if not use_dvd_fonts():
        print("warning: Lucida Grande not found in", FONTS)
    strings = mf.read_strings(RES / "English.lproj/Localizable.strings")
    render_toolbar(out / "toolbar.png", strings)
    render_sidebar(out / "sidebar.png", strings)
    row = render_list(out / "list.png")
    render_atoms(out / "atoms.png")
    render_badges(out / "badges.png")
    print(f"rendered into {out} (list row height {row} px)")


if __name__ == "__main__":
    main()
