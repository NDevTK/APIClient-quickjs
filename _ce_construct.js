// Polarity gate — custom-element construction + connectedCallback fetch.
// Regression guard for the fix where qjs_dom's prelude I() must NOT clobber
// the dom_ctor-backed HTMLElement constructor: `class X extends HTMLElement`
// + super() has to reach dom_ctor so the element is a REAL Lexbor node
// (tagName set), and the driver then fires connectedCallback against it so a
// deferred-fragment loader (the include-fragment pattern github/MS/etc. use)
// fetches its CONCRETE src. Expected: @H fetch GET /api/used_by_list.
class IncludeFragment extends HTMLElement {
  connectedCallback() { fetch(this.getAttribute("src")); }
}
customElements.define("include-fragment", IncludeFragment);
var f = document.createElement("include-fragment");
f.setAttribute("src", "/api/used_by_list");
document.body.appendChild(f);
