// Models github protobuf es(): recursion gated by `key in t`, walking t.parent.
// With OP_in opaque-infectious, `if("kind" in t)` forks (opaque t → opaque
// membership) and the loop-revisit fixpoint bounds the recursion. Without it,
// "kind" in opaque → concrete false → recurses up opaque .parent forever →
// stack overflow. MUST CONVERGE + emit @H.
function es(e, t) {
  if ("kind" in t) {            // opaque t → must be opaque → forks
    return fetch("/api/es_kind");
  }
  return es(e, t.parent);        // walk up opaque parent chain
}
function Orphan() { es("x", this.root); }   // opaque this → this.root opaque
globalThis.__keep = [Orphan];
