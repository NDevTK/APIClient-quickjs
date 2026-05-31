// Polarity gate for the non-terminating-orphan freeze: OPAQUE-GATED RECURSION.
//
// The loop-revisit fixpoint (quickjs.c OP_if handlers + qjs_fe_seen) bounds an
// opaque-forced branch by keying on the branch SITE. For a self-contained loop
// the key is per-invocation (`_site ^ inv*golden`) and the SAME frame revisits
// the SAME site → fixpoint fires. But RECURSION makes a NEW frame per level,
// each with a fresh qjs_inv, and (in pure recursion) no ancestor has
// qjs_looped — so `_inloop=0`, the key is inv-salted, every recursion level
// hashes DIFFERENTLY, the seen-set never matches, and the opaque-gated
// recursion never reaches the fixpoint. It yields per opaque branch
// (qjs_host_yield) forever — the orphan never returns, starving every other
// page's grind (learn.microsoft.com freeze, /index-docs.js:148, seen_n→25M,
// resumeCount→108870, deepRem stuck).
//
// FIX (quickjs.c): a frame whose function bytecode also appears on an ancestor
// frame is a recursive re-entry — key its opaque branches on `_site` alone
// (collapse across recursion levels, like a loop-of-calls callee), so the
// second re-entry at the same branch site hits the seen-set and terminates.
// Concrete recursion never reaches qjs_fe_loop_revisit (only opaque branches
// do), so concrete tree walks are unaffected.
//
// Run: ./qjs.exe --fe-deep-grind hostedge.js _opqrec.js
// EXPECT: TERMINATES + @H fetch GET /api/rec-hit. Regression ⇒ hang (no @DS).
//
// The recurse arm carries the host edge so priority_default forces INTO the
// recursion (the productive arm); node.next reads opaque off an opaque receiver
// so the chain never bottoms out — without the recursion fixpoint this spins.
function walk(node) {
  if (node) {
    fetch("/api/rec-hit");      // host edge lives on the recurse arm
    walk(node.next);            // node.next opaque → unbounded opaque recursion
  }
}

function Tree() { walk(this.head); }   // opaque `this` → this.head opaque
globalThis.__TreeOrphan = Tree;        // registered as a value, NEVER invoked
