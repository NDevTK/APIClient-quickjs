// Minimal forced-multi-path driver (X-Force schedule enumeration)
// against the proven patched QuickJS. Interface is concrete + proven:
//   - FORCED_SCHEDULE = string of 0/1 decisions
//   - FORCED_TRACE    = file: "B <cur> <dec>" per branch, "F <cur>"
//                       when the schedule ran out at that branch
//   - stdout "@H {...}" = host-edge record
// Enumerate: start []; each run, for every frontier index i emit the
// run's decision prefix[0..i-1] + "1" (flip it); dedup by schedule.
// Finite: programs terminate per run, prefixes are distinct.
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync, rmSync } from "node:fs";
import { resolve } from "node:path";

// QJS_BIN unset -> native ./qjs.exe ; set to a .js -> the wasm build
// (`node qjs_wasm.js ...`). Same driver, same trace/host protocol.
const WBIN = process.env.QJS_BIN;
const QJS = WBIN ? process.execPath : "./qjs.exe";
const PRE = WBIN ? [WBIN] : [];
const files = process.argv.slice(2);          // hostedge.js bundle.js call.js
// Single fixed trace path reused across every forced run. The engine
// opens it with "w" mode (quickjs-forced.h qjs_forced_config) so each
// run truncates and writes fresh content — no accumulation, no
// rmSync between iterations, no per-PID/per-seq file spam in cwd.
// Absolute forward-slash path so MSYS-built qjs.exe (fopen) and
// Node-on-Windows (fs) resolve identically.
const TRACE_PATH = resolve(process.cwd(), `.fe.trace.${process.pid}`).replace(/\\/g, "/");
function nextTracePath() { return TRACE_PATH; }
process.on("exit", () => { try { rmSync(TRACE_PATH, { force: true }); } catch (_) {} });
const work = [""];                            // schedule strings
const seen = new Set([""]);
const distinct = new Set();                   // distinct host records (count only)
const coveredEdges = new Set();               // X-Force Linear Search fitness: "<branchKey>:<dec>" seen
const structural = new Map();                 // JAW @T: host-edge sites in unreached code
const sinkSeen = new Set();                   // sink-discovery for the Quadratic/bootstrap switch
const sinkRecs = new Map();                   // reached tainted sinks (@S records), key type|sink|pos
const zMap = new Map();                       // @Z path-satisfiability verdicts, key sink|psi (dedup across runs)
// X-Force (Peng et al., USENIX Sec '14) Algorithm 1 §3.2 with a
// principled hybrid of the paper's three named fitness functions —
// composing them gives the right behaviour for security-sink
// discovery in realistic JS bundles where pure Exponential is too
// slow and pure Linear misses state-dependent sinks:
//
//   - **Linear Search** (§3.2, fitness 1, O(n)) — default. `eval =
//     !covered(p, ¬b)`: enqueue a frontier flip only if its
//     `(key, dec)` edge is uncovered. Bounds the post-saturation
//     backlog; preserves jQuery (no opaque branch → no frontier).
//
//   - **Quadratic Search** (§3.2, fitness 2, O(n²)) — boost. When a
//     run discovers a NEW security sink, "the execution is considered
//     important and all eligible unique predicates (not instances) in
//     the execution are further explored" — verbatim from the paper.
//     This is what lets a multi-message PoC's deeper schedules get
//     explored even after Linear has covered the same edges via a
//     non-_cfg path.
//
//   - **Exponential Search** (§3.2, fitness 3) bounded to the
//     bootstrap window. "X-Force provides the capability for the
//     user to limit such exponential search within a sub-range." Our
//     sub-range = "no sink has fired anywhere yet" — until the first
//     sink discovery, drop the edge prune entirely (seen-set still
//     bounds enumeration to distinct prefixes). After the first sink,
//     Linear+Quadratic take over.
//
// Termination is structural — the seen-set of distinct schedule
// prefixes is finite (each forced run is a terminating program;
// opaque loops fork → an arm exits → distinct prefixes saturate).
let runs = 0;

// Adapter: proven @H host records -> the API model the extension
// consumes (endpoint + params with location query|body, real example
// values gathered ACROSS the forced paths). Pairing of open->send is
// done PER RUN (one coherent forced path) so every branch's body
// variant is kept; examples then aggregate across runs.
function parsePairs(s) {            // a=1&b=2  (form-encoded body/query)
  const o = {};
  for (const kv of String(s).split("&")) {
    if (!kv) continue;
    const i = kv.indexOf("=");
    const k = decodeURIComponent(i < 0 ? kv : kv.slice(0, i));
    const v = i < 0 ? "" : decodeURIComponent(kv.slice(i + 1));
    (o[k] || (o[k] = [])).push(v);
  }
  return o;
}
const methods = new Map();          // "METHOD path" -> {method,path,params:Map(name->{location,valueSource,examples:Set}),headers:Map(name->{valueSource,examples:Set})}
// Walk a hostedge.js bodyShape into per-field add() calls. Preserves
// nested structure by joining field paths with "." (e.g. "user.email")
// so a {user:{email:opaque}} body shape becomes one param `user.email`
// with valueSource="opaque" — exactly the discovery the API-learning
// goal needs. Literal leaves become example values; opaque leaves are
// recorded as attacker-controlled with no synthesized value.
// `location` defaults to "body" for fetch POST bodies; form-driving
// GET passes "query" so the per-field params carry the right wire
// classification (the URL was kept literal because of the opaque-in-
// URL leak; the form's fields flow through the shape regardless of
// GET vs POST, with method choosing the location).
function walkShape(shape, prefix, add, location) {
  const loc = location || "body";
  if (!shape || typeof shape !== "object") return;
  if (shape.kind === "literal") { add(prefix || loc, loc, shape.value, "literal"); return; }
  if (shape.kind === "opaque")  { add(prefix || loc, loc, undefined, "opaque"); return; }
  if (shape.kind === "array" && Array.isArray(shape.items)) {
    for (let i = 0; i < shape.items.length; i++) walkShape(shape.items[i], (prefix ? prefix : loc) + "[" + i + "]", add, loc);
    return;
  }
  const fields = shape.fields;
  if (fields && typeof fields === "object") {
    for (const k of Object.keys(fields)) {
      const child = prefix ? prefix + "." + k : k;
      walkShape(fields[k], child, add, loc);
    }
  }
}
function ep(method, rawUrl, body, shape, headers) {
  let path = rawUrl, qs = "";
  const q = rawUrl.indexOf("?");
  if (q >= 0) { path = rawUrl.slice(0, q); qs = rawUrl.slice(q + 1); }
  const key = method + " " + path;
  let m = methods.get(key);
  if (!m) { m = { method, path, params: new Map(), headers: new Map() }; methods.set(key, m); }
  if (!m.headers) m.headers = new Map();
  function add(name, loc, val, source) {
    let p = m.params.get(name);
    if (!p) { p = { name, location: loc, valueSource: source || "literal", examples: new Set() }; m.params.set(name, p); }
    if (source === "opaque") p.valueSource = "opaque";    // taint wins (TAINT > SYNTH discipline applied to provenance)
    // A "{name}" value is a __feUrlShape opaque marker, never an example.
    if (val !== undefined && val !== "" && !/^\{[^}]*\}$/.test(String(val))) p.examples.add(val);
  }
  // Path-template params from __feUrlShape: opaque path segments render as
  // {name} (e.g. /settings/avatars/{id}). Record each as an opaque path
  // param, no example — the structure is real, the value is attacker/
  // server input the bundle interpolated.
  { const re = /\{([^}\/]+)\}/g; let pm; while ((pm = re.exec(path))) add(pm[1], "path", undefined, "opaque"); }
  // Per-endpoint required-header capture (transport metadata, not body
  // params). The hostedge.js headersShape maps header name → {kind:
  // "literal"|"opaque", value?}. Across forced runs the set grows; a
  // header that opaque-flows on one run and literal-flows on another
  // is recorded with valueSource="opaque" (taint wins) AND its literal
  // example, so the reviewer sees both "this header is required" AND
  // "here is a real value the bundle once sent". Crucial for vendor
  // APIs (Github X-Requested-With, MS Graph Authorization Bearer,
  // Google's X-Goog-* family).
  if (headers && typeof headers === "object") {
    for (const hk of Object.keys(headers)) {
      const hv = headers[hk];
      if (!hv || typeof hv !== "object") continue;
      let h = m.headers.get(hk);
      if (!h) { h = { name: hk, valueSource: hv.kind === "opaque" ? "opaque" : "literal", examples: new Set() }; m.headers.set(hk, h); }
      if (hv.kind === "opaque") h.valueSource = "opaque";
      if (hv.kind === "literal" && hv.value != null && hv.value !== "") h.examples.add(hv.value);
    }
  }
  // Query param valueSource: a "[object Object]" string (opaque coerced
  // via a non-infectious path) OR a "{name}" __feUrlShape template marker
  // (opaque interpolated into the query, e.g. ?z={search}) both mean the
  // value never resolved — drop the example, mark opaque. Never record
  // the marker itself as a literal example.
  for (const [k, vs] of Object.entries(parsePairs(qs)))
    for (const v of vs) {
      const isOpaque = v === "[object Object]" || /^\{[^}]*\}$/.test(v);
      add(k, "query", isOpaque ? undefined : v, isOpaque ? "opaque" : "literal");
    }
  // Structured shape from hostedge.js carries opaque-vs-literal per
  // field for fetch(...,{body:{…}}) call shapes — fully walked. But
  // when a wrapper like jQuery serialized the data to a JSON STRING
  // before calling XHR.send, the shape is {kind:"literal", value:JSON}
  // and the per-field structure lives INSIDE the literal. Detect that
  // case and parse the literal as JSON / form-encoded so per-field
  // params still surface; structured shapes (object/formdata/array)
  // are walked directly. Opaque body stays opaque (no per-field).
  // WHATWG method semantics: GET/HEAD encode params as URL query;
  // POST/PUT/DELETE/PATCH carry them as body. The hostedge.js form-
  // driver passes per-field shape regardless of method (literal URL
  // + structured shape, never opaque stringified into the URL), so
  // the location decision lives here on the consumer side.
  const queryMethod = (method === "GET" || method === "HEAD");
  const fieldLoc = queryMethod ? "query" : "body";
  const structured = shape && shape.kind && shape.kind !== "literal" && shape.kind !== "opaque";
  if (structured) {
    walkShape(shape, "", add, fieldLoc);
  } else if (shape && shape.kind === "opaque") {
    add(fieldLoc, fieldLoc, undefined, "opaque");
  } else {
    const literalStr = (shape && shape.kind === "literal") ? shape.value : (body != null && body !== "" ? body : null);
    if (literalStr != null && literalStr !== "") {
      let bj = null; try { bj = JSON.parse(literalStr); } catch {}
      if (bj && typeof bj === "object") for (const k of Object.keys(bj)) add(k, "body", typeof bj[k] === "object" ? JSON.stringify(bj[k]) : String(bj[k]), "literal");
      else for (const [k, vs] of Object.entries(parsePairs(literalStr))) for (const v of vs) add(k, "body", v, "literal");
    }
  }
}
// Forced-path enumeration. Each run = one coherent forced execution;
// pair open->send WITHIN the run, so every branch's body variant is
// preserved (global dedup of identical opens previously dropped them).
while (work.length) {
  const job = work.shift();
  const sched = typeof job === "string" ? job : job.sched;
  // Pop-time gate: `(key, 1)` already covered → skip (no coverage lost)
  // unless `job.deep` (Quadratic-boost bypass from a fruitful run). NOT
  // gated on a sink having fired — that gating let an API-only page (no
  // @S) run every queued schedule (the X-Force path explosion / OOM).
  if (job && job.key && coveredEdges.has(job.key + ":1")) continue;
  runs++;
  const TRACE = nextTracePath();
  let out = "";
  try {
    out = execFileSync(QJS, [...PRE, "--fe-exec", "--fe-sched=" + sched, "--fe-trace=" + TRACE, ...files], {
      env: process.env,
      encoding: "utf8", timeout: 60000, maxBuffer: 64 * 1024 * 1024,
    });
  } catch (e) { out = (e.stdout || "").toString(); }   // engine may exit nonzero; trace still valid

  const rh = [];                                       // this run's host records, in order
  // Quadratic-boost trigger: any NEW @S sink OR NEW @H call site this
  // run marks it "fruitful" (X-Force §3.2 fitness 2: "if cardinality
  // of [icalls] grows with the currently explored path"). Original
  // paper applies to icalls — observed indirect-call targets — so a
  // new HOST call (fetch/XHR with a previously-unseen call site)
  // qualifies just as a new sink does. Without this, Linear fitness
  // saturates at a few @H records on real-site bundles (github goes
  // hc=3 then stays there forever) because new fetches behind input/
  // click handler gates require deep schedule prefixes the Linear
  // edge-prune won't enqueue. All eligible frontiers from a fruitful
  // run bypass the edge prune (`deepen`) AND the pop-time gate (`deep`).
  let fruitful = false;
  for (const line of out.split("\n")) {
    if (line.startsWith("@S ")) {
      try {
        const r = JSON.parse(line.slice(3));
        const at = (r.at || []).find((a) => String(a.file).indexOf("/h.js") < 0 && String(a.file).indexOf("/d.js") < 0);
        const sk = (r.type || "?") + "|" + (r.sink || "?") + "|" + (at ? at.line + ":" + at.col : "?");
        if (!sinkSeen.has(sk)) { sinkSeen.add(sk); fruitful = true; }
        sinkRecs.set(sk, { type: r.type || "?", sink: r.sink || "?", at: at || null });
      } catch {}
      continue;
    }
    if (line.startsWith("@Z ")) {                      // Z3 path-satisfiability verdict for a tainted sink
      try {
        const r = JSON.parse(line.slice(3));
        zMap.set((r.sink || "?") + "|" + (r.psi || ""), { sink: r.sink, verdict: r.verdict, witness: r.witness });
      } catch {}
      continue;
    }
    if (line.startsWith("@T ")) {                      // JAW static structural site
      try {
        const r = JSON.parse(line.slice(3));
        // Skip @T from hostedge.js / driver.js itself — those are the
        // host-edge shim's OWN fetch/XHR sites (the form-driver, the
        // entry-point loop, the microtask pump's wrappers). They're
        // not bundle endpoints.
        const f = String(r.file || "");
        if (f.indexOf("hostedge.js") >= 0 || f.indexOf("driver.js") >= 0 ||
            f.indexOf("/h.js") >= 0 || f.indexOf("/d.js") >= 0) continue;
        structural.set(r.api + "@" + r.file + ":" + r.line + ":" + r.col,
          { api: r.api, file: r.file, line: r.line, col: r.col, args: r.args || [], url: r.url });
      } catch {}
      continue;
    }
    if (!line.startsWith("@H ")) continue;
    try {
      const r = JSON.parse(line.slice(3));
      rh.push(r);
      const at = (r.at || []).find((a) => String(a.file).indexOf("/h.js") < 0 && String(a.file).indexOf("/d.js") < 0);
      // Fruitfulness on @H is keyed by (api, call site) so the same
      // function called many times from one site doesn't keep boosting;
      // a NEW call site for an api does. distinct still grows by
      // full-line identity (records distinct argument values too).
      const hk = (r.api || "?") + "|" + (at ? at.file + ":" + at.line + ":" + at.col : "?");
      const wasNew = !distinct.has("HK:" + hk);
      distinct.add("HK:" + hk);
      distinct.add(line);
      if (wasNew) fruitful = true;
    } catch {}
  }
  let pend = null;
  for (const r of rh) {
    if (r.api === "XMLHttpRequest.open") { if (pend) ep(pend.m, pend.u, null); pend = { m: r.args[0], u: r.args[1] }; }
    else if (r.api === "XMLHttpRequest.send") { if (pend) { ep(pend.m, pend.u, r.args[0], r.args[1], r.args[2]); pend = null; } }
    else if (r.api === "fetch") ep(r.args[1] || "GET", r.args[0], r.args[2], r.args[3], r.args[4]);
  }
  if (pend) ep(pend.m, pend.u, null);

  if (!existsSync(TRACE)) continue;
  // Trace: "B <ord> <dec> <key>" (executed branch) and "F <ord> <key>"
  // (ran-out frontier, took dec 0). Both Linear (edge prune) and the
  // boost states (Quadratic/bootstrap-Exponential) push frontier
  // flips; the differences are (a) Linear skips when `(key, 1)` is
  // already covered, (b) `deep` jobs bypass the pop-time gate too.
  const decisions = [];
  const frontiers = [];
  for (const ln of readFileSync(TRACE, "utf8").split("\n")) {
    const b = ln.match(/^B (\d+) (\d) (\d+)/);
    if (b) { decisions[+b[1]] = b[2]; coveredEdges.add(b[3] + ":" + b[2]); continue; }
    const f = ln.match(/^F (\d+) (\d+)/);
    if (f) frontiers.push({ i: +f[1], key: f[2] });
  }
  // deepen (push-time): drop the edge-prune so a fruitful run's
  // frontiers are pushed regardless of `(key, 1)` coverage; `deep` on
  // the job also bypasses the pop-time re-check. ONLY on a fruitful run
  // (new endpoint or sink) — NO unbounded bootstrap-Exponential: the
  // `|| sinkSeen.size === 0` term made an API-only page (no @S) deepen
  // every run forever (path explosion / OOM). Fruitful runs are bounded
  // by the distinct endpoint+sink set, so deepening terminates; the rest
  // is Linear edge-coverage (bounded by distinct branch-edges).
  // Pure Linear edge-coverage: a frontier is enqueued ONLY if its flipped
  // edge (key,1) is uncovered — N is bounded by the distinct host-relevant
  // branch-edge set (polynomial), so the BFS terminates and can't OOM.
  // The Quadratic "deepen" (re-enqueue COVERED edges from a fruitful run
  // to chase branch COMBINATIONS) is removed: combination exploration is
  // exponential by nature, and without a cap (forbidden) it explodes — on
  // github the deepen queue grew 46→819 in 20 runs. Per-field value
  // spreads (_gate role/tier) come from each branch's OWN edge, so they
  // survive edge-coverage; the cost is deep multi-condition-gated endpoints
  // (`if(a)if(b)fetch`), which are the inherent X-Force path-explosion
  // limit under no-cap/no-snapshot, not a coverage we silently drop.
  let enq = 0;
  for (const fr of frontiers) {
    if (coveredEdges.has(fr.key + ":1")) continue;
    const ns = decisions.slice(0, fr.i).map((d) => d || "0").join("") + "1";
    if (!seen.has(ns)) { seen.add(ns); work.push({ sched: ns, key: fr.key }); enq++; }
  }
  if (process.env.FE_PROGRESS && runs % (+process.env.FE_PROGRESS || 1) === 0) {
    const sa = out.split("\n").filter((l) => l.startsWith("@S ")).length;
    console.error(`[prog] runs=${runs} sched="${sched}" @S=${sa} work=${work.length} seen=${seen.size} ` +
      `B=${decisions.length} F=${frontiers.length} enq=${enq} hc=${distinct.size}`);
  }
}

console.log("runs=" + runs + " host_calls=" + distinct.size + " endpoints=" + methods.size);
for (const m of methods.values()) {
  const params = [...m.params.values()].map((p) => ({ name: p.name, in: p.location, valueSource: p.valueSource, examples: [...p.examples] }));
  const hdrs = m.headers ? [...m.headers.values()].map((h) => ({ name: h.name, valueSource: h.valueSource, examples: [...h.examples] })) : [];
  let line = m.method + " " + m.path + "  params=" + JSON.stringify(params);
  if (hdrs.length) line += "  headers=" + JSON.stringify(hdrs);
  console.log(line);
}
// JAW hybrid static half: host-edge call sites the dynamic forced runs
// never reached (unrequired modules). Reported as structural
// candidates with value unresolved — never a fabricated value
// (CLAUDE.md: structural learning). Network vs sink split for review.
const NETW = new Set(["fetch", "XMLHttpRequest", "send", "sendBeacon", "WebSocket", "EventSource"]);
const sCand = [...structural.values()];
const sNet = sCand.filter((s) => NETW.has(s.api));
const sSink = sCand.filter((s) => !NETW.has(s.api));
console.log("structural=" + sCand.length + " net=" + sNet.length + " sink=" + sSink.length);
for (const s of sNet)
  console.log("  ~" + s.api + " @ " + s.file + ":" + s.line + ":" + s.col + "  (unreached; value unresolved)");
// A @T sink site is a STATIC candidate; if a sink of the same kind fired a
// tainted @S at runtime it is REACHED — don't mislabel it "unreached".
const reachedSinks = new Set([...sinkRecs.values()].map((x) => x.sink));
for (const s of sSink)
  console.log("  !" + s.api + " @ " + s.file + ":" + s.line + ":" + s.col +
    (reachedSinks.has(s.api) ? "  (static @T candidate; REACHED at runtime — see security_findings)" : "  (static @T candidate, not reached this run)"));
// Security findings = tainted sinks reached at runtime (@S) + their Z3 path-
// satisfiability verdict (@Z). THIS is the security result; a sink listed under
// `structural` above is only a static candidate. (Raw @S/@Z are on qjs stdout.)
const zf = [...zMap.values()];
console.log("security_findings=" + sinkRecs.size + " verdicts=" + zf.length);
for (const v of zf)
  console.log("  [" + (v.verdict || "?") + "] " + (v.sink || "?") +
    (v.witness != null ? "  witness=" + JSON.stringify(String(v.witness)).slice(0, 90) : ""));
