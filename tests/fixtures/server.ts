// Server used by tests/http_test.py
const port = Number(process.env.PORT);
const server = rtn.serve(
  { port, hostname: "127.0.0.1", keepAliveTimeout: 400, requestTimeout: 800, onListen() { console.log("ready"); } },
  async (req: Request): Promise<Response> => {
    const url = new URL(req.url);
    switch (url.pathname) {
      case "/":
        return new Response("hello");
      case "/json":
        return Response.json({ query: Object.fromEntries(url.searchParams), method: req.method });
      case "/echo":
        return new Response(await req.bytes(), { headers: { "content-type": req.headers.get("content-type") ?? "application/octet-stream" } });
      case "/echo-json":
        return Response.json(await req.json());
      case "/headers":
        return new Response(null, { status: 204, headers: { "x-custom": "yes", "set-cookie": "a=1" } });
      case "/multi-cookie": {
        const h = new Headers();
        h.append("set-cookie", "a=1");
        h.append("set-cookie", "b=2");
        return new Response("ok", { headers: h });
      }
      case "/slow":
        await new Promise((r) => setTimeout(r, 150));
        return new Response("slow");
      case "/throw":
        throw new Error("boom");
      case "/redirect":
        return Response.redirect("http://example.com/", 307);
      case "/url":
        return new Response(req.url);
      case "/stop":
        setTimeout(() => server.stop(), 10);
        return new Response("stopping");
      default:
        return new Response("not found", { status: 404 });
    }
  },
);
