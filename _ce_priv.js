// github's include-fragment connectedCallback calls a #PRIVATE method
// (`f(this,u,"m",h).call(this)`), and the loader awaits via more #private
// methods. Tests whether a #private method invoked from connectedCallback on an
// upgraded SSR node runs + reaches fetch. If this FAILS, #private-method
// dispatch under forced exec / CE upgrade is a general gap (ubiquitous in modern
// bundles). Expect GREEN: GET /api/priv_test.
customElements.define('priv-frag', class extends HTMLElement {
  #load() { fetch(this.getAttribute('src'), { headers: { Accept: 'text/fragment+html' } }); }
  connectedCallback() { if (this.isConnected) this.#load(); }
});
