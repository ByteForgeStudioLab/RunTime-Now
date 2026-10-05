// crypto.getRandomValues(), crypto.randomUUID() and structuredClone().
(function (native, internal) {
  "use strict";

  const { randomFill } = native;
  const INSPECT = Symbol.for("rtn.inspect");

  function define(name, value) {
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  }

  // ------------------------------------------------------------------
  // crypto
  // ------------------------------------------------------------------

  const INTEGER_ARRAYS = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
    Int32Array, Uint32Array, BigInt64Array, BigUint64Array];
  const HEX = Array.from({ length: 256 }, (_, i) => i.toString(16).padStart(2, "0"));
  const ILLEGAL = Symbol("Crypto constructor");

  class Crypto {
    constructor(token) {
      if (token !== ILLEGAL) throw new TypeError("Illegal constructor");
    }

    getRandomValues(array) {
      if (!INTEGER_ARRAYS.some((T) => array instanceof T)) {
        throw new DOMException("The data argument must be an integer-type TypedArray", "TypeMismatchError");
      }
      if (array.byteLength > 65536) {
        throw new DOMException(
          `The ArrayBufferView's byte length (${array.byteLength}) exceeds the number of bytes of entropy available via this API (65536)`,
          "QuotaExceededError");
      }
      randomFill(new Uint8Array(array.buffer, array.byteOffset, array.byteLength));
      return array;
    }

    // RFC 9562 version 4 (random) UUID.
    randomUUID() {
      const b = new Uint8Array(16);
      randomFill(b);
      b[6] = (b[6] & 0x0f) | 0x40;
      b[8] = (b[8] & 0x3f) | 0x80;
      const h = Array.from(b, (x) => HEX[x]).join("");
      return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
    }

    get [Symbol.toStringTag]() { return "Crypto"; }
    [INSPECT]() { return {}; }
  }

  // ------------------------------------------------------------------
  // structuredClone (HTML "StructuredSerialize" for the ECMAScript types)
  // ------------------------------------------------------------------

  const ERROR_TYPES = { Error, EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError };
  // rtn's own platform objects can't be cloned (like in browsers).
  const NOT_CLONEABLE = ["URL", "URLSearchParams", "Headers", "Request", "Response", "AbortController",
    "AbortSignal", "EventTarget", "Event", "TextEncoder", "TextDecoder", "Crypto"]
    .map((name) => globalThis[name] ?? (name === "Crypto" ? Crypto : undefined)).filter(Boolean);
  const { toString } = Object.prototype;

  function dataCloneError(what) {
    return new DOMException(`${what} could not be cloned.`, "DataCloneError");
  }

  function structuredClone(value, options = undefined) {
    if (arguments.length === 0) {
      throw new TypeError("Failed to execute 'structuredClone': 1 argument required, but only 0 present.");
    }
    const memory = new Map();
    const transfer = options?.transfer === undefined ? [] : [...options.transfer];
    for (const buffer of transfer) {
      if (!(buffer instanceof ArrayBuffer)) throw dataCloneError("Value in transfer list");
      if (memory.has(buffer)) throw new DOMException("ArrayBuffer is duplicated in the transfer list", "DataCloneError");
      if (buffer.detached) throw new DOMException("An ArrayBuffer is detached and could not be cloned.", "DataCloneError");
      memory.set(buffer, buffer.slice(0));
    }
    const result = clone(value, memory);
    for (const buffer of transfer) buffer.transfer();  // detach the originals
    return result;
  }

  function clone(v, memory) {
    if (typeof v === "symbol") throw dataCloneError(String(v));
    if (typeof v === "function") throw dataCloneError(Function.prototype.toString.call(v).slice(0, 40));
    if (v === null || typeof v !== "object") return v;
    if (memory.has(v)) return memory.get(v);

    const tag = toString.call(v);
    let out;
    switch (tag) {
      case "[object Boolean]": out = Object(Boolean.prototype.valueOf.call(v)); break;
      case "[object Number]": out = Object(Number.prototype.valueOf.call(v)); break;
      case "[object String]": out = Object(String.prototype.valueOf.call(v)); break;
      case "[object BigInt]": out = Object(BigInt.prototype.valueOf.call(v)); break;
      case "[object Date]": out = new Date(v.getTime()); break;
      case "[object RegExp]": out = new RegExp(v.source, v.flags); break;
      case "[object ArrayBuffer]":
        if (v.detached) throw new DOMException("An ArrayBuffer is detached and could not be cloned.", "DataCloneError");
        out = v.slice(0);
        break;
    }
    if (out !== undefined) {
      memory.set(v, out);
      return out;
    }

    if (ArrayBuffer.isView(v)) {
      const buffer = clone(v.buffer, memory);
      out = v instanceof DataView
        ? new DataView(buffer, v.byteOffset, v.byteLength)
        : new v.constructor(buffer, v.byteOffset, v.length);
      memory.set(v, out);
      return out;
    }
    if (v instanceof Map) {
      out = new Map();
      memory.set(v, out);
      for (const [k, val] of [...v]) out.set(clone(k, memory), clone(val, memory));
      return out;
    }
    if (v instanceof Set) {
      out = new Set();
      memory.set(v, out);
      for (const k of [...v]) out.add(clone(k, memory));
      return out;
    }
    if (v instanceof DOMException) {
      out = new DOMException(v.message, v.name);
      memory.set(v, out);
      return out;
    }
    if (tag === "[object Error]") {
      const name = ERROR_TYPES[v.name] ? v.name : "Error";
      out = new ERROR_TYPES[name]();
      memory.set(v, out);
      const desc = Object.getOwnPropertyDescriptor(v, "message");
      if (desc && "value" in desc) {
        Object.defineProperty(out, "message", { value: String(desc.value), writable: true, configurable: true });
      }
      if (typeof v.stack === "string") {
        Object.defineProperty(out, "stack", { value: v.stack, writable: true, configurable: true });
      }
      if (Object.hasOwn(v, "cause")) {
        Object.defineProperty(out, "cause", { value: clone(v.cause, memory), writable: true, configurable: true });
      }
      return out;
    }
    if (v instanceof WeakMap || v instanceof WeakSet || v instanceof WeakRef || v instanceof Promise ||
        NOT_CLONEABLE.some((C) => v instanceof C)) {
      throw dataCloneError(tag === "[object Object]" ? `#<${v.constructor?.name ?? "Object"}>` : tag);
    }

    // Arrays and plain objects (class instances become plain objects, like in browsers).
    if (Array.isArray(v)) {
      out = new Array(v.length);
    } else {
      out = {};
    }
    memory.set(v, out);
    for (const key of Object.keys(v)) out[key] = clone(v[key], memory);
    return out;
  }

  Object.defineProperty(globalThis, "crypto", {
    value: new Crypto(ILLEGAL), writable: true, configurable: true, enumerable: true,
  });
  define("Crypto", Crypto);
  define("structuredClone", structuredClone);
});
