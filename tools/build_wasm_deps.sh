#!/bin/sh
# This file is part of tagplay.  GPL-3.0-or-later; see COPYING.
#
# Build the WASM static libraries tagplay-gui needs: pcre2 (the query
# language) and libFLAC. Requires emscripten (apt install emscripten
# on Ubuntu 24). Results land in third_party/wasm/{lib,include}.
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD/third_party/wasm"
mkdir -p "$ROOT/src" "$ROOT/lib" "$ROOT/include"

# ---- one toolchain, never a mix -----------------------------------
# An emsdk install is self-sufficient (bundled LLVM, writable cache):
# it must NOT get our private config, which exists only to unfreeze
# Debian's apt emscripten. Mixing a new emsdk frontend with the
# system clang produced exactly the '-fwrapv-pointer' failure once.
EMCC_PATH=$(command -v emcc) || {
    echo "build_wasm_deps: emcc not found (apt install emscripten,"
    echo "or source your emsdk env)"; exit 1; }
CUR_TC="$EMCC_PATH $(emcc --version 2>/dev/null | head -1)"
STAMP="$ROOT/toolchain.stamp"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" != "$CUR_TC" ]; then
    echo "build_wasm_deps: toolchain changed -- rebuilding deps clean"
    rm -rf "$ROOT/lib" "$ROOT/include" "$ROOT/emcache"            "$ROOT/emconfig"
    mkdir -p "$ROOT/lib" "$ROOT/include"
fi

DEBIAN_EM=0
if [ "$EMCC_PATH" = "/usr/bin/emcc" ] &&    grep -qs "FROZEN_CACHE = True"         /usr/share/emscripten/.emscripten 2>/dev/null; then
    DEBIAN_EM=1
else
    rm -f "$ROOT/emconfig"      # emsdk manages its own cache/config
fi

cd "$ROOT/src"

if [ ! -f "$ROOT/lib/libpcre2-8.a" ]; then
    [ -f pcre2-10.42.tar.gz ] || \
        curl -sLO https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.42/pcre2-10.42.tar.gz
    rm -rf pcre2-10.42 && tar xzf pcre2-10.42.tar.gz
    cd pcre2-10.42
    emconfigure ./configure --host=wasm32-unknown-emscripten --disable-shared --enable-static \
        --disable-pcre2grep-libz --disable-pcre2grep-libbz2 \
        >/dev/null
    emmake make -j"$(nproc)" libpcre2-8.la >/dev/null
    cp .libs/libpcre2-8.a "$ROOT/lib/"
    cp src/pcre2.h "$ROOT/include/"
    cd ..
fi

if [ ! -f "$ROOT/lib/libFLAC.a" ]; then
    [ -f flac-1.4.3.tar.xz ] || \
        curl -sLO https://github.com/xiph/flac/releases/download/1.4.3/flac-1.4.3.tar.xz
    rm -rf flac-1.4.3 && tar xJf flac-1.4.3.tar.xz
    cd flac-1.4.3
    emconfigure ./configure --host=wasm32-unknown-emscripten --disable-shared --enable-static \
        --disable-ogg --disable-programs --disable-examples \
        --disable-cpplibs --disable-doxygen-docs >/dev/null
    emmake make -j"$(nproc)" -C src/libFLAC >/dev/null
    cp src/libFLAC/.libs/libFLAC.a "$ROOT/lib/" 2>/dev/null || \
        cp src/libFLAC/.libs/libFLAC-static.a "$ROOT/lib/libFLAC.a"
    cp -r include/FLAC "$ROOT/include/"
    cd ..
fi
# Debian's apt emscripten freezes its system cache: derive a private,
# writable config + cache copy so ports (SDL2) can install, while the
# prebuilt sysroot is reused. emsdk installs skip all of this.
if [ "$DEBIAN_EM" = 1 ]; then
    if [ ! -f "$ROOT/emconfig" ]; then
        cp /usr/share/emscripten/.emscripten "$ROOT/emconfig"
        printf "\nCACHE = '%s/emcache'\nFROZEN_CACHE = False\n" \
            "$ROOT" >> "$ROOT/emconfig"
    fi
    if [ ! -d "$ROOT/emcache/sysroot" ] && \
       [ -d /usr/share/emscripten/cache ]; then
        cp -r /usr/share/emscripten/cache "$ROOT/emcache"
        chmod -R u+w "$ROOT/emcache"
    fi
fi

printf '%s' "$CUR_TC" > "$STAMP"
echo "wasm deps ready: $(ls "$ROOT/lib")"
