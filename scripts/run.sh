#!/bin/sh
set -e
cd "$(dirname "$0")/.."
if [ $# -lt 1 ]; then
    echo "usage: run.sh <elf> [disc.iso] [patches.toml]" >&2
    exit 2
fi
cmake --build build --target run_guest -j"$(nproc)" >/dev/null
exec ./build/bin/run_guest "$@"
