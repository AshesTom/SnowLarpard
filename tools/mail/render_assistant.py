# SPDX-License-Identifier: LGPL-2.1-or-later
"""Draws Mail's account setup assistant ("Welcome to Mail") from its nibs' frames and the code
recovered in docs/mail-reverse.md (section 15), with your DVD's images, strings and fonts.

    python3 tools/mail/render_assistant.py [--compare SCREENSHOT.png] [OUT_DIR]

With --compare, the screenshot's window is cropped, brought to the nib's scale and put next to
the render, with a difference image, in OUT_DIR/assistant-compare.png.

Exact (from the nibs and the code): the window's size, every control's frame, fonts and sizes,
the texts, which field has the focus, the stamp (Mail_Large, top-left, unscaled), the white veil
(75 %, radius 8, inset 3/4/6/6 from the box). Approximate (AppKit/CoreUI, not recovered here):
the title bar, push buttons, text field bezels, the box's border, the window's grey.
"""
import argparse
import os
import pwd
import sys
from pathlib import Path

import cairo

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mailformats as mf  # noqa: E402
import render_check as rc  # noqa: E402

RES = rc.RES
W, H = 578, 416          # AssistantWindow.nib NSWindowRect
TITLEBAR = 22
PAGE = (20 + 119 + 2, 55 + 5 + 2)   # TransparentImageView + NSBox + its content view (nib)


def flip(x, y, w, h):
    """Cocoa (y up, content view) to image coordinates (y down, below the title bar)."""
    return x, TITLEBAR + H - (y + h), w, h


def page(x, y, w, h):
    return flip(PAGE[0] + x, PAGE[1] + y, w, h)


def rounded(cr, x, y, w, h, r):
    cr.new_path()
    cr.arc(x + w - r, y + r, r, -1.5708, 0)
    cr.arc(x + w - r, y + h - r, r, 0, 1.5708)
    cr.arc(x + r, y + h - r, r, 1.5708, 3.1416)
    cr.arc(x + r, y + r, r, 3.1416, 4.7124)
    cr.close_path()


def wrap(cr, s, width):
    lines = []
    for para in s.split("\n"):
        words, line = para.split(" "), ""
        for w in words:
            t = (line + " " + w).strip()
            if rc.width(cr, t) <= width or not line:
                line = t
            else:
                lines.append(line)
                line = w
        lines.append(line)
    return lines


def label(cr, frame, s, size=13, bold=False, align="left", color=(0, 0, 0)):
    x, y, w, h = frame
    rc.font(cr, size, bold)
    asc = cr.font_extents()[0]
    lh = round(size * 1.3)          # the Mac's line height for Lucida Grande: 17 px at 13 pt
    for i, line in enumerate(wrap(cr, s, w - 4)):
        tw = rc.width(cr, line)
        tx = x + 2 if align == "left" else x + w - 2 - tw
        rc.text(cr, tx, y + 1 + asc + i * lh, line, color)


def field(cr, frame, value="", focused=False):
    x, y, w, h = frame
    if focused:   # approximation of the focus ring
        rounded(cr, x - 2.5, y - 2.5, w + 5, h + 5, 3)
        cr.set_source_rgba(0.29, 0.56, 0.86, 0.6)
        cr.set_line_width(3)
        cr.stroke()
    cr.rectangle(x + 0.5, y + 0.5, w - 1, h - 1)
    cr.set_source_rgb(1, 1, 1)
    cr.fill_preserve()
    cr.set_source_rgb(*rc.rgb((155, 155, 155)))
    cr.set_line_width(1)
    cr.stroke()
    if value:
        label(cr, (x + 2, y + 3, w - 4, h), value)


def button(cr, frame, title, default=False, enabled=True):
    """Approximation of the Aqua push button (CoreUI's art is reversed in shell/src/aquakit)."""
    x, y, w, h = frame
    bx, by, bw, bh = x + 6, y + 5, w - 12, 21     # the bezel inside a 32 px push button frame
    rounded(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 10)
    g = cairo.LinearGradient(0, by, 0, by + bh)
    if default:
        g.add_color_stop_rgb(0, *rc.rgb((206, 230, 251)))
        g.add_color_stop_rgb(0.5, *rc.rgb((133, 190, 240)))
        g.add_color_stop_rgb(1, *rc.rgb((175, 220, 251)))
    else:
        g.add_color_stop_rgb(0, *rc.rgb((255, 255, 255)))
        g.add_color_stop_rgb(1, *rc.rgb((226, 226, 226)))
    cr.set_source(g)
    cr.fill_preserve()
    cr.set_source_rgb(*rc.rgb((80, 80, 110) if default else ((120, 120, 120) if enabled else (175, 175, 175))))
    cr.set_line_width(1)
    cr.stroke()
    rc.font(cr, 13)
    tw = rc.width(cr, title)
    rc.text(cr, x + (w - tw) / 2, by + 15, title, (0, 0, 0) if enabled else (160, 160, 160))


def full_name():
    """What NSFullUserName gives: the account's full name."""
    try:
        gecos = pwd.getpwuid(os.getuid()).pw_gecos.split(",")[0]
        return gecos or pwd.getpwuid(os.getuid()).pw_name
    except KeyError:
        return ""


def render(out, name=None):
    strings = mf.read_strings(RES / "English.lproj/Assistant.strings")
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H + TITLEBAR)
    cr = cairo.Context(s)
    cr.set_source_rgb(*rc.rgb((237, 237, 237)))      # approximation: windowBackgroundColor
    cr.paint()
    # title bar (approximation; WindowServer draws the real one)
    g = cairo.LinearGradient(0, 0, 0, TITLEBAR)
    g.add_color_stop_rgb(0, *rc.rgb((232, 232, 232)))
    g.add_color_stop_rgb(1, *rc.rgb((205, 205, 205)))
    cr.rectangle(0, 0, W, TITLEBAR)
    cr.set_source(g)
    cr.fill()
    rc.font(cr, 13)
    title = strings.get("TITLE_FOR_WELCOME", "")
    rc.text(cr, (W - rc.width(cr, title)) / 2, 16, title, (40, 40, 40))  # windowTitle

    # TransparentImageView (20, 55, 548 x 355): Mail_Large, top-left, not scaled
    tx, ty, tw, th = flip(20, 55, 548, 355)
    stamp = rc.image("Mail_Large")
    cr.save()
    cr.rectangle(tx, ty, tw, th)
    cr.clip()
    rc.paint(cr, stamp, tx, ty)
    cr.restore()
    # ... then the white veil over the box: bounds converted, +3 x, +4 y, -6 w, -6 h, radius 8
    bx, by, bw, bh = flip(20 + 119 + 3, 55 + 5 + 4, 422 - 6, 346 - 6)
    rounded(cr, bx, by, bw, bh, 8)
    cr.set_source_rgba(1, 1, 1, 0.75)
    cr.fill()
    # the NSBox's border (approximation)
    x, y, w, h = flip(20 + 119, 55 + 5, 422, 346)
    cr.rectangle(x + 0.5, y + 0.5, w - 1, h - 1)
    cr.set_source_rgba(0, 0, 0, 0.18)
    cr.set_line_width(1)
    cr.stroke()

    # the "Welcome" tab of AccountSetupAssistant.nib
    label(cr, page(12, 314, 285, 18), strings.get("TITLE_FOR_WELCOME", ""), 14, bold=True)
    label(cr, page(29, 231, 362, 75), strings.get("DESCRIPTION_FOR_WELCOME", ""))
    label(cr, page(60, 203, 82, 17), "Full Name:", align="right")
    label(cr, page(36, 176, 106, 17), "Email Address:", align="right")
    label(cr, page(74, 149, 68, 17), "Password:", align="right")
    field(cr, page(147, 201, 197, 22), name if name is not None else full_name())
    field(cr, page(147, 174, 197, 22), focused=True)        # _firstResponderForBasicInformation
    field(cr, page(147, 147, 197, 22))

    # AssistantWindow.nib's buttons
    button(cr, flip(136, 12, 98, 32), "Cancel")
    button(cr, flip(348, 12, 108, 32), "Go Back", enabled=False)   # canGoBack: no previous tab
    button(cr, flip(456, 12, 108, 32), "Continue", default=True)    # key equivalent Return
    hx, hy, hw, hh = flip(20, 17, 21, 23)                            # help button (round)
    cr.new_path()
    cr.arc(hx + hw / 2, hy + hh / 2, 10, 0, 6.2832)
    cr.set_source_rgb(1, 1, 1)
    cr.fill_preserve()
    cr.set_source_rgb(*rc.rgb((120, 120, 120)))
    cr.stroke()
    rc.font(cr, 13, bold=True)
    rc.text(cr, hx + hw / 2 - rc.width(cr, "?") / 2, hy + hh / 2 + 5, "?", (90, 60, 160))
    s.write_to_png(str(out))


def compare(render_png, shot, out):
    from PIL import Image, ImageChops, ImageDraw
    r = Image.open(render_png).convert("RGB")
    im = Image.open(shot).convert("RGB")
    # find the window: the title bar's top edge and the window's left/right edges (light pixels)
    px = im.load()
    w, h = im.size

    def light(p):
        return min(p) > 180 and max(p) - min(p) < 30

    rows = [y for y in range(h) if sum(light(px[x, y]) for x in range(0, w, 4)) > w / 8]
    top, bottom = rows[0], rows[-1]
    mid = (top + bottom) // 2
    cols = [x for x in range(w) if light(px[x, mid])]
    left, right = cols[0], cols[-1]
    win = im.crop((left, top, right + 1, bottom + 1)).resize(r.size, Image.LANCZOS)
    diff = ImageChops.difference(win, r)
    gray = diff.convert("L").point(lambda v: min(255, v * 3))
    sheet = Image.new("RGB", (r.width * 3 + 40, r.height + 30), "white")
    for i, (img, cap) in enumerate(((win, "10.6 (your screenshot)"), (r, "render"), (gray.convert("RGB"), "difference x3"))):
        sheet.paste(img, (10 + i * (r.width + 10), 25))
        ImageDraw.Draw(sheet).text((10 + i * (r.width + 10), 6), cap, fill=(0, 0, 0))
    sheet.save(out)
    data = gray.get_flattened_data() if hasattr(gray, "get_flattened_data") else gray.getdata()
    return (left, top, right, bottom), sum(data) / (gray.width * gray.height)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=str(rc.ROOT / ".local/media/re/mail/render"))
    ap.add_argument("--compare")
    ap.add_argument("--name", help="the Full Name field's contents (default: your account's)")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rc.use_dvd_fonts()
    png = out / "assistant.png"
    render(png, a.name)
    print("rendered", png)
    if a.compare:
        box, score = compare(png, a.compare, out / "assistant-compare.png")
        print(f"window found at {box}; mean difference {score:.1f}/255 -> {out / 'assistant-compare.png'}")


if __name__ == "__main__":
    main()
