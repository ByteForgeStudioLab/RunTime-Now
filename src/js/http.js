// rtn.serve() — an HTTP server with the same shape as Deno.serve / Bun.serve:
//
//   rtn.serve({ port: 3000 }, (req) => new Response("Salom!"));
//
// Sockets and HTTP parsing are done in C++ (src/bindings/http.cpp); this file
// turns raw requests into Request objects and Responses back into bytes.
(function (native, internal) {
  "use strict";

  const { httpListen, httpRespond, httpClose } = native;
  const { makeHeaders, makeRequest, headerList, bodyData } = internal;

  function serve(options, handler) {
    if (typeof options === "function") {
      handler = options;
      options = {};
    }
    options = options ?? {};
    handler = handler ?? options.fetch ?? options.handler;
    if (typeof handler !== "function") {
      throw new TypeError("rtn.serve: a handler function is required, e.g. rtn.serve((req) => new Response('hi'))");
    }
    const hostname = String(options.hostname ?? "0.0.0.0");
    const requestedPort = options.port === undefined ? 3000 : Number(options.port);
    const onError = options.onError;

    let port = requestedPort;
    let origin = "";

    function send(id, res, isRetry) {
      if (!(res instanceof Response)) {
        return fail(id, new TypeError("The handler must return a Response (or a Promise that resolves to one)"), isRetry);
      }
      const data = bodyData(res);
      httpRespond(id, res.status, res.statusText, headerList(res.headers), data === null ? "" : data);
    }

    function fail(id, error, isRetry) {
      if (onError && !isRetry) {
        let r;
        try {
          r = onError(error);
        } catch (e) {
          error = e;
        }
        if (r !== undefined) {
          Promise.resolve(r).then((res) => send(id, res, true), (e) => fail(id, e, true));
          return;
        }
      }
      console.error(error);
      httpRespond(id, 500, "", [["content-type", "text/plain;charset=UTF-8"]], "Internal Server Error");
    }

    // Called from C++ for every complete request.
    function onRequest(id, method, target, rawHeaders, body, remoteAddr, remotePort, host) {
      const headers = makeHeaders(rawHeaders);
      let url;
      if (target[0] === "/") url = (host ? "http://" + host : origin) + target;
      else if (target.startsWith("http://") || target.startsWith("https://")) url = target;  // absolute-form
      else url = (host ? "http://" + host : origin) + "/";  // "*" (OPTIONS *)
      const req = makeRequest(url, method, headers, body);
      const info = { remoteAddr: { transport: "tcp", hostname: remoteAddr, port: remotePort } };

      let result;
      try {
        result = handler(req, info);
      } catch (e) {
        fail(id, e);
        return;
      }
      if (result instanceof Response) {
        send(id, result);  // fast path: no promise needed
      } else {
        Promise.resolve(result).then((res) => send(id, res), (e) => fail(id, e));
      }
    }

    const [serverId, actualPort] = httpListen(
      hostname, requestedPort, onRequest, options.keepAliveTimeout, options.requestTimeout);
    port = actualPort;
    const displayHost = hostname === "0.0.0.0" || hostname === "::" ? "localhost" : hostname;
    origin = `http://${displayHost}:${port}`;

    let resolveFinished;
    const finished = new Promise((r) => { resolveFinished = r; });
    let stopped = false;

    const server = {
      hostname,
      port,
      url: new URL(origin + "/"),
      finished,
      stop() {
        if (stopped) return;
        stopped = true;
        httpClose(serverId);
        resolveFinished();
      },
      shutdown() {
        server.stop();
        return finished;
      },
    };

    if (typeof options.onListen === "function") options.onListen({ hostname, port });
    else if (options.onListen !== false) console.log(`Listening on ${origin}/`);
    return server;
  }

  Object.defineProperty(globalThis, "rtn", {
    value: { version: native.version, serve },
    writable: true,
    configurable: true,
    enumerable: false,
  });
});
