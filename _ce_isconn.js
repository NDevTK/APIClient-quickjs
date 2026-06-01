// Mirrors github include-fragment's connectedCallback guard EXACTLY:
// the eager load fires only `if (this.isConnected)`. Element.isConnected was
// defined only on `document`, so an upgraded SSR node returned undefined and
// the fetch was skipped. With Element.prototype.isConnected (parentNode walk),
// the upgraded SSR <isc-frag> must report connected → fetch fires.
customElements.define('isc-frag', class extends HTMLElement {
  connectedCallback() {
    if (this.isConnected) {
      fetch(this.getAttribute('src'), { headers: { Accept: 'text/fragment+html' } });
    }
  }
});
