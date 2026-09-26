// Web platform APIs: TextEncoder, TextDecoder, URL, URLSearchParams,
// Headers, Request, Response.
//
// This file is embedded into the rtn binary and runs once at startup.
// `native` holds C++ helpers; anything put on `internal` is shared with
// the other embedded files (http.js) but never visible to user code.
(function (native, internal) {
  "use strict";

  const { utf8Encode, utf8Decode } = native;
  const INSPECT = Symbol.for("rtn.inspect");  // custom console.log output

  function define(name, value) {
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  }

  // Uint8Array view over any BufferSource (no copy).
  function toBytes(input) {
    if (input instanceof Uint8Array) return input;
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    throw new TypeError("The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
  }

  // ------------------------------------------------------------------
  // TextEncoder / TextDecoder (UTF-8 only)
  // ------------------------------------------------------------------

  class TextEncoder {
    get encoding() { return "utf-8"; }
    encode(input = "") { return utf8Encode(String(input)); }
    encodeInto(source, destination) {
      const bytes = utf8Encode(String(source));
      let written = Math.min(bytes.length, destination.length);
      // Don't cut a multi-byte character in half.
      while (written > 0 && written < bytes.length && (bytes[written] & 0xc0) === 0x80) written--;
      destination.set(bytes.subarray(0, written));
      const read = utf8Decode(bytes.subarray(0, written), false).length;
      return { read, written };
    }
  }

  class TextDecoder {
    #fatal;
    #ignoreBOM;
    constructor(label = "utf-8", options = {}) {
      const l = String(label).trim().toLowerCase();
      if (l !== "utf-8" && l !== "utf8" && l !== "unicode-1-1-utf-8") {
        throw new RangeError(`The encoding label provided ('${label}') is not supported.`);
      }
      this.#fatal = Boolean(options.fatal);
      this.#ignoreBOM = Boolean(options.ignoreBOM);
    }
    get encoding() { return "utf-8"; }
    get fatal() { return this.#fatal; }
    get ignoreBOM() { return this.#ignoreBOM; }
    decode(input = new Uint8Array(0)) {
      let s = utf8Decode(toBytes(input), this.#fatal);
      if (!this.#ignoreBOM && s.charCodeAt(0) === 0xfeff) s = s.slice(1);
      return s;
    }
  }

  // ------------------------------------------------------------------
  // Percent-encoding helpers
  // ------------------------------------------------------------------

  const HEX = "0123456789ABCDEF";

  // Encodes every character for which `keep(code)` is false, as UTF-8 %XX.
  function percentEncode(str, keep) {
    let out = "";
    for (let i = 0; i < str.length; i++) {
      const c = str.charCodeAt(i);
      if (c < 0x80 && keep(c)) {
        out += str[i];
        continue;
      }
      let ch = str[i];
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) ch += str[++i];
      for (const b of utf8Encode(ch)) out += "%" + HEX[b >> 4] + HEX[b & 15];
    }
    return out;
  }

  // Decodes %XX sequences; malformed ones are left as-is (never throws).
  function percentDecode(str) {
    if (!str.includes("%")) return str;
    return str.replace(/(?:%[0-9a-fA-F]{2})+/g, (run) => {
      const bytes = new Uint8Array(run.length / 3);
      for (let i = 0; i < bytes.length; i++) bytes[i] = parseInt(run.slice(i * 3 + 1, i * 3 + 3), 16);
      return utf8Decode(bytes, false);
    });
  }

  const isC0orSpace = (c) => c <= 0x20 || c === 0x7f;
  // Character sets from the URL Standard.
  const keepFragment = (c) => !isC0orSpace(c) && c !== 0x22 && c !== 0x3c && c !== 0x3e && c !== 0x60;
  const keepQuery = (c) => !isC0orSpace(c) && c !== 0x22 && c !== 0x23 && c !== 0x3c && c !== 0x3e;
  const keepSpecialQuery = (c) => keepQuery(c) && c !== 0x27;
  const keepPath = (c) => keepQuery(c) && c !== 0x3f && c !== 0x60 && c !== 0x7b && c !== 0x7d;
  const keepUserinfo = (c) => keepPath(c) && !"/:;=@[\\]^|".includes(String.fromCharCode(c));
  // application/x-www-form-urlencoded
  const keepForm = (c) =>
    (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) ||
    c === 0x2a || c === 0x2d || c === 0x2e || c === 0x5f;

  // ------------------------------------------------------------------
  // URLSearchParams
  // ------------------------------------------------------------------

  let linkSearchParams;  // (params, url) -> void; set in the class's static block

  class URLSearchParams {
    #list = [];
    #url = null;  // the URL this object belongs to (url.searchParams)

    static {
      linkSearchParams = (params, url) => { params.#url = url; };
    }

    constructor(init = "") {
      if (init instanceof URLSearchParams) {
        this.#list = init.#list.map(([k, v]) => [k, v]);
      } else if (typeof init === "object" && init !== null && typeof init[Symbol.iterator] === "function") {
        for (const pair of init) {
          const p = [...pair];
          if (p.length !== 2) throw new TypeError("Each query pair must be an iterable [name, value] tuple");
          this.#list.push([String(p[0]), String(p[1])]);
        }
      } else if (typeof init === "object" && init !== null) {
        for (const k of Object.keys(init)) this.#list.push([k, String(init[k])]);
      } else {
        this.#parse(String(init));
      }
    }

    #parse(str) {
      this.#list = [];
      if (str.startsWith("?")) str = str.slice(1);
      for (const part of str.split("&")) {
        if (!part) continue;
        const eq = part.indexOf("=");
        const k = eq < 0 ? part : part.slice(0, eq);
        const v = eq < 0 ? "" : part.slice(eq + 1);
        this.#list.push([percentDecode(k.replace(/\+/g, " ")), percentDecode(v.replace(/\+/g, " "))]);
      }
    }

    #update() {
      if (this.#url) internal.setUrlQuery(this.#url, this.toString());
    }

    get size() { return this.#list.length; }
    append(name, value) { this.#list.push([String(name), String(value)]); this.#update(); }
    delete(name, value) {
      name = String(name);
      this.#list = this.#list.filter(([k, v]) => k !== name || (value !== undefined && v !== String(value)));
      this.#update();
    }
    get(name) {
      name = String(name);
      const hit = this.#list.find(([k]) => k === name);
      return hit ? hit[1] : null;
    }
    getAll(name) { name = String(name); return this.#list.filter(([k]) => k === name).map(([, v]) => v); }
    has(name, value) {
      name = String(name);
      return this.#list.some(([k, v]) => k === name && (value === undefined || v === String(value)));
    }
    set(name, value) {
      name = String(name);
      value = String(value);
      const i = this.#list.findIndex(([k]) => k === name);
      if (i < 0) this.#list.push([name, value]);
      else {
        this.#list[i][1] = value;
        this.#list = this.#list.filter(([k], j) => k !== name || j === i);
      }
      this.#update();
    }
    sort() {
      this.#list = this.#list.map((p, i) => [p, i])
        .sort((a, b) => (a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]))
        .map(([p]) => p);
      this.#update();
    }
    forEach(callback, thisArg) { for (const [k, v] of this.#list) callback.call(thisArg, v, k, this); }
    *entries() { for (const [k, v] of this.#list) yield [k, v]; }
    *keys() { for (const [k] of this.#list) yield k; }
    *values() { for (const [, v] of this.#list) yield v; }
    [Symbol.iterator]() { return this.entries(); }
    toString() {
      return this.#list
        .map(([k, v]) => percentEncode(k, keepForm).replace(/%20/g, "+") + "=" + percentEncode(v, keepForm).replace(/%20/g, "+"))
        .join("&");
    }
    get [Symbol.toStringTag]() { return "URLSearchParams"; }
    [INSPECT]() { return Object.fromEntries(this.#list); }
  }

  // ------------------------------------------------------------------
  // URL (WHATWG URL Standard, the commonly used subset)
  // ------------------------------------------------------------------

  const SPECIAL = { "http:": "80", "https:": "443", "ws:": "80", "wss:": "443", "ftp:": "21", "file:": "" };
  const FORBIDDEN_HOST = /[\0\t\n\r #/:<>?@[\\\]^|%]/;

  function normalizePath(path, special) {
    const segments = path.split("/");
    const out = [];
    for (let i = 0; i < segments.length; i++) {
      const s = segments[i];
      const last = i === segments.length - 1;
      const lower = s.toLowerCase();
      if (s === ".." || lower === ".%2e" || lower === "%2e." || lower === "%2e%2e") {
        if (out.length > 1) out.pop();
        if (last) out.push("");
      } else if (s === "." || lower === "%2e") {
        if (last) out.push("");
      } else {
        out.push(s);
      }
    }
    let result = out.join("/");
    if (special && !result.startsWith("/")) result = "/" + result;
    return percentEncode(result, (c) => keepPath(c) || c === 0x25);
  }

  function parseHost(host, special) {
    if (host.startsWith("[")) {
      if (!host.endsWith("]") || !/^\[[0-9a-fA-F:.]+\]$/.test(host)) return null;
      return host.toLowerCase();
    }
    if (!special) return percentEncode(host, (c) => !isC0orSpace(c) || c === 0x20);
    const decoded = percentDecode(host).toLowerCase();
    if (decoded === "" || FORBIDDEN_HOST.test(decoded)) return null;
    return decoded;
  }

  let setUrlQuery;

  class URL {
    #protocol = "";
    #username = "";
    #password = "";
    #hostname = "";
    #port = "";
    #pathname = "";
    #search = "";
    #hash = "";
    #hasAuthority = false;
    #searchParams = null;

    static {
      setUrlQuery = (url, query) => { url.#search = query ? "?" + query : ""; };
    }

    constructor(url, base) {
      url = String(url).replace(/^[\0- ]+|[\0- ]+$/g, "").replace(/[\t\n\r]/g, "");
      let baseUrl = null;
      if (base !== undefined) baseUrl = base instanceof URL ? base : new URL(base);
      if (!this.#parse(url, baseUrl)) {
        throw new TypeError(`Invalid URL: '${url}'` + (baseUrl ? ` with base '${baseUrl.href}'` : ""));
      }
    }

    static canParse(url, base) {
      try { new URL(url, base); return true; } catch { return false; }
    }
    static parse(url, base) {
      try { return new URL(url, base); } catch { return null; }
    }

    #parse(input, base) {
      const m = /^([a-zA-Z][a-zA-Z0-9+.\-]*):/.exec(input);
      if (m) {
        const scheme = m[1].toLowerCase() + ":";
        let rest = input.slice(m[0].length);
        const special = scheme in SPECIAL;
        if (special) rest = rest.replace(/\\/g, "/");
        // "http:foo" relative to an http: base
        if (special && base && base.#protocol === scheme && !rest.startsWith("/")) {
          return this.#relative(rest, base);
        }
        this.#protocol = scheme;
        if (special) {
          rest = scheme === "file:" ? rest.replace(/^\/\//, "") : rest.replace(/^\/*/, "");
          return this.#authorityAndRest(rest, true);
        }
        if (rest.startsWith("//")) return this.#authorityAndRest(rest.slice(2), false);
        // opaque path, e.g. mailto:x@y.z or data:text/plain,hi
        return this.#pathQueryHash(rest, false, true);
      }
      if (!base) return false;
      return this.#relative(input, base);
    }

    #relative(input, base) {
      const special = base.#protocol in SPECIAL;
      if (special) input = input.replace(/\\/g, "/");
      this.#protocol = base.#protocol;
      if (input.startsWith("//")) return this.#authorityAndRest(input.slice(2), special);
      if (!base.#hasAuthority && !base.#pathname.startsWith("/") && !input.startsWith("#")) return false;

      this.#hasAuthority = base.#hasAuthority;
      this.#username = base.#username;
      this.#password = base.#password;
      this.#hostname = base.#hostname;
      this.#port = base.#port;

      if (input === "" || input.startsWith("#")) {
        this.#pathname = base.#pathname;
        this.#search = base.#search;
        this.#hash = input ? "#" + percentEncode(input.slice(1), (c) => keepFragment(c) || c === 0x25) : "";
        return true;
      }
      if (input.startsWith("?")) {
        this.#pathname = base.#pathname;
        return this.#pathQueryHash(input, special, false, true);
      }
      if (input.startsWith("/")) return this.#pathQueryHash(input, special);
      const dir = base.#pathname.slice(0, base.#pathname.lastIndexOf("/") + 1);
      return this.#pathQueryHash((dir || "/") + input, special);
    }

    #authorityAndRest(rest, special) {
      this.#hasAuthority = true;
      const end = rest.search(/[/?#]/);
      let authority = end < 0 ? rest : rest.slice(0, end);
      const after = end < 0 ? "" : rest.slice(end);

      const at = authority.lastIndexOf("@");
      if (at >= 0) {
        const info = authority.slice(0, at);
        authority = authority.slice(at + 1);
        const colon = info.indexOf(":");
        this.#username = percentEncode(colon < 0 ? info : info.slice(0, colon), (c) => keepUserinfo(c) || c === 0x25);
        this.#password = colon < 0 ? "" : percentEncode(info.slice(colon + 1), (c) => keepUserinfo(c) || c === 0x25);
      }

      let host = authority;
      let port = "";
      const portMatch = /:(\d*)$/.exec(authority);
      if (portMatch && !authority.endsWith("]")) {
        host = authority.slice(0, portMatch.index);
        port = portMatch[1];
      } else if (/:[^\]]*$/.test(authority) && !authority.startsWith("[")) {
        return false;  // "host:abc"
      }
      if (port !== "") {
        const n = Number(port);
        if (n > 65535) return false;
        port = String(n);
        if (SPECIAL[this.#protocol] === port) port = "";
      }
      if (host === "") {
        if (special && this.#protocol !== "file:") return false;
        this.#hostname = "";
      } else {
        const parsed = parseHost(host, special);
        if (parsed === null) return false;
        this.#hostname = this.#protocol === "file:" && parsed === "localhost" ? "" : parsed;
      }
      this.#port = port;
      return this.#pathQueryHash(after, special);
    }

    #pathQueryHash(rest, special, opaque = false, keepPathname = false) {
      const hashAt = rest.indexOf("#");
      if (hashAt >= 0) {
        this.#hash = "#" + percentEncode(rest.slice(hashAt + 1), (c) => keepFragment(c) || c === 0x25);
        rest = rest.slice(0, hashAt);
      } else {
        this.#hash = "";
      }
      const queryAt = rest.indexOf("?");
      if (queryAt >= 0) {
        const keep = special ? keepSpecialQuery : keepQuery;
        this.#search = "?" + percentEncode(rest.slice(queryAt + 1), (c) => keep(c) || c === 0x25);
        rest = rest.slice(0, queryAt);
      } else {
        this.#search = "";
      }
      if (this.#search === "?") this.#search = "";
      if (keepPathname) return true;
      if (opaque) {
        this.#pathname = percentEncode(rest, (c) => !isC0orSpace(c) || c === 0x20);
        return true;
      }
      this.#pathname = rest === "" && !special ? "" : normalizePath(rest, special || this.#hasAuthority);
      return true;
    }

    get href() {
      let auth = "";
      if (this.#hasAuthority) {
        auth = "//";
        if (this.#username || this.#password) {
          auth += this.#username + (this.#password ? ":" + this.#password : "") + "@";
        }
        auth += this.host;
      }
      return this.#protocol + auth + this.#pathname + this.#search + this.#hash;
    }
    set href(value) {
      const u = new URL(value);
      this.#protocol = u.#protocol; this.#username = u.#username; this.#password = u.#password;
      this.#hostname = u.#hostname; this.#port = u.#port; this.#pathname = u.#pathname;
      this.#search = u.#search; this.#hash = u.#hash; this.#hasAuthority = u.#hasAuthority;
      this.#searchParams = null;
    }
    get origin() {
      const p = this.#protocol;
      return p in SPECIAL && p !== "file:" ? p + "//" + this.host : "null";
    }
    get protocol() { return this.#protocol; }
    set protocol(value) {
      const p = String(value).replace(/:.*$/, "").toLowerCase() + ":";
      if (/^[a-z][a-z0-9+.\-]*:$/.test(p) && (p in SPECIAL) === (this.#protocol in SPECIAL)) {
        this.#protocol = p;
        if (SPECIAL[p] === this.#port) this.#port = "";
      }
    }
    get username() { return this.#username; }
    set username(v) { this.#username = percentEncode(String(v), keepUserinfo); }
    get password() { return this.#password; }
    set password(v) { this.#password = percentEncode(String(v), keepUserinfo); }
    get host() { return this.#hostname + (this.#port ? ":" + this.#port : ""); }
    set host(value) {
      const u = URL.parse(this.#protocol + "//" + String(value));
      if (u) { this.#hostname = u.#hostname; this.#port = u.#port; }
    }
    get hostname() { return this.#hostname; }
    set hostname(value) {
      const h = parseHost(String(value), this.#protocol in SPECIAL);
      if (h !== null) this.#hostname = h;
    }
    get port() { return this.#port; }
    set port(value) {
      const s = String(value);
      if (s === "") { this.#port = ""; return; }
      const m = /^\d+/.exec(s);
      if (!m || Number(m[0]) > 65535) return;
      const p = String(Number(m[0]));
      this.#port = SPECIAL[this.#protocol] === p ? "" : p;
    }
    get pathname() { return this.#pathname; }
    set pathname(value) {
      let v = String(value);
      const special = this.#protocol in SPECIAL;
      if (special) v = v.replace(/\\/g, "/");
      if (!v.startsWith("/") && (special || this.#hasAuthority)) v = "/" + v;
      this.#pathname = normalizePath(v, special);
    }
    get search() { return this.#search; }
    set search(value) {
      let v = String(value);
      if (v.startsWith("?")) v = v.slice(1);
      const keep = this.#protocol in SPECIAL ? keepSpecialQuery : keepQuery;
      this.#search = v ? "?" + percentEncode(v, (c) => keep(c) || c === 0x25) : "";
      this.#searchParams = null;
    }
    get searchParams() {
      if (!this.#searchParams) {
        this.#searchParams = new URLSearchParams(this.#search);
        linkSearchParams(this.#searchParams, this);
      }
      return this.#searchParams;
    }
    get hash() { return this.#hash === "#" ? "" : this.#hash; }
    set hash(value) {
      let v = String(value);
      if (v.startsWith("#")) v = v.slice(1);
      this.#hash = v ? "#" + percentEncode(v, (c) => keepFragment(c) || c === 0x25) : "";
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
    get [Symbol.toStringTag]() { return "URL"; }
    [INSPECT]() {
      return {
        href: this.href, origin: this.origin, protocol: this.protocol, username: this.username,
        password: this.password, host: this.host, hostname: this.hostname, port: this.port,
        pathname: this.pathname, search: this.search, searchParams: this.searchParams, hash: this.hash,
      };
    }
  }

  // ------------------------------------------------------------------
  // Headers
  // ------------------------------------------------------------------

  const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;

  function headerName(name) {
    name = String(name);
    if (!TOKEN.test(name)) throw new TypeError(`Header name is not valid: "${name}"`);
    return name.toLowerCase();
  }
  function headerValue(value) {
    value = String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, "");
    if (/[\0\r\n]/.test(value)) throw new TypeError(`Header value is not valid: "${value}"`);
    return value;
  }

  let headerList;      // (headers) -> [[name, value], ...]
  let headersFromList; // ([[name, value], ...]) -> Headers  (trusted input from the HTTP parser)

  class Headers {
    #list = [];

    static {
      headerList = (h) => h.#list;
      headersFromList = (list) => {
        const h = new Headers();
        h.#list = list;
        return h;
      };
    }

    constructor(init) {
      if (init === undefined || init === null) return;
      if (init instanceof Headers) {
        this.#list = init.#list.map(([k, v]) => [k, v]);
      } else if (typeof init === "object" && typeof init[Symbol.iterator] === "function") {
        for (const pair of init) {
          const p = [...pair];
          if (p.length !== 2) throw new TypeError("Header pairs must contain exactly two items");
          this.append(p[0], p[1]);
        }
      } else if (typeof init === "object") {
        for (const k of Object.keys(init)) this.append(k, init[k]);
      } else {
        throw new TypeError("Headers init must be an object or an iterable of pairs");
      }
    }

    append(name, value) { this.#list.push([headerName(name), headerValue(value)]); }
    delete(name) {
      name = headerName(name);
      this.#list = this.#list.filter(([k]) => k !== name);
    }
    get(name) {
      name = headerName(name);
      let result = null;
      for (const [k, v] of this.#list) {
        if (k === name) result = result === null ? v : result + ", " + v;
      }
      return result;
    }
    getSetCookie() { return this.#list.filter(([k]) => k === "set-cookie").map(([, v]) => v); }
    has(name) {
      name = headerName(name);
      return this.#list.some(([k]) => k === name);
    }
    set(name, value) {
      name = headerName(name);
      value = headerValue(value);
      const i = this.#list.findIndex(([k]) => k === name);
      if (i < 0) {
        this.#list.push([name, value]);
        return;
      }
      this.#list[i] = [name, value];
      this.#list = this.#list.filter(([k], j) => k !== name || j === i);
    }
    forEach(callback, thisArg) { for (const [k, v] of this) callback.call(thisArg, v, k, this); }
    // Sorted by name; repeated headers are combined (except set-cookie).
    *entries() {
      const names = [...new Set(this.#list.map(([k]) => k))].sort();
      for (const n of names) {
        if (n === "set-cookie") for (const v of this.getSetCookie()) yield [n, v];
        else yield [n, this.get(n)];
      }
    }
    *keys() { for (const [k] of this.entries()) yield k; }
    *values() { for (const [, v] of this.entries()) yield v; }
    [Symbol.iterator]() { return this.entries(); }
    get [Symbol.toStringTag]() { return "Headers"; }
    [INSPECT]() { return Object.fromEntries(this.entries()); }
  }

  // ------------------------------------------------------------------
  // Body (shared by Request and Response)
  // ------------------------------------------------------------------

  // Normalizes a body init to string | Uint8Array | null (+ default content type).
  function extractBody(body) {
    if (body === null || body === undefined) return [null, null];
    if (typeof body === "string") return [body, "text/plain;charset=UTF-8"];
    if (body instanceof URLSearchParams) return [body.toString(), "application/x-www-form-urlencoded;charset=UTF-8"];
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return [toBytes(body).slice(), null];
    return [String(body), "text/plain;charset=UTF-8"];
  }

  let bodyData;     // (body) -> string | Uint8Array | null
  let setBodyData;  // (body, data) -> void

  class Body {
    #data = null;
    #used = false;

    static {
      bodyData = (b) => b.#data;
      setBodyData = (b, d) => { b.#data = d; };
    }

    get bodyUsed() { return this.#used; }
    get body() { return null; }  // ReadableStream is not implemented yet

    #take() {
      if (this.#used) throw new TypeError("Body is unusable: Body has already been read");
      this.#used = true;
      return this.#data;
    }
    async text() {
      const d = this.#take();
      if (d === null) return "";
      return typeof d === "string" ? d : utf8Decode(d, false);
    }
    async json() {
      return JSON.parse(await this.text());
    }
    async bytes() {
      const d = this.#take();
      if (d === null) return new Uint8Array(0);
      return typeof d === "string" ? utf8Encode(d) : d.slice();
    }
    async arrayBuffer() {
      return (await this.bytes()).buffer;
    }
    async formData() {
      throw new TypeError("formData() is not supported yet");
    }
  }

  // ------------------------------------------------------------------
  // Response
  // ------------------------------------------------------------------

  const NULL_BODY_STATUS = [101, 103, 204, 205, 304];

  class Response extends Body {
    #status = 200;
    #statusText = "";
    #headers;
    #type = "default";

    constructor(body = null, init = {}) {
      super();
      init = init ?? {};
      const status = init.status === undefined ? 200 : Number(init.status);
      if (!Number.isInteger(status) || status < 200 || status > 599) {
        throw new RangeError(`The status provided (${init.status}) is outside the range [200, 599].`);
      }
      const statusText = init.statusText === undefined ? "" : String(init.statusText);
      if (statusText && /[\r\n]/.test(statusText)) throw new TypeError("Invalid statusText");
      this.#status = status;
      this.#statusText = statusText;

      const [data, contentType] = extractBody(body);
      if (data !== null && NULL_BODY_STATUS.includes(status)) {
        throw new TypeError(`Response with null body status (${status}) cannot have body`);
      }
      setBodyData(this, data);
      if (init.headers === undefined) {
        // Fast path (the common `new Response("text")`): our own values need no validation.
        this.#headers = headersFromList(contentType ? [["content-type", contentType]] : []);
      } else {
        this.#headers = new Headers(init.headers);
        if (contentType && !this.#headers.has("content-type")) this.#headers.set("content-type", contentType);
      }
    }

    get status() { return this.#status; }
    get statusText() { return this.#statusText; }
    get ok() { return this.#status >= 200 && this.#status <= 299; }
    get headers() { return this.#headers; }
    get type() { return this.#type; }
    get url() { return ""; }
    get redirected() { return false; }

    clone() {
      if (this.bodyUsed) throw new TypeError("Response.clone: Body has already been consumed.");
      const r = new Response(null, { status: this.#status, statusText: this.#statusText, headers: this.#headers });
      setBodyData(r, bodyData(this));
      r.#type = this.#type;
      return r;
    }

    static json(data, init = {}) {
      const body = JSON.stringify(data);
      if (body === undefined) throw new TypeError("Value is not JSON serializable");
      const headers = new Headers(init?.headers);
      if (!headers.has("content-type")) headers.set("content-type", "application/json");
      return new Response(body, { ...init, headers });
    }

    static redirect(url, status = 302) {
      if (![301, 302, 303, 307, 308].includes(status)) throw new RangeError(`Invalid redirect status: ${status}`);
      return new Response(null, { status, headers: { location: String(url) } });
    }

    static error() {
      const r = new Response(null, { status: 200 });
      r.#status = 0;
      r.#type = "error";
      return r;
    }

    get [Symbol.toStringTag]() { return "Response"; }
    [INSPECT]() {
      return {
        status: this.#status, statusText: this.#statusText, ok: this.ok, headers: this.#headers,
        bodyUsed: this.bodyUsed, type: this.#type,
      };
    }
  }

  // ------------------------------------------------------------------
  // Request
  // ------------------------------------------------------------------

  const TRUSTED = Symbol("trusted request");
  const NORMALIZED_METHODS = ["DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"];

  class Request extends Body {
    #method = "GET";
    #url = "";
    #headers;

    constructor(input, init = {}) {
      super();
      if (input === TRUSTED) {  // fast path used by the HTTP server
        this.#url = init.url;
        this.#method = init.method;
        this.#headers = init.headers;
        setBodyData(this, init.body);
        return;
      }
      init = init ?? {};
      let source = null;
      if (input instanceof Request) {
        source = input;
        this.#url = input.url;
      } else {
        this.#url = new URL(String(input)).href;
      }

      let method = init.method ?? source?.method ?? "GET";
      method = String(method);
      if (!TOKEN.test(method)) throw new TypeError(`'${method}' is not a valid HTTP method.`);
      if (NORMALIZED_METHODS.includes(method.toUpperCase())) method = method.toUpperCase();
      this.#method = method;
      this.#headers = new Headers(init.headers ?? source?.headers);

      let data = null;
      let contentType = null;
      if (init.body !== undefined) [data, contentType] = extractBody(init.body);
      else if (source) data = bodyData(source);
      if (data !== null && (method === "GET" || method === "HEAD")) {
        throw new TypeError("Request with GET/HEAD method cannot have body.");
      }
      setBodyData(this, data);
      if (contentType && !this.#headers.has("content-type")) this.#headers.set("content-type", contentType);
    }

    get method() { return this.#method; }
    get url() { return this.#url; }
    get headers() { return this.#headers; }

    clone() {
      if (this.bodyUsed) throw new TypeError("Request.clone: Body has already been consumed.");
      return new Request(TRUSTED, {
        url: this.#url, method: this.#method, headers: new Headers(this.#headers), body: bodyData(this),
      });
    }

    get [Symbol.toStringTag]() { return "Request"; }
    [INSPECT]() {
      return { method: this.#method, url: this.#url, headers: this.#headers, bodyUsed: this.bodyUsed };
    }
  }

  define("TextEncoder", TextEncoder);
  define("TextDecoder", TextDecoder);
  define("URL", URL);
  define("URLSearchParams", URLSearchParams);
  define("Headers", Headers);
  define("Request", Request);
  define("Response", Response);

  internal.setUrlQuery = setUrlQuery;
  internal.headerList = headerList;
  internal.bodyData = bodyData;
  internal.makeHeaders = headersFromList;
  internal.makeRequest = (url, method, headers, body) =>
    new Request(TRUSTED, { url, method, headers, body });
});
