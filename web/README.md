# DOSBox-X in the browser (WebAssembly)

A js-dos-style browser frontend: the emulator runs single-threaded (JSPI) inside
a Web Worker (`worker.js`); the page (`index.html`) only composites the
transferred OffscreenCanvas, plays audio through an AudioWorklet, and forwards
input into the SDL event queue via the `EMWORKER_*` exports in
`src/gui/sdlmain.cpp` / `src/hardware/mixer.cpp`.

## Build

```sh
./build-wasm.sh      # from the repository root
```

The script builds the WebAssembly target from a git worktree of this same
repository under `build-wasm/` (ignored) and deploys `dosbox-x.js` and
`dosbox-x.wasm` into `web/`. A worktree is used because autotools will not do
a VPATH build against a source directory that is already configured, and the
source directory holds the native build's configuration.

It needs emsdk (tested with 6.0.5, which JSPI requires); source its
`emsdk_env.sh` first or point `EMSDK_DIR` at it. The flags it uses are worth
knowing about:

- `--enable-dynrec` builds the WebAssembly recompiling core
  (`src/cpu/core_dynrec/risc_wasm.h`), roughly twice the interpreter's speed.
- `-s JSPI` uses native stack switching rather than Asyncify instrumentation.
- `-s INITIAL_MEMORY` with no `ALLOW_MEMORY_GROWTH`: growth copies the whole
  heap, which shows up as a multi-hundred-millisecond freeze.
- `--disable-gamelink` is required, or `C_GAMELINK` routes RAM allocation
  through a library the emscripten link excludes and the machine aborts at
  boot.
- `PKG_CONFIG_LIBDIR=/nonexistent` keeps the host's pkg-config from offering
  a native SDL2 with `-framework Cocoa` in its link line.

The emsdk SDL2 port also needs small worker-proofing patches (guards around
`document`, `window`, `screen` and `canvas.style` in
`src/video/emscripten/*.c`, and `MAIN_THREAD_EM_ASM_INT` for the sampleRate
read in `src/audio/emscripten/SDL_emscriptenaudio.c`). Delete
`cache/sysroot/lib/wasm32-emscripten/libSDL2.a` after patching so it rebuilds.

## Run

```sh
./serve.sh   # http://localhost:8000/ (wasm cannot load from file://)
```

Pick a folder to mount as C: (Chromium: File System Access API, read-write
with "Sync changes back to disk"; other browsers: read-only directory upload).
The config panel is written into the emulator as `dosbox-x.conf` at launch.
Keep fixed `cycles=` — `cycles=max` mis-calibrates badly under browser timing.
