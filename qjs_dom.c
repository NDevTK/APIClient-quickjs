/* QuickJS <-> Lexbor DOM binding. The analyzed bundle's `document` and
   every element ARE Lexbor nodes (spec HTML5 parser + DOM + CSS
   selectors), not hand-rolled stubs. Node identity is the Lexbor
   node's own `user` slot: one stable JS wrapper per node (weak — the
   finalizer clears it; recreated on next access since the document
   owns the node). Only the DOM is bound here; the host edge
   (fetch/XHR/eval/opaque/taint) stays in hostedge.js unchanged. */
#include "quickjs.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/url/url.h>
#include <lexbor/encoding/encoding.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static JSClassID dom_cid;
static lxb_html_document_t *g_doc;
static lxb_css_parser_t   *g_cssp;
static lxb_selectors_t    *g_sel;

/* ---- wrapper / identity ---------------------------------------- */
static JSValue dom_wrap(JSContext *ctx, lxb_dom_node_t *n) {
    if (n == NULL) return JS_NULL;
    if (n->user != NULL)
        return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, n->user));
    JSValue o = JS_NewObjectClass(ctx, dom_cid);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, n);
    n->user = JS_VALUE_GET_PTR(o);          /* weak back-pointer */
    return o;
}
static void dom_finalizer(JSRuntime *rt, JSValue val) {
    lxb_dom_node_t *n = JS_GetOpaque(val, dom_cid);
    if (n != NULL && n->user == JS_VALUE_GET_PTR(val)) n->user = NULL;
}
static lxb_dom_node_t *self_node(JSValueConst v) { return JS_GetOpaque(v, dom_cid); }
/* Element APIs (get/set attribute, qualified_name, inner_html) must
   NEVER be called on a non-element node (document/text/comment) — the
   interface cast would write through a wrong struct and corrupt the
   heap. Return NULL for non-elements; callers no-op safely. */
static lxb_dom_element_t *el_of(lxb_dom_node_t *n) {
    return (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        ? lxb_dom_interface_element(n) : NULL;
}

/* ---- helpers --------------------------------------------------- */
typedef struct { char *buf; size_t len, cap; } sbuf_t;
static lxb_status_t ser_cb(const lxb_char_t *d, size_t l, void *c) {
    sbuf_t *s = c;
    if (s->len + l + 1 > s->cap) {
        size_t nc = (s->cap ? s->cap * 2 : 256);
        while (nc < s->len + l + 1) nc *= 2;
        char *nb = realloc(s->buf, nc);
        if (!nb) return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        s->buf = nb; s->cap = nc;
    }
    memcpy(s->buf + s->len, d, l); s->len += l;
    return LXB_STATUS_OK;
}
static JSValue ser_children(JSContext *ctx, lxb_dom_node_t *n) {
    sbuf_t s = {0};
    for (lxb_dom_node_t *c = n->first_child; c; c = c->next)
        lxb_html_serialize_tree_cb(c, ser_cb, &s);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len);
    free(s.buf); return r;
}
static JSValue ser_self(JSContext *ctx, lxb_dom_node_t *n) {
    sbuf_t s = {0};
    lxb_html_serialize_tree_cb(n, ser_cb, &s);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len);
    free(s.buf); return r;
}

/* find: collect matched nodes */
typedef struct { JSContext *ctx; JSValue arr; uint32_t n; int one; lxb_dom_node_t *first; } findctx_t;
static lxb_status_t find_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t sp, void *c) {
    findctx_t *f = c;
    if (f->one) { f->first = node; return LXB_STATUS_STOP; }
    JS_SetPropertyUint32(f->ctx, f->arr, f->n++, dom_wrap(f->ctx, node));
    return LXB_STATUS_OK;
}
/* Spec NodeList.item(i): indexed accessor (returns null when out of range).
   Bundles iterate via list.item(t) — github, Microsoft Learn (`z2`'s
   `e.item(t)` on the meta-tags query) — and a plain Array (no `.item`)
   throws "is not a function", aborting the bundle's boot before any of
   its fetches fire. Native ECMA-via-Lexbor: `this[idx]` is the same
   indexed access the spec defines, so item(i) is one-line. */
static JSValue m_nodelist_item(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    if (ac < 1) return JS_NULL;
    int32_t idx;
    if (JS_ToInt32(ctx, &idx, av[0]) < 0) return JS_EXCEPTION;
    JSValue lenv = JS_GetPropertyStr(ctx, t, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
    if (idx < 0 || idx >= len) return JS_NULL;
    return JS_GetPropertyUint32(ctx, t, (uint32_t)idx);
}
static JSValue make_nodelist(JSContext *ctx) {
    JSValue a = JS_NewArray(ctx);
    /* Defined as configurable+non-enumerable so it doesn't appear in for-in /
       JSON.stringify enumeration of the array's numeric entries — matches
       the spec NodeList shape (item is a method, not a "key"). */
    JS_DefinePropertyValueStr(ctx, a, "item",
        JS_NewCFunction(ctx, m_nodelist_item, "item", 1),
        JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return a;
}
static JSValue do_query(JSContext *ctx, lxb_dom_node_t *root, const char *sel, size_t slen, int one) {
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(g_cssp, (const lxb_char_t *)sel, slen);
    if (list == NULL) return one ? JS_NULL : make_nodelist(ctx);
    findctx_t f = { ctx, JS_UNDEFINED, 0, one, NULL };
    if (!one) f.arr = make_nodelist(ctx);
    lxb_selectors_find(g_sel, root, list, find_cb, &f);
    if (one) return f.first ? dom_wrap(ctx, f.first) : JS_NULL;
    return f.arr;
}

/* ---- methods --------------------------------------------------- */
static JSValue m_createElement(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    size_t l; const char *nm = JS_ToCStringLen(ctx, &l, av[0]);
    if (!nm) return JS_EXCEPTION;
    lxb_dom_element_t *el = lxb_dom_document_create_element(&g_doc->dom_document,
        (const lxb_char_t *)nm, l, NULL);
    JS_FreeCString(ctx, nm);
    return el ? dom_wrap(ctx, lxb_dom_interface_node(el)) : JS_NULL;
}
static JSValue m_createTextNode(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, av[0]);
    if (!s) return JS_EXCEPTION;
    lxb_dom_text_t *tn = lxb_dom_document_create_text_node(&g_doc->dom_document,
        (const lxb_char_t *)s, l);
    JS_FreeCString(ctx, s);
    return tn ? dom_wrap(ctx, lxb_dom_interface_node(tn)) : JS_NULL;
}
/* Fire custom-element connection reactions for a just-inserted node. The
   JS-side __ceConnect (qjs-dom-prelude) upgrades + runs connectedCallback for
   any registered custom element in `node`'s subtree — so a JS-injected loader
   like <include-fragment src> runs its fetch under forced execution. Cheap:
   __ceConnect returns immediately when no custom element is registered. */
static void qjs_ce_connect(JSContext *ctx, JSValueConst node) {
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, glob, "__ceConnect");
    if (JS_IsFunction(ctx, fn)) {
        JSValue arg = JS_DupValue(ctx, node);
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, glob);
}
/* Fire the custom-element attributeChangedCallback reaction for a just-changed
   attribute. The JS-side __ceAttr (prelude) checks the CE class's
   observedAttributes and runs attributeChangedCallback(name,old,new) under
   forced execution -- so a loader that fetches on a src/data-* change
   (include-fragment src, and many web components) is driven, not just ones that
   fetch in connectedCallback. No-ops when no custom element is registered. */
static void qjs_ce_attr(JSContext *ctx, JSValueConst node, JSValueConst name,
                        JSValueConst oldv, JSValueConst newv) {
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, glob, "__ceAttr");
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst args[4] = { node, name, oldv, newv };
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 4, args);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, glob);
}
/* WHATWG "ensure pre-insertion validity": is `anc` an INCLUSIVE ancestor of
   `node` (anc == node, or anc contains node)? Inserting `anc` under `node`
   would make `node` its own descendant — the spec throws HierarchyRequestError.
   Lexbor's raw append/insert does NO such check (it trusts well-formed parser
   input), so it instead splices the next/child chain into a CYCLE, and the
   next sibling/tree walk (g_prevSibling's `for(;p&&p!=n;p=p->next)`) loops in C
   forever — no interrupt poll, no yield → the whole grind freezes (verified
   live + native on learn.microsoft.com). Forced execution feeds these bindings
   arbitrary node args, so the guard the browser always applies is load-bearing
   here. */
static int dom_is_inclusive_ancestor(lxb_dom_node_t *anc, lxb_dom_node_t *node) {
    for (lxb_dom_node_t *q = node; q; q = q->parent)
        if (q == anc) return 1;
    return 0;
}
static void qjs_load_dynamic_script(JSContext *ctx, JSValueConst scriptVal);   /* dyn <script src> loader (def. near qjs_run_doc_scripts) */
static JSValue m_appendChild(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *p = self_node(t), *c = self_node(av[0]);
    if (!p || !c) return JS_NULL;
    if (dom_is_inclusive_ancestor(c, p))
        return JS_ThrowTypeError(ctx, "appendChild: node is a host-including inclusive ancestor of the parent (would cycle the tree)");
    /* DOM move semantics: a node already in the tree must be UNLINKED from its
       old position first. Lexbor's low-level append/insert is parser-oriented
       (assumes a detached node) and does NOT unlink — it just overwrites
       node->parent/next/prev, leaving the OLD neighbors still pointing at the
       node, which corrupts the sibling chain into a CYCLE. Frameworks re-parent
       constantly (move an attached node), so without this every move could
       cycle the tree → later sibling/descendant walks loop forever in C
       (freeze #2 on learn.microsoft.com: lxb_dom_node_shadow_including_
       descendants under insert_before). */
    if (c->parent) lxb_dom_node_remove(c);
    lxb_dom_node_append_child(p, c);
    /* Fire CE connection reactions ONLY when appending INTO the document tree
       (spec: connectedCallback fires on document-connection). This restores the
       createElement(CE)+appendChild render-cycle the prelude's up()/__ceConnect
       design intends (see qjs_dom.c "CE-reactions on CONNECTION" comment): a
       JS-INJECTED loader (<include-fragment src>, image-cropper, React-rendered
       custom element) runs its connectedCallback fetch — goal #1's unused-feature
       surface (confirmed lost by the _ce_render gate). The original perf worry
       (appendChild is the hottest op; a per-call subtree walk slowed real
       bundles) is addressed by the document-connection gate: OFF-document bulk
       construction skips (the parent-walk finds no DOCUMENT and returns — O(depth),
       no JS call, no subtree walk); only the single append that connects a
       subtree to the document fires qjs_ce_connect. createElement of a CE is no
       longer the "broken null-tagName path" — dom_ctor builds a real element and
       up() upgrades it. */
    { lxb_dom_node_t *q = p; while (q) { if (q->type == LXB_DOM_NODE_TYPE_DOCUMENT) { qjs_ce_connect(ctx, av[0]); qjs_load_dynamic_script(ctx, av[0]); break; } q = q->parent; } }
    return JS_DupValue(ctx, av[0]);
}
static JSValue m_insertBefore(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *p = self_node(t), *c = self_node(av[0]);
    lxb_dom_node_t *r = (ac > 1) ? self_node(av[1]) : NULL;
    if (!p || !c) return JS_NULL;
    /* Cycle guards: c can't be an inclusive ancestor of the parent (c under p
       would loop) NOR of the ref node r (inserting c BEFORE its own descendant
       r links c under r's parent — inside c's own subtree — so Lexbor's
       insert_before then DFS-walks c's now-cyclic descendants forever:
       lxb_dom_node_shadow_including_descendants, the freeze #2 hang on
       learn.microsoft.com). */
    if (dom_is_inclusive_ancestor(c, p) || (r && dom_is_inclusive_ancestor(c, r)))
        return JS_ThrowTypeError(ctx, "insertBefore: node is an inclusive ancestor of the parent or reference node (would cycle the tree)");
    if (c->parent) lxb_dom_node_remove(c);   /* DOM move semantics — unlink before re-insert (see m_appendChild) */
    if (r) lxb_dom_node_insert_before(r, c); else lxb_dom_node_append_child(p, c);
    /* CE connection reactions on document-connecting insert — same rationale +
       perf gate as m_appendChild. */
    { lxb_dom_node_t *q = p; while (q) { if (q->type == LXB_DOM_NODE_TYPE_DOCUMENT) { qjs_ce_connect(ctx, av[0]); qjs_load_dynamic_script(ctx, av[0]); break; } q = q->parent; } }
    return JS_DupValue(ctx, av[0]);
}
static JSValue m_removeChild(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *c = self_node(av[0]);
    if (c) lxb_dom_node_remove(c);
    return ac ? JS_DupValue(ctx, av[0]) : JS_NULL;
}
/* WHATWG replaceChild(newChild, oldChild): put newChild (av[0]) where oldChild
   (av[1]) was. Was previously MIS-MAPPED to m_removeChild, which removed the
   NEW node and left old in place — silently corrupting frameworks' render
   trees. lit-html (Microsoft Learn) reconciles via replaceChild, and the
   corrupted tree fed a cyclic child list that froze `removeChildNodes`'s
   `for(t=e.firstChild; t; t=e.firstChild) e.removeChild(t)` (freeze #3). Move
   semantics: unlink new, insert it before old, then remove old. */
static JSValue m_replaceChild(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *p = self_node(t);
    lxb_dom_node_t *nw = (ac > 0) ? self_node(av[0]) : NULL;
    lxb_dom_node_t *old = (ac > 1) ? self_node(av[1]) : NULL;
    if (!p || !nw || !old) return JS_NULL;
    if (dom_is_inclusive_ancestor(nw, p))
        return JS_ThrowTypeError(ctx, "replaceChild: new node is an inclusive ancestor of the parent (would cycle the tree)");
    if (old->parent != p) return JS_NULL;       /* spec NotFoundError: old must be a child of p */
    if (nw == old) return JS_DupValue(ctx, av[1]);
    if (nw->parent) lxb_dom_node_remove(nw);    /* move semantics — unlink new first */
    lxb_dom_node_insert_before(old, nw);        /* new takes old's position */
    lxb_dom_node_remove(old);                   /* then remove old */
    return JS_DupValue(ctx, av[1]);             /* spec: returns oldChild */
}
static JSValue m_setAttribute(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_UNDEFINED;
    size_t kl, vl; const char *k = JS_ToCStringLen(ctx, &kl, av[0]);
    const char *v = JS_ToCStringLen(ctx, &vl, av[1]);
    if (k && v) {
        /* Snapshot the prior value (a copy, valid after the set) for the
           attributeChangedCallback(name, oldValue, newValue) reaction below. */
        size_t ovl = 0;
        const lxb_char_t *ov = lxb_dom_element_get_attribute(el,
            (const lxb_char_t *)k, kl, &ovl);
        JSValue oldv = ov ? JS_NewStringLen(ctx, (const char *)ov, ovl) : JS_NULL;
        lxb_dom_element_set_attribute(el,
            (const lxb_char_t *)k, kl, (const lxb_char_t *)v, vl);
        /* Custom-element attribute reaction: a defined CE with this attribute in
           its observedAttributes runs attributeChangedCallback under forced exec
           (the include-fragment src mechanism + many web components fetch on an
           attribute change, not just in connectedCallback). */
        qjs_ce_attr(ctx, t, av[0], oldv, av[1]);
        JS_FreeValue(ctx, oldv);
    }
    if (k) JS_FreeCString(ctx, k); if (v) JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}
static JSValue m_getAttribute(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_NULL;
    size_t kl; const char *k = JS_ToCStringLen(ctx, &kl, av[0]);
    if (!k) return JS_NULL;
    size_t vl = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)k, kl, &vl);
    JS_FreeCString(ctx, k);
    return v ? JS_NewStringLen(ctx, (const char *)v, vl) : JS_NULL;
}
static JSValue m_hasAttribute(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_FALSE;
    size_t kl; const char *k = JS_ToCStringLen(ctx, &kl, av[0]);
    if (!k) return JS_FALSE;
    bool h = lxb_dom_element_has_attribute(el,
        (const lxb_char_t *)k, kl);
    JS_FreeCString(ctx, k);
    return JS_NewBool(ctx, h);
}
static JSValue m_removeAttribute(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_UNDEFINED;
    size_t kl; const char *k = JS_ToCStringLen(ctx, &kl, av[0]);
    if (k) { lxb_dom_element_remove_attribute(el,
        (const lxb_char_t *)k, kl); JS_FreeCString(ctx, k); }
    return JS_UNDEFINED;
}
static JSValue m_querySelector(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, av[0]);
    if (!s) return JS_NULL;
    JSValue r = do_query(ctx, n, s, l, 1); JS_FreeCString(ctx, s); return r;
}
static JSValue m_querySelectorAll(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NewArray(ctx);
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, av[0]);
    if (!s) return JS_NewArray(ctx);
    JSValue r = do_query(ctx, n, s, l, 0); JS_FreeCString(ctx, s); return r;
}
static JSValue m_getElementById(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    size_t l; const char *id = JS_ToCStringLen(ctx, &l, av[0]);
    if (!id) return JS_NULL;
    char *q = malloc(l + 2); q[0] = '#'; memcpy(q + 1, id, l); q[l + 1] = 0;
    JSValue r = do_query(ctx, n, q, l + 1, 1);
    free(q); JS_FreeCString(ctx, id); return r;
}
static JSValue m_getElementsByTagName(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NewArray(ctx);
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, av[0]);
    if (!s) return JS_NewArray(ctx);
    JSValue r = (l == 1 && s[0] == '*') ? do_query(ctx, n, "*", 1, 0)
                                        : do_query(ctx, n, s, l, 0);
    JS_FreeCString(ctx, s); return r;
}
static JSValue m_getElementsByClassName(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NewArray(ctx);
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, av[0]);
    if (!s) return JS_NewArray(ctx);
    char *q = malloc(l + 2); q[0] = '.';
    for (size_t i = 0; i < l; i++) q[i + 1] = (s[i] == ' ') ? '.' : s[i];
    q[l + 1] = 0;
    JSValue r = do_query(ctx, n, q, l + 1, 0);
    free(q); JS_FreeCString(ctx, s); return r;
}
static JSValue m_contains(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *a = self_node(t), *b = self_node(av[0]);
    while (b) { if (b == a) return JS_TRUE; b = b->parent; }
    return JS_FALSE;
}
static JSValue m_noop(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) { return JS_UNDEFINED; }
/* DOM-interface constructor. `class C extends HTMLElement {}` requires
   the parent to be a real constructor; a plain JS_NewCFunction is not,
   so QuickJS throws "parent class must be constructor" and the whole
   bundle aborts (GitHub Catalyst / web components do this at module
   top level). For a JS_CFUNC_constructor, new_target is passed as the
   `this` arg — build the instance from new_target.prototype so the
   chain is C.prototype -> HTMLElement.prototype (the Lexbor node
   proto). The instance is not a live Lexbor node; element-mutating ops
   are el_of()-guarded so they no-op, and addEventListener/classList/
   etc. resolve off the shared proto. */
static JSValue dom_ctor(JSContext *ctx, JSValueConst nt, int ac, JSValueConst *av) {
    /* HTML spec custom-element upgrade: the construction stack. When
       customElements.define upgrades an element already in the parsed
       document, it pushes that live element in __ceUp and runs the
       class ctor; super() reaches here and must return THAT element so
       the constructor initialises the real element in place (Turbo's
       `this.delegate=new Delegate(this)` then binds the real element).
       Single-slot, consumed on the first super() of the construct. */
    {
        JSValue glob = JS_GetGlobalObject(ctx);
        JSValue up = JS_GetPropertyStr(ctx, glob, "__ceUp");
        if (JS_IsObject(up)) {
            JS_SetPropertyStr(ctx, glob, "__ceUp", JS_NULL);
            JS_FreeValue(ctx, glob);
            return up;
        }
        JS_FreeValue(ctx, up);
        JS_FreeValue(ctx, glob);
    }
    JSValue pr = JS_GetPropertyStr(ctx, nt, "prototype");
    /* A defined custom element IS a live element whose local name is
       the registered tag (customElements.define stamps __ceTag on the
       class prototype). Build the real Lexbor element so tagName /
       localName / querySelector / appendChild behave per spec, then
       chain its [[Prototype]] to the user class prototype (which
       chains to HTMLElement.prototype = the node proto) so the class's
       own methods resolve too. Class id stays dom_cid, so the opaque
       live-node slot is intact (self_node still works). */
    JSValue tag = JS_IsObject(pr) ? JS_GetPropertyStr(ctx, pr, "__ceTag")
                                  : JS_UNDEFINED;
    JSValue obj = JS_UNDEFINED;
    if (JS_IsString(tag)) {
        size_t l; const char *nm = JS_ToCStringLen(ctx, &l, tag);
        lxb_dom_element_t *el = nm ? lxb_dom_document_create_element(
            &g_doc->dom_document, (const lxb_char_t *)nm, l, NULL) : NULL;
        if (nm) JS_FreeCString(ctx, nm);
        if (el) {
            obj = dom_wrap(ctx, lxb_dom_interface_node(el));
            JS_SetPrototype(ctx, obj, pr);
        }
    }
    if (JS_IsUndefined(obj))
        obj = JS_NewObjectProto(ctx, JS_IsObject(pr) ? pr : JS_NULL);
    JS_FreeValue(ctx, tag);
    JS_FreeValue(ctx, pr);
    return obj;
}
static JSValue m_cloneNode(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    bool deep = ac && JS_ToBool(ctx, av[0]);
    lxb_dom_node_t *cl = lxb_dom_node_clone(n, deep);
    return cl ? dom_wrap(ctx, cl) : JS_NULL;
}

/* ---- getters / setters ----------------------------------------- */
static JSValue g_tagName(JSContext *ctx, JSValueConst t) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_NULL;
    size_t l = 0; const lxb_char_t *q = lxb_dom_element_qualified_name(el, &l);
    if (!q) return JS_NULL;
    char *u = malloc(l); for (size_t i = 0; i < l; i++)
        u[i] = (q[i] >= 'a' && q[i] <= 'z') ? q[i] - 32 : q[i];
    JSValue r = JS_NewStringLen(ctx, u, l); free(u); return r;
}
static JSValue g_nodeName(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    if (n->type == LXB_DOM_NODE_TYPE_TEXT) return JS_NewString(ctx, "#text");
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) return JS_NewString(ctx, "#document");
    return g_tagName(ctx, t);
}
static JSValue g_nodeType(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); return JS_NewInt32(ctx, n ? (int)n->type : 0);
}
/* HTMLTemplateElement.content is the parsed-content DocumentFragment —
   a real node with appendChild, NOT the string the generic attribute-
   reflection list assumed (that made `tpl.content.appendChild` throw
   "not a function" on GitHub's task-lists element). <meta content> is
   the only other standard `content`: a plain attribute string. */
static JSValue g_content(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_UNDEFINED;
    if (n->local_name == LXB_TAG_TEMPLATE) {
        lxb_dom_document_fragment_t *frag =
            ((lxb_html_template_element_t *)n)->content;
        return frag ? dom_wrap(ctx, lxb_dom_interface_node(frag)) : JS_NULL;
    }
    lxb_dom_element_t *el = el_of(n);
    if (el) {
        size_t l = 0; const lxb_char_t *v = lxb_dom_element_get_attribute(
            el, (const lxb_char_t *)"content", 7, &l);
        if (v) return JS_NewStringLen(ctx, (const char *)v, l);
    }
    return JS_UNDEFINED;
}
static JSValue g_id(JSContext *ctx, JSValueConst t) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_NewString(ctx, "");
    size_t l = 0; const lxb_char_t *v =
        lxb_dom_element_get_attribute(el, (const lxb_char_t *)"id", 2, &l);
    return v ? JS_NewStringLen(ctx, (const char *)v, l) : JS_NewString(ctx, "");
}
static JSValue s_id(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (s) { lxb_dom_element_set_attribute(el,
        (const lxb_char_t *)"id", 2, (const lxb_char_t *)s, l); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue g_className(JSContext *ctx, JSValueConst t) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_NewString(ctx, "");
    size_t l = 0; const lxb_char_t *v =
        lxb_dom_element_get_attribute(el, (const lxb_char_t *)"class", 5, &l);
    return v ? JS_NewStringLen(ctx, (const char *)v, l) : JS_NewString(ctx, "");
}
static JSValue s_className(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (s) { lxb_dom_element_set_attribute(el,
        (const lxb_char_t *)"class", 5, (const lxb_char_t *)s, l); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
/* Reflected global string attributes (HTMLElement.lang/dir/title, …). Same
   shape as g_id/g_className but magic-indexed by a name table so one pair of
   functions covers every plain string-reflected attribute. `documentElement.lang`
   feeds many bundles' locale (MS Learn `b.data.userLocale` → the content-nav
   URL); without this it read undefined and the URL became `/undefined/…`. */
static const char *const dom_refl_attrs[] = { "lang", "dir", "title" };
static JSValue g_refl(JSContext *ctx, JSValueConst t, int magic) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_NewString(ctx, "");
    const char *name = dom_refl_attrs[magic]; size_t l = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &l);
    return v ? JS_NewStringLen(ctx, (const char *)v, l) : JS_NewString(ctx, "");
}
static JSValue s_refl(JSContext *ctx, JSValueConst t, JSValueConst v, int magic) {
    lxb_dom_element_t *el = el_of(self_node(t)); if (!el) return JS_UNDEFINED;
    const char *name = dom_refl_attrs[magic]; size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (s) { lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)s, l); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue g_innerHTML(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); return n ? ser_children(ctx, n) : JS_NewString(ctx, "");
}
static JSValue s_innerHTML(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_UNDEFINED;
    lxb_dom_element_t *el = el_of(n); if (!el) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (!s) return JS_UNDEFINED;
    /* WHATWG HTMLTemplateElement: innerHTML setter parses INTO the
       template's content DocumentFragment, NOT into the template
       element's children directly. Without this, `template.innerHTML =
       "<div>…</div>"` leaves `template.content.firstChild === null` —
       which breaks lit-html's template parser (it walks template.content
       via TreeWalker; an empty fragment makes the parse loop spin
       waiting for nodes that never arrive). Detect template, parse the
       HTML in a fresh fragment-parse, and append the parsed children
       to template.content. */
    if (n->local_name == LXB_TAG_TEMPLATE) {
        lxb_html_template_element_t *tpl = (lxb_html_template_element_t *)n;
        lxb_dom_document_fragment_t *frag = tpl->content;
        if (frag) {
            lxb_dom_node_t *frag_node = lxb_dom_interface_node(frag);
            /* Clear existing children of the fragment so repeated
               `tpl.innerHTML = "…"` replaces, matches WHATWG. */
            while (frag_node->first_child) {
                lxb_dom_node_t *c = frag_node->first_child;
                lxb_dom_node_remove(c);
                lxb_dom_node_destroy_deep(c);
            }
            /* Parse the HTML in a body context (WHATWG: template
               content uses the body's parsing rules), then move parsed
               children into the fragment. */
            lxb_html_parser_t *p = lxb_html_parser_create();
            if (p && lxb_html_parser_init(p) == LXB_STATUS_OK) {
                lxb_html_body_element_t *body = lxb_html_document_body_element(g_doc);
                lxb_dom_node_t *parsed = lxb_html_parse_fragment(p,
                    lxb_html_interface_element(body),
                    (const lxb_char_t *)s, l);
                if (parsed) {
                    while (parsed->first_child) {
                        lxb_dom_node_t *c = parsed->first_child;
                        lxb_dom_node_remove(c);
                        lxb_dom_node_insert_child(frag_node, c);
                    }
                    lxb_dom_node_destroy_deep(parsed);
                }
                lxb_html_parser_destroy(p);
            }
            JS_FreeCString(ctx, s);
            return JS_UNDEFINED;
        }
    }
    lxb_html_element_inner_html_set(lxb_html_interface_element(el),
        (const lxb_char_t *)s, l);
    JS_FreeCString(ctx, s);
    /* Newly-parsed children are now connected to the live element — fire CE
       reactions so an injected <include-fragment src>/etc. runs its fetch.
       (Template content above is inert per spec, so it is intentionally
       excluded.) */
    qjs_ce_connect(ctx, t);
    return JS_UNDEFINED;
}
static JSValue g_outerHTML(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); return n ? ser_self(ctx, n) : JS_NewString(ctx, "");
}
/* WHATWG outerHTML setter: parse v as a fragment in the PARENT's
   context, splice the parsed children before `self`, then remove
   `self`. Without this, an outerHTML assignment was a silent no-op
   and security writes through this sink missed the @S record (the
   @S fires before the Lexbor call, but the lack of a real DOM
   mutation hid downstream behavior). The parent must be an Element
   per spec — outerHTML on the root document is a NoModificationAllowed
   error which we treat as a silent no-op (no host-side throw). */
static JSValue s_outerHTML(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_dom_node_t *n = self_node(t);
    if (!n || !n->parent) return JS_UNDEFINED;
    lxb_dom_element_t *par = el_of(n->parent);
    if (!par) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (!s) return JS_UNDEFINED;
    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (parser) {
        if (lxb_html_parser_init(parser) == LXB_STATUS_OK) {
            lxb_dom_node_t *frag = lxb_html_parse_fragment(parser,
                lxb_html_interface_element(par), (const lxb_char_t *)s, l);
            if (frag) {
                lxb_dom_node_t *c = frag->first_child;
                while (c) {
                    lxb_dom_node_t *next = c->next;
                    lxb_dom_node_remove(c);
                    lxb_dom_node_insert_before(n, c);
                    c = next;
                }
                lxb_dom_node_remove(n);
            }
        }
        lxb_html_parser_destroy(parser);
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
static JSValue g_textContent(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NewString(ctx, "");
    size_t l = 0; lxb_char_t *txt = lxb_dom_node_text_content(n, &l);
    return txt ? JS_NewStringLen(ctx, (const char *)txt, l) : JS_NewString(ctx, "");
}
static JSValue s_textContent(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (s) { lxb_dom_node_text_content_set(n, (const lxb_char_t *)s, l); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue g_parentNode(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); return (n && n->parent) ? dom_wrap(ctx, n->parent) : JS_NULL;
}
static JSValue g_firstChild(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); return (n && n->first_child) ? dom_wrap(ctx, n->first_child) : JS_NULL;
}
static JSValue g_nextSibling(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); return (n && n->next) ? dom_wrap(ctx, n->next) : JS_NULL;
}
static JSValue g_childNodes(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); JSValue a = JS_NewArray(ctx);
    if (!n) return a;
    uint32_t i = 0;
    for (lxb_dom_node_t *c = n->first_child; c; c = c->next)
        JS_SetPropertyUint32(ctx, a, i++, dom_wrap(ctx, c));
    return a;
}
static JSValue g_children(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); JSValue a = JS_NewArray(ctx);
    if (!n) return a;
    uint32_t i = 0;
    for (lxb_dom_node_t *c = n->first_child; c; c = c->next)
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT)
            JS_SetPropertyUint32(ctx, a, i++, dom_wrap(ctx, c));
    return a;
}
static JSValue g_firstElementChild(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    for (lxb_dom_node_t *c = n->first_child; c; c = c->next)
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) return dom_wrap(ctx, c);
    return JS_NULL;
}
static JSValue g_lastChild(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); if (!n || !n->first_child) return JS_NULL;
    lxb_dom_node_t *c = n->first_child; while (c->next) c = c->next;
    return dom_wrap(ctx, c);
}
static JSValue g_prevSibling(JSContext *ctx, JSValueConst t) {
    /* Lexbor siblings are doubly-linked — read n->prev DIRECTLY (O(1)). The
       old form walked parent->first_child forward to n (O(siblings) PER call),
       so a caller's `while(node.previousSibling)` over a large sibling list
       (a docs page's flat content) was O(n^2) — on learn.microsoft.com that
       was a multi-minute SYNC C loop in this getter with no interrupt poll,
       freezing the whole grind (gdb: #0 g_prevSibling under qjs_deep_step_c).
       O(1) here makes the same caller walk O(n) total. */
    lxb_dom_node_t *n = self_node(t);
    return (n && n->prev) ? dom_wrap(ctx, n->prev) : JS_NULL;
}
/* previousElementSibling: the previous SIBLING that is an element (skips text/
   comment nodes) — was wrongly aliased to g_prevSibling (immediate prev of any
   type), the asymmetric counterpart to g_nextElement which correctly skips. */
static JSValue g_prevElement(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    for (lxb_dom_node_t *c = n->prev; c; c = c->prev)
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) return dom_wrap(ctx, c);
    return JS_NULL;
}
static JSValue g_nextElement(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    for (lxb_dom_node_t *c = n->next; c; c = c->next)
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) return dom_wrap(ctx, c);
    return JS_NULL;
}
static JSValue g_ownerDocument(JSContext *ctx, JSValueConst t) {
    return dom_wrap(ctx, lxb_dom_interface_node(g_doc));
}
static JSValue g_attributes(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t); JSValue a = JS_NewArray(ctx);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return a;
    lxb_dom_element_t *el = lxb_dom_interface_element(n);
    lxb_dom_attr_t *attr = lxb_dom_element_first_attribute(el);
    uint32_t i = 0;
    while (attr) {
        size_t nl = 0, vl = 0;
        const lxb_char_t *nm = lxb_dom_attr_qualified_name(attr, &nl);
        const lxb_char_t *vv = lxb_dom_attr_value(attr, &vl);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, (const char *)(nm ? nm : (const lxb_char_t *)""), nm ? nl : 0));
        JS_SetPropertyStr(ctx, o, "value", JS_NewStringLen(ctx, (const char *)(vv ? vv : (const lxb_char_t *)""), vv ? vl : 0));
        JS_SetPropertyUint32(ctx, a, i++, o);
        attr = lxb_dom_element_next_attribute(attr);
    }
    return a;
}
static JSValue g_nodeValue(JSContext *ctx, JSValueConst t) {
    lxb_dom_node_t *n = self_node(t);
    if (!n || (n->type != LXB_DOM_NODE_TYPE_TEXT && n->type != LXB_DOM_NODE_TYPE_COMMENT))
        return JS_NULL;
    size_t l = 0; lxb_char_t *txt = lxb_dom_node_text_content(n, &l);
    return txt ? JS_NewStringLen(ctx, (const char *)txt, l) : JS_NULL;
}
static JSValue m_matches(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_FALSE;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, av[0]);
    if (!s) return JS_FALSE;
    /* match = element appears in parent-scope query for the selector */
    lxb_dom_node_t *root = n->parent ? n->parent : n;
    JSValue arr = do_query(ctx, root, s, l, 0);
    JS_FreeCString(ctx, s);
    JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t len = 0; JS_ToUint32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
    bool hit = false;
    for (uint32_t i = 0; i < len && !hit; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (self_node(e) == n) hit = true;
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, arr);
    return JS_NewBool(ctx, hit);
}
static JSValue m_closest(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t);
    while (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        JSValue w = dom_wrap(ctx, n);
        JSValue m = m_matches(ctx, w, ac, av);
        int hit = JS_ToBool(ctx, m);
        JS_FreeValue(ctx, m);
        if (hit) return w;
        JS_FreeValue(ctx, w);
        n = n->parent;
    }
    return JS_NULL;
}
/* WHATWG Element.insertAdjacentElement(where, element): insert a node
   at a position relative to this element. Returns the inserted element.
   `where` is one of "beforebegin" | "afterbegin" | "beforeend" | "afterend"
   per the spec. Distinct from insertAdjacentHTML — that parses HTML
   string; this moves an existing node. Frameworks (lit-html / catalyst
   modal wrappers in Microsoft Learn) call this when assembling fragments
   pre-attached to the DOM tree. */
static JSValue m_insertAdjacentElement(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_NULL;
    if (ac < 2) return JS_NULL;
    size_t pl; const char *pos = JS_ToCStringLen(ctx, &pl, av[0]);
    if (!pos) return JS_NULL;
    lxb_dom_node_t *el = self_node(av[1]);
    if (!el) { JS_FreeCString(ctx, pos); return JS_NULL; }
    if (dom_is_inclusive_ancestor(el, n)) { JS_FreeCString(ctx, pos); return JS_ThrowTypeError(ctx, "insertAdjacentElement: node is an inclusive ancestor of the target (would cycle the tree)"); }
    if (el->parent) lxb_dom_node_remove(el);   /* DOM move semantics — unlink before re-insert (see m_appendChild) */
    if (strcmp(pos, "beforeend") == 0) {
        lxb_dom_node_append_child(n, el);
    } else if (strcmp(pos, "afterbegin") == 0) {
        if (n->first_child) lxb_dom_node_insert_before(n->first_child, el);
        else lxb_dom_node_append_child(n, el);
    } else if (strcmp(pos, "beforebegin") == 0 && n->parent) {
        lxb_dom_node_insert_before(n, el);
    } else if (strcmp(pos, "afterend") == 0 && n->parent) {
        if (n->next) lxb_dom_node_insert_before(n->next, el);
        else lxb_dom_node_append_child(n->parent, el);
    }
    JS_FreeCString(ctx, pos);
    qjs_ce_connect(ctx, av[1]);   /* inserted element: fire CE connection reactions */
    return JS_DupValue(ctx, av[1]);   /* spec: returns the inserted element */
}
static JSValue m_insertAdjacentHTML(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_dom_node_t *n = self_node(t); if (!n) return JS_UNDEFINED;
    size_t pl, hl;
    const char *pos = JS_ToCStringLen(ctx, &pl, av[0]);
    const char *html = JS_ToCStringLen(ctx, &hl, av[1]);
    if (!pos || !html) { if (pos) JS_FreeCString(ctx, pos); if (html) JS_FreeCString(ctx, html); return JS_UNDEFINED; }
    lxb_dom_element_t *tmp = lxb_dom_document_create_element(&g_doc->dom_document,
        (const lxb_char_t *)"div", 3, NULL);
    lxb_html_element_inner_html_set(lxb_html_interface_element(tmp), (const lxb_char_t *)html, hl);
    lxb_dom_node_t *tn = lxb_dom_interface_node(tmp), *c, *nx;
    if (strcmp(pos, "beforeend") == 0) {
        for (c = tn->first_child; c; c = nx) { nx = c->next; lxb_dom_node_append_child(n, c); }
    } else if (strcmp(pos, "afterbegin") == 0) {
        lxb_dom_node_t *fc = n->first_child;
        for (c = tn->first_child; c; c = nx) { nx = c->next; if (fc) lxb_dom_node_insert_before(fc, c); else lxb_dom_node_append_child(n, c); }
    } else if (strcmp(pos, "beforebegin") == 0 && n->parent) {
        for (c = tn->first_child; c; c = nx) { nx = c->next; lxb_dom_node_insert_before(n, c); }
    } else if (strcmp(pos, "afterend") == 0 && n->parent) {
        lxb_dom_node_t *nn = n->next;
        for (c = tn->first_child; c; c = nx) { nx = c->next; if (nn) lxb_dom_node_insert_before(nn, c); else lxb_dom_node_append_child(n->parent, c); }
    }
    /* Fire CE connection reactions on the container that received the parsed
       children: beforeend/afterbegin insert into `n`, beforebegin/afterend
       into n->parent. Computed BEFORE freeing `pos`. */
    lxb_dom_node_t *ce_container = (strcmp(pos, "beforeend") == 0 || strcmp(pos, "afterbegin") == 0)
        ? n : (n->parent ? n->parent : n);
    JS_FreeCString(ctx, pos); JS_FreeCString(ctx, html);
    {
        JSValue cw = dom_wrap(ctx, ce_container);
        qjs_ce_connect(ctx, cw);
        JS_FreeValue(ctx, cw);
    }
    return JS_UNDEFINED;
}

/* document.__feLoadPage(html) — re-parse the raw server-rendered HTML
   into g_doc and refresh cached body/head/documentElement bindings.
   Called from hostedge.js at worker boot when content.js shipped the
   real page HTML via CONTENT_HTML. lxb_html_document_parse on an
   already-parsed document replaces its tree, so this is safe to call
   even though the seed was parsed at dom_init time. Old wrappers
   into the seed body/head become stale, which is correct: the
   document property re-cache below points at the fresh nodes. */
static JSValue m_feLoadPage(JSContext *ctx, JSValueConst t,
                            int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, argv[0]);
    if (!s) return JS_UNDEFINED;
    lxb_status_t st = lxb_html_document_parse(g_doc, (const lxb_char_t *)s, l);
    JS_FreeCString(ctx, s);
    if (st != LXB_STATUS_OK) return JS_UNDEFINED;
    lxb_dom_node_t *body = lxb_dom_interface_node(lxb_html_document_body_element(g_doc));
    lxb_dom_node_t *head = body ? body->parent->first_child : NULL;
    lxb_dom_node_t *htmlEl = body ? body->parent : NULL;
    JS_SetPropertyStr(ctx, t, "body", dom_wrap(ctx, body));
    JS_SetPropertyStr(ctx, t, "head", dom_wrap(ctx, head));
    JS_SetPropertyStr(ctx, t, "documentElement", dom_wrap(ctx, htmlEl));
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dom_proto[] = {
    JS_CFUNC_DEF("appendChild", 1, m_appendChild),
    JS_CFUNC_DEF("insertBefore", 2, m_insertBefore),
    JS_CFUNC_DEF("removeChild", 1, m_removeChild),
    JS_CFUNC_DEF("replaceChild", 2, m_replaceChild),
    JS_CFUNC_DEF("append", 1, m_appendChild),
    JS_CFUNC_DEF("remove", 0, m_noop),
    JS_CFUNC_DEF("cloneNode", 1, m_cloneNode),
    JS_CFUNC_DEF("contains", 1, m_contains),
    JS_CFUNC_DEF("setAttribute", 2, m_setAttribute),
    JS_CFUNC_DEF("getAttribute", 1, m_getAttribute),
    JS_CFUNC_DEF("hasAttribute", 1, m_hasAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, m_removeAttribute),
    JS_CFUNC_DEF("querySelector", 1, m_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, m_querySelectorAll),
    JS_CFUNC_DEF("getElementById", 1, m_getElementById),
    JS_CFUNC_DEF("getElementsByTagName", 1, m_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName", 1, m_getElementsByClassName),
    JS_CFUNC_DEF("createElement", 1, m_createElement),
    JS_CFUNC_DEF("createTextNode", 1, m_createTextNode),
    JS_CFUNC_DEF("matches", 1, m_matches),
    JS_CFUNC_DEF("closest", 1, m_closest),
    JS_CFUNC_DEF("insertAdjacentHTML", 2, m_insertAdjacentHTML),
    JS_CFUNC_DEF("insertAdjacentElement", 2, m_insertAdjacentElement),
    JS_CFUNC_DEF("getElementsByName", 1, m_getElementsByClassName),
    JS_CFUNC_DEF("hasChildNodes", 0, m_contains),
    JS_CGETSET_DEF("lastChild", g_lastChild, NULL),
    JS_CGETSET_DEF("lastElementChild", g_lastChild, NULL),
    JS_CGETSET_DEF("previousSibling", g_prevSibling, NULL),
    JS_CGETSET_DEF("previousElementSibling", g_prevElement, NULL),
    JS_CGETSET_DEF("nextElementSibling", g_nextElement, NULL),
    JS_CGETSET_DEF("ownerDocument", g_ownerDocument, NULL),
    JS_CGETSET_DEF("attributes", g_attributes, NULL),
    JS_CGETSET_DEF("nodeValue", g_nodeValue, NULL),
    JS_CGETSET_DEF("data", g_nodeValue, NULL),
    JS_CGETSET_DEF("tagName", g_tagName, NULL),
    JS_CGETSET_DEF("nodeName", g_nodeName, NULL),
    JS_CGETSET_DEF("nodeType", g_nodeType, NULL),
    JS_CGETSET_DEF("content", g_content, NULL),
    JS_CGETSET_DEF("id", g_id, s_id),
    JS_CGETSET_DEF("className", g_className, s_className),
    JS_CGETSET_MAGIC_DEF("lang", g_refl, s_refl, 0),
    JS_CGETSET_MAGIC_DEF("dir", g_refl, s_refl, 1),
    JS_CGETSET_MAGIC_DEF("title", g_refl, s_refl, 2),
    JS_CGETSET_DEF("innerHTML", g_innerHTML, s_innerHTML),
    JS_CGETSET_DEF("outerHTML", g_outerHTML, s_outerHTML),
    JS_CGETSET_DEF("textContent", g_textContent, s_textContent),
    JS_CGETSET_DEF("parentNode", g_parentNode, NULL),
    JS_CGETSET_DEF("parentElement", g_parentNode, NULL),
    JS_CGETSET_DEF("firstChild", g_firstChild, NULL),
    JS_CGETSET_DEF("firstElementChild", g_firstElementChild, NULL),
    JS_CGETSET_DEF("nextSibling", g_nextSibling, NULL),
    JS_CGETSET_DEF("childNodes", g_childNodes, NULL),
    JS_CGETSET_DEF("children", g_children, NULL),
};

/* ---- WHATWG URL (Lexbor url module) ---------------------------- */
/* `URL` is a Web API, not ECMAScript — QuickJS does not provide it and
   a hand-rolled JS parser would be subtly wrong (relative resolution,
   IDNA, path normalisation). Lexbor's url module IS the WHATWG URL
   Standard in C; bind it the same way the DOM is bound. Schema/endpoint
   learning depends on `new URL(x,base).pathname/.searchParams` being
   exactly spec-correct, so this is a real implementation, not a stub. */
static JSClassID url_cid;
static lxb_url_parser_t *g_urlp;

static void url_finalizer(JSRuntime *rt, JSValue v) {
    lxb_url_t *u = JS_GetOpaque(v, url_cid);
    if (u) lxb_url_destroy(u);
}
static lxb_url_t *url_of(JSValueConst v) { return JS_GetOpaque(v, url_cid); }

/* component serializers: build a string via the existing sbuf ser_cb */
static JSValue u_full(JSContext *ctx, const lxb_url_t *u, bool exfrag) {
    sbuf_t s = {0}; lxb_url_serialize(u, ser_cb, &s, exfrag);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len);
    free(s.buf); return r;
}
static void u_collect(sbuf_t *s, lxb_status_t (*fn)(const lxb_url_t *,
                       lexbor_serialize_cb_f, void *), const lxb_url_t *u) {
    if (u) fn(u, ser_cb, s);
}
static JSValue g_href(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); return u ? u_full(ctx, u, false) : JS_NewString(ctx, "");
}
static JSValue g_u_scheme(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; u_collect(&s, lxb_url_serialize_scheme, u);
    /* protocol per spec = scheme + ":" */
    if (s.len == 0 || s.buf[s.len - 1] != ':') ser_cb((const lxb_char_t *)":", 1, &s);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : ":", s.len); free(s.buf); return r;
}
static JSValue g_u_username(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; u_collect(&s, lxb_url_serialize_username, u);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len); free(s.buf); return r;
}
static JSValue g_u_password(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; u_collect(&s, lxb_url_serialize_password, u);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len); free(s.buf); return r;
}
static JSValue g_u_hostname(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; lxb_url_serialize_host(&u->host, ser_cb, &s);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len); free(s.buf); return r;
}
static JSValue g_u_port(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u || !u->has_port) return JS_NewString(ctx, "");
    sbuf_t s = {0}; u_collect(&s, lxb_url_serialize_port, u);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len); free(s.buf); return r;
}
static JSValue g_u_host(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; lxb_url_serialize_host(&u->host, ser_cb, &s);
    if (u->has_port) { ser_cb((const lxb_char_t *)":", 1, &s);
        u_collect(&s, lxb_url_serialize_port, u); }
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len); free(s.buf); return r;
}
static JSValue g_u_pathname(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; lxb_url_serialize_path(&u->path, ser_cb, &s);
    JSValue r = JS_NewStringLen(ctx, s.buf ? s.buf : "", s.len); free(s.buf); return r;
}
static JSValue g_u_search(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; u_collect(&s, lxb_url_serialize_query, u);
    if (s.len == 0) { free(s.buf); return JS_NewString(ctx, ""); }
    sbuf_t q = {0}; ser_cb((const lxb_char_t *)"?", 1, &q);
    ser_cb((const lxb_char_t *)s.buf, s.len, &q); free(s.buf);
    JSValue r = JS_NewStringLen(ctx, q.buf, q.len); free(q.buf); return r;
}
static JSValue g_u_hash(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "");
    sbuf_t s = {0}; u_collect(&s, lxb_url_serialize_fragment, u);
    if (s.len == 0) { free(s.buf); return JS_NewString(ctx, ""); }
    sbuf_t f = {0}; ser_cb((const lxb_char_t *)"#", 1, &f);
    ser_cb((const lxb_char_t *)s.buf, s.len, &f); free(s.buf);
    JSValue r = JS_NewStringLen(ctx, f.buf, f.len); free(f.buf); return r;
}
static JSValue g_u_origin(JSContext *ctx, JSValueConst t) {
    lxb_url_t *u = url_of(t); if (!u) return JS_NewString(ctx, "null");
    sbuf_t sc = {0}; u_collect(&sc, lxb_url_serialize_scheme, u);
    int special = sc.buf && (
        (sc.len == 4 && !memcmp(sc.buf, "http", 4)) ||
        (sc.len == 5 && !memcmp(sc.buf, "https", 5)) ||
        (sc.len == 2 && !memcmp(sc.buf, "ws", 2)) ||
        (sc.len == 3 && !memcmp(sc.buf, "wss", 3)) ||
        (sc.len == 3 && !memcmp(sc.buf, "ftp", 3)));
    if (!special) { free(sc.buf); return JS_NewString(ctx, "null"); }
    sbuf_t o = {0};
    ser_cb((const lxb_char_t *)sc.buf, sc.len, &o); free(sc.buf);
    ser_cb((const lxb_char_t *)"://", 3, &o);
    lxb_url_serialize_host(&u->host, ser_cb, &o);
    if (u->has_port) { ser_cb((const lxb_char_t *)":", 1, &o);
        u_collect(&o, lxb_url_serialize_port, u); }
    JSValue r = JS_NewStringLen(ctx, o.buf, o.len); free(o.buf); return r;
}
static JSValue g_u_search_params(JSContext *ctx, JSValueConst t) {
    /* URLSearchParams over the query. hostedge defines URLSearchParams
       (it runs before any bundle code, after this install), so resolve
       it at call time. LIVE two-way bind: the params carry a _feUrl
       back-ref to THIS url, so hostedge's URLSearchParams._sync
       re-serializes a mutation (url.searchParams.set("select", cols))
       into url.search — the supabase/axios query-builder pattern that a
       detached snapshot silently dropped (lost moat query params). */
    JSValue q = g_u_search(ctx, t);
    const char *qs = JS_ToCString(ctx, q);
    JSValue arg = JS_NewString(ctx, (qs && qs[0] == '?') ? qs + 1 : (qs ? qs : ""));
    if (qs) JS_FreeCString(ctx, qs);
    JS_FreeValue(ctx, q);
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue USP = JS_GetPropertyStr(ctx, glob, "URLSearchParams");
    JS_FreeValue(ctx, glob);
    JSValue r;
    if (JS_IsConstructor(ctx, USP)) {
        JSValueConst a[1] = { arg };
        r = JS_CallConstructor(ctx, USP, 1, a);
        if (!JS_IsException(r)) JS_SetPropertyStr(ctx, r, "_feUrl", JS_DupValue(ctx, t));
    } else r = JS_NewObject(ctx);
    JS_FreeValue(ctx, USP); JS_FreeValue(ctx, arg);
    return r;
}
/* setters via lxb_url_api_* (all reparse through the spec state machine) */
#define U_SET(NAME, FN, HASPARSER)                                          \
static JSValue NAME(JSContext *ctx, JSValueConst t, JSValueConst v) {        \
    lxb_url_t *u = url_of(t); if (!u) return JS_UNDEFINED;                   \
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);                  \
    if (s) { HASPARSER ? FN(u, g_urlp, (const lxb_char_t *)s, l)            \
                       : FN(u, (lxb_url_parser_t *)NULL, (const lxb_char_t *)s, l); \
             JS_FreeCString(ctx, s); }                                      \
    return JS_UNDEFINED;                                                    \
}
static JSValue s_u_href(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_url_t *u = url_of(t); if (!u) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (s) { lxb_url_api_href_set(u, g_urlp, (const lxb_char_t *)s, l); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
U_SET(s_u_protocol, lxb_url_api_protocol_set, 1)
U_SET(s_u_host,     lxb_url_api_host_set,     1)
U_SET(s_u_hostname, lxb_url_api_hostname_set, 1)
U_SET(s_u_port,     lxb_url_api_port_set,     1)
U_SET(s_u_pathname, lxb_url_api_pathname_set, 1)
U_SET(s_u_search,   lxb_url_api_search_set,   1)
U_SET(s_u_hash,     lxb_url_api_hash_set,     1)
static JSValue s_u_username(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_url_t *u = url_of(t); if (!u) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (s) { lxb_url_api_username_set(u, (const lxb_char_t *)s, l); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue s_u_password(JSContext *ctx, JSValueConst t, JSValueConst v) {
    lxb_url_t *u = url_of(t); if (!u) return JS_UNDEFINED;
    size_t l; const char *s = JS_ToCStringLen(ctx, &l, v);
    if (s) { lxb_url_api_password_set(u, (const lxb_char_t *)s, l); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue m_u_toString(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_url_t *u = url_of(t); return u ? u_full(ctx, u, false) : JS_NewString(ctx, "");
}
static lxb_url_t *url_parse_arg(JSContext *ctx, int ac, JSValueConst *av) {
    if (ac < 1) return NULL;
    size_t il; const char *in = JS_ToCStringLen(ctx, &il, av[0]);
    if (!in) return NULL;
    lxb_url_t *base = NULL;
    int own_base = 0;
    if (ac > 1 && !JS_IsUndefined(av[1]) && !JS_IsNull(av[1])) {
        /* WHATWG: base may be a URL object (no round-trip) or a string —
           use the URL object's parsed state directly when it's a URL, fall
           through to string parse otherwise. The ToString round-trip was
           costing us info AND triggering JS_ToString protocol overhead on
           every relative-URL build the bundle does. */
        lxb_url_t *bu = url_of(av[1]);
        if (bu) {
            base = bu;          /* borrowed: don't destroy */
        } else {
            size_t bl; const char *bs = JS_ToCStringLen(ctx, &bl, av[1]);
            if (bs) {
                base = lxb_url_parse(g_urlp, NULL, (const lxb_char_t *)bs, bl);
                JS_FreeCString(ctx, bs);
                own_base = base != NULL;
            }
        }
    }
    lxb_url_t *u = lxb_url_parse(g_urlp, base, (const lxb_char_t *)in, il);
    JS_FreeCString(ctx, in);
    if (own_base) lxb_url_destroy(base);
    return u;
}
static JSValue url_ctor(JSContext *ctx, JSValueConst nt, int ac, JSValueConst *av) {
    lxb_url_t *u = url_parse_arg(ctx, ac, av);
    if (!u) return JS_ThrowTypeError(ctx, "Failed to construct 'URL': Invalid URL");
    JSValue pr = JS_GetPropertyStr(ctx, nt, "prototype");
    JSValue o = JS_NewObjectProtoClass(ctx, JS_IsObject(pr) ? pr : JS_NULL, url_cid);
    JS_FreeValue(ctx, pr);
    if (JS_IsException(o)) { lxb_url_destroy(u); return o; }
    JS_SetOpaque(o, u);
    return o;
}
static JSValue url_canParse(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    lxb_url_t *u = url_parse_arg(ctx, ac, av);
    if (u) { lxb_url_destroy(u); return JS_TRUE; }
    return JS_FALSE;
}
static unsigned long g_blobseq;
static JSValue url_createObjectURL(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    char b[64]; int n = snprintf(b, sizeof b,
        "blob:null/%lx-%lx", (unsigned long)g_blobseq++, (unsigned long)g_blobseq);
    return JS_NewStringLen(ctx, b, n);
}
static const JSCFunctionListEntry url_proto[] = {
    JS_CGETSET_DEF("href", g_href, s_u_href),
    /* WHATWG URL.toString() is the spec's serializer — a METHOD, not a
       getter. ECMA-side code that does `let s = u.toString()` expects a
       callable; a getter makes `u.toString` a primitive that throws when
       invoked. Real-site bundles do this (Microsoft Learn / Apple /
       Google) — the framework path was building URLs and immediately
       calling toString() on them. */
    JS_CFUNC_DEF("toString", 0, m_u_toString),
    JS_CGETSET_DEF("origin", g_u_origin, NULL),
    JS_CGETSET_DEF("protocol", g_u_scheme, s_u_protocol),
    JS_CGETSET_DEF("username", g_u_username, s_u_username),
    JS_CGETSET_DEF("password", g_u_password, s_u_password),
    JS_CGETSET_DEF("host", g_u_host, s_u_host),
    JS_CGETSET_DEF("hostname", g_u_hostname, s_u_hostname),
    JS_CGETSET_DEF("port", g_u_port, s_u_port),
    JS_CGETSET_DEF("pathname", g_u_pathname, s_u_pathname),
    JS_CGETSET_DEF("search", g_u_search, s_u_search),
    JS_CGETSET_DEF("hash", g_u_hash, s_u_hash),
    JS_CGETSET_DEF("searchParams", g_u_search_params, NULL),
    JS_CFUNC_DEF("toJSON", 0, m_u_toString),
};
/* ---- WHATWG Encoding (TextEncoder/TextDecoder) ----------------- */
/* TextEncoder is utf-8-only per spec: a JS string -> UTF-8 is the
   engine's own exact conversion (JS_ToCStringLen) — the most
   fundamental "don't reinvent". TextDecoder routes through Lexbor's
   real WHATWG codec set (already compiled in), one path for every
   label incl. utf-8, so malformed input gets spec U+FFFD replacement
   without relying on engine string-decode behaviour. No hand-rolled
   UTF-8. */
static const uint8_t *td_in(JSContext *ctx, JSValueConst v, size_t *n,
                            JSValue *hold) {
    *hold = JS_UNDEFINED; *n = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) return (const uint8_t *)"";
    uint8_t *p = JS_GetUint8Array(ctx, n, v);
    if (p) return p;
    JS_GetException(ctx);                       /* clear "not a Uint8Array" */
    if (JS_IsArrayBuffer(v)) return JS_GetArrayBuffer(ctx, n, v);
    JS_GetException(ctx);
    size_t off, len, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (!JS_IsException(ab)) {
        size_t abn; uint8_t *base = JS_GetArrayBuffer(ctx, &abn, ab);
        *hold = ab;
        if (base) { *n = len; return base + off; }
        return (const uint8_t *)"";
    }
    JS_GetException(ctx);
    return (const uint8_t *)"";
}
static JSValue te_encode(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    size_t n = 0;
    const char *s = (ac > 0 && !JS_IsUndefined(av[0]))
                     ? JS_ToCStringLen(ctx, &n, av[0]) : NULL;
    JSValue r = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(s ? s : ""), s ? n : 0);
    if (s) JS_FreeCString(ctx, s);
    return r;
}
static JSValue te_encodeInto(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    size_t n = 0;
    const char *s = (ac > 0) ? JS_ToCStringLen(ctx, &n, av[0]) : NULL;
    size_t dn = 0; uint8_t *d = (ac > 1) ? JS_GetUint8Array(ctx, &dn, av[1]) : NULL;
    int64_t srclen = 0;
    if (ac > 0) { JSValue lp = JS_GetPropertyStr(ctx, av[0], "length");
                  JS_ToInt64(ctx, &srclen, lp); JS_FreeValue(ctx, lp); }
    size_t w = 0;             /* bytes written */
    int64_t rd = 0;           /* UTF-16 code units consumed */
    if (s && d) {
        if (n <= dn) { w = n; memcpy(d, s, w); rd = srclen; }
        else {
            /* trim to a UTF-8 sequence boundary so no codepoint splits */
            w = dn; while (w > 0 && ((unsigned char)s[w] & 0xC0) == 0x80) w--;
            memcpy(d, s, w);
            /* read = UTF-16 units in the written prefix (astral = 2) */
            for (size_t i = 0; i < w; ) {
                unsigned char c = s[i];
                int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                rd += (len == 4) ? 2 : 1; i += len;
            }
        }
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "read", JS_NewInt64(ctx, rd));
    JS_SetPropertyStr(ctx, o, "written", JS_NewInt64(ctx, (int64_t)w));
    if (s) JS_FreeCString(ctx, s);
    return o;
}
static JSValue td_decode(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    JSValue hold; size_t inlen;
    const uint8_t *in = td_in(ctx, ac > 0 ? av[0] : JS_UNDEFINED, &inlen, &hold);
    JSValue encv = JS_GetPropertyStr(ctx, t, "encoding");
    size_t lbln; const char *lbl = JS_ToCStringLen(ctx, &lbln, encv);
    const lxb_encoding_data_t *d = lbl
        ? lxb_encoding_data_by_name((const lxb_char_t *)lbl, lbln) : NULL;
    if (!d) d = lxb_encoding_data_by_name((const lxb_char_t *)"utf-8", 5);
    if (lbl) JS_FreeCString(ctx, lbl);
    JS_FreeValue(ctx, encv);
    /* WHATWG error mode: default (fatal:false) replaces every malformed
       sequence with U+FFFD and CONTINUES; fatal:true aborts with a
       TypeError. Lexbor implements both via decode.replace_to: a non-NULL
       replacement codepoint makes d->decode emit it and keep going, NULL
       makes it stop and return error. The previous code set neither, so it
       stopped at the first bad byte and silently dropped the rest (observed:
       decode([0xFF,0xFE]) → "" instead of "��"). */
    JSValue fatalv = JS_GetPropertyStr(ctx, t, "fatal");
    int fatal = JS_ToBool(ctx, fatalv);
    JS_FreeValue(ctx, fatalv);
    /* #codepoints <= #input bytes for every WHATWG encoding (each cp
       consumes >=1 byte; a U+FFFD replacement is also one cp per bad byte) —
       one-shot, no grow loop. */
    lxb_codepoint_t *cps = malloc((inlen + 1) * sizeof(lxb_codepoint_t));
    lxb_char_t *ob = NULL; JSValue r;
    if (!cps) { r = JS_NewStringLen(ctx, "", 0); goto done; }
    {
        const lxb_codepoint_t repl = LXB_ENCODING_REPLACEMENT_CODEPOINT;
        lxb_encoding_decode_t dec;
        lxb_encoding_decode_init(&dec, d, cps, inlen + 1);
        if (!fatal) { dec.replace_to = &repl; dec.replace_len = 1; }
        const lxb_char_t *p = (const lxb_char_t *)in, *end = p + inlen;
        lxb_status_t dst = d->decode(&dec, &p, end);
        if (lxb_encoding_decode_finish(&dec) != LXB_STATUS_OK && dst == LXB_STATUS_OK)
            dst = LXB_STATUS_ERROR;   /* trailing incomplete sequence */
        if (fatal && dst != LXB_STATUS_OK) {
            r = JS_ThrowTypeError(ctx, "TextDecoder: malformed input in fatal mode");
            goto done;
        }
        size_t ncp = lxb_encoding_decode_buf_used(&dec);
        const lxb_encoding_data_t *u8 = lxb_encoding_data_by_name((const lxb_char_t *)"utf-8", 5);
        ob = malloc(ncp * 4 + 1);
        if (!ob) { r = JS_NewStringLen(ctx, "", 0); goto done; }
        lxb_encoding_encode_t enc;
        lxb_encoding_encode_init(&enc, u8, ob, ncp * 4 + 1);
        const lxb_codepoint_t *cp = cps, *cpe = cps + ncp;
        u8->encode(&enc, &cp, cpe);
        r = JS_NewStringLen(ctx, (const char *)ob, lxb_encoding_encode_buf_used(&enc));
    }
done:
    free(cps); free(ob);
    JS_FreeValue(ctx, hold);
    return r;
}
static JSValue te_ctor(JSContext *ctx, JSValueConst nt, int ac, JSValueConst *av) {
    JSValue pr = JS_GetPropertyStr(ctx, nt, "prototype");
    JSValue o = JS_NewObjectProto(ctx, JS_IsObject(pr) ? pr : JS_NULL);
    JS_FreeValue(ctx, pr);
    JS_SetPropertyStr(ctx, o, "encoding", JS_NewString(ctx, "utf-8"));
    return o;
}
static JSValue td_ctor(JSContext *ctx, JSValueConst nt, int ac, JSValueConst *av) {
    const lxb_encoding_data_t *d;
    if (ac > 0 && !JS_IsUndefined(av[0])) {
        size_t ln; const char *l = JS_ToCStringLen(ctx, &ln, av[0]);
        d = l ? lxb_encoding_data_by_name((const lxb_char_t *)l, ln) : NULL;
        if (l) JS_FreeCString(ctx, l);
        if (!d) return JS_ThrowRangeError(ctx, "TextDecoder: unsupported encoding label");
    } else {
        d = lxb_encoding_data_by_name((const lxb_char_t *)"utf-8", 5);
    }
    JSValue pr = JS_GetPropertyStr(ctx, nt, "prototype");
    JSValue o = JS_NewObjectProto(ctx, JS_IsObject(pr) ? pr : JS_NULL);
    JS_FreeValue(ctx, pr);
    JS_SetPropertyStr(ctx, o, "encoding", JS_NewString(ctx, (const char *)d->name));
    int fatal = 0, ibom = 0;
    if (ac > 1 && JS_IsObject(av[1])) {
        JSValue f = JS_GetPropertyStr(ctx, av[1], "fatal"); fatal = JS_ToBool(ctx, f); JS_FreeValue(ctx, f);
        JSValue b = JS_GetPropertyStr(ctx, av[1], "ignoreBOM"); ibom = JS_ToBool(ctx, b); JS_FreeValue(ctx, b);
    }
    JS_SetPropertyStr(ctx, o, "fatal", JS_NewBool(ctx, fatal));
    JS_SetPropertyStr(ctx, o, "ignoreBOM", JS_NewBool(ctx, ibom));
    return o;
}
/* ---- WHATWG Crypto subtle.digest (host-bridge to real WebCrypto) ----
   Bundles that hash a request body to compute a signature header (MS
   Graph signature, AWS SigV4 canonical hash, GitHub webhook
   X-Hub-Signature) need a REAL digest, not a stub — the previous JS
   fallback returned `Promise.resolve(new ArrayBuffer(0))` so the
   learned `requiredHeaders` carried a real-looking base64 of the
   digest-of-empty masquerading as the actual payload hash. The fix
   per CLAUDE.md bind-before-build #1 ("engine intrinsic") is to use
   the HOST RUNTIME's WebCrypto: in the wasm worker that's Chromium's
   real BoringSSL via `self.crypto.subtle.digest(...)`, reached through
   a JSPI import that suspends the wasm stack until the host Promise
   resolves. Native qjs.exe has no Web Crypto and is for polarity/
   forced-execution tests that don't exercise digest — bundles that
   need real crypto MUST run via the Chrome harness; the native path
   throws a clear error. WHATWG `digest()` is async so the binding
   resolves a Promise inline with the computed buffer; `await digest`
   resumes naturally inside the bundle's existing async flow. */
static int crypto_pull_bytes(JSContext *ctx, JSValueConst v, const uint8_t **out, size_t *outlen, JSValue *hold) {
    *hold = JS_UNDEFINED; *out = NULL; *outlen = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) return 0;
    uint8_t *p = JS_GetUint8Array(ctx, outlen, v);
    if (p) { *out = p; return 0; }
    JS_GetException(ctx);
    if (JS_IsArrayBuffer(v)) {
        *out = JS_GetArrayBuffer(ctx, outlen, v);
        return *out ? 0 : -1;
    }
    JS_GetException(ctx);
    size_t off, len, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (!JS_IsException(ab)) {
        size_t abn; uint8_t *base = JS_GetArrayBuffer(ctx, &abn, ab);
        *hold = ab;
        if (base) { *out = base + off; *outlen = len; return 0; }
        return -1;
    }
    JS_GetException(ctx);
    return -1;
}

/* Normalize a WHATWG algorithm identifier to one of {SHA-1,SHA-256,SHA-384,
   SHA-512}; case-insensitive. The argument is either a string OR an object
   like { name: "SHA-256" }. Returns the digest byte length, or 0 on
   unsupported. Writes the canonical name into a small buffer for the
   resulting NotSupportedError when applicable. */
static int crypto_algo_size(JSContext *ctx, JSValueConst alg, char *out_name, size_t out_name_cap) {
    JSValue name_v;
    if (JS_IsString(alg)) {
        name_v = JS_DupValue(ctx, alg);
    } else if (JS_IsObject(alg)) {
        name_v = JS_GetPropertyStr(ctx, alg, "name");
        if (JS_IsException(name_v)) { JS_GetException(ctx); return 0; }
    } else {
        return 0;
    }
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, name_v);
    JS_FreeValue(ctx, name_v);
    if (!s) return 0;
    /* uppercase compare so "sha-256", "SHA-256", "Sha-256" all match.
       Algorithm identifier is a fixed enumerated set (WHATWG Subtle
       Crypto §18), not a name-pattern match on bundle code. */
    char up[16]; size_t i;
    for (i = 0; i < n && i + 1 < sizeof up; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        up[i] = c;
    }
    up[i] = 0;
    if (out_name && out_name_cap) { snprintf(out_name, out_name_cap, "%s", up); }
    JS_FreeCString(ctx, s);
    /* WHATWG SubtleCrypto §18 digest output lengths (NIST FIPS 180-4). */
    if (!strcmp(up, "SHA-1"))   return 20;
    if (!strcmp(up, "SHA-256")) return 32;
    if (!strcmp(up, "SHA-384")) return 48;
    if (!strcmp(up, "SHA-512")) return 64;
    return 0;
}

#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
extern int qjs_host_digest(const char *algName, const uint8_t *data, int dataLen, uint8_t *out);
#endif

static JSValue crypto_digest_sync(JSContext *ctx, const char *up, const uint8_t *data, size_t len) {
#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
    /* Wasm worker → bridge to the host's real WebCrypto (Chromium's
       BoringSSL when the extension is loaded in Chrome). The wasm stack
       suspends via JSPI while self.crypto.subtle.digest() runs, then
       resumes with the digest bytes written to `out`. */
    uint8_t out[64];   /* fits SHA-512 (the largest WHATWG digest) */
    int outlen = qjs_host_digest(up, data, (int)len, out);
    if (outlen < 0) {
        /* Host WebCrypto rejected (algorithm absent in this UA, etc.).
           The bridge emitted a @WHY host_digest_throw on stderr. Propagate
           as a JS TypeError — bundle's await rejects with a visible error,
           never a fabricated value. */
        return JS_ThrowTypeError(ctx, "crypto.subtle.digest: host WebCrypto rejected (alg=%s, len=%zu) — see @WHY host_digest_throw", up, len);
    }
    return JS_NewArrayBufferCopy(ctx, out, (size_t)outlen);
#else
    /* Native qjs.exe has no Web Crypto host. The native build is the
       forced-execution/polarity test driver; bundles that exercise
       crypto.subtle.digest must run through the Chrome extension
       harness (the wasm worker has the host bridge above). Throw a
       clear error rather than vendoring an unaudited crypto lib whose
       digest could ship the wrong value for a body-hash header. */
    (void)data; (void)len;
    return JS_ThrowTypeError(ctx, "crypto.subtle.digest('%s', …): not available in the native build — run the bundle through the Chrome extension harness (the wasm worker bridges to real Web Crypto)", up);
#endif
}

static JSValue crypto_subtle_digest(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    if (ac < 2) {
        JSValue e = JS_ThrowTypeError(ctx, "crypto.subtle.digest requires (algorithm, data)");
        JSValue eo = JS_GetException(ctx);
        JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &eo);
        JS_FreeValue(ctx, eo); JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    char up[16] = {0};
    if (!crypto_algo_size(ctx, av[0], up, sizeof up)) {
        JSValue e = JS_ThrowTypeError(ctx, "crypto.subtle.digest: unsupported algorithm");
        JSValue eo = JS_GetException(ctx);
        JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &eo);
        JS_FreeValue(ctx, eo); JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    const uint8_t *data = NULL; size_t dlen = 0; JSValue hold;
    if (crypto_pull_bytes(ctx, av[1], &data, &dlen, &hold) < 0) {
        JSValue e = JS_ThrowTypeError(ctx, "crypto.subtle.digest: data is not a BufferSource");
        JSValue eo = JS_GetException(ctx);
        JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &eo);
        JS_FreeValue(ctx, eo); JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    JSValue ab = crypto_digest_sync(ctx, up, data ? data : (const uint8_t *)"", dlen);
    JS_FreeValue(ctx, hold);
    if (JS_IsException(ab)) {
        JSValue eo = JS_GetException(ctx);
        JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &eo);
        JS_FreeValue(ctx, eo);
    } else {
        JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &ab);
        JS_FreeValue(ctx, ab);
    }
    JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static int crypto_install(JSContext *ctx, JSValue glob) {
    /* Install a `crypto.subtle` whose digest is BearSSL-backed BEFORE
       hostedge.js evaluates; hostedge's fallback `G.crypto.subtle =
       G.crypto.subtle || { stub }` keeps the real one. The other subtle
       methods (encrypt/sign/etc.) stay stubbed via hostedge for now — the
       binding pattern here generalises when those gaps are closed too. */
    JSValue cur = JS_GetPropertyStr(ctx, glob, "crypto");
    JSValue cryp = JS_IsUndefined(cur) || JS_IsNull(cur) ? JS_NewObject(ctx) : cur;
    JSValue subtle = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, subtle, "digest",
        JS_NewCFunction(ctx, crypto_subtle_digest, "digest", 2));
    JS_SetPropertyStr(ctx, cryp, "subtle", subtle);
    JS_SetPropertyStr(ctx, glob, "crypto", JS_DupValue(ctx, cryp));
    if (JS_IsUndefined(cur) || JS_IsNull(cur)) {
        /* JS_DupValue above bumped the refcount on cryp; the original
           binding holds the only other ref. Free the local. */
    }
    JS_FreeValue(ctx, cryp);
    return 0;
}

#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
extern int qjs_load_script_begin(const char *url);   /* suspends; returns handle id or -1 */
extern int qjs_load_script_len(int h);
extern void qjs_load_script_take(int h, uint8_t *out, int cap);  /* copies + frees the handle */
#endif

/* __feLoadScript(url) — the realm-facing half of the in-run external-script
   loader (the one-message-per-document keystone). hostedge.js calls this when
   forced exec reaches an external <script src> / a programmatically-discovered
   src; the wasm stack suspends via JSPI (qjs_load_script_begin) while the worker
   safeFetches the page's own subresource, then resumes with the code returned
   here as a JS string — so hostedge can _eval it IN THE SAME REALM, in document
   order. Returns null on the native build (no JSPI host) or on a fetch failure /
   blocked subresource, so hostedge falls back to its @SCRIPTSRC emit (no
   regression). The engine stays sandboxed: this binding cannot fetch — only the
   worker's safeFetch (the single chokepoint) can, under the document principal. */
static JSValue js_fe_load_script(JSContext *ctx, JSValueConst this_val, int ac, JSValueConst *av) {
    (void)this_val;
#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
    if (ac < 1) return JS_NULL;
    const char *url = JS_ToCString(ctx, av[0]);
    if (!url) return JS_NULL;
    int h = qjs_load_script_begin(url);   /* JSPI suspend → worker safeFetch */
    JS_FreeCString(ctx, url);
    if (h < 0) return JS_NULL;
    int n = qjs_load_script_len(h);
    if (n < 0) return JS_NULL;
    uint8_t *buf = js_malloc(ctx, (size_t)n + 1);
    if (!buf) { qjs_load_script_take(h, NULL, 0); return JS_NULL; }  /* still free the handle */
    qjs_load_script_take(h, buf, n);
    buf[n] = 0;
    JSValue s = JS_NewStringLen(ctx, (const char *)buf, (size_t)n);
    js_free(ctx, buf);
    return s;
#else
    (void)ctx; (void)ac; (void)av;
    return JS_NULL;
#endif
}

static int fe_loadscript_install(JSContext *ctx, JSValue glob) {
    JS_SetPropertyStr(ctx, glob, "__feLoadScript",
        JS_NewCFunction(ctx, js_fe_load_script, "__feLoadScript", 1));
    return 0;
}

#if defined(__EMSCRIPTEN__) && defined(QJS_HAS_JSPI)
extern JSValue qjs_eval_script(JSContext *ctx, const char *src, size_t n,
                               const char *filename, int start_line);   /* qjsmain.c — the DRIVEN bundle eval */

static int qjs_is_js_script_type(const lxb_char_t *ty, size_t tl) {
    if (!ty || tl == 0) return 1;                 /* no type = classic JS */
    /* No type attribute (tl==0) or empty type = a CLASSIC JS script per the HTML
       spec — the most common inline shape. Skipping these (the old behaviour: an
       empty buf matches none of the literals below) dropped page-config SEEDERS like
       gitlab.com's `<script nonce="">window.gon={};gon.sentry_dsn="...@new-sentry.
       gitlab.net/4";gon.api_version="v4"</script>`, so `gon` stayed OPAQUE and the
       gon-gated Sentry init + Apollo client built OPAQUE URLs (the envelope and
       /api/graphql never resolved). Run them. (type="module" and non-JS types like
       application/json / importmap are still classified by the literals below.) */
    if (tl == 0) return 1;
    char buf[40]; if (tl >= sizeof buf) return 0;
    for (size_t i = 0; i < tl; i++) buf[i] = (char)((ty[i] >= 'A' && ty[i] <= 'Z') ? ty[i] + 32 : ty[i]);
    buf[tl] = 0;
    return (!strcmp(buf, "text/javascript") || !strcmp(buf, "application/javascript") ||
            !strcmp(buf, "text/ecmascript") || !strcmp(buf, "application/ecmascript") ||
            !strcmp(buf, "module"));   /* type=module: qjs_eval_script compiles it as an ES module */
}

/* HTML-relative line offset for an INLINE script: the engine evals each script at
   line 1, but a finding/@H from an inline <script> should carry the line in the
   SERVED HTML (popup click-through), not the script-local line. Lexbor keeps no
   element source position, so locate the script's verbatim text in the stashed raw
   page HTML (globalThis.__pageHtml, set by /pre.js) and return the count of '\n'
   before it. qjs_run_doc_scripts prepends that many newlines so eval lines align
   with the HTML. 0 if __pageHtml/the text isn't found (no shift — safe fallback);
   external scripts are their own file and need no shift. */
static int qjs_inline_html_line_offset(JSContext *ctx, const char *code, size_t cl) {
    if (!code || cl == 0) return 0;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue hv = JS_GetPropertyStr(ctx, g, "__pageHtml");
    JS_FreeValue(ctx, g);
    int off = 0;
    size_t hl = 0;
    const char *html = JS_ToCStringLen(ctx, &hl, hv);
    if (html && cl <= hl) {
        const char *found = NULL;
        for (size_t i = 0; i + cl <= hl; i++) {
            if (html[i] == code[0] && !memcmp(html + i, code, cl)) { found = html + i; break; }
        }
        if (found) for (const char *p = html; p < found; p++) if (*p == '\n') off++;
    }
    if (html) JS_FreeCString(ctx, html);
    JS_FreeValue(ctx, hv);
    return off;
}

/* The actual work of loading a dynamically-injected <script src> — runs as a JOB
   (enqueued by qjs_load_dynamic_script), so qjs_eval_script is NOT re-entrant with
   the eval that did the appendChild (re-entrant eval breaks forced-exec function
   registration — the old SSR-phase bug; see qjs_run_doc_scripts). Fetch via the same
   JSPI safeFetch bridge as the static path, eval (so a webpack/rollup chunk's
   self.webpackChunk.push runs → its modules register → the gated entry startup runs),
   then fire the load event for onload-gated chunk-promise resolution. */
static JSValue qjs_dyn_script_job(JSContext *ctx, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    JSValueConst scriptVal = argv[0];
    lxb_dom_node_t *n = self_node(scriptVal);
    if (!n || n->local_name != LXB_TAG_SCRIPT) return JS_UNDEFINED;
    lxb_dom_element_t *e = el_of(n);
    if (!e) return JS_UNDEFINED;
    size_t sl = 0;
    const lxb_char_t *src = lxb_dom_element_get_attribute(e, (const lxb_char_t *)"src", 3, &sl);
    if (!src || !sl) return JS_UNDEFINED;
    char *url = js_malloc(ctx, sl + 1);
    if (!url) return JS_UNDEFINED;
    memcpy(url, src, sl); url[sl] = 0;
    int h = qjs_load_script_begin(url);   /* JSPI suspend → worker safeFetch (one-per-URL cached) */
    if (h >= 0) {
        int cn = qjs_load_script_len(h);
        if (cn >= 0) {
            char *code = js_malloc(ctx, (size_t)cn + 1);
            if (code) {
                qjs_load_script_take(h, (uint8_t *)code, cn); code[cn] = 0;
                JSValue r = qjs_eval_script(ctx, code, (size_t)cn, url, 1);
                if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); }
                JS_FreeValue(ctx, r);
                js_free(ctx, code);
            }
        } else { qjs_load_script_take(h, NULL, 0); }
    }
    js_free(ctx, url);
    /* Fire load: an onload-gated startup (jsonp resolving its chunk promise on the
       script's load event) then runs. Both the .onload property and dispatchEvent. */
    JSValue onload = JS_GetPropertyStr(ctx, scriptVal, "onload");
    if (JS_IsFunction(ctx, onload)) { JSValue r = JS_Call(ctx, onload, scriptVal, 0, NULL); if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); } JS_FreeValue(ctx, r); }
    JS_FreeValue(ctx, onload);
    JSValue de = JS_GetPropertyStr(ctx, scriptVal, "dispatchEvent");
    if (JS_IsFunction(ctx, de)) { JSValue ev = JS_NewObject(ctx); JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "load")); JSValueConst a1[1] = { ev }; JSValue r = JS_Call(ctx, de, scriptVal, 1, a1); if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); } JS_FreeValue(ctx, r); JS_FreeValue(ctx, ev); }
    JS_FreeValue(ctx, de);
    return JS_UNDEFINED;
}
/* Hooked from m_appendChild/m_insertBefore when a <script src> connects to the
   document — the webpack/rollup JSONP lazy-CHUNK mechanism. Static <script src> in the
   parsed HTML run via qjs_run_doc_scripts; a script element created + connected at
   RUNTIME was NOT loaded, so its chunk never evaluated, its modules never registered,
   the gated entry startup never ran, and the lazy chunk's endpoints (route/feature
   code — the moat's lazy-chunk surface) were never learned (testing/fixtures/
   webpack_jsonp.html learned 0 until this). Per-element dedup; webpack's installedChunks
   guard handles same-URL re-pushes. Deferred via a job (qjs_dyn_script_job). */
static void qjs_load_dynamic_script(JSContext *ctx, JSValueConst scriptVal) {
    lxb_dom_node_t *n = self_node(scriptVal);
    if (!n || n->local_name != LXB_TAG_SCRIPT) return;
    lxb_dom_element_t *e = el_of(n);
    if (!e) return;
    size_t sl = 0;
    const lxb_char_t *src = lxb_dom_element_get_attribute(e, (const lxb_char_t *)"src", 3, &sl);
    if (!src || !sl) return;   /* only src scripts need a fetch; inline run via their text content */
    JSValue _ld = JS_GetPropertyStr(ctx, scriptVal, "__feDynLoaded");
    int already = JS_ToBool(ctx, _ld);
    JS_FreeValue(ctx, _ld);
    if (already) return;
    JS_SetPropertyStr(ctx, scriptVal, "__feDynLoaded", JS_NewBool(ctx, 1));
    JSValueConst a[1] = { scriptVal };
    JS_EnqueueJob(ctx, qjs_dyn_script_job, 1, a);
}
/* qjs_run_doc_scripts: run the parsed document's scripts as QuickJS DRIVEN bundle
   code — a browser runs the page's scripts on its JS engine; here that's QuickJS's
   job (forced multi-path), NOT Lexbor's and NOT a hostedge SSR _eval. Called from
   qjsmain's TOP-LEVEL boot loop right after the DOM is parsed — crucially NOT
   re-entrantly from inside a JS eval (running qjs_eval_script while /h.js is still
   on the stack breaks the forced-exec function registration; that was the SSR
   phase's bug). Inline bodies + external <script src> (fetched through the
   __feLoadScript safeFetch bridge) run in document order via qjs_eval_script,
   identically to a /b.N.js bundle slice — so the BFS + value-spread + deep grind
   drive them and their instance-bound methods. Lexbor only parses; QuickJS runs. */
void qjs_run_doc_scripts(JSContext *ctx) {
    if (!g_doc) return;
    lxb_dom_node_t *root = lxb_dom_interface_node(g_doc);
    if (!root) return;
    JSValue arr = do_query(ctx, root, "script", 6, 0);   /* document order */
    if (!JS_IsObject(arr)) { JS_FreeValue(ctx, arr); return; }
    uint32_t len = 0;
    { JSValue lv = JS_GetPropertyStr(ctx, arr, "length"); JS_ToUint32(ctx, &len, lv); JS_FreeValue(ctx, lv); }
    for (uint32_t i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, i);
        lxb_dom_node_t *n = self_node(el);
        lxb_dom_element_t *e = n ? el_of(n) : NULL;
        if (!e) { JS_FreeValue(ctx, el); continue; }
        size_t tl = 0;
        const lxb_char_t *ty = lxb_dom_element_get_attribute(e, (const lxb_char_t *)"type", 4, &tl);
        if (!qjs_is_js_script_type(ty, tl)) { JS_FreeValue(ctx, el); continue; }   /* JSON/importmap = data; module + classic run */
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(e, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) {
            char *url = js_malloc(ctx, sl + 1);
            if (url) {
                memcpy(url, src, sl); url[sl] = 0;
                int h = qjs_load_script_begin(url);   /* JSPI suspend → worker safeFetch */
                if (h >= 0) {
                    int cn = qjs_load_script_len(h);
                    if (cn >= 0) {
                        char *code = js_malloc(ctx, (size_t)cn + 1);
                        if (code) {
                            qjs_load_script_take(h, (uint8_t *)code, cn); code[cn] = 0;
                            JSValue r = qjs_eval_script(ctx, code, (size_t)cn, url, 1);
                            if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); }
                            JS_FreeValue(ctx, r);
                            js_free(ctx, code);
                        }
                    } else { qjs_load_script_take(h, NULL, 0); }
                }
                js_free(ctx, url);
            }
        } else {
            size_t cl = 0;
            lxb_char_t *code = lxb_dom_node_text_content(n, &cl);
            if (code && cl) {
                /* Prepend (lines-before-this-script-in-the-HTML) newlines so @S/@H
                   line numbers from an inline <script> are HTML-relative (the line
                   in the served page), matching the popup's click-through. */
                int pre = qjs_inline_html_line_offset(ctx, (const char *)code, cl);
                const char *evcode = (const char *)code; size_t evlen = cl; char *shifted = NULL;
                if (pre > 0 && (shifted = js_malloc(ctx, (size_t)pre + cl + 1))) {
                    memset(shifted, '\n', (size_t)pre);
                    memcpy(shifted + pre, code, cl);
                    shifted[pre + cl] = 0;
                    evcode = shifted; evlen = (size_t)pre + cl;
                }
                JSValue r = qjs_eval_script(ctx, evcode, evlen, "/b.inline.js", 1);
                if (JS_IsException(r)) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); }
                JS_FreeValue(ctx, r);
                if (shifted) js_free(ctx, shifted);
            }
        }
        JS_FreeValue(ctx, el);
    }
    JS_FreeValue(ctx, arr);
}
#else
void qjs_run_doc_scripts(JSContext *ctx) { (void)ctx; }
#endif

static int txt_install(JSContext *ctx, JSValue glob) {
    JSValue tep = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tep, "encode", JS_NewCFunction(ctx, te_encode, "encode", 1));
    JS_SetPropertyStr(ctx, tep, "encodeInto", JS_NewCFunction(ctx, te_encodeInto, "encodeInto", 2));
    JSValue tec = JS_NewCFunction2(ctx, (JSCFunction *)te_ctor, "TextEncoder", 0, JS_CFUNC_constructor, 0);
    JS_DefinePropertyValueStr(ctx, tec, "prototype", tep, 0);
    JS_SetPropertyStr(ctx, glob, "TextEncoder", tec);
    JSValue tdp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tdp, "decode", JS_NewCFunction(ctx, td_decode, "decode", 1));
    JSValue tdc = JS_NewCFunction2(ctx, (JSCFunction *)td_ctor, "TextDecoder", 0, JS_CFUNC_constructor, 0);
    JS_DefinePropertyValueStr(ctx, tdc, "prototype", tdp, 0);
    JS_SetPropertyStr(ctx, glob, "TextDecoder", tdc);
    return 0;
}

static int url_install(JSContext *ctx, JSValue glob) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &url_cid);
    JSClassDef ucd = { "URL", .finalizer = url_finalizer };
    if (JS_NewClass(rt, url_cid, &ucd) < 0) return -1;
    g_urlp = lxb_url_parser_create();
    if (lxb_url_parser_init(g_urlp, NULL) != LXB_STATUS_OK) return -1;
    JSValue uproto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, uproto, url_proto,
        sizeof(url_proto) / sizeof(url_proto[0]));
    /* real method toString (not just the getter alias) so
       `String(u)` / `""+u` / template literals serialize. */
    JS_SetPropertyStr(ctx, uproto, "toString",
        JS_NewCFunction(ctx, m_u_toString, "toString", 0));
    JS_SetClassProto(ctx, url_cid, JS_DupValue(ctx, uproto));
    JSValue uctor = JS_NewCFunction2(ctx, (JSCFunction *)url_ctor, "URL", 1,
        JS_CFUNC_constructor, 0);
    JS_DefinePropertyValueStr(ctx, uctor, "prototype", uproto, 0);
    JS_SetPropertyStr(ctx, uctor, "canParse",
        JS_NewCFunction(ctx, url_canParse, "canParse", 2));
    JS_SetPropertyStr(ctx, uctor, "createObjectURL",
        JS_NewCFunction(ctx, url_createObjectURL, "createObjectURL", 1));
    JS_SetPropertyStr(ctx, uctor, "revokeObjectURL",
        JS_NewCFunction(ctx, (JSCFunction *)m_noop, "revokeObjectURL", 1));
    JS_SetPropertyStr(ctx, glob, "URL", uctor);
    JS_SetPropertyStr(ctx, glob, "webkitURL", JS_DupValue(ctx, uctor));
    return 0;
}

/* Installed by qjsmain before scripts run. Returns 0 on success. */
void qjs_host_atoms_init(JSContext *ctx);
int qjs_dom_install(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* Install the op/memory watchdog handler + initialize the host-edge
       atom set before any eval runs. qjs_host_atoms_init does both. The
       atoms must exist for forced-execution opcode handlers (OP_call /
       OP_get_field) to recognise host-edge atoms, and the watchdog
       handler installation matches the same runtime-wide setup point. */
    qjs_host_atoms_init(ctx);
    JS_NewClassID(rt, &dom_cid);
    JSClassDef cd = { "DOMNode", .finalizer = dom_finalizer };
    if (JS_NewClass(rt, dom_cid, &cd) < 0) return -1;

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, dom_proto,
        sizeof(dom_proto) / sizeof(dom_proto[0]));
    JS_SetClassProto(ctx, dom_cid, proto);

    /* qjs_dom_install runs once per main() invocation. The wasm
       worker calls main() repeatedly (one per forced schedule), so
       the previous run's Lexbor structures must be freed before
       allocating fresh ones — otherwise each schedule leaks ~5-10 MB
       (g_doc + tree + sel/css parsers) and the offscreen page's
       Chrome-imposed heap quota exhausts after a few dozen schedules
       on a real-website bundle, crashing the SW. */
    if (g_doc)  { lxb_html_document_destroy(g_doc);     g_doc  = NULL; }
    if (g_sel)  { lxb_selectors_destroy(g_sel, true);   g_sel  = NULL; }
    if (g_cssp) { lxb_css_parser_destroy(g_cssp, true); g_cssp = NULL; }
    g_doc = lxb_html_document_create();
    if (g_doc == NULL) return -1;
    static const lxb_char_t seed[] = "<!DOCTYPE html><html><head></head><body></body></html>";
    if (lxb_html_document_parse(g_doc, seed, sizeof(seed) - 1) != LXB_STATUS_OK) return -1;
    g_cssp = lxb_css_parser_create();
    if (lxb_css_parser_init(g_cssp, NULL) != LXB_STATUS_OK) return -1;
    g_sel = lxb_selectors_create();
    if (lxb_selectors_init(g_sel) != LXB_STATUS_OK) return -1;

    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue jdoc = dom_wrap(ctx, lxb_dom_interface_node(g_doc));
    lxb_dom_node_t *body = lxb_dom_interface_node(lxb_html_document_body_element(g_doc));
    lxb_dom_node_t *head = body ? body->parent->first_child : NULL; /* <head> */
    lxb_dom_node_t *htmlEl = body ? body->parent : NULL;
    JS_SetPropertyStr(ctx, jdoc, "body", dom_wrap(ctx, body));
    JS_SetPropertyStr(ctx, jdoc, "head", dom_wrap(ctx, head));
    JS_SetPropertyStr(ctx, jdoc, "documentElement", dom_wrap(ctx, htmlEl));
    /* __feLoadPage: re-parse the raw server-rendered HTML into the
       existing Lexbor document, then refresh the cached body / head /
       documentElement bindings. Hostedge.js calls this at boot when
       content.js shipped CONTENT_HTML — without it, the analyser
       runs against the minimal seed document and bundles that gate
       fetches behind connectedCallback / querySelector on SSR
       elements never reach their host-edge calls. */
    JS_SetPropertyStr(ctx, jdoc, "__feLoadPage",
        JS_NewCFunction(ctx, (JSCFunction *)m_feLoadPage, "__feLoadPage", 1));
    JS_SetPropertyStr(ctx, glob, "document", jdoc);

    /* Expose constructors whose .prototype IS the node class proto, so
       `x instanceof HTMLElement` / `class C extends HTMLElement` and
       the JS prelude's `Object.getPrototypeOf(document)` all resolve to
       the one Lexbor-backed prototype. */
    JSValue ctor = JS_NewCFunction2(ctx, (JSCFunction *)dom_ctor, "Node", 0,
        JS_CFUNC_constructor, 0);
    JS_DefinePropertyValueStr(ctx, ctor, "prototype",
        JS_DupValue(ctx, proto), 0);
    /* The complete WHATWG HTML element-interface set + the DOM-core
       node hierarchy + the legacy factory constructors (Image/Audio/
       Option) + SVG core. Spec-enumerated and finite — exposing the
       full table in one shot, not one name per re-test cycle (a real
       bundle does `class X extends HTMLButtonElement`, `el instanceof
       HTMLFormElement`, `new Image()` for any tag). All share the one
       Lexbor node proto + dom_ctor: our model is structural, not
       per-interface, and the live node + el_of() guards give correct
       runtime behaviour regardless of which interface name was used. */
    const char *cn[] = {
        "EventTarget", "Node", "Element", "Document", "HTMLDocument",
        "XMLDocument", "DocumentFragment", "ShadowRoot", "CharacterData",
        "Text", "Comment", "CDATASection", "ProcessingInstruction",
        "DocumentType", "Attr",
        "HTMLElement", "HTMLUnknownElement", "HTMLAnchorElement",
        "HTMLAreaElement", "HTMLAudioElement", "HTMLBaseElement",
        "HTMLBodyElement", "HTMLBRElement", "HTMLButtonElement",
        "HTMLCanvasElement", "HTMLDataElement", "HTMLDataListElement",
        "HTMLDetailsElement", "HTMLDialogElement", "HTMLDirectoryElement",
        "HTMLDivElement", "HTMLDListElement", "HTMLEmbedElement",
        "HTMLFieldSetElement", "HTMLFontElement", "HTMLFormElement",
        "HTMLFrameElement", "HTMLFrameSetElement", "HTMLHeadElement",
        "HTMLHeadingElement", "HTMLHRElement", "HTMLHtmlElement",
        "HTMLIFrameElement", "HTMLImageElement", "HTMLInputElement",
        "HTMLLabelElement", "HTMLLegendElement", "HTMLLIElement",
        "HTMLLinkElement", "HTMLMapElement", "HTMLMarqueeElement",
        "HTMLMediaElement", "HTMLMenuElement", "HTMLMetaElement",
        "HTMLMeterElement", "HTMLModElement", "HTMLObjectElement",
        "HTMLOListElement", "HTMLOptGroupElement", "HTMLOptionElement",
        "HTMLOutputElement", "HTMLParagraphElement", "HTMLParamElement",
        "HTMLPictureElement", "HTMLPreElement", "HTMLProgressElement",
        "HTMLQuoteElement", "HTMLScriptElement", "HTMLSelectElement",
        "HTMLSlotElement", "HTMLSourceElement", "HTMLSpanElement",
        "HTMLStyleElement", "HTMLTableCaptionElement", "HTMLTableCellElement",
        "HTMLTableColElement", "HTMLTableElement", "HTMLTableRowElement",
        "HTMLTableSectionElement", "HTMLTemplateElement",
        "HTMLTextAreaElement", "HTMLTimeElement", "HTMLTitleElement",
        "HTMLTrackElement", "HTMLUListElement", "HTMLVideoElement",
        "Image", "Audio", "Option",
        "SVGElement", "SVGSVGElement", "SVGGraphicsElement",
        "SVGGeometryElement", "SVGPathElement", "SVGUseElement", NULL };
    for (int i = 0; cn[i]; i++)
        JS_SetPropertyStr(ctx, glob, cn[i], JS_DupValue(ctx, ctor));
    JS_FreeValue(ctx, ctor);
    if (url_install(ctx, glob) != 0)
        fprintf(stderr, "warning: url_install failed\n");
    if (txt_install(ctx, glob) != 0)
        fprintf(stderr, "warning: txt_install failed\n");
    if (crypto_install(ctx, glob) != 0)
        fprintf(stderr, "warning: crypto_install failed\n");
    if (fe_loadscript_install(ctx, glob) != 0)
        fprintf(stderr, "warning: fe_loadscript_install failed\n");
    JS_FreeValue(ctx, glob);

    /* Scriptable layer Lexbor doesn't provide (it's a parser/DOM, not
       a scripting host): real capture/target/bubble event dispatch
       over the Lexbor tree, on*-handler reflection, classList, dataset,
       a style object, common attribute-reflected element props, and a
       few document extras. The host edge (fetch/XHR/eval/opaque/taint,
       location/cookie, the event-loop pump) stays in hostedge.js. */
    static const char prelude[] =
    "(function(){var P=Object.getPrototypeOf(document);var LIS=new WeakMap();var ALLT=Object.create(null);"
    "function lst(n){var m=LIS.get(n);if(!m){m={};LIS.set(n,m);}return m;}"
    /* Expose the list of event types subscribed on a given element AND
       the union of types subscribed anywhere in the document so the
       entry-point driver can dispatch any-listener type on every
       element — letting WHATWG bubble semantics carry the event up to
       delegated handlers (the github / Catalyst pattern: handlers
       registered on <html> that route via event.target.closest(...)).
       No hardcoded type list, no framework recognition: drive exactly
       what the bundle wired. */
    "globalThis.__feListenerTypes=function(n){var m=LIS.get(n);return m?Object.keys(m):[];};"
    "globalThis.__feAllListenerTypes=function(){return Object.keys(ALLT);};"
    "P.addEventListener=function(t,f,o){if(typeof f!=='function'&&!(f&&f.handleEvent))return;var m=lst(this);(m[t]||(m[t]=[])).push({fn:f,cap:!!(o===true||(o&&o.capture))});ALLT[t]=1;"
    /* J-Force §3.1.3: element/document handlers funnel into the same
       forced-handler sink as window handlers so the entry-point driver
       reaches handler-hidden fetch/XHR (Catalyst/Turbo [data-action],
       button-click loaders). __feHandler is installed by hostedge,
       which runs after this prelude, so it exists at call time.
       The 2nd arg is `this` — the registration target. The driver
       binds it as the handler's `this` so reads like `this.src` /
       `this.dataset.X` resolve to the real CE element's concrete
       Lexbor-parsed attributes instead of opaque. WHATWG event-
       listener "this" semantics. */
    "try{var _h=typeof f==='function'?f:f.handleEvent;if(_h&&globalThis.__feHandler)globalThis.__feHandler(_h,this);}catch(e){}};"
    "P.removeEventListener=function(t,f){var m=LIS.get(this);if(m&&m[t])m[t]=m[t].filter(function(l){return l.fn!==f;});};"
    "P.dispatchEvent=function(ev){if(!ev||typeof ev!=='object')ev={type:String(ev)};"
    "if(ev.preventDefault===undefined)ev.preventDefault=function(){this.defaultPrevented=true;};"
    "if(ev.stopPropagation===undefined)ev.stopPropagation=function(){this._s=true;};"
    "if(ev.stopImmediatePropagation===undefined)ev.stopImmediatePropagation=function(){this._s=true;};"
    "ev.target=this;var path=[],n=this;while(n){path.push(n);n=n.parentNode;}var i,l,a;"
    "for(i=path.length-1;i>=1&&!ev._s;i--){a=(LIS.get(path[i])||{})[ev.type];if(a)for(l=0;l<a.length;l++)if(a[l].cap){ev.currentTarget=path[i];try{(a[l].fn.handleEvent||a[l].fn).call(path[i],ev);}catch(e){}}}"
    "if(!ev._s){a=(LIS.get(this)||{})[ev.type];if(a)for(l=0;l<a.length;l++){ev.currentTarget=this;try{(a[l].fn.handleEvent||a[l].fn).call(this,ev);}catch(e){}}var oh=this['on'+ev.type];if(typeof oh==='function'){ev.currentTarget=this;try{oh.call(this,ev);}catch(e){}}}"
    "if(ev.bubbles!==false)for(i=1;i<path.length&&!ev._s;i++){a=(LIS.get(path[i])||{})[ev.type];if(a)for(l=0;l<a.length;l++)if(!a[l].cap){ev.currentTarget=path[i];try{(a[l].fn.handleEvent||a[l].fn).call(path[i],ev);}catch(e){}}var bh=path[i]['on'+ev.type];if(typeof bh==='function'){try{bh.call(path[i],ev);}catch(e){}}}"
    "return !ev.defaultPrevented;};"
    "P.click=function(){this.dispatchEvent({type:'click',bubbles:true});};"
    /* HTMLFormElement.prototype.submit per WHATWG forms spec, defined
       here in the Lexbor binding layer (not as a hostedge.js monkey-
       patch). Effective only on FORM elements. Walks Lexbor input
       descendants to build a per-field body shape (literal default
       from value attribute, else __opaque marker for user-typed),
       then issues globalThis.fetch — the same hook the bundle's
       own JS reaches. Optional override = {formaction, formmethod}
       for <button formaction> submissions. The opaque marker for
       missing inputs is the engine's own taint sentinel (carries the
       symbolic shadow for security path; never stringifies to
       [object Object] inside the URL because the body is structured,
       not concatenated into the URL string). */
    "P.submit=function(o){if(String(this.tagName||'').toUpperCase()!=='FORM')return;"
    "var act=(o&&o.formaction)||this.getAttribute('action')||'';"
    "var m=((o&&o.formmethod)||this.getAttribute('method')||'GET').toUpperCase();"
    "var body={};var ins=this.querySelectorAll('input[name],select[name],textarea[name]');"
    "for(var i=0;i<ins.length;i++){var inp=ins[i],nm=inp.getAttribute('name');if(!nm)continue;"
    "var d=inp.getAttribute('value');body[nm]=d!=null?d:(typeof __opaque==='function'?__opaque('form.input.'+nm):null);}"
    "try{globalThis.fetch(act,{method:m,body:body});}catch(e){}};"
    "P.focus=function(){};P.blur=function(){};P.scrollIntoView=function(){};"
    "P.getBoundingClientRect=function(){return{top:0,left:0,right:0,bottom:0,width:0,height:0,x:0,y:0};};"
    "P.getClientRects=function(){return[];};P.normalize=function(){};"
    "P.hasAttributes=function(){return this.attributes.length>0;};"
    "P.getAttributeNode=function(n){var v=this.getAttribute(n);return v==null?null:{name:n,value:v};};"
    "P.setAttributeNS=function(ns,n,v){return this.setAttribute(n,v);};"
    "P.getAttributeNS=function(ns,n){return this.getAttribute(n);};"
    "P.removeAttributeNS=function(ns,n){return this.removeAttribute(n);};"
    "P.toggleAttribute=function(n,f){var h=this.hasAttribute(n);var on=f===undefined?!h:!!f;if(on)this.setAttribute(n,'');else this.removeAttribute(n);return on;};"
    "P.before=function(){};P.after=function(){};P.prepend=function(){};P.replaceWith=function(){};"
    "P.attachShadow=function(){return this;};P.animate=function(){return{cancel:function(){},finished:Promise.resolve()};};"
    "P.getContext=function(){return null;};P.addEventListener;"
    /* DOMTokenList — WHATWG defines it as iterable<DOMString>, so the
       returned stub must expose [Symbol.iterator] yielding the tokens.
       MDN's airgap.js destructures `e[Symbol.iterator]()` over every
       collection it touches (Set/Map/String/NodeList/DOMTokenList/
       HTMLCollection); a classList missing Symbol.iterator throws
       "TypeError: not a function" mid-script-eval and aborts the entire
       bundle before any of its fetches fire. The iterator pulls a
       fresh-tokens snapshot at iteration time (matches the spec —
       DOMTokenList is live so re-reading is correct). */
    "Object.defineProperty(P,'classList',{configurable:true,get:function(){var el=this;function A(){return(el.getAttribute('class')||'').split(/\\s+/).filter(Boolean);}var o={add:function(){var s=A();for(var i=0;i<arguments.length;i++)if(s.indexOf(arguments[i])<0)s.push(arguments[i]);el.setAttribute('class',s.join(' '));},remove:function(){var r=[].slice.call(arguments);el.setAttribute('class',A().filter(function(c){return r.indexOf(c)<0;}).join(' '));},toggle:function(c,f){var s=A(),i=s.indexOf(c),on=f===undefined?i<0:!!f;if(on&&i<0)s.push(c);else if(!on&&i>=0)s.splice(i,1);el.setAttribute('class',s.join(' '));return on;},contains:function(c){return A().indexOf(c)>=0;},replace:function(x,y){var s=A(),i=s.indexOf(x);if(i>=0){s[i]=y;el.setAttribute('class',s.join(' '));return true;}return false;},item:function(i){return A()[i]||null;},get length(){return A().length;},toString:function(){return el.getAttribute('class')||'';}};o[Symbol.iterator]=function(){return A()[Symbol.iterator]();};return o;}});"
    "Object.defineProperty(P,'dataset',{configurable:true,get:function(){var el=this;return new Proxy({},{get:function(_,k){if(typeof k!=='string')return undefined;var v=el.getAttribute('data-'+k.replace(/[A-Z]/g,function(m){return '-'+m.toLowerCase();}));return v==null?undefined:v;},set:function(_,k,v){el.setAttribute('data-'+String(k).replace(/[A-Z]/g,function(m){return '-'+m.toLowerCase();}),String(v));return true;},has:function(_,k){return el.hasAttribute('data-'+String(k));}});}});"
    "Object.defineProperty(P,'style',{configurable:true,get:function(){if(!this.__st){var m={};this.__st={setProperty:function(k,v){m[k]=v;},removeProperty:function(k){delete m[k];},getPropertyValue:function(k){return m[k]||'';},item:function(){return '';},get cssText(){return Object.keys(m).map(function(k){return k+':'+m[k];}).join(';');},set cssText(v){}};}return this.__st;}});"
    "['value','checked','selected','disabled','readOnly','href','src','type','name','title','action','placeholder','rel','htmlFor'].forEach(function(pk){var at=pk==='htmlFor'?'for':pk;Object.defineProperty(P,pk,{configurable:true,get:function(){var v=this.getAttribute(at);if(pk==='checked'||pk==='disabled'||pk==='selected'||pk==='readOnly')return v!=null;return v==null?'':v;},set:function(v){this.setAttribute(at,v===true?'':String(v));}});});"
    /* Element.isConnected — spec: true iff the node is reachable from the
       document root via parentNode. It was defined ONLY on `document` (a
       constant), so ELEMENTS returned undefined. That silently broke any
       custom element whose connectedCallback gates on it — github's
       <include-fragment> does `connectedCallback(){this.isConnected&&(this.#load(),…)}`,
       so on an upgraded SSR node `this.isConnected===undefined` short-circuited
       the eager #load() and its fetch NEVER fired (used_by_list /
       sidebar_partial lost despite the node being upgraded — the layer-5 gap).
       Walk parentNode to a document node (nodeType 9 / === document); detached
       nodes correctly return false. Acyclic tree ⇒ terminates at the root. */
    "Object.defineProperty(P,'isConnected',{configurable:true,get:function(){var n=this;while(n){if(n===document||n.nodeType===9)return true;n=n.parentNode;}return false;}});"
    "document.createDocumentFragment=function(){return document.createElement('div');};"
    "document.createComment=function(){return document.createTextNode('');};"
    "document.createElementNS=function(ns,t){return document.createElement(t);};"
    /* WHATWG document.importNode(node, deep): clone a node from another
       document into this document. For a single-document model (our
       only document is the page document), this is structurally
       equivalent to a deep clone of the node — the source and target
       documents are the same, no cross-document adoption needed.
       lit-html falls back to document.importNode when feature-detect
       A2 picks it over content.cloneNode; without this it throws
       'document.importNode is not a function' and aborts the entire
       template render path. */
    "document.importNode=function(n,deep){return n&&typeof n.cloneNode==='function'?n.cloneNode(deep!==false):null;};"
    /* WHATWG document.adoptNode: same as importNode but for a node
       being MOVED rather than cloned. In our single-document model,
       adoption is a no-op (the node already belongs to our document);
       return the node as-is. lit-html and other frameworks call this
       during DocumentFragment manipulation. */
    "document.adoptNode=function(n){return n;};"
    /* Custom-element upgrade: per HTML spec, createElement(name) of a
       *defined* element yields an upgraded instance (its constructor
       ran, prototype = the class). dom_ctor already builds the correct
       live Lexbor element for a registered tag (__ceTag), so `new C()`
       IS the upgrade. Re-entrancy guarded (a ctor that creates its own
       tag would recurse); the ctor is NOT try/caught — a throw is the
       next real host-model gap to surface, not to mask. */
    "(function(){var _oce=document.createElement.bind(document),UPG={};"
    "document.createElement=function(tag){var k=String(tag).toLowerCase();"
    "var R=globalThis.customElements,C=(R&&R.get)?R.get(k):null;"
    /* createElement of a defined tag runs the ctor (= upgrade) but does NOT
       connect it — mark __ceUpgraded so a later insertion's __ceConnect fires
       connectedCallback without re-running the ctor. */
    "if(C&&!UPG[k]){UPG[k]=1;try{var _inst=new C();try{_inst.__ceUpgraded=1;}catch(e){}return _inst;}finally{UPG[k]=0;}}"
    "return _oce(tag);};})();"
    "document.createEvent=function(){return{initEvent:function(){},type:''};};"
    /* Define each Document attribute as a real accessor on dom_proto so
       Object.getOwnPropertyDescriptor(Document.prototype, "X").get is
       a function (the airgap.js / lit-html introspection pattern). The
       prior `document.X = value` form created instance data properties
       — invisible from getOwnPropertyDescriptor on the prototype, and
       data-shape from getOwnPropertyDescriptor on the instance. Real
       WHATWG IDL defines these as Document attributes, i.e. prototype
       accessors. */
    "(function(){var P=Object.getPrototypeOf(document);"
    "function defAcc(n,fn){Object.defineProperty(P,n,{configurable:true,enumerable:false,get:fn,set:function(){}});}"
    "function defConst(n,v){defAcc(n,function(){return v;});}"
    /* currentScript MUST be a live getter that consults the engine's
       __feCurFile + /p.js-installed __feScriptElMap so airgap.js's
       cached descriptor.get returns the right script element when
       called with document as `this`. A constant null getter returned
       null even when /p.js installed an instance-level override —
       airgap caches the PROTOTYPE descriptor at boot, so the override
       was bypassed. The prototype-level getter is the canonical
       single source-of-truth. */
    "defAcc('currentScript',function(){var f=globalThis.__feCurFile;var m=globalThis.__feScriptElMap;return (m&&f&&m[f])||null;});"
    "defConst('readyState','complete');defConst('compatMode','CSS1Compat');"
    "defConst('characterSet','UTF-8');defConst('contentType','text/html');defConst('referrer','');"
    "defConst('defaultView',globalThis);defConst('xmlVersion','1.0');defConst('hidden',false);"
    "defConst('visibilityState','visible');defConst('dir','ltr');defConst('isConnected',true);"
    "defConst('baseURI',(globalThis.location&&globalThis.location.href)||'');"
    "var impl={createHTMLDocument:function(){return document;},hasFeature:function(){return true;},createDocumentType:function(){return{};}};"
    "defConst('implementation',impl);"
    "defAcc('namespaceURI',function(){return 'http://www.w3.org/1999/xhtml';});"
    /* doctype: synthesized minimal DocumentType node; airgap.js uses it
       structurally (existence + .name). Real value would come from the
       parsed <!DOCTYPE> declaration which Lexbor preserves — exposing
       that is a future binding. */
    "defAcc('doctype',function(){return{name:'html',publicId:'',systemId:'',nodeType:10,nodeName:'html'};});"
    /* cookie: host-edge tainted source per hostedge.js shim. The
       prototype accessor MUST defer to the live document.cookie value
       at call time so the taint stays attached to whoever reads it. */
    "defAcc('cookie',function(){return globalThis.__cookieValue||'';});"
    "})();"
    "document.getSelection=function(){return{toString:function(){return '';},removeAllRanges:function(){},addRange:function(){},rangeCount:0};};"
    "document.createRange=function(){return{setStart:function(){},setEnd:function(){},selectNodeContents:function(){},collapse:function(){},createContextualFragment:function(h){var d=document.createElement('div');d.innerHTML=h;return d;},getBoundingClientRect:function(){return{top:0,left:0,width:0,height:0};},getClientRects:function(){return[];}};};"
    /* Spec WHATWG TreeWalker (real implementation, not a null stub). Used
       by lit-html's template-parse loop: `for(;l<u;){let p=s.nextNode();
       if(p===null){s.currentNode=r.pop();continue}...}` — a null stub
       returns null forever, l never advances, and the bundle's eval
       does not terminate (the lit-html template parse is at module
       init time on every page that imports lit). The algorithm walks
       the Lexbor DOM through the wrapper's firstChild/nextSibling/
       parentNode getters, filtered by whatToShow and the optional
       NodeFilter. Pure JS so no engine binding is needed; the cost of
       traversal is bounded by the actual DOM (a real, finite tree),
       not by an iteration limit. */
    "document.createTreeWalker=function(root,whatToShow,filter){"
        "var W=(whatToShow>>>0)||0xFFFFFFFF;"
        "var F=filter&&typeof filter.acceptNode==='function'?filter:(typeof filter==='function'?{acceptNode:filter}:null);"
        "var cur=root;"
        "function acc(n){"
            "if(!n)return 2;"
            "var t=n.nodeType|0;if(t<1||t>12)return 2;"
            "if(!(W&(1<<(t-1))))return 3;"
            "if(F){var r;try{r=F.acceptNode(n)|0;}catch(e){r=2;}return r||1;}"
            "return 1;"
        "}"
        "return{root:root,whatToShow:W,filter:filter||null,"
            "get currentNode(){return cur;},set currentNode(v){cur=v;},"
            "nextNode:function(){"
                "var n=cur,r=1;"
                "for(;;){"
                    "while(r!==2&&n&&n.firstChild){n=n.firstChild;r=acc(n);if(r===1){cur=n;return n;}}"
                    "var sib=null,t=n;"
                    "while(t){if(t===root)return null;sib=t.nextSibling;if(sib){n=sib;break;}t=t.parentNode;}"
                    "if(!sib)return null;"
                    "r=acc(n);if(r===1){cur=n;return n;}"
                "}"
            "},"
            "firstChild:function(){var n=cur&&cur.firstChild;while(n){var r=acc(n);if(r===1){cur=n;return n;}if(r===3&&n.firstChild){n=n.firstChild;continue;}n=n.nextSibling;}return null;},"
            "lastChild:function(){return null;},previousNode:function(){return null;},"
            "nextSibling:function(){var n=cur&&cur.nextSibling;while(n){if(acc(n)===1){cur=n;return n;}n=n.nextSibling;}return null;},"
            "previousSibling:function(){return null;},"
            "parentNode:function(){var n=cur&&cur.parentNode;while(n&&n!==root){if(acc(n)===1){cur=n;return n;}n=n.parentNode;}return null;}"
        "};"
    "};"
    /* WHATWG NodeFilter / TreeWalker / NodeIterator interface objects with
       their constant set — real bundles (MDN's airgap.js, lit-html,
       Catalyst observers) destructure `TreeWalker:X` / `NodeFilter:X`
       and then read `X.SHOW_ELEMENT` etc. as masks. The constants ARE
       the contract; without them a literal-mask comparison goes opaque
       and the bundle either aborts (ReferenceError) or sees the wrong
       whatToShow value and walks nothing. Constructors stay as
       interface-object placeholders (real browsers expose them as
       [[Construct]]-able only via `document.createTreeWalker` etc., so
       there's no instance constructor to mimic — only the static side
       matters). */
    "(function(){var NF={FILTER_ACCEPT:1,FILTER_REJECT:2,FILTER_SKIP:3,"
        "SHOW_ALL:0xFFFFFFFF,SHOW_ELEMENT:1,SHOW_ATTRIBUTE:2,SHOW_TEXT:4,"
        "SHOW_CDATA_SECTION:8,SHOW_ENTITY_REFERENCE:16,SHOW_ENTITY:32,"
        "SHOW_PROCESSING_INSTRUCTION:64,SHOW_COMMENT:128,SHOW_DOCUMENT:256,"
        "SHOW_DOCUMENT_TYPE:512,SHOW_DOCUMENT_FRAGMENT:1024,SHOW_NOTATION:2048};"
        "function NodeFilter(){}Object.assign(NodeFilter,NF);"
        "function TreeWalker(){}Object.assign(TreeWalker,NF);"
        "function NodeIterator(){}Object.assign(NodeIterator,NF);"
        "globalThis.NodeFilter=NodeFilter;globalThis.TreeWalker=TreeWalker;globalThis.NodeIterator=NodeIterator;"
    "})();"
    /* WHATWG DOM/HTML interface-object stubs — real bundles dereference
       `DOMImplementation.prototype.createHTMLDocument`,
       `MessageEvent.prototype.data`, `Navigator.prototype.userAgent` etc.
       as part of their boot-time capability detection. The interface
       object's `prototype` carries the method/getter signatures the
       bundle reads. Missing globals were throwing ReferenceError
       mid-eval and aborting whole scripts (airgap.js bails before any
       fetch site fires). Spec-accurate enough for boot — actual
       instances come from `document.implementation`, `new MessageEvent`,
       `navigator` which we already model. */
    "(function(){"
        /* Do NOT clobber an interface already installed as a constructor by
           the C side (the cn[] loop set HTMLElement/Element/Node/Document/…
           to the Lexbor-backed dom_ctor). Overwriting HTMLElement with a
           plain function broke `class X extends HTMLElement` + super(): the
           super call no longer reached dom_ctor, so the element was never a
           real Lexbor node (null tagName) and its custom-element
           connectedCallback could not read this.src — the include-fragment /
           deferred-loader endpoints stayed unlearned. Skip if already a
           function; otherwise define the stub. */
        "function I(name,protoExtra){if(typeof globalThis[name]==='function')return;var f=function(){};if(protoExtra)Object.assign(f.prototype,protoExtra);globalThis[name]=f;}"
        "I('DOMImplementation',{createHTMLDocument:function(){return document;},createDocument:function(){return document;},hasFeature:function(){return true;}});"
        "I('Navigator',{userAgent:'',platform:'',language:'en-US',languages:['en-US'],cookieEnabled:true,onLine:true,sendBeacon:function(){return true;},clipboard:null});"
        "I('Performance',{now:function(){return Date.now();},mark:function(){},measure:function(){},getEntries:function(){return[];},getEntriesByType:function(){return[];},getEntriesByName:function(){return[];},clearMarks:function(){},clearMeasures:function(){},clearResourceTimings:function(){},setResourceTimingBufferSize:function(){},timeOrigin:0});"
        "I('PerformanceEntry',{name:'',entryType:'',startTime:0,duration:0});"
        "I('PerformanceObserver',{observe:function(){},disconnect:function(){},takeRecords:function(){return[];}});"
        "I('MessageEvent',{data:null,origin:'',lastEventId:'',source:null,ports:[]});"
        "I('Event',{type:'',target:null,currentTarget:null,bubbles:false,cancelable:false,defaultPrevented:false,preventDefault:function(){this.defaultPrevented=true;},stopPropagation:function(){},stopImmediatePropagation:function(){}});"
        "I('CustomEvent',{detail:null});"
        "I('SubmitEvent',{submitter:null});"
        "I('UIEvent',{view:null,detail:0});"
        "I('CookieChangeEvent',{changed:[],deleted:[]});"
        "I('CookieStore',{get:function(){return Promise.resolve(null);},getAll:function(){return Promise.resolve([]);},set:function(){return Promise.resolve();},delete:function(){return Promise.resolve();}});"
        "I('SecurityPolicyViolationEvent',{blockedURI:'',violatedDirective:''});"
        "I('FormData',{append:function(){},delete:function(){},get:function(){return null;},getAll:function(){return[];},has:function(){return false;},set:function(){},forEach:function(){},entries:function(){return[][Symbol.iterator]();},keys:function(){return[][Symbol.iterator]();},values:function(){return[][Symbol.iterator]();}});"
        "I('Headers',{append:function(){},delete:function(){},get:function(){return null;},getSetCookie:function(){return[];},has:function(){return false;},set:function(){},forEach:function(){},entries:function(){return[][Symbol.iterator]();},keys:function(){return[][Symbol.iterator]();},values:function(){return[][Symbol.iterator]();}});"
        "I('Request',{url:'',method:'GET',headers:null,body:null,bodyUsed:false,credentials:'same-origin',cache:'default',mode:'no-cors',redirect:'follow',clone:function(){return this;}});"
        "I('Response',{url:'',status:200,statusText:'',ok:true,redirected:false,type:'basic',headers:null,body:null,bodyUsed:false,clone:function(){return this;},text:function(){return Promise.resolve('');},json:function(){return Promise.resolve(null);},arrayBuffer:function(){return Promise.resolve(new ArrayBuffer(0));},blob:function(){return Promise.resolve(null);},formData:function(){return Promise.resolve(null);}});"
        "I('AbortController',{signal:null,abort:function(){}});"
        "I('AbortSignal',{aborted:false,reason:undefined,onabort:null});"
        "I('MutationObserver',{observe:function(){},disconnect:function(){},takeRecords:function(){return[];}});"
        /* IntersectionObserver: a placeholder for the prelude window only —
           hostedge.js OVERRIDES it (and Mutation/Resize/Performance/Reporting
           Observer) with _ObserverCtor, which is the REAL handler: it drives the
           callback via setTimeout→TQ (drained by __hostFlush) with an OPQ
           isIntersecting (so the bundle's `if(entry.isIntersecting)` forks both
           arms). An earlier __IO that drove the callback itself was DEAD CODE
           (overridden) — reverted to this stub. */
        "I('IntersectionObserver',{observe:function(){},unobserve:function(){},disconnect:function(){},takeRecords:function(){return[];}});"
        "I('ResizeObserver',{observe:function(){},unobserve:function(){},disconnect:function(){}});"
        "I('FileReader',{readAsText:function(){},readAsDataURL:function(){},readAsArrayBuffer:function(){},result:null,error:null,readyState:0});"
        "I('Blob',{size:0,type:'',slice:function(){return new Blob();},text:function(){return Promise.resolve('');},arrayBuffer:function(){return Promise.resolve(new ArrayBuffer(0));},stream:function(){return null;}});"
        "I('File',{name:'',size:0,type:'',lastModified:0});"
        "I('DOMParser',{parseFromString:function(s,t){var d=document.createElement('div');d.innerHTML=String(s);return d;}});"
        "I('XMLSerializer',{serializeToString:function(n){return n&&n.outerHTML||'';}});"
        "I('TextEncoder',{encoding:'utf-8',encode:function(){return new Uint8Array(0);},encodeInto:function(){return{read:0,written:0};}});"
        "I('TextDecoder',{encoding:'utf-8',fatal:false,ignoreBOM:false,decode:function(){return '';}});"
        "I('ReadableStream',{getReader:function(){return{read:function(){return Promise.resolve({done:true,value:undefined});},releaseLock:function(){},cancel:function(){return Promise.resolve();}};},cancel:function(){return Promise.resolve();},tee:function(){return[null,null];},pipeTo:function(){return Promise.resolve();},pipeThrough:function(){return null;}});"
        "I('WritableStream',{getWriter:function(){return{write:function(){return Promise.resolve();},close:function(){return Promise.resolve();},abort:function(){return Promise.resolve();},releaseLock:function(){}};},abort:function(){return Promise.resolve();},close:function(){return Promise.resolve();}});"
        "I('TransformStream',{readable:null,writable:null});"
        "I('CompressionStream',{readable:null,writable:null});"
        "I('DecompressionStream',{readable:null,writable:null});"
        "I('BroadcastChannel',{name:'',postMessage:function(){},close:function(){},onmessage:null});"
        "I('EventSource',{url:'',readyState:0,withCredentials:false,onopen:null,onmessage:null,onerror:null,close:function(){}});"
        "I('WebSocket',{url:'',readyState:0,bufferedAmount:0,onopen:null,onmessage:null,onerror:null,onclose:null,send:function(){},close:function(){}});"
        "I('Worker',{postMessage:function(){},terminate:function(){},onmessage:null,onerror:null});"
        "I('SharedWorker',{port:null});"
        "I('Notification',{title:'',body:'',icon:'',close:function(){}});"
        "I('Document',{});"
        "I('HTMLDocument',{});"
        "I('DocumentFragment',{});"
        "I('Element',{});"
        "I('HTMLElement',{});"
        "I('Node',{});"
        "I('NodeList',{});"
        "I('HTMLCollection',{});"
        "I('Range',{setStart:function(){},setEnd:function(){},selectNodeContents:function(){},collapse:function(){},cloneContents:function(){return document.createDocumentFragment();}});"
        "I('Selection',{toString:function(){return '';},removeAllRanges:function(){},addRange:function(){},rangeCount:0});"
        "I('CSSStyleSheet',{cssRules:[],insertRule:function(){return 0;},deleteRule:function(){},replaceSync:function(){}});"
        "I('CSSRule',{cssText:'',type:0});"
        "I('Storage',{getItem:function(){return null;},setItem:function(){},removeItem:function(){},clear:function(){},key:function(){return null;},length:0});"
        "I('History',{length:0,state:null,scrollRestoration:'auto',pushState:function(){},replaceState:function(){},go:function(){},back:function(){},forward:function(){}});"
        "I('Screen',{width:1920,height:1080,availWidth:1920,availHeight:1080,colorDepth:24,pixelDepth:24});"
        "I('Window',{});"
        /* Point each WHATWG DOM-interface constructor's .prototype at the
           Lexbor-wrapper proto (the one Object.getPrototypeOf(document)
           returns). Real bundles (MDN's airgap.js) introspect via
           `Object.getOwnPropertyDescriptor(Node.prototype, "ownerDocument")
           .get` to obtain the accessor getter, then call it on Lexbor
           nodes. With separate empty .prototype objects (the I() default),
           getOwnPropertyDescriptor returns undefined → `.get` is undefined
           → bundle throws "not a function" and aborts before any fetch
           fires. Sharing dom_proto makes every spec attribute a real
           accessor backed by Lexbor. */
        "var IDOMS=['Node','Element','HTMLElement','Document','HTMLDocument','DocumentFragment','NodeList','HTMLCollection','CharacterData','Text','Comment','Attr','ProcessingInstruction','ValidityState','DOMTokenList','DOMStringMap','NamedNodeMap','CSSStyleDeclaration'];"
        /* Every HTML*Element interface ALSO gets its .prototype = dp.
           WHATWG defines dozens (HTMLInputElement, HTMLAnchorElement,
           HTMLScriptElement, ...) — each one is a constructor function
           globalThis exposes with its own .prototype. Bundles read
           `HTMLInputElement.prototype.value.set` etc. to obtain the
           accessor. Without sharing dp, every one would have an empty
           prototype and the accessor lookup returns undefined. */
        "var dp=Object.getPrototypeOf(document);"
        "for(var iD=0;iD<IDOMS.length;iD++){var ic=globalThis[IDOMS[iD]];if(ic&&typeof ic==='function'){try{ic.prototype=dp;}catch(e){}}}"
        "var gN2=Object.getOwnPropertyNames(globalThis);"
        "for(var iH=0;iH<gN2.length;iH++){var nm=gN2[iH];if(/^HTML[A-Z]\\w*Element$/.test(nm)){var ic2=globalThis[nm];if(ic2&&typeof ic2==='function'){try{ic2.prototype=dp;}catch(e){}}}}"
        /* Copy every own-property of `document` onto dp (= Document.prototype
           per WHATWG IDL). Many were set as instance data properties
           earlier in this prelude (`document.createElement = function…`,
           `document.body = …`, etc.) — bundles destructure these via
           `Document.prototype.createElement` etc., which requires them
           on the prototype. Skip if dp already has a descriptor (the
           defAcc()-installed accessors take precedence). */
        "var dN=Object.getOwnPropertyNames(document);"
        "for(var dn=0;dn<dN.length;dn++){var dk=dN[dn];if(Object.getOwnPropertyDescriptor(dp,dk))continue;try{Object.defineProperty(dp,dk,Object.getOwnPropertyDescriptor(document,dk));}catch(e){}}"
    "})();"
    "document.activeElement=document.body;document.scrollingElement=document.documentElement;"
    "document.dir='ltr';document.visibilityState='visible';document.hidden=false;"
    /* CustomElementRegistry: Catalyst (@controller), Lit, Stimulus all
       call customElements.define(name,class) at module top level — an
       absent registry is an immediate throw that aborts the bundle.
       Spec surface only (define/get/getName/whenDefined/upgrade);
       actually constructing + connectedCallback-driving registered
       elements is the entry-point driver's job, not the registry's. */
    "(function(){var R={},W={};"
    /* up(el,c): HTML-spec custom-element upgrade of one element — push el
       on the construction stack (__ceUp) so super() in dom_ctor returns
       el (the ctor initialises the REAL element in place, keeping its
       parsed attributes like include-fragment@src), set its prototype to
       the class, then fire connectedCallback. __ceUpgraded guards against
       re-fire (and connectedCallback-triggered re-entry). */
    /* Construction and connection are SEPARATE reactions (spec): an element
       may already be upgraded (its ctor ran — e.g. document.createElement of
       a defined tag) yet not connected. Guard them independently so
       connectedCallback still fires on insertion of a pre-constructed
       element. */
    "function up(el,c){if(!el)return;"
    "if(!el.__ceUpgraded){el.__ceUpgraded=1;try{Object.setPrototypeOf(el,c.prototype);}catch(e){}"
    "globalThis.__ceUp=el;try{Reflect.construct(c,[],c);}catch(e){}globalThis.__ceUp=null;}"
    "if(!el.__ceConnected){el.__ceConnected=1;if(typeof el.connectedCallback==='function'){try{el.connectedCallback();}catch(e){}}}}"
    /* CE-reactions on CONNECTION: the C DOM binding calls __ceConnect(node)
       after appendChild / insertBefore / innerHTML / insertAdjacent* so a
       custom element (or a subtree containing one) that is INSERTED after
       its class was defined fires connectedCallback — exactly as a browser
       fires CE reactions on connection. This is what makes JS-INJECTED
       loaders run their fetch(this.src): github renders its
       <include-fragment src=…> partials (latest-commit, used_by_list,
       tree-commit-info, …) post-hydration, so they are NOT in the SSR HTML
       define()'s upgrade loop walks — they arrive via DOM insertion, and
       only this hook drives them. Cheap-guarded: returns immediately when
       no custom element is registered yet. */
    "globalThis.__ceConnect=function(node){if(!node)return;var rk=0;for(var z in R){rk=1;break;}if(!rk)return;try{"
    "var tn=(node.tagName||'').toLowerCase();if(R[tn])up(node,R[tn]);"
    "if(node.querySelectorAll){var d=node.querySelectorAll('*');for(var i=0;i<d.length;i++){var e=d[i],t=(e.tagName||'').toLowerCase();if(R[t])up(e,R[t]);}}"
    "}catch(e){}};"
    /* CE-reactions on ATTRIBUTE CHANGE: the C DOM binding calls __ceAttr(node,
       name, old, new) after setAttribute so a defined custom element runs its
       attributeChangedCallback under forced exec. include-fragment (and many web
       components) fetch on a `src`/`data-*` change via attributeChangedCallback,
       not only connectedCallback -- without this the concrete value set through
       setAttribute never reaches the fetch (the orphan drive only yields an
       opaque arg). Spec-aligned: fires only for attrs in observedAttributes of
       an already-upgraded element. Cheap-guarded: returns when no CE registered. */
    "globalThis.__ceAttr=function(node,name,old,val){if(!node)return;var rk=0;for(var z in R){rk=1;break;}if(!rk)return;try{"
    "var tn=(node.tagName||'').toLowerCase();var C=R[tn];if(!C)return;"
    "var ob;try{ob=C.observedAttributes;}catch(e){}if(!(ob&&ob.indexOf&&ob.indexOf(name)>=0))return;"
    "if(typeof node.attributeChangedCallback==='function'){try{node.attributeChangedCallback(name,old,val);}catch(e){}}"
    "}catch(e){}};"
    /* Capture the real `print` NOW (prelude eval, before any bundle clobbers
       the global — same trick hostedge.js uses for EPRINT) so the ce_define
       diagnostic below survives into bundle-exec time when define() actually
       runs. */
    "var __feDomPrint=(typeof print==='function')?print:null;"
    "globalThis.customElements={"
    "define:function(n,c){R[n]=c;try{c.__ceName=n;if(c&&c.prototype)Object.defineProperty(c.prototype,'__ceTag',{value:n,configurable:true});}catch(e){}"
    /* On define, upgrade every element of this tag already in the (real
       server-rendered) document — the SSR-present loaders. Emit one deduped
       @WHY per distinct tag with the SSR-match count: this surfaces WHICH
       custom elements actually got defined+executed during analysis and
       whether the define found SSR nodes to upgrade — the layer-4 signal for
       lazy-defined elements like include-fragment (define present in the
       folded combined code but maybe never executed, or executed under an
       opaque/computed tag, or executed with 0 SSR matches). */
    "try{var EX=document.querySelectorAll(n);for(var qi=0;qi<EX.length;qi++){up(EX[qi],c);}"
    "if(__feDomPrint){if(!R.__ceLg)R.__ceLg={};var _tg=String(n);if(!R.__ceLg[_tg]){R.__ceLg[_tg]=1;__feDomPrint('@WHY {\"phase\":\"ce_define\",\"tag\":'+JSON.stringify(_tg)+',\"ssr\":'+EX.length+'}');}}}catch(e){}"
    "var w=W[n];if(w){W[n]=null;for(var i=0;i<w.length;i++)w[i](c);}},"
    "get:function(n){return R[n];},"
    "getName:function(c){for(var k in R)if(R[k]===c)return k;return null;},"
    /* The bundle's `await customElements.whenDefined(name)` is its way of
       gating "feature X needs tag-Y registered first." If we hand it a
       PENDING promise (W[n].push(r)), the awaiting async function suspends
       FOREVER — the resolver `r` is captured in W[n] which is owned by
       globalThis.customElements (rt-rooted), the await reaction pins the
       continuation, and the suspended frame + its closure (≈1800 GC objs
       on github) never collect. JS_FreeRuntime then asserts on the residue.
       Resolve with an opaque sentinel (structural-learning rule) so the
       await completes, the gated path runs, and any property access /
       instanceof / construction on the class flows opaque downstream — the
       BFS still explores branches keyed on those values, instead of stalling
       on a never-settling promise. Real customElements.define(name,c)
       overwrites R[n]=c so a later whenDefined hits the real class. */
    "whenDefined:function(n){if(!R[n])R[n]=(typeof globalThis.__opaque==='function')?globalThis.__opaque():null;return Promise.resolve(R[n]);},"
    "upgrade:function(){}};})();"
    "})();";
    JSValue pv = JS_Eval(ctx, prelude, sizeof(prelude) - 1, "<qjs-dom-prelude>",
        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(pv)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "qjs_dom prelude error: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, pv);
        return -1;
    }
    JS_FreeValue(ctx, pv);
    return 0;
}
