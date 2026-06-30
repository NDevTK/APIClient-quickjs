// #5 cross-session determinism — emscripten-layer clock override.
//
// `_emscripten_date_now = () => Date.now()` is emscripten's libc wall-clock date source. WASMFS
// uses it for file timestamps (atime/mtime/ctime, stored as ms doubles), which the host's infra
// files (/h.js, /d.js, /pre.js, /p.js, the bundle .bc + slices) acquire at boot. Those doubles land
// in the EARLIEST heap allocations and made two boots of the same bundle hash differently — breaking
// the byte-identical re-boot that cross-session resume replays an (address,word) delta against. This
// is a layer BELOW quickjs (emscripten libc), so the quickjs-side js__gettimeofday_us/js__hrtime_ns
// determinism fix did not cover it; and these are DOUBLE ms values (~1.75e12), i.e. emscripten_date_now,
// NOT the JS Date.now intrinsic (which is already synth-opaque under forced exec).
//
// Override it with a deterministic monotonic counter. Real file time is never wanted in the analysis
// engine. This replaces ONLY the wasm's internal libc date source; the host worker's own global
// Date.now (used for scheduling/sliceCost) is a different function and stays real.
addToLibrary({
  $qjsDetMs: '1750000000000',
  emscripten_date_now__deps: ['$qjsDetMs'],
  emscripten_date_now: function () { return qjsDetMs++; },
});
