#!/usr/bin/env bash
# Build the iOS simulator (arm64) target: dosbox-x compiled and archived so
# an SDL2/UIKit app can link it into its binary (the app supplies main(),
# calls SDL_UIKitRunApp with dosbox-x's SDL_main; see the dosbox-x-ios app
# project). Mirrors build-wasm.sh / build-android.sh: built from a git
# worktree under build-ios/ (ignored), so the native build in the source
# tree keeps its own configuration.
#
#   ./build-ios.sh          # build into build-ios/
#
# Products (under build-ios/):
#   prefix/lib/libSDL2.a, libSDL2main.a, libSDL2_net.a, libpng.a
#   tree/src/dosbox.o and tree/src/**/lib*.a   - link with -force_load
#
# Needs Xcode (not just the command line tools); set DEVELOPER_DIR if
# xcode-select points elsewhere.
set -e
cd "$(dirname "$0")"
repo=$(pwd)

export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
: "${IOS_MIN:=13.0}"
: "${SDL2_VERSION:=2.32.10}"
: "${LIBPNG_VERSION:=1.6.50}"
: "${SDL2_NET_VERSION:=2.2.0}"

SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
TARGET="-target arm64-apple-ios${IOS_MIN}-simulator -isysroot $SDK"

BI=$repo/build-ios
PREFIX=$BI/prefix
mkdir -p "$BI"
cd "$BI"

cmake_ios() {
    cmake -S "$1" -B "$2" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_MIN \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        "${@:3}" > "$2.log" 2>&1
    ninja -C "$2" install >> "$2.log" 2>&1
}

# --- SDL2, libpng, SDL2_net (all static) ---------------------------------
if [ ! -f "$PREFIX/lib/libSDL2.a" ]; then
    [ -d SDL2-$SDL2_VERSION ] || { curl -sL https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz | tar xz; }
    cmake_ios SDL2-$SDL2_VERSION sdl2-build -DSDL_STATIC=ON -DSDL_SHARED=OFF
fi
if [ ! -f "$PREFIX/lib/libpng.a" ]; then
    [ -d libpng-$LIBPNG_VERSION ] || { curl -sL https://github.com/pnggroup/libpng/archive/refs/tags/v$LIBPNG_VERSION.tar.gz | tar xz; }
    cmake_ios libpng-$LIBPNG_VERSION libpng-build -DPNG_SHARED=OFF -DPNG_STATIC=ON \
        -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
fi
if [ ! -f "$PREFIX/lib/libSDL2_net.a" ]; then
    [ -d SDL2_net-$SDL2_NET_VERSION ] || { curl -sL https://github.com/libsdl-org/SDL_net/releases/download/release-$SDL2_NET_VERSION/SDL2_net-$SDL2_NET_VERSION.tar.gz | tar xz; }
    cmake_ios SDL2_net-$SDL2_NET_VERSION sdl2net-build -DBUILD_SHARED_LIBS=OFF -DSDL2NET_SAMPLES=OFF \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DSDL2_INCLUDE_DIR="$PREFIX/include/SDL2" -DSDL2_LIBRARY="$PREFIX/lib/libSDL2.a"
fi

# --- dosbox-x, from a worktree so the main tree keeps its own build ------
cd "$repo"
if [ ! -d build-ios/tree ]; then
    git worktree add --force --detach build-ios/tree HEAD
else
    git -C build-ios/tree checkout --detach -f HEAD
fi
cd build-ios/tree
[ -f configure ] || ./autogen.sh
top="$(pwd)"

if [ ! -f Makefile ]; then
    CC="$(xcrun -f clang) $TARGET" \
    CXX="$(xcrun -f clang++) $TARGET" \
    OBJC="$(xcrun -f clang) $TARGET" \
    OBJCXX="$(xcrun -f clang++) $TARGET" \
    AR="$(xcrun -f ar)" RANLIB="$(xcrun -f ranlib)" STRIP="$(xcrun -f strip)" \
    CFLAGS="-O2 -I$PREFIX/include" CXXFLAGS="-O2 -I$PREFIX/include -DC_IOS=1" \
    OBJCXXFLAGS="-O2" CPPFLAGS="-DC_IOS=1" \
    LDFLAGS="-L$PREFIX/lib" LIBS="-lz -lm" \
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
    SDL2_CONFIG="$PREFIX/bin/sdl2-config" SDL_CONFIG=/nonexistent \
    "${top}/configure" --host=aarch64-apple-ios --enable-dynrec \
        --disable-dynamic-x86 --disable-fpu-x86 --enable-sdl2 \
        --with-sdl-prefix="$PREFIX" --disable-screenshots --disable-opengl \
        --disable-mt32 --enable-force-menu-sdldraw --disable-x11 \
        --disable-avcodec --disable-libslirp --disable-libfluidsynth \
        --disable-freetype --disable-gamelink
fi

# The final executable link fails by design: main() is SDL_main and the
# UIKit frameworks are supplied by the app project's link.
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" || true

[ -f src/dosbox.o ] || { echo "build failed; see make output" >&2; exit 1; }
echo "Built. Link the app against:"
echo "  $top/src/dosbox.o + src/**/lib*.a (with -force_load)"
echo "  $PREFIX/lib/{libSDL2,libSDL2main,libSDL2_net,libpng}.a"
