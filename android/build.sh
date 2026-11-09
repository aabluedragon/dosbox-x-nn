#!/bin/sh
# Builds the DOSBox-X Android APK: SDL2 Java glue + DosBoxActivity on the
# Java side, libSDL2.so + libmain.so (dosbox-x) on the native side, and a
# DOS environment (stage/assets/dos: conf + e.g. Duke3D) in assets.
#
# Run ../build-android.sh first: it produces libSDL2.so and libmain.so
# (under ../build-android/), which are staged into stage/lib/arm64-v8a/.
#
#   ./build.sh                  # -> out/dosbox-x.apk
#   adb install -r out/dosbox-x.apk
set -e
cd "$(dirname "$0")"
repo=$(cd .. && pwd)
if [ -z "$ANDROID_SDK" ]; then
    for d in "$ANDROID_HOME" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
        [ -n "$d" ] && [ -d "$d" ] && { ANDROID_SDK=$d; break; }
    done
fi
[ -d "$ANDROID_SDK" ] || { echo "Android SDK not found; set ANDROID_SDK." >&2; exit 1; }
: "${BUILD_TOOLS:=36.0.0}"
: "${ANDROID_PLATFORM:=android-34}"
BT=$ANDROID_SDK/build-tools/$BUILD_TOOLS
AJ=$ANDROID_SDK/platforms/$ANDROID_PLATFORM/android.jar

# Native libraries from ../build-android.sh (skipped if already staged by hand).
BA=$repo/build-android
if [ -f "$BA/tree/src/libmain.so" ]; then
    mkdir -p stage/lib/arm64-v8a
    cp "$BA/prefix/lib/libSDL2.so" "$BA/tree/src/libmain.so" stage/lib/arm64-v8a/
fi
[ -f stage/lib/arm64-v8a/libmain.so ] || { echo "libmain.so missing; run ../build-android.sh first" >&2; exit 1; }

rm -rf out/classes out/classes.dex out/base.apk out/app.apk
mkdir -p out/classes
javac -source 11 -target 11 -nowarn -classpath "$AJ" \
      -d out/classes $(find src -name '*.java')
"$BT/d8" --lib "$AJ" --output out $(find out/classes -name '*.class')
"$BT/aapt2" link -I "$AJ" --manifest AndroidManifest.xml -o out/base.apk \
      --min-sdk-version 28 --target-sdk-version 34

cp out/base.apk out/app.apk
cp out/classes.dex stage/
( cd stage && zip -q -r -0 ../out/app.apk classes.dex lib \
             && zip -q -r -6 ../out/app.apk assets )
"$BT/zipalign" -p -f 4 out/app.apk out/app-aligned.apk
rm -f out/dosbox-x.apk
"$BT/apksigner" sign --ks ~/.android/debug.keystore --ks-pass pass:android \
      --key-pass pass:android --out out/dosbox-x.apk out/app-aligned.apk
rm -f out/app.apk out/app-aligned.apk out/base.apk stage/classes.dex
ls -la out/dosbox-x.apk
