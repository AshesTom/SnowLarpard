#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Takes what the Snow Leopard shell needs from your own Mac OS X 10.6 install DVD.

    extract.py DVD.iso [ASSETS]        (ASSETS: .local/assets by default)
    extract.py .local/media/MacOSX.hfs [ASSETS]   (again, from the partition a first run kept)
    extract.py --update PKG|DIR... [--assets ASSETS]   an Apple update's packages over the files
                                                       (the 10.6.8 Combo Update: tools/softwareupdate.py)

The DVD is a hybrid: an ISO 9660 side for Boot Camp and an Apple partition map whose Apple_HFS
partition is the installer. Its packages are xar archives holding a gzip'd cpio Payload. Only the
files listed in WANTED are kept, at their Mac path under ASSETS (ASSETS/System/Library/...), so the
shell finds them where Snow Leopard has them. Nothing of the DVD is in this repository.

Needs 7z (for HFS+) and cpio.
"""
import codecs
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # cursors.py beside it

PACKAGES = "Mac OS X Install DVD/System/Installation/Packages"

# package -> cpio patterns (fnmatch, '*' crosses '/')
WANTED = {
    "BaseSystem.pkg": [
        # the system's version, build and copyright (About This Mac)
        "./System/Library/CoreServices/SystemVersion.plist",
        # the controls', menus' and title bars' art: CoreUI's ArtFile.bin and SArtFile.bin (its
        # UI.bundle recipes and HIToolbox's .rsrc are only for reading, in the DVD's packages)
        "./System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/HIToolbox.framework/Versions/A/Resources/*.png",
        "./System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/Resources/*ArtFile.bin",
        "./System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/*.icns",
        # the types' names (the Finder's Kind: UTTypeDescription, by extension) and their icons
        "./System/Library/CoreServices/CoreTypes.bundle/Contents/Info.plist",
        "./System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/English.lproj/InfoPlist.strings",
        "./System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Resources/English.lproj/Localized.strings",
        # the disks' icons (the Finder's sidebar: the internal disk, external and removable ones)
        "./System/Library/Extensions/IOStorageFamily.kext/Contents/Resources/*.icns",
        "./System/Library/CoreServices/DefaultDesktop.jpg",
        # the AirPort menu extra's bars, scanning and off pictures (BaseSystem has AirPort.menu,
        # Essentials the other extras)
        "./System/Library/CoreServices/Menu Extras/AirPort.menu/Contents/Resources/*.pdf",
        "./System/Library/CoreServices/Menu Extras/*.menu/Contents/Info.plist",
        "./System/Library/CoreServices/Menu Extras/*.menu/Contents/Resources/English.lproj/*.strings",
        # its panels (Apple80211's ClientDialogs: join a network, its password, create one) and the
        # security types' names (ClientController)
        "./System/Library/PrivateFrameworks/Apple80211.framework/Versions/A/Resources/ClientDialogs.bundle/Contents/Resources/English.lproj/*",
        "./System/Library/PrivateFrameworks/Apple80211.framework/Versions/A/Resources/ClientDialogs.bundle/Contents/Resources/*.icns",
        "./System/Library/PrivateFrameworks/Apple80211.framework/Versions/A/Resources/ClientDialogs.bundle/Contents/Resources/*.pdf",
        "./System/Library/PrivateFrameworks/Apple80211.framework/Versions/A/Resources/ClientController.bundle/Contents/Resources/English.lproj/Localizable.strings",
        # the colour spaces ImageIO gives untagged icns data (Generic RGB, Generic Gray), for
        # matching them to the screen (_CGImagePluginInitICNS)
        "./System/Library/ColorSync/Profiles/Generic RGB Profile.icc",
        "./System/Library/ColorSync/Profiles/Generic Gray Profile.icc",
        "./System/Library/Fonts/LucidaGrande.ttc",
        "./System/Library/Fonts/HelveticaNeue.ttc",
        # the fonts a Stickies note may be in (its RTF names them: Helvetica, Times, Courier...)
        "./System/Library/Fonts/*.dfont",
        "./System/Library/Fonts/Menlo.ttc",
        # NSDocument's and NSUndoManager's strings (AppKit's): Untitled, the save alerts, Undo %@
        "./System/Library/Frameworks/AppKit.framework/Versions/C/Resources/English.lproj/Document.strings",
        "./System/Library/Frameworks/AppKit.framework/Versions/C/Resources/English.lproj/Undo.strings",
        # NSTextView's find panel and its ruler's accessory and strings (TextEdit's, AppKit's)
        "./System/Library/Frameworks/AppKit.framework/Versions/C/Resources/English.lproj/NSFindPanel.nib/*",
        "./System/Library/Frameworks/AppKit.framework/Versions/C/Resources/English.lproj/FindPanel.strings",
        "./System/Library/Frameworks/AppKit.framework/Versions/C/Resources/English.lproj/NSTextRulerAccessory.nib/*",
        "./System/Library/Frameworks/AppKit.framework/Versions/C/Resources/English.lproj/Ruler.strings",
        "./Applications/Utilities/*.app/Contents/Info.plist",
        "./Applications/Utilities/*.app/Contents/Resources/*.icns",
        # Parental Controls' pane is BaseSystem's (the others, Essentials')
        "./System/Library/PreferencePanes/ParentalControls.prefPane/Contents/Info.plist",
        "./System/Library/PreferencePanes/ParentalControls.prefPane/Contents/Resources/*.icns",
        "./System/Library/PreferencePanes/ParentalControls.prefPane/Contents/Resources/*.png",
        "./System/Library/PreferencePanes/ParentalControls.prefPane/Contents/Resources/*.tif*",
        "./System/Library/PreferencePanes/ParentalControls.prefPane/Contents/Resources/English.lproj/*",
    ],
    "Essentials.pkg": [
        # Calculator (shell/src/calculator): its LCD, the tape's button, the keys' pictures, its nibs,
        # strings and the conversions' tables
        "./Applications/Calculator.app/Contents/Resources/*.tif",
        "./Applications/Calculator.app/Contents/Resources/*.plist",
        "./Applications/Calculator.app/Contents/Resources/*.xml",
        "./Applications/Calculator.app/Contents/Resources/English.lproj/*",
        "./Applications/Calculator.app/Contents/PlugIns/*.calcview/Contents/Info.plist",
        "./Applications/Calculator.app/Contents/PlugIns/*.calcview/Contents/Resources/*",
        # Stickies (shell/src/stickies): its default settings and welcome notes, its template's icon,
        # the self test's note, its nibs and strings; the fonts of /Library/Fonts its notes may use
        "./Applications/Stickies.app/Contents/Resources/*.plist",
        "./Applications/Stickies.app/Contents/Resources/*.tiff",
        "./Applications/Stickies.app/Contents/Resources/*.rtf",
        "./Applications/Stickies.app/Contents/Resources/English.lproj/*",
        "./Applications/Stickies.app/Contents/Resources/Stickies.help/*",
        "./Library/Fonts/*.ttc",
        "./Library/Fonts/*.ttf",
        # TextEdit (shell/src/textedit): its window's and panels' nibs, strings, document icons, help
        # and test document (TextEdit 1.6 (264): the same binary on 10.6 and 10.6.8)
        "./Applications/TextEdit.app/Contents/version.plist",
        "./Applications/TextEdit.app/Contents/Resources/*.rtf",
        "./Applications/TextEdit.app/Contents/Resources/DocumentWindow.nib/*",
        "./Applications/TextEdit.app/Contents/Resources/English.lproj/*",
        "./Applications/TextEdit.app/Contents/Resources/TextEdit.help/*",
        # Photo Booth (shell/src/photobooth): its pictures, sounds, nibs and strings, and the three
        # compositions whose embedded pictures its effects use (composition_images takes them out;
        # the effects themselves are in effects.cpp, so the .qtz files go afterwards)
        "./Applications/Photo Booth.app/Contents/Resources/*.png",
        "./Applications/Photo Booth.app/Contents/Resources/*.tiff",
        "./Applications/Photo Booth.app/Contents/Resources/*.aiff",
        "./Applications/Photo Booth.app/Contents/Resources/*.plist",
        "./Applications/Photo Booth.app/Contents/Resources/English.lproj/*",
        "./System/Library/Compositions/ThermalCamera.qtz",
        "./System/Library/Compositions/XRay.qtz",
        "./System/Library/Compositions/ColorPencil.qtz",
        # Preview's thumbnails: IKImageBrowserView's shadow (IKShadowTool's pbShadow* pictures)
        "./System/Library/Frameworks/Quartz.framework/Versions/A/Frameworks/ImageKit.framework/Versions/A/Resources/pbShadow*.tiff",
        # About This Mac: loginwindow's window (its nib, strings and the Mac OS X picture)
        "./System/Library/CoreServices/loginwindow.app/Contents/Resources/English.lproj/AboutThisMac.*",
        "./System/Library/CoreServices/loginwindow.app/Contents/Resources/English.lproj/AboutThisMac.nib/*",
        "./System/Library/CoreServices/loginwindow.app/Contents/Resources/MacOSX.tif",
        "./System/Library/CoreServices/loginwindow.app/Contents/Info.plist",
        # the numbers' formats About This Mac uses (AppleSystemInfo: "%.*f GB")
        "./System/Library/CoreServices/Resources/English.lproj/AppleSystemInfo.strings",
        # Software Update (shell/src/softwareupdate): its nibs, strings and pictures
        "./System/Library/CoreServices/Software Update.app/Contents/Info.plist",
        "./System/Library/CoreServices/Software Update.app/Contents/Resources/*.tif",
        "./System/Library/CoreServices/Software Update.app/Contents/Resources/*.tiff",
        "./System/Library/CoreServices/Software Update.app/Contents/Resources/*.icns",
        "./System/Library/CoreServices/Software Update.app/Contents/Resources/English.lproj/*",
        "./System/Library/CoreServices/Dock.app/Contents/Resources/*.png",
        "./System/Library/CoreServices/Dock.app/Contents/Resources/*.icns",
        "./System/Library/CoreServices/Dock.app/Contents/Resources/*.plist",
        "./System/Library/CoreServices/Dock.app/Contents/Resources/English.lproj/*.strings",
        "./System/Library/CoreServices/Finder.app/Contents/Info.plist",
        # the Finder's windows: its nibs, strings, pictures (its code is encrypted; docs/reverse-status.md 13)
        "./System/Library/CoreServices/Finder.app/Contents/Resources/English.lproj/*",
        "./System/Library/CoreServices/Finder.app/Contents/Resources/English.lproj/*.nib/*",
        "./System/Library/CoreServices/Finder.app/Contents/Resources/*.png",
        "./System/Library/CoreServices/Finder.app/Contents/Resources/*.tiff",
        "./System/Library/CoreServices/Finder.app/Contents/Resources/*.tif",
        "./System/Library/CoreServices/Finder.app/Contents/Resources/*.icns",
        # the sidebar's SEARCH FOR items: each canned search's icon and name
        "./System/Library/CoreServices/Finder.app/Contents/Resources/CannedSearches/*/Resources/*.icns",
        "./System/Library/CoreServices/Finder.app/Contents/Resources/CannedSearches/*/Resources/English.lproj/*",
        # System Preferences and its panes: the groups, names, icons, pictures and nibs (AquaKit loads
        # the panes' nibs at run time); not their code
        "./Applications/System Preferences.app/Contents/Resources/*.xml",
        "./Applications/System Preferences.app/Contents/Resources/*.png",
        "./Applications/System Preferences.app/Contents/Resources/*.tif",
        "./Applications/System Preferences.app/Contents/Resources/English.lproj/*",
        "./System/Library/PreferencePanes/*.prefPane/Contents/Info.plist",
        "./System/Library/PreferencePanes/*.prefPane/Contents/Resources/*.icns",
        "./System/Library/PreferencePanes/*.prefPane/Contents/Resources/*.png",
        "./System/Library/PreferencePanes/*.prefPane/Contents/Resources/*.tif",
        "./System/Library/PreferencePanes/*.prefPane/Contents/Resources/*.tiff",
        "./System/Library/PreferencePanes/*.prefPane/Contents/Resources/*.pdf",
        "./System/Library/PreferencePanes/*.prefPane/Contents/Resources/English.lproj/*",
        # the search field's terms (Fibre Channel keeps them beside its resources, not in English.lproj)
        "./System/Library/PreferencePanes/*.prefPane/Contents/Resources/*.searchTerms",
        # Preview: its nibs, toolbar, pictures and strings (Preview.app on AquaKit reads them)
        "./Applications/Preview.app/Contents/Resources/*.tiff",
        "./Applications/Preview.app/Contents/Resources/*.tif",
        "./Applications/Preview.app/Contents/Resources/*.png",
        "./Applications/Preview.app/Contents/Resources/*.proplist",
        "./Applications/Preview.app/Contents/Resources/*.plist",
        "./Applications/Preview.app/Contents/Resources/English.lproj/*",
        # a new account's Documents and Downloads: Apple's PDFs about Stacks (the Dock puts them there once)
        "./System/Library/User Template/English.lproj/Documents/*.pdf",
        "./System/Library/User Template/English.lproj/Downloads/*.pdf",
        # Displays' panels (resolutions, colour, arrangement, brightness): nibs of their own
        "./System/Library/MonitorPanels/*.monitorPanel*/Contents/Info.plist",
        "./System/Library/MonitorPanels/*/Contents/Resources/English.lproj/*",
        "./System/Library/MonitorPanels/*/Contents/Resources/*.tif*",
        "./System/Library/MonitorPanels/*/Contents/Resources/*.png",
        "./System/Library/MonitorPanels/*/Contents/Resources/*.icns",
        "./System/Library/CoreServices/Menu Extras/*.menu/Contents/Resources/*.pdf",
        # Spotlight's item (Search.bundle): its magnifier, its menu's patterns, its strings
        "./System/Library/CoreServices/Search.bundle/Contents/Info.plist",
        "./System/Library/CoreServices/Search.bundle/Contents/Resources/*.pdf",
        "./System/Library/CoreServices/Search.bundle/Contents/Resources/*.tif*",
        "./System/Library/CoreServices/Search.bundle/Contents/Resources/English.lproj/*.strings",
        "./System/Library/CoreServices/Menu Extras/*.menu/Contents/Resources/*.png",
        "./System/Library/CoreServices/Menu Extras/*.menu/Contents/Resources/*.tif*",
        # their Info.plist (NSMenuExtraWidth, NSPrincipalClass) and their strings (the Clock's menu)
        "./System/Library/CoreServices/Menu Extras/*.menu/Contents/Info.plist",
        "./System/Library/CoreServices/Menu Extras/*.menu/Contents/Resources/English.lproj/*.strings",
        "./System/Library/CoreServices/SystemUIServer.app/Contents/Resources/*",
        "./System/Library/CoreServices/loginwindow.app/Contents/Resources/*",
        "./Applications/*.app/Contents/Info.plist",
        "./Applications/*.app/Contents/Resources/*.icns",
    ],
    "MediaFiles.pkg": ["./Library/Desktop Pictures/*"],
}
for app in ("Safari", "Mail", "iTunes", "iCal", "AddressBook", "iChat", "DVDPlayer"):
    WANTED[app + ".pkg"] = ["./Applications/*.app/Contents/Info.plist", "./Applications/*.app/Contents/Resources/*.icns"]
# Mail.app (shell/src/mail): its pictures, toolbars and English nibs and strings (docs/mail-reverse.md)
WANTED["Mail.pkg"] += [
    "./Applications/Mail.app/Contents/Resources/*.tiff",
    "./Applications/Mail.app/Contents/Resources/*.tif",
    "./Applications/Mail.app/Contents/Resources/*.png",
    "./Applications/Mail.app/Contents/Resources/*.toolbar",
    "./Applications/Mail.app/Contents/Resources/English.lproj/*",
]


def say(*args):
    print("==>", *args, flush=True)


def hfs_partition(iso):
    """(offset, length) of the Apple_HFS partition in the DVD's Apple partition map."""
    with open(iso, "rb") as f:
        head = f.read(4)
        if head[:2] != b"ER":
            sys.exit(f"{iso}: no Apple partition map (is it the Mac OS X install DVD?)")
        bs = struct.unpack(">H", head[2:4])[0]
        for i in range(1, 64):
            f.seek(bs * i)
            entry = f.read(512)
            if entry[:2] != b"PM":
                break
            start, count = struct.unpack(">II", entry[8:16])
            if entry[48:80].rstrip(b"\0") == b"Apple_HFS":
                return start * bs, count * bs
    sys.exit(f"{iso}: no Apple_HFS partition")


def is_hfs(path):
    """An HFS+ volume (the partition carve() writes) rather than the DVD."""
    with open(path, "rb") as f:
        f.seek(1024)
        return f.read(2) in (b"H+", b"HX")


def carve(iso, out):
    """The HFS+ partition as a file of its own, for 7z (kept as a cache next to ASSETS)."""
    offset, length = hfs_partition(iso)
    if out.exists() and out.stat().st_size == length:
        return
    say(f"Reading the HFS+ partition ({length >> 20} MiB)")
    tmp = out.with_suffix(".part")
    with open(iso, "rb") as src, open(tmp, "wb") as dst:
        src.seek(offset)
        left = length
        while left:
            chunk = src.read(min(left, 16 << 20))
            if not chunk:
                sys.exit(f"{iso}: truncated")
            dst.write(chunk)
            left -= len(chunk)
    tmp.rename(out)


def xar_payload(pkg):
    """(offset, length, encoding) of a flat package's Payload."""
    with open(pkg, "rb") as f:
        magic, hsize, _ver, toc_c, _toc_u, _ck = struct.unpack(">4sHHQQI", f.read(28))
        if magic != b"xar!":
            raise ValueError(f"{pkg}: not a xar archive")
        f.seek(hsize)
        toc = ET.fromstring(zlib.decompress(f.read(toc_c)))
        heap = hsize + toc_c
    for file in toc.iter("file"):
        if file.findtext("name") == "Payload":
            data = file.find("data")
            return (heap + int(data.findtext("offset")), int(data.findtext("length")),
                    data.find("encoding").get("style"))
    raise ValueError(f"{pkg}: no Payload")


def unpack(pkg, patterns, dest, overwrite=False, progress=None):
    """The Payload's files matching the patterns, under dest. overwrite: over the files already there
    (an update's, whatever their dates); progress(bytes) as the Payload is read."""
    offset, length, encoding = xar_payload(pkg)
    cpio = subprocess.Popen(["cpio", "-idm", "--quiet", "--no-absolute-filenames", *(["-u"] if overwrite else []),
                             *patterns], cwd=dest, stdin=subprocess.PIPE, stderr=subprocess.DEVNULL if overwrite else None)
    with open(pkg, "rb") as f:
        f.seek(offset)
        # The TOC says octet-stream for Payloads that are gzip'd all the same: the bytes tell.
        gzipped = "gzip" in encoding or f.read(2) == b"\x1f\x8b"
        inflate = zlib.decompressobj(31) if gzipped else None
        f.seek(offset)
        left = length
        while left:
            chunk = f.read(min(left, 4 << 20))
            if not chunk:
                break
            left -= len(chunk)
            cpio.stdin.write(inflate.decompress(chunk) if inflate else chunk)
            if progress:
                progress(len(chunk))
        if inflate:
            cpio.stdin.write(inflate.flush())
    cpio.stdin.close()
    if cpio.wait():
        raise RuntimeError(f"cpio failed on {pkg.name}")


MENU_APPLE = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/MenuApple-32x22.rgba"


def macho_slice(data, cputype=0x01000007):
    """The x86_64 slice of a universal Mach-O (or the file itself)."""
    if data[:4] == b"\xca\xfe\xba\xbe":
        for i in range(struct.unpack(">I", data[4:8])[0]):
            ct, _cs, off, size, _al = struct.unpack(">5I", data[8 + 20 * i:28 + 20 * i])
            if ct == cputype:
                return data[off:off + size]
    return data


def macho_symbol(data, name):
    """The bytes' file offset of a symbol of a 64-bit Mach-O, or None."""
    ncmds = struct.unpack_from("<I", data, 16)[0]
    o, sections, symtab = 32, [], None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, o)
        if cmd == 0x19:   # LC_SEGMENT_64: its sections' addresses and file offsets
            for i in range(struct.unpack_from("<I", data, o + 64)[0]):
                s = o + 72 + 80 * i
                sections.append(struct.unpack_from("<QQI", data, s + 32))
        elif cmd == 0x2:  # LC_SYMTAB
            symtab = struct.unpack_from("<IIII", data, o + 8)
        o += size
    if not symtab:
        return None
    symoff, nsyms, stroff, _strsize = symtab
    for i in range(nsyms):
        strx, _type, _sect, _desc, value = struct.unpack_from("<IBBHQ", data, symoff + 16 * i)
        end = data.index(b"\0", stroff + strx)
        if data[stroff + strx:end] == name:
            for addr, size, off in sections:
                if addr <= value < addr + size:
                    return off + value - addr
    return None


def menu_apple(pkg, assets, tmp):
    """The window server's menu bar Apple (QuartzCore's CIMenuGenerator draws it from a 32 x 22 RGBA
    picture inside the library): its bytes, for the shell's menu bar filter."""
    quartz = "./System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore"
    unpack(pkg, [quartz], tmp)
    binary = Path(tmp) / quartz
    if not binary.exists():
        return
    data = macho_slice(binary.read_bytes())
    at = macho_symbol(data, b"_imageApple")
    if at is None:
        print("    QuartzCore: no _imageApple, the menu bar keeps CoreUI's Apple")
        return
    out = assets / MENU_APPLE.lstrip("/")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data[at:at + 32 * 22 * 4])
    shutil.rmtree(Path(tmp) / "System", ignore_errors=True)


CURSOR_THEME = "share/icons/SnowLeopard"


def cursor_theme(pkg, assets, tmp):
    """10.6's cursors (the window server's in CoreGraphics, HIServices' cursordata) as an Xcursor
    theme the session uses (tools/cursors.py)."""
    import cursors
    cg = "./System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics"
    hi = "./System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/HIServices.framework/Versions/A/Resources/cursordata"
    unpack(pkg, [cg, hi], tmp)
    cgfile, hifile = Path(tmp) / cg, Path(tmp) / hi
    if cgfile.exists() and hifile.exists():
        n = cursors.write_theme(assets / CURSOR_THEME, cursors.coregraphics_cursors(cgfile.read_bytes()),
                                cursors.hiservices_cursors(hifile.read_bytes()))
        print(f"    {n} cursors")
    shutil.rmtree(Path(tmp) / "System", ignore_errors=True)


def strings_to_dict(text):
    """An OpenStep .strings file (KEY = "value"; or "key" = "value";) as a dict."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(?m)^\s*//.*$", "", text)
    token = r'(?:"((?:[^"\\]|\\.)*)"|([A-Za-z0-9_.\-]+))'
    out = {}
    for m in re.finditer(token + r"\s*=\s*" + token + r"\s*;", text):
        key = m.group(1) if m.group(1) is not None else m.group(2)
        val = m.group(3) if m.group(3) is not None else m.group(4)
        out[codecs.decode(key, "unicode_escape") if "\\" in key else key] = \
            val.replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")
    return out


def to_json(assets):
    """The plists and .strings the shell reads, as JSON beside them (Qt reads JSON, not plists)."""
    for path in list(assets.rglob("*.plist")) + list(assets.rglob("*.strings")):
        try:
            if path.suffix == ".plist":
                with open(path, "rb") as f:
                    data = plistlib.load(f)
            else:
                raw = path.read_bytes()
                enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
                data = strings_to_dict(raw.decode(enc, "replace"))
        except Exception as e:  # noqa: BLE001 - a file the shell can do without
            print(f"    {path.name}: {e}")
            continue
        Path(str(path) + ".json").write_text(json.dumps(data, default=str, ensure_ascii=False))


def composition_images(assets):
    """The pictures Quartz compositions embed (an Image Loader patch's imageData): Thermal Camera's
    and X-Ray's colour maps, Color Pencil's paper, written beside the composition as
    <Name>.qtz.<patch key>.png|tiff for Photo Booth's effects."""
    def walk(patch, out):
        for node in patch.get("state", {}).get("nodes", []):
            data = node.get("state", {}).get("imageData")
            if node.get("class") == "QCImageLoader" and isinstance(data, bytes):
                ext = "png" if data[:4] == b"\x89PNG" else "tiff" if data[:2] in (b"MM", b"II") else "jpg"
                Path(f"{out}.{node['key']}.{ext}").write_bytes(data)
            walk(node, out)
    for name in ("ThermalCamera", "XRay", "ColorPencil"):
        path = assets / "System/Library/Compositions" / f"{name}.qtz"
        try:
            with open(path, "rb") as f:
                walk(plistlib.load(f)["rootPatch"], str(path))
        except Exception as e:  # noqa: BLE001 - Photo Booth then leaves the effect out
            print(f"    {path.name}: {e}")


def is_macho(path):
    with open(path, "rb") as f:
        return f.read(4) in (b"\xca\xfe\xba\xbe", b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf",
                             b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe")


def prune(assets):
    """What the patterns take beyond the shell's needs: cpio's '*' crosses '/', so Resources/*.png
    also takes every language's pictures. Only English is kept, and no code (Mach-O)."""
    for lproj in sorted(assets.rglob("*.lproj"), reverse=True):
        if lproj.is_dir() and not lproj.is_symlink() and lproj.name not in ("English.lproj", "en.lproj"):
            shutil.rmtree(lproj)
    for path in list(assets.rglob("*")):
        if path.is_file() and not path.is_symlink() and is_macho(path):
            path.unlink()
    for qtz in (assets / "System/Library/Compositions").glob("*.qtz"):
        qtz.unlink()      # composition_images took what Photo Booth reads out of them
    for d in sorted((p for p in assets.rglob("*") if p.is_dir()), key=lambda p: -len(p.parts)):
        if not any(d.iterdir()):
            d.rmdir()


def optimize_jpegs(assets):
    """The pictures' Huffman tables optimized (jpegtran -optimize): the same pixels, about 5%
    smaller. Baseline still, so that turbojpeg's scaled decoding (the Desktop pane's thumbnails)
    stays fast."""
    if not shutil.which("jpegtran"):
        return
    for jpg in list(assets.rglob("*.jpg")):
        out = jpg.with_suffix(".tmp")
        got = subprocess.run(["jpegtran", "-copy", "all", "-optimize", "-outfile", str(out), str(jpg)],
                             stderr=subprocess.DEVNULL)
        if got.returncode == 0 and out.exists() and 0 < out.stat().st_size < jpg.stat().st_size:
            shutil.copystat(jpg, out)
            out.replace(jpg)
        else:
            out.unlink(missing_ok=True)


def system_version(assets):
    try:
        with open(assets / "System/Library/CoreServices/SystemVersion.plist", "rb") as f:
            return plistlib.load(f)
    except OSError:
        return {}


def update(sources, final):
    """An Apple update's flat packages (Software Update's, or the Combo Update's) over the files:
    the patterns of every DVD package taken from each Payload, over a copy of the files that
    replaces them once complete. Prints "PROGRESS done total" as the Payloads are read."""
    if not (final / ".complete").exists():
        sys.exit(f"{final}: no DVD files to update (extract.py DVD.iso first)")
    pkgs = []
    for src in sources:
        src = Path(src).expanduser().resolve()
        pkgs += sorted(src.glob("*.pkg")) if src.is_dir() else [src]
    with_payload = []
    for pkg in pkgs:
        try:
            with_payload.append((pkg, xar_payload(pkg)[1]))
        except (ValueError, OSError, struct.error, zlib.error):
            print(f"    {pkg.name}: no payload, skipped")    # the product's own package: scripts only
    if not with_payload:
        sys.exit("no package with files to install")
    patterns = list(dict.fromkeys(p for pats in WANTED.values() for p in pats))
    assets = final.with_name(final.name + ".new")
    shutil.rmtree(assets, ignore_errors=True)
    say(f"Copying {final}")
    shutil.copytree(final, assets, symlinks=True)
    (assets / ".complete").unlink(missing_ok=True)
    total = sum(n for _, n in with_payload)
    done = 0

    def advance(n):
        nonlocal done
        done += n
        print(f"PROGRESS {done} {total}", flush=True)

    for pkg, _ in with_payload:
        say(f"Installing {pkg.name}")
        unpack(pkg, patterns, assets, overwrite=True, progress=advance)
    if any((assets / "System/Library/Compositions").glob("*.qtz")):
        composition_images(assets)
    prune(assets)
    optimize_jpegs(assets)
    to_json(assets)
    version = system_version(assets)
    (assets / ".complete").write_text(f"{version.get('ProductVersion', '10.6')}\n")
    old = final.with_name(final.name + ".old")
    shutil.rmtree(old, ignore_errors=True)
    final.rename(old)
    assets.rename(final)
    shutil.rmtree(old)
    say(f"Done: Mac OS X {version.get('ProductVersion', '?')} ({version.get('ProductBuildVersion', '?')}) in {final}")
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--update":
        args = sys.argv[2:]
        root = Path(__file__).resolve().parent.parent
        final = root / ".local/assets"
        if "--assets" in args:
            i = args.index("--assets")
            final = Path(args[i + 1]).expanduser().resolve()
            del args[i:i + 2]
        if not args:
            sys.exit("extract.py --update PKG|DIR... [--assets ASSETS]")
        if not shutil.which("cpio"):
            sys.exit("cpio is needed (pacman -S cpio)")
        return update(args, final)
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if len(sys.argv) > 1 else 2
    for tool in ("7z", "cpio"):
        if not shutil.which(tool):
            sys.exit(f"{tool} is needed (pacman -S 7zip cpio)")
    iso = Path(sys.argv[1]).expanduser().resolve()
    root = Path(__file__).resolve().parent.parent
    final = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else root / ".local/assets"
    if final.exists() and any(final.iterdir()) and not (final / ".complete").exists():
        sys.exit(f"{final}: not empty and not a previous extraction; give another folder")
    # a fresh tree beside it, which replaces the previous extraction only once complete: a run
    # with fewer patterns leaves nothing of the previous one behind
    assets = final.with_name(final.name + ".new")
    shutil.rmtree(assets, ignore_errors=True)
    cache = root / ".local/media"
    cache.mkdir(parents=True, exist_ok=True)
    assets.mkdir(parents=True)
    hfs = cache / "MacOSX.hfs"
    if is_hfs(iso):
        hfs = iso          # the partition a previous run carved out: no need for the DVD again
    else:
        carve(iso, hfs)
    with tempfile.TemporaryDirectory(dir=cache) as tmp:
        for name, patterns in WANTED.items():
            say(f"Taking {name}")
            got = subprocess.run(["7z", "e", "-tHFS", "-y", f"-o{tmp}", str(hfs), f"{PACKAGES}/{name}"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            pkg = Path(tmp) / name
            if got.returncode or not pkg.exists():
                print(f"    not on this DVD, skipped ({got.stderr.strip()[:200]})")
                continue
            unpack(pkg, patterns, assets)
            if name == "BaseSystem.pkg":
                menu_apple(pkg, assets, tmp)
                cursor_theme(pkg, assets, tmp)
            pkg.unlink()
    composition_images(assets)
    prune(assets)
    optimize_jpegs(assets)
    to_json(assets)
    (assets / ".complete").write_text("10.6\n")
    if final.exists():
        old = final.with_name(final.name + ".old")
        shutil.rmtree(old, ignore_errors=True)
        final.rename(old)
        assets.rename(final)
        shutil.rmtree(old)
    else:
        assets.rename(final)
    assets = final
    size = sum(p.stat().st_size for p in assets.rglob("*") if p.is_file() and not p.is_symlink())
    say(f"Done: {assets} ({size >> 20} MiB)")
    print(f"    The HFS+ partition stays in {hfs} for the next run; delete it to get the space back.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
