// Multi-layer GraphQL client value-depth gate (the REAL github fetchGraphQL
// shape, confirmed by reading _ghbundle.js: a generic client fn takes the
// operation as PARAMETERS — method, persisted-query id, operationName — and
// builds the body/URL; the per-operation call site passes LITERAL descriptors
// + prop-derived variables). This tests whether a literal operation descriptor
// survives the CROSS-FUNCTION indirection to the captured body under a cold
// orphan drive (the deep grind drives FeedQuery with opaque props; it calls the
// client with concrete literal id/name + opaque variables).
//
// EXPECT (the goal): POST /_graphql with body id="abc123hash",
// operationName="SubredditFeed", variables.name=<opaque>. If the literals come
// through, the multi-layer indirection does NOT lose the operation descriptor,
// so a real SPA's empty {} body is a MORE-opaque construction (operation itself
// opaque at the call site / registry-looked-up / whole-query dynamic) — NOT the
// indirection. If they are lost, cross-function literal threading is the gap.
function gqlClient(method, opId, opName, variables) {
  return fetch("/_graphql", {
    method: method,
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ id: opId, operationName: opName, variables: variables }),
  });
}
function FeedQuery(props) {
  return gqlClient("POST", "abc123hash", "SubredditFeed", { name: props.subreddit });
}
globalThis.__keepGql = FeedQuery;
