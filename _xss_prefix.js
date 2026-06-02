// Prefix-bypass: startsWith("safe-") does NOT sanitize — "safe-<img ...>" passes
// the gate AND carries the payload. MUST be REAL_EXPLOIT (the phiModel fix must
// not turn this into a false NEGATIVE).
var x = new XMLHttpRequest();
x.open("GET", "/api/x");
x.onload = function () {
  var h = x.responseText;
  if (h.startsWith("safe-")) {
    document.body.innerHTML = h;   // expected: REAL_EXPLOIT
  }
};
x.send();
