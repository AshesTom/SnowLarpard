#!/bin/sh
# Builds the shell into .local/build (the programs in .local/build/apps/<Name>.app/Contents/MacOS).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cmake -S "$root/shell" -B "$root/.local/build" -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo} >/dev/null
cmake --build "$root/.local/build" -j"$(nproc)"
