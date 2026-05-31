// Gate: a worklist loop whose array grows by concatenating an OPAQUE (unknown)
// collection must TERMINATE. `r.concat(opaque)` currently returns a REAL array
// with the opaque appended as ONE element, so `r.length` stays a concrete 1
// forever and `for(;r.length;)` is a CONCRETE infinite loop the forced-exec
// fixpoint can't see (it only bounds OPAQUE-predicate loops). Array.prototype
// .concat must be opaque-infectious: concat with an opaque arg (or opaque this)
// yields opaque, so `r` becomes opaque, `r.length` is opaque, the loop predicate
// forks and the loop-revisit fixpoint bounds it. Real freeze: the MathJax
// speech-rule-engine TrieNode.collectRules_ on learn.microsoft.com
// (cs.js:11701) froze the deep grind at orphan #17732 (native @DSTART stuck
// 90s≡220s). Same CLASS as [[project_opaque_infectivity_builtins_synth]].
//
// Run: node qjs_wasm.js --fe-deep-grind hostedge.js _opqconcat.js
// EXPECT: terminates (@DLOOP_DONE). Before the fix: spins forever.
class Trie {
  // never instantiated -> __feDriveStatic drives collect() cold (opaque arg)
  collect(t) {
    let r = [t];
    for (; r.length; ) {
      const x = r.shift();
      if (x.getKind() === 1) fetch("/api/rule?k=" + x.getRule());
      r = r.concat(x.getChildren());   // getChildren() opaque -> concat(opaque)
    }
    return r;
  }
}
