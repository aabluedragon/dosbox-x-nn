#!/bin/sh
# Builds the DOSBox-X Android APK: SDL2 Java glue + DosBoxActivity on the
# Java side, libSDL2.so + libmain.so (dosbox-x) on the native side, and a
# DOS environment (Duke3D + dosbox conf) in assets.
set -e
cd "$(dirname "$0")"
SDK=$HOME/Library/Android/sdk
BT=$SDK/build-tools/36.0.0
AJ=$SDK/platforms/android-34/android.jar

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
