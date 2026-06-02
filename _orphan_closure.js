// Orphan reachable ONLY via a global ARRAY (not a global FUNCTION property), so
// __hostDrive (which enumerates only `typeof v === "function"` globals, MUTED)
// does NOT drive it — __feDriveStatic does, UNMUTED. This cleanly tests whether
// the __feDriveStatic path explores an orphan's internal opaque branch (both
// arms) vs single-passes it. arm_true present = __feDriveStatic forks; only
// arm_false = the boot-frontier single-pass gap.
var registry = [];
(function () {
  function op(props) {
    if (props.flag) { fetch("/api/arm_true"); }
    else { fetch("/api/arm_false"); }
  }
  registry.push(op);
})();
globalThis.__keepReg = registry;
