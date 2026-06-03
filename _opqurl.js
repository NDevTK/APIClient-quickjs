// A fetch whose URL is genuine attacker/server-opaque (location.hash), driven
// cold. No real receiver can concretize it — the URL is honestly opaque. Per
// CLAUDE.md "a fully-opaque URL is a resolverError, never dropped" + "No silent
// failure", this must surface as a resolverError (an opaque-URL call site is
// real signal: a gated endpoint whose URL is attacker/config-controlled), NOT
// vanish. Today H()'s "@H "+JSON.stringify(...) goes whole-opaque, renders
// "[object Object]", and the worker drops it (no @H prefix, no resolverError).
globalThis.__probe = function () { return fetch(location.hash); };
