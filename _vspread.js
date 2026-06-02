// Value-spread gate: a fired ORPHAN with a value-determining branch (both arms
// set `role`, both reach the SAME fetch). __feDriveStatic fires the default arm
// (role=guest); __feValueSpread enumerates the other arm (role=admin). EXPECT:
// POST /api/act {role=guest|admin}.
var reg = [];
(function () {
  function feature(d) {
    var role = (d && d.role === "admin") ? "admin" : "guest";
    fetch("/api/act", { method: "POST", body: JSON.stringify({ role: role }) });
  }
  reg.push(feature);
})();
globalThis.__keepReg = reg;
