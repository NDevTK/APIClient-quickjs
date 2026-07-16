/* JSON.parse reviver — suspendable component (see CONT_JSON_REVIVE / JSJsonReviver in quickjs.c).
 *
 * ONE assertable contract: flatten internalize_json_property's C recursion into an EXPLICIT DFS stack so the
 * reviver runs on the tramp chain (a reviver body loop preempts) and resumes byte-identically. Post-order walk;
 * the sole suspension point is reviver.call(holder, name, val, context). Owns the source CString + parse record
 * + the frame stack; js_json_reviver_end releases them and returns the revived root (or JS_EXCEPTION).
 *
 * TEXTUALLY INCLUDED into quickjs.c (needs its static internals: JSONParseRecord, JS_ParseJSON_internal,
 * json_parse_record_*, JSPropertyEnum, etc.), keeping the fork's delta modular — one file, one contract — rather
 * than piling the walk into quickjs.c. The interpreter drive (do_json_revive_tramp/step) stays in JS_CallInternal
 * (goto labels), the struct/forward-decls stay early in quickjs.c (sizeof + call-site gating). */

/* ---- Suspendable JSON.parse reviver (explicit-stack post-order walk; see JSJsonReviver) ---- */
static int jr_push(JSContext *ctx, JSJsonReviver *s) {
    if (s->sp >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        JRFrame *ns = js_realloc(ctx, s->stack, (size_t)nc * sizeof(JRFrame));
        if (!ns) return -1;
        s->stack = ns; s->cap = nc;
    }
    JRFrame *f = &s->stack[s->sp++];
    f->holder = JS_UNDEFINED; f->val = JS_UNDEFINED; f->context = JS_UNDEFINED;
    f->name = JS_ATOM_NULL; f->name_val = JS_UNDEFINED; f->atoms = NULL;
    f->len = 0; f->i = 0; f->is_array = 0; f->fpr = NULL; f->vpr = NULL; f->phase = 0;
    return 0;
}
/* descend a HOLDER's parse-record to the child `name` whose value is `cval`, mirroring internalize's pr walk. */
static JSONParseRecord *jr_child_pr(JSContext *ctx, JSONParseRecord *holder_pr, JSAtom name, JSValueConst cval) {
    JSONParseRecord *pr = holder_pr;
    if (!pr) return NULL;
    if (js_is_array(ctx, pr->value)) {
        if (__JS_AtomIsTaggedInt(name)) {
            uint32_t idx = __JS_AtomToUInt32(name);
            pr = (idx < (uint32_t)pr->u.array.count) ? &pr->u.array.elements[idx] : NULL;
        } else pr = NULL;
    } else {
        pr = json_parse_record_find(pr, name);
    }
    if (pr && !js_same_value(ctx, pr->value, cval)) pr = NULL;
    return pr;
}
static int js_json_reviver_init(JSContext *ctx, JSJsonReviver *s, JSValueConst text_arg, JSValueConst reviver) {
    size_t len; JSValue parsed; JSONParseRecord *pr1; int size = 0;
    s->reviver = reviver; s->text = NULL; s->root = JS_UNDEFINED; s->pr = NULL;
    s->stack = NULL; s->sp = 0; s->cap = 0; s->result = JS_UNDEFINED;
    for (int i = 0; i < 5; i++) s->cb_args[i] = JS_UNDEFINED;
    s->text = JS_ToCStringLen(ctx, &len, text_arg);
    if (!s->text) return -1;
    s->pr = js_mallocz(ctx, sizeof(JSONParseRecord));
    if (!s->pr) return -1;
    s->root = JS_NewObject(ctx);
    if (JS_IsException(s->root)) return -1;
    json_parse_record_init_obj(ctx, s->pr, s->root);
    pr1 = json_parse_record_add(ctx, s->pr, JS_ATOM_empty_string, &size);
    if (!pr1) return -1;
    parsed = JS_ParseJSON_internal(ctx, s->text, len, "<input>", pr1);
    if (JS_IsException(parsed)) return -1;
    if (JS_DefinePropertyValue(ctx, s->root, JS_ATOM_empty_string, parsed, JS_PROP_C_W_E) < 0) return -1;
    if (jr_push(ctx, s) < 0) return -1;                 /* the root frame: holder={""}, name="", holder-record=pr */
    s->stack[0].holder = s->root; s->stack[0].name = JS_DupAtom(ctx, JS_ATOM_empty_string);
    s->stack[0].fpr = s->pr;
    return 0;
}
/* Drive the DFS until the next reviver call is needed (return 1, out_args=[name,val,context]) or done (0).
   `res` (owned) is the reviver's result for the just-completed node, or JS_UNDEFINED on the first call. */
static int js_json_reviver_step(JSContext *ctx, JSJsonReviver *s, JSValue res, JSValueConst out_args[3]) {
    if (s->sp > 0 && s->stack[s->sp - 1].phase == 2) {   /* a node's reviver just returned `res` */
        JRFrame *f = &s->stack[s->sp - 1];
        JSAtom fname = f->name;                          /* keep for the parent apply */
        if (f->atoms) js_free_prop_enum(ctx, f->atoms, f->len);
        JS_FreeValue(ctx, f->name_val);
        JS_FreeValue(ctx, f->val);
        JS_FreeValue(ctx, f->context);
        s->sp--;
        if (s->sp == 0) { JS_FreeAtom(ctx, fname); s->result = res; return 0; }   /* root: done */
        JRFrame *p = &s->stack[s->sp - 1];
        int ret;
        if (JS_IsUndefined(res)) { JS_FreeValue(ctx, res); ret = JS_DeleteProperty(ctx, p->val, fname, 0); }
        else ret = JS_DefinePropertyValue(ctx, p->val, fname, res, JS_PROP_C_W_E);   /* consumes res */
        JS_FreeAtom(ctx, fname);
        if (ret < 0) return -1;
        p->i++;
    } else {
        JS_FreeValue(ctx, res);                          /* JS_UNDEFINED on the first call */
    }
    while (s->sp > 0) {
        JRFrame *f = &s->stack[s->sp - 1];
        if (f->phase == 0) {
            f->val = JS_GetProperty(ctx, f->holder, f->name);
            if (JS_IsException(f->val)) return -1;
            f->vpr = jr_child_pr(ctx, f->fpr, f->name, f->val);   /* val's own parse record */
            f->context = JS_NewObject(ctx);
            if (JS_IsException(f->context)) return -1;
            if (JS_IsObject(f->val)) {
                f->is_array = js_is_array(ctx, f->val);
                if (f->is_array < 0) return -1;
                if (f->is_array) { if (js_get_length32(ctx, &f->len, f->val)) return -1; }
                else { int r = JS_GetOwnPropertyNamesInternal(ctx, &f->atoms, &f->len, JS_VALUE_GET_OBJ(f->val),
                                                              JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK); if (r < 0) return -1; }
            } else if (f->vpr) {   /* primitive with a source record -> context.source */
                JSValue src = JS_NewStringLen(ctx, s->text + f->vpr->u.primitive.source_pos, f->vpr->u.primitive.source_len);
                if (JS_IsException(src)) return -1;
                if (JS_DefinePropertyValue(ctx, f->context, JS_ATOM_source, src, JS_PROP_C_W_E) < 0) return -1;
            }
            f->phase = 1;
        }
        if ((int64_t)f->i < (int64_t)f->len) {           /* descend into child i */
            JSAtom prop;
            if (f->is_array) { prop = JS_NewAtomUInt32(ctx, f->i); if (prop == JS_ATOM_NULL) return -1; }
            else prop = JS_DupAtom(ctx, f->atoms[f->i].atom);
            JSValueConst pval = f->val; JSONParseRecord *pvpr = f->vpr;   /* snapshot before push may realloc s->stack */
            if (jr_push(ctx, s) < 0) { JS_FreeAtom(ctx, prop); return -1; }
            JRFrame *child = &s->stack[s->sp - 1];
            child->holder = pval; child->name = prop; child->fpr = pvpr;   /* child's holder-record = this val's record */
            continue;
        }
        /* children done -> call the reviver on this node (SUSPEND) */
        f->name_val = JS_AtomToValue(ctx, f->name);
        if (JS_IsException(f->name_val)) return -1;
        s->cb_args[0] = f->holder;                       /* this = holder (borrowed) */
        out_args[0] = f->name_val; out_args[1] = f->val; out_args[2] = f->context;
        f->phase = 2;
        return 1;
    }
    return 0;
}
static JSValue js_json_reviver_end(JSContext *ctx, JSJsonReviver *s, bool ok) {
    while (s->sp > 0) {   /* free any frames still open (exception mid-walk) */
        JRFrame *f = &s->stack[--s->sp];
        if (f->atoms) js_free_prop_enum(ctx, f->atoms, f->len);
        JS_FreeValue(ctx, f->name_val);
        JS_FreeValue(ctx, f->val);
        JS_FreeValue(ctx, f->context);
        JS_FreeAtom(ctx, f->name);
    }
    js_free(ctx, s->stack); s->stack = NULL;
    if (s->pr) { json_free_parse_record(ctx, s->pr); js_free(ctx, s->pr); s->pr = NULL; }
    JS_FreeValue(ctx, s->root); s->root = JS_UNDEFINED;
    if (s->text) { JS_FreeCString(ctx, s->text); s->text = NULL; }
    if (!ok) { JS_FreeValue(ctx, s->result); return JS_EXCEPTION; }
    return s->result;
}
static bool tramp_can_call_json_parse(JSValueConst func, int argc, JSValueConst *argv) {
    JSObject *mp, *rp;
    if (JS_VALUE_GET_TAG(func) != JS_TAG_OBJECT) return false;
    mp = JS_VALUE_GET_OBJ(func);
    if (!(mp->class_id == JS_CLASS_C_FUNCTION && mp->u.cfunc.c_function.generic == js_json_parse)) return false;
    if (argc < 2 || JS_VALUE_GET_TAG(argv[1]) != JS_TAG_OBJECT) return false;
    rp = JS_VALUE_GET_OBJ(argv[1]);
    return rp->class_id == JS_CLASS_BYTECODE_FUNCTION && rp->u.func.function_bytecode->func_kind == JS_FUNC_NORMAL;
}

