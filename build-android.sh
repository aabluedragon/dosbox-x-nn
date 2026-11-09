#!/usr/bin/env bash
# Build the Android (arm64) target: dosbox-x compiled as libmain.so for an
# SDL2-based Android app (SDLActivity loads libSDL2.so + libmain.so and
# calls SDL_main). Mirrors build-wasm.sh: the target is built from a git
# worktree of this same repository under build-android/ (ignored), so the
# native build in the source tree keeps its own configuration.
#
#   ./build-android.sh          # build into build-android/
#
# Products (under build-android/):
#   prefix/lib/libSDL2.so       - ship in the APK (lib/arm64-v8a/)
#   tree/src/libmain.so         - ship in the APK (lib/arm64-v8a/)
# android/build.sh then packages them into out/dosbox-x.apk.
# The SDL2 Java glue the app needs is under
#   SDL2-*/android-project/app/src/main/java/org/libsdl/app/
#
# Needs the Android NDK; set ANDROID_SDK or ANDROID_NDK to override the
# default SDK location (macOS ~/Library/Android/sdk, Linux ~/Android/Sdk).
# API 28+ is required (bionic gained iconv in API 28).
set -e
cd "$(dirname "$0")"
repo=$(pwd)

if [ -z "$ANDROID_SDK" ]; then
    for d in "$ANDROID_HOME" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
        [ -n "$d" ] && [ -d "$d" ] && { ANDROID_SDK=$d; break; }
    done
fi
: "${ANDROID_NDK:=$(ls -d "$ANDROID_SDK"/ndk/* 2>/dev/null | sort -V | tail -1)}"
: "${ANDROID_API:=28}"
: "${SDL2_VERSION:=2.32.10}"
: "${LIBPNG_VERSION:=1.6.50}"
: "${SDL2_NET_VERSION:=2.2.0}"

[ -n "$ANDROID_NDK" ] || { echo "Android NDK not found; set ANDROID_NDK." >&2; exit 1; }
TC=$(ls -d "$ANDROID_NDK"/toolchains/llvm/prebuilt/* | head -1)
CC="$TC/bin/aarch64-linux-android${ANDROID_API}-clang"
CXX="$TC/bin/aarch64-linux-android${ANDROID_API}-clang++"
[ -x "$CC" ] || { echo "NDK clang not found at $CC" >&2; exit 1; }

BA=$repo/build-android
PREFIX=$BA/prefix
mkdir -p "$BA"
cd "$BA"

cmake_android() {
    cmake -S "$1" -B "$2" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$ANDROID_API \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" "${@:3}" > "$2.log" 2>&1
    ninja -C "$2" install >> "$2.log" 2>&1
}

# --- SDL2 (shared), libpng + SDL2_net (static) --------------------------
if [ ! -f "$PREFIX/lib/libSDL2.so" ]; then
    [ -d SDL2-$SDL2_VERSION ] || { curl -sL https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz | tar xz; }
    cmake_android SDL2-$SDL2_VERSION sdl2-build -DSDL_STATIC=OFF -DSDL_SHARED=ON
    # Android's bionic has no librt; keep it out of the link probes.
    sed -i '' 's/ -lrt//g' "$PREFIX/bin/sdl2-config" "$PREFIX/lib/pkgconfig/sdl2.pc" 2>/dev/null || \
        sed -i 's/ -lrt//g' "$PREFIX/bin/sdl2-config" "$PREFIX/lib/pkgconfig/sdl2.pc"
fi
if [ ! -f "$PREFIX/lib/libpng.a" ]; then
    [ -d libpng-$LIBPNG_VERSION ] || { curl -sL https://github.com/pnggroup/libpng/archive/refs/tags/v$LIBPNG_VERSION.tar.gz | tar xz; }
    cmake_android libpng-$LIBPNG_VERSION libpng-build -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
fi
if [ ! -f "$PREFIX/lib/libSDL2_net.a" ]; then
    [ -d SDL2_net-$SDL2_NET_VERSION ] || { curl -sL https://github.com/libsdl-org/SDL_net/releases/download/release-$SDL2_NET_VERSION/SDL2_net-$SDL2_NET_VERSION.tar.gz | tar xz; }
    cmake_android SDL2_net-$SDL2_NET_VERSION sdl2net-build -DBUILD_SHARED_LIBS=OFF -DSDL2NET_SAMPLES=OFF \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DSDL2_INCLUDE_DIR="$PREFIX/include/SDL2" -DSDL2_LIBRARY="$PREFIX/lib/libSDL2.so"
fi

# --- dosbox-x, from a worktree so the main tree keeps its own build ------
cd "$repo"
if [ ! -d build-android/tree ]; then
    git worktree add --force --detach build-android/tree HEAD
else
    git -C build-android/tree checkout --detach -f HEAD
fi
cd build-android/tree
[ -f configure ] || ./autogen.sh
top="$(pwd)"

if [ ! -f Makefile ]; then
    CC="$CC" CXX="$CXX" \
    AR="$TC/bin/llvm-ar" RANLIB="$TC/bin/llvm-ranlib" STRIP="$TC/bin/llvm-strip" \
    CFLAGS="-O2 -fPIC -I$PREFIX/include" CXXFLAGS="-O2 -fPIC -I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib" LIBS="-lz -lm" \
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
    SDL2_CONFIG="$PREFIX/bin/sdl2-config" SDL_CONFIG=/nonexistent \
    PATH="$PREFIX/bin:$PATH" \
    "${top}/configure" --host=aarch64-linux-android --enable-dynrec \
        --disable-dynamic-x86 --disable-fpu-x86 --enable-sdl2 \
        --with-sdl-prefix="$PREFIX" --disable-screenshots --disable-opengl \
        --disable-mt32 --enable-force-menu-sdldraw --disable-x11 \
        --disable-avcodec --disable-libslirp --disable-libfluidsynth \
        --disable-freetype --disable-gamelink
fi

make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" || true  # final exe link fails: main is SDL_main

# Link the emulator as libmain.so for SDLActivity. --whole-archive keeps
# SDL_main (and every registered device/module) in the shared object.
cd src
"$CXX" -shared -O2 -fPIC -o libmain.so \
  dosbox.o -Wl,--whole-archive \
  debug/libdebug.a dos/libdos.a shell/libshell.a builtin/libbuiltin.a \
  ints/libints.a hardware/serialport/libserial.a hardware/parport/libparallel.a \
  libs/passthroughio/libpassthroughio.a gui/libgui.a libs/gui_tk/libgui_tk.a \
  hardware/libhardware.a cpu/libcpu.a hardware/reSID/libresid.a fpu/libfpu.a \
  misc/libmisc.a output/liboutput.a hardware/mame/libmame.a libs/zmbv/libzmbv.a \
  libs/decoders/internal/libopusint.a aviwriter/libaviwriter.a libs/xBRZ/libxbrz.a \
  libs/fluidsynth/libfluidsynth.a -Wl,--no-whole-archive \
  -L"$PREFIX/lib" -lSDL2 -lSDL2_net -lpng -lz -lm -latomic -llog -landroid \
  -static-libstdc++ -Wl,--no-undefined

echo "Built:"
echo "  $PREFIX/lib/libSDL2.so"
echo "  $top/src/libmain.so"
