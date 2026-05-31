// KNOWN-OPEN FRONTIER (not yet solved) — cold-instance with an INERT
// constructor. `doSearch` fetches via instance state `this.base`; it is driven
// as its own @T orphan with opaque `this` → fetch(opaque) → @H with an OPAQUE
// url, never the concrete "/api/method-search".
//
// Why the constructor-new fix ([[project_drive_class_ctor_with_new]]) does NOT
// reach this: the `Api` constructor is INERT — `this.base="/api/method-search"`
// has no host edge and creates no host-reaching closure — so it is NOT in the
// @T residue and is NEVER driven. No constructor drive ⇒ no real instance ⇒
// nothing to give `doSearch` a real `this`. (Contrast _opqctor, whose
// constructor creates a host-reaching arrow → IS @T → new-driven → arrow
// captured with real this → concrete.) A post-construct prototype-method drive
// was tried and REVERTED: it only fires for constructors already in the residue
// (zero help here + zero new concrete @H on _idxdocs).
//
// To solve: the @T scan must include a class CONSTRUCTOR when any of its
// prototype methods is @T (link the @T method back to its class), so the
// constructor is new-driven and its methods then drive with the real instance.
// That method→constructor association is the hard part (a plain method's
// func_obj has no home_object unless it uses `super`). Forced-exec, not a
// heuristic. See [[project_net_orphan_promotion_gap]].
//
// Run: ./qjs.exe --fe-deep-grind hostedge.js _opqmethod.js  (today: opaque @H)
class Api {
  constructor() { this.base = "/api/method-search"; }   // concrete instance field
  doSearch() { return fetch(this.base); }                // prototype method, never called
}
globalThis.__ApiOrphan = Api;                            // registered, never new'd
