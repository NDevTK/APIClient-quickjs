// Tests whether `childEl instanceof HTMLElement` holds for a real SSR child
// element in our DOM — github's image-cropper (and much CE code) guards
// `if(!(t instanceof HTMLElement)) return;` before branding #private fields, so
// a false instanceof skips setup and a later #private access throws "Cannot read
// private member" (caught → endpoint lost). Learns /api/iof_OK if instanceof
// works, /api/iof_FAIL if it doesn't.
customElements.define('iof-el', class extends HTMLElement {
  connectedCallback() {
    if (!this.isConnected) return;
    var t = this.querySelector('span');
    if (t instanceof HTMLElement) fetch('/api/iof_OK');
    else fetch('/api/iof_FAIL' + (t == null ? '_null' : '_notHTMLElement'));
  }
});
