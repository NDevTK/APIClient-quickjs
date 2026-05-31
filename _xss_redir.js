// Open-redirect vs javascript-scheme XSS — same source (location.search),
// two paths to location.href, two different verdicts. Proves Z3 reasons
// about the URL family (str.prefixof "javascript:" / "data:text/html").

var q = location.search;

// A) Raw assignment, no gate — Z3 finds prefix "javascript:" SAT
//    on the unconstrained leaf → REAL_EXPLOIT (witness: javascript:…).
location.href = q;                       // expected: REAL_EXPLOIT

// B) Same source under an http(s) prefix gate — startsWith("https://")
//    is incompatible with prefix "javascript:" or "data:text/html"
//    → TAINT_REACH (open-redirect risk, NOT scheme XSS).
if (q.startsWith("https://")) {
  location.replace(q);                   // expected: TAINT_REACH
}

// C) Path-with-substring gate — Φ asserts includes("redirect=") which
//    is compatible with both javascript: and http:// prefixes → Z3 finds
//    a javascript:…?redirect=… witness → REAL_EXPLOIT.
if (q.includes("redirect=")) {
  location.assign(q);                    // expected: REAL_EXPLOIT
}
