// Render-cycle test: a custom element whose connectedCallback fetches, but the
// element is CREATED by an orphan function that itself reaches NO host edge
// (createElement+appendChild only) — models React/Catalyst rendering an injected
// element after hydration. The deep grind drives @T functions (those with a
// host-edge get_var/get_field); a pure CE-creator has none, so it may be SKIPPED
// → the element is never created → its connectedCallback fetch never fires.
// If GET /api/rendered_frag is NOT learned, the render-cycle gap is confirmed:
// the fix is to extend the driven set to CE-creating functions.
customElements.define('rnd-frag', class extends HTMLElement {
  connectedCallback() {
    if (!this.isConnected) return;
    var s = this.getAttribute('src');
    if (s) fetch(s, { headers: { Accept: 'text/fragment+html' } });
  }
});
function __renderInjected() {
  var el = document.createElement('rnd-frag');
  el.setAttribute('src', '/api/rendered_frag');
  document.body.appendChild(el);
}
// __renderInjected is never called at top level — only the deep grind can drive it.
globalThis.__keepRef = __renderInjected;
