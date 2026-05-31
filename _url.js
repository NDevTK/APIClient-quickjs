// WHATWG URL spec checks: parsing, relative resolution, components,
// searchParams, mutation. Each result is sent so drive.mjs surfaces it.
var u = new URL("https://user:pw@api.github.com:8443/repos/x?q=1&n=2#frag");
var rel = new URL("/v2/y?z=9", "https://api.github.com/a/b");
var sp = u.searchParams;
u.pathname = "/changed";
u.search = "?a=10";
fetch("/r?" + [
  "href=" + encodeURIComponent(u.href),
  "proto=" + u.protocol,
  "host=" + u.host,
  "hostname=" + u.hostname,
  "port=" + u.port,
  "origin=" + encodeURIComponent(u.origin),
  "user=" + u.username,
  "spq=" + sp.get("q"),
  "rel=" + encodeURIComponent(rel.href),
  "relpath=" + rel.pathname,
  "canParse=" + (URL.canParse ? URL.canParse("http://a/") : "n/a"),
  "afterPath=" + u.pathname,
  "afterSearch=" + u.search,
].join("&"));
try { new URL("not a url"); fetch("/bad?threw=0"); }
catch (e) { fetch("/bad?threw=1"); }
