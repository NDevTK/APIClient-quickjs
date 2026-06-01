// instanceof gating a type-dispatch recursion over an opaque chain (mirrors
// github 43540 `k`/type-walk). With instanceof opaque-infectious, `if(x
// instanceof T)` forks and the recursion terminates; without it, concrete
// false → recurse up opaque .inner forever → overflow. MUST CONVERGE + @H.
function T() {}
function conv(x) {
  if (x instanceof T) {        // opaque x → must be opaque → forks
    return fetch("/api/instof_match");
  }
  return conv(x.inner);         // walk up opaque chain
}
function Orphan() { conv(this.root); }
globalThis.__keep = [Orphan];
