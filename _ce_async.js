// Mirrors github include-fragment's eager-load chain: connectedCallback (guarded
// by isConnected) calls an async loader that AWAITS setTimeout(0) (the lib's
// loadstart-event dispatcher does `await new Promise(r=>setTimeout(r,0))`) BEFORE
// fetching. Tests whether that timer-gated async fetch is driven (TQ drained in
// the upgrade phase). Expect GREEN: GET /api/async_test.
customElements.define('async-frag', class extends HTMLElement {
  connectedCallback() { if (this.isConnected) this.load(); }
  async load() {
    await new Promise(function (r) { setTimeout(r, 0); });
    fetch(this.getAttribute('src'), { headers: { Accept: 'text/fragment+html' } });
  }
});
