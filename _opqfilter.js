// Gate: a worklist loop whose array is reassigned via .filter() with an OPAQUE
// predicate must TERMINATE. `t = t.filter(x => o.indexOf(x) < 0)` where `o` is
// opaque → `o.indexOf(x)` opaque → `<0` opaque → the predicate returns opaque.
// Array.prototype.filter consumed that via JS_ToBool (concretizing it truthy →
// KEEP all elements → t unchanged → `t.length` a constant), so `for(;t.length;)`
// was a CONCRETE infinite loop the forced-exec fixpoint can't see (no OP_if ever
// forks on the concrete length). filter/some/every must be opaque-infectious:
// an opaque predicate result ⇒ opaque output, so `t` becomes opaque, `t.length`
// is opaque, the loop predicate forks and the loop-revisit fixpoint bounds it.
// Real freeze: MathJax speech-rule-engine `setdifference = t.filter(x=>o.indexOf
// (x)<0)` on learn.microsoft.com's safeFetch-loaded tex-mml-chtml.js froze the
// deep grind (live @currentOrphan /tex-mml-chtml.js:11694). Same CLASS as
// [[project_opaque_infectivity_array_concat]].
//
// Run: node qjs_wasm.js --fe-deep-grind hostedge.js _opqfilter.js
// EXPECT: terminates (@DLOOP_DONE). Before the fix: spins forever.
class Diff {
  // never instantiated -> __feDriveStatic drives run() cold (opaque arg)
  run(other) {
    let t = [1, 2, 3];                              // REAL array (concrete length)
    for (; t.length > 0; ) {
      t = t.filter((x) => other.indexOf(x) < 0);    // opaque predicate -> filter must yield opaque
      fetch("/api/diff?n=" + t.length);
    }
    return t;
  }
}
