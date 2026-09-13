#!/usr/bin/env bash
# Puts what pcsx2-gs.exe loads at run time next to it: the dependency DLLs,
# PCSX2's graphics library and the resources its hardware renderers read.
set -euo pipefail

usage="usage: stage.sh <directory of pcsx2-gs.exe> <pcsx2-hook build> <dependency prefix>"
bin=${1:?$usage}
hook=${2:?$usage}
deps=${3:?$usage}

cp -u "$deps"/bin/*.dll "$bin"/
cp -u "$hook"/pcsx2-hook/pcsx2hook.dll "$bin"/
source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$hook/CMakeCache.txt")
mkdir -p "$bin/resources"
cp -ru "$source/bin/resources/." "$bin/resources/"
