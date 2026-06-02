// Conditional-spread body DIRECTLY in a closure-orphan (no nested closure-var
// call, so it isolates the branch-enumeration fix from the real-instance/closure
// frontier). The body arm is behind `...body ? {body} : void 0`. The per-orphan
// local enumeration must explore the body-bearing arm. EXPECT: POST /_graphql
// {id, operationName, variables.login}.
var reg = [];
(function () {
  function op(props) {
    var body = JSON.stringify({ id: "xyz789", operationName: "ListRepos", variables: { login: props.login } });
    return fetch("/_graphql", { method: "POST", headers: { Accept: "application/json" }, ...(body ? { body: body } : void 0) });
  }
  reg.push(op);
})();
globalThis.__keepReg3 = reg;
