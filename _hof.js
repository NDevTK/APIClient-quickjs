// Opaque-HOF callback driving on an EXECUTED path (not an orphan): the parent
// runs at top level with a host-edge opaque (JSON.parse(location.hash)), hits
// `.forEach`/`.map` on an opaque collection, and the callback's fetch must
// fire via the opaque-callee callback-drive (NOT the deep grind — these
// parents EXECUTE, so __feDriveStatic never re-drives them; without the
// opaque-call callback-drive the per-element fetch is silently dropped).
var data = JSON.parse(location.hash);   // opaque (tainted) — real host-edge source, no __opaque()

data.rows.forEach(function (r) {
  fetch("/api/row?key=" + r.key);
});

data.ids.map(function (id) {
  return fetch("/api/detail", { method: "POST", body: JSON.stringify({ id: id }) });
});

// Object.keys/values/entries over an opaque must yield one opaque element so
// the downstream CONCRETE .forEach/.map (callee NOT opaque — it's real
// Array.prototype on the returned array) runs once and the per-key fetch
// fires. Without the opaque guard in JS_GetOwnPropertyNames2 these return []
// and the iteration is empty.
Object.keys(data.cfg).forEach(function (k) {
  fetch("/api/setting/" + k);
});

Object.entries(data.map).forEach(function (e) {
  fetch("/api/kv?k=" + e[0] + "&v=" + e[1]);
});
