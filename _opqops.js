// Polarity gate for the opcode-infectivity audit — the opaque-concretization
// CLASS (the systematic close-out after freezes #1/#3 pointed at one opcode
// each). bitwise (& | ^ << >> >>>), unary (- ~ +), and typeof on an opaque must
// STAY opaque; otherwise a loop gated by them collapses to a CONCRETE infinite
// loop the forced-exec loop-revisit fixpoint can never terminate (it forces
// only OPAQUE branches). Companion to _opqnull (null checks) / _opqloop (++ in
// a muted cb-drive).
// Run: ./qjs.exe --fe-deep-grind hostedge.js _opqops.js
// EXPECT: terminates + @H fetch GET /api/ops-ok. A regression hangs (timeout).
function opLoops(e) {
  var a = e.flags;                              // opaque
  while (a & 1) a = a >> 1;                      // bitwise & and >> on opaque must fork
  var b = e.count;                              // opaque
  while (typeof b !== "undefined") b = b.next;  // typeof on opaque must fork
  var c = e.n;                                  // opaque
  while (-c < 0) c = c.parent;                  // unary neg on opaque must fork
  return fetch("/api/ops-ok");                  // @H fires IFF all three loops terminated
}
globalThis.__opsOrphan = opLoops;               // registered as a value, NEVER invoked
