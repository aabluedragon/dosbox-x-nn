#!/bin/sh
set -eu
# NukemNet's DOSBox-X fork (github.com/aabluedragon/dosbox-x-nn), which carries the
# full-size IPX and guest turbo changes. RELEASE names the packages.
COMMIT=fa5f1c0539ba1e42733dc1f29bfda75231389559
RELEASE=20261109
TARGET_ARCH=${TARGET_ARCH:-armv7l}
export TARGET_ARCH
case "$TARGET_ARCH" in
    x64) TARGET_TRIPLE=x86_64-linux-gnu; CPU_FLAGS= ;;
    arm64) TARGET_TRIPLE=aarch64-linux-gnu; CPU_FLAGS= ;;
    armv7l) TARGET_TRIPLE=arm-linux-gnueabihf; CPU_FLAGS='-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard' ;;
    *) echo "Unsupported architecture: $TARGET_ARCH" >&2; exit 1 ;;
esac
mkdir -p /build/src /build/sdl /build/output
curl -fL --retry 3 "https://github.com/aabluedragon/dosbox-x-nn/archive/$COMMIT.tar.gz" -o /build/source.tar.gz
printf '%s\n' '1ec5c133a0f18610d955ae94f86eea4dae431f32fb6a915e9b848d37c617aa93  /build/source.tar.gz' | sha256sum -c -
tar -xf /build/source.tar.gz -C /build/src --strip-components=1
export CC=$TARGET_TRIPLE-gcc CXX=$TARGET_TRIPLE-g++
export CFLAGS="-O2 $CPU_FLAGS"
export CXXFLAGS="$CFLAGS"
export PKG_CONFIG_LIBDIR=/usr/lib/$TARGET_TRIPLE/pkgconfig:/usr/share/pkgconfig
cd /build/sdl
/build/src/vs/sdl2/configure --host=$TARGET_TRIPLE --prefix=/opt/nn-dosbox \
    --disable-static --disable-video-wayland --disable-video-vulkan
make -j4
make install
cd /build/src
./autogen.sh
# DOSBox-X includes some system headers before config.h. Set this early so ARM32
# uses 64-bit directory entries/stat structures, including on NFS and large disks.
export CPPFLAGS=-D_FILE_OFFSET_BITS=64
export SDL2_CONFIG=/opt/nn-dosbox/bin/sdl2-config
export PKG_CONFIG_LIBDIR=/opt/nn-dosbox/lib/pkgconfig:$PKG_CONFIG_LIBDIR
export LDFLAGS=-L/opt/nn-dosbox/lib
./configure --host=$TARGET_TRIPLE --enable-sdl2 --disable-debug --disable-avcodec --disable-fluidsynth
make -j4
python3 /recipe/bundle.py
cp /build/source.tar.gz "/build/output/dosbox-x-nn-$RELEASE-source.tar.gz"
sh /recipe/collect-sources.sh
(cd /recipe && zip -r "/build/output/dosbox-x-nn-linux-$TARGET_ARCH-build-$RELEASE.zip" .)
cd /build/output
sha256sum *.zip *.tar.gz > "dosbox-x-nn-linux-$TARGET_ARCH-$RELEASE.sha256"
