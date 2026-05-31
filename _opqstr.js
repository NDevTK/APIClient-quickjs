// Probe: does an opaque STRING drive a non-terminating parser loop? A search
// index builder (index-docs.js) loops over input with .length / charCodeAt /
// indexOf / slice. If any of these CONCRETIZES an opaque string (returns a
// concrete number/string instead of staying opaque-infectious), a loop gated by
// it becomes a CONCRETE infinite loop the opaque-only loop-revisit fixpoint
// can't see (seen_n stays 0 for that loop) — the freeze class.
//
// Run: ./qjs.exe --fe-deep-grind hostedge.js _opqstr.js
// EXPECT (after fix): TERMINATES + @H. Regression/hole ⇒ hang (no @DS).

// (a) .length on an opaque string, loop bound:
function scanLen(s) {
  for (var i = 0; i < s.length; i++) {     // s.length concrete ⇒ i<len concrete-forced?
    if (s.charCodeAt(i) === 65) fetch("/api/str-a");
  }
  return fetch("/api/str-len-done");
}
function ScanLen() { scanLen(this.text); }
globalThis.__ScanLenOrphan = ScanLen;

// (b) indexOf walk (markdown-it style: find next delimiter, advance):
function scanIdx(s) {
  var pos = 0, n;
  while ((n = s.indexOf("\n", pos)) !== -1) {   // indexOf on opaque ⇒ concrete -1/idx?
    fetch("/api/str-line");
    pos = n + 1;
  }
  return fetch("/api/str-idx-done");
}
function ScanIdx() { scanIdx(this.text); }
globalThis.__ScanIdxOrphan = ScanIdx;

// (c) slice/charAt advance (tokenizer):
function scanTok(s) {
  var rest = s;
  while (rest.length > 0) {                 // .length on opaque slice result
    if (rest.charAt(0) === "<") fetch("/api/str-tag");
    rest = rest.slice(1);
  }
  return fetch("/api/str-tok-done");
}
function ScanTok() { scanTok(this.text); }
globalThis.__ScanTokOrphan = ScanTok;
