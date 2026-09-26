// Web API'lar: rtn va node natijasi bir xil bo'lishi kerak
const out = [];
const log = (...a) => out.push(JSON.stringify(a));
const tryUrl = (u, b) => { try { const x = new URL(u, b); return [x.href, x.origin, x.host, x.pathname, x.search, x.hash]; } catch (e) { return e.constructor.name; } };

// URL
for (const [u, b] of [
  ["https://example.com"], ["HTTPS://User:Pa ss@EXAMPLE.com:443/a/./b/../c?q=1 2&x=ё#frag ment"],
  ["http://localhost:3000/api/users?id=5"], ["http://[::1]:8080/x"], ["http://h:65536/"], ["http://h:abc/"],
  ["/path?x=1", "http://base.com/a/b"], ["../up", "http://base.com/a/b/c"], ["?only=query", "http://b.com/p/q"],
  ["#h", "http://b.com/p?x"], ["//other.com/z", "https://b.com"], ["mailto:ali@example.com"],
  ["file:///home/user/a.txt"], ["not a url"], ["http://example.com/a%2Fb/%zz/ü"], ["ws://x.com:80/s"],
  ["http://EXAMPLE.com.:8080/../../x"], ["https://a.com/p?a=1&b=2&a=3"], [""], ["http://a.com\\b\\c"],
]) log("URL", u, b ?? null, tryUrl(u, b));

const url = new URL("http://x.com/p?a=1&b=2");
url.searchParams.append("c", "3 4");
url.searchParams.set("a", "ü");
url.hash = "top"; url.pathname = "/new path"; url.port = "8080";
log("URL mutate", url.href, [...url.searchParams], url.searchParams.get("c"), URL.canParse("x"), URL.canParse("x", "http://a"));

// URLSearchParams
const sp = new URLSearchParams("?a=1&b=x+y&c=%F0%9F%98%80&a=2&e=&f");
log("USP", [...sp], sp.getAll("a"), sp.get("b"), sp.has("f"), sp.size, sp.toString());
sp.delete("a"); sp.sort(); log("USP2", sp.toString(), new URLSearchParams({ q: "a&b=c", n: 1 }).toString(), new URLSearchParams([["x", "1"], ["y", "2"]]).toString());

// Headers
const h = new Headers({ "Content-Type": "text/plain", "X-A": "1" });
h.append("x-a", "2"); h.append("Set-Cookie", "a=1"); h.append("set-cookie", "b=2"); h.set("X-B", " spaced ");
log("Headers", [...h], h.get("x-a"), h.get("X-B"), h.has("content-type"), h.get("missing"), h.getSetCookie());
h.delete("x-a"); log("Headers2", [...h.keys()]);
let bad; try { new Headers({ "bad name": "x" }); } catch (e) { bad = e.constructor.name; } log("bad header", bad);

// TextEncoder / TextDecoder
const enc = new TextEncoder().encode("Salom 🌍 ё");
log("enc", [...enc], new TextDecoder().decode(enc), new TextDecoder().decode(new Uint8Array([0xff, 0x61, 0xe2, 0x82])), new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x68])));
let fatal; try { new TextDecoder("utf-8", { fatal: true }).decode(new Uint8Array([0xc3])); } catch (e) { fatal = e.constructor.name; } log("fatal", fatal);
log("lone surrogate", [...new TextEncoder().encode("a\ud800b")]);

// Response / Request
const r = new Response("hi", { status: 201, headers: { "x-y": "z" } });
log("Response", r.status, r.ok, r.statusText, [...r.headers], await r.text(), r.bodyUsed);
const j = Response.json({ a: [1, 2] }, { status: 400 });
log("json", j.status, j.headers.get("content-type"), await j.json());
log("redirect", Response.redirect("http://a.com/x", 301).status, Response.redirect("http://a.com/x").headers.get("location"));
const bytesRes = new Response(new Uint8Array([104, 105]));
log("bytes", bytesRes.headers.get("content-type"), await bytesRes.text(), new Response(null).headers.get("content-type"));
let rangeErr; try { new Response("x", { status: 99 }); } catch (e) { rangeErr = e.constructor.name; } log("status range", rangeErr);
const req = new Request("http://a.com/p?q=1", { method: "post", body: JSON.stringify({ n: 1 }), headers: { "x-k": "v" } });
log("Request", req.method, req.url, req.headers.get("content-type"), await req.clone().json(), await req.text());
let getBody; try { new Request("http://a.com", { method: "GET", body: "x" }); } catch (e) { getBody = e.constructor.name; } log("GET body", getBody);
const form = new Response(new URLSearchParams({ a: "1 2" }));
log("form", form.headers.get("content-type"), await form.text());
const ab = await new Response("abc").arrayBuffer(); log("arrayBuffer", ab.byteLength, ab instanceof ArrayBuffer);

for (const line of out) console.log(line);
