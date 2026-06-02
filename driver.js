// Host epilogue — runs LAST. Pumps the event loop AND force-drives
// the functions the app's data-fetching code hides behind: bundle-
// introduced global functions and every registered event handler
// (J-Force §3.1.3), by source identity — no bundler/registry
// recognition. __hostRan counts distinct functions executed; it is
// MONOTONE and bounded by the bundle's finite function-source set, so
// looping while it grows is a finite fixpoint (same seen-set principle
// as the schedule enumeration) — no magic cap. Real Promise jobs drain
// separately on the engine's own js_std_loop after this.
//
// IIFE-scoped on purpose: a top-level `var` here would be a
// bundle-introduced GLOBAL, and __hostDrive force-invokes those — it
// would call __hostFlush itself while muted, pumping the program's own
// event handlers under mute and collapsing their forced exploration.
(function () {
  /* @WHY breadcrumb: driver.js entered. Absence in stdout means a
     per-script eval aborted before reaching driver.js (the abort would
     show in stderr as "Aborted(...)"). Distinguishes "driver hung in
     pump fixpoint" from "driver never ran". Uses __feEmit (the real
     print captured by hostedge before it clobbered globalThis.print
     so bundle code's print() can't pollute the @H/@S/@T/@E/@WHY
     channel) — driver.js's print() would be a no-op. */
  try { (globalThis.__feEmit || function () {})("@WHY {\"phase\":\"driver_entry\"}"); } catch (e) {}
  var flush = (typeof __hostFlush === "function") ? __hostFlush : function () {};
  var drive = (typeof __hostDrive === "function") ? __hostDrive : function () {};
  var ran = (typeof __hostRan === "function") ? __hostRan : function () { return 0; };
  // JS_ExecutePendingJob loop exposed by qjsmain.c; drains Promise
  // microtasks inside the fixpoint so fetch().then(cb) callbacks fire
  // WHILE __hostDrive can still react to newly-registered handlers
  // (Catalyst/Turbo/React `then`-chained fetches on github otherwise
  // wait until js_std_loop after driver.js exits — too late).
  var pump = (typeof __hostMicrotaskDrain === "function") ? __hostMicrotaskDrain : function () { return 0; };
  // Spin-defer the BFS drive (the analogue of the deep grind's defer): a
  // force-invoked global that hits a CONCRETE spin — a cold-state loop like
  // MathJax's `for(;t.length>0;)` over an empty worklist, which the opaque
  // loop-revisit fixpoint can't key and the deep-grind's seen_n-flat signal
  // is gated off for here — would otherwise hang the entire analysis (the
  // seed schedule never returns; runs:1, no endpoints). __feBfsActive(1)
  // arms a same-back-edge spin signal; the interrupt throws out of the
  // spinning global into __hostDrive's per-global catch, which skips it and
  // continues. Brackets BOTH the __hostDrive fixpoint and the @T static
  // drive below. Pause-and-defer, never a cap.
  var bfsActive = (typeof __feBfsActive === "function") ? __feBfsActive : function () {};
  var n = -1, m = ran();
  bfsActive(1);
  try {
  while (m !== n) { n = m; drive(); flush(); pump(); m = ran(); }
  // The JAW static half (site scan + @T→@H drive) is SCHEDULE-INDEPENDENT:
  // it enumerates the whole compiled-function set and drives the unreached
  // host-bearing ones, identically regardless of the forced decision
  // vector. Running it on every schedule would redo the full-bundle drive N
  // times (the github perf cliff). Run it ONCE, on the seed schedule (empty
  // decision string → __feLen()===0); subsequent schedules contribute only
  // their schedule-DEPENDENT dynamic frontier. A function reached only on a
  // later schedule emits its @H on that schedule's own dynamic run, so
  // nothing is missed — the seed static drive covers exactly the
  // never-dynamically-reached residue. Not a cap: the static phase is
  // complete, it just isn't redundantly repeated.
  var feSeed = (typeof __feLen === "function") ? (__feLen() === 0) : true;
  // JAW static half: read-only bytecode scan for host-edge call sites
  // in ALL compiled functions, incl. ones no forced path reached
  // (unrequired modules). No execution — emits @T structural
  // candidates only; the loop above already supplied @H values for the
  // reached subset.
  if (feSeed && typeof __feStaticSites === "function") { try { __feStaticSites(); } catch (e) {} }
  // JAW @T → @H promotion via forced static-function driving (J-Force
  // §3.1.3 generalised to non-global functions). Calls every bytecode
  // function containing a host-edge atom with opaque args matching its
  // declared arg_count — AND an opaque `this`, since most real host-edge
  // sites are class methods reading `this.fooUrl`/`this.src` (with
  // this=undefined the first `this.X` throws and the method aborts before
  // its fetch). Its fetch/XHR/WebSocket then emits a real @H with whatever
  // ECMA computed for the call. The fetch is usually behind a branch
  // (`if(cond) fetch(...)`), so the BFS must explore both arms to reach
  // the branch-conditional endpoint + body-field example values — that
  // unused-feature surface is what code analysis exists to recover.
  if (feSeed && typeof __feDriveStatic === "function") {
    // NO __feMute: muting frontier emission inside driven functions was a
    // perf shortcut that BROKE depth — a force-driven feature function's
    // fetch is usually behind a branch (`if(cond) fetch(...)`); with mute
    // the BFS never explores the other arm, so the branch-conditional
    // endpoint + its body-field example values are silently dropped (the
    // exact unused-feature surface code analysis exists to recover —
    // audited on github: 92 fetch @T collapsed to ~9 @H under mute).
    // Termination is structural (distinct prefixes + X-Force fitness +
    // SMT pruning), never a mute/cap; a slow run is a fitness bug to fix.
    //
    // FIXPOINT: a chunk-loader drive runs `require.e(id)` and queues
    // `.then(require.bind(require, moduleId))`; that callback (which RUNS the
    // lazy module, instantiating its functions — incl. the login-gated
    // `preheat` fetch) only fires on the microtask drain AFTER this
    // __feDriveStatic returned, so its newly-instantiated host closures
    // weren't in this pass's target set. Re-drive until a pass drives
    // nothing new: monotone (every pass drives strictly-new qjs_executed=0
    // functions, bounded by the finite instantiated-closure set), so it
    // terminates — NOT a cap. Each pass: drive, then drain so the next pass
    // sees what this one's loaders instantiated.
    var _dn = -1;
    while (_dn !== 0) {
      try { _dn = __feDriveStatic(); } catch (e) { _dn = 0; }
      if (typeof __hostMicrotaskDrain === "function") { try { __hostMicrotaskDrain(); } catch (e) {} }
    }
    // The DEEP pass (orphan @T drive for render-gated chunk endpoints like
    // preheat) is NOT run here — it's stepped from the worker via qjsmain's
    // persistent-runtime --fe-deep-step batches with sleeps between, so it
    // runs at a low CPU duty cycle (cool, background) instead of pegging this
    // run's core. The boot above (incl. this static/loader drive) marks the
    // reached set, so the stepped deep pass only drives the true residue.
  }
  } finally { bfsActive(0); }
})();
