/* THE STEP-MACHINE DECLARATION SURFACE — public, so a HOST component can declare one.
 *
 * A continuation-holding builtin is a step machine in this engine: it declares itself at its definition
 * (JS_CFUNC_STEP_DEF) and is driven from the one convergence point every call shape reaches, so its callback's
 * body suspends and resumes at any depth. Everything needed to WRITE one lived in quickjs.c, which meant only
 * quickjs.c could write one — and a browser component under engine/host/browser was therefore forced to read
 * its Web IDL arguments with JS_GetPropertyStr from C. That is page code running with no flow base, which is
 * exactly what the twelve internal-method entries abort on: core/fetch/fetch.c hit it on
 * `fetch(new Proxy({url:"/q"}, {get(){}}))` the first time it ran, and every component after it would have.
 *
 * These three types are that surface, moved here unchanged. The types their operations name that remain
 * internal (StringBuffer, ValueSlot, JSMapRecord, REExecContext) are forward tags: a machine passes them
 * through, it never opens them. */
#ifndef QUICKJS_STEP_H
#define QUICKJS_STEP_H

#include "quickjs.h"

/* Internal to the engine, opaque to a machine that only forwards them to a visit operation.
   THESE TAGS ARE DECLARED HERE, AT FILE SCOPE, AND THAT PLACEMENT IS THE POINT. C scopes a struct tag to
   wherever it FIRST appears, so a tag whose first mention is inside a prototype is a fresh type belonging to
   that prototype — and every visitor assigning js_step_visit_*_slots would then be an incompatible-pointer
   error. gcc -w hid all three; the project's clang build does not. */
typedef struct StringBuffer StringBuffer;
struct ValueSlot;
struct JSMapRecord;
struct REExecContext;
struct JSStepHdr;

typedef struct JSStepVisit JSStepVisit;
struct JSStepVisit {
    void (*val)(JSContext *ctx, JSValue *slot);
    /* An ACCUMULATOR the machine is building into. Not a JSValue and not shareable: two forked arms each append
       their own remaining elements, so the clone needs its own storage holding what has been accumulated so
       far. Declared as its own operation rather than left to the machine because a machine must never learn
       WHICH consumer is visiting it — that is what keeps the one declaration honest. */
    void (*strbuf)(JSContext *ctx, StringBuffer *slot);
    /* An OWN-KEY SNAPSHOT: the JSPropertyEnum array a walk took of its source, `n` entries, each holding an
       atom reference. Its own operation for the same reason the accumulator is — it is one allocation the
       byte-copy would leave two states pointing at, and its elements are atoms rather than values. */
    void (*props)(JSContext *ctx, JSPropertyEnum **slot, uint32_t n);
    /* A SLOT ARRAY a machine is sorting or gathering into: `cap` entries of storage, of which [from,to) hold
       live references. Sort's comparator is the page's code, so a fork mid-sort is ordinary, and the two arms
       merge independently from there — one array cannot serve both. */
    void (*slots)(JSContext *ctx, struct ValueSlot **slot, int64_t cap, int64_t from, int64_t to);
    /* A PLAIN BUFFER: storage holding no references, copied whole. It does not try to distinguish
       write-before-read scratch from a buffer read back across a suspension — that distinction was here and it
       is a footgun, because using the non-copying form on the second kind loses the data silently and nothing
       catches it. One operation that always copies is correct for both and cannot be got wrong. */
    void (*buf)(JSContext *ctx, void **slot, size_t bytes);
    /* An ATOM reference. Not a JSValue, and a machine that parked one mid-read owns it exactly as it owns a
       value — GetSubstitution holds the key between `$<` and `>` across the read it names. */
    void (*atom)(JSContext *ctx, JSAtom *slot);
    /* A DELEGATED MACHINE: one step machine performing an abstract operation that is itself a machine, held
       until the driver adopts it. It clones through the same tramp_step_state_clone as any other, so
       delegation composes rather than being a hole — a fork inside `str.replace`'s built-in @@replace is a
       fork inside the machine it handed its whole walk to. */
    void (*machine)(JSContext *ctx, void **slot);
    /* An ARRAY OF SUB-OBJECTS: `n` live elements of `size` bytes in a `cap`-element allocation, each visited by
       `each`. ONE operation rather than a buffer plus a loop in the machine, because the two consumers need
       OPPOSITE ORDER — the clone must copy the array before taking references into it, the teardown must
       release those references before freeing it — and getting that backwards is a use-after-free the machine
       should never be in a position to write. */
    void (*array)(JSContext *ctx, void **slot, size_t size, int n, int cap,
                  void (*each)(JSContext *ctx, void *elem, JSStepVisit *v));
    /* A REFCOUNTED READ-ONLY structure two flows may SHARE. Sharing rather than copying is not a shortcut here,
       it is the correct answer: nothing writes it after it is built, which is also what keeps pointers already
       taken INTO it valid in both arms — a copy would leave the sibling's interior pointers naming the
       original's tree. The clone takes a reference, the teardown drops one and destroys at zero. */
    void (*shared)(JSContext *ctx, void **slot, int *refs, void (*destroy)(JSContext *ctx, void *p));
    /* A COLLECTION RECORD a machine LOCKS across a callback. Map.forEach holds the record it is standing on so
       the page's callback cannot delete the cursor out from under it, and a clone standing on the same record
       needs its own lock or the first arm to finish frees what the second is still reading. */
    void (*maprec)(JSContext *ctx, struct JSMapRecord **slot);
    /* A LIVE REGEXP MATCH. The one owned thing whose two consumers are not the same walk over the same fields:
       the teardown's job IS lre_exec_end, and the fork's is to give the sibling its own backtracking stack and
       its own `reach` filter AND to re-point the two SELF-REFERENCES a byte copy leaves aimed at the original —
       the inline stack buffer, and the machine's capture array. That is why this is an operation and not a
       list: a machine must never learn which consumer is visiting it, and here the two do different work.
       `capture` is the MACHINE's block, already visited by the time this runs, so the context is re-pointed at
       whichever copy it now belongs to. Every other pointer in the context aims into the subject string or the
       compiled bytecode, and both arms hold their own reference to those, so both stay valid unchanged. */
    void (*reexec)(JSContext *ctx, struct REExecContext *ec, uint8_t **capture);
};

/* THE HOST-FACING DOOR IS NOT OPEN YET, AND THIS IS WHERE IT GOES.
   A browser component under engine/host/browser cannot declare a step machine: JS_CFUNC_STEP_DEF is public in
   quickjs.h but the STEPDEF_* ids it takes index js_tramp_step_defs[], a static table in this file, and
   JSTrampStepDef / JSStepHdr / JSStepVisit / step_getprop_run are all internal to it. So every Web IDL
   attribute read a component performs is forced to be a JS_GetPropertyStr from C — which is exactly what the
   twelve internal-method entries abort on, and core/fetch/fetch.c hit it on `fetch(new Proxy({url:"/q"},…))`
   the first time it ran.
   Opening it is: this struct plus JSStepHdr, JSStepVisit and the step_*_run request helpers move to a public
   quickjs-step.h, and js_tramp_step_defs gains a runtime tail so JS_RegisterStepDef can hand a host component
   an id past STEPDEF_COUNT. Until then a component handles what it can read without touching the page's
   objects and DFAILs on the rest at its own name. */
typedef struct JSTrampStepDef {
    size_t   size;
    int     (*step)(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc);
    JSValue (*fini)(JSContext *ctx, void *st, bool take_result);
    int      arg;
    /* COERCE-THEN-COMPUTE machines only (js_primargs_step); zero for every other definition. Such a builtin's
       ONLY user code is ToPrimitive on some of its arguments, and everything after — JS_ToIndex, JS_ToNumber,
       JS_ToBigInt, JS_ToPropertyKey on a PRIMITIVE — runs none. So the builtin DECLARES which arguments it
       coerces and this one machine performs those coercions on the tramp, then calls the body with the
       primitives in place. The body is not a legacy twin: with primitive arguments it has no user code left to
       reach, which is exactly what the declaration asserts, and it is the only implementation there is. */
    JSCFunctionType body;
    uint8_t  body_proto;   /* JS_CFUNC_generic, _generic_magic or _constructor_or_func */
    int      body_magic;
    /* A VALIDATION the spec performs BEFORE the coercions — DataView's setters reject an immutable buffer
       before reading either argument, and test262 pins that ordering. It is part of the declaration because
       "everything before the coercions" is exactly what the machine has to reproduce; a builtin whose leading
       check this cannot express does not carry the declaration at all. -1 = threw.
       It is handed THE INVOCATION, not a receiver and a magic: a leading check is as likely to be about an
       ARGUMENT (FinalizationRegistry rejects a non-callable, WeakRef an unholdable target, both before the
       `prototype` read) as about the receiver, and the header is what carries all of it. */
    int      (*precheck)(JSContext *ctx, const struct JSStepHdr *h);
    /* A validation that runs between the FIRST coerced argument and the next one. DataView's setters must throw
       ToIndex's RangeError for a fractional byteOffset before the value's valueOf runs, and test262 pins that
       too. Like precheck it is part of the declaration: it names the computation the spec interleaves, and the
       body re-runs it on the way through. -1 = threw. */
    int      (*midcheck)(JSContext *ctx, JSValueConst *argp, int magic);
    /* IfAbruptCloseIterator and its kin: what the spec does when an ARGUMENT COERCION throws. The coercion
       aborts in the driver, which tears the machine down through fini, so this runs from there — and only when
       a coercion was actually in flight, because a body that threw has already run its own cleanup. */
    void     (*onerror)(JSContext *ctx, JSValueConst this_val, int magic);
    /* This machine's algorithm CATCHES an abrupt request result instead of propagating it — 27.2.1.3.2 step 9
       rejects the promise when `Get(resolution, "then")` throws, so the throw is a VALUE to it, the way
       CONT_IMPORT's is to the interpreter. Declaring it is what lets the teardown hand the exception back to
       step() (as JS_EXCEPTION, with the throw still live in the context) rather than free the chain. Zero for
       every other definition, which is the default a positional initializer leaves. */
    uint8_t  catches_abrupt;
    /* SetterThatIgnoresPrototypeProperties' `home` operand, as a class_proto id: the object the setter refuses to
       write to because the accessor LIVES there. `arg` carries the AO's `p` for the same reason — both are
       operands of the algorithm, so both are declared rather than hard-coded, which is what lets one machine
       serve %Iterator.prototype%'s two accessors and Error.prototype.stack. 0 (JS_CLASS_OBJECT) for every other
       definition; those machines never read it. */
    int      home_class;
    /* WHAT THIS MACHINE OWNS — declared ONCE and visited by everything that needs to know. This is Blink's
       Trace, and for Trace's reason: two consumers need the same list and a list written twice drifts. They are
       the TEARDOWN (release each) and the DEEP-FORK CLONE (take a second reference to each, because a concolic
       branch inside a callback forks the flow at that depth and `[1,2].forEach(e => cfg.admin ? … : …)` must
       not leave two arms sharing one cursor and one result array). Written as two lists they have already
       drifted twice in this file — a consume state co-owned `next` across two flows, and a for-await drive left
       three of its five fields borrowed — which is what ITERCONS_OWNED and AFS_OWNED were each written to stop
       locally and what this stops everywhere.
       Only the fields the machine OWNS: a BORROWED view of its own cb array or of the header's captures is not
       visited, or the clone over-counts and the teardown over-frees. Ownership that is conditional is an `if`
       around the visit, which is why this is a function and not a table of offsets — the TypedArray-filter
       writeback owns three cb slots and only while that call is in flight.
       A machine that does not declare one cannot be forked and says so at the fork rather than silently handing
       two flows one state; declaring it is part of declaring the machine. */
    void     (*visit)(JSContext *ctx, void *st, JSStepVisit *v);
    /* WHICH ALGORITHM THIS MACHINE IS, AND WHICH OF ITS STEPS EACH STAGE NAMES.
     *
     * `stage` was documented as "the machine numbers the rest however it likes", and that is the whole of the
     * problem: a stage is where a machine RESTS — where it parks, where a sibling flow overtakes it, where a
     * cold-tier resume picks it up — and a private integer says nothing about where in the algorithm that is.
     * Two spec steps bundled into one stage are two steps between which this engine cannot suspend, which is
     * exactly the skipped logic the design forbids, and nothing could see it because the machine's own numbers
     * were internally consistent either way.
     *
     * `steps[stage]` is the spec step that stage rests at, as the standard writes it ("22.2.7.2 step 2"). That
     * makes a resume point EXACT rather than approximate: the driver asserts that a machine only ever rests at
     * a step of its own algorithm, a parked machine can SAY where it is parked, and a stage number means the
     * same thing in the next session as in this one — which is what a cross-session resume of a suspended
     * machine requires and what a private counter can never give it.
     *
     * NULL-terminated, indexed by stage, so a stage past the end is a machine that invented one. BOTH ARE
     * MANDATORY: this said "a definition that has not been converted yet declares neither, and the count of
     * those is the conversion's work queue" and pointed at a build gate that reports it. The gate does not
     * exist — it named one that had already been deleted — and the queue is empty, so what the sentence
     * actually described was an exemption with nothing left to exempt and nothing watching it. Every definition
     * now declares both, asserted where a def enters the runtime (JS_RegisterStepDef for a host component's,
     * js_step_defs_check_table for the engine's own) rather than counted anywhere; the conditional that used to
     * skip step_stage_check for an undeclared def is gone with it, because a guard whose false arm checks
     * nothing is how a machine that LOSES its declaration goes on resting wherever it likes.
     *
     * THE LABEL IS THE STAGE'S IDENTITY; THE INDEX IS ONLY THIS BUILD'S NAME FOR IT — and that is why the two
     * are ONE declaration (JS_STEP_STAGES below) rather than an enum beside an array. Written apart they are
     * two statements of one fact: RegExpBuiltinExec had eight constants and eight strings, and inserting a step
     * into one and not the other renames every stage after it, so `steps[stage]` names the wrong step and the
     * numbers still agree with themselves. The old defence was to number a stage OUT of sequence rather than
     * renumber the tail, which is a workaround for a drift the single declaration makes impossible.
     * ACROSS BUILDS the index cannot be the identity at all: a flow parked at stage 5 and resumed by a build
     * whose stages moved resumes at a different step of its algorithm, silently. So a parked machine's rest
     * point is its LABEL, and a resume RESOLVES that label back to an index in the build doing the resuming —
     * which is also why two stages may not declare the same label, asserted at every rest (step_stage_check).
     * A build-version stamp is the wrong answer to the same question: it rejects every parked snapshot whenever
     * ANY machine changes, and §scheduler's frontier never drops a work item. */
    const char *algorithm;
    const char *const *steps;
} JSTrampStepDef;

/* ONE DECLARATION OF A MACHINE'S STAGES — the enum and the labels, from one list, so neither can move without
   the other. A machine writes:
       #define REX_STAGES(X) \
           X(REX_REQUIRE,  "22.2.6.2 step 2 (RequireInternalSlot)") \
           X(REX_TOSTRING, "22.2.6.2 step 3 (S is ToString(string))")
       enum { REX_STAGES(JS_STEP_STAGE_ENUM) };
       static const char *const js_regexp_exec_steps[] = { REX_STAGES(JS_STEP_STAGE_LABEL) NULL };
   and the step gate refuses a `.steps` array that is anything else — a hand-written list of strings beside a
   hand-written enum is exactly the pair that drifts. See JSTrampStepDef.steps for why the label, not the index,
   is what a parked machine is holding. */
#define JS_STEP_STAGE_ENUM(name, label)  name,
#define JS_STEP_STAGE_LABEL(name, label) label,

typedef struct JSStepHdr {
    const JSTrampStepDef *def;
    /* THE RUNTIME'S CENSUS OF LIVE MACHINES — the same accounting gc_obj_list gives every GC object, given to
       the one allocation class that escapes it. A step state is plain js_malloc'd memory holding references, so
       a machine the driver drops is invisible to JS_FreeRuntime's object walk AND to its refcount report: the
       objects it held are reported with nothing naming their owner, and the state itself is reported by nothing
       at all. Two pointers rather than a list_head because this header is public and list.h is not; the list is
       quickjs.c's (js_step_census_*), and its invariants are asserted there — a machine is on it from
       tramp_step_state_new (and from the CLONE, whose memcpy copies the ORIGINAL's links and must relink)
       until tramp_step_state_free_1 takes it off, immediately before the definition's fini frees the block. */
    struct JSStepHdr *census_prev, *census_next;
    int orig_cfirst, orig_cargc;
    uint8_t orig_is_tail;
    /* A step machine whose CALLBACK is itself a step machine — `arr.map(String)` is the ordinary case. The inner
       one is driven by the same do_step_tramp, and its result is delivered to the outer step instead of being
       pushed. Without this the inner reached js_call_c_function's DFAIL: the CALL arm invoked a C callback in
       place, which is right for a callback with no body and wrong for one that IS a machine. Third place this
       outer-continuation shape has been needed (construct, ToPrimitive, and here), which is what makes it the
       primitive rather than three special cases. */
    void *outer;
    uint8_t outer_kind;
    /* A CONSTRUCT request's NEW TARGET (owned), UNINITIALIZED when it is the constructor itself — which is what
       every builtin's Construct performs and what the request meant before Reflect.construct needed to say
       otherwise. It lives here rather than in an interpreter register because a machine cannot reach one, and
       the request can suspend between the step that sets it and the dispatch that reads it. */
    JSValue ctor_ntgt;
    /* A CAPABILITY request's result: NewPromiseCapability's [promise, resolve, reject], OWNED. It lives here for
       the reason ctor_ntgt does — a machine cannot reach an interpreter register, and the request SUSPENDS
       between the step that asks and the delivery that answers, because the subclass constructor is user code.
       Three values rather than one because that is what the record is; packing them into the step's single
       result would make every consumer unpack a tuple the engine had just built. */
    JSValue cap_promise, cap_funcs[2];
    /* A DEFINE request's descriptor SHAPE, for a machine that needs one other than CreateDataProperty's. The
       request expressed only that one shape, hardcoded at three sites, and Object.defineProperty needs an
       arbitrary descriptor — so the shape became operands. They ride the HEADER rather than the cb array for the
       same reason get_atom does: a request's operands that have no stack slot live here, and every existing
       DEFINE keeps working by leaving them alone. desc_flags == 0 means CreateDataProperty's shape.
       READ AND RESET by the driver like every other request input, so a shape cannot leak into the next one. */
    JSValueConst desc_get, desc_set;   /* BORROWED: the machine holds them across the request */
    int desc_flags;
    /* A GETOWNPROP request's ANSWER SHAPE, for a machine that wants the descriptor RECORD instead of the
       descriptor OBJECT — the invariants' JSDescFacts, reached from a step machine rather than from a
       continuation. It rides the header for the same reason the shape above does: a request's operand with no
       stack slot lives here, and the driver READS AND RESETS it like every other request input, so an answer
       shape cannot leak into the next request. NULL = the object, which is what every other consumer takes. */
    struct JSDescFacts *desc_facts;
    /* A DELEGATE request's inner machine, owned until the driver adopts it. Delegation is how a step builtin
       performs an ABSTRACT OPERATION that is itself a machine — .finally's PromiseResolve(C, v), which 27.2.5.3
       step 4 states as the operation and not as C.resolve, so it cannot be reached as an ordinary call.
       FORWARDING the inner's step from the outer's (what str.replace's @@replace delegation does) works only
       while the inner's requests carry everything in cb_result: a CAPABILITY or CONSTRUCT records the state the
       DRIVER is stepping, which under forwarding is the OUTER, so the answer lands on the wrong header. Handing
       the inner to the driver instead makes it a real machine on the chain, and the CONT_STEP outer it already
       carries is what delivers its result back. */
    void *delegate;
    /* CONT_FOROF_NEXT carries no STATE — its enum_rec offset is the whole of what the continuation needs — so a
       machine reached as a for-of .next() (Iterator.concat's) has an outer_kind with a NULL outer, and this is
       what its delivery reads. Without it the DONE arm saw no outer at all and pushed the result as a bare
       operand, which the loop then read as its enum_rec. */
    int outer_forof;
    /* THE INVOCATION. Captured (owned) by the driver before the first step, because a prologue that suspends
       resumes into a step whose C locals are gone and whose operand stack the machine must not depend on. It is
       also what let every machine drop its private copy of the receiver and the argument vector: those were the
       same three lines of duplication in twelve inits, and each one was a separate chance to forget a dup. */
    uint16_t stage;      /* 0 = the prologue has not run; the machine numbers the rest however it likes */
    int argc;
    int arg;             /* def->arg — the mode that used to be an init parameter */
    JSValue this_val;    /* the receiver; new_target for a constructor step */
    JSValue func_obj;    /* the callee — a closure step machine reads its func_data record through this */
    JSValue *argv;       /* argc owned values, in the tail of the state's own allocation */
    /* A prologue's in-flight sub-sequence (today: LengthOfArrayLike). `coerce` is the value it is holding across
       a suspension — the machine's C locals are gone when it resumes — and cb_coerce is the request buffer, a
       borrowed [value] for a TOPRIMITIVE, [obj] for a GETPROP, or [ctor, arg] for a CONSTRUCT. `len_phase` is the
       sub-sequence's own cursor, kept here so the machine spends ONE of its stages on the whole read instead of
       one per suspension point. Every coercing prologue needs exactly this, so it lives in the header rather
       than being re-declared (and mis-declared) in each state. */
    JSValue coerce;
    /* THE UNKNOWN-LENGTH CHAIN's cursor and its operation string — see step_length_unknown. A length read that
       produced unknown external input is answered by a chain of per-position outcome forks, and the machine
       SUSPENDS at every one of them (the sibling's snapshot is taken at the ask), so neither the position nor
       the string the fork is keyed by may live in a C local. The string is the header's own storage rather than
       a shared scratch buffer because two machines can be in flight at once — an outer and its delegate — and
       one formatting over the other's would key two different collections' positions to one predicate. */
    int len_probe;
    char len_op[32];
    JSValue cb_coerce[3];   /* three, because the BARE [[Set]] request carries [obj, receiver, value] — the
                               receiver is a separate operand in the spec and a builtin's Set(O,P,V,true) is the
                               one form that can leave it implicit. */
    uint8_t len_phase, spc_phase, num_phase, str_phase, get_phase, cs_phase, exec_phase, prog_phase;
    /* the [[OwnPropertyKeys]] sub-sequence's own cursor. Its OWN byte rather than get_phase's, because the one
       algorithm that needs it — Web IDL's record<> conversion — takes the key list and then reads each key,
       so the two sub-sequences are in flight in the same stage and sharing a phase would answer one at the
       other's call site. */
    uint8_t keys_phase;
    /* the [[GetOwnProperty]] sub-sequence's own cursor, for the same reason keys_phase has one: Web IDL's
       record<> conversion asks for a key's DESCRIPTOR and then READS that key, so the two are in flight in the
       same stage and a shared phase would answer one at the other's call site. */
    uint8_t desc_phase;
    /* an ARGUMENT COERCION is outstanding, so an abandon here is the spec's abrupt-completion case (take/drop's
       IfAbruptCloseIterator). It lives on the header because the teardown is what has to act on it, and the
       teardown releases this_val — the receiver the handler needs — before the machine's own fini can see it. */
    uint8_t coercing;
    /* An OPCODE-driven machine whose operation yields NOTHING — an object spread mutates its target in place, the
       way a property write does. The DONE path otherwise always leaves exactly one value on the operand stack, and
       an opcode's COMPILED stack has no slot for it (unlike a call site, whose callee slot is the result slot). */
    uint8_t discard_result;
    /* AN OUTCOME FORK — see step_fork_run. `fork_over` and `fork_op` are the request's operands and are
       BORROWED, exactly as desc_get/desc_set are: the machine holds the operand (it is one of its own
       arguments or a value on its state) and the driver READS AND RESETS them before it does anything else,
       so a request cannot leak into the next one and a SNAPSHOT taken at the fork carries neither.
       `fork_arm` is the answer and `fork_phase` says whether one is outstanding — 0 is "ask", and it is 0 in
       the sibling's snapshot ON PURPOSE: the sibling re-enters at the same point, ASKS AGAIN, and its own
       decision vector replays the arm it was forked for. That is what makes a parked and resumed sibling take
       the same arm in the next session as in this one; an arm baked into the clone would be a second, weaker
       answer to a question the vector already answers. */
    JSValueConst fork_over;
    const char  *fork_op;
    int          fork_n;
    int          fork_arm;
    uint8_t      fork_phase;
    /* the key a GETPROP sub-sequence is holding across its suspension. It is OWNED here because the driver
       BORROWS the atom the request carries, and released by the shared teardown so an abandon mid-read cannot
       leak it. JS_ATOM_NULL is zero, so a js_mallocz'd state already reads as "no read in flight". */
    JSAtom get_atom;
} JSStepHdr;


/* AN OWNERSHIP DECLARATION, AS A TYPE — JSTrampStepDef::visit's signature, and IdlStepDecl::visit's, and the
   signature of every helper record's own (a request cursor, a work block, a queue). Named because the three
   consumers below take one, and a type written out at each of them is a type that can drift at one of them. */
typedef void (*JSStepVisitFn)(JSContext *ctx, void *state, JSStepVisit *v);

/* THE FREEING CONSUMER OF A DECLARATION — release everything `visit` names, once.
 *
 * A state's `visit` is the ONE list of what it owns. Until this was exported, only quickjs.c could read it that
 * way, so every host step machine wrote a SECOND list in its teardown: the same JSValues, by hand, in another
 * function. That is precisely the pair this engine forbids — a field added to a state creates an obligation at
 * the clone and at the teardown, and nothing catches the one that is missed. It had already been missed, in
 * querySelectorAll: `visit` named the collected-matches array, the teardown named nothing, and every abandoned
 * selector walk leaked its element wrappers.
 *
 * The visitor is IDEMPOTENT — a value slot is left JS_UNDEFINED, a pointer slot NULL, an atom JS_ATOM_NULL — so
 * a state torn down through a nested declaration that has already been discharged frees nothing twice.
 * JS_StepFreeVisitor is the same consumer for a helper record whose visit takes its own typed pointer rather
 * than a `void *`; there is one implementation, spelled two ways because C cannot cast the function type
 * without lying about it. */
JS_EXTERN void JS_StepVisitFree(JSContext *ctx, JSStepVisitFn visit, void *state);
JS_EXTERN JSStepVisit *JS_StepFreeVisitor(void);

/* WHAT THE OTHER HALF OF A TEARDOWN MAY NOT TOUCH, AS A NUMBER.
 *
 * A host machine's teardown has two halves and the split between them is the whole point: the DECLARATION owns
 * every reference (discharged by JS_StepVisitFree), and the component's own `release` owns what the declaration
 * does not name — a lexbor handle, a foreign C allocation, a global flag the algorithm took and must give back
 * (§4.13.4 step 14's "regardless of whether the above steps threw", HTML §4.10.22.3 step 8's). `release` runs
 * FIRST and may READ an owned value, because those flags live on one.
 *
 * A `release` that FREES one is the second list growing back, and it is invisible either way it is written:
 * free-and-null leaves the visit's free a no-op, free-without-null makes the visit's free a double free. So the
 * teardown folds the declaration into this number before `release` and again after, and asserts they match.
 * Only the reference-holding operations are folded (`val`, `atom`, and an `array`'s elements through their own
 * element visit) — a `buf`/`shared`/`machine` pointer is exactly what `release` is allowed to own, so folding
 * those would assert the opposite of the contract. Cheap, allocation-free, and dev-only at its call sites. */
JS_EXTERN uint64_t JS_StepVisitOwnedFingerprint(JSContext *ctx, JSStepVisitFn visit, void *state);

/* Register a host component's step machine; the return value is the id JS_CFUNC_STEP_DEF names it by. The
   definition is BORROWED and must outlive the runtime — static data, as the engine's own are. A machine with no
   `visit` is refused: it could not be forked, and a concolic branch inside its callback would hand two flows
   one state. */
JS_EXTERN int JS_RegisterStepDef(JSRuntime *rt, const JSTrampStepDef *def);

/* A STEP MACHINE THAT CARRIES CAPTURED VALUES — the step analogue of JS_NewCFunctionData, and the shape a
   PROMISE REACTION has to be. A reaction knows which object it belongs to only by capture, and the work it does
   (calling the page's algorithm again, rejecting parked requests) is work only a machine may do; without this
   the two were mutually exclusive from outside quickjs.c. `stepid` is what JS_RegisterStepDef handed out; the
   machine reads what it captured with JS_StepClosureData, off the callee its header already carries. */
JS_EXTERN JSValue JS_NewStepClosure(JSContext *ctx, int stepid, int length, int data_len, JSValueConst *data);
JS_EXTERN JSValueConst JS_StepClosureData(const JSStepHdr *h, int i);

/* WHAT step() RETURNS. A machine reports one of these at every stage, and a host machine reports them for the
   same reasons the engine's own do. */
#define JS_STEP_DONE     0   /* the machine is finished; fini yields its result */
#define JS_STEP_ABRUPT (-1)  /* it threw; the completion value is live in the context */
#define JS_STEP_REQUEST  6   /* it parked on a sub-sequence's request and will be re-entered with the answer */
#define JS_STEP_CALL     3   /* it parked on a CALL of the page's code (step_call_run); same re-entry contract */
/* IT PARKED ON A [[Construct]] OF THE PAGE'S CODE (step_construct_run); same re-entry contract as a CALL.
   The code existed and was spelled `4` at every site, which is a request nobody outside quickjs.c could name
   and therefore one no HOST machine could make — and the first host algorithm that needs one is not exotic:
   DOM §4.9's "create an element" step 5.1.4.1 CONSTRUCTS the page's custom element class, synchronously,
   inside document.createElement. A number with no name is also how a driver's dispatch and a machine's return
   drift apart, which is the same reason the stage labels are a declaration rather than an integer. */
#define JS_STEP_CONSTRUCT 4
/* "I HAVE MORE WORK; PREEMPT ME IF YOU WANT." The bytecode half of this is a loop back-edge asking the flow
   control whether to yield; a machine that walks a structure of the PAGE'S SIZE — a DOM subtree, a token list,
   a document to serialise, a parse — needs the same, because otherwise it runs to completion inside one opcode
   however carefully the frames beneath it were flattened. Running no user code is NOT what makes a C body safe
   to leave un-parkable; being O(1) is, and almost nothing that walks a page is.
   Return it with no request pending; the machine is re-entered with JS_UNDEFINED, and when nobody is waiting it
   is re-entered immediately, which costs one predicted call per iteration. That is cheap enough to ask at every
   step of a walk, which is where it belongs. */
#define JS_STEP_YIELD   22
/* "MY COMPLETION IS ONE OF N FEASIBLE OUTCOMES — FORK HERE." Returned by step_fork_run; the driver decides the
   arm and snapshots the flow for the others. Never returned by a machine directly — see step_fork_run. */
#define JS_STEP_FORK    23

/* The machine's own arguments, borrowed. Out-of-range reads undefined, which is what the IDL's optional
   arguments mean at this level. */
static inline JSValueConst step_arg(const JSStepHdr *h, int i)
{
    return i < h->argc ? h->argv[i] : JS_UNDEFINED;
}

/* [[Get]] AS A REQUEST — the operation a browser component needs and the reason this header exists. Reading a
   Web IDL attribute off an object the page supplied is one accessor or Proxy trap away from being the page's
   code, so it cannot be a JS_GetPropertyStr from C; this parks the machine on the read and answers at the SAME
   call site when it is re-entered. Returns JS_STEP_REQUEST (the caller returns it), 0 once *pout is set, or -1.
   `atom` is BORROWED.

   -1 IS ALSO HOW AN ABRUPT COMPLETION ARRIVES — for this request and for every KEYED one below it (the write,
   the define, the delete, the membership test, the own-keys and own-descriptor reads). The request ENDS at the
   call site that parked on it, exactly as a normal completion ends it — the cursor rewinds, the key is
   released — and returns -1 with the completion live in the context. A machine that propagates writes what it
   already writes (`if (r < 0) return JS_STEP_ABRUPT;`); a machine whose definition declares `catches_abrupt`
   branches there instead and says what its algorithm does with the throw. The CALL and CONSTRUCT requests
   still report theirs as a JS_EXCEPTION in `*pout` — the encoding JS_Call itself uses — so the two halves of
   the request layer do not yet answer the same way; moving them onto -1 is a change at ninety-nine call sites
   and belongs in its own diff, not smuggled into this one. Neither encoding can go unnoticed: a machine that
   ignores an abrupt delivery and asks for anything with that throw still live aborts at the driver's one
   convergence point, naming its algorithm, its stage and the step code it asked for. */
JS_EXTERN int step_getprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in,
                               JSValue *pout, JSValue **out_cb, int *out_argc);

/* A NUMERIC COERCION AS A REQUEST — the other half of what a browser component needs, and the half that was
   missing. Web IDL's integer types (`[EnforceRange] unsigned long long milliseconds`, `long`, `unsigned long`)
   are ToNumber on whatever the page passed, so `AbortSignal.timeout({valueOf(){ for(;;){} }})` is the page's
   code and cannot be a JS_ToInt64Sat from C. With only the property read exported, a component's only honest
   options were to abort on an object argument or to run the coercion off the chain; this makes it a request
   like any other. Returns JS_STEP_REQUEST (the caller returns it), 0 once *pres is set, or -1. */
JS_EXTERN int step_toint64_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, int64_t *pres,
                               JSValue **out_cb, int *out_argc);

/* THE SAME REQUEST STOPPING AT THE NUMBER. Web IDL's integer conversions are NOT ToIntegerOrInfinity: `long` is
   ToNumber then a modulo 2^32 and `[Clamp] long long` is ToNumber then a round-half-to-EVEN, and a saturating
   int64 has thrown away what each of those needs before it is one. The coercion is the part that runs the page's
   code; the arithmetic after it is the caller's type. */
JS_EXTERN int step_todouble_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, double *pres,
                                JSValue **out_cb, int *out_argc);

/* A CALL AS A REQUEST — see the definition in quickjs.c. A browser component that must RUN the page's code and
   then continue (dispatchEvent walking a listener list, §2.9's synchronous dispatch) cannot JS_Call from C:
   that is the drive-to-completion the engine aborts on. `phase` and `cb` are the MACHINE's own — a host machine
   holds a call across several of its stages, and the buffer must be in its `visit` for a fork to copy it.
   `cb` is 2 + argc slots, [this, func, args...]; this dups in and releases out, so the machine owns nothing it
   has to remember. Returns 3 (the caller returns it), or 0 once *pout is the call's result. */
JS_EXTERN int step_call_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst func,
                            JSValueConst this_val, int argc, JSValueConst *argv, JSValue in, JSValue *pout,
                            JSValue **out_cb, int *out_argc);

/* A [[Construct]] AS A REQUEST — step_call_run's sibling, and the operation a browser component needs to run a
   page's CLASS. `document.createElement('x-app')` performs DOM §4.9 step 5.1.4.1's "constructing C with no
   arguments" SYNCHRONOUSLY, and that constructor is the page's code with loops, awaits and DOM mutations in
   it; JS_CallConstructor from C is the drive-to-completion the engine aborts on, and the class's own `super()`
   would reach a C entry with no flow base under it.
   Same contract as the call request in every respect but the buffer SHAPE, which is what the driver's
   JS_STEP_CONSTRUCT arm reads: `cb` is 1 + argc slots, [ctor, args...] — there is no receiver, because a
   construct makes one. NEW TARGET is the constructor itself, which is what every algorithm that states
   "Construct(C)" means; a machine needing a distinct one sets `h->ctor_ntgt` (Reflect.construct's operand).
     0 = *pout is the constructed object, JS_STEP_CONSTRUCT = the caller must return that step code, and a
   throw ABANDONS the machine unless its definition declares that it catches an abrupt request result. */
JS_EXTERN int step_construct_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst ctor,
                                 int argc, JSValueConst *argv, JSValue in, JSValue *pout,
                                 JSValue **out_cb, int *out_argc);

/* PASS THE BUFFER THROUGH THIS, and pass it the ARRAY. The capacity is then the array's own size rather than a
   number written beside it that a later argument can outgrow — which is exactly how a two-argument call into a
   three-slot buffer scribbled over the field next door. A machine that FORWARDS someone else's buffer takes
   `(JSValue *cb, int cb_cap)` and forwards both; handing this macro that pointer yields a capacity of zero, so
   the mistake is a DCHECK on the first call rather than a corrupted neighbour. */
#define STEP_CB_SLOTS(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))
#define STEP_CB(arr) (arr), STEP_CB_SLOTS(arr)

/* THE SAME DERIVATION, FOR THE THREE LOOPS EVERY OWNER OF A REQUEST BUFFER WRITES — the `init` that undefines
   its slots, the `visit` that walks them for a fork, and the `fini` that releases them. Each of those was a
   literal count beside an array declared with a literal size, so a buffer that grew had FOUR places to grow in
   and the ones that were missed did not fail where they were: a visit one slot short leaves a live value the
   fork never dups, and a fini one slot short leaks it. Derive the count from the array and there is one. */
#define STEP_CB_FOREACH(arr, i) for ((i) = 0; (i) < STEP_CB_SLOTS(arr); (i)++)

/* LEAVING A STAGE WITH A REQUEST STILL IN FLIGHT IS THE BUG THIS EXISTS FOR — every stage transition in a
 * machine that owns a request buffer goes through here.
 *
 * A request is TWO-PHASE: the stage that issues it is re-entered with its cursor at 1 and MUST reach the SAME
 * step_call_run / step_construct_run / step_getprop_run to collect the answer, because the cursor is what tells
 * that call site "you are the resume, take the value" rather than "ask". A stage that re-evaluates its decision
 * on the way back in can decide differently — Streams §4.2.4's shutdown did exactly that, since ISSUING the
 * close is what made WritableStreamCloseQueuedOrInFlight true, so the resume took the "already closing" branch
 * and walked away from its own call. The cursor stayed at 1, the NEXT stage's request read it as a resume and
 * took the abandoned call's result as its own, and `writer.releaseLock()` was never called at all: the pipe
 * fulfilled with the destination still locked. Nothing said so, because the value a stage received was a REAL
 * value — the other request's. quickjs.c's step_getprop_done DFAILs on the same event one step LATER, and only
 * when the two call sites happen to name DIFFERENT keys; when they name the same one it says nothing.
 *
 * The rule is structural, which is why it is a primitive and not a defence one component wrote for itself: a
 * stage's DECISION is made once, with every cursor at rest, and a stage that holds a request does nothing but
 * hold it. Aborting AT the transition names the stage that walked away, rather than the innocent stage three
 * transitions later whose request silently answered without asking.
 *
 * THE CURSOR LIST IS NULL-TERMINATED AND NAMES EVERY SUB-SEQUENCE THIS MACHINE CAN HAVE IN FLIGHT — its own
 * call/construct phase bytes, a sub-record's (`&s->w.phase`), and, for a machine whose requests are the keyed
 * ones, the header's own `&h->get_phase`. Listing them is the machine's statement of what it owns, in the same
 * spirit as `visit`: a machine that holds no request buffer at all lists none and assigns its stage directly,
 * because there is no cursor to leave behind and a fabricated one would assert nothing. The list is where the
 * cursor goes the day such a machine grows one.
 *
 * `dst` IS THE STAGE LVALUE, NOT A HEADER, because the invariant is about the cursor and not about who owns the
 * stage: a WORK RECORD a machine suspends inside (AbortSignalWork's §3.2 walk, report_exception's) carries a
 * stage and a phase of its own and needs exactly this over them.
 *
 * A MACRO RATHER THAN AN INLINE FUNCTION, for the reason the whole assert exists: DCHECK stamps the file and
 * line it is WRITTEN at, so a function here would report this header at every abort and say nothing about which
 * transition walked away. Expanding at the call site is what makes the message the answer. DCHECK itself is
 * therefore the caller's — engine/host/check.h in a host component, quickjs-check.h in the engine — which is
 * also why this header must not include either of them. */
#define STEP_GOTO(dst, to, ...) do {                                                                    \
        const uint8_t *const step_goto_rest_[] = { __VA_ARGS__ };                                       \
        int step_goto_i_;                                                                               \
        for (step_goto_i_ = 0; step_goto_rest_[step_goto_i_] != NULL; step_goto_i_++)                   \
            DCHECK(*step_goto_rest_[step_goto_i_] == 0,                                                 \
                   "a stage was left with a sub-sequence's request still in flight — the next stage's " \
                   "request will collect this one's answer");                                           \
        (dst) = (to);                                                                                   \
    } while (0)

/* A WELL-KNOWN SYMBOL'S ATOM. A host machine can read any NAMED property (step_getprop_run takes an atom, and
   JS_NewAtom makes one from a string), but a symbol-keyed one it could not name at all — and @@iterator is the
   discriminator Web IDL uses for every `sequence or record` union. Headers' fill is the first to need it: the
   spec picks the sequence arm when the init is ITERABLE, and without this the closest available test was
   JS_IsArray, which is narrower — `new Headers(new Map(...))` is iterable, is not an array, and would have
   taken the record arm and produced an EMPTY header list. These are permanent atoms, so the answer needs
   neither a dup nor a free. */
typedef enum {
    JS_WKS_ITERATOR = 0,
    JS_WKS_ASYNC_ITERATOR,
    JS_WKS_TO_STRING_TAG,
} JSWellKnownSymbol;
JS_EXTERN JSAtom JS_WellKnownSymbolAtom(JSWellKnownSymbol which);

/* WEB IDL §3.2.11's `USVString`: a DOMString whose UNPAIRED SURROGATES have each been replaced by U+FFFD.
   That replacement is the whole of what makes the type different from a DOMString, and it cannot be done from
   outside the engine — a host sees only the UTF-8 the C-string conversion produces, where an unpaired
   surrogate has already been written out in whatever form that conversion chose. Every URL member takes
   USVStrings, and wpt asserts the replacement directly. `str` is OWNED; a string with no unpaired surrogate is
   returned unchanged, so the common case allocates nothing. */
JS_EXTERN JSValue JS_ToScalarValueString(JSContext *ctx, JSValue str);

/* A DIAGNOSTIC string for a value, built WITHOUT invoking the page's `toString`. JS_ToCString does invoke it,
   and at a C entry there is no flow to run it on — which since the coercion methods became step machines is
   the state the engine's own backstop names ("route this site to the ToPrimitive trampoline"). That backstop
   is right, and this is the answer for the one shape of caller that cannot take its advice: a HOST reporting
   what went wrong must not depend on the code that went wrong. Every C embedder reaching it derives the same
   fallback, so the engine owns it once instead: an object is its `name`/`message`, falling back to its
   CONSTRUCTOR's name — test262's Test262Error carries only `message` and a custom `toString`, which is exactly
   the page code this must not run — and anything else is its class via JS_ToObjectString.
   The result is malloc'd when *powned is set and a JS C-string otherwise, which is the only reason the caller
   has to know which; JS_DiagFreeCString takes both and releases the right one. */
JS_EXTERN const char *JS_DiagCString(JSContext *ctx, JSValueConst v, char **powned);
JS_EXTERN void JS_DiagFreeCString(JSContext *ctx, const char *s, char *owned);

/* WEB IDL'S `BufferSource` AS ONE READ. The type is `(ArrayBuffer or ArrayBufferView)` and ArrayBufferView is
   `(Int8Array or ... or DataView)` — so a host component reading a BodyInit has to accept a DataView, which
   JS_GetTypedArrayBuffer refuses because it is not a typed array even though it carries the same
   JSTypedArray. The alternative from outside the engine is reading `buffer`/`byteOffset`/`byteLength` as
   PROPERTIES, which are accessors — page-visible, patchable, and a C-driven getter call the flow design has no
   driver for. Returns the underlying ArrayBuffer (owned) with the view's window, or an exception.
   The bytes come from JS_GetArrayBuffer on the result. */
JS_EXTERN JSValue JS_GetArrayBufferView(JSContext *ctx, JSValueConst obj, size_t *pbyte_offset,
                                        size_t *pbyte_length);

/* %IteratorPrototype%. Web IDL §3.7.10 states that the ITERATOR PROTOTYPE OBJECT of an `iterable<>` interface
   has %IteratorPrototype% as its [[Prototype]] — that inheritance is what gives `headers.keys()` the whole
   iterator-helper surface (`@@iterator` returning this, `take`, `drop`, `map`) without the component defining
   one member of it. A host component cannot reach the intrinsic from outside: it is neither a global nor
   reachable by name, and the only script-level route is
   `Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()))` — which is exactly the walk the WPT
   assertion performs, so deriving it that way from C would be checking the engine against itself. Dup'd. */
JS_EXTERN JSValue JS_GetIteratorPrototype(JSContext *ctx);

/* %AsyncIteratorPrototype%, for the same reason and with less of a workaround available: Web IDL §3.7.11 makes
   it the [[Prototype]] of every `async iterable<>` interface's iterator prototype object, and unlike
   %IteratorPrototype% there is no script-level walk to it at all from a host that has no async generator to
   hand. Dup'd. */
JS_EXTERN JSValue JS_GetAsyncIteratorPrototype(JSContext *ctx);

/* 27.1.4.1 CreateAsyncFromSyncIterator — the ONE step of GetIterator(obj, ASYNC) a host cannot perform itself.
   The @@asyncIterator read, the fallback @@iterator read and the method call are all requests this header
   already exports; the wrapper is an intrinsic whose `next` awaits the sync result's VALUE, which is what makes
   an iterable of promises yield what they resolve to. `sync_iter` and `next_method` are CONSUMED; *pnext is the
   wrapper's `next`, called exactly as the sync one would be. */
JS_EXTERN JSValue JS_NewAsyncFromSyncIterator(JSContext *ctx, JSValue sync_iter, JSValue next_method,
                                              JSValue *pnext);

/* [[OwnPropertyKeys]] AS A REQUEST — what a Web IDL `record<K, V>` argument is made of, and the last operation a
   browser component needed that it could not perform. `fetch(u, {headers: {...}})` converts that bag by taking
   its own keys and then reading each one, and on a Proxy the key list IS the page's `ownKeys` trap — so taking
   it with JS_GetOwnPropertyNames from C is the drive-to-completion every other C-side key walk was converted
   away from, and adding a third one would move the build's own C-enum-only-walk ratchet backwards.
   The answer is the ARRAY the spec's CreateArrayFromList builds, of Strings and Symbols 10.5.11's invariant has
   already validated — so the caller may walk it with plain index reads. Returns 11 (the caller returns it), 0
   once *pout is the array, or -1. */
JS_EXTERN int step_ownkeys_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSValue in, JSValue *pout,
                               JSValue **out_cb, int *out_argc);

/* [[GetOwnProperty]] AS A REQUEST — the other half of a `record<K, V>` conversion, and the half that decides
   whether a key counts at all. Web IDL §es-to-record step 5.1 asks for each key's DESCRIPTOR and step 5.2 keeps
   the key only if it is present and ENUMERABLE; on a Proxy that is the page's `getOwnPropertyDescriptor` trap.
   Skipping it does not merely lose the trap — it silently includes non-enumerable properties, and wpt's
   headers-record pins the operation SEQUENCE (get @@iterator, ownKeys, then getOwnPropertyDescriptor and get per
   key), so the omission is observable as a count.
   The answer is the descriptor OBJECT the engine builds, or undefined when the property is gone between the
   key snapshot and here — reading `enumerable` off it runs none of the page's code. `atom` is BORROWED.
   Returns 12 (the caller returns it), 0 once *pout is set, or -1. */
JS_EXTERN int step_getownprop_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom atom, JSValue in,
                                  JSValue *pout, JSValue **out_cb, int *out_argc);

/* AN OUTCOME FORK AS A REQUEST — the primitive that lets a step machine fork, and the one operation for which
   there was previously no answer but an abort.
 *
 * A bytecode branch forks because the interpreter has everything a fork needs at an OP_if: a condition, a pc
 * to resume the sibling at, a frame to snapshot. A C builtin whose result DEPENDS ON UNKNOWN INPUT has the
 * same decision and none of those things — `JSON.parse(text)` over unknown text completes with a value or with
 * a SyntaxError, both feasible, and a builtin that picks one has DELETED the other arm, which is exactly the
 * arm a `catch` reaches and a sink lives behind. So the machine declares the fork instead, and the driver
 * snapshots the whole flow AT the machine (the machine's own state included, cloned through its `visit`) so
 * the sibling RESUMES here with the other outcome. It is a snapshot, never a replay.
 *
 * `over` is the unknown operand the outcome depends on and `op` names the operation; together they are the
 * constraint key, which the SOLVER builds — the engine states the question, never the policy. `n` is how many
 * completions the machine declares feasible; which one means "threw" is a fact about the algorithm and stays
 * the machine's own, with ONE rule on the numbering: OUTCOME 0 IS THE ONE A RUN WITH NO FORKING POLICY TAKES.
 * A machine therefore numbers its ordinary completion first, because the @S candidate re-fire runs ONE
 * concrete path and must not be diverted down an exceptional arm on its way to the sink.
 *
 * Returns JS_STEP_FORK (the caller returns it — the state must be complete-or-empty at that point, since the
 * SIBLING'S SNAPSHOT IS TAKEN THERE), or 0 once *parm is this flow's outcome. Both a park and a cross-session
 * resume land back on the ask, which re-derives the same arm from the flow's decision vector. */
JS_EXTERN int step_fork_run(JSContext *ctx, JSStepHdr *h, JSValueConst over, const char *op, int n, int *parm);

/* ToString AS A REQUEST — the coercion nearly every Web IDL argument actually is. `DOMString type`,
   `DOMString name`, `DOMString selector`: each is ToString on whatever the page passed, so
   `el.addEventListener({toString(){ for(;;){} }}, f)` is the page's loop. Without this a component's only
   honest move was JS_ToCString from C, which in this engine does not quietly misbehave — it reaches
   JS_ToPrimitiveFree's DFAIL and aborts, naming the site — but aborting is where a capability is missing, not
   where it is built. Returns JS_STEP_REQUEST (the caller returns it), 0 once *pout is set, or -1. */
JS_EXTERN int step_tostring_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, JSValue *pout,
                                JSValue **out_cb, int *out_argc);

#endif
