// XSS polarity fixture — positives MUST flag, negatives MUST NOT.
// No __opaque() anywhere: taint originates at the host edge exactly
// like a real page (location.hash, storage, document.cookie).

// + DOM-HTML: attacker-controlled location.hash -> innerHTML
var frag = location.hash;
document.body.innerHTML = frag;

// + code-exec: eval of attacker-controlled storage value
eval(localStorage.getItem("payload"));

// + open-redirect: cookie-derived value -> location.href
location.href = document.cookie;

// + DOM-HTML via insertAdjacentHTML, attacker search string
document.body.insertAdjacentHTML("beforeend", location.search);

// - NEGATIVE: concrete constant innerHTML (routine DOM, not a finding)
document.body.innerHTML = "<section id='app'></section>";

// - NEGATIVE: concrete navigation (not attacker-controlled)
location.assign("/dashboard");

// - NEGATIVE: eval of a concrete string (not attacker-controlled)
eval("1 + 1");
