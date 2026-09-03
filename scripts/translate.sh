#!/bin/sh
set -e
cd "$(dirname "$0")/.."
if [ $# -lt 2 ]; then
    echo "usage: translate.sh <elf> <out.cpp>" >&2
    exit 2
fi
cmake --build build --target translate_elf -j"$(nproc)" >/dev/null
exec ./build/bin/translate_elf "$@"
