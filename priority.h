/* Picker heuristics — ORDER only, never COVERAGE.
 *
 * Engine-side counterpart to extension/lib/priority.js. Every function here
 * decides WHICH pending unit to drive NEXT; none of them adds, removes, or
 * skips a unit. Deleting this header would not lose any learned endpoint —
 * the deep grind would simply explore the orphan @T set in arbitrary GC-list
 * order. That invariant is what allows tuning these heuristics without
 * correctness review: a different comparator changes WHEN something is
 * driven, not WHETHER it is driven.
 *
 * Single comparator currently lives here:
 *   qjs_deep_relcmp(a, b, ctx)
 *     — qsort_r-style comparator over qjs_deep_ent. Lexicographic on
 *       (network-reach, executed, sink-reach, byte_code_len, idx). The
 *       endpoint-bearing functions come first because the deep grind's
 *       purpose is lazy-chunk fetch recovery; quality (executed) next
 *       because a function whose body ran has concrete closure state and
 *       resolves CONCRETE URL/body values where a cold orphan only an
 *       opaque shape; security-sink reachability third; effort (bytecode
 *       size) fourth; idx is the stable tiebreaker.
 *
 * `qjs_deep_ent` is forward-declared here for the included-in-place build
 * model (quickjs.c includes this header AFTER the struct is defined; the
 * comparator function definition lives inline so the engine compiles
 * identically). If the build switches to a separate priority.c TU, the
 * struct would move into a shared layout header and `qjs_deep_relcmp` would
 * become non-inline.
 *
 * The JS-side counterparts (BFS schedule, paused-fiber, deep-round-rotation
 * comparators) live in extension/lib/priority.js — same discipline, no
 * shared code possible since one is JS-in-Worker and one is C-in-wasm.
 */
#ifndef QJS_PRIORITY_H
#define QJS_PRIORITY_H

/* The struct layout is defined in quickjs.c next to the qjs_deep_* state.
   This header is included AFTER that definition so the type is known. */

static inline int qjs_priority_deep_relcmp(const void *a, const void *b, void *o) {
    const qjs_deep_ent *x = (const qjs_deep_ent *)a;
    const qjs_deep_ent *y = (const qjs_deep_ent *)b;
    /* GOAL #1: API-endpoint (network fetch) orphans first — the deep grind's
       whole purpose is lazy-chunk fetch recovery (github preheat etc.). */
    if (x->net != y->net) return y->net - x->net;
    /* QUALITY: a fn whose body RAN (concrete closure/module state) but whose
       host site was guard-skipped (preheat shape) resolves CONCRETE values;
       a cold orphan only an opaque shape. */
    if (x->exec != y->exec) return y->exec - x->exec;
    /* SECURITY (#10): sink-reaching next — same forced-execution pass as
       endpoint learning ("one execution, two views" per CLAUDE.md), just
       ordered after endpoints in the live-pick. */
    if (x->sink != y->sink) return y->sink - x->sink;
    /* EFFORT: cheaper (smaller) first so more orphans get driven per cycle. */
    if (x->size != y->size) return x->size - y->size;
    /* Stable tiebreak so the sort is deterministic across batches. */
    return x->idx - y->idx;
}

/* Live single-element comparison — same dimensions as qjs_priority_deep_relcmp
   but reads the engine state DIRECTLY at call time, so a function whose
   net/exec/sink/size bits changed since the last sort gets its NEW priority
   on the next pick. Used by the (not-yet-wired) per-orphan yield design
   where qjs_deep_step_c picks the best NOT-YET-DRIVEN function from
   qjs_deep_rb at every JSPI yield boundary, instead of iterating a sorted
   array. The comparator returns x_is_better_than_y (1) or worse (0) so the
   caller can do a single linear scan to find the maximum. */
static inline int qjs_priority_live_orphan_better(JSFunctionBytecode *x,
                                                  JSFunctionBytecode *y) {
    int xn = qjs_fn_reaches_net(x), yn = qjs_fn_reaches_net(y);
    if (xn != yn) return xn > yn;
    int xe = x->qjs_executed, ye = y->qjs_executed;
    if (xe != ye) return xe > ye;
    int xs = qjs_fn_reaches_sink(x), ys = qjs_fn_reaches_sink(y);
    if (xs != ys) return xs > ys;
    if (x->byte_code_len != y->byte_code_len) return x->byte_code_len < y->byte_code_len;
    return 0;   /* equal priority — caller's iteration order (idx) breaks ties */
}

/* O(1) live comparator — same lexicographic dimensions as the bytecode-
   walking variant above, but net/sink come PRECOMPUTED (cached at residue
   build) and only exec/size are read live. This is the hot-path version
   used by the per-pick scan in qjs_deep_step_c: net/sink are static
   facts of the bytecode that never change as the grind runs, so re-walking
   them per comparison (O(bytecode) × O(N²) picks) was pure waste. exec
   stays live (a function whose body runs mid-grind promotes immediately);
   size is a static struct field. Returns 1 if x outranks y. */
static inline int qjs_priority_live_orphan_better_bits(int xnet, int xexec, int xsink, int xsize,
                                                       int ynet, int yexec, int ysink, int ysize) {
    if (xnet != ynet) return xnet > ynet;
    if (xexec != yexec) return xexec > yexec;
    if (xsink != ysink) return xsink > ysink;
    if (xsize != ysize) return xsize < ysize;
    return 0;
}

#endif /* QJS_PRIORITY_H */
