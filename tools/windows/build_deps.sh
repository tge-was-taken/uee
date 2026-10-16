#!/usr/bin/env bash
# Cross-builds the libraries pcsx2-gs and PCSX2's graphics library need on
# Windows, at the versions and options PCSX2's own Windows script uses.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
prefix=${1:?usage: build_deps.sh <install prefix> [download directory]}
downloads=${2:-$prefix-src}
mkdir -p "$prefix" "$downloads"
prefix=$(cd "$prefix" && pwd)
downloads=$(cd "$downloads" && pwd)

fetch() {
    local file=$1 url=$2 sha256=$3 directory=$4
    if [ ! -f "$downloads/$file" ]; then
        curl -fL -o "$downloads/$file.part" "$url"
        mv "$downloads/$file.part" "$downloads/$file"
    fi
    echo "$sha256  $downloads/$file" | sha256sum -c --quiet
    if [ ! -d "$downloads/$directory" ]; then
        case $file in
            *.zip) python3 -m zipfile -e "$downloads/$file" "$downloads" ;;
            *.tar.*) tar -xf "$downloads/$file" -C "$downloads" ;;
        esac
    fi
}

build() {
    local source=$downloads/$1
    shift
    cmake -S "$source" -B "$source/build-windows" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$here/clang-cl.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_FIND_ROOT_PATH="$prefix" -DBUILD_SHARED_LIBS=ON "$@"
    cmake --build "$source/build-windows" --parallel
    cmake --install "$source/build-windows"
}

fetch zlib132.zip https://github.com/madler/zlib/releases/download/v1.3.2/zlib132.zip \
    e8bf55f3017aa181690990cb58a994e77885da140609fc8f94abe9b65d2cae28 zlib-1.3.2
build zlib-1.3.2 -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_STATIC=OFF

fetch lpng1658.zip https://download.sourceforge.net/libpng/lpng1658.zip \
    b32f170855dbbe3e6d9e645af40b538137041773672c3ba3e02db5816c82d376 lpng1658
# Its install asks whether the host is Windows where the build asked the target.
build lpng1658 -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF \
    -DSKIP_INSTALL_FILES=ON

fetch libjpeg-turbo-3.2.0.tar.gz \
    https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz \
    6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e libjpeg-turbo-3.2.0
build libjpeg-turbo-3.2.0 -DENABLE_STATIC=OFF -DWITH_CRT_DLL=ON

fetch lz4-1.10.0.zip https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.zip \
    3224b4c80f351f194984526ef396f6079bd6332dd9825c72ac0d7a37b3cdc565 lz4-1.10.0
build lz4-1.10.0/build/cmake -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF

fetch freetype-2.14.3.tar.gz \
    https://sourceforge.net/projects/freetype/files/freetype2/2.14.3/freetype-2.14.3.tar.gz/download \
    e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1 freetype-2.14.3
build freetype-2.14.3 -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE \
    -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE

fetch zstd-1.5.7.zip https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.zip \
    7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4 zstd-1.5.7
build zstd-1.5.7/build/cmake -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF \
    -DZSTD_BUILD_PROGRAMS=OFF

fetch libwebp-1.6.0.tar.gz \
    https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz \
    e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564 libwebp-1.6.0
build libwebp-1.6.0 -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
    -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF

# Unlike PCSX2's, this keeps video: the frontend opens its window through it.
fetch SDL3-3.4.12.zip https://libsdl.org/release/SDL3-3.4.12.zip \
    3d4de8967a49c0451e775a0c1e9022092c19fdef41ba38a83fcf031c5a6496e2 SDL3-3.4.12
build SDL3-3.4.12 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF

fetch plutovg-1.3.2.zip https://github.com/sammycage/plutovg/archive/v1.3.2.zip \
    4fe4e48f28aa80171b2166d45c0976ab0f21eecedb52cd4c3ef73b5afb48fac9 plutovg-1.3.2
build plutovg-1.3.2 -DPLUTOVG_BUILD_EXAMPLES=OFF

fetch plutosvg-0.0.7.zip https://github.com/sammycage/plutosvg/archive/v0.0.7.zip \
    82dee2c57ad712bdd6d6d81d3e76249d89caa4b5a4214353660fd5adff12201a plutosvg-0.0.7
build plutosvg-0.0.7 -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF

fetch rapidyaml-0.12.1-src.zip \
    https://github.com/biojppm/rapidyaml/releases/download/v0.12.1/rapidyaml-0.12.1-src.zip \
    96276f55b9fa7837ac8f3f72fd52965879cbb5d5d2e6af548c69a177fb078304 rapidyaml-0.12.1-src
build rapidyaml-0.12.1-src

fetch DirectX-Headers-1.619.1.zip https://github.com/microsoft/DirectX-Headers/archive/v1.619.1.zip \
    9eb8b102a90a42e4ea72a825f7d249d55ec90d164f030966c9b7784b93374927 DirectX-Headers-1.619.1
build DirectX-Headers-1.619.1 -DDXHEADERS_BUILD_TEST=OFF -DDXHEADERS_BUILD_GOOGLE_TEST=OFF

# Headers the P3FES port's renderer takes from the system on Linux.
fetch glm-1.0.1.zip https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip \
    09c5716296787e1f7fcb87b1cbdbf26814ec1288ed6259ccd30d5d9795809fa5 glm-1.0.1
mkdir -p "$prefix/include"
cp -r "$downloads/glm-1.0.1/glm" "$prefix/include/"

fetch khrplatform.h \
    https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/5961a7fe64cf8a126890ced6f13d69e0a1e1b83e/api/KHR/khrplatform.h \
    7b1e01aaa7ad8f6fc34b5c7bdf79ebf5189bb09e2c4d2e79fc5d350623d11e83 khrplatform.h
mkdir -p "$prefix/include/KHR"
cp "$downloads/khrplatform.h" "$prefix/include/KHR/"
