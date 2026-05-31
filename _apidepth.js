// API learning depth — example values come from RUNNING the code, NOT
// from a literal that already sits inside fetch(). Each test stresses a
// computation chain that only the real ECMA engine could resolve:
//   - JSON.stringify of an object built from forced gates
//   - btoa around it (Web-edge), then concatenated into a header-style
//     query param
//   - Array.from/map/join over a list literal
//   - encodeURIComponent of a derived value
// If any value is "[object Object]" or missing, the engine isn't really
// computing — it's matching a literal in source.

// 1) Forced-gate enum + JSON.stringify chain
//    Schedule '0', '1' will pick guest/admin and free/PRO.
var body = JSON.parse('{"role":"guest","tier":"free","id":7}');
if (body.role === "admin") {}            // force-binds role
if (body.tier === "PRO")  {}             // force-binds tier
var payload = JSON.stringify({ role: body.role, tier: body.tier, id: body.id });
// btoa is at the Web edge; the encoded value must reflect the
// forced-gated body (real base64 of {"role":"…","tier":"…","id":7}).
fetch("/api/audit?p=" + encodeURIComponent(btoa(payload)));

// 2) Array.from / map / join over a list literal — example
//    "items=a,b,c" must appear AS-IS in the captured params list.
var items = ["alpha", "beta", "gamma"];
fetch("/api/list?items=" + items.map(function (s) { return s.toUpperCase(); }).join(","));

// 3) Reduce-driven URL — sum, mapped, formatted.
var ns = [1, 2, 3, 4, 5];
var sum = ns.reduce(function (a, b) { return a + b; }, 0);
fetch("/api/sum?n=" + sum + "&seen=" + ns.join("|"));

// 4) Multi-stage computation through a closure — the closure CAPTURES
//    a forced-bound variable and the final URL is its closure return.
function mk(prefix) { return function (n) { return prefix + ":" + n; }; }
var t = mk("kind")(42);
fetch("/api/ping?tag=" + encodeURIComponent(t));
