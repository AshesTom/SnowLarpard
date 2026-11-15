#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Snow Leopard in a window of your current desktop, without touching anything on your system.
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
# In a window: Restart and Shut Down don't touch the computer (SNOWLEOPARD_POWER unset).
DISPLAY=$display WAYLAND_DISPLAY= "$root/session/snowleopard-session"
