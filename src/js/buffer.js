// Buffer (global, and node:buffer): Node's byte array, a Uint8Array subclass.
(function (native, internal) {
  "use strict";

  const { utf8Encode, utf8Decode } = native;
  const INSPECT = Symbol.for("rtn.inspect");

  const ENCODINGS = new Map([
    ["utf8", "utf8"], ["utf-8", "utf8"], ["hex", "hex"], ["base64", "base64"], ["base64url", "base64url"],
    ["latin1", "latin1"], ["binary", "latin1"], ["ascii", "ascii"],
    ["utf16le", "utf16le"], ["utf-16le", "utf16le"], ["ucs2", "utf16le"], ["ucs-2", "utf16le"],
  ]);

  function normalizeEncoding(enc) {
    if (enc === undefined || enc === null) return "utf8";
    const e = ENCODINGS.get(String(enc).toLowerCase());
    if (!e) {
      const err = new TypeError(`Unknown encoding: ${enc}`);
      err.code = "ERR_UNKNOWN_ENCODING";
      throw err;
    }
    return e;
  }

  // string -> Uint8Array
  function encode(str, enc) {
    switch (enc) {
      case "utf8": return utf8Encode(str);
      case "hex": {
        const n = str.length >>> 1;
        const out = new Uint8Array(n);
        let i = 0;
        for (; i < n; i++) {
          const b = parseInt(str.substr(i * 2, 2), 16);
          if (Number.isNaN(b)) break;  // Node stops at the first invalid pair
          out[i] = b;
        }
        return i === n ? out : out.subarray(0, i);
      }
      case "base64":
      case "base64url": {
        let s = str.replace(/[^A-Za-z0-9+/\-_]/g, "").replace(/-/g, "+").replace(/_/g, "/");
        if (s.length % 4 === 1) s = s.slice(0, -1);
        s += "=".repeat((4 - (s.length % 4)) % 4);
        const bin = atob(s);
        const out = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
        return out;
      }
      case "latin1":
      case "ascii": {
        const out = new Uint8Array(str.length);
        for (let i = 0; i < str.length; i++) out[i] = str.charCodeAt(i) & 0xff;
        return out;
      }
      case "utf16le": {
        const out = new Uint8Array(str.length * 2);
        for (let i = 0; i < str.length; i++) {
          const c = str.charCodeAt(i);
          out[i * 2] = c & 0xff;
          out[i * 2 + 1] = c >>> 8;
        }
        return out;
      }
    }
  }

  function latin1String(bytes, mask = 0xff) {
    let s = "";
    for (let i = 0; i < bytes.length; i += 8192) {
      const chunk = bytes.subarray(i, i + 8192);
      s += String.fromCharCode.apply(null, mask === 0xff ? chunk : Array.from(chunk, (b) => b & mask));
    }
    return s;
  }

  // Uint8Array -> string
  function decode(bytes, enc) {
    switch (enc) {
      case "utf8": return utf8Decode(bytes, false);
      case "hex": {
        let s = "";
        for (const b of bytes) s += (b < 16 ? "0" : "") + b.toString(16);
        return s;
      }
      case "base64": return btoa(latin1String(bytes));
      case "base64url": return btoa(latin1String(bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
      case "latin1": return latin1String(bytes);
      case "ascii": return latin1String(bytes, 0x7f);
      case "utf16le": {
        let s = "";
        for (let i = 0; i + 1 < bytes.length; i += 2) s += String.fromCharCode(bytes[i] | (bytes[i + 1] << 8));
        return s;
      }
    }
  }

  function rangeError(name, value, range) {
    const e = new RangeError(`The value of "${name}" is out of range. It must be ${range}. Received ${value}`);
    e.code = "ERR_OUT_OF_RANGE";
    return e;
  }

  function checkOffset(buf, offset, size) {
    if (offset === undefined) offset = 0;
    if (!Number.isInteger(offset) || offset < 0 || offset + size > buf.length) {
      throw rangeError("offset", offset, `>= 0 and <= ${buf.length - size}`);
    }
    return offset;
  }

  function view(buf) {
    return new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  }

  class Buffer extends Uint8Array {
    static from(value, encodingOrOffset, length) {
      if (typeof value === "string") {
        const bytes = encode(value, normalizeEncoding(encodingOrOffset));
        return new Buffer(bytes.buffer, bytes.byteOffset, bytes.length);
      }
      if (value instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer)) {
        const offset = encodingOrOffset === undefined ? 0 : Number(encodingOrOffset);
        const len = length === undefined ? value.byteLength - offset : Number(length);
        return new Buffer(value, offset, len);  // shares memory, like Node
      }
      if (ArrayBuffer.isView(value)) {
        const b = new Buffer(value.byteLength);
        b.set(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
        return b;
      }
      if (value !== null && typeof value === "object") {
        if (value.type === "Buffer" && Array.isArray(value.data)) return Buffer.from(value.data);
        if (typeof value.length === "number" || Array.isArray(value)) {
          const b = new Buffer(value.length >>> 0);
          for (let i = 0; i < b.length; i++) b[i] = Number(value[i]) & 0xff;
          return b;
        }
        if (typeof value[Symbol.toPrimitive] === "function" || typeof value.valueOf === "function") {
          const prim = value.valueOf();
          if (prim !== value) return Buffer.from(prim, encodingOrOffset, length);
        }
      }
      const e = new TypeError("The first argument must be of type string or an instance of Buffer, ArrayBuffer, or Array or an Array-like Object.");
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }

    static alloc(size, fill, encoding) {
      if (!Number.isInteger(size) || size < 0) throw rangeError("size", size, ">= 0");
      const b = new Buffer(size);
      if (fill !== undefined && fill !== 0) b.fill(fill, 0, size, encoding);
      return b;
    }
    static allocUnsafe(size) { return Buffer.alloc(size); }
    static allocUnsafeSlow(size) { return Buffer.alloc(size); }

    static byteLength(value, encoding) {
      if (typeof value !== "string") {
        if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer) return value.byteLength;
        throw new TypeError('The "string" argument must be of type string or an instance of Buffer or ArrayBuffer.');
      }
      const enc = normalizeEncoding(encoding);
      if (enc === "utf8") return utf8Encode(value).length;
      return encode(value, enc).length;
    }

    static concat(list, totalLength) {
      if (!Array.isArray(list)) throw new TypeError('The "list" argument must be an instance of Array.');
      const total = totalLength ?? list.reduce((n, b) => n + b.length, 0);
      const out = Buffer.alloc(total);
      let pos = 0;
      for (const b of list) {
        if (pos >= total) break;
        const chunk = b.length > total - pos ? b.subarray(0, total - pos) : b;
        out.set(chunk, pos);
        pos += chunk.length;
      }
      return out;
    }

    static isBuffer(value) { return value instanceof Buffer; }
    static isEncoding(enc) { return typeof enc === "string" && ENCODINGS.has(enc.toLowerCase()); }
    static compare(a, b) { return a.compare(b); }

    get parent() { return this.buffer; }
    get offset() { return this.byteOffset; }

    toString(encoding, start = 0, end = this.length) {
      start = Math.max(0, start | 0);
      end = Math.min(this.length, end === undefined ? this.length : end | 0);
      if (end <= start) return "";
      return decode(this.subarray(start, end), normalizeEncoding(encoding));
    }
    toLocaleString(encoding, start, end) { return this.toString(encoding, start, end); }

    toJSON() { return { type: "Buffer", data: Array.from(this) }; }

    write(string, offset, length, encoding) {
      if (typeof offset === "string") [encoding, offset, length] = [offset, 0, undefined];
      else if (typeof length === "string") [encoding, length] = [length, undefined];
      offset = offset === undefined ? 0 : offset >>> 0;
      const bytes = encode(String(string), normalizeEncoding(encoding));
      let n = Math.min(bytes.length, this.length - offset, length === undefined ? Infinity : length >>> 0);
      // Don't write half of a UTF-8 character.
      if (n < bytes.length && normalizeEncoding(encoding) === "utf8") {
        while (n > 0 && (bytes[n] & 0xc0) === 0x80) n--;
      }
      this.set(bytes.subarray(0, n), offset);
      return n;
    }

    fill(value, offset = 0, end = this.length, encoding) {
      if (typeof offset === "string") [encoding, offset, end] = [offset, 0, this.length];
      else if (typeof end === "string") [encoding, end] = [end, this.length];
      if (typeof value === "string") {
        const bytes = value.length === 1 && normalizeEncoding(encoding) === "utf8" && value.charCodeAt(0) < 128
          ? [value.charCodeAt(0)] : encode(value, normalizeEncoding(encoding));
        if (bytes.length === 0) return super.fill(0, offset, end);
        for (let i = offset, j = 0; i < end; i++, j = (j + 1) % bytes.length) this[i] = bytes[j];
        return this;
      }
      if (ArrayBuffer.isView(value)) {
        const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        for (let i = offset, j = 0; i < end; i++, j = (j + 1) % bytes.length) this[i] = bytes[j];
        return this;
      }
      return super.fill(Number(value) & 0xff, offset, end);
    }

    equals(other) {
      if (!(other instanceof Uint8Array)) throw new TypeError('The "otherBuffer" argument must be an instance of Buffer or Uint8Array.');
      return this.compare(other) === 0;
    }

    compare(target, targetStart = 0, targetEnd = target.length, sourceStart = 0, sourceEnd = this.length) {
      const a = this.subarray(sourceStart, sourceEnd);
      const b = target.subarray(targetStart, targetEnd);
      const n = Math.min(a.length, b.length);
      for (let i = 0; i < n; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
      return a.length === b.length ? 0 : a.length < b.length ? -1 : 1;
    }

    copy(target, targetStart = 0, sourceStart = 0, sourceEnd = this.length) {
      const chunk = this.subarray(sourceStart, Math.min(sourceEnd, sourceStart + (target.length - targetStart)));
      target.set(chunk, targetStart);
      return chunk.length;
    }

    // Node's slice() shares memory (it is subarray(), not a copy).
    slice(start, end) { return this.subarray(start, end); }

    indexOf(value, byteOffset = 0, encoding) {
      if (typeof byteOffset === "string") [encoding, byteOffset] = [byteOffset, 0];
      if (byteOffset < 0) byteOffset = Math.max(0, this.length + byteOffset);
      if (typeof value === "number") return super.indexOf(value & 0xff, byteOffset);
      const needle = typeof value === "string" ? encode(value, normalizeEncoding(encoding)) : value;
      if (needle.length === 0) return byteOffset <= this.length ? byteOffset : this.length;
      outer: for (let i = byteOffset; i <= this.length - needle.length; i++) {
        for (let j = 0; j < needle.length; j++) if (this[i + j] !== needle[j]) continue outer;
        return i;
      }
      return -1;
    }
    lastIndexOf(value, byteOffset = this.length, encoding) {
      const needle = typeof value === "number" ? [value & 0xff]
        : typeof value === "string" ? encode(value, normalizeEncoding(encoding)) : value;
      for (let i = Math.min(byteOffset, this.length - needle.length); i >= 0; i--) {
        let j = 0;
        while (j < needle.length && this[i + j] === needle[j]) j++;
        if (j === needle.length) return i;
      }
      return -1;
    }
    includes(value, byteOffset, encoding) { return this.indexOf(value, byteOffset, encoding) !== -1; }

    swap16() {
      for (let i = 0; i + 1 < this.length; i += 2) [this[i], this[i + 1]] = [this[i + 1], this[i]];
      return this;
    }

    get [Symbol.toStringTag]() { return "Uint8Array"; }

    [INSPECT]() {
      const max = 50;
      let s = "<Buffer";
      for (let i = 0; i < Math.min(this.length, max); i++) s += " " + (this[i] < 16 ? "0" : "") + this[i].toString(16);
      if (this.length > max) s += ` ... ${this.length - max} more byte${this.length - max === 1 ? "" : "s"}`;
      return s + ">";
    }
  }

  // readUInt16LE, writeInt32BE, readDoubleLE, readBigUInt64LE, ... (and the lowercase "Uint" aliases)
  const NUMERIC = [
    ["UInt8", 1, "getUint8", "setUint8"], ["Int8", 1, "getInt8", "setInt8"],
    ["UInt16", 2, "getUint16", "setUint16"], ["Int16", 2, "getInt16", "setInt16"],
    ["UInt32", 4, "getUint32", "setUint32"], ["Int32", 4, "getInt32", "setInt32"],
    ["Float", 4, "getFloat32", "setFloat32"], ["Double", 8, "getFloat64", "setFloat64"],
    ["BigUInt64", 8, "getBigUint64", "setBigUint64"], ["BigInt64", 8, "getBigInt64", "setBigInt64"],
  ];
  for (const [name, size, get, set] of NUMERIC) {
    for (const endian of size === 1 ? [""] : ["LE", "BE"]) {
      const little = endian === "LE";
      const read = function (offset) {
        offset = checkOffset(this, offset, size);
        return view(this)[get](offset, little);
      };
      const write = function (value, offset) {
        offset = checkOffset(this, offset, size);
        view(this)[set](offset, value, little);
        return offset + size;
      };
      for (const n of new Set([name, name.replace("UInt", "Uint")])) {
        Object.defineProperty(Buffer.prototype, `read${n}${endian}`, { value: read, writable: true, configurable: true });
        Object.defineProperty(Buffer.prototype, `write${n}${endian}`, { value: write, writable: true, configurable: true });
      }
    }
  }

  const bufferModule = {
    Buffer,
    kMaxLength: 2 ** 32,
    constants: { MAX_LENGTH: 2 ** 32, MAX_STRING_LENGTH: 2 ** 29 },
    atob,
    btoa,
    isUtf8: (input) => { try { new TextDecoder("utf-8", { fatal: true }).decode(input); return true; } catch { return false; } },
  };

  Object.defineProperty(globalThis, "Buffer", { value: Buffer, writable: true, configurable: true, enumerable: false });
  internal.modules.buffer = bufferModule;
});
