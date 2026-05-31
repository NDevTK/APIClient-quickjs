// Cold-instance class constructor gate — models learn.microsoft.com's search
// component (`this._input=$0e(async()=>{await this.fetch()})`): a class whose
// methods fetch via instance state (`this.url`), never `new`-instantiated by
// the page during the grind.
//
// __feDriveStatic drives every @T orphan with a plain JS_Call + opaque `this`.
// A class constructor begins with OP_check_ctor, which THROWS "must be invoked
// with 'new'" on a plain call — so the constructor drive is SILENT (no @H), no
// real instance is created, and `lookup()`/the arrow driven separately with
// opaque `this` resolve `this.url` to OPAQUE → the endpoint is learned only as
// an opaque shape, never the concrete "/api/box-search".
//
// FIX: drive a class-constructor orphan with JS_CallConstructor (real `this`),
// so the body runs, instance state is concrete, and js_closure2 captures the
// arrow's REAL closure (qjs_deep_capture_inst) → its re-drive resolves
// this.url CONCRETELY → @H GET /api/box-search (not an opaque shape).
//
// Run: ./qjs.exe --fe-deep-grind hostedge.js _opqctor.js
// EXPECT (after fix): @H fetch with CONCRETE "/api/box-search" (today: only an
// opaque-url @H from lookup()'s direct opaque-this drive).
class Box {
  constructor() {
    this.url = "/api/box-search";        // concrete instance field
    this._cb = () => this.lookup();      // arrow captures real `this`
  }
  lookup() { return fetch(this.url); }   // real fetch via instance state
}
globalThis.__BoxOrphan = Box;            // registered as a value, never new'd
