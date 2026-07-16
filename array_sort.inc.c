/* Array.prototype.sort comparator — suspendable component (see CONT_SORT / JSArraySort in quickjs.c).
 *
 * ONE assertable contract: a bottom-up (iterative), STABLE, O(n log n) merge sort whose only suspension point
 * is cmp(array[l], array[r]) inside the inner merge — all algorithm state (width, block index, merge cursors,
 * temp buffer) lives in JSArraySort, so no C-stack recursion and a comparator body loop preempts, resuming
 * byte-identically. init snapshots present elements (undefined -> end, holes -> after); end writes back. Only a
 * NORMAL bytecode comparator routes here; default/native comparators keep rqsort.
 *
 * TEXTUALLY INCLUDED into quickjs.c (needs ValueSlot + the array/property helpers). Keeps the fork delta
 * modular — the interpreter drive (do_sort_tramp/step) and the struct/forward-decls stay in quickjs.c. */

/* read the present, non-undefined elements into s->array (undefined -> the end, holes -> after that), alloc tmp,
   set up the first merge block. Returns 0 ok / -1 exception (safe to js_array_sort_end). */
static int js_array_sort_init(JSContext *ctx, JSArraySort *s, JSValueConst this_val, JSValueConst method)
{
    int64_t i, pos = 0; int present;
    s->obj = JS_UNDEFINED; s->method = method; s->array = NULL; s->tmp = NULL;
    s->n = 0; s->len = 0; s->undefined_count = 0; s->pending = 0;
    s->width = 1; s->i = 0; s->lo = s->mid = s->hi = s->l = s->r = s->k = 0;
    for (i = 0; i < 4; i++) s->cb_args[i] = JS_UNDEFINED;
    s->obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &s->len, s->obj)) return -1;
    if (s->len > 0) {
        s->array = js_malloc(ctx, (size_t)s->len * sizeof(ValueSlot));
        if (!s->array) return -1;
    }
    for (i = 0; i < s->len; i++) {
        JSValue v;
        present = JS_TryGetPropertyInt64(ctx, s->obj, i, &v);
        if (present < 0) { s->n = pos; return -1; }
        if (present == 0) continue;
        if (JS_IsUndefined(v)) { s->undefined_count++; continue; }
        s->array[pos].val = v; s->array[pos].str = NULL; s->array[pos].pos = i; pos++;
    }
    s->n = pos;
    if (s->n > 1) {
        s->tmp = js_malloc(ctx, (size_t)s->n * sizeof(ValueSlot));
        if (!s->tmp) return -1;
        s->lo = 0; s->mid = 1 < s->n ? 1 : s->n; s->hi = 2 < s->n ? 2 : s->n;   /* first block, width=1 */
        s->l = 0; s->r = s->mid; s->k = 0;
    }
    return 0;
}

/* Drive the merge until the next comparison is needed (return 1, out_args = the two element values) or the sort
   is complete (return 0). `res` (owned) is the comparator's return value for the pending comparison, or JS_UNDEFINED
   on the first call. Returns -1 on exception. */
static int js_array_sort_step(JSContext *ctx, JSArraySort *s, JSValue res, JSValueConst out_args[2])
{
    if (s->pending) {
        double v;
        if (JS_ToFloat64Free(ctx, &v, res) < 0) return -1;   /* consumes res */
        s->pending = 0;
        if (v <= 0) s->tmp[s->k++] = s->array[s->l++];   /* stable: <= keeps the left (earlier) element first */
        else        s->tmp[s->k++] = s->array[s->r++];
    } else {
        JS_FreeValue(ctx, res);   /* JS_UNDEFINED on the first call, or a stray value */
    }
    for (;;) {
        if (s->width >= s->n) break;                 /* fully sorted (n<=1 lands here immediately) */
        if (s->i >= s->n) {                          /* this width pass done -> next width */
            s->width *= 2; s->i = 0;
            if (s->width >= s->n) break;
            s->lo = 0; s->mid = s->width < s->n ? s->width : s->n;
            s->hi = 2 * s->width < s->n ? 2 * s->width : s->n;
            s->l = 0; s->r = s->mid; s->k = 0;
            continue;
        }
        if (s->l < s->mid && s->r < s->hi) {         /* the ONE suspension point */
            out_args[0] = s->array[s->l].val;
            out_args[1] = s->array[s->r].val;
            s->pending = 1;
            return 1;
        }
        while (s->l < s->mid) s->tmp[s->k++] = s->array[s->l++];   /* drain remainders */
        while (s->r < s->hi)  s->tmp[s->k++] = s->array[s->r++];
        { int64_t t; for (t = s->lo; t < s->hi; t++) s->array[t] = s->tmp[t]; }   /* copy block back */
        s->i += 2 * s->width;                        /* next block at this width */
        if (s->i < s->n) {
            s->lo = s->i; s->mid = s->i + s->width < s->n ? s->i + s->width : s->n;
            s->hi = s->i + 2 * s->width < s->n ? s->i + 2 * s->width : s->n;
            s->l = s->lo; s->r = s->mid; s->k = s->lo;
        }
    }
    return 0;   /* caller writes the sorted array back in js_array_sort_end */
}

/* On ok: write the sorted elements back to obj, then the undefineds, then delete the holes; return obj (owned,
   transferred to caller). On failure: free everything, return JS_EXCEPTION. Either way the state is released. */
static JSValue js_array_sort_end(JSContext *ctx, JSArraySort *s, bool ok)
{
    int64_t i, w = 0;
    if (!ok) goto fail;
    while (w < s->n) {                               /* sorted present elements back in place */
        if (s->array[w].pos == w) { JS_FreeValue(ctx, s->array[w].val); }
        else if (JS_SetPropertyInt64(ctx, s->obj, w, s->array[w].val) < 0) { w++; goto fail; }
        w++;
    }
    js_free(ctx, s->array); s->array = NULL;
    js_free(ctx, s->tmp); s->tmp = NULL;
    for (i = w; s->undefined_count-- > 0; i++)
        if (JS_SetPropertyInt64(ctx, s->obj, i, JS_UNDEFINED) < 0) goto fail2;
    for (; i < s->len; i++)
        if (JS_DeletePropertyInt64(ctx, s->obj, i, JS_PROP_THROW) < 0) goto fail2;
    return s->obj;
fail:
    for (; w < s->n; w++) JS_FreeValue(ctx, s->array[w].val);
    js_free(ctx, s->array); s->array = NULL;
    js_free(ctx, s->tmp); s->tmp = NULL;
fail2:
    JS_FreeValue(ctx, s->obj); s->obj = JS_UNDEFINED;
    return JS_EXCEPTION;
}

/* arr.sort(cmp) where cmp (argv[0]) is a NORMAL bytecode function -> route to the suspendable coroutine. A
   default sort (no cmp) or a native/bound comparator has no preemptible JS body and stays on rqsort. */
static bool tramp_can_call_array_sort(JSValueConst func, int argc, JSValueConst *argv)
{
    JSObject *mp, *cp;
    if (JS_VALUE_GET_TAG(func) != JS_TAG_OBJECT) return false;
    mp = JS_VALUE_GET_OBJ(func);
    if (!(mp->class_id == JS_CLASS_C_FUNCTION && mp->u.cfunc.c_function.generic == js_array_sort)) return false;
    if (argc < 1 || JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT) return false;
    cp = JS_VALUE_GET_OBJ(argv[0]);
    return cp->class_id == JS_CLASS_BYTECODE_FUNCTION
        && cp->u.func.function_bytecode->func_kind == JS_FUNC_NORMAL;
}

