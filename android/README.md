# DOSBox-X for Android

An SDL2 app that runs dosbox-x as `libmain.so`. `DosBoxActivity` extends
SDL's `SDLActivity`. On first launch it unpacks the bundled DOS environment
from the APK's assets into the app's files directory, then starts dosbox-x
with `dos/dosbox-android.conf`. The app is locked to landscape, and
stdout/stderr go to `stdout.txt`/`stderr.txt` in the files directory.

## Build

```sh
../build-android.sh        # native side: libSDL2.so + libmain.so (arm64-v8a)
./build.sh                 # Java side + packaging -> out/dosbox-x.apk
adb install -r out/dosbox-x.apk
```

You need the Android SDK (with build-tools 36.0.0, platform android-34 and
an NDK) and a JDK. The SDK is looked up in `$ANDROID_SDK`, `$ANDROID_HOME`,
`~/Library/Android/sdk` and `~/Android/Sdk`. You can override the versions
with `BUILD_TOOLS` and `ANDROID_PLATFORM`. The APK is signed with
`~/.android/debug.keystore`.

## DOS environment

Anything under `stage/assets/dos/` is shipped in the APK and mounted as
`C:`. The default conf runs Duke Nukem 3D from `stage/assets/dos/DUKE3D/`.
That folder is gitignored, so you supply the game files yourself. Edit
`dosbox-android.conf` to run something else. When you change the shipped
payload, bump the `.unpacked-v1` marker in `DosBoxActivity.java` so devices
that already have the app unpack it again.

`src/org/libsdl/app/` holds SDL 2.32.10's stock Java glue. It must match
the SDL2 version that `build-android.sh` builds (`SDL2_VERSION`).
