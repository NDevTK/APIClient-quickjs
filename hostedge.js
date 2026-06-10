// Host-edge model — ONLY the Web boundary. The DOM is the real spec
// Lexbor DOM (qjs_dom.c, installed by qjsmain BEFORE this script):
// `document`/elements are Lexbor nodes with a JS event/classList/
// dataset prelude. This file no longer defines a DOM — it provides
// what Lexbor isn't: opaque/taint host inputs, fetch/XHR/eval
// recording, location/cookie, storage, the event-loop pump, and the
// entry-point driver. Every Web call → @H {api,args,at}; an attacker-
// tainted sink → @S; errors are surfaced (qjsmain @E), never swallowed.
(function () {
  var G = globalThis;
  // Engine stdout print captured before any window stubbing so the
  // @H/@S/@E channel can't be clobbered by window.print().
  var EPRINT = print;
  var OPQ = (typeof __opaque === "function") ? __opaque : function () { return undefined; };
  var ISOPQ = (typeof __isOpaque === "function") ? __isOpaque : function () { return false; };
  // Provenance predicate (TAINT or SYNTH) — header/body field is "an
  // unknown the bundle computed", not a concrete literal. ISOPQ stays
  // TAINT-only for the security sink path; this is for API learning, so a
  // SYNTH static-drive arg flowing into a header/body is also opaque (no
  // fabricated value), matching urlOf's __feUrlShape treatment of URL holes.
  var ISOPQANY = (typeof __isOpaqueAny === "function") ? __isOpaqueAny : ISOPQ;
  // Concrete presence test. `opaque != null` is opaque-infectious (forks), so a
  // shim presence check on a maybe-opaque value (init.body, init.headers,
  // input.url) would fork — and __feDriveStatic's single forced pass takes one
  // arm, dropping the value. An opaque is always present, so short-circuit it
  // first (ISOPQANY is concrete); only a non-opaque reaches the `!= null`.
  var PRESENT = function (x) { return ISOPQANY(x) || x != null; };
  var URLSHAPE = (typeof __feUrlShape === "function") ? __feUrlShape : function () { return undefined; };
  // JSON.stringify({k:opaque,...}) returns an infectious opaque (taint kept for
  // @S/@Z) but stashes the rendered concrete-keyed JSON; OPQSHAPE recovers it so
  // bodyShape can emit the body field KEYS + literal values instead of a bare
  // {kind:opaque}. undefined for a bare/unary opaque (no nested object).
  var OPQSHAPE = (typeof __opaqueShape === "function") ? __opaqueShape : function () { return undefined; };
  // Per-URL-hole generated bundle positions ([{name,line,col}], same order as
  // the __feUrlShape template). Lets the SW pair each opaque path param with
  // its read site and resolve the declared name (e→owner) via a source-map
  // library — the engine supplies the runtime position only it knows.
  var URLHOLES = (typeof __feUrlHoles === "function") ? __feUrlHoles : function () { return undefined; };
  function urlHolesOf(v) {
    try { var s = URLHOLES(v); return (typeof s === "string") ? JSON.parse(s) : null; }
    catch (e) {
      /* __feUrlHoles intrinsic call or JSON.parse of its output failed.
         Without this, per-hole source-map name resolution (e/a/t →
         owner/repo/source) is gone for THIS endpoint. Surface as @WHY
         so a malformed engine emission is visible — silent return null
         here means the popup shows minified `e` labels with no signal
         that name resolution would have run but errored. */
      if (typeof printErr === "function")
        printErr('@WHY {"phase":"urlHolesOf_throw","err":' + JSON.stringify(String(e && e.message || e)) + '}');
      return null;
    }
  }
  // URL for an @H record. When the value is an opaque template (literal
  // segments + interpolated attacker/SYNTH holes — e.g. a `/x/${id}`
  // template literal, which lowers to "/x/".concat(id) and so stays an
  // opaque carrying its concat term), record the structural template
  // `/x/{id}` from that term instead of String(opaque) = "[object
  // Object]". Concrete URLs and fully-opaque URLs (no literal anchor →
  // __feUrlShape returns undefined) fall through to String(); the
  // adapter's isUnresolved files the latter as a resolverError, never a
  // fabricated endpoint.
  function urlOf(v) {
    // A concrete URL OBJECT's .href is its CURRENT serialization, reflecting
    // url.searchParams.set / url.search mutations (the supabase/axios query
    // builder) — prefer it over __feUrlShape, which is the structural/opaque-
    // template shape and drops a query built after construction. Opaque URLs
    // (ISOPQANY) and non-URL inputs keep the shape/String path.
    var s;
    if (v && typeof v === "object" && typeof v.href === "string" && !ISOPQANY(v)) s = v.href;
    else { s = URLSHAPE(v); s = (typeof s === "string") ? s : String(v); }
    // Resolve EVERY concrete fetch/XHR target against the page base (WHATWG
    // URL), exactly as the browser does: "/api/x", "api/x", "../x", "./x",
    // "?q=1", "#h", and "" (fetch(location.hash) → the host page) all become the
    // real request URL — a relative target no longer records a malformed
    // "api/x". The fragment is dropped (never sent to the server). Skips opaque
    // URL TEMPLATES (a "{id}" hole carries "{", which new URL would percent-
    // encode away) and a fully-opaque "[object Object]" (no concrete shape —
    // stays a resolverError, never fabricated into "/[object Object]").
    if (s.indexOf("{") < 0 && s.indexOf("[object Object]") < 0) {
      try { var u = new URL(s, loc._href); return u.origin + u.pathname + u.search; }
      catch (e) {}
    }
    return s;
  }
  /* WHATWG URL constructor preserves opacity. The C-side url_ctor in
     qjs_dom.c calls JS_ToCString on its first argument, which lowers an
     opaque value via the qjs_opq class's Object.prototype.toString to
     "[object Object]" — the URL then parses to `<base>/[object Object]`
     (concrete string). Downstream `fetch(url.href)` records a concrete
     URL with the "[object Object]" marker, the brain's isUnresolved
     catches it and files a resolverError; the bundle's `new URL(t, b)`
     for an opaque `t` (template-literal-built path interpolated with
     attacker/SYNTH input) thus produces NO learnable endpoint shape.
     Wrap the constructor so an opaque first arg is shape-substituted
     via __feUrlShape FIRST — `/api/${id}` (opaque concat term) becomes
     "/api/{id}" before Lexbor parses, and the resulting URL carries the
     real structural template downstream. Falls through to the original
     constructor for concrete args + opaque-without-literal-anchor
     (URLSHAPE returns undefined → still a resolverError, not invented). */
  if (typeof G.URL === "function") {
    var _origURL = G.URL;
    /* Function EXPRESSION (not declaration) so hoisting + strict-mode block-
       scoped semantics don't confuse the closure capture of _origURL. Bundles
       that test `URL.length === 1` or `URL.name === "URL"` keep working because
       the static-property copy below preserves those intrinsics. */
    var URL_wrap = function URL(input, base) {
      if (input && typeof input === "object" && ISOPQANY(input)) {
        var sh = URLSHAPE(input);
        if (typeof sh === "string") input = sh;
      }
      return base !== undefined ? new _origURL(input, base) : new _origURL(input);
    };
    URL_wrap.prototype = _origURL.prototype;
    // Preserve static methods (canParse, createObjectURL, etc.). Some host-
    // bound static descriptors are non-writable; copying via direct property
    // descriptor preserves the original attributes and avoids the silent
    // "this static now missing on the wrapper" gap a bare try/catch would
    // hide. If a specific property can't be redefined, the engine throws —
    // surfacing the host-model gap on stderr instead of dropping the static.
    var _uks = Object.getOwnPropertyNames(_origURL);
    for (var _ui = 0; _ui < _uks.length; _ui++) {
      var _uk = _uks[_ui];
      if (_uk === "prototype" || _uk === "length" || _uk === "name") continue;
      var _ud = Object.getOwnPropertyDescriptor(_origURL, _uk);
      if (_ud) Object.defineProperty(URL_wrap, _uk, _ud);
    }
    G.URL = URL_wrap;
  }
  G.window = G; G.self = G; G.top = G; G.parent = G; G.frames = G; G.globalThis = G;

  // Real page DOM seed: content.js ships the server-rendered HTML as
  // self.__pageHtml (via /pre.js generated by ast-thread.js). Lexbor
  // re-parses it into g_doc via document.__feLoadPage, replacing the
  // minimal seed installed by qjs_dom.c at boot. Bundles whose data
  // fetches sit behind customElements.connectedCallback (Catalyst,
  // Turbo, React Server Components) or behind querySelector results
  // on the SSR HTML only become reachable after this — without it
  // github boots with zero @H because no <react-app>/<turbo-frame>
  // is present for the bundle's CE classes to upgrade. Diagnosed
  // independent of the forced-exec hang on github's 6.3MB bundle —
  // that hang reproduces without __feLoadPage too, so this binding is
  // not the culprit.
  try {
    var _ph = G.__pageHtml;
    if (typeof _ph === "string" && _ph.length > 0 &&
        typeof document !== "undefined" &&
        typeof document.__feLoadPage === "function") {
      document.__feLoadPage(_ph);
    }
  } catch (e) {
    // NO SILENT FAILURE: a throw here zeroes the DOM-gated surface (CE
    // connectedCallback / querySelector-on-SSR fetches — the moat) with no @H;
    // surface it so a 0-endpoint run carries a reason instead of looking clean.
    if (typeof printErr === "function") printErr('@WHY {"phase":"feLoadPage_throw","len":' + ((G.__pageHtml && G.__pageHtml.length) | 0) + ',"err":' + JSON.stringify(String(e && e.message || e)) + '}');
  }

  function siteOf() {
    var s; try { s = new Error().stack || ""; } catch (e) { return null; }
    var ls = s.split("\n"), fr = [];
    for (var i = 0; i < ls.length; i++) {
      // QuickJS emits `    at <fn> (file:line:col)`. Capture the function name
      // too (optional group) so the interprocedural call chain is readable —
      // a frame is which FUNCTION the taint passed through, not just a L:C.
      var m = ls[i].match(/at\s+(.*?)\s*\(([^()]+):(\d+):(\d+)\)\s*$/);
      if (!m) { m = ls[i].match(/\(([^()]+):(\d+):(\d+)\)\s*$/); if (m) m = [m[0], "", m[1], m[2], m[3]]; }
      if (!m) continue;
      var f = m[2];
      if (f.indexOf("/h.js") >= 0 || f.indexOf("/d.js") >= 0 ||
          f.indexOf("/p.js") >= 0 || f.indexOf("qjs-dom-prelude") >= 0) continue;
      var nm = (m[1] || "").trim();
      if (nm === "<anonymous>" || nm === "<eval>") nm = "";
      fr.push({ file: f, line: +m[3], col: +m[4], name: nm || undefined });
    }
    return fr.length ? fr : null;
  }
  function deepConcretize(v) {
    if (ISOPQANY(v)) return "[object Object]";
    if (v && typeof v === "object") {
      if (Array.isArray(v)) { var a = []; for (var i = 0; i < v.length; i++) a.push(deepConcretize(v[i])); return a; }
      var o = {}; for (var k in v) { try { o[k] = deepConcretize(v[k]); } catch (e) {} } return o;
    }
    return v;
  }
  function H(api, args) {
    var rec = { api: api, args: args, at: siteOf() };
    var s;
    try { s = JSON.stringify(rec); } catch (e) { s = null; }
    // String()/JSON.stringify are opaque-infectious, so a fully-opaque arg (a
    // URL the drive couldn't ground to a literal) makes the WHOLE "@H {…}"
    // string opaque → it renders "[object Object]" and the worker DROPS it (no
    // @H prefix, no resolverError). Per "No silent failure" + "a fully-opaque
    // URL is a resolverError, never dropped", concretize opaque leaves to the
    // "[object Object]" marker (as S() does for @S) and re-emit, so the call
    // site surfaces as a resolverError instead of vanishing. ISOPQANY is a
    // concrete bool ⇒ no fork, and is tested FIRST so `s == null` never
    // compares an opaque (which would fork).
    if (ISOPQANY(s) || s == null) {
      try { s = JSON.stringify(deepConcretize(rec)); } catch (e2) { return; }
    }
    if (!_hSeen.has(s)) { _hSeen.add(s); _hProg++; }
    EPRINT("@H " + s);
  }
  function tainted(v) {
    if (ISOPQ(v)) return true;
    return typeof v === "string" && v.indexOf("[object Object]") >= 0;
  }
  var FESEC = (typeof __feSec === "function") ? __feSec : null;
  var MUTE = (typeof __feMute === "function") ? __feMute : function () {};
  var PHIMUTE = (typeof __fePhiMute === "function") ? __fePhiMute : function () {};
  function S(type, sink, value, extra) {
    if (!tainted(value)) return;
    // Mute Φ-push (NOT schedule seeding) around the @S record body:
    // the bundle's gates already pushed; hostedge's own internal
    // checks (typeof / null / String coercion) are bookkeeping and
    // would pollute Φ with predicates that are not source→sink flow.
    // Schedule frontier-seeding stays active so X-Force still
    // discovers new branches reached from sinks.
    PHIMUTE(1);
    var verdictWritten = false;
    try {
      // The @S record DESCRIBES a tainted value, so it must serialize
      // CONCRETELY. String()/JSON.stringify are opaque-infectious (an
      // opaque operand yields an opaque result — required so API-learning
      // keeps `/api/${String(id)}` as a shape, not a baked literal), so
      // String(opaque) now returns an OPAQUE; left raw it makes the whole
      // `@S {…}` string opaque and the record is silently dropped (and
      // EPRINT of an opaque throws before __feSec, killing @Z too).
      // Concretize to the historical "[object Object]" marker — the real
      // witness is the @Z Z3 model. ISOPQ is a concrete bool ⇒ no fork.
      var rec = { type: type, sink: sink, tainted: true,
        value: (ISOPQ(value) ? "[object Object]" : String(value)).slice(0, 256), at: siteOf() };
      if (extra) for (var k in extra) rec[k] = extra[k];
      EPRINT("@S " + JSON.stringify(rec));
      // Z3 path-satisfiability verdict: REAL_EXPLOIT / TAINT_REACH /
      // INFEASIBLE. @Z carries the witness (concrete attacker string
      // Z3 synthesized) on REAL_EXPLOIT; INFEASIBLE means X-Force
      // forced a path the gates reject — drive.mjs / ast-thread
      // suppress those (the false-finding filter).
      if (FESEC) { try { FESEC(value, sink); verdictWritten = true; } catch (e) {} }
    } finally { PHIMUTE(0); }
    return verdictWritten;
  }
  G.__H = H; G.__S = S;
  function defAccessor(o, name, get, set) {
    try { Object.defineProperty(o, name, { get: get, set: set, configurable: true }); } catch (e) {}
  }

  // ── event-loop pump (source-identity dedup → finite fixpoint) ─────
  var TQ = [];
  var ranKeys = new Set();
  var _enqSeen = new Set();        // IDENTITY dedup: each distinct callback object enqueues once
  // Timer-spinner DEFER (the across-flush analogue of the per-flush 512 idle break):
  // a re-queuing task that schedules a NEW closure each tick — setTimeout(()=>tick(),0)
  // inside tick — has a new object identity every time, so _enqSeen never dedups it and
  // it churns the pump (github round-1: 1278 flushes × up to 512 drains = ~650k wasted
  // drains, so round-1 never completes and the deep round never starts). Key by SOURCE
  // TEXT (fn.toString(), identical across the spinner's ticks); once a key has run K
  // times WITHOUT advancing host-progress (_hProg / @H), it is a proven NO-OUTPUT spinner
  // — defer future enqueues of it. Safe: a no-@H-progress task produces no endpoints, so
  // dropping it loses nothing (the starve-don't-bound principle); a legit same-source loop
  // (for(i)setTimeout(()=>f(i))) whose f DOES emit @H keeps resetting its stale count and
  // is never deferred. NOT the per-flush cap (which truncates late progress).
  var _spinKeys = new Set();       // source-text keys proven to be no-output spinners
  var _taskProg = new Map();       // source-text key -> { h:_hProg at last progress, s:stale-run count }
  var _SPIN_K = 64;                // K no-@H-progress runs of a key => defer it
  var _hProg = 0;                  // monotone progress: count of DISTINCT @H host edges emitted (see H)
  var _hSeen = new Set();
  var fnsrc = Function.prototype.toString;
  function keyOf(fn) { try { return fnsrc.call(fn); } catch (e) { return String(fn); } }
  // Timer dedup is by IDENTITY (each distinct callback object runs once). The old
  // source-keyed dedup wrongly dropped every distinct callback sharing a source -- the
  // native Promise resolve from new Promise(r=>setTimeout(r,0)) that include-fragment and
  // most await->macrotask code defer their fetch behind (all "[native code]") -- losing
  // every CE/timer instance fetch but the first, and each iteration of an await-loop over
  // DISTINCT endpoints. The drain (__hostFlush) additionally stops once a stretch is
  // genuinely dry (no new @H): a no-progress fixpoint that bounds distinct-object spin
  // loops WITHOUT dropping productive ones. No depth/step cap -- progress resets dry.
  function enq(fn) {
    if (typeof fn !== "function" || _enqSeen.has(fn)) return;
    try { if (_spinKeys.has(fn.toString())) return; } catch (e) {}   // defer a proven no-output spinner
    _enqSeen.add(fn); TQ.push(fn);
  }
  var _tid = 0;
  G.setTimeout = function (f) { if (typeof f === "string") { S("code-exec", "setTimeout", f); return ++_tid; } enq(f); return ++_tid; };
  G.setInterval = function (f) { if (typeof f === "string") { S("code-exec", "setInterval", f); return ++_tid; } enq(f); return ++_tid; };
  G.requestAnimationFrame = function (f) { enq(f); return ++_tid; };
  G.requestIdleCallback = function (f) { enq(typeof f === "function" ? function () { f({ timeRemaining: function () { return 0; }, didTimeout: false }); } : f); return ++_tid; };
  G.queueMicrotask = function (f) { enq(f); };
  G.clearTimeout = G.clearInterval = G.cancelAnimationFrame = G.cancelIdleCallback = function () {};
  G.__hostRan = function () { return ranKeys.size; };
  // Distinct @H host edges emitted so far (monotone). The driver pump loops while
  // this grows so an await->macrotask loop over DISTINCT endpoints (one async fn,
  // so __hostRan is flat) still drains every iteration's timer. Flat = no new
  // endpoint this cycle (a same-endpoint poll loop) -> the pump can quiesce.
  G.__feHProg = function () { return _hProg; };

  // ── events factory ────────────────────────────────────────────────
  function mkEvent(type, init) {
    init = init || {};
    var ev = {
      type: String(type), bubbles: init.bubbles !== false, cancelable: !!init.cancelable,
      defaultPrevented: false, target: null, currentTarget: null, srcElement: null,
      eventPhase: 0, isTrusted: false, timeStamp: 0, _stop: false, _s: false,
      composed: !!init.composed, detail: init.detail != null ? init.detail : null,
      preventDefault: function () { if (this.cancelable) this.defaultPrevented = true; },
      stopPropagation: function () { this._stop = true; this._s = true; },
      stopImmediatePropagation: function () { this._stop = true; this._s = true; },
      composedPath: function () { return []; },
      initEvent: function (t, b, c) { this.type = t; this.bubbles = b; this.cancelable = c; },
      initCustomEvent: function (t, b, c, d) { this.type = t; this.bubbles = b; this.cancelable = c; this.detail = d; },
    };
    for (var k in init) if (!(k in ev)) ev[k] = init[k];
    return ev;
  }
  function mkMessageEvent() { var e = mkEvent("message", { bubbles: false }); e.data = OPQ("postMessage.data"); e.origin = OPQ("postMessage.origin"); e.lastEventId = ""; e.source = G; e.ports = []; return e; }
  // Event for driving a registered element handler. target/currentTarget
  // are the REAL Lexbor registration element so the handler's DOM-derived
  // request parts — event.currentTarget.dataset.url, event.target
  // .getAttribute("data-id"), event.target.closest("form").action —
  // resolve to CONCRETE server-rendered attribute values (the example
  // usages the bundle would actually send). Only the genuine attacker
  // surface stays opaque: .data (postMessage / CustomEvent payload) and
  // .detail. Element attributes are server-controlled, not attacker
  // input, so concrete is correct for both the API view (real example)
  // and the security view (not a taint source).
  function mkHandlerEvent(target) {
    var e = mkEvent("", { bubbles: true, cancelable: true });
    e.isTrusted = true;
    if (target != null) { e.target = target; e.currentTarget = target; e.srcElement = target; }
    e.data = OPQ("handler.event.data");
    e.detail = OPQ("handler.event.detail");
    return e;
  }
  G.Event = function (t, i) { return mkEvent(t, i); };
  G.CustomEvent = function (t, i) { var e = mkEvent(t, i); if (i && "detail" in i) e.detail = i.detail; return e; };
  G.MouseEvent = G.PointerEvent = G.KeyboardEvent = G.InputEvent = G.TouchEvent = G.FocusEvent = G.WheelEvent = G.UIEvent = G.DragEvent = function (t, i) { return mkEvent(t, i); };
  G.MessageEvent = function () { return mkMessageEvent(); };
  G.PopStateEvent = function (t, i) { var e = mkEvent(t, i); e.state = i && i.state != null ? i.state : null; return e; };
  G.HashChangeEvent = function (t, i) { return mkEvent(t, i); };
  G.ErrorEvent = function (t, i) { return mkEvent(t, i); };
  G.PromiseRejectionEvent = function (t, i) { var e = mkEvent(t, i); e.reason = i && i.reason; e.promise = i && i.promise; return e; };
  G.EventTarget = function () { return G.document ? G.document.createElement("div") : {}; };
  G.AbortController = function () { var sig = { aborted: false, reason: undefined, _ev: { abort: [] }, addEventListener: function (t, f) { (this._ev[t] || (this._ev[t] = [])).push(f); }, removeEventListener: function () {}, dispatchEvent: function () { return true; }, onabort: null, throwIfAborted: function () {} }; this.signal = sig; this.abort = function (r) { sig.aborted = true; sig.reason = r; (sig._ev.abort || []).forEach(function (f) { try { f({ type: "abort" }); } catch (e) {} }); if (typeof sig.onabort === "function") try { sig.onabort({ type: "abort" }); } catch (e) {} }; };
  G.AbortSignal = { timeout: function () { return new G.AbortController().signal; }, abort: function () { var c = new G.AbortController(); c.abort(); return c.signal; } };

  // ── window-level events (globalThis is not a Lexbor node) ─────────
  // J-Force §3.1.3: every function registered as an event handler is
  // force-executed without its triggering event, as a separately
  // explored block (deduped by source identity — same `ranKeys`
  // model). FH is the unified driver-visible sink: window handlers
  // (here), element/document handlers (qjs_dom prelude calls
  // __feHandler), and on* props all funnel in, so the entry-point
  // driver reaches handler-hidden fetch/XHR call sites that no page
  // event would trigger at analysis time.
  // FH entries are {fn, target}. target preserves the registration
  // element so `this` inside the handler resolves to the real CE
  // element when __hostDrive fires it — without that, `this.src` /
  // `this.dataset.someApiPath` return opaque instead of the
  // concrete parsed-attribute value, and any `fetch(this.src + …)`
  // inside the handler emits @H with an opaque URL marker. CE-
  // bound handlers come from qjs_dom.c's Element.prototype.
  // addEventListener which now passes `this` as the second arg;
  // window-level handlers (G.addEventListener) pass null → __hostDrive
  // falls back to G as `this`. WHATWG event-listener "this" semantics.
  var FH = [];
  G.__feHandler = function (f, target) {
    if (typeof f !== "function" || ranKeys.has(keyOf(f))) return;
    // Tag the handler so the deep-grind drives its event arg as attacker TAINT
    // (not SYNTH) — a reached sink becomes a real solvable exploit/PoC.
    try { if (typeof __feMarkHandler === "function") __feMarkHandler(f); } catch (e) {}
    FH.push({ fn: f, target: target == null ? null : target });
  };
  var WLIS = Object.create(null);
  G.addEventListener = function (t, f, o) { if (typeof f === "function") { (WLIS[t] || (WLIS[t] = [])).push(f); G.__feHandler(f); } };
  G.removeEventListener = function (t, f) { var a = WLIS[t]; if (a) WLIS[t] = a.filter(function (x) { return x !== f; }); };
  G.dispatchEvent = function (ev) { var a = WLIS[ev && ev.type]; if (a) for (var i = 0; i < a.length; i++) { var k = keyOf(a[i]); if (ranKeys.has(k)) continue; try { a[i].call(G, ev); } catch (e) {} } return true; };
  ["load", "DOMContentLoaded", "message", "popstate", "hashchange", "resize", "scroll", "error", "unhandledrejection", "beforeunload", "pageshow", "online", "offline", "readystatechange"].forEach(function (t) { defAccessor(G, "on" + t, function () { return (WLIS[t] && WLIS[t]._on) || null; }, function (v) { (WLIS[t] || (WLIS[t] = []))._on = v; if (typeof v === "function") { WLIS[t].push(v); G.__feHandler(v); } }); });
  G.postMessage = function (m, o) { H("postMessage", [m == null ? null : (typeof m === "object" ? JSON.stringify(m) : String(m)), o == null ? null : String(o)]); };

  // Forced-exec coverage for PROMISE REACTIONS. A .then/.catch/.finally callback
  // registered on a promise that never SETTLES during the analysis is un-fired
  // code the residue does NOT reach — it was consumed by .then (a promise
  // reaction), not left a free orphan, and forced exec has no event to fire it.
  // The canonical case: a webpack JSONP chunk-load Promise, `new Promise(r => {
  // script.onload = r })`, whose load event the analysis cannot faithfully fire
  // (the dynamically-injected <script>'s onload is not engine-readable at the
  // deferred load job) — so `.then(() => init())` never runs, the lazy
  // Sentry.init / Apollo client never builds its configured transport, and the
  // envelope + /api/graphql URLs stay opaque (send()/request() only ever drive
  // COLD). Funnel the callbacks into the SAME __feHandler sink as event handlers
  // so the entry-point driver runs the continuation as an orphan (reading the
  // page's concrete `gon`/state -> building the real instance -> concrete URL).
  // ranKeys dedups, so a callback that ALSO settles for real is not re-driven;
  // the original then/catch/finally still runs for naturally-settling promises
  // (transparent delegation). Only an explicit .then is hooked here — engine-side
  // `await` continuations need the OP_await path; this covers the .then form
  // (webpack chunk loads, SDK init chains) that is the bulk of the moat surface.
  (function () {
    if (!G.Promise || !G.Promise.prototype) return;
    var PP = G.Promise.prototype;
    var _then = PP.then;
    if (typeof _then === "function") {
      PP.then = function (onF, onR) {
        try { if (typeof onF === "function" && G.__feHandler) G.__feHandler(onF, null); } catch (e) {}
        try { if (typeof onR === "function" && G.__feHandler) G.__feHandler(onR, null); } catch (e) {}
        return _then.call(this, onF, onR);
      };
    }
    var _catch = PP.catch;
    if (typeof _catch === "function") {
      PP.catch = function (onR) {
        try { if (typeof onR === "function" && G.__feHandler) G.__feHandler(onR, null); } catch (e) {}
        return _catch.call(this, onR);
      };
    }
    var _finally = PP.finally;
    if (typeof _finally === "function") {
      PP.finally = function (onFin) {
        try { if (typeof onFin === "function" && G.__feHandler) G.__feHandler(onFin, null); } catch (e) {}
        return _finally.call(this, onFin);
      };
    }
  })();

  // ── location: real page URL when content.js shipped it (G.__pageUrl,
  // parsed via the WHATWG URL bound to Lexbor). Structure concrete so
  // origin-gated init runs and same-origin URLs the bundle builds from
  // location.origin/host resolve to the actual site, not a placeholder
  // — github routes the landing vs app bundle on location.hostname, and
  // `fetch(location.origin + "/api/...")` is wrong against example.com.
  // hash/search carry their CONCRETE value (the page's real hash/search, from
  // the parsed page URL) as the opaque's example shape AND keep the attacker-
  // source leaf term: API learning resolves fetch(location.hash) to a concrete
  // endpoint (never a resolverError — a resolverError on a drivable value is a
  // gap, not acceptable), while Z3/security still treats them as the attacker-
  // controlled navigation surface. Concrete-for-learning + attacker-for-Z3 on
  // one value (qjs_opq carries both term and shape).
  var _pageUrl = (typeof G.__pageUrl === "string" && G.__pageUrl) ? G.__pageUrl : "https://example.com/";
  var _pu = null; try { _pu = new URL(_pageUrl); } catch (e) { _pu = null; }
  var loc = { origin: _pu ? _pu.origin : "https://example.com",
    protocol: _pu ? _pu.protocol : "https:", host: _pu ? _pu.host : "example.com",
    hostname: _pu ? _pu.hostname : "example.com", port: _pu ? _pu.port : "",
    pathname: _pu ? _pu.pathname : "/", _href: _pu ? _pu.href : "https://example.com/",
    assign: function (u) { S("open-redirect", "location.assign", u); },
    replace: function (u) { S("open-redirect", "location.replace", u); },
    reload: function () {}, toString: function () { return this._href; },
    ancestorOrigins: { length: 0, item: function () { return null; }, contains: function () { return false; } } };
  defAccessor(loc, "href", function () { return this._href; }, function (v) { S("open-redirect", "location.href", v); });
  defAccessor(loc, "hash", function () { return OPQ("location.hash", _pu ? _pu.hash : ""); }, function (v) { S("open-redirect", "location.hash", v); });
  defAccessor(loc, "search", function () { return OPQ("location.search", _pu ? _pu.search : ""); }, function (v) { S("open-redirect", "location.search", v); });
  defAccessor(G, "location", function () { return loc; }, function (v) { S("open-redirect", "window.location", v); });
  defAccessor(G, "name", function () { return OPQ("window.name"); }, function () {});

  // ── document data-props onto the REAL Lexbor document ─────────────
  if (typeof document !== "undefined" && document) {
    defAccessor(document, "cookie", function () { return OPQ("document.cookie"); }, function (v) { S("dom-attr", "document.cookie", v, { attr: "cookie" }); });
    defAccessor(document, "location", function () { return loc; }, function (v) { S("open-redirect", "document.location", v); });
    defAccessor(document, "URL", function () { return loc._href; });
    defAccessor(document, "documentURI", function () { return loc._href; });
    defAccessor(document, "baseURI", function () { return loc._href; });
    defAccessor(document, "domain", function () { return loc.hostname; }, function () {});
    try { document.referrer = ""; } catch (e) {}
    // DOM-HTML / script-attr XSS sinks: Lexbor's binding does the real
    // parse/mutation; wrap its prototype setters so an attacker-tainted
    // value still records an @S (concrete framework writes don't — S()
    // is taint-gated, so jQuery's feature-detect innerHTML stays B=0).
    var P = Object.getPrototypeOf(document);
    if (P) {
      ["innerHTML", "outerHTML"].forEach(function (prop) {
        var d = Object.getOwnPropertyDescriptor(P, prop);
        if (!d || !d.set) return;
        var oset = d.set, oget = d.get;
        defAccessor(P, prop, oget, function (v) { S("dom-html", prop, v); return oset.call(this, v); });
      });
      var iah = P.insertAdjacentHTML;
      if (typeof iah === "function") P.insertAdjacentHTML = function (pos, html) { S("dom-html", "insertAdjacentHTML", html, { position: String(pos) }); return iah.call(this, pos, html); };
      var sa = P.setAttribute;
      if (typeof sa === "function") P.setAttribute = function (nm, vv) {
        var ln = String(nm).toLowerCase();
        if (/^on/.test(ln) || ln === "href" || ln === "src" || ln === "formaction" || ln === "srcdoc" || ln === "xlink:href" || ln === "action")
          S("dom-attr", "setAttribute(" + ln + ")", vv, { attr: ln });
        return sa.call(this, nm, vv);
      };
    }
    document.write = function (s) { S("dom-html", "document.write", s); };
    document.writeln = function (s) { S("dom-html", "document.write", s); };
    // Lazy-chunk / dynamic-script discovery: a code-splitter loads a chunk
    // by creating a <script> and assigning its src to the chunk URL (webpack
    // b.l does `d.src=e`). Record that as a @H "script" host edge so the SW
    // can fetch the chunk and feed it back through the SAME analyzer — the
    // unused-feature API surface (GraphQL persisted queries, lazily-imported
    // controllers) lives in those chunks, which is where most of a complex
    // app's endpoints are. Engine-grounded: we record exactly the URL the
    // bundle's OWN loader requests, not a manifest regex. The src getter
    // returns the stored value because the loader reads it back
    // (`d.src.indexOf(window.location.origin)`).
    if (typeof document.createElement === "function") {
      var _feOrigCE = document.createElement.bind(document);
      document.createElement = function (tag) {
        var elx = _feOrigCE(tag);
        try {
          if (elx && String(tag).toLowerCase() === "script") {
            var _feSrc = "";
            defAccessor(elx, "src", function () { return _feSrc; }, function (v) {
              _feSrc = String(v == null ? "" : v);
              if (_feSrc) H("script", [urlOf(_feSrc), "GET"]);
            });
          }
        } catch (e) {}
        return elx;
      };
    }
  }


  // Intl: QuickJS-ng ships no ICU, so Intl is absent — github (and most
  // vendor bundles) read Intl.DateTimeFormat / NumberFormat at module top
  // level, so a missing Intl throws ReferenceError that aborts the WHOLE
  // bundle (the home page dodged it; repo pages don't). Record-only
  // analyzer-boundary shim (same class as the fetch/XHR stubs): the
  // constructors return objects whose format()/of() yield OPQ — an unknown
  // formatted value (structural-learning correct, never a fabricated date/
  // number) — so the bundle runs through to its host-edge calls.
  (function () {
    function fmtObj() {
      return { format: function () { return OPQ("Intl.format"); }, formatToParts: function () { return []; },
        formatRange: function () { return OPQ("Intl.formatRange"); }, formatRangeToParts: function () { return []; },
        resolvedOptions: function () { return { locale: "en-US" }; } };
    }
    G.Intl = {
      DateTimeFormat: function () { return fmtObj(); },
      NumberFormat: function () { return fmtObj(); },
      RelativeTimeFormat: function () { return fmtObj(); },
      ListFormat: function () { return fmtObj(); },
      Collator: function () { return { compare: function () { return 0; }, resolvedOptions: function () { return { locale: "en-US" }; } }; },
      PluralRules: function () { return { select: function () { return "other"; }, resolvedOptions: function () { return { locale: "en-US" }; } }; },
      DisplayNames: function () { return { of: function () { return OPQ("Intl.DisplayNames"); }, resolvedOptions: function () { return { locale: "en-US" }; } }; },
      Segmenter: function () { return { segment: function () { return []; }, resolvedOptions: function () { return { locale: "en-US" }; } }; },
      getCanonicalLocales: function (x) { return x == null ? [] : (Array.isArray(x) ? x.slice() : [String(x)]); },
      supportedValuesOf: function () { return []; },
    };
  })();
  G.history = { length: 1, scrollRestoration: "auto", state: null,
    pushState: function (s, t, u) { this.state = s; if (u != null) H("history.pushState", [String(u)]); },
    replaceState: function (s, t, u) { this.state = s; if (u != null) H("history.replaceState", [String(u)]); },
    back: function () {}, forward: function () {}, go: function () {} };
  G.navigator = {
    userAgent: "Mozilla/5.0 (forced-exec) QuickJS", appVersion: "5.0", platform: "",
    language: "en-US", languages: ["en-US", "en"], onLine: true, cookieEnabled: true,
    doNotTrack: null, hardwareConcurrency: 4, deviceMemory: 8, maxTouchPoints: 0,
    vendor: "", product: "Gecko", productSub: "20030107", appName: "Netscape",
    sendBeacon: function (u, d) { H("sendBeacon", [urlOf(u), recBody(d), bodyShape(d)]); return true; },
    clipboard: { writeText: function () { return Promise.resolve(); }, readText: function () { return Promise.resolve(OPQ("clipboard.readText")); } },
    serviceWorker: { register: function () { return Promise.resolve({ scope: "/", update: function () {}, unregister: function () { return Promise.resolve(true); }, addEventListener: function () {} }); }, ready: Promise.resolve({ active: {} }), addEventListener: function () {}, controller: null, getRegistrations: function () { return Promise.resolve([]); } },
    permissions: { query: function () { return Promise.resolve({ state: "prompt", addEventListener: function () {} }); } },
    geolocation: { getCurrentPosition: function () {}, watchPosition: function () { return 0; }, clearWatch: function () {} },
    mediaDevices: { getUserMedia: function () { return Promise.reject(new Error("denied")); }, enumerateDevices: function () { return Promise.resolve([]); } },
    storage: { estimate: function () { return Promise.resolve({ usage: 0, quota: 0 }); }, persisted: function () { return Promise.resolve(false); } },
    connection: { effectiveType: "4g", downlink: 10, rtt: 50, saveData: false, addEventListener: function () {} },
    userAgentData: { brands: [], mobile: false, platform: "", getHighEntropyValues: function () { return Promise.resolve({}); } },
    credentials: { get: function () { return Promise.resolve(null); }, store: function () { return Promise.resolve(); } },
    requestMIDIAccess: function () { return Promise.reject(new Error("no")); },
    getBattery: function () { return Promise.resolve({ level: 1, charging: true, addEventListener: function () {} }); },
    vibrate: function () { return true; }, registerProtocolHandler: function () {},
  };
  G.screen = { width: 1280, height: 800, availWidth: 1280, availHeight: 800, colorDepth: 24, pixelDepth: 24, orientation: { type: "landscape-primary", angle: 0, addEventListener: function () {} } };
  G.innerWidth = 1280; G.innerHeight = 800; G.outerWidth = 1280; G.outerHeight = 800;
  G.pageXOffset = 0; G.pageYOffset = 0; G.scrollX = 0; G.scrollY = 0; G.devicePixelRatio = 1;
  // Nondeterministic host sources → SYNTH-opaque (NOT a fake literal): same
  // class as the engine's Math.random/Date.now. A real/fixed value would bake a
  // client nonce into a learned endpoint (`?t=performance.now()` / request-id =
  // crypto.randomUUID()); opaque makes the derived param a dynamic-nonce shape.
  var SYN = (typeof __synth === "function") ? __synth : function () { return 0; };
  G.performance = {
    now: function () { return SYN("performance.now"); }, timeOrigin: 0,
    mark: function () {}, measure: function () {}, clearMarks: function () {}, clearMeasures: function () {},
    getEntries: function () { return []; }, getEntriesByType: function () { return []; }, getEntriesByName: function () { return []; },
    navigation: { type: 0 }, timing: {}, memory: { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 0 },
  };
  G.crypto = G.crypto || {};
  if (!G.crypto.getRandomValues) G.crypto.getRandomValues = function (a) { for (var i = 0; i < (a ? a.length : 0); i++) a[i] = 0; return a; };
  if (!G.crypto.randomUUID) G.crypto.randomUUID = function () { return SYN("crypto.randomUUID"); };
  G.crypto.subtle = G.crypto.subtle || { digest: function () { return Promise.resolve(new ArrayBuffer(0)); }, encrypt: function () { return Promise.resolve(new ArrayBuffer(0)); }, decrypt: function () { return Promise.resolve(new ArrayBuffer(0)); }, sign: function () { return Promise.resolve(new ArrayBuffer(0)); }, verify: function () { return Promise.resolve(true); }, generateKey: function () { return Promise.resolve({}); }, importKey: function () { return Promise.resolve({}); }, exportKey: function () { return Promise.resolve(new ArrayBuffer(0)); }, deriveKey: function () { return Promise.resolve({}); }, deriveBits: function () { return Promise.resolve(new ArrayBuffer(0)); } };
  G.CSS = { supports: function () { return false; }, escape: function (s) { return String(s); } };
  G.Notification = function () {}; G.Notification.permission = "default"; G.Notification.requestPermission = function () { return Promise.resolve("default"); };
  G.indexedDB = { open: function () { var r = { onsuccess: null, onerror: null, onupgradeneeded: null, result: { objectStoreNames: [], transaction: function () { return { objectStore: function () { return { get: function () { return {}; }, put: function () {}, add: function () {}, delete: function () {} }; } }; }, createObjectStore: function () { return {}; }, close: function () {} } }; enq(function () { if (typeof r.onsuccess === "function") r.onsuccess({ target: r }); }); return r; }, deleteDatabase: function () { return {}; } };
  G.caches = { open: function () { return Promise.resolve({ match: function () { return Promise.resolve(undefined); }, put: function () { return Promise.resolve(); }, add: function () { return Promise.resolve(); }, keys: function () { return Promise.resolve([]); }, delete: function () { return Promise.resolve(false); } }); }, match: function () { return Promise.resolve(undefined); }, has: function () { return Promise.resolve(false); }, keys: function () { return Promise.resolve([]); }, delete: function () { return Promise.resolve(false); } };
  G.matchMedia = function (q) { return { matches: false, media: String(q || ""), onchange: null, addListener: function () {}, removeListener: function () {}, addEventListener: function () {}, removeEventListener: function () {}, dispatchEvent: function () { return true; } }; };
  G.getComputedStyle = function (el) { return (el && el.style) ? el.style : { getPropertyValue: function () { return ""; }, cssText: "" }; };
  // Observer callbacks (IntersectionObserver / ResizeObserver /
  // MutationObserver / PerformanceObserver / ReportingObserver) gate
  // most lazy-load fetches on real production sites. Without a layout
  // engine the visibility/intersection/resize state can't be computed,
  // so the host-edge surface treats those entry fields as OPAQUE
  // (attacker-controlled unknowns). The .target IS real (the element
  // the bundle handed to observe()); everything else (intersectionRatio,
  // isIntersecting, contentRect, boundingClientRect, intersectionRect,
  // rootBounds, time, addedNodes, removedNodes, attributeName,
  // oldValue) is opaque so any predicate the bundle writes on those
  // values FORKS under forced exec — same discipline as location.hash
  // / responseText / cookie. __feHandler hooks the callback so the
  // entry-point driver can re-invoke under opaque args; the FH dedup
  // by source identity makes re-invocation idempotent.
  function _ObserverCtor(cb) {
    var self2 = this;
    self2._cb = (typeof cb === "function") ? cb : null;
    self2._targets = [];
    self2.observe = function (target) {
      if (!target || self2._targets.indexOf(target) >= 0) return;
      self2._targets.push(target);
      if (self2._cb) {
        // WHATWG IntersectionObserver / ResizeObserver / MutationObserver
        // all queue their callbacks asynchronously (microtask / task
        // queue), NOT synchronously inside observe(). Deferring via
        // G.setTimeout (which our timer shim routes to TQ → drained by
        // __hostFlush) preserves real init order: the bundle finishes
        // setting up state, THEN the cb fires with the entry array.
        // Synchronous firing breaks bundles that rely on post-observe
        // setup variables.
        try { G.__feHandler && G.__feHandler(self2._cb, self2); } catch (e) {}
        var fireOnce = function () {
          try {
            var ent = {
              target: target,
              isIntersecting: OPQ("observer.entry.isIntersecting"),
              intersectionRatio: OPQ("observer.entry.intersectionRatio"),
              contentRect: OPQ("observer.entry.contentRect"),
              boundingClientRect: OPQ("observer.entry.boundingClientRect"),
              intersectionRect: OPQ("observer.entry.intersectionRect"),
              rootBounds: OPQ("observer.entry.rootBounds"),
              time: OPQ("observer.entry.time"),
              type: OPQ("observer.entry.type"),
              addedNodes: OPQ("observer.entry.addedNodes"),
              removedNodes: OPQ("observer.entry.removedNodes"),
              attributeName: OPQ("observer.entry.attributeName"),
              oldValue: OPQ("observer.entry.oldValue"),
            };
            self2._cb.call(self2, [ent], self2);
          } catch (e) {}
        };
        try { G.setTimeout ? G.setTimeout(fireOnce, 0) : fireOnce(); } catch (e) {}
      }
    };
    self2.unobserve = function () {};
    self2.disconnect = function () {};
    self2.takeRecords = function () { return []; };
  }
  G.MutationObserver = G.ResizeObserver = G.IntersectionObserver = G.PerformanceObserver = G.ReportingObserver = _ObserverCtor;
  G.DOMParser = function () { this.parseFromString = function (str) { var d = G.document.createElement("div"); d.innerHTML = String(str == null ? "" : str); d.body = d; d.head = d; d.documentElement = d; return d; }; };
  G.XMLSerializer = function () { this.serializeToString = function (n) { return n && n.outerHTML != null ? n.outerHTML : ""; }; };
  G.DOMException = function (m, n) { this.message = m || ""; this.name = n || "Error"; };
  G.alert = G.focus = G.blur = function () {};
  /* hostedge captured the real print as EPRINT before clobbering — the
     analyzer's stdout protocol uses @H/@S/@T/@E/@WHY records, so the
     bundle's own print() is a no-op (we don't want bundle output mixed
     into the protocol channel). But analyzer infrastructure (driver.js
     breadcrumbs, @WHY observability records emitted from JS) needs a
     way to write protocol records. Expose EPRINT as __feEmit. */
  G.__feEmit = function (s) { EPRINT(String(s)); };
  G.print = function () {};
  G.confirm = function () { return false; }; G.prompt = function () { return null; };
  G.scroll = G.scrollTo = G.scrollBy = function () {};
  G.open = function (u) { if (u != null) H("window.open", [String(u)]); return G; };
  G.close = function () {};
  // btoa/atob are provided by the patched QuickJS context via
  // JS_AddIntrinsicAToB (called inside JS_NewContext). The previous
  // claim that wiring it "wedges" was stale — `typeof btoa` is now
  // "function" out of the box. No JS polyfill: real engine code only.
  // TextEncoder/TextDecoder are bound to the engine (exact JS->UTF-8)
  // + Lexbor's real WHATWG codec set in qjs_dom.c — not reimplemented
  // here. structuredClone stays a JSON round-trip (honest stub —
  // loses Map/Set/Date/cycles; no small spec lib to bind).
  G.structuredClone = G.structuredClone || function (o) { try { return JSON.parse(JSON.stringify(o)); } catch (e) { return o; } };
  G.reportError = function () {};

  // ── Storage (real backing; unknown keys opaque) ──────────────────
  function Storage() {
    var m = Object.create(null);
    return {
      getItem: function (k) { k = String(k); return k in m ? m[k] : OPQ("storage." + k); },
      setItem: function (k, v) { m[String(k)] = String(v); },
      removeItem: function (k) { delete m[String(k)]; },
      clear: function () { m = Object.create(null); },
      key: function (i) { var ks = Object.keys(m); return i < ks.length ? ks[i] : null; },
      get length() { return Object.keys(m).length; },
    };
  }
  G.localStorage = Storage(); G.sessionStorage = Storage();
  G.Storage = function () {};

  // ── XMLHttpRequest (opaque server reply, fired via the pump) ─────
  function XHR() {
    this.readyState = 0; this.timeout = 0; this.withCredentials = false;
    this.responseType = ""; this.responseURL = ""; this._h = {}; this._ev = Object.create(null);
    this.upload = { addEventListener: function () {}, removeEventListener: function () {}, _ev: {} };
    var self = this;
    // Status/statusText are CONCRETE (the success case). Modeling them
    // opaque makes a framework's completion state machine (jQuery:
    // status-range, readyState, dataType, converters, ifModified) fan
    // out ~2^14 forced schedules for ~zero learning value. The
    // genuinely-unknown attacker/server DATA is the response BODY
    // (responseText/response) — that stays opaque so gate/API learning
    // still forks on it.
    Object.defineProperty(this, "status", { get: function () { return self.readyState >= 2 ? 200 : 0; }, configurable: true });
    Object.defineProperty(this, "statusText", { get: function () { return self.readyState >= 2 ? "OK" : ""; }, configurable: true });
    Object.defineProperty(this, "responseText", { get: function () { return self.responseType && self.responseType !== "text" ? "" : OPQ("XHR.responseText"); }, configurable: true });
    Object.defineProperty(this, "response", { get: function () { return OPQ("XHR.response"); }, configurable: true });
    Object.defineProperty(this, "responseXML", { get: function () { return OPQ("XHR.responseXML"); }, configurable: true });
  }
  XHR.prototype.open = function (m, u) { H("XMLHttpRequest.open", [String(m || "GET").toUpperCase(), urlOf(u), urlHolesOf(u)]); this._m = m; this._u = u; this.responseURL = String(u); this.readyState = 1; this._fire("readystatechange"); };
  XHR.prototype.setRequestHeader = function (k, v) { this._h[String(k)] = ISOPQANY(v) ? { kind: "opaque" } : { kind: "literal", value: String(v) }; };
  XHR.prototype.getAllResponseHeaders = function () { return OPQ("XHR.responseHeaders"); };
  XHR.prototype.getResponseHeader = function () { return OPQ("XHR.responseHeader"); };
  XHR.prototype.overrideMimeType = function () {};
  XHR.prototype.addEventListener = function (t, f) { (this._ev[t] || (this._ev[t] = [])).push(f); };
  XHR.prototype.removeEventListener = function (t, f) { var a = this._ev[t]; if (a) this._ev[t] = a.filter(function (x) { return x !== f; }); };
  XHR.prototype.dispatchEvent = function () { return true; };
  XHR.prototype._fire = function (t, ev) {
    ev = ev || { type: t, target: this, currentTarget: this };
    var a = this._ev[t]; if (a) for (var i = 0; i < a.length; i++) { try { a[i].call(this, ev); } catch (e) {} }
    var oh = this["on" + t]; if (typeof oh === "function") { try { oh.call(this, ev); } catch (e) {} }
  };
  XHR.prototype.abort = function () { this._fire("abort"); };
  XHR.prototype.send = function (b) {
    var bs = recBody(b);
    // Per-header provenance: setRequestHeader stores each value as
    // {kind:"literal"|opaque} on this._h. Pass the whole map along
    // as the 3rd @H arg so the learner can surface required headers
    // (X-Requested-With, Authorization, etc.) per-endpoint without
    // collapsing opaque-bearer-tokens into a literal record.
    var hs = null;
    if (this._h) { hs = {}; for (var hk in this._h) hs[hk] = this._h[hk]; }
    H("XMLHttpRequest.send", [bs, bodyShape(b), hs]);   // bodyShape is null+opaque-safe; `b == null` would fork on an opaque body
    var self = this;
    enq(function () {
      var rs = [2, 3, 4];
      for (var i = 0; i < rs.length; i++) { self.readyState = rs[i]; self._fire("readystatechange"); }
      self._fire("load"); self._fire("loadend");
    });
  };
  G.XMLHttpRequest = XHR;

  // ── fetch / Headers / Request / Response (real WHATWG shape) ─────
  function Headers(init) {
    var m = Object.create(null);
    function set(k, v) { m[String(k).toLowerCase()] = String(v); }
    if (init) { if (typeof init.forEach === "function") init.forEach(function (v, k) { set(k, v); }); else if (Array.isArray(init)) init.forEach(function (p) { set(p[0], p[1]); }); else for (var k in init) set(k, init[k]); }
    this.get = function (k) { k = String(k).toLowerCase(); return k in m ? m[k] : null; };
    this.set = set; this.append = set;
    this.has = function (k) { return String(k).toLowerCase() in m; };
    this.delete = function (k) { delete m[String(k).toLowerCase()]; };
    this.forEach = function (cb, t) { for (var k in m) cb.call(t, m[k], k, this); };
    this.keys = function () { return Object.keys(m); };
    this.values = function () { return Object.keys(m).map(function (k) { return m[k]; }); };
    this.entries = function () { return Object.keys(m).map(function (k) { return [k, m[k]]; }); };
    this[Symbol.iterator] = function () { return Object.keys(m).map(function (k) { return [k, m[k]]; })[Symbol.iterator](); };
  }
  G.Headers = Headers;
  // A response body is a WHATWG ReadableStream. Leaving it null/opaque made EVERY
  // stream consumer SPIN: `response.body.getReader().read()` (and the openai
  // stream-utils `for await`) never reached `done`, looping forever. We do NOT
  // issue the request (SECURITY.md:69 — recorded-not-issued), and a real COOKIELESS
  // GET would 401 an auth endpoint, pushing the bundle onto its error branch and
  // off the success/feature path the moat learns. So model the body CONCRETE +
  // TERMINATING: ONE chunk then done. The chunk DATA stays OPAQUE (genuine server
  // input — preserves any chained-request shape); only the stream PROTOCOL (`done`)
  // is concrete, so a stream loop runs its chunk handler ONCE and ENDS.
  function _bodyStream() {
    function rdr() {
      var pulled = false;
      return {
        closed: Promise.resolve(),
        read: function () { var d = pulled; pulled = true; return Promise.resolve(d ? { done: true, value: undefined } : { done: false, value: OPQ("fetch.body.chunk") }); },
        releaseLock: function () {}, cancel: function () { return Promise.resolve(); }
      };
    }
    var s = {
      locked: false,
      getReader: function () { return rdr(); },
      cancel: function () { return Promise.resolve(); },
      tee: function () { return [_bodyStream(), _bodyStream()]; },
      pipeTo: function () { return Promise.resolve(); },
      pipeThrough: function (t) { return (t && t.readable) || _bodyStream(); }
    };
    s.values = function () {
      var r = rdr();
      var it = { next: function () { return r.read(); } };
      it["return"] = function () { return Promise.resolve({ done: true, value: undefined }); };
      it[Symbol.asyncIterator] = function () { return this; };
      return it;
    };
    s[Symbol.asyncIterator] = function () { return s.values(); };
    return s;
  }
  function bodyMethods(o, srcUrl) {
    o.bodyUsed = false;
    // The reply opaque carries its SOURCE endpoint in the label (space-separated
    // after the tag — a URL never contains a raw space), so a reply->request
    // chain (fetch(reply.field)) records WHICH endpoint a bounded reply-GET must
    // fetch for the real example value (the fromReply seed, per CLAUDE.md policy).
    // Concrete source URL only; an opaque/templated fetch URL contributes none.
    var rs = (typeof srcUrl === "string" && srcUrl && srcUrl.indexOf("{") < 0 && srcUrl.indexOf("[object") < 0) ? (" " + srcUrl) : "";
    o.json = function () { return Promise.resolve(OPQ("fetch.body.json" + rs)); };
    o.text = function () { return Promise.resolve(OPQ("fetch.body.text" + rs)); };
    o.arrayBuffer = function () { return Promise.resolve(new ArrayBuffer(0)); };
    o.blob = function () { return Promise.resolve(OPQ("fetch.body.blob" + rs)); };
    o.formData = function () { return Promise.resolve(OPQ("fetch.body.formData" + rs)); };
    o.bytes = function () { return Promise.resolve(new Uint8Array(0)); };
    o.clone = function () { return o; };
    o.body = _bodyStream();   // concrete + terminating (was null → stream consumers spun)
    return o;
  }
  function Response(body, init) {
    init = init || {};
    bodyMethods(this, init.url);
    // Concrete success status (see XHR note) — body stays opaque.
    this.status = init.status != null ? init.status : 200;
    this.statusText = init.statusText != null ? init.statusText : "OK";
    this.ok = true;
    this.redirected = false; this.type = "basic"; this.url = init.url || "";
    this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
    this.clone = function () { return new Response(body, init); };
  }
  Response.error = function () { return new Response(null, { status: 0 }); };
  Response.json = function (d, init) { return new Response(JSON.stringify(d), init); };
  Response.redirect = function (u) { return new Response(null, { status: 302, headers: { location: u } }); };
  G.Response = Response;
  function Request(input, init) {
    init = init || {};
    this.url = (input && typeof input === "object" && input.url != null) ? String(input.url) : String(input);
    this.method = (init.method || (input && input.method) || "GET").toUpperCase();
    this.headers = new Headers(init.headers || (input && input.headers));
    this.body = init.body != null ? init.body : (input && input.body) || null;
    this.credentials = init.credentials || "same-origin";
    this.mode = init.mode || "cors"; this.cache = init.cache || "default";
    this.redirect = init.redirect || "follow"; this.referrer = init.referrer || "";
    this.signal = init.signal || null;
    bodyMethods(this);
    this.clone = function () { return new Request(input, init); };
  }
  G.Request = Request;
  function safeBody(b) { try { if (b && b._fd) return JSON.stringify(b._fd); return JSON.stringify(b); } catch (e) { return String(b); } }
  // Body → @H-record string. A raw String(opaque) body INFECTS the record:
  // String()/JSON.stringify are opaque-infectious (so API-learning keeps
  // `${String(x)}` a shape), so an opaque body left raw makes the whole
  // JSON.stringify(@H) opaque → the record never prints → the endpoint is
  // silently dropped (e.g. fetch(url,{body:JSON.stringify({id:opaque})})).
  // ISOPQANY is a CONCRETE bool, checked FIRST so it short-circuits before
  // the infectious typeof/String. The per-field SHAPE arg carries the
  // structure separately; this is just the readable string form. Mirrors
  // S()'s @S concretization — the same invariant for the @H record layer.
  function recBody(b) {
    if (ISOPQANY(b)) return "[opaque]";   // concrete check before `== null` (which forks on opaque)
    if (b == null) return null;
    return (typeof b === "object" && !b._fd) ? safeBody(b) : String(b);
  }
  // Per-header provenance: literal vs opaque per header name. Accepts
  // any of the WHATWG `init.headers` shapes — a Headers instance
  // (.forEach), an Array of [k,v] pairs, or a plain object. Opaque
  // values (e.g. an Authorization token built from a host-edge stored
  // attacker leaf) stay marked opaque so the learner records "this
  // endpoint needs an Authorization header" without inventing a value.
  function headersShape(h) {
    if (ISOPQANY(h)) return null;   // concrete before `== null`; a wholly-opaque headers value has no enumerable per-header provenance
    if (h == null) return null;
    var out = {};
    function record(k, v) {
      var name = String(k);
      out[name] = ISOPQANY(v) ? { kind: "opaque" } : { kind: "literal", value: String(v) };
    }
    try {
      if (typeof h.forEach === "function") { h.forEach(function (v, k) { record(k, v); }); return out; }
      if (Array.isArray(h)) { for (var i = 0; i < h.length; i++) record(h[i][0], h[i][1]); return out; }
      if (typeof h === "object") {
        var keys = Object.keys(h);
        for (var ki = 0; ki < keys.length; ki++) record(keys[ki], h[keys[ki]]);
        return out;
      }
    } catch (e) {
      /* Header-set enumeration threw — same pattern as body shape: keep
         whatever was captured before the throw, but surface so the
         "requiredHeaders missing the auth header" symptom is traceable
         to the enumeration failure (revoked Proxy, throwing getter,
         exotic Headers subclass) instead of silently disappearing. */
      if (typeof printErr === "function")
        printErr('@WHY {"phase":"headersShape_throw","keys_captured":' + Object.keys(out).length + ',"err":' + JSON.stringify(String(e && e.message || e)) + '}');
    }
    return out;
  }
  // Walk a fetch / XHR body into a structured shape with per-field
  // provenance: literal value vs opaque (host-edge attacker-tainted
  // input). Lets the @H consumer see "POST body = {action: 'favorite'
  // (literal), target_id: <opaque>}" instead of collapsing to a
  // single stringified blob with [object Object] markers. Recursive
  // over object / array / FormData / URLSearchParams. Termination is
  // structural — bounded by the bundle's body-object's own structure
  // (which is constructed at runtime, finite per construction).
  // Walk a recovered __opaqueShape JSON tree. The engine renders opaque-valued
  // fields as the literal marker "[object Object]" (the sentinel's String()
  // form); map THAT back to {kind:"opaque"} so the field's provenance stays
  // honest (no fabricated value) while literal keys + literal scalar values are
  // emitted as captured. Confined to shape JSON (only reached via OPQSHAPE) so
  // the marker check can't misclassify a legitimate real-body value.
  function shapeWalk(v) {
    if (v === "[object Object]") return { kind: "opaque" };
    if (v == null) return { kind: "literal", value: String(v) };
    var tv = typeof v;
    if (tv === "string" || tv === "number" || tv === "boolean") return { kind: "literal", value: String(v) };
    if (Array.isArray(v)) { var a = []; for (var i = 0; i < v.length; i++) a.push(shapeWalk(v[i])); return { kind: "array", items: a }; }
    if (tv === "object") { var o = {}; for (var k in v) if (Object.prototype.hasOwnProperty.call(v, k)) o[k] = shapeWalk(v[k]); return { kind: "object", fields: o }; }
    return { kind: "literal", value: String(v) };
  }
  function bodyShape(b) {
    // ISOPQANY (concrete) BEFORE the `== null` check: `opaque == null` is
    // opaque-infectious (forks), and __feDriveStatic's single forced pass would
    // take the "is null" arm and skip recovery. An opaque value is present.
    if (ISOPQANY(b)) {
      // A mixed-opaque body (`JSON.stringify({k:opaque, n:7})`) is an infectious
      // opaque, but the engine stashed the rendered field structure — recover
      // the keys + literal values rather than collapsing to {kind:opaque}.
      var sh = OPQSHAPE(b);
      if (typeof sh === "string" && sh.length > 1) {
        var c0 = sh.charCodeAt(0);
        if (c0 === 123 || c0 === 91) {            // '{' or '[' — engine renders shape as valid JSON
          try {
            var pj = JSON.parse(sh);
            if (pj && typeof pj === "object") return shapeWalk(pj);
          } catch (e) {
            if (typeof printErr === "function")
              printErr('@WHY {"phase":"opaqueShape_parse","err":' + JSON.stringify(String(e && e.message || e)) + '}');
          }
        }
      }
      return { kind: "opaque" };
    }
    if (b == null) return null;
    var t = typeof b;
    if (t === "string") {
      // The ubiquitous `body: JSON.stringify({...})` arrives here as a STRING.
      // Recording it as one literal blob loses every field KEY + value — the
      // goal-#2 depth gap (POST endpoints learned with NO body params despite a
      // content-type:application/json header; _bodykeys gate). If it parses as a
      // JSON object/array, walk the PARSED structure so per-field keys+values are
      // captured. A non-JSON string (or one not starting with {/[) stays literal.
      if (b.length > 1) { var c0 = b.charCodeAt(0); if (c0 === 123 || c0 === 91) {   // '{' or '['
        try { var pj = JSON.parse(b); if (pj && typeof pj === "object") return bodyShape(pj); } catch (_) {}
      } }
      return { kind: "literal", value: String(b) };
    }
    if (t === "number" || t === "boolean") return { kind: "literal", value: String(b) };
    if (b && b._fd) {                              // FormData (our shim's _fd backing object)
      var f = {};
      for (var k in b._fd) f[k] = bodyShape(b._fd[k]);
      return { kind: "formdata", fields: f };
    }
    if (b && typeof b.entries === "function" && typeof b.toString === "function" && !Array.isArray(b)) {
      // URLSearchParams / Headers / Map-like: enumerate entries.
      try {
        var es = b.entries();
        if (Array.isArray(es)) {
          var um = {};
          for (var ei = 0; ei < es.length; ei++) um[es[ei][0]] = bodyShape(es[ei][1]);
          return { kind: "params", fields: um };
        }
      } catch (_) {}
    }
    if (Array.isArray(b)) {
      var arr = [];
      for (var i = 0; i < b.length; i++) arr.push(bodyShape(b[i]));
      return { kind: "array", items: arr };
    }
    /* Binary bodies — ArrayBuffer, TypedArray (Uint8Array, etc.), DataView,
       SharedArrayBuffer. Bundles use these for Protobuf/JSPB/gRPC-Web
       payloads, raw-binary file uploads, signed-body POSTs. The previous
       fall-through to the generic-object branch emitted `{kind:"object",
       fields:{}}` (no enumerable own properties on these types) which
       silently dropped the entire payload — protocol classification
       (#7) can't run on data it never sees, and per-field examples (#2)
       likewise lose the content. Emit `kind:"binary"` with the FULL
       byte sequence as a hex string (total over every byte value, no
       capping/sampling — protocol classification reads what it needs
       from the prefix; we don't truncate the record). */
    var ab = null, off = 0, len = 0;
    /* Blob/File — read the bytes captured by the Blob constructor. The
       _bytes Uint8Array is the concatenated payload (strings encoded
       UTF-8, TypedArrays/ArrayBuffers copied verbatim). Captures the
       content-type for protocol classification context. */
    if (b && b._bytes instanceof Uint8Array && (b instanceof G.Blob || (typeof G.File === "function" && b instanceof G.File))) {
      ab = b._bytes.buffer; off = b._bytes.byteOffset | 0; len = b._bytes.byteLength | 0;
    } else if (b && b.buffer instanceof ArrayBuffer) {        // TypedArray / DataView
      ab = b.buffer; off = b.byteOffset | 0; len = b.byteLength | 0;
    } else if (b instanceof ArrayBuffer) {
      ab = b; off = 0; len = b.byteLength | 0;
    } else if (typeof SharedArrayBuffer !== "undefined" && b instanceof SharedArrayBuffer) {
      ab = b; off = 0; len = b.byteLength | 0;
    }
    if (ab) {
      var view = new Uint8Array(ab, off, len);
      var hex = "";
      for (var bi = 0; bi < len; bi++) {
        var ch = view[bi];
        hex += (ch < 16 ? "0" : "") + ch.toString(16);
      }
      return { kind: "binary", byteLength: len, hex: hex };
    }
    if (t === "object") {
      var o = {};
      try {
        var keys = Object.keys(b);
        for (var ki = 0; ki < keys.length; ki++) o[keys[ki]] = bodyShape(b[keys[ki]]);
      } catch (e) {
        /* Enumeration threw — most commonly a revoked Proxy or a getter
           that throws on traversal. The fields captured BEFORE the throw
           are kept (graceful), but the throw itself must surface so
           per-field example loss for the bundle's body object is visible
           to the reviewer; silent here was a "0 body params learned"
           with no clue why on production sites that wrap body objects
           in revocable Proxies. */
        if (typeof printErr === "function")
          printErr('@WHY {"phase":"bodyShape_object_throw","keys_captured":' + Object.keys(o).length + ',"err":' + JSON.stringify(String(e && e.message || e)) + '}');
      }
      return { kind: "object", fields: o };
    }
    return { kind: "literal", value: String(b) };
  }
  G.fetch = function (input, init) {
    init = init || {};
    // `input` may be a URL string, an OPAQUE url value (a qjs_opq object — its
    // typeof is "object" but it is NOT a Request), or a real Request. Reading
    // .method/.body/.headers off an opaque url is a property-get on opaque →
    // opaque → the recorded method became "[object Object]", blocking the call
    // as reached-but-opaque (github netdiff: template-literal urls like
    // fetch(`/${owner}/${repo}/...`) produce an opaque url whose method was lost).
    // A genuine Request is a CONCRETE object with a concrete .url. An opaque url
    // is ISOPQANY-true even when its typeof happens to read "object" (flaky), and
    // PRESENT(opaque) is TRUE — so `typeof==="object" && PRESENT(input.url)` alone
    // wrongly classifies an opaque url as a Request and reads opaque .url/.method
    // off it. Exclude opaque inputs: only a NON-opaque object with a concrete .url
    // is a Request; an opaque url falls through to urlOf(input) with method "GET".
    var _isReq = input && typeof input === "object" && !ISOPQANY(input) && PRESENT(input.url);
    var url = _isReq ? input.url : input;
    var method = (init.method || (_isReq && input.method) || "GET");
    var body = PRESENT(init.body) ? init.body : (_isReq ? input.body : null);
    var headers = PRESENT(init.headers) ? init.headers : (_isReq ? input.headers : null);
    var bodyStr = recBody(body);
    // bodyShape/headersShape are null- AND opaque-safe (ISOPQANY-first); calling
    // them directly avoids a `body == null` fork on an opaque body.
    H("fetch", [urlOf(url), String(method).toUpperCase(), bodyStr, bodyShape(body), headersShape(headers), urlHolesOf(url)]);
    return Promise.resolve(new Response(null, { url: String(url) }));
  };
  /* Blob / File — minimal WHATWG models. The point isn't full Blob
     semantics; it's that a bundle building a body via
     `new Blob([uint8Array], {type:"image/png"})` (or `new File(...)`)
     needs the BYTES preserved so bodyShape's binary path emits the
     real upload payload. Previously hostedge had no Blob/File, so
     `b instanceof Blob` checks in bundle code threw, AND a FormData
     append of a Blob silently became the string "[file]" (no bytes
     captured). The constructor concatenates parts: each part can be
     a string (UTF-8 encoded), a TypedArray/ArrayBuffer (bytes copied
     verbatim), or another Blob (its _bytes concatenated). The
     resulting _bytes Uint8Array IS what fetch/XHR sends; bodyShape
     reads it directly. */
  G.Blob = function Blob(parts, opts) {
    var pieces = [];
    var total = 0;
    if (parts && typeof parts.length === "number") {
      for (var i = 0; i < parts.length; i++) {
        var p = parts[i];
        if (p == null) continue;
        var bytes;
        if (p instanceof G.Blob && p._bytes instanceof Uint8Array) {
          bytes = p._bytes;
        } else if (p && p.buffer instanceof ArrayBuffer) {
          bytes = new Uint8Array(p.buffer, p.byteOffset | 0, p.byteLength | 0);
        } else if (p instanceof ArrayBuffer) {
          bytes = new Uint8Array(p);
        } else {
          /* String → UTF-8 bytes via the existing TextEncoder binding
             (real Lexbor-backed encoding, not a hand-rolled fallback). */
          var s = String(p);
          if (typeof TextEncoder === "function") {
            bytes = new TextEncoder().encode(s);
          } else {
            bytes = new Uint8Array(s.length);
            for (var ci = 0; ci < s.length; ci++) bytes[ci] = s.charCodeAt(ci) & 0xff;
          }
        }
        pieces.push(bytes);
        total += bytes.length;
      }
    }
    var out = new Uint8Array(total);
    var off = 0;
    for (var pj = 0; pj < pieces.length; pj++) {
      out.set(pieces[pj], off);
      off += pieces[pj].length;
    }
    this._bytes = out;
    this.size = out.length;
    this.type = (opts && opts.type) ? String(opts.type) : "";
  };
  G.Blob.prototype.slice = function (start, end, ctype) {
    var b = new G.Blob([], { type: ctype || this.type });
    b._bytes = this._bytes.slice(start | 0, end == null ? this._bytes.length : (end | 0));
    b.size = b._bytes.length;
    return b;
  };
  G.Blob.prototype.arrayBuffer = function () { return Promise.resolve(this._bytes.buffer.slice(this._bytes.byteOffset, this._bytes.byteOffset + this._bytes.byteLength)); };
  G.Blob.prototype.text = function () {
    var dec = (typeof TextDecoder === "function") ? new TextDecoder() : null;
    return Promise.resolve(dec ? dec.decode(this._bytes) : String.fromCharCode.apply(null, this._bytes));
  };
  G.Blob.prototype.stream = function () { return null; };   /* ReadableStream not modelled */
  G.File = function File(parts, name, opts) {
    G.Blob.call(this, parts, opts);
    this.name = String(name == null ? "" : name);
    this.lastModified = (opts && opts.lastModified) ? +opts.lastModified : 0;
  };
  G.File.prototype = Object.create(G.Blob.prototype);
  G.File.prototype.constructor = G.File;
  /* FormData — methods on the prototype per WHATWG XHR §FormData. The
     prior factory-returns-object form left FormData.prototype empty
     and bundles destructuring FormData.prototype.{append,get,…} got
     undefined → throw. */
  G.FormData = function FormData() { this._fd = Object.create(null); };
  var FDp = G.FormData.prototype;
  /* Preserve Blob/File AND TypedArray/ArrayBuffer/DataView instances so
     the body shape walker captures the real upload bytes. Stringifying
     non-Blob binary to "1,2,3,4" (the default TypedArray toString)
     dropped the per-byte payload — multipart uploads via TypedArray
     bodies lost their content. Now any value bodyShape recognizes as
     binary (Blob with _bytes, TypedArray with .buffer, raw ArrayBuffer)
     passes through verbatim; bodyShape's binary branch later turns
     each into kind:"binary" with full hex. Falls back to String(v) for
     plain strings/numbers/booleans/etc. */
  FDp.append = function (k, v) {
    if (v == null) { this._fd[k] = ""; return; }
    if (v instanceof G.Blob || (v && v._bytes instanceof Uint8Array)) { this._fd[k] = v; return; }
    if (v instanceof ArrayBuffer || (v && v.buffer instanceof ArrayBuffer)) { this._fd[k] = v; return; }
    if (typeof SharedArrayBuffer !== "undefined" && v instanceof SharedArrayBuffer) { this._fd[k] = v; return; }
    this._fd[k] = (typeof v === "object" ? String(v) : String(v));
  };
  FDp.set = function (k, v) { this._fd[k] = String(v); };
  FDp.get = function (k) { return k in this._fd ? this._fd[k] : null; };
  FDp.getAll = function (k) { return k in this._fd ? [this._fd[k]] : []; };
  FDp.has = function (k) { return k in this._fd; };
  FDp["delete"] = function (k) { delete this._fd[k]; };
  FDp.forEach = function (cb) { var fd = this._fd; for (var k in fd) cb(fd[k], k, this); };
  FDp.entries = function () { var fd = this._fd; return Object.keys(fd).map(function (k) { return [k, fd[k]]; })[Symbol.iterator](); };
  FDp.keys = function () { return Object.keys(this._fd)[Symbol.iterator](); };
  FDp.values = function () { var fd = this._fd; return Object.keys(fd).map(function (k) { return fd[k]; })[Symbol.iterator](); };
  FDp[Symbol.iterator] = function () { var fd = this._fd; return Object.keys(fd).map(function (k) { return [k, fd[k]]; })[Symbol.iterator](); };
  /* URLSearchParams — methods on the prototype per WHATWG IDL, so
     `Object.getOwnPropertyDescriptor(URLSearchParams.prototype, "get")`
     returns a real method descriptor. Real bundles destructure
     URLSearchParams.prototype.{get,has,set,delete,entries,...} at boot
     for fast-path access bypassing prototype lookup. Method-on-instance
     (the prior factory form) returns an empty .prototype and the bundle
     throws "not a function" on the destructured ref. */
  if (!G.URLSearchParams) {
    G.URLSearchParams = function URLSearchParams(init) {
      var m = [];
      // location.search/hash arrive as an attacker-tainted opaque carrying the
      // page's real query as the concrete SHAPE. Parse the shape (typeof opaque
      // !== "string" previously dropped EVERY URL param) so .get(k) yields the
      // concrete param VALUE for API learning, re-wrapped as a tainted opaque
      // (Z3 attacker source, sub-labelled by key). A literal-string init stays
      // concrete (no taint) — only an opaque source contributes attacker taint.
      var _tk = null;
      if (ISOPQANY(init)) { var _sh = OPQSHAPE(init); init = (typeof _sh === "string") ? _sh : ""; _tk = "location.search"; }
      if (typeof init === "string") {
        init.replace(/^\?/, "").split("&").forEach(function (p) {
          if (!p) return; var i = p.indexOf("=");
          var k = decodeURIComponent(i < 0 ? p : p.slice(0, i));
          var v = i < 0 ? "" : decodeURIComponent(p.slice(i + 1));
          m.push([k, _tk ? OPQ(_tk + "." + k, v) : v]);
        });
      } else if (init && typeof init.forEach === "function") {
        init.forEach(function (v, k) { m.push([k, String(v)]); });
      } else if (init) {
        for (var k in init) m.push([k, String(init[k])]);
      }
      this._m = m;
      // Source provenance: when built from an opaque (attacker-controlled
      // location.search/hash), a param ABSENT from the concrete shape must still
      // read as opaque — the attacker could supply it — so a `.get(k)`-gated arm
      // (oidc signinCallback's `if(!params.get("code"))return`) FORKS and its
      // gated endpoint surfaces, instead of bailing on a concrete null.
      this._opq = _tk;
    };
    var USPp = G.URLSearchParams.prototype;
    /* Live two-way bind: params obtained from url.searchParams carry a _feUrl
       back-ref (qjs_dom g_u_search_params sets it); a mutation re-serializes into
       url.search so `url.searchParams.set("select", cols); fetch(url)` records the
       query — the supabase/axios builder pattern. Detached `new URLSearchParams()`
       has no _feUrl → no-op. Re-parse keeps both sides exact (encoding round-trip). */
    USPp._sync = function () { if (this._feUrl) { try { this._feUrl.search = this.toString(); } catch (e) {} } };
    USPp.append = function (k, v) { this._m.push([k, String(v)]); this._sync(); };
    USPp.set = function (k, v) { this._m = this._m.filter(function (e) { return e[0] !== k; }); this._m.push([k, String(v)]); this._sync(); };
    USPp.get = function (k) { for (var i = 0; i < this._m.length; i++) if (this._m[i][0] === k) return this._m[i][1]; return this._opq ? OPQ(this._opq + "." + k) : null; };
    USPp.getAll = function (k) { var r = this._m.filter(function (e) { return e[0] === k; }).map(function (e) { return e[1]; }); return (r.length === 0 && this._opq) ? [OPQ(this._opq + "." + k)] : r; };
    USPp.has = function (k) { for (var i = 0; i < this._m.length; i++) if (this._m[i][0] === k) return true; return this._opq ? OPQ(this._opq + ".has." + k) : false; };
    USPp["delete"] = function (k) { this._m = this._m.filter(function (e) { return e[0] !== k; }); this._sync(); };
    USPp.sort = function () { this._m.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); this._sync(); };
    USPp.forEach = function (cb) { var mm = this._m.slice(); for (var i = 0; i < mm.length; i++) cb(mm[i][1], mm[i][0], this); };
    USPp.toString = function () { return this._m.map(function (e) { return encodeURIComponent(e[0]) + "=" + encodeURIComponent(e[1]); }).join("&"); };
    USPp.entries = function () { return this._m.slice()[Symbol.iterator](); };
    USPp.keys = function () { return this._m.map(function (e) { return e[0]; })[Symbol.iterator](); };
    USPp.values = function () { return this._m.map(function (e) { return e[1]; })[Symbol.iterator](); };
    USPp[Symbol.iterator] = function () { return this._m.slice()[Symbol.iterator](); };
  }
  G.WebSocket = function (u) { H("WebSocket", [urlOf(u)]); var s = this; this.readyState = 1; this.url = String(u); this.send = function (d) { H("WebSocket.send", [recBody(d)]); }; this.close = function () {}; this._ev = {}; this.addEventListener = function (t, f) { (s._ev[t] || (s._ev[t] = [])).push(f); }; this.removeEventListener = function () {}; this.dispatchEvent = function () { return true; }; }; G.WebSocket.CONNECTING = 0; G.WebSocket.OPEN = 1; G.WebSocket.CLOSING = 2; G.WebSocket.CLOSED = 3;
  G.EventSource = function (u) { H("EventSource", [urlOf(u)]); var s = this; this.url = String(u); this.readyState = 1; this._ev = {}; this.addEventListener = function (t, f) { (s._ev[t] || (s._ev[t] = [])).push(f); }; this.removeEventListener = function () {}; this.close = function () {}; }; G.EventSource.CLOSED = 2;
  G.Worker = function (u) { H("Worker", [urlOf(u)]); this.postMessage = function () {}; this.terminate = function () {}; this.addEventListener = function () {}; this.onmessage = null; };
  G.SharedWorker = function (u) { H("SharedWorker", [urlOf(u)]); this.port = { postMessage: function () {}, start: function () {}, addEventListener: function () {} }; };
  G.BroadcastChannel = function (n) { this.name = n; this.postMessage = function () {}; this.close = function () {}; this.addEventListener = function () {}; };
  // MessageChannel/MessagePort — React 18 + Turbo schedule macrotasks
  // via `port.postMessage(null)` (the yield-to-event-loop trick), so it
  // must drive the pump: a postMessage enqueues delivery of a `message`
  // event to the peer port's handlers. Delivery is a single source-site
  // closure, so ranKeys collapses it to one fire (same bounded model as
  // requestIdleCallback) — the scheduler's re-post is deduped, which is
  // exactly the structural termination, not a cap.
  function MessagePort() { this._l = {}; this.onmessage = null; this.onmessageerror = null; this._other = null; }
  MessagePort.prototype.addEventListener = function (t, f) { (this._l[t] || (this._l[t] = [])).push(f); };
  MessagePort.prototype.removeEventListener = function (t, f) { var a = this._l[t]; if (a) { var i = a.indexOf(f); if (i >= 0) a.splice(i, 1); } };
  MessagePort.prototype.start = function () {};
  MessagePort.prototype.close = function () {};
  MessagePort.prototype.dispatchEvent = function () { return true; };
  MessagePort.prototype.postMessage = function (data) {
    var dst = this._other; if (!dst) return;
    enq(function () {
      var ev = mkEvent("message", { bubbles: false });
      ev.data = data; ev.origin = ""; ev.lastEventId = ""; ev.source = null; ev.ports = [];
      if (typeof dst.onmessage === "function") { try { dst.onmessage(ev); } catch (e) {} }
      var a = dst._l.message; if (a) for (var i = 0; i < a.length; i++) { try { (a[i].handleEvent || a[i]).call(dst, ev); } catch (e) {} }
    });
  };
  G.MessagePort = MessagePort;
  G.MessageChannel = function () { var p1 = new MessagePort(), p2 = new MessagePort(); p1._other = p2; p2._other = p1; this.port1 = p1; this.port2 = p2; };
  G.sendBeacon = G.navigator.sendBeacon;
  G.XMLHttpRequestUpload = function () {};

  // ── code-exec sinks (record + still execute) ─────────────────────
  var _eval = G.eval; G.eval_ = _eval;
  G.eval = function (s) { S("code-exec", "eval", s); try { return _eval(s); } catch (e) { return OPQ("eval.exception"); } };
  var _Fn = G.Function;
  G.Function = function () { var a = [].slice.call(arguments); S("code-exec", "Function", a.length ? a[a.length - 1] : "", { argc: a.length }); try { return _Fn.apply(null, a); } catch (e) { return function () { return OPQ("Function.exception"); }; } };
  G.Function.prototype = _Fn.prototype;

  // SSR-phase deleted: hostedge does NOT run the page's scripts. Lexbor parses the
  // HTML into the DOM; QuickJS runs the document's scripts via the engine's driven
  // bundle path (qjsmain), not a separate unhooked _eval here.

  // ── event-loop flush: window lifecycle + dispatch on the Lexbor
  //    document (its prelude added addEventListener/dispatchEvent), so
  //    jQuery's ready / framework boot handlers run; then drain timers.
  function dispatchDoc(ev) {
    try { if (G.document && typeof G.document.dispatchEvent === "function") G.document.dispatchEvent(ev); } catch (e) {}
  }
  G.__hostFlush = function () {
    try { G.document.readyState = "interactive"; } catch (e) {}
    G.dispatchEvent(mkEvent("readystatechange")); dispatchDoc(mkEvent("readystatechange"));
    G.dispatchEvent(mkEvent("DOMContentLoaded")); dispatchDoc(mkEvent("DOMContentLoaded"));
    try { G.document.readyState = "complete"; } catch (e) {}
    G.dispatchEvent(mkEvent("readystatechange")); dispatchDoc(mkEvent("readystatechange"));
    G.dispatchEvent(mkEvent("load")); dispatchDoc(mkEvent("load"));
    G.dispatchEvent(mkEvent("pageshow"));
    // Real message-event dispatch. mkMessageEvent returns a CONCRETE
    // event-object shell with opaque `e.data` (the attacker-input
    // surface). `!e` on the concrete shell evaluates concrete false —
    // no fork on the existence guard. `!e.data` on opaque DOES fork
    // (as it should), but the cursor advancement is BFS-discovered
    // identically to __hostDrive's path. Without this dispatch the
    // bundle's framework boot (handlers attached via addEventListener)
    // never see a synthetic message event through the real
    // dispatchEvent path, losing coverage of any wrapper that
    // distinguishes dispatch-fire from direct-call.
    G.dispatchEvent(mkMessageEvent()); dispatchDoc(mkMessageEvent());
    var _drained = 0;
    while (TQ.length) {
      var fn = TQ.shift();
      var _sk; try { _sk = fn.toString(); } catch (e) { _sk = null; }
      try { fn.call(G); } catch (e) {}
      _drained++;
      // Per-key spinner accounting (ACROSS flushes): a source-text key that runs
      // _SPIN_K times without advancing _hProg is a no-output spinner — record it in
      // _spinKeys so enq() defers its future re-queues (a tick that schedules a NEW
      // closure each time evades _enqSeen, so identity dedup alone never stops it).
      if (_sk) {
        var _tp = _taskProg.get(_sk);
        if (!_tp) _taskProg.set(_sk, { h: _hProg, s: 0 });
        else if (_hProg > _tp.h) { _tp.h = _hProg; _tp.s = 0; }
        else if (++_tp.s >= _SPIN_K) _spinKeys.add(_sk);
      }
      // NO per-flush idle CAP — the old 512-idle break is DISSOLVED (policy: a count
      // that STOPS work is a vestigial bound). The _SPIN_K per-source-text-key defer
      // above IS the structural replacement it was a placeholder for: a no-@H-progress
      // key is added to _spinKeys so enq() SKIPS its future re-queues, so the TQ
      // provably DRAINS (a fixpoint — re-queues stop) instead of being truncated by a
      // count. A genuine producing loop keeps resetting its key's stale count via @H
      // progress, so it is never deferred; only a pure no-output spinner is.
    }
  };

  // ── entry-point driver (J-Force §3.1.3 / X-Force) ────────────────
  // form.submit() is defined in qjs_dom.c's Lexbor-binding prelude
  // (the WHATWG forms spec method lives in the DOM layer, not as a
  // host-edge monkey-patch). It routes through globalThis.fetch with
  // the form's literal action + per-field body shape — the same
  // fetch hook the bundle reaches.
  // Force every bundle-introduced global function and every registered
  // event handler by source identity (ranKeys), with opaque args. No
  // bundler/registry recognition (by name or shape) — that models the
  // bundler and is banned; reaching the in-bundle fetch/XHR call sites
  // behind unrun modules is forced execution at the function level.
  var PRE = Object.create(null);
  var FEMUTE = (typeof __feMute === "function") ? __feMute : function () {};
  // Drain the queued event-handler registrations (FH): fire each registered
  // handler ONCE with its REAL instance — `hf` is the exact closure passed to
  // addEventListener, so closure-var reads (e.g. `btn.getAttribute('data-op')`
  // in a click handler) resolve through the real created element, and `this` =
  // the registration target. Extracted from __hostDrive so the DEEP GRIND can
  // drain it too: a never-called factory (login/click/route-gated boot fn — the
  // moat's unused-API target) registers its handler DURING the grind, AFTER the
  // boot drain already ran; without a post-grind drain that real-instance
  // handler sits in FH unfired and its click-gated endpoint is lost (it only
  // ever runs as a cold opaque residue orphan). Structural termination via the
  // schedule cursor (re-queue only while a handler keeps advancing the forced
  // window, else retire its source key) — no cap, same invariant as before.
  G.__feDrainHandlers = function () {
    var FECUR = (typeof __feCursor === "function") ? __feCursor : function () { return 0; };
    var FELEN = (typeof __feLen === "function") ? __feLen : function () { return 0; };
    while (FH.length) {
      var hfe = FH.shift();
      var hf = hfe.fn, ht = hfe.target;
      var hk = keyOf(hf);
      var _curBefore = FECUR();
      var thisArg = ht != null ? ht : G;
      var hev = ht != null ? mkHandlerEvent(ht) : OPQ("handler.event");
      try { hf.call(thisArg, hev); } catch (x) {}
      var _curAfter = FECUR();
      if (_curAfter > _curBefore && _curBefore <= FELEN()) FH.push(hfe);
      else ranKeys.add(hk);
    }
  };
  G.__hostDrive = function () {
    var names;
    try { names = Object.getOwnPropertyNames(G); } catch (e) { return; }
    // The global-function enumeration calls EVERY bundle-defined fn
    // with opaque args — internal library code (jQuery utils, lodash
    // helpers, …) emits hundreds of irrelevant frontiers if F-report
    // is on here. Mute frontier seeding for this loop; the X-Force
    // BFS doesn't need to explore inside lodash to find a real sink.
    FEMUTE(1);
    try {
    for (var i = 0; i < names.length; i++) {
      var nm = names[i];
      if (PRE[nm]) continue;
      var v; try { v = G[nm]; } catch (e) { continue; }
      if (typeof v === "function") {
        var fk = keyOf(v); if (ranKeys.has(fk)) continue; ranKeys.add(fk);
        try { v.call(G, OPQ("handler.arg0"), OPQ("handler.arg1"), OPQ("handler.arg2")); } catch (x) {}
      }
    }
    } finally { FEMUTE(0); }
    // Mute lifted for the rest — frontier records from user
    // handlers/connectedCallbacks ARE what drive.mjs/ast-thread BFS
    // must see to enumerate schedules that reach the real sink-paths
    // (postMessage→render, click→submit, etc.). X-Force fitness gating
    // bounds the schedule space.
    // Catalyst/Turbo connectedCallback driving (the github + GMail
    // pattern). After the HTML body is parsed into Lexbor and custom
    // elements are upgraded, their connectedCallback should fire — in
    // a real browser the parser fires it as the element is connected.
    // In our spec-parser model (Lexbor parses only, doesn't execute),
    // we walk every upgraded element and invoke connectedCallback
    // directly. Deduped by element identity so a tree of N custom
    // elements gives N callbacks once, not per __hostDrive iteration.
    var connKeys = ranKeys;
    try {
      var all = document.querySelectorAll("*");
      for (var ei = 0; ei < all.length; ei++) {
        var el = all[ei];
        var cb;
        try { cb = el.connectedCallback; } catch (e) { continue; }
        if (typeof cb !== "function") continue;
        // Identity: function source + tagName. Same ctor across many
        // instances must STILL fire per-instance (each element has its
        // own this.dataset / getAttribute results), so include the
        // node identity (we don't have a stable pointer; use a counter
        // attached to the element via a WeakMap-equivalent).
        var ek;
        try { ek = (el.__feCBKey = el.__feCBKey || (++__feCBSeq)); } catch (e) { ek = -1; }
        var fk2 = keyOf(cb) + ":" + ek;
        if (connKeys.has(fk2)) continue;
        connKeys.add(fk2);
        try { cb.call(el); } catch (x) {}
      }
    } catch (e) {}

    // Form-submission driving via form.submit() — the same hook a
    // bundle's own JS submit() call reaches. The literal action URL +
    // structured body shape carries per-field opaque/literal so the
    // fetch @H record never embeds "[object Object]" in the URL; the
    // method (GET/POST) determines the per-field location at the
    // consumer (query vs body) downstream. Dedup by (method, action)
    // — the ENDPOINT identity.
    try {
      var forms2 = document.querySelectorAll("form[action]");
      for (var fi = 0; fi < forms2.length; fi++) {
        var fEl = forms2[fi];
        var fAction = fEl.getAttribute("action");
        if (!fAction) continue;
        var fMethod = (fEl.getAttribute("method") || "GET").toUpperCase();
        var formKey = "form-submit:" + fMethod + ":" + fAction;
        if (ranKeys.has(formKey)) continue;
        ranKeys.add(formKey);
        try { fEl.submit(); } catch (e) {}
      }
    } catch (e) {}
    // <button formaction> overrides the enclosing form's action when
    // that specific button submits the form. Each formaction is a real
    // distinct endpoint (e.g. /issues/42/close vs /issues/42/reopen on
    // the same form). button.click() in WHATWG triggers form submit
    // with the override; we hook the override via submit() on the
    // enclosing form after temporarily storing the override.
    try {
      var fbtns = document.querySelectorAll("[formaction]");
      for (var bi = 0; bi < fbtns.length; bi++) {
        var bEl = fbtns[bi];
        var bAction = bEl.getAttribute("formaction");
        if (!bAction) continue;
        var bMethod = (bEl.getAttribute("formmethod") || "POST").toUpperCase();
        var btnKey = "formaction-submit:" + bMethod + ":" + bAction;
        if (ranKeys.has(btnKey)) continue;
        ranKeys.add(btnKey);
        var owning = null;
        try {
          var node = bEl;
          while (node && node !== document) {
            if (node.tagName && String(node.tagName).toUpperCase() === "FORM") { owning = node; break; }
            node = node.parentNode;
          }
        } catch (e) {}
        if (!owning) continue;
        try { owning.submit({ formaction: bAction, formmethod: bMethod }); } catch (e) {}
      }
    } catch (e) {}

    /* Declared-HTML-endpoint extraction (the old "source #4": statically
       walking [action]/[formaction]/[*-url|-href|-src]/custom-element src
       off the parsed document and emitting @H directly) was REMOVED — it is
       parallel/static analysis, which the top-of-CLAUDE.md goal forbids:
       endpoints must come from the bundle EXECUTING (forced exec reaching the
       host edge), never from scanning HTML attributes. A `<form action>` is
       learned when the bundle's submit fires (the formaction driver above);
       an `<include-fragment src>` is learned when its connectedCallback runs
       and fetches (the customElements.define upgrade loop in qjs_dom.c drives
       it against the real element — the CE-construction fix makes that
       element real, so the fetch carries the concrete src). Reading the
       attribute statically bypassed the engine and is exactly the shortcut
       the "NO parallel analysis" rule exists to prevent. */

    /* Script-preload hints — <link rel="modulepreload" href="…">,
       <link rel="preload" as="script" href="…">, <link rel="prefetch"
       as="script" href="…">. These declare ADDITIONAL bundle URLs the
       browser will load. Emit as `script` so the brain's chunkUrls
       download+re-analyze path picks them up (alongside chunks the
       webpack/rspack runtime would load via require.e). Without this,
       module-preloaded bundles on Microsoft Graph Explorer / Apple
       support pages / Google docs (all use modulepreload) had their
       extra modules invisible to the analyzer. */
    try {
      var links = document.querySelectorAll("link[rel]");
      for (var li = 0; li < links.length; li++) {
        var lEl = links[li];
        var rel = (lEl.getAttribute("rel") || "").toLowerCase();
        var href = lEl.getAttribute("href");
        if (!href) continue;
        var asAttr = (lEl.getAttribute("as") || "").toLowerCase();
        var isScriptHint = (rel === "modulepreload") ||
                           ((rel === "preload" || rel === "prefetch") && asAttr === "script");
        if (!isScriptHint) continue;
        var hintKey = "preload-script:" + href;
        if (ranKeys.has(hintKey)) continue;
        ranKeys.add(hintKey);
        H("script", [href, "GET"]);
      }
    } catch (e) {
      if (typeof printErr === "function")
        printErr('@WHY {"phase":"linkPreloadHints_throw","err":' + JSON.stringify(String(e && e.message || e)) + '}');
    }

    // Event-delegation coverage comes from the FH loop below, which
    // fires each registered handler ONCE with an OPAQUE event. A
    // delegated handler does `event.target.closest(".x")` /
    // `event.target.dataset.y` — on an opaque event those reads are
    // opaque, so forced execution FORKS on every branch and explores
    // both the matched and unmatched delegation paths. That covers the
    // delegated-handler code WITHOUT the previous brute-force pass
    // that dispatched every listener type on every one of the ~1900
    // github DOM elements: that pass was O(elements × types × tree
    // depth) handler executions (a delegated <html> handler ran once
    // per descendant), which is what hung __hostDrive on real pages.
    // The opaque-event forcing is the X-Force/J-Force-correct way to
    // reach those paths; concrete per-element dispatch would only add
    // event.target attribute VALUES, and those are attacker-opaque for
    // the security view and structural for the API view anyway.

    // J-Force §3.1.3 + multi-message coverage: fire every registered event
    // handler against fresh opaque events so multi-message PoCs accumulate in Φ.
    // Extracted to G.__feDrainHandlers (defined above) so the deep grind drains
    // it too — see that function for the structural-termination rationale.
    G.__feDrainHandlers();
  };
  var __feCBSeq = 0;
  try { Object.getOwnPropertyNames(G).forEach(function (k) { PRE[k] = 1; }); } catch (e) {}
})();
