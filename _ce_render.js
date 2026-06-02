// Render-cycle-in-deep-context gate. VERIFIED GREEN (2026-06-02, --deep): the
// residue grind drives EVERY un-fired function (incl. no-host-edge wrapper-
// callers), so __renderInjected IS driven (GET /api/creator_ran proves it), and
// the DEEP-GRIND context (g_deep_ctx) drives the full createElement → setAttribute
// → appendChild → CE-upgrade → connectedCallback → fetch chain to a CONCRETE src
// (GET /api/rendered_frag). So the earlier "the CE-upgrade chain may not fire in
// g_deep_ctx" hypothesis is DISPROVEN — the deep runtime's QuickJS↔Lexbor DOM is
// wired the same as boot. A regression here = the deep ctx lost CE-upgrade or
// connectedCallback driving. EXPECT (--deep): GET /api/creator_ran AND
// GET /api/rendered_frag, both concrete, 0 phantoms.
customElements.define('rnd-frag', class extends HTMLElement {
  connectedCallback() {
    if (!this.isConnected) return;
    var s = this.getAttribute('src');
    if (s) fetch(s, { headers: { Accept: 'text/fragment+html' } });
  }
});
function __renderInjected() {
  fetch('/api/creator_ran');                       // proves __renderInjected was driven
  var el = document.createElement('rnd-frag');
  el.setAttribute('src', '/api/rendered_frag');    // concrete src
  document.body.appendChild(el);                   // connect → should upgrade → connectedCallback → fetch
}
globalThis.__keepRef = __renderInjected;
