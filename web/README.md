# DOSBox-X in the browser (WebAssembly)

A js-dos-style browser frontend: the emulator runs single-threaded (JSPI) inside
a Web Worker (`worker.js`); the page (`index.html`) only composites the
transferred OffscreenCanvas, plays audio through an AudioWorklet, and forwards
input into the SDL event queue via the `EMWORKER_*` exports in
`src/gui/sdlmain.cpp` / `src/hardware/mixer.cpp`.

## Build

Tested with emsdk **6.0.5** (JSPI needs a current Chrome). From a clean tree:

```sh
source $EMSDK_HOME/emsdk_env.sh
export PATH=$EMSDK/upstream/emscripten/system/bin:$PATH
export PKG_CONFIG_LIBDIR=/nonexistent   # keep host pkg-config out of the probe
embuilder build zlib libpng sdl2 sdl2_net
./autogen.sh
CC=emcc CXX=em++ LD=emcc LD_CXX=em++ AR=emar RANLIB=emranlib \
CFLAGS="-flto -msimd128 -DEMSCRIPTEN=1 -s USE_ZLIB=1 -s USE_LIBPNG=1 -s USE_SDL=2 -s USE_SDL_NET=2" \
CXXFLAGS="$CFLAGS" \
LDFLAGS="-O3 -flto -msimd128 -s USE_ZLIB=1 -s USE_LIBPNG=1 -s USE_SDL=2 -s USE_SDL_NET=2 \
  -s FORCE_FILESYSTEM=1 -s ALLOW_MEMORY_GROWTH=1 -s TOTAL_MEMORY=201326592 -s JSPI \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0" \
./configure --host=x86_64-linux --disable-dynrec --disable-dynamic-x86 --disable-fpu-x86 \
  --disable-dynamic-core --enable-sdl2 --with-sdl-prefix="$EMSDK/upstream/emscripten/system" \
  --disable-screenshots --disable-opengl --disable-mt32 --enable-emscripten \
  --enable-force-menu-sdldraw --disable-x11 --disable-avcodec --disable-libslirp \
  --disable-libfluidsynth --disable-freetype --disable-gamelink
make -j8
cp src/dosbox-x web/dosbox-x.js && cp src/dosbox-x.wasm web/
```

Notes:

- `--disable-gamelink` is required: the emscripten link excludes the gamelink
  library but `C_GAMELINK` would still route RAM allocation through it → abort
  at boot.
- Delete `vs/*/linux-host` if a native build ran in the same tree first, or
  configure picks up the in-tree SDL.
- The emsdk SDL2 port needs small worker-proofing patches (guards around
  `document` / `window` / `screen` / `canvas.style` in
  `src/video/emscripten/*.c`, and `MAIN_THREAD_EM_ASM_INT` for the sampleRate
  read in `src/audio/emscripten/SDL_emscriptenaudio.c`) — delete
  `cache/sysroot/lib/wasm32-emscripten/libSDL2.a` after patching so it rebuilds.

## Run

```sh
./serve.sh   # http://localhost:8000/ (wasm cannot load from file://)
```

Pick a folder to mount as C: (Chromium: File System Access API, read-write
with "Sync changes back to disk"; other browsers: read-only directory upload).
The config panel is written into the emulator as `dosbox-x.conf` at launch.
Keep fixed `cycles=` — `cycles=max` mis-calibrates badly under browser timing.
