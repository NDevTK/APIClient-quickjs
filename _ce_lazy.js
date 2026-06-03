// Lazy custom-element connectedCallback fetch — the github include-fragment /
// MS Learn breadcrumb shape: a CE whose connectedCallback registers an
// IntersectionObserver and fetches only when the (opaque) entry says it's
// intersecting. Forced exec must: upgrade the JS-created element, run
// connectedCallback, fire the observer callback, FORK the opaque isIntersecting
// gate, and take the fetch arm. EXPECT: @H GET /api/lazy-fragment.
class LazyFrag extends HTMLElement {
  connectedCallback() {
    var self = this;
    var io = new IntersectionObserver(function (entries) {
      if (entries[0].isIntersecting) fetch(self.getAttribute("src"));
    });
    io.observe(this);
  }
}
customElements.define("lazy-frag", LazyFrag);
var el = document.createElement("lazy-frag");
el.setAttribute("src", "/api/lazy-fragment");
document.body.appendChild(el);
