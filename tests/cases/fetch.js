// fetch() against our own rtn.serve() server.
const server = rtn.serve({ port: 0, hostname: "127.0.0.1", onListen: false }, async (req) => {
  const url = new URL(req.url);
  switch (url.pathname) {
    case "/json":
      return Response.json({ method: req.method, agent: req.headers.get("user-agent")?.split("/")[0], q: url.searchParams.get("q") });
    case "/echo":
      return new Response(await req.text(), {
        headers: { "x-length": req.headers.get("content-length") ?? "none", "x-type": req.headers.get("content-type") ?? "none" },
      });
    case "/redirect": return Response.redirect(new URL("/json?q=redirected", req.url), 302);
    case "/see-other": return Response.redirect(new URL("/echo", req.url), 303);
    case "/loop": return Response.redirect(new URL("/loop", req.url), 307);
    case "/bytes": return new Response(new Uint8Array([0, 1, 2, 255]));
    case "/empty": return new Response(null, { status: 204 });
    case "/slow":
      await new Promise((r) => setTimeout(r, 300));
      return new Response("late");
    default: return new Response("not here", { status: 404, statusText: "Nowhere" });
  }
});
const base = `http://127.0.0.1:${server.port}`;
const show = (url) => url.replace(base, "<base>");

let res = await fetch(`${base}/json?q=1`);
console.log(res.status, res.ok, res.statusText, res.headers.get("content-type"), await res.json());

res = await fetch(`${base}/echo`, { method: "POST", body: "héllo" });
console.log("POST:", res.headers.get("x-length"), res.headers.get("x-type"), await res.text());
res = await fetch(new Request(`${base}/echo`, { method: "PUT", body: new URLSearchParams({ a: "1", b: "x y" }) }));
console.log("Request object:", res.headers.get("x-type"), await res.text());
res = await fetch(`${base}/echo`, { method: "POST" });
console.log("POST without body:", res.headers.get("x-length"));

res = await fetch(`${base}/redirect`);
console.log("302:", res.status, res.redirected, show(res.url), await res.json());
res = await fetch(`${base}/see-other`, { method: "PUT", body: "dropped" });
console.log("303 turns PUT into GET:", res.status, show(res.url), JSON.stringify(await res.text()));
res = await fetch(`${base}/redirect`, { redirect: "manual" });
console.log("manual:", res.status, show(res.headers.get("location")));
try { await fetch(`${base}/redirect`, { redirect: "error" }); } catch (e) { console.log("redirect: error ->", e.message, "|", show(e.cause.message)); }
try { await fetch(`${base}/loop`); } catch (e) { console.log("loop ->", e.message, "|", e.cause.message); }

console.log("bytes:", await (await fetch(`${base}/bytes`)).bytes());
res = await fetch(`${base}/empty`);
console.log("204:", res.status, JSON.stringify(await res.text()));
res = await fetch(`${base}/missing`, { method: "HEAD" });
console.log("HEAD 404:", res.status, res.statusText, res.headers.get("content-length"), JSON.stringify(await res.text()));

const controller = new AbortController();
setTimeout(() => controller.abort(), 20);
try { await fetch(`${base}/slow`, { signal: controller.signal }); } catch (e) { console.log("abort:", e.name); }
try { await fetch(`${base}/slow`, { signal: AbortSignal.timeout(20) }); } catch (e) { console.log("timeout:", e.name, "-", e.message); }
try { await fetch(`${base}/json`, { signal: AbortSignal.abort() }); } catch (e) { console.log("already aborted:", e.name); }

try { await fetch("https://example.com/"); } catch (e) { console.log(e.name + ":", e.message, "|", e.cause.code); }
try { await fetch("ftp://example.com/"); } catch (e) { console.log(e.name + ":", e.message, "|", e.cause.message); }
try { await fetch("/relative"); } catch (e) { console.log(e.name + ":", e.message); }

const closed = rtn.serve({ port: 0, hostname: "127.0.0.1", onListen: false }, () => new Response(""));
const closedPort = closed.port;
closed.stop();
try { await fetch(`http://127.0.0.1:${closedPort}/`); } catch (e) {
  console.log("refused:", e.message, "|", e.cause.code, e.cause.syscall, e.cause.message.replace(String(closedPort), "<port>"));
}

res = await fetch("data:text/plain;base64,SGVsbG8sIFdvcmxkIQ==");
console.log("data: base64:", res.headers.get("content-type"), await res.text());
res = await fetch("data:,a%20b%F0%9F%98%80");
console.log("data: text:", res.headers.get("content-type"), await res.text());

const all = await Promise.all(Array.from({ length: 50 }, (_, i) => fetch(`${base}/json?q=${i}`).then((r) => r.json())));
console.log("50 parallel:", all.length, all.every((r, i) => r.q === String(i)));

res = await fetch(`${base}/json`);
const { date, ...headers } = Object.fromEntries(res.headers);
console.log(res.type, show(res.url), headers);
server.stop();
