/* Minimal QuickJS host: run script file args. Bypasses the cmake/qjsc
   REPL CLI (gen/*.c) — we only need: create rt/ctx, std helpers
   (print/console/std/os), eval each file as a global script. The
   forced-execution patch lives in quickjs.c; this just runs code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include "quickjs.h"
#include "quickjs-libc.h"

#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
#include <emscripten.h>
/* qjs_host_yield: cooperative pause point. The wasm engine calls this
   when it wants to release the JS thread back to the host (so the host
   can interleave another wasm fiber by priority, drain microtasks,
   etc.). Under JSPI the wasm CALL into this import transparently awaits
   a Promise — the wasm stack is suspended via real wasm stack switching
   (no save buffer, no depth cap). The host's resume policy decides
   WHEN to resolve: when the orchestrator gives this fiber a turn.
   See ast-thread.js Module.qjs_host_yield + _yieldDrain for the host
   impl. The build uses -fwasm-exceptions (not legacy -fexceptions) so
   indirect calls dispatch via pure-wasm `call_indirect`, never through
   emscripten invoke_* JS shims that would leave non-suspendable JS
   frames in the call chain. */
EM_ASYNC_JS(void, qjs_host_yield, (), {
    if (!Module.qjs_host_yield) return;
    await Module.qjs_host_yield();
});

/* qjs_load_module_host: in-run ESM module fetch. js_module_load (quickjs-libc.c)
   calls this when an `import` target is NOT yet staged in the in-memory map. The
   wasm stack suspends via JSPI (real stack switching, no depth cap — IDENTICAL
   mechanism to qjs_host_yield) while the host safeFetches the module + stages its
   SOURCE in _feMap under the SAME canonical id the loader reads; then the engine
   resumes and re-reads. This loads a deep ESM import graph (directus
   index→rest→commands→utils→auth) ON DEMAND in ONE analysis pass — no multi-round
   re-analysis, no link deadlock, no cap. module_name is a NUL-terminated wasm
   linear-memory pointer (MEMORY64: arrives as a BigInt i64, Number()-coerced for
   UTF8ToString). See ast-thread.js Module.qjs_load_module. */
EM_ASYNC_JS(void, qjs_load_module_host, (const char *module_name), {
    if (!Module.qjs_load_module) return;
    var nm = UTF8ToString(Number(module_name));
    await Module.qjs_load_module(nm);
});

/* qjs_host_digest: JSPI bridge to the host worker's real WebCrypto
   (Chromium's BoringSSL when the wasm runs in a Chrome extension). Called
   from qjs_dom.c's crypto.subtle.digest binding; the wasm stack suspends
   while the host's `self.crypto.subtle.digest(...)` Promise resolves, then
   resumes with the digest bytes written into the wasm linear-memory
   buffer at `out`. Returns the number of bytes written, or -1 on
   unsupported algorithm / failure. algName is a NUL-terminated wasm
   pointer; data points to dataLen bytes of input; out points to a buffer
   large enough for the algorithm's digest (caller sizes it from
   crypto_algo_size). algName/data/out are wasm linear-memory pointers,
   reachable via HEAPU8.subarray. */
EM_ASYNC_JS(int, qjs_host_digest, (const char *algName, const uint8_t *data, int dataLen, uint8_t *out), {
    // MEMORY64: algName/data/out arrive as BigInt (i64 wasm pointers). Convert to
    // Number up front for ALL HEAPU8 indexing + arithmetic (the heap is < 4 GiB so
    // each address fits a JS Number). `data + dataLen` (BigInt + Number) throws
    // "Cannot mix BigInt and other types"; the original code also called
    // UTF8ToString(algName) on the raw BigInt OUTSIDE the try, so that throw
    // propagated out of callMain as a wasm trap (not a catchable JS error) and
    // wedged the deep grind in an INFINITE recycle loop on every orphan whose
    // forced drive reached crypto.subtle.digest (observed live on learn.microsoft
    // .com, frozen at /index-docs.js:148). Converting + keeping every heap op
    // inside the try fixes the root cause. A crash is a host-glue bug to FIX, never
    // an orphan to bound/skip — fixing it lets the priority frontier complete.
    var algN = Number(algName), dataN = Number(data), outN = Number(out);
    try {
        var alg = UTF8ToString(algN);
        var input = HEAPU8.slice(dataN, dataN + dataLen);
        var buf = await self.crypto.subtle.digest(alg, input);
        var u8 = new Uint8Array(buf);
        HEAPU8.set(u8, outN);
        return u8.length;
    } catch (e) {
        /* Host WebCrypto rejection — surface the diagnostic via @WHY on
           stderr (printErr in the worker host wires it to self.stderr,
           which the brain exposes). Don't swallow: a failure here means
           a bundle's signature/HMAC header will be missing/wrong, and
           the reviewer needs to see WHICH algorithm + WHY. */
        var msg = (e && e.message) ? e.message : String(e);
        var rec = '@WHY {"phase":"host_digest_throw","alg":"' + alg + '","len":' + dataLen + ',"err":' + JSON.stringify(msg) + "}";
        if (typeof printErr === "function") printErr(rec);
        else if (typeof console !== "undefined") console.error(rec);
        return -1;
    }
});
#endif


/* qjs_load_script_begin / _take: the in-run, in-context external-script loader
   bridge (the one-message-per-document keystone). The forced-exec engine, on
   reaching an external <script src> (parsed from the Lexbor DOM) or a
   programmatically-inserted <script> whose src it discovered mid-drive, calls
   the native __feLoadScript binding (qjs_dom.c), which suspends the wasm stack
   via JSPI here while THIS worker safeFetches the page's own subresource — the
   same single chokepoint that loads source maps (_smGetParsed) + chunks;
   principal = the analysis sourceUrl. Two phases so the variable-length body is
   sized before the copy WITHOUT a JS-side _malloc (only HEAPU8 + UTF8ToString,
   both already in the worker's EM_JS scope): begin() suspends, fetches, stashes
   the UTF-8 bytes under a fresh integer HANDLE + returns it (-1 on
   failure/blocked); the C binding reads the length (qjs_load_script_len),
   js_malloc's len+1, and take() copies the bytes into wasm memory + frees the
   handle. The handle map (Module.__feLoad[id], id from a monotonic counter) is
   REENTRANCY-SAFE: under JSPI two fibers can be suspended in begin() at once
   (the multi-fiber deep-grind drives createElement-inserted <script> loads
   concurrently), and each gets its own id, so neither clobbers the other's
   bytes — unlike a single shared stash. begin() is the only suspending phase;
   len()/take() are synchronous. */
#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
EM_ASYNC_JS(int, qjs_load_script_begin, (const char *url), {
    if (!Module.qjs_load_script) return -1;
    try {
        var u = UTF8ToString(Number(url));
        if (!u) return -1;
        var code = await Module.qjs_load_script(u);
        if (code == null) return -1;
        var bytes = new TextEncoder().encode(code);
        if (!Module.__feLoad) { Module.__feLoad = {}; Module.__feLoadNext = 1; }
        var h = Module.__feLoadNext++;
        Module.__feLoad[h] = bytes;
        return h;
    } catch (e) {
        var rec = '@WHY {"phase":"qjs_load_script_begin_throw","err":' + JSON.stringify(String(e && e.message || e)) + "}";
        if (typeof printErr === "function") printErr(rec);
        return -1;
    }
});
EM_JS(int, qjs_load_script_len, (int h), {
    var b = Module.__feLoad && Module.__feLoad[h];
    return b ? b.length : -1;
});
EM_JS(void, qjs_load_script_take, (int h, uint8_t *out, int cap), {
    if (!Module.__feLoad) return;
    var b = Module.__feLoad[h];
    delete Module.__feLoad[h];
    if (!b || !out) return;
    var n = Math.min(cap, b.length);
    HEAPU8.set(b.subarray(0, Number(n)), Number(out));
});
#endif


/* --fe-timer: wall-clock instrumentation. Gated by an explicit argv flag so
   production runs (worker fast path) are byte-identical; the diagnostic
   profile path turns it on. Phase boundaries print one TIMER line each to
   stderr (the structured stdout channels @H/@T/@E/@DS stay clean). The
   metric is monotonic-clock elapsed-since-process-start in milliseconds —
   the only number a profile needs; pegging the dominant phase is what
   guides the surgical fix. */
static int g_qjs_timer = 0;
static struct timespec g_qjs_timer_t0;
static double qjs_timer_ms_since_start(void) {
    if (!g_qjs_timer) return 0.0;
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    double s = (double)(n.tv_sec - g_qjs_timer_t0.tv_sec);
    double ns = (double)(n.tv_nsec - g_qjs_timer_t0.tv_nsec);
    return s * 1000.0 + ns / 1.0e6;
}
static void qjs_timer_log(const char *phase, const char *label, double t0_ms) {
    if (!g_qjs_timer) return;
    double t1 = qjs_timer_ms_since_start();
    fprintf(stderr, "TIMER %s phase=%s dur_ms=%.2f t_abs_ms=%.2f\n",
            label ? label : "", phase, t1 - t0_ms, t1);
    fflush(stderr);
}

/* QuickJS<->Lexbor DOM binding (qjs_dom.c). */
int qjs_dom_install(JSContext *ctx);

/* Defined in quickjs.c's TU via quickjs-forced.h. */
void qjs_forced_config(int en, const char *sch, const char *tr);

/* Defined in qjs_dom.c — run the parsed document's scripts (inline + external
   <script src>) as DRIVEN bundle code. Called from the top-level boot loop AFTER
   /h.js parsed the HTML into the Lexbor DOM (NOT re-entrantly from inside a JS
   eval). Replaces the deleted hostedge SSR _eval phase. */
void qjs_run_doc_scripts(JSContext *ctx);
int qjs_settle_pending_promises(JSContext *ctx);   /* unwind await-on-never-settling-promise frames */
int qjs_free_suspended_generators(JSContext *ctx); /* unwind suspended-generator frames (same teardown leak) */

/* Unwind every suspended frame the runtime still holds before JS_FreeContext —
   a forced-exec `await <never-settling promise>` (resolve with opaque, running
   the continuation) and a suspended `function*` (a for-of over an opaque the
   consumer never exhausts; complete it, freeing the frame). Either kind pins a
   ctx ref → JS_FreeContext can't free the context → it roots the whole bundle →
   JS_FreeRuntime's list_empty(gc_obj_list) assert. Both the BOOT and DEEP ctx
   run the page's scripts (qjs_run_doc_scripts), so both teardowns need this;
   the per-batch grind-end sweep is skipped on a no-progress break, so the
   teardown is the GUARANTEED cleanup point. Loop with the job drain to a
   settle-nothing/free-nothing/drain-nothing fixpoint. */
void qjs_free_suspended_trampoline_frames(JSContext *ctx);   /* quickjs.c: heap call-stack teardown */

static void qjs_unwind_suspended(JSContext *ctx, JSRuntime *rt) {
    if (!ctx || !rt) return;
    int _tp = 0, _tg = 0;
    for (int _u = 0; _u < 64; _u++) {
        int _s = qjs_settle_pending_promises(ctx);
        int _g = qjs_free_suspended_generators(ctx);
        _tp += _s; _tg += _g;
        JSContext *_c; int _d = 0;
        while (JS_ExecutePendingJob(rt, &_c) > 0 && _d < 200000) _d++;
        if (_s == 0 && _g == 0 && _d == 0) break;
    }
    /* Promises/generators settled; now free any trampolined NORMAL frames still
       suspended on the heap call-stack (a per-call JSPI yield left them mid-run).
       The settle loop above can't reach them — they are neither — yet they pin the
       same ctx refs, so this must run BEFORE JS_FreeContext or the bundle stays
       rooted (the supabase list_empty(gc_obj_list) abort). */
    qjs_free_suspended_trampoline_frames(ctx);
    if (_tp || _tg) { printf("@WHY {\"phase\":\"teardown_unwind\",\"promises\":%d,\"gens\":%d}\n", _tp, _tg); fflush(stdout); }   /* observability: how many frames the teardown dropped */
}

/* Resumable/throttled deep orphan drive (quickjs.c). qjs_deep_step_c drives
   the next maxN unreached @T host functions and returns how many remain;
   qjs_deep_free releases its cached orphan list. Driven across callMains on a
   PERSISTENT runtime (g_deep_*) so the bundle boots once and the worker can
   sleep between batches — low CPU duty, no overheating. */
int qjs_deep_step_c(JSContext *ctx, int maxN, int fromCursor);
int qjs_deep_step_c_h(JSContext *ctx, int maxN, int fromCursor, int head_only);
int qjs_deep_cursor_c(void);
int qjs_deep_dnf_threw_c(void);
int qjs_deep_dnf_ret_c(void);
int qjs_deep_recv_thr_c(void);
int qjs_deep_recv_exc_kind_c(void);
const char *qjs_deep_recv_exc_msg_c(void);
int qjs_deep_syn_col_c(void);
int qjs_deep_syn_asn_c(void);
int qjs_deep_gen_susp_drv_c(void);
int qjs_deep_gen_susp_recv_c(void);
int qjs_deep_gen_susp_drained_c(void);
void qjs_deep_free(JSContext *ctx);
void qjs_deep_drain_jobs(JSContext *ctx);
void qjs_host_atoms_init(JSContext *ctx);
static JSRuntime *g_deep_rt = NULL;
static JSContext *g_deep_ctx = NULL;

/* Snapshot-restore SCHEDULE model (Wizer-style memory imaging). The X-Force
   schedule BFS re-boots a fresh wasm instance per schedule (mdrive.mjs /
   ast-thread.js), re-parsing+re-evaluating the whole bundle every time — the
   dominant per-schedule cost. Instead: boot the bundle ONCE into this
   persistent runtime (--fe-boot), the JS host snapshots the wasm LINEAR MEMORY
   (a position-independent buffer, so a byte copy is a coherent runtime image),
   then per schedule it RESTORES that image (memcpy back) and drives the bundle
   under that schedule (--fe-drive=<sched>) WITHOUT re-eval. Linear memory holds
   the entire C heap + QuickJS runtime + every JS object as offsets into the
   same base, so a full restore is bit-coherent post-boot state. Host-side state
   (stdout, MEMFS trace) lives outside linear memory and survives restore, so
   @H/trace accumulate per drive as before. The schedule cursor/Φ/seen reset in
   qjs_forced_config; the heap restore resets the bundle's mutated JS state. */
static JSRuntime *g_boot_rt = NULL;
static JSContext *g_boot_ctx = NULL;

/* __hostMicrotaskDrain() — drains the Promise microtask queue (calls
   JS_ExecutePendingJob until empty). Returns the number of jobs run.
   driver.js calls this inside its __hostDrive/__hostFlush fixpoint
   loop so fetch().then(cb) callbacks the bundle queues (Catalyst /
   Turbo / React effects on github) actually fire — without it the
   .then sits in the queue until after driver.js has exited, missing
   every microtask-chained fetch (the github discovery gap). The
   ranKeys identity-set in hostedge.js dedups any callbacks the drain
   fires, so the loop still converges. js_std_loop after the eval
   drains anything still pending — this just brings the drain INTO
   the driver loop where __hostDrive can react to newly-registered
   handlers. */
static JSValue js_host_microtask_drain(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSContext *ctx1;
    int n = 0;
    for (;;) {
        int err = JS_ExecutePendingJob(rt, &ctx1);
        if (err <= 0) break;
        n++;
    }
    return JS_NewInt32(ctx, n);
}

/* In-memory file bridge — replaces the worker's legacy inst.FS.* JS API, which WASMFS
   does NOT expose (no FORCE_FILESYSTEM, the legacy-compat layer we don't want). The
   worker stages a file's bytes in a JS-side Map (Module.__feMap: path → Uint8Array)
   passed INTO createQJS; the engine reads them STRAIGHT from that Map (readfile,
   js_load_file, qjs_dd_load, the .bc loader) and writes outputs (.bc via fe_map_set,
   the execution trace via fe_trace_append) back to JS — no fopen, no filesystem round-
   trip for JS-originated data. The 64-bit buffer pointers arrive as BigInt under
   MEMORY64, so the shims Number()-coerce them and BigInt()-coerce the i64 length
   return; genuine C POSIX I/O (std.open) still routes through WASMFS unchanged. */
EM_JS(long, fe_map_len, (const char *path), {
    var p = UTF8ToString(Number(path));
    var m = Module.__feMap;
    /* Reusable wiring diagnostic — SILENT on success, fires once per instance only when
       the in-memory map is missing/empty (the __feMap-not-merged-onto-Module failure that
       would dark every source read). Names the first requested path for context. */
    if (!Module.__feLogged && (!m || m.size === 0)) { Module.__feLogged = 1;
        err("@WHY {\"phase\":\"feMap\",\"hasMap\":" + (!!m) + ",\"size\":" + (m ? m.size : -1) + ",\"first\":" + JSON.stringify(p) + "}"); }
    if (!m) return -1n;
    var d = m.get(p);
    if (d === undefined) {
        Module.__feMiss = (Module.__feMiss || 0) + 1;
        if (Module.__feMiss <= 40) err("@WHY {\"phase\":\"feMiss\",\"path\":" + JSON.stringify(p) + ",\"size\":" + m.size + ",\"hasAuth\":" + (m.has("/@supabase/auth-js@2.108.1/es2022/auth-js.mjs")) + "}");
        return -1n;
    }
    return BigInt(d.length);
});
EM_JS(void, fe_map_copy, (const char *path, char *out), {
    var d = Module.__feMap.get(UTF8ToString(Number(path))); if (d) HEAPU8.set(d, Number(out));
});
EM_JS(void, fe_map_set, (const char *path, const char *data, long len), {
    if (!Module.__feMap) Module.__feMap = new Map();
    Module.__feMap.set(UTF8ToString(Number(path)), HEAPU8.slice(Number(data), Number(data) + Number(len)));
});
/* DIAG: identity (the host marks each instance's _feMap with __feId) + size of the
   map the ENGINE actually reads — to tell a host/engine map DESYNC (engine reads a
   different/fresh map than the host stages into) from a real key miss. */
EM_JS(long, fe_map_id, (void), { var m = Module.__feMap; return BigInt(m ? (m.__feId || 0) : -1); });
EM_JS(long, fe_map_size, (void), { var m = Module.__feMap; return BigInt(m ? m.size : -1); });
/* DIAG: crash-surviving counter on the worker global (self), used to settle whether
   the method trampoline FIRES on a given bundle when capped stderr @WHY logs are lost
   to an immediate-crash boot. self persists across instance recycles, so the worker can
   read self.__mtramp after the boot regardless of crashes. */
EM_JS(void, qjs_note_mtramp, (int line, int col), { try { self.__mtramp = (self.__mtramp | 0) + 1; var r = (self.__mtl || (self.__mtl = [])); r.push(Number(line) + ":" + Number(col)); if (r.length > 24) r.shift(); } catch (e) {} });
/* In-memory execution-trace sink (replaces the streaming `fopen` FILE* the forced
   controller used to fwrite B/F lines to). The engine appends each trace line here;
   the worker reads Module.__feTrace[path].join("") and never touches a filesystem.
   Keyed by path so /boot.tr and /t.tr stay separate. __feTrace is passed INTO
   createQJS (worker) so the EM_JS Module ref and the worker handle are one object. */
EM_JS(void, fe_trace_clear, (const char *path), {
    if (Module.__feTrace) delete Module.__feTrace[UTF8ToString(Number(path))];
});
EM_JS(void, fe_trace_append, (const char *path, const char *line), {
    if (!Module.__feTrace) Module.__feTrace = {};
    var p = UTF8ToString(Number(path));
    (Module.__feTrace[p] || (Module.__feTrace[p] = [])).push(UTF8ToString(Number(line)));
});
/* Read a slice STRAIGHT from the in-memory map into a malloc'd, NUL-terminated buffer
   — no fopen, no filesystem. The sources originate in JS (the worker's inMem), so
   reading them directly is the right path; fopen would only round-trip JS bytes
   through a backend. Non-static so the libc module loader (quickjs-libc.c) shares it.
   Returns NULL when the path isn't staged in Module.__feMap. */
char *fe_mem_read(const char *path, size_t *n) {
    long len = fe_map_len(path);
    if (len < 0) return NULL;
    char *b = (char *)malloc((size_t)len + 1);
    if (!b) return NULL;
    if (len > 0) fe_map_copy(path, b);
    b[len] = 0;
    if (n) *n = (size_t)len;
    return b;
}

static char *readfile(const char *p, size_t *n) {
    return fe_mem_read(p, n);
}

/* Spec-correct module dispatch (HTML §4.12.1): a <script> is a CLASSIC script
   (GLOBAL scope) unless it uses module-only syntax (import / export / top-level
   await). QuickJS's JS_DetectModule (quickjs.c) defaults AMBIGUOUS sources —
   valid as EITHER, i.e. nearly every minified bundle — to MODULE: it compiles
   the source AS a module and only returns false on a NON-import compile error,
   so any strict-clean script is mis-flagged a module. Run as a module, a
   slice's top-level `var X` is module-local, so a CDN <script src> bundle's
   `var Sentry` never becomes the global a later inline <script> reads (the
   measured cross-slice-global moat gap; an IIFE/UMD bundle with zero top-level
   import/export came back isModule:1). We probe the SCRIPT grammar instead: a
   source is a module IFF it CANNOT compile as a classic script (import /
   export / top-level await are script syntax errors). Ambiguous sources compile
   fine as a script -> CLASSIC — matching the <script> default and restoring
   top-level `this`=globalThis for UMD bundles. COMPILE_ONLY in the live ctx:
   no eval, no new runtime, side-effect-free; the pending compile error is
   cleared before return. */
static int qjs_source_is_module(JSContext *ctx, const char *src, size_t n,
                                const char *filename) {
    JSEvalOptions opt = { JS_EVAL_OPTIONS_VERSION,
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY,
                          filename ? filename : "<probe>", 1 };
    JSValue v = JS_Eval2(ctx, src, n, &opt);
    int is_mod = JS_IsException(v);
    if (is_mod) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
    JS_FreeValue(ctx, v);
    return is_mod;
}

/* Evaluate a script the way real browsers do: classic scripts run in
   GLOBAL scope; ES module scripts (`<script type="module">`, or any source
   with top-level `export`/`import`) compile+run in MODULE scope.

   Spec semantics (HTML §4.12.1, ESM §15.2): the dispatch isn't derivable
   from source text alone — the host carries the script's type. Our caller
   (the worker layer) writes each separate page-script to its own MEMFS
   file so we run them one-at-a-time and JS_DetectModule's MODULE-compile
   probe is a reliable per-script signal (a concatenated blob mixing
   classic + module syntax has neither valid MODULE nor valid GLOBAL
   parses, which was the original ZERO-endpoints failure mode on MDN /
   GitHub bundles).

   start_line carries the slice's starting line in the COMBINED-bundle
   space (1-based) so JS_Eval2's `line_num` makes stack frames report
   combined-bundle line numbers — downstream source-map resolution by
   `_findScriptForLine` (offscreen-brain) keeps working without a
   per-script lookup table.

   The MODULE branch mirrors qjs.c eval_buf: compile → set import.meta →
   eval → drain microtasks so top-level side-effects (the bundle's
   globalThis.X = Y, custom-element registrations, fetch IIFEs) actually
   run before we return to the driver loop. use_realpath=false:
   in-memory worker scripts have no filesystem path. */
/* Return-to-scheduler driver hooks (quickjs.c). */
JSValue qjs_resume_chain_call(JSContext *ctx);
void qjs_set_driving(JSContext *ctx, int v);
int  qjs_take_yielded(JSContext *ctx);

JSValue qjs_eval_script(JSContext *ctx, const char *src, size_t n,
                               const char *filename, int start_line) {
    if (start_line < 1) start_line = 1;
    /* @WHY breadcrumb: every per-file eval emits its filename so the
       reviewer can see whether the loop ever reached /d.js (driver) —
       a missing /d.js eval line is the signal that an earlier
       script's eval blocked the loop. Cheap stderr; doesn't pollute
       the stdout protocol. */
    /* Module-vs-classic dispatch via the script-grammar probe (see
       qjs_source_is_module): ambiguous sources -> CLASSIC/GLOBAL so a slice's
       top-level `var X` is a shared global the next slice reads. isModule is
       logged for verification (it should read 0 for a classic CDN bundle). */
    int _is_mod = qjs_source_is_module(ctx, src, n, filename);
    fprintf(stderr, "@WHY {\"phase\":\"eval_script\",\"file\":\"%s\",\"len\":%zu,\"isModule\":%d}\n",
            filename ? filename : "(null)", n, _is_mod);
    fflush(stderr);
    /* Set __feCurFile before the eval so the /p.js-installed
       document.currentScript live getter returns THIS script's
       element during ITS own top-level execution (matches HTML spec
       §4.12.1 currentScript semantics). Bundles read
       `document.currentScript.src` to derive their own URL / base
       path / publicPath — without this they see the wrong
       (always the last-loaded) element and the boot derivation
       fails. */
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__feCurFile", JS_NewString(ctx, filename));
        JS_FreeValue(ctx, g);
    }
    if (_is_mod) {
        JSEvalOptions opt = { JS_EVAL_OPTIONS_VERSION,
                              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY,
                              filename, start_line };
        JSValue v = JS_Eval2(ctx, src, n, &opt);
        if (JS_IsException(v)) return v;
        if (js_module_set_import_meta(ctx, v, false, true) < 0) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        /* Return-to-scheduler DRIVER. The module body + its async continuations
           yield-RETURN here (qjs_take_yielded) at the dispatch cadence instead of
           JSPI-suspending the heap chain (which can't be torn down). Drive to
           quiescence: deliver host work (qjs_host_yield: fetches + @H aggregate),
           drain microtasks, and RESUME any yield-returned chain
           (qjs_resume_chain_call). This replaces the old single microtask drain. */
        JSRuntime *rt = JS_GetRuntime(ctx);
        JSContext *ctx1;
        long _drained = 0;
        /* UNIVERSAL TRAMPOLINE (boot included): opcode-preemption ON (qjs_driving=1).
           The module body + every callee trampoline on the heap arena and yield-RETURN
           at the dispatch cadence (YIELD_POLL) instead of JSPI-suspending — so the
           post-boot HEAPU8 image is taken with the chain either fully returned or
           cleanly SUSPENDED (cur_sp set, freeable), never a "running" (cur_sp==NULL)
           orphan that the image would capture (the list_empty leak). Drive to
           quiescence in three nested loops:
             (a) resume the top-level body's parked chain to its real return;
             (b) drain microtasks — async resumes run as jobs and re-drive their own
                 parked chains inside js_async_function_resume;
             (c) catch a raw (non-async) job that itself yield-returned, and resume it.
           qjs_host_yield at each step delivers fetches / @H aggregate. */
        long _loopA = 0, _forIters = 0, _loopC = 0;   /* DIAG: spin localization (counters only, no cap) */
        qjs_set_driving(ctx, 1);
        fprintf(stderr, "@WHY {\"phase\":\"eval_evalfn_pre\",\"file\":\"%s\"}\n", filename ? filename : "(null)"); fflush(stderr);
        v = JS_EvalFunction(ctx, v);
        fprintf(stderr, "@WHY {\"phase\":\"eval_evalfn_post\",\"file\":\"%s\",\"exc\":%d,\"undef\":%d}\n",
                filename ? filename : "(null)", JS_IsException(v) ? 1 : 0, JS_IsUndefined(v) ? 1 : 0); fflush(stderr);
        while (qjs_take_yielded(ctx)) {           /* (a) top-level body → real return */
            qjs_host_yield();
            v = qjs_resume_chain_call(ctx);
            if ((++_loopA % 5000) == 0) { fprintf(stderr, "@WHY {\"phase\":\"eval_spin_A\",\"iters\":%ld}\n", _loopA); fflush(stderr); }
        }
        for (;;) {
            long _n = 0;
            qjs_host_yield();                     /* deliver fetches / host work */
            while (JS_ExecutePendingJob(rt, &ctx1) > 0) { _n++; _drained++; }   /* (b) */
            while (qjs_take_yielded(ctx)) {       /* (c) a raw job yield-returned */
                qjs_host_yield();
                qjs_resume_chain_call(ctx);
                _n++; _loopC++;
            }
            if ((++_forIters % 5000) == 0) { fprintf(stderr, "@WHY {\"phase\":\"eval_spin_BC\",\"forIters\":%ld,\"drained\":%ld,\"loopC\":%ld}\n", _forIters, _drained, _loopC); fflush(stderr); }
            if (_n == 0) break;                   /* no microtask ran → quiescent */
        }
        qjs_set_driving(ctx, 0);
        fprintf(stderr, "@WHY {\"phase\":\"eval_done\",\"file\":\"%s\",\"drained\":%ld,\"loopA\":%ld,\"forIters\":%ld,\"loopC\":%ld}\n",
                filename ? filename : "(null)", _drained, _loopA, _forIters, _loopC);
        fflush(stderr);
        return v;
    }
    JSEvalOptions opt = { JS_EVAL_OPTIONS_VERSION, JS_EVAL_TYPE_GLOBAL,
                          filename, start_line };
    JSValue _v = JS_Eval2(ctx, src, n, &opt);
    fprintf(stderr, "@WHY {\"phase\":\"eval_done\",\"file\":\"%s\",\"drained\":0}\n",
            filename ? filename : "(null)");
    fflush(stderr);
    return _v;
}

/* Same dispatch on the precompiled-bytecode path: a .bc emitted from an
   ESM source is restored as a JS_TAG_MODULE value, which needs
   set_import_meta + await around JS_EvalFunction. Classic-script .bc
   restores as a JS_TAG_FUNCTION_BYTECODE / JS_TAG_OBJECT; JS_EvalFunction
   runs it directly. Line numbers are baked into the bytecode at compile
   time, so the emit-bc site is what carries the per-slice start_line via
   JS_Eval2 — by the time we run the .bc here, stack frames already
   report combined-bundle line numbers. */
static JSValue qjs_eval_bc(JSContext *ctx, JSValue fn) {
    fprintf(stderr, "@WHY {\"phase\":\"eval_bc\",\"tag\":%d,\"exc\":%d}\n",
            (int)JS_VALUE_GET_TAG(fn), JS_IsException(fn) ? 1 : 0);
    fflush(stderr);
    if (JS_IsException(fn)) return fn;
    if (JS_VALUE_GET_TAG(fn) == JS_TAG_MODULE) {
        if (js_module_set_import_meta(ctx, fn, false, true) < 0) {
            JS_FreeValue(ctx, fn);
            return JS_EXCEPTION;
        }
        fprintf(stderr, "@WHY {\"phase\":\"eval_bc_evalfn_pre\"}\n"); fflush(stderr);
        JSValue v = JS_EvalFunction(ctx, fn);
        fprintf(stderr, "@WHY {\"phase\":\"eval_bc_evalfn_post\",\"exc\":%d}\n", JS_IsException(v) ? 1 : 0); fflush(stderr);
        /* Same rationale as qjs_eval_script's module branch: drain the
           microtask queue (side effects fire) but do not block on
           eval-promise resolution. js_std_await would pin a pending
           module-eval-promise's closure (~1800 GC objects) past
           JS_FreeContext, leaking the runtime. */
        JSRuntime *rt = JS_GetRuntime(ctx);
        JSContext *ctx1;
        long _drained = 0;
        while (JS_ExecutePendingJob(rt, &ctx1) > 0) { _drained++; }
        fprintf(stderr, "@WHY {\"phase\":\"eval_bc_done\",\"drained\":%ld,\"exc\":%d}\n",
                _drained, JS_IsException(v) ? 1 : 0);
        fflush(stderr);
        return v;
    }
    JSValue _v = JS_EvalFunction(ctx, fn);
    fprintf(stderr, "@WHY {\"phase\":\"eval_bc_done\",\"drained\":0,\"exc\":%d}\n",
            JS_IsException(_v) ? 1 : 0);
    fflush(stderr);
    return _v;
}

/* Per-slice start line in combined-bundle space (1-based). Populated from
   `--fe-script-start-lines=path1=N1,path2=N2,...` once at startup. The
   path is the MEMFS slice name (e.g. /b.0.js or /b.runtime.9f7.js); the
   integer is the slice's first line in the combined-bundle source. The
   alternative — purely positional CSV — would break the moment the
   worker switches a slice from numbered to basename naming, which it
   does to satisfy ESM cross-chunk imports. Dynamic allocation so a page
   with N <script> elements always gets all N attributions — no bound. */
typedef struct { char *path; int line; } qjs_start_line_t;
static qjs_start_line_t *g_bundle_start_lines = NULL;
static int g_bundle_start_line_count = 0;
static int g_bundle_start_line_cap = 0;

static void qjs_parse_start_lines(const char *csv) {
    /* Free any prior allocation (deep-grind re-callMain hits this again). */
    for (int i = 0; i < g_bundle_start_line_count; i++) free(g_bundle_start_lines[i].path);
    g_bundle_start_line_count = 0;
    if (!csv) return;
    const char *p = csv;
    while (*p) {
        const char *eq = p;
        while (*eq && *eq != '=' && *eq != ',') eq++;
        if (*eq != '=') break;   /* malformed segment — stop, don't guess */
        size_t plen = (size_t)(eq - p);
        char *end;
        long v = strtol(eq + 1, &end, 10);
        if (end == eq + 1) break;
        if (g_bundle_start_line_count >= g_bundle_start_line_cap) {
            int newcap = g_bundle_start_line_cap ? g_bundle_start_line_cap * 2 : 16;
            qjs_start_line_t *n = realloc(g_bundle_start_lines, newcap * sizeof(qjs_start_line_t));
            if (!n) return;
            g_bundle_start_lines = n;
            g_bundle_start_line_cap = newcap;
        }
        char *path = malloc(plen + 1);
        if (!path) return;
        memcpy(path, p, plen); path[plen] = 0;
        g_bundle_start_lines[g_bundle_start_line_count].path = path;
        g_bundle_start_lines[g_bundle_start_line_count].line = (int)v;
        g_bundle_start_line_count++;
        if (*end == ',') p = end + 1;
        else break;
    }
}

/* Custom ESM module loader: webpack/rspack bundles use bare-specifier
   `import "runtime.X.js"` (no leading dot or slash) for runtime-chunk
   references. The default libc loader js_load_file does `fopen(name)`
   which in emscripten MEMFS resolves relative to CWD, sometimes
   missing files in MEMFS root depending on FS state. Our worker writes
   the slice at `/runtime.X.js`, so prepending a single "/" makes the
   resolution reliable. Spec-correct: HTML modules § normalize relative
   names against the importer's URL (we model the importer's location
   as MEMFS root for the analyzed-bundle namespace). Falls back to the
   library loader for any name that already starts with "/" or "./". */
extern JSModuleDef *js_module_loader(JSContext *ctx, const char *module_name,
                                     void *opaque, JSValueConst attributes);
/* Custom module normalizer: makes "/<basename>" the canonical name for
   every bundle script so per-file iteration and import resolution agree.
   Without this, per-file eval registers a module under "/runtime.X.js"
   (the MEMFS path) but a bare-specifier `import "runtime.X.js"` resolves
   to the un-prefixed "runtime.X.js" — js_find_loaded_module misses the
   existing record and the libc loader creates a SECOND module record
   (10k+ duplicate JSObjects/bytecodes in the GC residue). The
   normalizer runs BEFORE the dedup lookup so both paths agree.

   Specifier shapes:
     - "/foo.js"           → keep as is (already absolute MEMFS path)
     - "./foo.js"          → resolve "foo.js" relative to base_name's dir
     - "../foo.js"         → strip one segment from base_name's dir, then foo.js
     - "foo.js" (bare)     → "/foo.js" (canonical MEMFS path)
   No regex/scope-matching; pure path string normalization per the WHATWG
   URL §relative resolution algorithm restricted to filesystem-style
   identifiers. */
static char *qjs_module_normalize(JSContext *ctx, const char *base_name,
                                  const char *name, void *opaque) {
    (void)opaque;
    if (!name || !name[0]) return js_strdup(ctx, name ? name : "");
    /* Fetched-CDN-module identity is a URL ENCODED as "/x/<host><pathname>" (scheme +
       ?query/#frag stripped) — collision-free (unlike a basename: esm.sh ships many
       same-basename modules) and relative imports resolve against it by path ops.
       Already-encoded "/x/…", inline "/b.N.js", and infra "/h.js"/"/d.js"/"/p.js"/
       "/pre.js" slice paths pass through untouched. */
    if (!strncmp(name, "/x/", 3) || !strncmp(name, "/b.", 3) ||
        !strcmp(name, "/h.js") || !strcmp(name, "/d.js") ||
        !strcmp(name, "/p.js") || !strcmp(name, "/pre.js"))
        return js_strdup(ctx, name);
    if (!strncmp(name, "http://", 7) || !strncmp(name, "https://", 8)) {
        /* Discovery emit (normalize sees every resolved import's raw specifier — a
           reliable complement to js_resolve_module's all-imports pass), then encode. */
        printf("@MODURL %s\n", name);
        fflush(stdout);
        const char *p = name + (name[4] == ':' ? 7 : 8);   /* skip "http://" / "https://" */
        size_t hlen = strcspn(p, "?#");                     /* host + path, drop query/frag */
        char *out = js_malloc(ctx, hlen + 4);
        if (!out) return NULL;
        out[0] = '/'; out[1] = 'x'; out[2] = '/';
        memcpy(out + 3, p, hlen); out[hlen + 3] = '\0';
        return out;
    }
    if (name[0] == '/') {
        /* Absolute-path import (esm.sh: a fetched module importing "/@supabase/auth-js.mjs"
           relative to its own origin). Resolve against the importer's ORIGIN: base_name is
           the importer's encoded "/x/<host>/…", so keep its "/x/<host>" prefix + the abs path. */
        if (base_name && !strncmp(base_name, "/x/", 3)) {
            const char *hs = strchr(base_name + 3, '/');   /* first '/' after the host */
            size_t olen = hs ? (size_t)(hs - base_name) : strlen(base_name);
            size_t nlen = strlen(name);
            char *out = js_malloc(ctx, olen + nlen + 1);
            if (!out) return NULL;
            memcpy(out, base_name, olen);
            memcpy(out + olen, name, nlen + 1);
            return out;
        }
        return js_strdup(ctx, name);   /* real absolute MEMFS path (page-relative) */
    }
    if (name[0] != '.') {
        size_t nlen = strlen(name);
        char *abs = js_malloc(ctx, nlen + 2);
        if (!abs) return NULL;
        abs[0] = '/';
        memcpy(abs + 1, name, nlen + 1);
        return abs;
    }
    /* Relative ("./foo.js" or "../foo.js"). Compute base directory from
       base_name, then walk the relative segments. */
    size_t blen = base_name ? strlen(base_name) : 0;
    const char *bslash = base_name ? strrchr(base_name, '/') : NULL;
    size_t bdir_len = bslash ? (size_t)(bslash - base_name) : 0;
    size_t cap = bdir_len + strlen(name) + 2;
    char *out = js_malloc(ctx, cap);
    if (!out) return NULL;
    memcpy(out, base_name ? base_name : "", bdir_len);
    out[bdir_len] = '\0';
    const char *p = name;
    while (*p) {
        if (p[0] == '.' && p[1] == '/') { p += 2; }
        else if (p[0] == '.' && p[1] == '.' && p[2] == '/') {
            char *prev = strrchr(out, '/');
            if (prev) *prev = '\0';
            else out[0] = '\0';
            p += 3;
        } else break;
    }
    size_t out_len = strlen(out);
    out[out_len++] = '/';
    strcpy(out + out_len, p);
    /* If the relative walk started from a RAW-URL base (a module whose own name is
       "https://host/…", not yet "/x/host/…"), the result is a raw URL too. Re-encode it
       to the canonical "/x/<host><path>" form — the SAME encoding the https:// branch
       above applies to a direct import — so this module dedups (js_find_loaded_module,
       by name) with the identical file reached via a /x/ base. Without this, directus's
       utils/error.js loads under BOTH /x/.../error.js and https://.../error.js → two
       JSModuleDefs → the duplicate's module function is C-held but NOT in loaded_modules
       (so JS_MarkContext can't mark it) → an external root at teardown that blocks the
       gc_obj_list cycle collection → the deep-grind FreeRuntime assert + lost progress. */
    if (!strncmp(out, "http://", 7) || !strncmp(out, "https://", 8)) {
        const char *q = out + (out[4] == ':' ? 7 : 8);   /* skip scheme */
        size_t qlen = strcspn(q, "?#");                   /* host + path, drop query/frag */
        char *enc = js_malloc(ctx, qlen + 4);
        if (enc) {
            enc[0] = '/'; enc[1] = 'x'; enc[2] = '/';
            memcpy(enc + 3, q, qlen); enc[qlen + 3] = '\0';
            js_free(ctx, out);
            return enc;
        }
    }
    return out;
}

extern JSModuleDef *js_module_loader(JSContext *ctx, const char *module_name,
                                     void *opaque, JSValueConst attributes);
/* Module loader: the normalizer already canonicalised the name to a
   MEMFS-absolute path, so just delegate to the libc loader. The dedup
   in js_host_resolve_imported_module (which runs BEFORE the loader)
   means this fires at most once per canonical name. */
static JSModuleDef *qjs_module_loader_hook(JSContext *ctx, const char *module_name,
                                           void *opaque, JSValueConst attributes) {
    /* CDN-URL import discovery is js_resolve_module's up-front pass (it emits the
       raw URL before this loader runs); qjs_module_normalize then maps the URL to
       the downloaded slice's basename, so by here module_name is already MEMFS-
       local and js_module_loader resolves it directly. */
    return js_module_loader(ctx, module_name, opaque, attributes);
}

/* Look up the slice start line for a MEMFS slice path. Matches the .js
   AND its .bc counterpart (the precompile step writes /b.X.bc; the
   driver later runs it under the same logical name). Returns 1 when no
   mapping exists, so /h.js / /d.js / /pre.js eval at line 1 of their
   own file as expected. */
static int qjs_lookup_start_line(const char *path) {
    if (!path) return 1;
    size_t pl = strlen(path);
    /* Match either an exact path or the .bc sibling of a .js entry. */
    int is_bc = (pl > 3 && path[pl-3] == '.' && path[pl-2] == 'b' && path[pl-1] == 'c');
    for (int i = 0; i < g_bundle_start_line_count; i++) {
        const char *p = g_bundle_start_lines[i].path;
        size_t pl2 = strlen(p);
        if (pl == pl2 && memcmp(p, path, pl) == 0) return g_bundle_start_lines[i].line;
        if (is_bc && pl == pl2 && pl >= 3) {
            /* compare ignoring the .bc/.js trailing 3 chars */
            if (memcmp(p, path, pl - 3) == 0 && p[pl2-3] == '.' && p[pl2-2] == 'j' && p[pl2-1] == 's')
                return g_bundle_start_lines[i].line;
        }
    }
    return 1;
}

/* Eval one script (or .bc) file into ctx for the snapshot-schedule modes
   (--fe-boot / --fe-drive); an uncaught throw surfaces as a stdout @E with the
   real message (host-model-gap signal), never silently swallowed. */
static int qjs_se_eval_one(JSContext *ctx, const char *path) {
    size_t n; char *src = readfile(path, &n);
    if (!src) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    size_t alen = strlen(path);
    JSValue v;
    if (alen > 3 && !strcmp(path + alen - 3, ".bc")) {
        JSValue fn = JS_ReadObject(ctx, (const uint8_t *)src, n, JS_READ_OBJ_BYTECODE);
        v = qjs_eval_bc(ctx, fn);
    } else {
        v = qjs_eval_script(ctx, src, n, path, qjs_lookup_start_line(path));
    }
    free(src);
    int rc = 0;
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        JSValue o = JS_NewObject(ctx);
        const char *em = JS_ToCString(ctx, e);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        JS_SetPropertyStr(ctx, o, "file", JS_NewString(ctx, path));
        JS_SetPropertyStr(ctx, o, "message", JS_NewString(ctx, em ? em : "(throw)"));
        JS_SetPropertyStr(ctx, o, "stack", JS_IsUndefined(st) ? JS_NewString(ctx, "") : JS_DupValue(ctx, st));
        JSValue js = JS_JSONStringify(ctx, o, JS_UNDEFINED, JS_UNDEFINED);
        const char *js_s = JS_ToCString(ctx, js);
        printf("@E %s\n", js_s ? js_s : "{\"message\":\"(unprintable)\"}");
        fflush(stdout);
        if (js_s) JS_FreeCString(ctx, js_s);
        if (em) JS_FreeCString(ctx, em);
        JS_FreeValue(ctx, js); JS_FreeValue(ctx, o); JS_FreeValue(ctx, st); JS_FreeValue(ctx, e);
        rc = 1;
    }
    JS_FreeValue(ctx, v);
    return rc;
}

int main(int argc, char **argv) {
    /* Persistent-runtime DEEP orphan drive. Two modes share the same persistent
       wasm runtime (the bundle boots ONCE, the residue set + driven-SET survive
       across calls):
         --fe-deep-grind <files…>  — drive ALL remaining orphans in ONE call,
            yielding per-orphan via JSPI (qjs_host_yield) so the host scheduler
            can rotate fibers across pages by priority.js's flowCmp. The cool-
            CPU duty cycle now lives in the JSPI scheduler's macrotask sleep,
            not in a JS-side outer-loop sleep between callMains. This is the
            normal path.
         --fe-deep-step=N <files…>  — drive the next N orphans then return.
            Legacy/debug path; kept so existing tests and harness probes that
            advance the residue one batch at a time still work.
         --fe-deep-end             — free the persistent runtime. */
    {
        int deep_step = -1, deep_end = 0, deep_from = -1, deep_grind = 0, deep_head = 0;
        for (int i = 1; i < argc; i++) {
            if (!strncmp(argv[i], "--fe-deep-step=", 15)) deep_step = atoi(argv[i] + 15);
            else if (!strncmp(argv[i], "--fe-deep-from=", 15)) deep_from = atoi(argv[i] + 15);
            else if (!strcmp(argv[i], "--fe-deep-end")) deep_end = 1;
            else if (!strcmp(argv[i], "--fe-deep-grind")) deep_grind = 1;
            /* Head-first scheduling: drive ONLY the net-reaching (endpoint) HEAD
               then return, so the worker can rotate to another open page's head
               before grinding THIS page's completeness tail (continuous-session
               scheduler). Same drive as --fe-deep-grind but qjs_deep_step_c
               stops once no net-reaching orphan remains undriven. */
            else if (!strcmp(argv[i], "--fe-deep-grind-head")) { deep_grind = 1; deep_head = 1; }
        }
        if (deep_end) {
            if (g_deep_ctx) {
                qjs_deep_free(g_deep_ctx);
                /* Drop unsettled Promise/fetch().then jobs the bundle's async
                   init left in the queue BEFORE freeing — they hold object
                   refs (→ JS_FreeRuntime's gc_obj_list-not-empty assert) and
                   settle during the free sweep (→ JS_EnqueueJob in_free
                   assert). learn.microsoft.com's heavy async init trips both;
                   github doesn't. Spec-legitimate teardown drain, not a GC mask. */
                qjs_deep_drain_jobs(g_deep_ctx);
                qjs_unwind_suspended(g_deep_ctx, g_deep_rt);   /* drop suspended-frame ctx refs before free */
                /* Free os timers/handlers BEFORE the context (canonical qjs.c order):
                   a still-pending os.setTimeout holds its callback (+ captured scope)
                   AND a ctx ref, so freeing handlers AFTER JS_FreeContext strands the
                   context and roots the whole bundle → JS_FreeRuntime's gc_obj_list
                   assert. directus's SDK leaves such a timer at deep-end teardown. */
                js_std_free_handlers(g_deep_rt);
                JS_FreeContext(g_deep_ctx);
                JS_FreeRuntime(g_deep_rt);
                g_deep_ctx = NULL; g_deep_rt = NULL;
            }
            return 0;
        }
        if (deep_step >= 0 || deep_grind) {
            if (!g_deep_ctx) {
                g_deep_rt = JS_NewRuntime();
                /* The JS recursion guard (js_check_stack_overflow) must throw a
                   CATCHABLE RangeError before the real wasm C-stack (8 MB,
                   -sSTACK_SIZE) is exhausted — otherwise a deeply-recursive
                   driven orphan (forced exec drives a recursive fn whose base
                   case never concretizes) overflows the C-stack into an
                   UNCATCHABLE wasm trap that poisons the instance, and the
                   grind's recycle abandons every remaining orphan (rem:-1).
                   Default stack_size is 1 MB (quickjs.h) — far under the 8 MB
                   wasm stack, AND its baseline stack_top is captured here at
                   NewRuntime; under JSPI each per-orphan resume runs on a
                   different physical stack, so that baseline goes stale. Size
                   the guard to 6 MB (8 MB wasm − ~2 MB headroom for host/JSPI
                   frames); JS_UpdateStackTop is re-called per orphan drive
                   (qjs_deep_step_c) so the limit tracks the live frame. This
                   is the spec stack guard made ACCURATE, not a depth cap. */
                JS_SetMaxStackSize(g_deep_rt, 6 * 1024 * 1024);
                js_std_init_handlers(g_deep_rt);
                JS_SetModuleLoaderFunc2(g_deep_rt, qjs_module_normalize, qjs_module_loader_hook,
                                        js_module_check_attributes, NULL);
                g_deep_ctx = JS_NewContext(g_deep_rt);
                js_std_add_helpers(g_deep_ctx, argc - 1, argv + 1);
                js_init_module_std(g_deep_ctx, "std");
                js_init_module_os(g_deep_ctx, "os");
                qjs_dom_install(g_deep_ctx);
                {
                    JSValue g = JS_GetGlobalObject(g_deep_ctx);
                    JS_SetPropertyStr(g_deep_ctx, g, "__hostMicrotaskDrain",
                                      JS_NewCFunction(g_deep_ctx, js_host_microtask_drain,
                                                      "__hostMicrotaskDrain", 0));
                    JS_FreeValue(g_deep_ctx, g);
                }
                qjs_forced_config(1, "", NULL);
                qjs_host_atoms_init(g_deep_ctx);   /* before boot, so qjs_h_fired marks correctly */
                /* Boot once: eval the bundle + driver so the reached set is
                   marked qjs_executed; the stepped drive then hits only the
                   never-reached residue. Parse --fe-script-start-lines first
                   so qjs_lookup_start_line returns the right per-slice line
                   when eval-ing /b.N.js (deep-grind has its own callMain so
                   the main-path parse above doesn't apply). */
                for (int i = 1; i < argc; i++) {
                    if (!strncmp(argv[i], "--fe-script-start-lines=", 24))
                        qjs_parse_start_lines(argv[i] + 24);
                }
                for (int i = 1; i < argc; i++) {
                    if (!strncmp(argv[i], "--fe-", 5)) continue;
                    size_t n; char *src = readfile(argv[i], &n);
                    if (!src) continue;
                    size_t alen = strlen(argv[i]);
                    JSValue v;
                    if (alen > 3 && !strcmp(argv[i] + alen - 3, ".bc")) {
                        JSValue fn = JS_ReadObject(g_deep_ctx, (const uint8_t *)src, n, JS_READ_OBJ_BYTECODE);
                        v = qjs_eval_bc(g_deep_ctx, fn);
                    } else {
                        v = qjs_eval_script(g_deep_ctx, src, n, argv[i], qjs_lookup_start_line(argv[i]));
                    }
                    free(src);
                    if (JS_IsException(v)) { JSValue e = JS_GetException(g_deep_ctx); JS_FreeValue(g_deep_ctx, e); }
                    JS_FreeValue(g_deep_ctx, v);
                }
            }
            /* SSR-phase replacement (deep-grind boot): /h.js parsed the HTML; run
               the document's scripts as DRIVEN code so the residue includes their
               instance-bound methods, top-level (not re-entrant). */
            qjs_run_doc_scripts(g_deep_ctx);
            /* Fire page-lifecycle events (DOMContentLoaded/load/pageshow) in the DEEP
               ctx so DEFERRED, event-gated inits RUN and build their transports —
               otherwise the residue grind drives their send()/request() COLD (no real
               transport instance on the heap to capture) and the URL stays opaque.
               gitlab.com: Sentry.init + the Apollo client boot on DOMContentLoaded/app-
               mount, so __SENTRY__ was never created in the deep ctx (gap2 probe:
               sentry:0 even at rem:0) and the envelope + /api/graphql stayed [object
               Object]. The BOOT ctx fires these via driver.js's __hostFlush; the deep
               ctx (a separate recycled instance) must fire them itself, AFTER the doc
               scripts registered their handlers and BEFORE the residue grind. Drain
               queued jobs so the inits' async setup (transport creation) completes. */
            {
                /* Complete the doc scripts' DEFERRED inits before the grind, via a
                   drive/flush/pump FIXPOINT (mirrors driver.js). __hostFlush alone
                   only fires page-lifecycle events — enough for a DOMContentLoaded-
                   gated init, but NOT for an `await loadChunk()`-gated transport: the
                   post-await global (e.g. createClient) is defined in the doc scripts
                   (qjs_run_doc_scripts above), which run AFTER driver.js already did
                   its one __hostDrive, so NOTHING ever drives createClient before the
                   grind — it then drives the transport's send() COLD (opaque
                   options.url, the gitlab sentry-envelope / factory shape). __hostDrive
                   force-invokes the doc-script globals so createClient RUNS and builds
                   the real transport on the heap (captured for the residue drive);
                   __hostFlush drives the chunk onload (resolves the await) and lifecycle
                   handlers; the pending-job drain runs the resumed continuations. Loop
                   until __hostRan stops growing AND no job drained AND nothing settles
                   (quiescence) — the same fixpoint as driver.js. UNBOUNDED: quiescence,
                   not a count, is the stop (the prior _it<64 / _dd<200000 backstops were
                   caps that truncated distinct driving — removed). */
                JSValue _gg = JS_GetGlobalObject(g_deep_ctx);
                JSValue _hd = JS_GetPropertyStr(g_deep_ctx, _gg, "__hostDrive");
                JSValue _hf = JS_GetPropertyStr(g_deep_ctx, _gg, "__hostFlush");
                JSValue _hr = JS_GetPropertyStr(g_deep_ctx, _gg, "__hostRan");
                int _prevRan = -1;
                /* Unbounded: the quiescence check below (no new __hostRan, no job
                   drained, nothing settling) is the ONLY stop — a count backstop
                   here would TRUNCATE distinct driving the fixpoint hasn't reached
                   yet. __hostRan is monotone over the bundle's finite function set,
                   so quiescence is reached without a count. */
                for (;;) {
                    if (JS_IsFunction(g_deep_ctx, _hd)) {
                        JSValue _r = JS_Call(g_deep_ctx, _hd, _gg, 0, NULL);
                        if (JS_IsException(_r)) { JSValue _e = JS_GetException(g_deep_ctx); JS_FreeValue(g_deep_ctx, _e); }
                        JS_FreeValue(g_deep_ctx, _r);
                    }
                    if (JS_IsFunction(g_deep_ctx, _hf)) {
                        JSValue _r = JS_Call(g_deep_ctx, _hf, _gg, 0, NULL);
                        if (JS_IsException(_r)) { JSValue _e = JS_GetException(g_deep_ctx); JS_FreeValue(g_deep_ctx, _e); }
                        JS_FreeValue(g_deep_ctx, _r);
                    }
                    JSContext *_c1; int _dd = 0;
                    while (JS_ExecutePendingJob(g_deep_rt, &_c1) > 0) _dd++;   /* drain to empty — a count would strand queued continuations */
                    int _ran = -1;
                    if (JS_IsFunction(g_deep_ctx, _hr)) {
                        JSValue _rv = JS_Call(g_deep_ctx, _hr, _gg, 0, NULL);
                        JS_ToInt32(g_deep_ctx, &_ran, _rv);
                        JS_FreeValue(g_deep_ctx, _rv);
                    }
                    if (_ran == _prevRan && _dd == 0) {
                        /* Natural quiescence: drive+flush+drain reached a fixpoint,
                           yet a suspended `await <pending promise>` (a webpack
                           chunk-load whose onload never fires, etc.) still pins its
                           async frame — __hostFlush can't fire that event. Fulfill
                           those never-settling promises with OPAQUE so the frames
                           UNWIND: the continuation past the await RUNS (coverage —
                           the lazy Sentry.init / Apollo client builds its real
                           transport) and no frame survives to hold the ctx ref at
                           teardown (the gc_obj_list leak → rem:-1 grind stop). Loop
                           again to drain the resume reactions; only truly done when
                           nothing remains to settle. */
                        int _settledN = qjs_settle_pending_promises(g_deep_ctx);
                        int _gensN = qjs_free_suspended_generators(g_deep_ctx);
                        if (_settledN == 0 && _gensN == 0) break;
                        _prevRan = -1;   /* re-pump: run the resumed continuations */
                    } else {
                        _prevRan = _ran;
                    }
                }
                JS_FreeValue(g_deep_ctx, _hr);
                JS_FreeValue(g_deep_ctx, _hf);
                JS_FreeValue(g_deep_ctx, _hd);
                JS_FreeValue(g_deep_ctx, _gg);
            }
            /* grind mode → unbounded; engine drives every remaining orphan,
               yielding per-orphan via JSPI so the host scheduler can rotate
               fibers. step mode → bounded batch (legacy/debug path). */
            int maxN = deep_grind ? INT_MAX : (deep_step > 0 ? deep_step : 1);
            int rem = qjs_deep_step_c_h(g_deep_ctx, maxN, deep_from, deep_head);
            printf("@DS {\"rem\":%d,\"cur\":%d,\"dnfThrew\":%d,\"dnfRet\":%d,\"head\":%d,\"gsDrv\":%d,\"gsRecv\":%d,\"gsDrn\":%d,\"recvThr\":%d,\"recvExcK\":%d,\"recvExcMsg\":\"%s\",\"synCol\":%d,\"synAsn\":%d}\n",
                   rem, qjs_deep_cursor_c(), qjs_deep_dnf_threw_c(), qjs_deep_dnf_ret_c(), deep_head,
                   qjs_deep_gen_susp_drv_c(), qjs_deep_gen_susp_recv_c(), qjs_deep_gen_susp_drained_c(), qjs_deep_recv_thr_c(), qjs_deep_recv_exc_kind_c(), qjs_deep_recv_exc_msg_c(), qjs_deep_syn_col_c(), qjs_deep_syn_asn_c());
            fflush(stdout);
            return 0;
        }
    }
    /* Snapshot-restore SCHEDULE modes (see g_boot_* above). --fe-boot evals the
       given files (hostedge + bundle) into the persistent g_boot_ctx and returns
       WITHOUT driving/freeing; the JS host snapshots linear memory. --fe-drive=
       <sched> reconfigures the forced schedule and evals the given file (driver)
       on g_boot_ctx — the host restores the memory image between drives.
       --fe-boot-end frees it. */
    {
        int do_boot = 0, do_boot_end = 0, do_drive = 0;
        const char *drive_sched = "", *drive_trace = NULL, *boot_sched = "";
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--fe-boot")) do_boot = 1;
            else if (!strcmp(argv[i], "--fe-boot-end")) do_boot_end = 1;
            else if (!strncmp(argv[i], "--fe-drive=", 11)) { do_drive = 1; drive_sched = argv[i] + 11; }
            else if (!strncmp(argv[i], "--fe-sched=", 11)) boot_sched = argv[i] + 11;
            else if (!strncmp(argv[i], "--fe-trace=", 11)) drive_trace = argv[i] + 11;
        }
        if (do_boot_end) {
            if (g_boot_ctx) {
                qjs_deep_drain_jobs(g_boot_ctx);
                qjs_unwind_suspended(g_boot_ctx, g_boot_rt);   /* drop suspended-frame ctx refs before free */
                js_std_free_handlers(g_boot_rt);   /* timers/handlers before the context (canonical order; a pending timer's ctx ref strands the context otherwise) */
                JS_FreeContext(g_boot_ctx);
                JS_FreeRuntime(g_boot_rt);
                g_boot_ctx = NULL; g_boot_rt = NULL;
            }
            return 0;
        }
        if (do_boot) {
            /* Re-boot for a NEW bootSched (the hybrid path: a module-init opaque
               branch needs module-init re-run under a flipped decision). Free
               the prior image's runtime first — a stale early-return here would
               re-image the OLD bootSched and the module-init-gated sink/endpoint
               would never fire (observed: _xss_neg's 3 top-level gates silently
               lost). */
            if (g_boot_ctx) {
                qjs_deep_drain_jobs(g_boot_ctx);
                qjs_unwind_suspended(g_boot_ctx, g_boot_rt);   /* drop suspended-frame ctx refs before free */
                js_std_free_handlers(g_boot_rt);   /* timers/handlers before the context (canonical order; a pending timer's ctx ref strands the context otherwise) */
                JS_FreeContext(g_boot_ctx);
                JS_FreeRuntime(g_boot_rt);
                g_boot_ctx = NULL; g_boot_rt = NULL;
            }
            g_boot_rt = JS_NewRuntime();
            js_std_init_handlers(g_boot_rt);
            JS_SetModuleLoaderFunc2(g_boot_rt, qjs_module_normalize, qjs_module_loader_hook,
                                    js_module_check_attributes, NULL);
            g_boot_ctx = JS_NewContext(g_boot_rt);
            js_std_add_helpers(g_boot_ctx, argc - 1, argv + 1);
            js_init_module_std(g_boot_ctx, "std");
            js_init_module_os(g_boot_ctx, "os");
            qjs_dom_install(g_boot_ctx);
            {
                JSValue g = JS_GetGlobalObject(g_boot_ctx);
                JS_SetPropertyStr(g_boot_ctx, g, "__hostMicrotaskDrain",
                                  JS_NewCFunction(g_boot_ctx, js_host_microtask_drain, "__hostMicrotaskDrain", 0));
                JS_FreeValue(g_boot_ctx, g);
            }
            /* Boot under the BOOT schedule (--fe-sched=, default empty). Empty
               ⇒ module-init takes default decisions ⇒ post-boot state is the
               base image every drive restores. A NON-empty boot schedule is
               the hybrid path: a module-init opaque branch (boot frontier) is
               re-explored by re-booting with that decision forced, so module-
               init-gated endpoints/sinks (e.g. `if(location.hash==="admin")…`
               at top level) are NOT lost — the snapshot images that flipped
               boot, and drives restore IT. The boot trace records module-init
               F-frontiers so the host enumerates exactly these re-boot
               schedules; nothing module-top is silently fixed. */
            qjs_forced_config(1, boot_sched, drive_trace);
            qjs_host_atoms_init(g_boot_ctx);   /* before boot, so qjs_h_fired marks correctly */
            for (int i = 1; i < argc; i++)
                if (!strncmp(argv[i], "--fe-script-start-lines=", 24)) qjs_parse_start_lines(argv[i] + 24);
            int rc = 0;
            for (int i = 1; i < argc; i++) {
                if (!strncmp(argv[i], "--fe-", 5)) continue;
                if (qjs_se_eval_one(g_boot_ctx, argv[i])) rc = 1;
            }
            /* SSR-phase replacement: /h.js parsed the HTML into the Lexbor DOM
               above; now run the document's scripts as DRIVEN bundle code, from
               this TOP-LEVEL boot frame (not re-entrantly from inside a JS eval). */
            qjs_run_doc_scripts(g_boot_ctx);
            /* Settle module-init microtasks so the snapshot captures quiescent
               boot state (mirrors the normal path's post-eval js_std_loop). */
            if (rc == 0) js_std_loop(g_boot_ctx);
            return rc;
        }
        if (do_drive) {
            if (!g_boot_ctx) { printf("@E {\"phase\":\"drive_no_boot\"}\n"); fflush(stdout); return 1; }
            qjs_forced_config(1, drive_sched, drive_trace);
            int rc = 0;
            for (int i = 1; i < argc; i++) {
                if (!strncmp(argv[i], "--fe-", 5)) continue;
                if (qjs_se_eval_one(g_boot_ctx, argv[i])) rc = 1;
            }
            if (rc == 0) js_std_loop(g_boot_ctx);
            fflush(stdout);
            return rc;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fe-timer")) {
            g_qjs_timer = 1;
            clock_gettime(CLOCK_MONOTONIC, &g_qjs_timer_t0);
            break;
        }
    }
    double t_init0 = qjs_timer_ms_since_start();
    JSRuntime *rt = JS_NewRuntime();
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, qjs_module_normalize, qjs_module_loader_hook,
                            js_module_check_attributes, NULL);
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, argc - 1, argv + 1);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    qjs_timer_log("init_rt_ctx", "(host)", t_init0);
    /* --fe-emit-bc only COMPILES (JS_EVAL_FLAG_COMPILE_ONLY) — it never runs
       the bundle, so it needs no DOM. Installing Lexbor here would create a
       g_doc + class registrations in this throwaway compile runtime; because
       the wasm instance is reused across callMain (EXIT_RUNTIME=0), that
       leftover host-model state corrupts the NEXT exec callMain's document
       (its publicPath probe finds no real <script> → "Automatic publicPath
       is not supported"). Skip it for the compile-only pass. */
    int fe_emit_only = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--fe-emit-bc")) { fe_emit_only = 1; break; }
    if (!fe_emit_only) {
        /* Spec DOM (Lexbor) installed before any script: the bundle's
           `document`/elements ARE Lexbor nodes, not hand-rolled stubs. */
        double t_dom0 = qjs_timer_ms_since_start();
        if (qjs_dom_install(ctx) != 0)
            fprintf(stderr, "warning: qjs_dom_install failed\n");
        qjs_timer_log("qjs_dom_install", "(host)", t_dom0);
    }
    /* Expose __hostMicrotaskDrain as a global so driver.js can drain
       Promise microtasks inside its __hostDrive fixpoint loop (fetch().
       then callbacks the bundle queues need to fire WHILE __hostDrive
       can still react to newly-registered handlers). */
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__hostMicrotaskDrain",
                          JS_NewCFunction(ctx, js_host_microtask_drain,
                                          "__hostMicrotaskDrain", 0));
        JS_FreeValue(ctx, g);
    }
    /* Forced-exec config via argv (reliable across native/wasm):
       --fe-exec  --fe-sched=<0/1 string>  --fe-trace=<path>
       --fe-script-start-lines=L0,L1,... (per-slice start line in
       combined-bundle space; lets JS_Eval2 stack frames report
       combined-bundle line numbers from each /b.N.js slice). */
    {
        int fe_en = 0; const char *fe_s = NULL, *fe_t = NULL, *seen_fe = NULL;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--fe-exec")) { fe_en = 1; seen_fe = argv[i]; }
            else if (!strncmp(argv[i], "--fe-sched=", 11)) { fe_s = argv[i] + 11; seen_fe = argv[i]; }
            else if (!strncmp(argv[i], "--fe-trace=", 11)) { fe_t = argv[i] + 11; seen_fe = argv[i]; }
            else if (!strncmp(argv[i], "--fe-script-start-lines=", 24)) qjs_parse_start_lines(argv[i] + 24);
        }
        if (seen_fe) qjs_forced_config(fe_en, fe_s, fe_t);
    }
    int rc = 0;
    /* --fe-html=<path>: parse the file as HTML into document.body via
       Lexbor's spec parser, then run every inline <script> in DOM
       order — mirrors the real browser bootstrap. Real-site bundles
       (github, GMail) ship a body HTML whose inline scripts set up
       window.__SSR_DATA / __INITIAL_DATA / module-resolver globals;
       loading the external bundle alone (the prior model) tripped a
       module-top throw because the env was unset. This is the open
       "entry-point/page-DOM cycle" CLAUDE.md flags. */
    static const char *FE_HTML_BOOT =
        "document.body.innerHTML = __fe_html;\n"
        "var __ss = document.querySelectorAll('script');\n"
        "for (var i = 0; i < __ss.length; i++) {\n"
        "  var s = __ss[i];\n"
        "  if (s.getAttribute && s.getAttribute('src')) continue;\n"
        "  var c = s.textContent;\n"
        "  if (c) { try { (0, eval)(c); } catch (e) { } }\n"
        "}\n"
        "delete __fe_html;\n";
    for (int i = 1; i < argc; i++) {
        /* --fe-emit-bc <src> <dst>: compile <src> to QuickJS bytecode and
           write it to <dst>, then exit. The driver runs this ONCE on the
           7 MB bundle, then passes the .bc to every schedule's fresh
           instance — turning a ~850 ms re-parse per run into a fast
           JS_ReadObject, the dominant per-schedule cost. */
        if (!strcmp(argv[i], "--fe-emit-bc") && i + 2 < argc) {
            size_t sn; char *s = readfile(argv[i + 1], &sn);
            if (!s) { fprintf(stderr, "emit-bc: cannot read %s\n", argv[i + 1]); rc = 1; break; }
            /* ESM sources compile under MODULE flags; the resulting .bc is
               a module record that JS_EvalFunction runs as a module. The
               classic-script .bc path stays the same (GLOBAL). start_line
               from the parsed --fe-script-start-lines bakes combined-bundle
               line numbers into the .bc's debug info so stack frames at
               run-time report combined-bundle line numbers without further
               offset math. */
            int bc_module = qjs_source_is_module(ctx, s, sn, argv[i + 1]);
            /* @WHY: the per-slice module-vs-classic dispatch is baked into the
               .bc HERE (the bundle takes the bytecode path, not qjs_eval_script).
               isModule=1 ⇒ top-level `var X` is module-local: a CDN <script src>
               defining `var Sentry` would be INVISIBLE to a later inline slice.
               After the script-grammar fix this reads 0 for a classic bundle —
               the verification signal that the cross-slice global is restored. */
            fprintf(stderr, "@WHY {\"phase\":\"bc_emit\",\"file\":\"%s\",\"len\":%zu,\"isModule\":%d}\n",
                    argv[i + 1] ? argv[i + 1] : "(null)", sn, bc_module);
            fflush(stderr);
            int bc_start = qjs_lookup_start_line(argv[i + 1]);
            JSEvalOptions bcopt = { JS_EVAL_OPTIONS_VERSION,
                                    (bc_module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL)
                                    | JS_EVAL_FLAG_COMPILE_ONLY,
                                    argv[i + 1], bc_start };
            JSValue fn = JS_Eval2(ctx, s, sn, &bcopt);
            free(s);
            if (JS_IsException(fn)) {
                JSValue e = JS_GetException(ctx); const char *em = JS_ToCString(ctx, e);
                printf("@E {\"file\":\"%s\",\"message\":\"emit-bc: %s\"}\n", argv[i + 1], em ? em : "(throw)");
                fflush(stdout);
                /* Also to stderr so it lands in ast-thread's per-module emitErr snapshot
                   (the @E stdout line is deduped/aggregated separately and was invisible
                   for the directus modular-SDK empty-compile triage). */
                fprintf(stderr, "EMITBC_THROW %s :: %s\n", argv[i + 1] ? argv[i + 1] : "?", em ? em : "(throw)");
                fflush(stderr);
                if (em) JS_FreeCString(ctx, em); JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, fn); rc = 1; break;
            }
            size_t bn = 0; uint8_t *bc = JS_WriteObject(ctx, &bn, fn, JS_WRITE_OBJ_BYTECODE);
            JS_FreeValue(ctx, fn);
            if (!bc) { fprintf(stderr, "emit-bc: write failed\n"); rc = 1; break; }
            fe_map_set(argv[i + 2], (const char *)bc, (long)bn);   /* compiled .bc → in-memory map (worker reads it back); no fopen */
            js_free(ctx, bc);
            break;   /* compile-only: done */
        }
        if (!strncmp(argv[i], "--fe-html=", 10)) {
            const char *p = argv[i] + 10;
            size_t hn; char *h = readfile(p, &hn);
            if (!h) { fprintf(stderr, "cannot read %s\n", p); rc = 1; break; }
            JSValue g = JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx, g, "__fe_html", JS_NewStringLen(ctx, h, hn));
            JS_FreeValue(ctx, g);
            free(h);
            JSValue v = JS_Eval(ctx, FE_HTML_BOOT, strlen(FE_HTML_BOOT), p, JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) {
                JSValue e = JS_GetException(ctx);
                const char *em = JS_ToCString(ctx, e);
                printf("@E {\"file\":\"%s\",\"message\":\"fe-html boot: %s\"}\n", p, em ? em : "(throw)");
                fflush(stdout);
                if (em) JS_FreeCString(ctx, em);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, v);
            continue;
        }
        if (!strncmp(argv[i], "--fe-", 5)) continue;   /* config, not a script */
        size_t n; char *src = readfile(argv[i], &n);
        if (!src) { fprintf(stderr, "cannot read %s\n", argv[i]); rc = 1; break; }
        size_t alen = strlen(argv[i]);
        JSValue v;
        double t_eval0 = qjs_timer_ms_since_start();
        if (alen > 3 && !strcmp(argv[i] + alen - 3, ".bc")) {
            /* Precompiled bytecode (from --fe-emit-bc): skip the ~850 ms
               re-parse of the 7 MB bundle that dominates per-schedule cost.
               JS_ReadObject restores the function + its atoms into this
               fresh instance; qjs_eval_bc dispatches on the value tag so a
               module-BC gets set_import_meta + await and a classic-script
               BC runs directly via JS_EvalFunction. The slice's start_line
               was baked into the BC at --fe-emit-bc time. Update
               __feCurFile here too so document.currentScript points to
               the BC's source slice during eval (BC filename has .bc
               suffix; map to the .js form for the slice lookup). */
            {
                JSValue g = JS_GetGlobalObject(ctx);
                size_t flen = strlen(argv[i]);
                char *js = malloc(flen + 1);
                if (js) {
                    memcpy(js, argv[i], flen + 1);
                    if (flen > 3 && js[flen-3] == '.' && js[flen-2] == 'b' && js[flen-1] == 'c') {
                        js[flen-2] = 'j'; js[flen-1] = 's';   /* .bc → .js */
                    }
                    JS_SetPropertyStr(ctx, g, "__feCurFile", JS_NewString(ctx, js));
                    free(js);
                }
                JS_FreeValue(ctx, g);
            }
            JSValue fn = JS_ReadObject(ctx, (const uint8_t *)src, n, JS_READ_OBJ_BYTECODE);
            v = qjs_eval_bc(ctx, fn);
        } else {
            v = qjs_eval_script(ctx, src, n, argv[i], qjs_lookup_start_line(argv[i]));
        }
        qjs_timer_log("JS_Eval", argv[i], t_eval0);
        free(src);
        /* An uncaught bundle exception is a SIGNAL that the host model
           is incomplete — never silently ignored. Surface it on stdout
           as an @E record (ast-thread captures it as a host-model gap
           to fix; stderr is dropped in the worker). The /driver
           epilogue still runs after — the event loop is host
           infrastructure, not the analyzed bundle (J-Force keeps
           driving after a fault) — so a real DOM/fetch model means
           production code does not fault here in the first place. */
        if (JS_IsException(v)) {
            /* Build {file,message,stack} and JSON-stringify it with
               the engine itself — correct escaping for free, and a
               single reliable stdout line ast-thread parses into the
               resolverError (stderr via emscripten printErr is
               line-split unreliably and was truncating the message). */
            JSValue e = JS_GetException(ctx);
            const char *em = JS_ToCString(ctx, e);
            JSValue st = JS_GetPropertyStr(ctx, e, "stack");
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "file", JS_NewString(ctx, argv[i]));
            JS_SetPropertyStr(ctx, o, "message", JS_NewString(ctx, em ? em : "(throw)"));
            JS_SetPropertyStr(ctx, o, "stack", JS_IsUndefined(st) ? JS_NewString(ctx, "") : JS_DupValue(ctx, st));
            JSValue js = JS_JSONStringify(ctx, o, JS_UNDEFINED, JS_UNDEFINED);
            const char *js_s = JS_ToCString(ctx, js);
            if (js_s) printf("@E %s\n", js_s);
            else printf("@E {\"file\":\"%s\",\"message\":\"(unprintable)\"}\n", argv[i]);
            /* The structured @E (with stack) is the host-model-gap
               diagnostic. stdout is block-buffered to a file/pipe; a
               later teardown abort (native debug assert) would drop it.
               Flush now so the gap is never silently lost. */
            fflush(stdout);
            if (em) fprintf(stderr, "@E %s :: %s\n", argv[i], em);
            if (js_s) JS_FreeCString(ctx, js_s);
            if (em) JS_FreeCString(ctx, em);
            JS_FreeValue(ctx, js);
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, st);
            JS_FreeValue(ctx, e);
            rc = 1;
        }
        JS_FreeValue(ctx, v);
    }
    if (rc == 0) {
        double t_loop0 = qjs_timer_ms_since_start();
        js_std_loop(ctx);
        qjs_timer_log("js_std_loop", "(host)", t_loop0);
    }
    double t_free0 = qjs_timer_ms_since_start();
    js_std_free_handlers(rt);   /* before JS_FreeContext (canonical order): a pending timer's callback + ctx ref would otherwise strand the context */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    qjs_timer_log("teardown", "(host)", t_free0);
    qjs_timer_log("TOTAL", "(host)", 0.0);
    return rc;
}
