// SSR-upgrade gate: <include-fragment src> already in parsed HTML when
// customElements.define runs → define() must upgrade the existing element and
// fire connectedCallback's fetch. The real github/MS SSR-fragment case.
// Expected: @H GET /api/ssr_fragment. Run with --deep (drives the epilogue).
class IncludeFragment extends HTMLElement {
  connectedCallback() { fetch(this.getAttribute("src")); }
}
customElements.define("include-fragment", IncludeFragment);
