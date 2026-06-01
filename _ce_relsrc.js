// Reproduces @github/include-fragment-element's EAGER connectedCallback fetch
// path EXACTLY, to isolate why github's SSR used_by_list/sidebar_partial (eager
// include-fragments) aren't learned while the simpler _ce_construct/_ce_upgrade
// gates (which fetch getAttribute('src') directly) are. The real library does
// NOT fetch the raw attribute — its `src` GETTER resolves relative→absolute by
// creating an <a> via this.ownerDocument.createElement('a'), assigning .href,
// and reading .href back, then connectedCallback guards `if (this.src) fetch()`.
// If our host model's ownerDocument.createElement / <a>.href resolution throws
// or returns empty, the guard skips the fetch and the endpoint is silently lost
// — exactly the live github gap. Expected GREEN: GET /api/rel_used_by_list.
customElements.define('rel-fragment', class extends HTMLElement {
  get src() {
    const src = this.getAttribute('src');
    if (src) {
      const link = this.ownerDocument.createElement('a');
      link.href = src;
      return link.href;
    } else {
      return '';
    }
  }
  connectedCallback() {
    if (this.src) {
      fetch(this.src, { method: 'GET', headers: { Accept: 'text/fragment+html' } });
    }
  }
});
