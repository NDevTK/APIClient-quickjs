// Gate: nondeterministic sources (Math.random / Date.now) must be SYNTH-OPAQUE
// under forced execution, so a derived URL param is a dynamic-nonce SHAPE
// ({opaque}), NOT a baked-in random/timestamp literal that (a) breaks
// snapshot/resume determinism and (b) pollutes the learned endpoint (observed
// live on stackoverflow: `/cdn-cgi/challenge-platform/.../0.6235…` and
// `?r=0.32585…&t=1780170627094`).
//
// Run: ./qjs.exe --fe-exec hostedge.js _opqrand.js
// EXPECT: @H fetch GET /api/nonce with r AND t as OPAQUE params ({p…}), never a
// concrete number. Plain `./qjs.exe` (no --fe-exec) leaves Math.random/Date.now
// REAL (the engine is still a normal JS engine outside forced exec).
//
// KNOWN-OPEN (follow-up): `+new Date()` / `new Date().getTime()` still leak a
// real timestamp — the argless Date CONSTRUCTOR stores date_now() concretely and
// getTime/valueOf read it back. Making that opaque needs Date-object-level
// handling (tag argless-now Dates; return opaque from getTime/valueOf) without
// breaking getFullYear etc. — more involved than the value-return patches here.
// Whole nondeterministic-source CLASS must be opaque: Math.random + Date.now
// (engine), performance.now + crypto.randomUUID (host model via __synth).
fetch("/api/nonce?r=" + Math.random() + "&t=" + Date.now() +
      "&p=" + performance.now() + "&u=" + crypto.randomUUID());
