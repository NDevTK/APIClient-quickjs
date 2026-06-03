// JS-INJECTED custom element — the MS Learn breadcrumb / React-rendered shape:
// the CE is NOT present at parse; a render function creates+appends it later.
// Forced exec must drive that residue render function so the element upgrades,
// connectedCallback runs, and its fetch fires. EXPECT (--deep): the grind drives
// __renderFrag → @H GET /api/render-injected.
class Frag2 extends HTMLElement {
  connectedCallback() { fetch(this.getAttribute("src")); }
}
customElements.define("frag-2", Frag2);
globalThis.__renderFrag = function () {
  var e = document.createElement("frag-2");
  e.setAttribute("src", "/api/render-injected");
  document.body.appendChild(e);
};
