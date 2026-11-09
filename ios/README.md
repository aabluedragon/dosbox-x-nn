# DOSBox-X for iOS (simulator)

A UIKit app that links dosbox-x's static archives and SDL2 into one binary.
`app/main.m` supplies `main()`. On first launch it copies the bundled DOS
environment into Documents, writes a conf with absolute container paths
on every launch, and starts dosbox-x through `SDL_UIKitRunApp`. The app is
locked to landscape, and stdout/stderr go to `stdout.txt`/`stderr.txt` in
Documents.

## Build (macOS with Xcode)

```sh
../build-ios.sh            # dosbox-x archives + static SDL2/libpng/SDL2_net
./build.sh                 # link + bundle -> out/DOSBox-X.app
./run.sh                   # install and launch on the booted simulator
```

The target is `arm64-apple-ios13.0-simulator`. Set `DEVELOPER_DIR` if Xcode
is not at `/Applications/Xcode.app`.

## DOS environment

Anything under `app/dos-assets/` is copied into the bundle as `dos/` and
mounted as `C:`. The generated conf runs Duke Nukem 3D from
`app/dos-assets/DUKE3D/`. That folder is gitignored, so you supply the game
files yourself. Edit the conf in `prepareDosEnvironment()` in `app/main.m`
to run something else, and bump its `.unpacked-v1` marker when the payload
changes.
