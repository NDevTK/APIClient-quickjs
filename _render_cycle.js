// Render-cycle reaching reproduction: a CLIENT-rendered SPA whose components
// are NOT in the SSR HTML — the boot CREATES the component tree dynamically
// (document.createElement of custom elements), and each component's
// connectedCallback fetches. This is the reddit/github pattern where the BFS
// reaches eps:0 because the create-code sits behind the SPA boot. _ce_construct
// proves a top-level createElement'd CE connects+fetches; THIS asks whether the
// J-Force __hostDrive drives the boot GLOBAL that does the rendering.
//
// EXPECT (correct): GET /api/app-init AND GET /api/feed/42 — __hostDrive
// force-invokes __bootSPA (a bundle-introduced global), which createElements
// app-root + feed-item, whose connectedCallbacks fire and fetch.
class AppRoot extends HTMLElement {
  connectedCallback() { fetch("https://api.test.com/app-init"); }
}
customElements.define("app-root", AppRoot);

class FeedItem extends HTMLElement {
  connectedCallback() { fetch(`/api/feed/${this.getAttribute("item-id") || "x"}`); }
}
customElements.define("feed-item", FeedItem);

// The SPA's client render — a bundle global the J-Force driver must invoke.
// Nothing calls it at module top level (a real SPA calls it from a
// DOMContentLoaded/hydration path); forced exec must DRIVE it to reach the
// component creation that connectedCallback's fetches hang behind.
globalThis.__bootSPA = function () {
  var root = document.createElement("app-root");
  document.body.appendChild(root);
  var item = document.createElement("feed-item");
  item.setAttribute("item-id", "42");
  root.appendChild(item);
};
