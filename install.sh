#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Installs Snow Leopard as a session of your login screen (SDDM, GDM, Plasma Login, LightDM...).
#
#   ./install.sh                 builds the shell, installs it in /usr/local (asks for sudo)
#   ./install.sh --prefix DIR    elsewhere than /usr/local
#   ./install.sh --uninstall     takes it all away again
#   ./install.sh --destdir DIR   installs under DIR (packaging, tests): nothing outside it
#
# It needs the DVD's files first: ./sl.sh /path/to/SnowLeopard.iso (or tools/extract.py). They
# are copied with the programs, readable by every user of the computer.
#
#   PREFIX/lib/snowleopard/apps        the programs (WindowServer, Finder.app, Dock.app...)
#   PREFIX/lib/snowleopard/assets      the DVD's files
#   PREFIX/lib/snowleopard/session     snowleopard-session (X), snowleopard-wayland
#   PREFIX/bin/snowleopard-session     to run it from ~/.xinitrc or another X session
#   /usr/share/wayland-sessions/snowleopard.desktop   the login screen's entry: KWin (or cage) alone,
#                                      a full-screen Xwayland, the session in it (no Xorg needed)
#   /usr/share/xsessions/snowleopard.desktop          with Xorg: the session on its own X server
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")" && pwd)
prefix=/usr/local
destdir=
uninstall=0
build=1
while (($#)); do
    case $1 in
    --prefix) prefix=$2; shift ;;
    --destdir) destdir=$2; shift ;;
    --uninstall) uninstall=1 ;;
    --no-build) build=0 ;;
    -h | --help) sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "install.sh: unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
prefix=$(realpath -m -- "$prefix")
lib=$destdir$prefix/lib/snowleopard
bin=$destdir$prefix/bin
share=$destdir/usr/share
manifest=$lib/installed-files

say() { printf '==> %s\n' "$*"; }

# what is written outside the user's own folders needs root: the rest of the script again, by sudo
need_root() {
    [[ -z $destdir && $EUID -ne 0 ]]
}
again_as_root() {
    local args=(--prefix "$prefix" --no-build)
    ((uninstall)) && args+=(--uninstall)
    say "Installing needs root: sudo $0 ${args[*]}"
    exec sudo -- "$0" "${args[@]}"
}

if ((uninstall)); then
    need_root && again_as_root
    if [[ -f $manifest ]]; then
        # the files, then the folders left empty
        while IFS= read -r f; do rm -f -- "$destdir$f"; done <"$manifest"
    fi
    rm -rf -- "$lib"
    rm -f -- "$bin/snowleopard-session" "$share/wayland-sessions/snowleopard.desktop" "$share/xsessions/snowleopard.desktop"
    say "Snow Leopard is uninstalled (your settings in ~/.config/snowleopard stay)"
    exit 0
fi

assets=$root/.local/assets
apps=$root/.local/build/apps
[[ -e $assets/.complete ]] || {
    echo "install.sh: no DVD files in $assets: run ./sl.sh /path/to/SnowLeopard.iso first (or tools/extract.py)" >&2
    exit 1
}

if ((build)); then
    if [[ $EUID -eq 0 && -n ${SUDO_USER:-} ]]; then
        say "Building as $SUDO_USER"
        sudo -u "$SUDO_USER" -- "$root/shell/build.sh"
    else
        say "Building"
        "$root/shell/build.sh"
    fi
fi
[[ -x $apps/WindowServer && -x $apps/Dock.app/Contents/MacOS/Dock ]] || { echo "install.sh: the build is not there ($apps)" >&2; exit 1; }

need_root && again_as_root

compositor=
command -v kwin_wayland >/dev/null && compositor=kwin_wayland
[[ -z $compositor ]] && command -v cage >/dev/null && compositor=cage
xorg=0
command -v Xorg >/dev/null && xorg=1
command -v Xwayland >/dev/null || compositor=
if [[ -z $compositor && $xorg -eq 0 ]]; then
    echo "install.sh: neither Xorg nor a Wayland compositor with Xwayland (kwin_wayland, cage): the login screen couldn't start it" >&2
    exit 1
fi

say "Installing in $prefix/lib/snowleopard"
rm -rf -- "$lib"
mkdir -p -- "$lib/session" "$bin"
cp -a -- "$apps" "$lib/apps"
cp -a -- "$assets" "$lib/assets"
install -m 755 -- "$root/session/snowleopard-session" "$root/session/snowleopard-wayland" "$lib/session/"
# every user's: the DVD's files keep their 0600 otherwise
chmod -R u+rwX,go+rX,go-w -- "$lib"
[[ $EUID -eq 0 ]] && chown -R root:root -- "$lib"
ln -sfn -- "$prefix/lib/snowleopard/session/snowleopard-session" "$bin/snowleopard-session"

: >"$manifest"
entry() {   # the login screen's .desktop entry
    local dir=$1 exec=$2 comment=$3
    mkdir -p -- "$dir"
    cat >"$dir/snowleopard.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Snow Leopard
Comment=$comment
Exec=$exec
TryExec=$exec
DesktopNames=SnowLeopard
EOF
    chmod 644 -- "$dir/snowleopard.desktop"
    echo "${dir#"$destdir"}/snowleopard.desktop" >>"$manifest"
    say "Login screen entry: ${dir#"$destdir"}/snowleopard.desktop"
}
if [[ -n $compositor ]]; then
    entry "$share/wayland-sessions" "$prefix/lib/snowleopard/session/snowleopard-wayland" \
        "Mac OS X 10.6's desktop: $compositor, a full-screen Xwayland, the Snow Leopard session"
fi
if ((xorg)); then
    entry "$share/xsessions" "$prefix/lib/snowleopard/session/snowleopard-session" "Mac OS X 10.6's desktop on its own X server"
fi
echo "${bin#"$destdir"}/snowleopard-session" >>"$manifest"
say "Done: log out and pick Snow Leopard on the login screen (./install.sh --uninstall takes it away)"
