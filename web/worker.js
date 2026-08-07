/* DOSBox-X worker host (js-dos style): the emulator runs entirely in this
 * worker; the page thread owns input capture, audio playback and displays the
 * transferred OffscreenCanvas that SDL draws into. */

/* heartbeat: if these timestamps show a gap, the worker's event loop was
 * blocked; if they are even while frames are late, the emulator simply did
 * not present. */
self.__beats = [];
setInterval(() => {
  self.__beats.push(performance.now());
  if (self.__beats.length > 4000) self.__beats.splice(0, 2000);
}, 5);

let logBatch = [];
function ringLog(t) {
  logBatch.push(t);
  if (/Abort|missing function|E_Exit|CRITICAL/i.test(t)) postMessage({ type: 'critical', line: t });
}
setInterval(() => {
  if (logBatch.length) { postMessage({ type: 'log', lines: logBatch }); logBatch = []; }
}, 250);

var Module = {
  noInitialRun: true,
  print: ringLog,
  printErr: ringLog,
  onAbort: w => postMessage({ type: 'status', text: 'Aborted: ' + w }),
  onRuntimeInitialized() { postMessage({ type: 'ready' }); },
};

function fsMkdirs(path) {
  const parts = path.split('/').filter(Boolean);
  let cur = '';
  for (const p of parts) {
    cur += '/' + p;
    try { FS.mkdir(cur); } catch (e) { /* exists */ }
  }
}

/* ---- audio: pull S16 stereo from the mixer, post to the page ----
 * Closed loop: pull exactly what the mixer ring holds. A fixed-size pull on a
 * fixed timer can neither match the mixer's production rate nor its phase -
 * over-pulling engages the mixer's prebuffer pause (an audible gap) and pads
 * the shortfall with cloned samples, and the resulting surplus perpetually
 * overflowed the page-side latency cap, which dropped ~5 chunks of audio per
 * second (the "stutter" in music and sound effects). */
let audioPtr = 0;
function startAudioPump() {
  const tryStart = () => {
    const freq = Module._EMWORKER_AudioFreq();
    if (!freq) { setTimeout(tryStart, 500); return; }
    audioPtr = Module._EMWORKER_AudioBuffer();
    postMessage({ type: 'audioinfo', freq });
    /* let this much accumulate before the first drain; it becomes the
     * standing cushion in the page-side queue that rides out worker stalls
     * and production shortfalls - it must exceed the burst amplitude */
    const cushion = Math.round(freq * 0.10);
    let started = false;
    /* Ship everything the ring holds, every tick. The emulator produces in
     * bursts (with cycles=max a chunk of emulated time runs quickly, then a
     * catch-up idle) and falls briefly behind during translation storms;
     * shipping it all lets every shortfall heal itself from the following
     * catch-up burst - any cap or pacing here makes dips permanent, and the
     * queue ratchets down to dropouts. The page-side jitter buffer absorbs
     * the peaks; audio is discarded only if the ring nears overflow after a
     * genuine multi-hundred-ms stall. */
    const ringPanic = Math.round(freq * 0.35);
    let lastTick = 0;
    self.__audioGaps = [];
    setInterval(() => {
      const tickNow = performance.now();
      if (lastTick && tickNow - lastTick > 40) {
        self.__audioGaps.push({ t: tickNow, gap: tickNow - lastTick, avail: Module._EMWORKER_AudioAvail() });
        if (self.__audioGaps.length > 200) self.__audioGaps.splice(0, 100);
      }
      lastTick = tickNow;
      let avail = Module._EMWORKER_AudioAvail();
      if (!started) { if (avail < cushion) return; started = true; }
      const heap = Module.HEAP16 || (typeof HEAP16 !== 'undefined' ? HEAP16 : null);
      if (!heap) return;
      for (let guard = 0; guard < 8 && avail > ringPanic; guard++) {
        const n = Math.min(avail - ringPanic, 4096);
        Module._EMWORKER_PullAudio(audioPtr, n); /* stall recovery: discard stale audio */
        avail -= n;
      }
      /* bounded: the mixer pads pulls it cannot fill instead of consuming,
       * so never loop on AudioAvail alone */
      for (let burst = 0; burst < 4 && avail > 0; burst++) {
        const n = Math.min(avail, 4096);
        Module._EMWORKER_PullAudio(audioPtr, n);
        const copy = new Int16Array(new Int16Array(heap.buffer, audioPtr, n * 2));
        postMessage({ type: 'audio', buf: copy.buffer }, [copy.buffer]);
        avail -= n;
      }
    }, 20);
  };
  tryStart();
}

function exportTree(path, out) {
  for (const name of FS.readdir(path)) {
    if (name === '.' || name === '..') continue;
    const full = path + '/' + name;
    const st = FS.stat(full);
    if (FS.isDir(st.mode)) exportTree(full, out);
    else if (FS.isFile(st.mode)) {
      const data = FS.readFile(full);
      out.push({ path: full, data: data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) });
    }
  }
}

onmessage = (e) => {
  const m = e.data;
  switch (m.type) {
    case 'init':
      Module.canvas = m.canvas; /* OffscreenCanvas transferred from the page */
      /* frame-pacing probe: record when the emulator actually presents a frame */
      try {
        const proto = self.OffscreenCanvasRenderingContext2D.prototype;
        const orig = proto.putImageData;
        self.__frameTimes = [];
        proto.putImageData = function (...a) {
          const t = performance.now();
          self.__frameTimes.push(t);
          if (self.__frameTimes.length > 600) self.__frameTimes.shift();
          return orig.apply(this, a);
        };
      } catch (e) { /* probe is optional */ }
      /* html5 library helpers expect an HTMLCanvasElement */
      m.canvas.getBoundingClientRect = () => ({ x: 0, y: 0, left: 0, top: 0,
        width: m.canvas.width, height: m.canvas.height,
        right: m.canvas.width, bottom: m.canvas.height });
      m.canvas.style = {};
      m.canvas.addEventListener = () => {};
      m.canvas.removeEventListener = () => {};
      /* Minimal DOM shim: emscripten's html5 library resolves "#canvas" via
       * document.querySelector and registers listeners on document; give it
       * just enough to run in a worker. (window stays undefined so the glue
       * still detects the worker environment.) */
      self.document = {
        querySelector: sel => (sel === '#canvas' || sel === 'canvas') ? Module.canvas : null,
        getElementById: id => (id === 'canvas') ? Module.canvas : null,
        createElement: () => null,
        addEventListener: () => {},
        removeEventListener: () => {},
        title: '',
        visibilityState: 'visible',
        hasFocus: () => true,
        body: {},
      };
      self.screen = { width: 1024, height: 768 };
      importScripts('dosbox-x.js');
      break;

    case 'start': {
      try {
        if (typeof m.drcLimit === 'number') Module.__drcLimit = m.drcLimit;
        FS.writeFile('/dosbox-x.conf', m.conf || '');
        if (m.files) {
          fsMkdirs('/c');
          for (const f of m.files) {
            const dir = f.path.substring(0, f.path.lastIndexOf('/'));
            if (dir) fsMkdirs(dir);
            FS.writeFile(f.path, new Uint8Array(f.data));
          }
        }
        const args = ['-conf', '/dosbox-x.conf'];
        if (m.mount) args.push('-c', 'MOUNT C /c', '-c', 'C:');
        postMessage({ type: 'started' });
        if (!m.noAudio) startAudioPump();
        (typeof callMain === 'function' ? callMain : Module.callMain)(args); /* JSPI: returns a promise-like, runs on */
      } catch (err) {
        postMessage({ type: 'status', text: 'Start failed: ' + err });
      }
      break;
    }

    case 'key':      Module._EMWORKER_PushKey(m.code, m.down ? 1 : 0); break;
    case 'mousemove': Module._EMWORKER_PushMouseMotion(m.x, m.y, m.xrel, m.yrel); break;
    case 'mousebtn': Module._EMWORKER_PushMouseButton(m.btn, m.down ? 1 : 0, m.x, m.y); break;

    case 'stats':
      postMessage({ type: 'stats', compiled: Module.__drcCount || 0,
                    failed: Module.__drcFail || 0, bail: Module.__drcBail || {}, ms: Module.__drcTime || 0,
                    path: Module.__drcPath || {},
                    smc: Module.__smc || {},
                    smcBytes: Module.__smcBytes || {},
                    audioGaps: (self.__audioGaps || []).slice(),
                    frames: (self.__frameTimes || []).slice(),
                    events: (Module.__drcEvents || []).slice(),
                    beats: (self.__beats || []).slice(),
                    cyclesRun: Module._EMWORKER_CyclesRun ? Module._EMWORKER_CyclesRun() : 0,
                    cycleMax: Module._EMWORKER_CycleMax ? Module._EMWORKER_CycleMax() : 0 });
      break;

    case 'export': {
      const out = [];
      try { exportTree('/c', out); } catch (err) {}
      postMessage({ type: 'exported', files: out }, out.map(f => f.data));
      break;
    }
  }
};
