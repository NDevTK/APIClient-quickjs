// Polarity gate for the non-terminating-orphan freeze fix (two coupled fixes):
//   (1) opaque-infectious ++/-- (quickjs.c qjs_unary_step): `++n` on an opaque
//       counter STAYS opaque, instead of ToNumber-coercing to NaN. With NaN,
//       `NaN >= 10` is a CONCRETE false, so the loop's exit branch is concrete
//       and the forced-exec loop-revisit fixpoint (which only fires on OPAQUE
//       branches) is blind to it.
//   (2) loop-revisit fixpoint NOT gated on qjs_fe_mute (quickjs-forced.h): the
//       opaque-callee cb-drive runs the pushed callback MUTED (to isolate the
//       outer cursor/Φ/frontiers); muting also disabled the termination
//       fixpoint, so the callback's opaque loop spun forever.
//
// Mirrors markdown-it's autolink rule reached via `ruler.push(rule)` on an
// opaque receiver (learn.microsoft.com froze here, 2026-05-30).
// Run: ./qjs.exe --fe-deep-grind hostedge.js _opqloop.js
// EXPECT: TERMINATES (no hang) + @H fetch GET /api/loop-hit.
// A regression in EITHER fix makes this hang (timeout, no @DLOOP_DONE, no @H).

function Ruler() { this.rules = []; }
Ruler.prototype.push = function (f) { this.rules.push(f); };

// The "rule": an opaque-counter loop with a CONCRETE bound (10) and a CONCRETE
// hit (5). Driven with an opaque parser state `e`, so n = e.pos is opaque.
function looprule(e) {
  var n = e.pos;                            // opaque
  for (;;) { if (++n >= 10) break; }        // concrete bound: needs opaque-infectious ++
  return fetch("/api/loop-hit");            // unconditional → @H fires IFF the loop terminated
}

// Orphan: a constructor-shaped fn the deep grind drives with an OPAQUE `this`.
// `this.ruler` reads back opaque (get on an opaque receiver), so
// `this.ruler.push(looprule)` is an opaque-callee call → the cb-drive invokes
// looprule with opaque args under qjs_fe_mute=1.
function Setup() {
  this.ruler = new Ruler();
  this.ruler.push(looprule);
}
globalThis.__SetupOrphan = Setup;   // registered as a value, NEVER invoked
