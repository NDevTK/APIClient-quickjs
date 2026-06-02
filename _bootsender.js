// Boot-constructed config-bearing sender (models jsll/AppInsights telemetry and
// any analytics/API client the PAGE instantiates at module top-level with a
// concrete endpoint literal, then sends through later). The instance is created
// during the BOOT eval — BEFORE the deep grind sets qjs_deep_rb — so
// qjs_deep_capture_inst (quickjs.c) never captures its `send` closure. The grind
// then drives `send` COLD (opaque `this`) → this.cfg.endpointUrl is opaque →
// the @H URL is opaque, never the concrete collector URL the live page POSTs.
//
// FIX target: drive a residue method with the REAL boot-created instance (its
// concrete config), not a cold opaque-this orphan — so the endpoint resolves to
// the literal. EXPECT (after fix): @H POST https://collector.example.com/v1/track
var config = { endpointUrl: "https://collector.example.com/v1/track" };
function Sender(cfg) { this.cfg = cfg; }
Sender.prototype.send = function (payload) {
  return fetch(this.cfg.endpointUrl, { method: "POST", body: payload });
};
var sender = new Sender(config);   // constructed at BOOT, never new'd by the grind
globalThis.__bootSender = sender;  // referenced; send() is never called by the page
