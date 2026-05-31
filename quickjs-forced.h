/* Forced multi-path execution controller for QuickJS (analysis only).
   Same proven design as the V8 ForcedExec (env schedule in, branch
   trace out, frontier on schedule exhaustion). C, header-only, no
   build-system change, portable (wasm/Emscripten getenv+stdio OK).
   Disabled (identity) unless FORCED_EXEC=1, so normal runs are
   unaffected. */
#ifndef QJS_FORCED_H
#define QJS_FORCED_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* qjs_host_yield: cooperative pause point, defined in qjsmain.c under
   EM_ASYNC_JS for the JSPI worker build. The forced controller does NOT
   yield per-branch — that's redundant with the op-poll heartbeat, which
   fires at every OP_if anyway (each OP_if handler calls js_poll_interrupts,
   and the heartbeat yields under JSPI). The per-orphan yield in the deep
   grind + that heartbeat cover cross-page preemption and cooling without
   taxing function-internals review with a macrotask round-trip at every
   opaque branch. Declared here only so the deep-grind/op-poll call sites
   in quickjs.c resolve. */
#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
void qjs_host_yield(void);
#endif

static void qjs_fe_seen_reset(void);   /* fwd: per-unit loop-revisit seen-set, defined below; qjs_forced_config resets it */

static int    qjs_fe_init = 0, qjs_fe_enabled = 0;
static const char *qjs_fe_sched = "";
static size_t qjs_fe_len = 0, qjs_fe_cur = 0;
static FILE  *qjs_fe_trace = NULL;

/* Explicit config (argv) — reliable in native, node-wasm and
   browser-worker alike (emscripten getenv is not). qjsmain calls
   this; getenv is only a fallback. sched is copied (caller's argv
   may not outlive the run). */
static char qjs_fe_sbuf[4096];
/* External linkage: qjsmain.c (a separate TU) calls this to push the
   argv config into the controller. The header is included only by
   quickjs.c, so there is exactly one definition; state + decide stay
   static, shared within quickjs.c's TU where the interpreter reads. */
void qjs_forced_config(int en, const char *sch, const char *tr) {
    qjs_fe_enabled = en;
    if (sch) {
        size_t n = strlen(sch);
        if (n >= sizeof(qjs_fe_sbuf)) n = sizeof(qjs_fe_sbuf) - 1;
        memcpy(qjs_fe_sbuf, sch, n); qjs_fe_sbuf[n] = 0;
        qjs_fe_sched = qjs_fe_sbuf; qjs_fe_len = n;
    }
    qjs_fe_cur = 0;                          /* schedule cursor restarts each run (stale across callMain otherwise) */
    if (qjs_fe_trace && qjs_fe_trace != stderr) fclose(qjs_fe_trace);   /* don't leak the prior run's trace handle */
    /* "w" (not "a") so each forced run TRUNCATES the trace file at
       open. drive.mjs reuses one fixed path across all forced runs;
       this lets it read the fresh trace per iteration without
       accumulating prior runs' lines, and avoids generating a unique
       file per schedule (the old "per-run unique path to dodge EPERM
       on rmSync" pattern was producing hundreds of trace files). */
    qjs_fe_trace = (tr && tr[0]) ? fopen(tr, "w") : stderr;
    qjs_fe_init = 1;
    qjs_pc_reset();                          /* fresh Φ per forced run */
    qjs_fe_seen_reset();                     /* fresh loop-revisit seen-set per forced run */
}
static void qjs_fe_setup(void) {
    if (qjs_fe_init) return;
    const char *e = getenv("FORCED_EXEC");
    qjs_fe_enabled = (e && e[0] == '1');
    const char *s = getenv("FORCED_SCHEDULE");
    if (s) { qjs_fe_sched = s; qjs_fe_len = strlen(s); }
    const char *t = getenv("FORCED_TRACE");
    qjs_fe_trace = (t && t[0]) ? fopen(t, "a") : stderr;
    qjs_fe_init = 1;
}
/* When muted, forced decisions still happen (default path) but NO
   frontier is reported — so code the entry-point driver force-runs for
   coverage (every webpack factory, etc.) does not seed the schedule
   cross-product. Without this, driving a real SPA's thousands of
   modules explodes enumeration (the X-Force path-explosion wall). The
   program's OWN branches (unmuted) are still fully enumerated. */
/* `mute` = no schedule-frontier seeding (entry-point driving from
   hostedge.js's __hostDrive turns this on so handler exploration
   doesn't manufacture spurious schedules); `phi_mute` = no Φ-push
   (hostedge.js's S() turns this on around its bookkeeping). These
   are independent: a handler running under __hostDrive STILL pushes
   its path constraints into Φ (we want that data flow captured),
   it just doesn't seed new schedules. Splitting the two flags was
   the fix for multi-message PoCs showing empty Φ. */
static int qjs_fe_mute = 0;
static int qjs_fe_phi_mute = 0;

/* Per-execution-unit seen-set of forced branch keys decided in the DEFAULT
   regime (past the schedule). Revisiting the same key means a loop whose
   opaque predicate never concretises (`for(;;){const{done}=await r.read();
   if(done)break;…}` — done is opaque, so if(done) forces and the non-break
   arm loops to the same pc forever). The Cousot–Cousot monotone fixpoint: the
   loop's opaque arm is taken FINITELY, then the EXIT arm is forced. Whichever
   arm we took on the first visit LOOPED back here, so the other arm (!d0) is
   the one that makes forward progress out of the loop — return it on revisit.
   This bounds re-visits of IDENTICAL forced-state, NEVER distinct work. The
   key is (branch-site line:col:offset ⊕ per-invocation id): NOT the growing
   decision-prefix (a forking loop's prefix differs every iteration, so a
   prefix check never fires), AND NOT site-alone (the SAME shared-code branch
   reached by two SEPARATE calls — e.g. two opaque-URL fetches both running
   the fetch-shim's `input && input.method` — is not a loop, and site-alone
   wrongly flipped the 2nd call's decision into a phantom opaque init). A
   genuine loop revisits within ONE invocation (same frame ⇒ same id); a
   repeated call is a fresh invocation (fresh id). Reset per forced run AND per
   deep-grind orphan drive (each is one execution unit; the invocation counter
   resets with it). */
static unsigned long long *qjs_fe_seen_k = NULL;
static unsigned char *qjs_fe_seen_d = NULL;
static int qjs_fe_seen_cap = 0, qjs_fe_seen_n = 0;
/* Per-run invocation counter — stamped onto each JSStackFrame at entry and
   mixed into the loop-revisit key so a site is keyed by (branch-site,
   invocation). Reset with the seen-set (same execution-unit scope), so it
   counts invocations within ONE run/drive and never wraps a 32-bit range in
   practice. */
static unsigned int qjs_inv_ctr = 0;
static void qjs_fe_seen_reset(void) {
    qjs_fe_seen_n = 0;
    qjs_inv_ctr = 0;
    if (qjs_fe_seen_k) memset(qjs_fe_seen_k, 0, (size_t)qjs_fe_seen_cap * sizeof *qjs_fe_seen_k);
}
/* If key already decided this unit: *d0 = its first decision, return 1.
   Else return 0 — caller decides, then calls qjs_fe_seen_put. */
static int qjs_fe_seen_get(unsigned long long k, int *d0) {
    if (k == 0 || qjs_fe_seen_cap == 0) return 0;
    unsigned int mask = (unsigned)qjs_fe_seen_cap - 1;
    unsigned int j = (unsigned)(k * 2654435761u) & mask;
    while (qjs_fe_seen_k[j]) {
        if (qjs_fe_seen_k[j] == k) { if (d0) *d0 = qjs_fe_seen_d[j]; return 1; }
        j = (j + 1) & mask;
    }
    return 0;
}
static void qjs_fe_seen_put(unsigned long long k, int d) {
    if (k == 0) return;
    if (qjs_fe_seen_cap == 0) {
        qjs_fe_seen_cap = 1024;
        qjs_fe_seen_k = (unsigned long long *)calloc(qjs_fe_seen_cap, sizeof *qjs_fe_seen_k);
        qjs_fe_seen_d = (unsigned char *)calloc(qjs_fe_seen_cap, sizeof *qjs_fe_seen_d);
        if (!qjs_fe_seen_k || !qjs_fe_seen_d) { free(qjs_fe_seen_k); free(qjs_fe_seen_d); qjs_fe_seen_k = NULL; qjs_fe_seen_d = NULL; qjs_fe_seen_cap = 0; return; }
    }
    if (qjs_fe_seen_n * 10 >= qjs_fe_seen_cap * 7) {   /* grow + rehash at 70% load */
        int nc = qjs_fe_seen_cap * 2;
        unsigned long long *nk = (unsigned long long *)calloc(nc, sizeof *nk);
        unsigned char *nd = (unsigned char *)calloc(nc, sizeof *nd);
        if (nk && nd) {
            unsigned int nm = (unsigned)nc - 1;
            for (int i = 0; i < qjs_fe_seen_cap; i++) {
                unsigned long long v = qjs_fe_seen_k[i];
                if (v) { unsigned int p = (unsigned)(v * 2654435761u) & nm; while (nk[p]) p = (p + 1) & nm; nk[p] = v; nd[p] = qjs_fe_seen_d[i]; }
            }
            free(qjs_fe_seen_k); free(qjs_fe_seen_d);
            qjs_fe_seen_k = nk; qjs_fe_seen_d = nd; qjs_fe_seen_cap = nc;
        } else { free(nk); free(nd); }   /* OOM: skip grow, keep correctness (more probes) */
    }
    unsigned int mask = (unsigned)qjs_fe_seen_cap - 1;
    unsigned int j = (unsigned)(k * 2654435761u) & mask;
    while (qjs_fe_seen_k[j]) { if (qjs_fe_seen_k[j] == k) { qjs_fe_seen_d[j] = (unsigned char)(d & 1); return; } j = (j + 1) & mask; }
    qjs_fe_seen_k[j] = k; qjs_fe_seen_d[j] = (unsigned char)(d & 1); qjs_fe_seen_n++;
}

/* Loop-revisit fixpoint for the OP_if_* opaque-branch handlers. Returns 1 and
   sets *res to the EXIT arm (the complement of the first decision at this
   branch site this execution unit) when the site was already decided — i.e.
   control returned here via a loop whose opaque predicate never concretised.
   Forcing the other arm makes forward progress out of the loop (Cousot–Cousot
   monotone fixpoint: the opaque arm is taken FINITELY, then the exit is
   forced). The handler checks this BEFORE its opaque-value pin / host-reach
   prune, because the value pin would otherwise re-pick the first decision
   forever (the pin is right for a value tested twice on a straight path, wrong
   for a loop). Bounds re-visits of identical state, never distinct work; no
   cap. Returns 0 on first visit — the caller runs its normal decision, then
   records the chosen res via qjs_fe_loop_seen. */
/* NOT gated on qjs_fe_mute. The loop-revisit fixpoint is a TERMINATION
   mechanism — it must run whenever forced execution is enabled, even inside a
   MUTED drive (the opaque-callee cb-drive and __hostDrive set qjs_fe_mute=1 to
   suppress FRONTIER seeding + cursor/Φ pollution). Gating termination on the
   frontier-mute meant a muted callback with an opaque loop (markdown-it's
   autolink `for(;;){if(++n>=r)return;…}` reached via `ruler.push` on an opaque
   receiver) spun forever and froze the whole priority frontier (verified live
   on learn.microsoft.com). The seen-set isolation the mute used to provide is
   now intrinsic to the KEY: it includes the per-invocation id, so a muted
   drive's decisions at a SHARED bytecode site (e.g. the fetch shim's
   `input&&input.method`) carry a different key than the main run's and cannot
   be mistaken for a revisit — the redundancy the invocation-id made safe to
   drop. Frontier seeding stays gated by qjs_fe_mute in qjs_forced_decide_k_p. */
static int qjs_fe_loop_revisit(unsigned long long key, int *res) {
    if (!qjs_fe_enabled || key == 0) return 0;
    int d0;
    if (qjs_fe_seen_get(key, &d0)) { *res = d0 ? 0 : 1; return 1; }
    return 0;
}
static void qjs_fe_loop_seen(unsigned long long key, int res) {
    if (!qjs_fe_enabled || key == 0) return;
    qjs_fe_seen_put(key, res);
}

/* orig/return are 0/1. Disabled -> identity. Enabled -> follow the
   schedule; past it take 0 and (unless muted) report the frontier.
   `key` is a cross-process-stable branch identity (function source
   line:col + bytecode offset, computed at the call site) so the
   driver can do X-Force coverage-guided exploration (Algorithm 1):
   enqueue a flip only if its branch-EDGE is uncovered — bounding the
   blind 2^n to the finite distinct-edge set. The ordinal is still the
   worklist position (X-Force switches sequence); the key is the
   fitness signal, at the individual-bytecode level. */
static int qjs_forced_decide_k_p(int orig, unsigned long long key, qjs_term *pt, int priority_default, int frontier_sig) {
    if (!qjs_fe_init) qjs_fe_setup();
    if (!qjs_fe_enabled) return orig;
    int d;
    if (qjs_fe_cur < qjs_fe_len) {
        d = (qjs_fe_sched[qjs_fe_cur] == '1') ? 1 : 0;
    } else {
        /* Priority-driven default decision: the FALL-THROUGH and JUMP arms
           of this opaque branch BOTH reach host-edges (otherwise the
           caller's host-reach prune would have filtered the F-emission),
           but they may have different DENSITY. The call site computes the
           priority bit (1 = jump arm carries the only host edge in its
           reachable region; 0 = symmetric or fall-through reaches too)
           and passes it here. Without this, the default was a static 0,
           so the FIRST forced run always explored the fall-through arm
           even when the jump arm was clearly more productive — a hidden
           FIFO that pushed the productive endpoint to a LATER schedule.
           drive.mjs still enqueues the OTHER arm via F-emission for
           coverage; priority_default only orders WHICH arm runs first. */
        d = priority_default ? 1 : 0;
        /* SMT-directed BFS pruning (KLEE/DART/ExpoSE discipline,
           X-Force §3 documented false-path filter): drive.mjs's BFS
           seeds a flipped-decision schedule on every F-emission; that
           schedule is wasted work if Φ ∧ predicate is UNSAT. When the
           predicate is tainted (pt != NULL), pre-check feasibility
           and suppress the F-emission on confident UNSAT — Φ stays
           authoritative, the schedule prefix is never enqueued, and
           the driver doesn't burn a forced run on an infeasible path.
           Untainted opaque frontiers (pt == NULL) always emit; only
           SMT-decidable predicates can be filtered. */
        if (!qjs_fe_mute && qjs_fe_trace) {
            int feasible = pt ? qjs_z3_check_flip_sat(pt) : 1;
            if (feasible)
                /* 4th field = frontier_sig: the reach-class of the arm THIS
                   frontier's flip will explore (2 = reaches a network edge,
                   1 = reaches some host edge but not a net one, 0 = unknown
                   from a legacy non-per-arm caller). The worker's schedCmp
                   runs net-reaching frontiers first so the most-likely-
                   endpoint internal code path is explored ahead of the rest
                   — priority within a function, not FIFO over its branches.
                   ORDER only; every frontier still enqueues (coverage
                   unchanged). */
                fprintf(qjs_fe_trace, "F %zu %llu %d\n", qjs_fe_cur, key, frontier_sig);
        }
    }
    if (qjs_fe_trace) { fprintf(qjs_fe_trace, "B %zu %d %llu\n", qjs_fe_cur, d, key); fflush(qjs_fe_trace); }
    qjs_fe_cur++;
    /* NO per-branch yield: the op-poll heartbeat (js_poll_interrupts, called
       by every OP_if handler) already gives the host a turn every
       JS_INTERRUPT_COUNTER_INIT ops, which is finer than any human-
       perceptible preemption need and is where cooling sleeps are inserted.
       A second explicit yield here only taxed function-internals review with
       a redundant macrotask round-trip at every opaque branch. Cross-page
       preemption + cooling are served by the heartbeat and the per-orphan
       yield in the deep grind. */
    return d;
}
/* Back-compat wrappers. Old call sites that don't yet know the per-arm
   reachability default fall through to priority_default=0 (which matches
   the historic static `d = 0` behavior); new call sites pass the computed
   bit via qjs_forced_decide_k_p directly. */
static int qjs_forced_decide_k(int orig, unsigned long long key, qjs_term *pt) {
    return qjs_forced_decide_k_p(orig, key, pt, 0, 0);
}
static int qjs_forced_decide(int orig) { return qjs_forced_decide_k_p(orig, 0ULL, NULL, 0, 0); }
#endif /* QJS_FORCED_H */
