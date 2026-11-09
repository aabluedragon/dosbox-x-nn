#!/bin/sh
# Builds the DOSBox-X iOS simulator app: dosbox-x static archives + SDL2
# linked into one binary with a small UIKit shim (app/main.m), bundled with
# the DOS environment (app/dos-assets, e.g. Duke3D) and signed ad hoc.
#
# Run ../build-ios.sh first: it produces the dosbox-x archives and static
# SDL2/libpng/SDL2_net this script links against (under ../build-ios/).
# Then ./run.sh installs and launches the bundle on the booted simulator.
set -e
cd "$(dirname "$0")"
repo=$(cd .. && pwd)
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
: "${IOS_MIN:=13.0}"
SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
TARGET="-target arm64-apple-ios${IOS_MIN}-simulator -isysroot $SDK"
TREE="${TREE:-$repo/build-ios/tree/src}"
PREFIX="${PREFIX:-$repo/build-ios/prefix}"
APP=out/DOSBox-X.app

[ -f "$TREE/dosbox.o" ] || { echo "dosbox-x not built; run ../build-ios.sh first" >&2; exit 1; }

CLANG="$(xcrun -f clang)"

rm -rf out && mkdir -p out
$CLANG $TARGET -fobjc-arc -O2 \
    -I$PREFIX/include -I$PREFIX/include/SDL2 \
    -c app/main.m -o out/main.o

FORCE=""
for a in \
    debug/libdebug.a dos/libdos.a shell/libshell.a builtin/libbuiltin.a \
    ints/libints.a hardware/serialport/libserial.a hardware/parport/libparallel.a \
    libs/passthroughio/libpassthroughio.a gui/libgui.a libs/gui_tk/libgui_tk.a \
    hardware/libhardware.a cpu/libcpu.a hardware/reSID/libresid.a fpu/libfpu.a \
    misc/libmisc.a output/liboutput.a hardware/mame/libmame.a libs/zmbv/libzmbv.a \
    libs/decoders/internal/libopusint.a aviwriter/libaviwriter.a libs/xBRZ/libxbrz.a \
    libs/fluidsynth/libfluidsynth.a; do
  FORCE="$FORCE -Wl,-force_load,$TREE/$a"
done

"$(xcrun -f clang++)" $TARGET -O2 -o out/dosbox-x \
    out/main.o "$TREE/dosbox.o" $FORCE \
    "$PREFIX/lib/libSDL2.a" "$PREFIX/lib/libSDL2main.a" \
    "$PREFIX/lib/libSDL2_net.a" "$PREFIX/lib/libpng.a" \
    -lz -lm -liconv \
    -framework AVFoundation -framework AudioToolbox -framework CoreAudio \
    -framework CoreBluetooth -framework CoreGraphics -framework CoreHaptics \
    -framework CoreMotion -framework CoreVideo -framework Foundation \
    -framework GameController -framework Metal -framework OpenGLES \
    -framework QuartzCore -framework UIKit

mkdir -p "$APP" app/dos-assets
cp out/dosbox-x "$APP/"
cp app/Info.plist "$APP/"
cp -r app/dos-assets "$APP/dos"
codesign --force --sign - "$APP"
ls -la "$APP/dosbox-x"
echo "Bundle ready: $APP"
