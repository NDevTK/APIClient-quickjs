// In-extension execution model: a fresh wasm Module instance per
// forced schedule (the offscreen doc / sandbox does exactly this — no
// child processes). Files go in MEMFS, FORCED_* via ENV in preRun,
// stdout captured via Module.print, trace read back from MEMFS. Same
// X-Force enumeration + same API-model adapter as drive.mjs. Run here
// via node to prove the model the SW will use is correct.
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const factory = (await import("./qjs_mod.mjs")).default;

const srcFiles = process.argv.slice(2);                 // host paths
const inMem = srcFiles.map((p, i) => ["/f" + i + ".js", readFileSync(p)]);
const fileArgs = inMem.map(([n]) => n);

function parsePairs(s) {
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
const methods = new Map();
function ep(method, rawUrl, body) {
  // Opaque-URL @H carry a non-string url (the [object Object] opaque marker) —
  // a real-bundle reality (learn.microsoft.com). Skip them here (this harness
  // is for schedule-coverage/teardown repro, not endpoint quality); the string
  // guard keeps the BFS+boot-end repro from crashing on the first opaque fetch.
  if (typeof rawUrl !== "string") return;
  let path = rawUrl, qs = "";
  const q = rawUrl.indexOf("?");
  if (q >= 0) { path = rawUrl.slice(0, q); qs = rawUrl.slice(q + 1); }
  const key = method + " " + path;
  let m = methods.get(key);
  if (!m) { m = { method, path, params: new Map() }; methods.set(key, m); }
  const add = (n, loc, val) => {
    let p = m.params.get(n);
    if (!p) { p = { name: n, location: loc, examples: new Set() }; m.params.set(n, p); }
    if (val !== undefined && val !== "") p.examples.add(val);
  };
  for (const [k, vs] of Object.entries(parsePairs(qs))) for (const v of vs) add(k, "query", v);
  if (body != null && body !== "") {
    let bj = null; try { bj = JSON.parse(body); } catch {}
    if (bj && typeof bj === "object") for (const k of Object.keys(bj)) add(k, "body", typeof bj[k] === "object" ? JSON.stringify(bj[k]) : String(bj[k]));
    else for (const [k, vs] of Object.entries(parsePairs(body))) for (const v of vs) add(k, "body", v);
  }
}

const distinct = new Set();
const verdicts = new Map();   // @Z verdict → count (security-finding parity check across FRESH/SNAP)
// X-Force §3.2 Linear Search fitness — the convergence gate drive.mjs and the
// shipped ast-thread.js BFS both use, but mdrive lacked: enqueue a frontier flip
// ONLY if its (branchKey,1) edge is uncovered. Keyed on the F/B record's stable
// branch key (line:col:offset), so N is bounded by DISTINCT branch edges, not by
// schedule strings — without it the schedule space is unbounded on a large bundle
// (work/seen climb without limit; converges only on tiny fixtures by accident).
const coveredEdges = new Set();   // "<branchKey>:<dec>"
// Process one drive's stdout (@H records) + trace (B/F lines) into endpoints +
// the next schedules its frontiers seed. Shared by both execution paths.
function processDrive(stdout, trace) {
  const rh = [];
  for (const line of stdout) {
    if (line.startsWith("@Z ")) {
      try { const z = JSON.parse(line.slice(3)); var k = z.sink + ":" + z.verdict; verdicts.set(k, (verdicts.get(k) || 0) + 1); } catch {}
    }
    if (!line.startsWith("@H ")) continue;
    try { const r = JSON.parse(line.slice(3)); rh.push(r); distinct.add(line); } catch {}
  }
  let pend = null;
  for (const r of rh) {
    if (r.api === "XMLHttpRequest.open") { if (pend) ep(pend.m, pend.u, null); pend = { m: r.args[0], u: r.args[1] }; }
    else if (r.api === "XMLHttpRequest.send") { if (pend) { ep(pend.m, pend.u, r.args[0]); pend = null; } }
    else if (r.api === "fetch") ep(r.args[1] || "GET", r.args[0], r.args[2]);
  }
  if (pend) ep(pend.m, pend.u, null);
  const decisions = [], frontiers = [], next = [];
  for (const ln of trace.split("\n")) {
    // B <cursor> <dec> <key>  — a decision actually taken: mark (key,dec) covered.
    const b = ln.match(/^B (\d+) (\d) (\d+)/); if (b) { decisions[+b[1]] = b[2]; coveredEdges.add(b[3] + ":" + b[2]); continue; }
    // F <cursor> <key> <sig>  — a frontier whose flip would explore (key,1).
    const f = ln.match(/^F (\d+) (\d+)/); if (f) frontiers.push({ i: +f[1], key: f[2] });
  }
  // Linear edge-coverage: seed the flip ONLY if its (key,1) edge is uncovered.
  for (const fr of frontiers) {
    if (coveredEdges.has(fr.key + ":1")) continue;
    next.push(decisions.slice(0, fr.i).map((d) => d || "0").join("") + "1");
  }
  return next;
}

const SNAP = process.env.SNAP === "1";
let runs = 0;
const t0 = Date.now();
let tBoot = t0;
let _boots = 0;   // distinct bootScheds imaged (1 = pure snapshot; >1 = module-init branches re-explored via re-boot)
if (!SNAP) {
  // FRESH: a new wasm instance per schedule (re-boots the bundle every time).
  const work = [""], seen = new Set([""]);
  while (work.length) {
    const sched = work.shift();
    runs++;
    const stdout = [];
    const m = await factory({ noInitialRun: true, print: (s) => stdout.push(s), printErr: () => {} });
    const argv = ["--fe-exec", "--fe-sched=" + sched, "--fe-trace=/t.tr", ...fileArgs];
    for (const [name, data] of inMem) m.FS.writeFile(name, data);
    try { await m.callMain(argv); } catch {}
    let trace = "";
    try { trace = m.FS.readFile("/t.tr", { encoding: "utf8" }); } catch {}
    for (const ns of processDrive(stdout, trace)) if (!seen.has(ns)) { seen.add(ns); work.push(ns); }
  }
} else {
  // SNAPSHOT (Wizer-style): boot the bundle ONCE, image linear memory, then
  // restore + drive per schedule (no per-schedule re-eval) — mirrors the shipped
  // ast-thread.js model. The driver epilogue (last file) is the per-schedule
  // DRIVE; the rest boot the bundle. Opaque branches at MODULE TOP-LEVEL run
  // during the boot eval (before the image), so the snapshot does NOT explore
  // their flip (no re-boot — that was a fallback, not a feature); the boot-trace
  // F count is SURFACED, never silently dropped.
  const bootArgs = fileArgs.slice(0, -1), driveArg = fileArgs[fileArgs.length - 1];
  let curOut = [];
  const m = await factory({ noInitialRun: true, print: (s) => curOut.push(s),
                            printErr: (s) => { if (process.env.MDBG) console.error("[werr] " + s); } });
  for (const [name, data] of inMem) m.FS.writeFile(name, data);
  _boots++;
  try { m.FS.writeFile("/boot.tr", ""); } catch {}
  await m.callMain(["--fe-boot", "--fe-trace=/boot.tr", ...bootArgs]);
  processDrive(curOut, "");                        // module-init @H/@Z, captured once
  let bt = ""; try { bt = m.FS.readFile("/boot.tr", { encoding: "utf8" }); } catch {}
  const _bfrUn = (bt.match(/^F /gm) || []).length;  // module-init branches NOT explored by pure snapshot
  if (_bfrUn) console.log("NOTE: " + _bfrUn + " module-init opaque branch(es) NOT explored (surfaced, not re-booted)");
  const snap = m.HEAPU8.slice();
  tBoot = Date.now();
  const work = [""], seen = new Set([""]);
  while (work.length) {
    const sched = work.shift();
    runs++;
    m.HEAPU8.set(snap);
    try { m.FS.writeFile("/t.tr", ""); } catch {}
    curOut = [];
    try { await m.callMain(["--fe-drive=" + sched, "--fe-trace=/t.tr", driveArg]); } catch {}
    let trace = "";
    try { trace = m.FS.readFile("/t.tr", { encoding: "utf8" }); } catch {}
    for (const ns of processDrive(curOut, trace)) if (!seen.has(ns)) { seen.add(ns); work.push(ns); }
  }
  // BOOTEND=1: exercise the teardown free of g_boot_ctx (the live worker's
  // --fe-boot-end), the deterministic repro for the Chrome-only gc_decref_child
  // UAF abort. Run with MDBG=1 to surface the __assert_fail message (the freed
  // object's gcType/classId) on node stderr.
  if (process.env.BOOTEND === "1") {
    try { await m.callMain(["--fe-boot-end"]); console.log("BOOTEND: clean (no abort)"); }
    catch (e) { console.log("BOOTEND ABORTED: " + (e && e.message || e)); }
  }
}
const tEnd = Date.now();
console.log("runs=" + runs + " host_calls=" + distinct.size + " endpoints=" + methods.size +
  " ms=" + (tEnd - t0) +
  (SNAP ? (" boot=" + (tBoot - t0) + " drive=" + (tEnd - tBoot) + " boots=" + _boots + " [SNAP]") : " [FRESH]"));
if (SNAP && _boots > 1)
  console.log("NOTE: " + _boots + " distinct bootScheds imaged — module-init opaque branches re-explored via re-boot (hybrid), coverage preserved");
if (verdicts.size)
  console.log("verdicts=" + JSON.stringify([...verdicts.entries()].sort().map(function (e) { return e[0] + "×" + e[1]; })));
for (const m of methods.values())
  console.log(m.method + " " + m.path + "  params=" + JSON.stringify(
    [...m.params.values()].map((p) => ({ name: p.name, in: p.location, examples: [...p.examples] }))));
