// COVERAGE (not the freeze reproducer): a cold-driven async orphan with an
// opaque-gated for(;;) loop that AWAITS each iteration must TERMINATE and promote
// its fetch. Guards async-loop driving in general.
//
// The actual MS-Learn SSE-reader freeze (_idxdocs.js:9485 `async run`) was an
// async-RESUME keying bug: `restart:` re-stamped sf->qjs_inv = ++qjs_inv_ctr on
// every await-resume, so an opaque loop containing an `await` was keyed by an
// ever-changing inv and the loop-revisit fixpoint never collapsed it (spin-probe
// confirmed inv climbing 92807→192807→… on the spinning frame). The fix preserves
// qjs_inv/qjs_looped across generator/async resume. This minimal fixture does NOT
// reproduce that freeze — the deep-grind's bounded microtask pump only resumes a
// simple await finitely, whereas the real bundle's promise chain re-suspends
// unboundedly. The regression gate for the freeze is the real bundle: `_idxdocs`
// (must reach @DLOOP_DONE, was frozen at orphan ~10825). See [[reference_quickjs_version]].
//
// Run: node qjs_wasm.js --fe-deep-grind hostedge.js _opqasync.js
// EXPECT: terminates (@DLOOP_DONE), promotes the fetch to @H.
class Reader {
  async run() {
    for (;;) {
      if (this.done) return;            // opaque exit; the continue arm always awaits
      await this.next();                // this opaque -> this.next() opaque -> await OPAQUE
      fetch("/api/stream-rec?i=" + this.recordIndex);
      this.recordIndex = this.recordIndex + 1;
    }
  }
}
