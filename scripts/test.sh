#!/bin/sh
set -e
cd "$(dirname "$0")/.."
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure "$@"
