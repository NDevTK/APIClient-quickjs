// Polarity gate for opaque-infectious null checks (freeze #3, learn.microsoft.com).
// OP_is_null / OP_is_undefined / OP_is_undefined_or_null tested the VALUE TAG
// and returned a CONCRETE boolean for an opaque (an opaque is an object — tag
// OBJECT — so "is null" = false, "!= null" = true). So the canonical DOM walk
// `while (node.firstChild !== null) …` over an opaque node was a CONCRETE
// infinite loop the forced-exec loop-revisit fixpoint could never see (it
// forces only OPAQUE branches; the seen-set stays empty). Frozen
// `removeChildNodes` on Microsoft Learn (lit-html). The fix makes the null
// checks opaque-infectious, so the branch forks and the fixpoint terminates it.
// Run: ./qjs.exe --fe-deep-grind hostedge.js _opqnull.js
// EXPECT: terminates + @H fetch GET /api/null-ok. A regression hangs (timeout).
function walkNull(e) {
  var n = e.firstChild;                  // opaque
  while (n !== null) n = n.nextSibling;  // null-check on opaque must FORK, not loop forever
  var m = e.lastChild;
  while (m != null) m = m.previousSibling; // loose != null too (OP_is_undefined_or_null)
  return fetch("/api/null-ok");          // @H fires IFF both loops terminated
}
globalThis.__nullOrphan = walkNull;      // registered as a value, NEVER invoked
