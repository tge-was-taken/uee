#!/bin/sh
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build "$@"
cmake --build build -j"$(nproc)"
