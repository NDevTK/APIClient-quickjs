// Render-cycle, HYDRATION-GATED variant (the real reddit/github shape): the
// client render is NOT a bare global — it's gated behind a hydration check on
// page-config state, and triggered from a DOMContentLoaded handler (not called
// at module top level). _render_cycle proved __hostDrive drives a bare boot
// global; this asks whether forced exec reaches a render that is (a) behind a
// concrete-looking config gate and (b) fired from an event handler.
//
// EXPECT (correct): GET /api/hydrated-feed — the gate's predicate reads
// opaque host state (so it forks), the DOMContentLoaded handler is driven by
// __hostFlush's lifecycle dispatch, and the created component's connectedCallback
// fetches. If it DOESN'T fire, the failing layer is named (gate polarity,
// handler dispatch, or create-in-handler).
class HydratedFeed extends HTMLElement {
  connectedCallback() { fetch("/api/hydrated-feed?sort=" + (this.getAttribute("sort") || "hot")); }
}
customElements.define("hydrated-feed", HydratedFeed);

function renderApp() {
  var el = document.createElement("hydrated-feed");
  el.setAttribute("sort", "best");
  document.body.appendChild(el);
}

// Hydration gate: reads server-injected config (a global an SSR inline script
// would set; here it's read off window so it's host-opaque under forced exec →
// the branch forks rather than being concrete-dead). The render is wired to
// DOMContentLoaded, exactly like a real SPA bootstrap — never called inline.
document.addEventListener("DOMContentLoaded", function () {
  var cfg = window.__PAGE_CONFIG__;
  if (cfg && cfg.hydrate) {
    renderApp();
  }
});
