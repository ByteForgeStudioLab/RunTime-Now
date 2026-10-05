// fetch() — an HTTP/1.1 client with the same API as in browsers, Node, Deno and Bun:
//
//   const res = await fetch("http://localhost:3000/todos", { method: "POST", body: "..." });
//   const todos = await res.json();
//
// DNS, sockets and response parsing are done in C++ (src/bindings/fetch.cpp);
// this file builds the request head, follows redirects and makes the Response.
// Supported URLs: http: and data:. https: needs TLS, which isn't built in yet.
(function (native, internal) {
  "use strict";

  const { fetchStart, fetchAbort, utf8Encode, version } = native;
  const { onAbort, bodyData, headerList, makeResponse, makeHeaders } = internal;

  const REDIRECT_STATUS = [301, 302, 303, 307, 308];
  const MAX_REDIRECTS = 20;
  // Set by fetch itself; a value given by the caller is ignored.
  const MANAGED_HEADERS = ["connection", "content-length", "keep-alive", "transfer-encoding", "upgrade"];

  // Like Node: TypeError("fetch failed") with the real reason in `cause`.
  function fetchFailed(message, code) {
    const cause = new Error(message);
    if (code) cause.code = code;
    return new TypeError("fetch failed", { cause });
  }

  function toBytes(data) {
    if (data === null) return null;
    return typeof data === "string" ? utf8Encode(data) : data;
  }

  // ------------------------------------------------------------------
  // data: URLs (https://fetch.spec.whatwg.org/#data-urls)
  // ------------------------------------------------------------------

  function percentDecodeBytes(str) {
    const bytes = [];
    for (const b of utf8Encode(str)) bytes.push(b);
    const out = [];
    for (let i = 0; i < bytes.length; i++) {
      const hex = String.fromCharCode(bytes[i + 1] ?? 0, bytes[i + 2] ?? 0);
      if (bytes[i] === 0x25 && /^[0-9a-fA-F]{2}$/.test(hex)) {
        out.push(parseInt(hex, 16));
        i += 2;
      } else {
        out.push(bytes[i]);
      }
    }
    return new Uint8Array(out);
  }

  function fetchDataUrl(url) {
    let rest = url.href.slice("data:".length);
    const hash = rest.indexOf("#");
    if (hash >= 0) rest = rest.slice(0, hash);
    const comma = rest.indexOf(",");
    if (comma < 0) throw fetchFailed("Invalid data: URL (missing ',')");
    let mime = rest.slice(0, comma).trim();
    let body = percentDecodeBytes(rest.slice(comma + 1));
    const base64 = /;[ \t]*base64[ \t]*$/i.exec(mime);
    if (base64) {
      mime = mime.slice(0, base64.index);
      let text = "";
      for (const b of body) text += String.fromCharCode(b);
      let binary;
      try {
        binary = atob(text.replace(/[\t\n\f\r ]/g, ""));
      } catch {
        throw fetchFailed("Invalid base64 in data: URL");
      }
      body = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) body[i] = binary.charCodeAt(i);
    }
    if (mime === "" || mime.startsWith(";")) mime = "text/plain" + (mime || ";charset=US-ASCII");
    return makeResponse({
      status: 200, statusText: "OK", headers: makeHeaders([["content-type", mime]]),
      body, url: url.href.replace(/#.*$/, ""), redirected: false, type: "basic",
    });
  }

  // ------------------------------------------------------------------
  // One HTTP request/response exchange
  // ------------------------------------------------------------------

  function send(url, method, headers, body, signal) {
    if (url.protocol === "https:") {
      throw fetchFailed(`https:// URLs are not supported yet (rtn has no TLS): ${url.href}`, "ERR_TLS_NOT_SUPPORTED");
    }
    if (url.protocol !== "http:") throw fetchFailed(`URL scheme "${url.protocol.slice(0, -1)}" is not supported`);
    if (url.username || url.password) {
      throw new TypeError(`Request cannot be constructed from a URL that includes credentials: ${url.href}`);
    }

    const bytes = toBytes(body);
    const given = headerList(headers);
    let head = `${method} ${url.pathname}${url.search} HTTP/1.1\r\n`;
    if (!headers.has("host")) head += `host: ${url.host}\r\n`;
    for (const [name, value] of given) {
      if (!MANAGED_HEADERS.includes(name)) head += `${name}: ${value}\r\n`;
    }
    if (!headers.has("accept")) head += "accept: */*\r\n";
    if (!headers.has("user-agent")) head += `user-agent: rtn/${version}\r\n`;
    // No compression support yet, so ask for none (no header would mean "anything").
    if (!headers.has("accept-encoding")) head += "accept-encoding: identity\r\n";
    if (bytes !== null) head += `content-length: ${bytes.length}\r\n`;
    else if (method === "POST" || method === "PUT" || method === "PATCH") head += "content-length: 0\r\n";
    head += "connection: close\r\n\r\n";

    const hostname = url.hostname.startsWith("[") ? url.hostname.slice(1, -1) : url.hostname;
    const port = url.port ? Number(url.port) : 80;

    return new Promise((resolve, reject) => {
      let stopListening = () => {};
      const id = fetchStart(hostname, port, head, bytes, method === "HEAD", (error, status, statusText, rawHeaders, resBody) => {
        stopListening();
        if (error) reject(new TypeError("fetch failed", { cause: error }));
        else resolve({ status, statusText, headers: makeHeaders(rawHeaders), body: resBody });
      });
      if (signal) {
        stopListening = onAbort(signal, () => {
          fetchAbort(id);
          reject(signal.reason);
        });
      }
    });
  }

  // ------------------------------------------------------------------
  // fetch()
  // ------------------------------------------------------------------

  async function fetch(input, init = undefined) {
    const request = new Request(input, init);
    const signal = request.signal;
    signal.throwIfAborted();
    if (request.bodyUsed) throw new TypeError("fetch: the request body has already been read");

    let url = new URL(request.url);
    if (url.protocol === "data:") return fetchDataUrl(url);

    let method = request.method;
    const headers = new Headers(request.headers);
    let body = bodyData(request);
    let redirected = false;

    for (let redirects = 0; ; redirects++) {
      const res = await send(url, method, headers, body, signal);
      const location = res.headers.get("location");
      if (REDIRECT_STATUS.includes(res.status) && location !== null && request.redirect !== "manual") {
        if (request.redirect === "error") throw fetchFailed(`unexpected redirect to ${location}`);
        if (redirects >= MAX_REDIRECTS) throw fetchFailed("redirect count exceeded");
        let next;
        try {
          next = new URL(location, url);
        } catch {
          throw fetchFailed(`invalid redirect location: ${location}`);
        }
        if (next.protocol !== "http:" && next.protocol !== "https:") {
          throw fetchFailed(`URL scheme must be a HTTP(S) scheme: ${next.href}`);
        }
        // 303, and 301/302 after a POST, turn into a GET without a body (like every browser).
        if ((res.status === 303 && method !== "GET" && method !== "HEAD") ||
            ((res.status === 301 || res.status === 302) && method === "POST")) {
          method = "GET";
          body = null;
          for (const h of ["content-type", "content-length", "content-encoding", "content-language", "content-location"]) {
            headers.delete(h);
          }
        }
        // Credentials must not leak to another site.
        if (next.origin !== url.origin) {
          for (const h of ["authorization", "proxy-authorization", "cookie", "host"]) headers.delete(h);
        }
        url = next;
        redirected = true;
        continue;
      }
      return makeResponse({
        status: res.status, statusText: res.statusText, headers: res.headers, body: res.body,
        url: url.href.replace(/#.*$/, ""), redirected, type: "basic",
      });
    }
  }

  Object.defineProperty(globalThis, "fetch", { value: fetch, writable: true, configurable: true, enumerable: true });
});
