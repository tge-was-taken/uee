#!/usr/bin/env bash
# Makes a directory p3fes.exe can be played from: the executable and the DLLs
# it loads, the port's shaders and per-serial addresses, and this runtime's
# patches under the serial they are for. A disc image dropped beside it boots.
set -euo pipefail

usage="usage: stage_p3fes.sh <output directory> <Windows build> <port working copy> <dependency prefix>"
out=${1:?$usage}
build=${2:?$usage}
port=${3:?$usage}
deps=${4:?$usage}
here=$(cd "$(dirname "$0")" && pwd)
patches=$here/../../tests/patches

mkdir -p "$out/patches"
cp -u "$build/bin/p3fes.exe" "$deps/bin/SDL3.dll" "$deps/bin/z.dll" "$out/"
cp -ru "$port/src/runtime/renderware/sky2/shaders" "$port/config" "$out/"
cp -u "$patches/p3fes.toml" "$out/patches/SLUS-21621.toml"
