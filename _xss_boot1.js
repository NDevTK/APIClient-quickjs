// Single top-level boot-frontier XSS sink: hash pinned to "admin" by the gate,
// so Z3 should say TAINT_REACH (admin has no payload tokens). If the re-boot
// gives REAL_EXPLOIT, the boot-forced strict-eq isn't pinning the value in Φ.
var h = location.hash;
if (h === "admin") {
  document.body.innerHTML = h;   // expected: TAINT_REACH
}
