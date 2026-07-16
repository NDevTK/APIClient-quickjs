/* Array iteration builtins (forEach/map/every/some/filter + reduce/reduceRight) — suspendable component
 * (see CONT_ARRAY_ITER / CONT_ARRAY_REDUCE, JSArrayEvery / JSArrayReduce in quickjs.c).
 *
 * ONE assertable contract: drive each JS callback from a resumable STEP so the callback runs on the tramp chain
 * (a callback body loop preempts the base flow) instead of a C-recursive JS_Call. The state owns the callback's
 * operand buffer (cb_args), so nothing grows the caller's compiler-sized stack. init/step/end are ONE source of
 * truth shared by the plain C driver (js_array_every/js_array_reduce) and the tramp coroutine.
 *
 * TEXTUALLY INCLUDED into quickjs.c (needs the array/property/typed-array helpers). The interpreter drive
 * (do_array_iter_tramp/step, do_array_reduce_tramp/step) and the structs/forward-decls stay in quickjs.c. */

/* One coroutine step: PROCESS the previous callback's `res` (for element pending_k), then ADVANCE to the next
   present element and fill out_args with (value, index, obj). Returns 1 = CALL (invoke func on out_args), 0 = DONE
   (s->ret is the final result), -1 = EXCEPTION. Consumes `res`. Identical semantics to the original C loop. */
static int js_array_every_step(JSContext *ctx, JSArrayEvery *s, JSValue res, JSValueConst out_args[3])
{
    if (s->pending_k >= 0) {
        int64_t k = s->pending_k;
        s->pending_k = -1;
        switch (s->special) {
        case special_every:
        case special_every | special_TA:
            if (!JS_ToBoolFree(ctx, res)) { s->ret = JS_FALSE; goto done; }
            break;
        case special_some:
        case special_some | special_TA:
            if (JS_ToBoolFree(ctx, res)) { s->ret = JS_TRUE; goto done; }
            break;
        case special_map:
            if (JS_DefinePropertyValueInt64(ctx, s->ret, k, res, JS_PROP_C_W_E | JS_PROP_THROW) < 0) return -1;
            break;
        case special_map | special_TA:
            if (JS_SetPropertyValue(ctx, s->ret, js_int32(k), res, JS_PROP_THROW) < 0) return -1;
            break;
        case special_filter:
        case special_filter | special_TA:
            if (JS_ToBoolFree(ctx, res)) {
                if (JS_DefinePropertyValueInt64Const(ctx, s->ret, s->n++, s->val, JS_PROP_C_W_E | JS_PROP_THROW) < 0) return -1;
            }
            break;
        default:
            JS_FreeValue(ctx, res);
            break;
        }
        JS_FreeValue(ctx, s->val);
        s->val = JS_UNDEFINED;
    }
    while (s->k < s->len) {
        int64_t k = s->k++;
        int present;
        JSValue val;
        if (s->special & special_TA) {
            val = JS_GetPropertyInt64(ctx, s->obj, k);
            if (JS_IsException(val)) return -1;
            present = true;
        } else {
            present = JS_TryGetPropertyInt64(ctx, s->obj, k, &val);
            if (present < 0) return -1;
        }
        if (present) {
            s->val = val;
            s->pending_k = k;
            out_args[0] = val;
            out_args[1] = js_int64(k);
            out_args[2] = s->obj;
            return 1;
        }
    }
done:
    if (s->special == (special_filter | special_TA)) {
        JSValue arr, res2;
        JSValueConst a2[2];
        a2[0] = s->obj;
        a2[1] = js_int32(s->n);
        arr = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, a2, true);
        if (JS_IsException(arr)) return -1;
        a2[0] = s->ret;
        res2 = JS_Invoke(ctx, arr, JS_ATOM_set, 1, a2);
        if (check_exception_free(ctx, res2)) { JS_FreeValue(ctx, arr); return -1; }
        JS_FreeValue(ctx, s->ret);
        s->ret = arr;
    }
    return 0;
}

/* Initialize the resumable state (the pre-loop setup: obj/len/func/this_arg + the per-special `ret` seed).
   Returns 0 = ok, -1 = exception (state is safe to js_array_every_end). Shared by the plain C driver
   (js_array_every) and the tramp-chain coroutine (do_array_iter_tramp) — ONE source of truth. */
static int js_array_every_init(JSContext *ctx, JSArrayEvery *s, JSValueConst this_val,
                               int argc, JSValueConst *argv, int special)
{
    JSValueConst args[2];

    s->obj = JS_UNDEFINED; s->ret = JS_UNDEFINED; s->val = JS_UNDEFINED;
    s->len = 0; s->k = 0; s->n = 0; s->special = special; s->pending_k = -1;
    if (special & special_TA) {
        s->obj = js_dup(this_val);
        s->len = js_typed_array_get_length_unsafe(ctx, s->obj);
        if (s->len < 0)
            return -1;
    } else {
        s->obj = JS_ToObject(ctx, this_val);
        if (js_get_length64(ctx, &s->len, s->obj))
            return -1;
    }
    s->func = argv[0];
    s->this_arg = (argc > 1) ? argv[1] : JS_UNDEFINED;
    if (check_function(ctx, s->func))
        return -1;

    switch (special) {
    case special_every:
    case special_every | special_TA:
        s->ret = JS_TRUE;
        break;
    case special_some:
    case special_some | special_TA:
        s->ret = JS_FALSE;
        break;
    case special_map:
        s->ret = JS_ArraySpeciesCreate(ctx, s->obj, js_int64(s->len));
        if (JS_IsException(s->ret)) return -1;
        break;
    case special_filter:
        s->ret = JS_ArraySpeciesCreate(ctx, s->obj, js_int32(0));
        if (JS_IsException(s->ret)) return -1;
        break;
    case special_map | special_TA:
        args[0] = s->obj;
        args[1] = js_int32(s->len);
        s->ret = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, args, true);
        if (JS_IsException(s->ret)) return -1;
        break;
    case special_filter | special_TA:
        s->ret = JS_NewArray(ctx);
        if (JS_IsException(s->ret)) return -1;
        break;
    }
    return 0;
}

/* Release the transient state. The caller takes s->ret on success (pass take_ret=true); on failure it is freed. */
static void js_array_every_end(JSContext *ctx, JSArrayEvery *s, bool take_ret)
{
    if (!take_ret)
        JS_FreeValue(ctx, s->ret);
    JS_FreeValue(ctx, s->val);
    JS_FreeValue(ctx, s->obj);
    s->ret = JS_UNDEFINED; s->val = JS_UNDEFINED; s->obj = JS_UNDEFINED;
}

static JSValue js_array_every(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int special)
{
    JSArrayEvery s;
    JSValueConst args[3];
    JSValue res = JS_UNDEFINED, ret;
    int st;

    if (js_array_every_init(ctx, &s, this_val, argc, argv, special))
        goto exception;
    for (;;) {
        st = js_array_every_step(ctx, &s, res, args);   /* consumes res */
        res = JS_UNDEFINED;
        if (st < 0)
            goto exception;
        if (st == 0)
            break;
        res = JS_Call(ctx, s.func, s.this_arg, 3, args);
        if (JS_IsException(res))
            goto exception;
    }
    ret = s.ret;
    js_array_every_end(ctx, &s, true);
    return ret;

exception:
    JS_FreeValue(ctx, res);
    js_array_every_end(ctx, &s, false);
    return JS_EXCEPTION;
}

/* Is `func` one of the array iteration builtins whose callback drive runs on the tramp chain? Exact C-function
   identity + its magic (the `special`) — the same dispatch quickjs itself uses, never a name/shape match. */
static bool tramp_can_call_array_iter(JSValueConst func, int *out_special)
{
    JSObject *fp;
    if (JS_VALUE_GET_TAG(func) != JS_TAG_OBJECT) return false;
    fp = JS_VALUE_GET_OBJ(func);
    if (fp->class_id != JS_CLASS_C_FUNCTION) return false;
    if (fp->u.cfunc.cproto != JS_CFUNC_generic_magic) return false;
    if (fp->u.cfunc.c_function.generic_magic != js_array_every) return false;
    *out_special = fp->u.cfunc.magic;
    return true;
}

#define special_reduce       0
#define special_reduceRight  1

/* Seed the accumulator: argv[1] if given, else the first present element (spec: empty + no seed -> TypeError). */
static int js_array_reduce_init(JSContext *ctx, JSArrayReduce *s, JSValueConst this_val,
                                int argc, JSValueConst *argv, int special)
{
    int64_t k1;
    int present;

    s->obj = JS_UNDEFINED; s->acc = JS_UNDEFINED; s->val = JS_UNDEFINED;
    s->len = 0; s->k = 0; s->special = special; s->pending = 0;
    if (special & special_TA) {
        s->obj = js_dup(this_val);
        s->len = js_typed_array_get_length_unsafe(ctx, s->obj);
        if (s->len < 0) return -1;
    } else {
        s->obj = JS_ToObject(ctx, this_val);
        if (js_get_length64(ctx, &s->len, s->obj)) return -1;
    }
    s->func = argv[0];
    if (check_function(ctx, s->func)) return -1;
    if (argc > 1) {
        s->acc = js_dup(argv[1]);
        return 0;
    }
    for (;;) {
        if (s->k >= s->len) { JS_ThrowTypeError(ctx, "empty array"); return -1; }
        k1 = (special & special_reduceRight) ? s->len - s->k - 1 : s->k;
        s->k++;
        if (special & special_TA) {
            s->acc = JS_GetPropertyInt64(ctx, s->obj, k1);
            if (JS_IsException(s->acc)) return -1;
            return 0;
        }
        present = JS_TryGetPropertyInt64(ctx, s->obj, k1, &s->acc);
        if (present < 0) return -1;
        if (present) return 0;
    }
}

/* One coroutine step: adopt the previous callback's result as the accumulator, then advance to the next present
   element and fill out_args with (acc, value, index, obj). 1 = CALL, 0 = DONE (s->acc is the result), -1 = EXC. */
static int js_array_reduce_step(JSContext *ctx, JSArrayReduce *s, JSValue acc1, JSValueConst out_args[4])
{
    if (s->pending) {
        s->pending = 0;
        JS_FreeValue(ctx, s->acc);
        s->acc = acc1;                 /* the callback's result IS the next accumulator (owned) */
        JS_FreeValue(ctx, s->val);
        s->val = JS_UNDEFINED;
    }
    while (s->k < s->len) {
        int64_t k1 = (s->special & special_reduceRight) ? s->len - s->k - 1 : s->k;
        int present;
        JSValue val;
        s->k++;
        if (s->special & special_TA) {
            val = JS_GetPropertyInt64(ctx, s->obj, k1);
            if (JS_IsException(val)) return -1;
            present = true;
        } else {
            present = JS_TryGetPropertyInt64(ctx, s->obj, k1, &val);
            if (present < 0) return -1;
        }
        if (present) {
            s->val = val;
            s->pending = 1;
            out_args[0] = s->acc;
            out_args[1] = val;
            out_args[2] = js_int64(k1);
            out_args[3] = s->obj;
            return 1;
        }
    }
    return 0;
}

static void js_array_reduce_end(JSContext *ctx, JSArrayReduce *s, bool take_acc)
{
    if (!take_acc)
        JS_FreeValue(ctx, s->acc);
    JS_FreeValue(ctx, s->val);
    JS_FreeValue(ctx, s->obj);
    s->acc = JS_UNDEFINED; s->val = JS_UNDEFINED; s->obj = JS_UNDEFINED;
}

static JSValue js_array_reduce(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int special)
{
    JSArrayReduce s;
    JSValueConst args[4];
    JSValue acc1 = JS_UNDEFINED, ret;
    int st;

    if (js_array_reduce_init(ctx, &s, this_val, argc, argv, special))
        goto exception;
    for (;;) {
        st = js_array_reduce_step(ctx, &s, acc1, args);   /* consumes acc1 */
        acc1 = JS_UNDEFINED;
        if (st < 0)
            goto exception;
        if (st == 0)
            break;
        acc1 = JS_Call(ctx, s.func, JS_UNDEFINED, 4, args);
        if (JS_IsException(acc1))
            goto exception;
    }
    ret = s.acc;
    js_array_reduce_end(ctx, &s, true);
    return ret;

exception:
    JS_FreeValue(ctx, acc1);
    js_array_reduce_end(ctx, &s, false);
    return JS_EXCEPTION;
}

/* Exact C-function identity + magic, as with js_array_every. */
static bool tramp_can_call_array_reduce(JSValueConst func, int *out_special)
{
    JSObject *fp;
    if (JS_VALUE_GET_TAG(func) != JS_TAG_OBJECT) return false;
    fp = JS_VALUE_GET_OBJ(func);
    if (fp->class_id != JS_CLASS_C_FUNCTION) return false;
    if (fp->u.cfunc.cproto != JS_CFUNC_generic_magic) return false;
    if (fp->u.cfunc.c_function.generic_magic != js_array_reduce) return false;
    *out_special = fp->u.cfunc.magic;
    return true;
}
