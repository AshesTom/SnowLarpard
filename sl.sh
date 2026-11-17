#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Snow Leopard in a window of your current desktop, without touching anything on your system: its
# own home in .local/home/<you> (SL_HOME to choose another).
#
#   ./sl.sh /path/to/SnowLeopard.iso [WxH]    the first time: takes the DVD's files, builds, runs
#   ./sl.sh -- [WxH]                          later runs
#
# The window is a nested X server: Xephyr, or Xwayland's own window under a Wayland desktop.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")" && pwd)
size=1280x800
if [[ ${1:-} != -- && -n ${1:-} ]]; then
    python3 "$root/tools/extract.py" "$1"
fi
shift || true
[[ -n ${1:-} ]] && size=$1
[[ -e $root/.local/assets/.complete ]] || { echo "First run: ./sl.sh /path/to/SnowLeopard.iso" >&2; exit 1; }
"$root/shell/build.sh"

for n in $(seq 2 50); do [[ -e /tmp/.X11-unix/X$n ]] || { display=:$n; break; }; done
if command -v Xephyr >/dev/null; then
    Xephyr "$display" -screen "$size" -title "Snow Leopard ($display)" -no-host-grab -nolisten tcp &
else
    Xwayland "$display" -geometry "$size" -decorate &
fi
server=$!
trap 'kill $server 2>/dev/null' EXIT
for _ in $(seq 100); do [[ -e /tmp/.X11-unix/X${display#:} ]] && break; sleep 0.05; done
# The session's own home, a new 10.6 account's folders: what the session writes (the .DS_Store files,
# Stacks' PDFs in Documents and Downloads, its settings and caches) stays in it, your files out of it.
# SL_HOME=DIR for another one (SL_HOME=$HOME: yours).
home=${SL_HOME:-$root/.local/home/$(id -un)}
mkdir -p -- "$home"/{Desktop,Documents,Downloads,Library,Movies,Music,Pictures,Public,Sites}
# In a window: Restart and Shut Down don't touch the computer (SNOWLEOPARD_POWER unset).
env -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_CACHE_HOME -u XDG_STATE_HOME HOME="$home" \
    DISPLAY=$display WAYLAND_DISPLAY= "$root/session/snowleopard-session"
