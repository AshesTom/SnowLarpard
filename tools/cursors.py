#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Snow Leopard's cursors, from your own DVD's files, as an Xcursor theme for the session.

10.6 has two sets:

  - the window server's (CoreGraphics' _gCGXCursorInfo, 9 records of 0x38 bytes: id, name,
    hot spot x and y as doubles, width, height and frame count as 16-bit fields, the frames'
    duration as a float, the pixels' address): Arrow, IBeam, IBeamXOR, Alias, Copy, Move,
    ArrowCtx, Wait (the spinning wheel, 6 frames) and Empty, premultiplied BGRA;
  - HIServices' (Resources/cursordata: a version, a count, the entries' offsets; each entry's
    frames are PNGs, each after its width, height, hot spot and length): the theme cursors
    (kThemeAliasArrowCursor, kThemeNotAllowedCursor, the busy arrow, the hands, the resize
    cursors, the contextual menu arrow, the poof...).

The theme names them as freedesktop's cursor names (and X's core ones) ask, each the Mac's cursor
for that job: no diagonal resize cursors, as 10.6 had none (windows resize from their corner with
the arrow).

    cursors.py CoreGraphics cursordata THEME_DIR     (extract.py runs it on the DVD's)
"""
import struct
import sys
import zlib
from pathlib import Path

CG_RECORD = 0x38


def macho_slice(data, cputype=0x01000007):
    """The x86_64 slice of a fat Mach-O (or the file itself)."""
    if data[:4] == b"\xca\xfe\xba\xbe":
        n = struct.unpack_from(">I", data, 4)[0]
        for i in range(n):
            cpu, _sub, off, size, _align = struct.unpack_from(">IIIII", data, 8 + 20 * i)
            if cpu == cputype:
                return data[off:off + size]
    return data


def macho_reader(data):
    """(read(address, n), symbol(name) -> address) for a 64-bit Mach-O."""
    ncmds = struct.unpack_from("<I", data, 16)[0]
    o, segments, symtab = 32, [], None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, o)
        if cmd == 0x19:
            vmaddr, _vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, o + 24)
            segments.append((vmaddr, fileoff, filesize))
        elif cmd == 0x2:
            symtab = struct.unpack_from("<IIII", data, o + 8)
        o += size

    def read(address, n):
        for vmaddr, fileoff, filesize in segments:
            if vmaddr <= address < vmaddr + filesize:
                return data[fileoff + address - vmaddr:fileoff + address - vmaddr + n]
        return b""

    def symbol(name):
        if not symtab:
            return None
        symoff, nsyms, stroff, _ = symtab
        for i in range(nsyms):
            strx, _type, _sect, _desc, value = struct.unpack_from("<IBBHQ", data, symoff + 16 * i)
            end = data.index(b"\0", stroff + strx)
            if data[stroff + strx:end] == name:
                return value
        return None

    return read, symbol


def coregraphics_cursors(binary):
    """{name: (hot x, hot y, width, height, delay ms, [premultiplied ARGB frames as uint32 lists])}"""
    data = macho_slice(binary)
    read, symbol = macho_reader(data)
    table = symbol(b"_gCGXCursorInfo")
    out = {}
    if table is None:
        return out
    for i in range(9):
        rec = read(table + i * CG_RECORD, CG_RECORD)
        if len(rec) < CG_RECORD:
            break
        _id, name_at, hx, hy, packed, duration, pixels = struct.unpack("<QQddQQQ", rec)
        name = read(name_at, 64).split(b"\0")[0].decode()
        w, h, frames = packed & 0xFFFF, (packed >> 16) & 0xFFFF, (packed >> 32) & 0xFFFF
        delay = struct.unpack("<f", struct.pack("<I", duration & 0xFFFFFFFF))[0]
        raw = read(pixels, w * h * 4 * frames)
        # little-endian 0xAARRGGBB words: already Xcursor's own pixels
        words = list(struct.unpack(f"<{w * h * frames}I", raw))
        out[name.rsplit(".", 1)[-1]] = (int(hx), int(hy), w, h, int(round(delay * 1000)) or 50,
                                        [words[k * w * h:(k + 1) * w * h] for k in range(frames)])
    return out


def png_pixels(png):
    """A PNG's (width, height, premultiplied 0xAARRGGBB words), for 8-bit grey, grey and alpha, RGB, RGBA."""
    pos, chunks = 8, {}
    idat = b""
    while pos < len(png):
        length, kind = struct.unpack_from(">I4s", png, pos)
        body = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            chunks["IHDR"] = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    w, h, depth, color, _c, _f, interlace = struct.unpack(">IIBBBBB", chunks["IHDR"])
    if depth != 8 or color not in (0, 2, 4, 6) or interlace:
        raise ValueError("unsupported PNG")
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[color]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    words = []
    for line in rows:
        for x in range(w):
            px = line[x * bpp:(x + 1) * bpp]
            if color in (0, 4):
                r = g = b = px[0]
                a = px[1] if color == 4 else 255
            else:
                r, g, b = px[0], px[1], px[2]
                a = px[3] if color == 6 else 255
            words.append(a << 24 | (r * a // 255) << 16 | (g * a // 255) << 8 | (b * a // 255))
    return w, h, words


def hiservices_cursors(cursordata):
    """[(hot x, hot y, width, height, [frames])], in the file's order (frames: PNGs stacked down)"""
    _version, count = struct.unpack_from(">HH", cursordata, 0)
    offsets = [struct.unpack_from(">I", cursordata, 4 + 4 * i)[0] for i in range(count)]
    ends = offsets[1:] + [len(cursordata)]
    out = []
    for start, end in zip(offsets, ends):
        at = cursordata.find(b"\x89PNG", start, end)
        if at < 0:
            out.append(None)
            continue
        fw, fh, hx, hy = struct.unpack_from(">HHHH", cursordata, at - 20)
        length = struct.unpack_from(">I", cursordata, at - 4)[0]
        w, h, words = png_pixels(cursordata[at:at + length])
        frames = max(1, h // fh) if fh else 1
        fh = h // frames
        out.append((hx, hy, w, fh, [words[k * w * fh:(k + 1) * w * fh] for k in range(frames)]))
    return out


# HIServices' entries, in cursordata's order
HISERVICES = ["alias", "notallowed", "busy", "copy", "poof-old", "screenshot", "screenshot-selection",
              "camera", "camera-window", "closedhand", "openhand", "pointinghand",
              "countingup", "countingdown", "countingupdown", "resizeleft", "resizeright", "resizeleftright",
              "cross", "resizeup", "resizedown", "resizeupdown", "contextualmenu", "poof"]

# freedesktop's names (and X's core ones) for each of the Mac's cursors
NAMES = {
    "Arrow": ["default", "left_ptr", "arrow", "top_left_arrow", "X_cursor", "pirate", "nw-resize", "ne-resize",
              "sw-resize", "se-resize", "nwse-resize", "nesw-resize", "size_fdiag", "size_bdiag", "top_left_corner",
              "top_right_corner", "bottom_left_corner", "bottom_right_corner", "fd_double_arrow", "bd_double_arrow",
              "all-scroll", "fleur", "size_all"],
    "IBeam": ["text", "xterm", "ibeam", "vertical-text"],
    "Wait": ["wait", "watch", "clock"],
    "busy": ["progress", "left_ptr_watch", "half-busy"],
    "pointinghand": ["pointer", "hand", "hand1", "hand2", "pointing_hand", "9d800788f1b08800ae810202380a0822",
                     "e29285e634086352946a0e7090d73106"],
    "openhand": ["grab", "openhand", "fleur-open"],
    "closedhand": ["grabbing", "closedhand", "dnd-move", "move", "dnd-none"],
    "copy": ["copy", "dnd-copy", "1081e37283d90000800003c07f3ef6bf", "6407b0e94181790501fd1e167b474872"],
    "alias": ["alias", "link", "dnd-link", "3085a0e285430894940527032f8b26df", "640fb0e74195791501fd1ed57b41487f"],
    "notallowed": ["not-allowed", "no-drop", "forbidden", "crossed_circle", "dnd-no-drop", "circle",
                   "03b6e0fcb3499374a867c041f52298f0"],
    "contextualmenu": ["context-menu"],
    "cross": ["crosshair", "cross", "tcross", "cell", "plus", "diamond_cross", "cross_reverse"],
    "resizeleftright": ["col-resize", "ew-resize", "sb_h_double_arrow", "h_double_arrow", "size_hor", "split_h",
                        "14fef782d02440884392942c11205230", "028006030e0e7ebffc7f7070c0600140"],
    "resizeupdown": ["row-resize", "ns-resize", "sb_v_double_arrow", "v_double_arrow", "size_ver", "split_v",
                     "2870a09082c103050810ffdffffe0204", "00008160000006810000408080010102"],
    "resizeleft": ["w-resize", "left_side"],
    "resizeright": ["e-resize", "right_side"],
    "resizeup": ["n-resize", "top_side"],
    "resizedown": ["s-resize", "bottom_side"],
    "poof": ["poof"],
    # screencapture's (CoreCursorSet 7 to 10: HIServices' entries 5 to 8): the selection's, and the
    # camera of the window mode, to a file or to the clipboard
    "screenshot": ["snowleopard-screenshot"],
    "screenshot-selection": ["snowleopard-screenshot-selection"],
    "camera": ["snowleopard-camera"],
    "camera-window": ["snowleopard-camera-window"],
    "Empty": ["none"],
}


def xcursor(cursor, nominal):
    """One cursor's Xcursor file: its frames, each an image chunk"""
    hx, hy, w, h, delay, frames = cursor
    toc, chunks = [], b""
    header = 16 + 12 * len(frames)
    for frame in frames:
        toc.append((0xFFFD0002, nominal, header + len(chunks)))
        chunks += struct.pack("<9I", 36, 0xFFFD0002, nominal, 1, w, h, min(hx, w - 1), min(hy, h - 1), delay)
        chunks += struct.pack(f"<{w * h}I", *frame)
    out = struct.pack("<4sIII", b"Xcur", 16, 0x10000, len(frames))
    for t in toc:
        out += struct.pack("<III", *t)
    return out + chunks


def write_theme(theme, coregraphics, hiservices):
    """The theme's folder: cursors/<name> (the names of one cursor linked to its first)"""
    cursors = dict(coregraphics)
    for name, entry in zip(HISERVICES, hiservices):
        if entry:
            hx, hy, w, h, frames = entry
            # the busy arrow turns as the wheel does (its 15 frames)
            cursors[name] = (hx, hy, w, h, 33 if len(frames) > 1 else 50, frames)
    folder = Path(theme) / "cursors"
    folder.mkdir(parents=True, exist_ok=True)
    (Path(theme) / "index.theme").write_text("[Icon Theme]\nName=Snow Leopard\nComment=Mac OS X 10.6.8's cursors, "
                                             "from the window server and HIServices\nInherits=default\n")
    written = 0
    for mac, names in NAMES.items():
        if mac not in cursors:
            continue
        data = xcursor(cursors[mac], 24)
        first = folder / names[0]
        first.write_bytes(data)
        written += 1
        for other in names[1:]:
            link = folder / other
            if link.exists() or link.is_symlink():
                link.unlink()
            link.symlink_to(names[0])
    return written


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip())
        return 2
    cg = coregraphics_cursors(Path(sys.argv[1]).read_bytes())
    hi = hiservices_cursors(Path(sys.argv[2]).read_bytes())
    n = write_theme(sys.argv[3], cg, hi)
    print(f"{n} cursors ({len(cg)} from CoreGraphics, {sum(1 for h in hi if h)} from HIServices) in {sys.argv[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
