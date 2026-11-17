#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Software Update for the shell's Mac OS X files: Apple's own catalog, as 10.6's Software Update reads it.

    softwareupdate.py scan [--assets ASSETS]              the updates for the files (JSON)
    softwareupdate.py download KEY [--dir DIR]            one update's packages (PROGRESS done total)
    softwareupdate.py install KEY [--dir DIR] [--assets ASSETS]   its packages over the files

10.6's Software Update (SoftwareUpdate.framework) reads the catalog
index-leopard-snowleopard.merged-1.sucatalog on swscan.apple.com: products, each with its packages
(swcdn.apple.com), a distribution script that says which systems it is for, and its metadata (.smd:
the English title, version and description). Apple still serves them. The only update the shell
can apply is the client's Mac OS X Update Combined 10.6.8 v1.1, for 10.6 (10A432) up to 10.6.7, as
its .dist allows (InstallationCheck / VolumeCheck); its packages hold the 10.6.8 files, which
extract.py --update lays over the DVD's. X11, QuickTime Player 7 and Rosetta's packages, which have
nothing the shell reads, are left out.

Nothing is sent but the requests for Apple's public files. The packages (about 1 GB) go to
$XDG_CACHE_HOME/snowleopard/Updates/KEY, as Software Update keeps them in /Library/Updates, and are
deleted once installed.
"""
import json
import os
import plistlib
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

CATALOG = "https://swscan.apple.com/content/catalogs/others/index-leopard-snowleopard.merged-1.sucatalog"
AGENT = "Software%20Update (unknown version) CFNetwork/454.12.4 Darwin/10.8.0 (i386)"
# the client's combined update: its packages are named so (the server's are MacOSXServerUpdCombo...)
COMBO = re.compile(r"/(MacOSXUpdCombo10\.6\.8\.Part\d+|SUBaseSystemCombo10\.6\.8)\.pkg$")
ROOT = Path(__file__).resolve().parent.parent


def fetch(url, timeout=60):
    req = urllib.request.Request(https(url), headers={"User-Agent": AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def https(url):
    return "https://" + url[len("http://"):] if url.startswith("http://") else url


def assets_dir(args):
    if "--assets" in args:
        return Path(args[args.index("--assets") + 1]).expanduser().resolve()
    env = os.environ.get("SNOWLEOPARD_ASSETS")
    return Path(env).resolve() if env else ROOT / ".local/assets"


def cache_dir(args, key):
    if "--dir" in args:
        return Path(args[args.index("--dir") + 1]).expanduser().resolve()
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return Path(base) / "snowleopard/Updates" / key


def version_tuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v))


def build_tuple(b):
    # the .dist's compareBuildVersions: 10A432 -> (10, 'A', 432, '')
    m = re.match(r"(\d+)([A-Z])(\d+)([a-z]?)", b or "")
    return (int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)) if m else (0, "", 0, "")


def applies(system):
    """The combo's .dist: ProductVersion in [10.6, 10.6.8), build from 10A432 to 10K1999, a client."""
    v, b = system.get("ProductVersion", ""), system.get("ProductBuildVersion", "")
    return (version_tuple("10.6") <= version_tuple(v) < version_tuple("10.6.8")
            and build_tuple("10A432") <= build_tuple(b) <= build_tuple("10K1999"))


def products():
    """The catalog's combined 10.6.8 client update(s): the most recent first."""
    catalog = plistlib.loads(fetch(CATALOG))
    found = []
    for key, p in catalog.get("Products", {}).items():
        pkgs = [x for x in p.get("Packages", []) if COMBO.search(x.get("URL", ""))]
        if len(pkgs) < 2 or any("Server" in x.get("URL", "") for x in p.get("Packages", [])):
            continue
        found.append((p.get("PostDate"), key, p, pkgs))
    found.sort(key=lambda f: str(f[0]), reverse=True)
    return found


def describe(key, p, pkgs):
    meta = {}
    if p.get("ServerMetadataURL"):
        try:
            meta = plistlib.loads(fetch(p["ServerMetadataURL"]))
        except Exception:  # noqa: BLE001 - the catalog's own names then
            meta = {}
    english = meta.get("localization", {}).get("English", {})
    desc = english.get("description", b"")
    return {
        "key": key,
        "title": english.get("title", "Mac OS X Update Combined"),
        "version": meta.get("CFBundleShortVersionString", "10.6.8 v1.1"),
        "serverComment": english.get("serverComment", ""),
        "description": desc.decode("utf-8", "replace") if isinstance(desc, bytes) else desc,
        "size": sum(x.get("Size", 0) for x in pkgs),
        "restart": True,   # its first package's onConclusion="RequireRestart"
        "packages": [{"url": x["URL"], "size": x.get("Size", 0)} for x in pkgs],
    }


def read_system(assets):
    try:
        with open(assets / "System/Library/CoreServices/SystemVersion.plist", "rb") as f:
            return plistlib.load(f)
    except OSError:
        return {}


def scan(args):
    assets = assets_dir(args)
    system = read_system(assets)
    out = {"system": {k: system.get(k, "") for k in ("ProductVersion", "ProductBuildVersion")}, "products": []}
    if applies(system):
        found = products()
        if found:
            _, key, p, pkgs = found[0]
            item = describe(key, p, pkgs)
            item["downloaded"] = downloaded(cache_dir(args, key), item)
            out["products"].append(item)
    print(json.dumps(out))
    return 0


def downloaded(folder, item):
    return all((folder / Path(x["url"]).name).is_file() and
               (folder / Path(x["url"]).name).stat().st_size == x["size"] for x in item["packages"])


def product(key):
    for _, k, p, pkgs in products():
        if k == key:
            return describe(k, p, pkgs)
    sys.exit(f"{key}: not in Apple's catalog")


def download(args):
    key = args[0]
    item = product(key)
    folder = cache_dir(args, key)
    folder.mkdir(parents=True, exist_ok=True)
    total = item["size"]
    done = sum(min((folder / Path(x["url"]).name).stat().st_size, x["size"]) if (folder / Path(x["url"]).name).exists()
               else (folder / (Path(x["url"]).name + ".partial")).stat().st_size
               if (folder / (Path(x["url"]).name + ".partial")).exists() else 0 for x in item["packages"])
    print(f"PROGRESS {done} {total}", flush=True)
    for x in item["packages"]:
        name = Path(x["url"]).name
        final, part = folder / name, folder / (name + ".partial")
        if final.exists() and final.stat().st_size == x["size"]:
            continue
        print(f"ITEM {name}", flush=True)
        have = part.stat().st_size if part.exists() else 0
        req = urllib.request.Request(https(x["url"]), headers={"User-Agent": AGENT, **({"Range": f"bytes={have}-"} if have else {})})
        with urllib.request.urlopen(req, timeout=60) as r, open(part, "ab" if have and r.status == 206 else "wb") as f:
            if have and r.status != 206:
                done -= have
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                print(f"PROGRESS {done} {total}", flush=True)
        if part.stat().st_size != x["size"]:
            sys.exit(f"{name}: {part.stat().st_size} bytes, the catalog says {x['size']}")
        part.rename(final)
    (folder / "product.json").write_text(json.dumps(item))
    print("DONE", flush=True)
    return 0


def install(args):
    key = args[0]
    folder = cache_dir(args, key)
    info = json.loads((folder / "product.json").read_text())
    if not downloaded(folder, info):
        sys.exit(f"{key}: not downloaded (softwareupdate.py download {key})")
    pkgs = [str(folder / Path(x["url"]).name) for x in info["packages"]]
    got = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("extract.py")), "--update", *pkgs,
                          "--assets", str(assets_dir(args))])
    if got.returncode:
        return got.returncode
    for p in pkgs:
        Path(p).unlink(missing_ok=True)
    (folder / "product.json").unlink(missing_ok=True)
    try:
        folder.rmdir()
    except OSError:
        pass
    # Software Update's InstallHistory.plist: what was installed, and when
    history = assets_dir(args) / ".installed-updates.json"
    try:
        done = json.loads(history.read_text())
    except (OSError, ValueError):
        done = []
    import datetime
    done.append({"key": key, "title": info["title"], "version": info["version"],
                 "date": datetime.datetime.now().isoformat(timespec="seconds")})
    history.write_text(json.dumps(done, indent=1))
    print("DONE", flush=True)
    return 0


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if args else 2
    cmd, rest = args[0], args[1:]
    try:
        if cmd == "scan":
            return scan(rest)
        if cmd in ("download", "install") and rest:
            return (download if cmd == "download" else install)(rest)
    except (OSError, ValueError) as e:
        print(f"ERROR {e}", file=sys.stderr, flush=True)
        return 1
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
