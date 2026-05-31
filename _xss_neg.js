// XSS false-positive prevention — Z3 must distinguish:
//   REAL_EXPLOIT : a concrete attacker witness satisfies Φ AND exploits Ψ
//   TAINT_REACH  : path feasible, but Φ pins Ψ to a literal that can't exploit
//   INFEASIBLE   : X-Force forced incompatible gates — false @S, suppress
// All four are tainted opaque values reaching real sinks; without Z3
// every one would be flagged as exploitable.

// 1) Strict-equality gate — Φ forces hash === "admin", so Ψ = "admin"
//    is the only feasible value. "admin" contains none of the HTML
//    payload tokens → exploit-shape UNSAT → TAINT_REACH.
var hash = location.hash;
if (hash === "admin") {
  document.body.innerHTML = hash;                       // expected: TAINT_REACH
}

// 2) Prefix-only "sanitizer" — DEMONSTRATES that this isn't actually a
//    sanitizer. Z3 finds witness "safe-<img …" satisfying both Φ
//    (startsWith "safe-") AND the contains-"<img" exploit shape →
//    REAL_EXPLOIT. This is the gate the analyzer must flag.
var h2 = location.search;
if (h2.startsWith("safe-")) {
  document.body.innerHTML = h2;                         // expected: REAL_EXPLOIT (prefix bypass)
}

// 3) Mutually-exclusive equality gates on ONE leaf — Φ has cookie==="admin"
//    AND cookie==="guest" both true at the same time. Z3 → UNSAT → INFEASIBLE.
var r = document.cookie;
if (r === "admin") {
  if (r === "guest") {
    document.body.insertAdjacentHTML("beforeend", r);   // expected: INFEASIBLE
  }
}

// 4) URL strict-eq gate — Φ pins cookie to "/home". URL exploit shape
//    needs prefix "javascript:" or "data:text/html" — "/home" satisfies
//    neither → UNSAT → TAINT_REACH (open-redirect risk is also rejected
//    because the value IS pinned to "/home", which is harmless).
var ck = localStorage.getItem("ret");
if (ck === "/home") {
  location.href = ck;                                    // expected: TAINT_REACH
}
