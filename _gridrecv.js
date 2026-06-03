// Config client constructed DURING the grind (not at module top-level): the
// jsll/telemetry & sign-in shape. `__initTelemetry` is never called at boot, so
// the grind drives it as a residue fn → `new Channel(CONFIG)` builds an instance
// with the CONCRETE config. But Channel.sendPOST fired opaque at boot
// (__feDriveStatic, opaque this) → qjs_h_fired → excluded from the deep residue,
// and the boot-receiver scan ran before the instance existed. So its concrete
// collector URL is never recovered. EXPECT (after a grind-time receiver pass):
// @H POST https://collector.test/v1/events.
class Channel {
  constructor(cfg) { this.cfg = cfg; }
  sendPOST(payload) { return fetch(this.cfg.url, { method: "POST", body: payload }); }
}
var CONFIG = { url: "https://collector.test/v1/events" };
globalThis.__initTelemetry = function () {
  var ch = new Channel(CONFIG);
  globalThis.__ch = ch;        // kept alive; sendPOST never called by the page
};
