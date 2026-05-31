// Realistic server-side gate reply — NO __opaque() anywhere. The
// opacity must flow from the host edge: xhr.responseText (unknown
// server reply) -> JSON.parse -> .role/.tier -> if/switch -> derived
// POST body. This is exactly the shape the user asked about: "one code
// path sets role to admin and one sets guest based on an IF/Switch ...
// could be a server side gate reply." Lives inside an onload callback,
// so it only runs once the event-loop pump fires XHR completion.
var x = new XMLHttpRequest();
x.open("GET", "/api/whoami?fields=role,tier");
x.onload = function () {
  var d = JSON.parse(x.responseText);
  var role = (d.role === "admin") ? "admin" : "guest";
  var tier;
  switch (d.tier) {
    case "pro": tier = "PRO"; break;
    case "ent": tier = "ENT"; break;
    default: tier = "FREE";
  }
  var p = new XMLHttpRequest();
  p.open("POST", "/api/users");
  p.setRequestHeader("Content-Type", "application/json");
  p.send(JSON.stringify({ role: role, tier: tier, id: 7 }));
};
x.send();
