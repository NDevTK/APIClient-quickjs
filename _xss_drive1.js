// Same strict-eq gate but reached via the DRIVE phase (onload, post-image), NOT
// boot. If TAINT_REACH -> the Φ-pin bug is boot-specific; if REAL_EXPLOIT -> it's
// a GENERAL forced-strict-eq-doesn't-pin bug.
var x = new XMLHttpRequest();
x.open("GET", "/api/x");
x.onload = function () {
  var r = x.responseText;
  if (r === "admin") {
    document.body.innerHTML = r;   // expected: TAINT_REACH (r pinned to "admin")
  }
};
x.send();
