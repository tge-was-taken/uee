#!/bin/sh
# Run P3FES with the native renderer. SKY2HOOK_DIR must name the P3FES PC port
# working copy the renderer is compiled from. The disc is yours: pass an image or
# an extracted directory as the first argument, or set P3FES_DISC. Anything else
# passes through to the binary (-surfaceless for a headless run).
set -e
cd "$(dirname "$0")/../.."

elf=scratch/games/p3fes/SLUS_216.21
card=${P3FES_CARD:-scratch/games/p3fes/card.mc2}
disc=$P3FES_DISC
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    disc=$1
    shift
fi

if [ -z "$SKY2HOOK_DIR" ]; then
    echo "run.sh: set SKY2HOOK_DIR to the P3FES PC port working copy" >&2
    exit 2
fi
if [ -z "$disc" ]; then
    echo "usage: run.sh <disc> [args...], or set P3FES_DISC" >&2
    exit 2
fi
if [ ! -f "$elf" ]; then
    echo "run.sh: no boot ELF at $elf; extract it from the disc first" >&2
    exit 2
fi

# Under WSL, Mesa reaches the host GPU through its d3d12 driver; without this GL
# falls back to llvmpipe, which is several times slower. Both variables are
# needed. Set GALLIUM_DRIVER yourself to opt out.
if [ -z "$GALLIUM_DRIVER" ] && [ -e /dev/dxg ] && [ -e /usr/lib/wsl/lib/libd3d12.so ]; then
    export GALLIUM_DRIVER=d3d12 MESA_LOADER_DRIVER_OVERRIDE=d3d12
    export LD_LIBRARY_PATH=/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
fi

# Reconfigure: the target only exists for the SKY2HOOK_DIR in the environment.
cmake -S . -B build >/dev/null
cmake --build build --target p3fes -j"$(nproc)" >/dev/null
exec ./build/bin/p3fes "$elf" "$disc" tests/patches/p3fes.toml "$card" "$@"
