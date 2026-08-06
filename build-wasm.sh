#!/usr/bin/env bash
# Build the WebAssembly target out of tree, so the native build in this same
# checkout keeps its own configuration and object files.
#
#   ./build-wasm.sh            # build into build-wasm/ and deploy into web/
#
# Needs emsdk (tested with 6.0.5) sourced, or EMSDK_DIR pointing at one.
set -e
cd "$(dirname "$0")"
repo=$(pwd)

if [ -z "$EMSDK" ]; then
    : "${EMSDK_DIR:=$HOME/dev/emsdk}"
    # shellcheck disable=SC1091
    source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1 || {
        echo "Set up emsdk first, or point EMSDK_DIR at it." >&2; exit 1; }
fi
export PATH="${EMSDK}/upstream/emscripten/system/bin:${PATH}"
# keep the host's pkg-config out of the probe or it finds the native SDL2
export PKG_CONFIG_LIBDIR=/nonexistent

embuilder build zlib libpng sdl2 sdl2_net >/dev/null

# autotools refuses a VPATH build while the source tree itself is configured
# (the native build lives there), so the wasm target is built from a git
# worktree of this same repository: one repo, one history, separate build
# state. build-wasm/ is ignored.
if [ ! -d build-wasm/tree ]; then
    mkdir -p build-wasm
    git worktree add --force --detach build-wasm/tree HEAD
else
    git -C build-wasm/tree checkout --detach -f HEAD
fi
cd build-wasm/tree
[ -f configure ] || ./autogen.sh
top="$(pwd)"

FLAGS="-flto -msimd128 -DEMSCRIPTEN=1 -s USE_ZLIB=1 -s USE_LIBPNG=1 -s USE_SDL=2 -s USE_SDL_NET=2"
if [ ! -f Makefile ]; then
    CC=emcc CXX=em++ LD=emcc LD_CXX=em++ AR=emar RANLIB=emranlib \
    CFLAGS="$FLAGS" CXXFLAGS="$FLAGS" \
    LDFLAGS="-O3 -flto -msimd128 --profiling-funcs -sALLOW_TABLE_GROWTH $FLAGS \
             -s FORCE_FILESYSTEM=1 -s INITIAL_MEMORY=536870912 -s JSPI \
             -s ERROR_ON_UNDEFINED_SYMBOLS=0" \
    "${top}/configure" --host=x86_64-linux --enable-dynrec --disable-dynamic-x86 \
        --disable-fpu-x86 --enable-sdl2 \
        --with-sdl-prefix="${EMSDK}/upstream/emscripten/system" --disable-screenshots \
        --disable-opengl --disable-mt32 --enable-emscripten --enable-force-menu-sdldraw \
        --disable-x11 --disable-avcodec --disable-libslirp --disable-libfluidsynth \
        --disable-freetype --disable-gamelink
fi

make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
cp src/dosbox-x "${repo}/web/dosbox-x.js"
cp src/dosbox-x.wasm "${repo}/web/"
echo "Deployed to web/. Serve it with web/serve.sh"
