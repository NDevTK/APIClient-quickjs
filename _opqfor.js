// GREEN gate — an opaque-bounded forEach-polyfill loop driven as an orphan
// MUST terminate by the loop-revisit fixpoint and emit @H /api/each.
// Models learn.microsoft.com /index-docs.js's `ye`:
//   function ye(e,t,n){if(e)for(var r=e[b]>>>0,i=0;
//        i<r&&!(i in e&&-1===t[ce](n||e,e[i],i,e)); i++);}
// `e` opaque ⇒ `r = e.length >>> 0` opaque ⇒ `i < r` is an opaque-forced
// OP_if the fixpoint bounds. This SIMPLE shape terminates (EXIT 0, @H=1).
// Run: `./qjs.exe --fe-exec hostedge.js _opqfor.js driver.js` (driver.js
// LAST — the epilogue drives the __FeedOrphan via __feDriveStatic).
//
// KNOWN-OPEN (NOT yet reproduced minimally): the REAL ye in _idxdocs HANGS
// (`./qjs.exe --fe-exec hostedge.js _idxdocs.pre.js _idxdocs.js driver.js`
// → EXIT 124). The spin probe shows the fixpoint FIRES (revisit_seen=1) yet
// `i` climbs to 24M+ — the per-frame key changes across iterations because
// the callback recurses through a NATIVE frame (each invocation a fresh
// frame/inv). This minimal version doesn't trigger that; the gap is in how
// the loop-revisit key survives recursion-through-native-callback. See
// [[project_recursion_revisit_fixpoint]].
function each(arr, cb) {
  // Matches ye exactly: the `i in arr` (OP_in on an OPAQUE arr) sits next to
  // the `i < r` opaque guard. If OP_in concretizes the opaque, the AND's
  // shape changes and the loop-revisit fixpoint's forced `i<r` exit no longer
  // governs continuation.
  for (var r = arr.length >>> 0, i = 0; i < r && !(i in arr && cb(arr[i], i, arr) === -1); i++);
  return fetch("/api/each");        // @H fires IFF the loop terminated
}
function feed(coll) {               // orphan; opaque `this`/arg → coll opaque
  each(coll, function (v, i) { return each(v, function () { return 0; }); });
}
globalThis.__FeedOrphan = feed;     // registered as a value, never invoked
