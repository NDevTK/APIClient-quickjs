// Real-state driving repro (the reddit/gitlab GraphQL-operation shape): a
// CLICK handler on a real element builds a POST body from the element's OWN
// data attributes. __hostDrive force-invokes the handler, but COLD (opaque
// `this`) → this.dataset.* is opaque → opaque operation body. Dispatching the
// click on the REAL element gives this = the real button → concrete dataset →
// concrete operationName/variables (the moat's value depth).
//
// EXPECT (the goal): POST /api/graphql with body
// {operationName:"upvote", variables:{id:"t3_abc"}} — concrete, from the real
// element. CURRENT (cold force-invoke): operationName/id opaque.
var btn = document.createElement("button");
btn.setAttribute("data-action", "upvote");
btn.setAttribute("data-id", "t3_abc");
document.body.appendChild(btn);

btn.addEventListener("click", function () {
  fetch("/api/graphql", {
    method: "POST",
    body: JSON.stringify({
      operationName: this.dataset ? this.dataset.action : (this.getAttribute && this.getAttribute("data-action")),
      variables: { id: this.dataset ? this.dataset.id : (this.getAttribute && this.getAttribute("data-id")) },
    }),
  });
});
