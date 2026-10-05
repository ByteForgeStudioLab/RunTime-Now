// Node.js compatibility: node:events, node:util, node:os, node:assert, node:url,
// node:crypto, node:timers, node:process, plus the globals npm packages expect
// (global, setImmediate, process.on / emitWarning / versions.node ...).
(function (native, internal) {
  "use strict";

  const INSPECT = Symbol.for("rtn.inspect");
  const { toString: objectToString } = Object.prototype;
  const inspect = (value, opts) => native.inspect(value, Boolean(opts?.colors));
  const format = (...args) => native.format(...args);

  function argTypeError(name, expected, value) {
    const e = new TypeError(`The "${name}" argument must be ${expected}. Received ${value === null ? "null" : typeof value}`);
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  }

  // ------------------------------------------------------------------
  // Deep equality (assert.deepStrictEqual, util.isDeepStrictEqual, expect().toEqual)
  // ------------------------------------------------------------------

  function deepEqual(a, b, strict, memo = new Map()) {
    if (strict ? Object.is(a, b) : a == b || (a !== a && b !== b)) return true;  // eslint-disable-line eqeqeq
    const aObj = typeof a === "object" && a !== null || typeof a === "function";
    const bObj = typeof b === "object" && b !== null || typeof b === "function";
    if (!aObj || !bObj) return false;
    if (typeof a === "function" || typeof b === "function") return false;
    if (strict && Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
    const tag = objectToString.call(a);
    if (tag !== objectToString.call(b)) return false;
    if (Array.isArray(a) !== Array.isArray(b)) return false;

    let seen = memo.get(a);
    if (seen?.has(b)) return true;
    if (!seen) memo.set(a, (seen = new Set()));
    seen.add(b);

    if (a instanceof Date) return a.getTime() === b.getTime();
    if (a instanceof RegExp) return a.source === b.source && a.flags === b.flags && a.lastIndex === b.lastIndex;
    if (a instanceof Error && (a.message !== b.message || a.name !== b.name)) return false;
    if (["[object Number]", "[object String]", "[object Boolean]", "[object BigInt]", "[object Symbol]"].includes(tag)) {
      if (!Object.is(a.valueOf(), b.valueOf())) return false;
    }
    if (ArrayBuffer.isView(a) || a instanceof ArrayBuffer) {
      const x = a instanceof ArrayBuffer ? new Uint8Array(a) : new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
      const y = b instanceof ArrayBuffer ? new Uint8Array(b) : new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
      if (x.length !== y.length) return false;
      for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
      if (!(a instanceof DataView) && !(a instanceof ArrayBuffer)) {
        // still compare extra (non-index) properties below
        const ka = Object.keys(a).filter((k) => !/^\d+$/.test(k));
        const kb = Object.keys(b).filter((k) => !/^\d+$/.test(k));
        if (ka.length !== kb.length) return false;
        return ka.every((k) => Object.hasOwn(b, k) && deepEqual(a[k], b[k], strict, memo));
      }
      return true;
    }
    if (a instanceof Map) {
      if (a.size !== b.size) return false;
      outer: for (const [k, v] of a) {
        if (b.has(k)) {
          if (!deepEqual(v, b.get(k), strict, memo)) return false;
          continue;
        }
        if (typeof k !== "object" || k === null) return false;
        for (const [k2, v2] of b) {
          if (deepEqual(k, k2, strict, memo) && deepEqual(v, v2, strict, memo)) continue outer;
        }
        return false;
      }
      return true;
    }
    if (a instanceof Set) {
      if (a.size !== b.size) return false;
      outer: for (const v of a) {
        if (b.has(v)) continue;
        if (typeof v !== "object" || v === null) return false;
        for (const v2 of b) if (deepEqual(v, v2, strict, memo)) continue outer;
        return false;
      }
      return true;
    }
    const ka = Object.keys(a);
    const kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
      if (!Object.hasOwn(b, k) || !deepEqual(a[k], b[k], strict, memo)) return false;
    }
    if (strict) {
      const sa = Object.getOwnPropertySymbols(a).filter((s) => Object.prototype.propertyIsEnumerable.call(a, s));
      const sb = Object.getOwnPropertySymbols(b).filter((s) => Object.prototype.propertyIsEnumerable.call(b, s));
      if (sa.length !== sb.length) return false;
      for (const s of sa) if (!deepEqual(a[s], b[s], strict, memo)) return false;
    }
    return true;
  }
  internal.deepEqual = deepEqual;

  // ------------------------------------------------------------------
  // events: EventEmitter (a plain constructor function, so `EventEmitter.call(this)` works)
  // ------------------------------------------------------------------

  const kErrorMonitor = Symbol("events.errorMonitor");

  function checkListener(listener) {
    if (typeof listener !== "function") throw argTypeError("listener", "of type function", listener);
  }

  function EventEmitter(opts) {
    EventEmitter.init.call(this, opts);
  }
  EventEmitter.prototype._events = undefined;
  EventEmitter.prototype._eventsCount = 0;
  EventEmitter.prototype._maxListeners = undefined;
  EventEmitter.defaultMaxListeners = 10;
  EventEmitter.errorMonitor = kErrorMonitor;
  EventEmitter.captureRejections = false;
  EventEmitter.init = function () {
    if (this._events === undefined || this._events === Object.getPrototypeOf(this)._events) {
      this._events = Object.create(null);
      this._eventsCount = 0;
    }
    this._maxListeners = this._maxListeners || undefined;
  };

  function addListener(target, type, listener, prepend) {
    checkListener(listener);
    if (target._events === undefined) EventEmitter.init.call(target);
    if (target._events.newListener !== undefined) target.emit("newListener", type, listener.listener ?? listener);
    const list = target._events[type];
    if (list === undefined) {
      target._events[type] = listener;
      target._eventsCount++;
    } else if (typeof list === "function") {
      target._events[type] = prepend ? [listener, list] : [list, listener];
    } else if (prepend) {
      list.unshift(listener);
    } else {
      list.push(listener);
    }
    const max = target.getMaxListeners();
    const count = target.listenerCount(type);
    if (max > 0 && count > max && !target._events[type].warned) {
      if (typeof target._events[type] === "object") target._events[type].warned = true;
      process.emitWarning(`Possible EventEmitter memory leak detected. ${count} ${String(type)} listeners added. ` +
        "Use emitter.setMaxListeners() to increase limit", "MaxListenersExceededWarning");
    }
    return target;
  }

  function onceWrapper(target, type, listener) {
    const state = { fired: false, target, type, listener };
    const wrapped = function (...args) {
      if (state.fired) return undefined;
      state.fired = true;
      state.target.removeListener(state.type, wrapped);
      return state.listener.apply(state.target, args);
    };
    wrapped.listener = listener;
    return wrapped;
  }

  Object.assign(EventEmitter.prototype, {
    setMaxListeners(n) {
      if (typeof n !== "number" || n < 0 || Number.isNaN(n)) throw new RangeError(`The value of "n" is out of range. Received ${n}`);
      this._maxListeners = n;
      return this;
    },
    getMaxListeners() {
      return this._maxListeners === undefined ? EventEmitter.defaultMaxListeners : this._maxListeners;
    },
    emit(type, ...args) {
      const events = this._events;
      if (type === "error" && events?.[kErrorMonitor] !== undefined) this.emit(kErrorMonitor, ...args);
      const handler = events?.[type];
      if (handler === undefined) {
        if (type === "error") {
          const err = args[0];
          if (err instanceof Error) throw err;
          const e = new Error(`Unhandled error. (${inspect(err)})`);
          e.code = "ERR_UNHANDLED_ERROR";
          e.context = err;
          throw e;
        }
        return false;
      }
      if (typeof handler === "function") {
        Reflect.apply(handler, this, args);
      } else {
        for (const fn of handler.slice()) Reflect.apply(fn, this, args);
      }
      return true;
    },
    addListener(type, listener) { return addListener(this, type, listener, false); },
    on(type, listener) { return addListener(this, type, listener, false); },
    prependListener(type, listener) { return addListener(this, type, listener, true); },
    once(type, listener) {
      checkListener(listener);
      return this.on(type, onceWrapper(this, type, listener));
    },
    prependOnceListener(type, listener) {
      checkListener(listener);
      return this.prependListener(type, onceWrapper(this, type, listener));
    },
    removeListener(type, listener) {
      checkListener(listener);
      const events = this._events;
      const list = events?.[type];
      if (list === undefined) return this;
      if (list === listener || list.listener === listener) {
        if (--this._eventsCount === 0) this._events = Object.create(null);
        else delete events[type];
        if (events.removeListener) this.emit("removeListener", type, list.listener ?? listener);
      } else if (typeof list !== "function") {
        for (let i = list.length - 1; i >= 0; i--) {
          if (list[i] === listener || list[i].listener === listener) {
            const removed = list[i];
            list.splice(i, 1);
            if (list.length === 1) events[type] = list[0];
            if (events.removeListener !== undefined) this.emit("removeListener", type, removed.listener ?? removed);
            break;
          }
        }
      }
      return this;
    },
    off(type, listener) { return this.removeListener(type, listener); },
    removeAllListeners(type) {
      if (this._events === undefined) return this;
      if (type === undefined) {
        for (const key of Reflect.ownKeys(this._events)) {
          if (key !== "removeListener") this.removeAllListeners(key);
        }
        this._events = Object.create(null);
        this._eventsCount = 0;
        return this;
      }
      const list = this._events[type];
      if (typeof list === "function") this.removeListener(type, list);
      else if (list !== undefined) for (let i = list.length - 1; i >= 0; i--) this.removeListener(type, list[i]);
      return this;
    },
    listeners(type) {
      const list = this._events?.[type];
      if (list === undefined) return [];
      return typeof list === "function" ? [list.listener ?? list] : list.map((l) => l.listener ?? l);
    },
    rawListeners(type) {
      const list = this._events?.[type];
      if (list === undefined) return [];
      return typeof list === "function" ? [list] : list.slice();
    },
    listenerCount(type, listener) {
      const list = this._events?.[type];
      if (list === undefined) return 0;
      const all = typeof list === "function" ? [list] : list;
      return listener === undefined ? all.length : all.filter((l) => l === listener || l.listener === listener).length;
    },
    eventNames() {
      return this._events ? Reflect.ownKeys(this._events) : [];
    },
  });

  // await once(emitter, "ready") -> the event's arguments
  EventEmitter.once = function once(emitter, name, options = {}) {
    return new Promise((resolve, reject) => {
      const signal = options?.signal;
      if (signal?.aborted) return reject(signal.reason);
      if (typeof emitter.addEventListener === "function" && typeof emitter.on !== "function") {
        emitter.addEventListener(name, (...args) => resolve(args), { once: true, signal });
        return;
      }
      const onError = (err) => {
        emitter.removeListener(name, onEvent);
        reject(err);
      };
      const onEvent = (...args) => {
        if (name !== "error") emitter.removeListener("error", onError);
        resolve(args);
      };
      emitter.once(name, onEvent);
      if (name !== "error") emitter.once("error", onError);
      signal?.addEventListener("abort", () => {
        emitter.removeListener(name, onEvent);
        emitter.removeListener("error", onError);
        reject(signal.reason);
      }, { once: true });
    });
  };

  // for await (const [value] of on(emitter, "data")) { ... }
  EventEmitter.on = function on(emitter, name, options = {}) {
    const queue = [];
    const waiting = [];
    let error = null;
    let done = false;
    const push = (...args) => {
      if (waiting.length) waiting.shift().resolve({ value: args, done: false });
      else queue.push(args);
    };
    const fail = (err) => {
      error = err;
      if (waiting.length) waiting.shift().reject(err);
    };
    emitter.on(name, push);
    if (name !== "error") emitter.on("error", fail);
    const close = () => {
      done = true;
      emitter.removeListener(name, push);
      emitter.removeListener("error", fail);
      for (const w of waiting.splice(0)) w.resolve({ value: undefined, done: true });
    };
    options?.signal?.addEventListener("abort", close, { once: true });
    return {
      next() {
        if (queue.length) return Promise.resolve({ value: queue.shift(), done: false });
        if (error) {
          const e = error;
          error = null;
          return Promise.reject(e);
        }
        if (done) return Promise.resolve({ value: undefined, done: true });
        return new Promise((resolve, reject) => waiting.push({ resolve, reject }));
      },
      return() {
        close();
        return Promise.resolve({ value: undefined, done: true });
      },
      [Symbol.asyncIterator]() { return this; },
    };
  };

  EventEmitter.listenerCount = (emitter, type) => emitter.listenerCount(type);
  EventEmitter.getEventListeners = (emitter, type) =>
    typeof emitter.listeners === "function" ? emitter.listeners(type) : [];
  EventEmitter.setMaxListeners = (n = EventEmitter.defaultMaxListeners, ...targets) => {
    if (targets.length === 0) EventEmitter.defaultMaxListeners = n;
    for (const t of targets) t.setMaxListeners?.(n);
  };
  EventEmitter.EventEmitter = EventEmitter;
  EventEmitter.usingDomains = false;
  for (const k of ["once", "on", "listenerCount", "getEventListeners", "setMaxListeners", "EventEmitter",
    "defaultMaxListeners", "errorMonitor", "captureRejections", "init"]) {
    Object.defineProperty(EventEmitter, k, { enumerable: k !== "init" });
  }

  // ------------------------------------------------------------------
  // process: EventEmitter methods, exit events, warnings, Node-compatible versions
  // ------------------------------------------------------------------

  // Node version whose APIs rtn follows; packages check process.versions.node / process.version.
  const NODE_COMPAT = "22.12.0";
  Object.setPrototypeOf(process, EventEmitter.prototype);
  EventEmitter.init.call(process);
  for (const k of ["_events", "_eventsCount", "_maxListeners"]) Object.defineProperty(process, k, { enumerable: false });
  process.versions.node = NODE_COMPAT;
  Object.defineProperty(process, "version", { value: "v" + NODE_COMPAT, writable: true, configurable: true, enumerable: true });
  process.release = { name: "rtn", lts: false };
  process.argv0 = "rtn";
  process.execArgv = [];
  process.config = { variables: {} };
  process.features = { inspector: false, ipv6: true, tls: false, typescript: "strip" };
  process.browser = false;
  if (!process.umask) process.umask = () => 0o22;
  process.getuid = () => native.osInfo().uid;
  process.getgid = () => native.osInfo().gid;
  process.cpuUsage = () => ({ user: 0, system: 0 });
  process.binding = () => { throw new Error("process.binding is not supported"); };
  process.emitWarning = function emitWarning(warning, type, code) {
    if (typeof type === "object" && type !== null) ({ type, code } = type);
    const name = warning instanceof Error ? warning.name : type || "Warning";
    const message = warning instanceof Error ? warning.message : String(warning);
    if (process.listenerCount("warning") > 0) {
      const w = warning instanceof Error ? warning : Object.assign(new Error(message), { name });
      if (code) w.code = code;
      queueMicrotask(() => process.emit("warning", w));
      return;
    }
    process.stderr.write(`(rtn:${process.pid}) ${code ? `[${code}] ` : ""}${name}: ${message}\n`);
  };
  for (const stream of [process.stdout, process.stderr]) {
    Object.setPrototypeOf(stream, EventEmitter.prototype);
    EventEmitter.init.call(stream);
    stream.writable = true;
    stream.end = () => {};
    Object.defineProperty(stream, "columns", { get: () => Number(process.env.COLUMNS) || 80, configurable: true });
    Object.defineProperty(stream, "rows", { get: () => Number(process.env.LINES) || 24, configurable: true });
    stream.hasColors = (count = 16) => Boolean(stream.isTTY) && !process.env.NO_COLOR && count <= 16 ** 6;
    stream.getColorDepth = () => (stream.isTTY ? (/truecolor|24bit/.test(process.env.COLORTERM ?? "") ? 24 : 8) : 1);
  }

  // 'exit' listeners run on normal exit (src/runtime.cpp) and on process.exit().
  let exiting = false;
  internal.emitExit = (code) => {
    if (exiting) return;
    exiting = true;
    process.emit("exit", process.exitCode ?? code);
  };
  const nativeExit = process.exit;
  process.exit = function exit(code) {
    if (code !== undefined) process.exitCode = code;
    internal.emitExit(process.exitCode ?? 0);
    return nativeExit(process.exitCode ?? 0);
  };
  process.reallyExit = nativeExit;

  // ------------------------------------------------------------------
  // Globals npm packages expect
  // ------------------------------------------------------------------

  function defineGlobal(name, value) {
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  }
  defineGlobal("global", globalThis);
  defineGlobal("setImmediate", (fn, ...args) => {
    if (typeof fn !== "function") throw argTypeError("callback", "of type function", fn);
    return setTimeout(fn, 0, ...args);
  });
  defineGlobal("clearImmediate", (id) => clearTimeout(id));

  // ------------------------------------------------------------------
  // util
  // ------------------------------------------------------------------

  const kCustomPromisify = Symbol.for("nodejs.util.promisify.custom");

  function promisify(original) {
    if (typeof original !== "function") throw argTypeError("original", "of type function", original);
    if (original[kCustomPromisify]) return original[kCustomPromisify];
    function fn(...args) {
      return new Promise((resolve, reject) => {
        Reflect.apply(original, this, [...args, (err, ...values) => {
          if (err) reject(err);
          else resolve(values.length > 1 ? values : values[0]);
        }]);
      });
    }
    Object.setPrototypeOf(fn, Object.getPrototypeOf(original));
    Object.defineProperty(fn, kCustomPromisify, { value: fn });
    return Object.defineProperties(fn, Object.getOwnPropertyDescriptors(original));
  }
  promisify.custom = kCustomPromisify;

  const warned = new Set();
  const isType = (tag) => (v) => objectToString.call(v) === `[object ${tag}]`;
  const types = {
    isPromise: (v) => v instanceof Promise,
    isDate: (v) => v instanceof Date,
    isRegExp: (v) => v instanceof RegExp,
    isMap: (v) => v instanceof Map,
    isSet: (v) => v instanceof Set,
    isWeakMap: (v) => v instanceof WeakMap,
    isWeakSet: (v) => v instanceof WeakSet,
    isNativeError: (v) => v instanceof Error,
    isTypedArray: (v) => ArrayBuffer.isView(v) && !(v instanceof DataView),
    isUint8Array: (v) => v instanceof Uint8Array,
    isArrayBuffer: (v) => v instanceof ArrayBuffer,
    isAnyArrayBuffer: (v) => v instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && v instanceof SharedArrayBuffer),
    isArrayBufferView: (v) => ArrayBuffer.isView(v),
    isDataView: (v) => v instanceof DataView,
    isAsyncFunction: isType("AsyncFunction"),
    isGeneratorFunction: (v) => isType("GeneratorFunction")(v) || isType("AsyncGeneratorFunction")(v),
    isGeneratorObject: isType("Generator"),
    isBoxedPrimitive: (v) => ["Number", "String", "Boolean", "BigInt", "Symbol"].some((t) => typeof v === "object" && isType(t)(v)),
    isProxy: () => false,
  };

  const util = {
    format,
    formatWithOptions: (_opts, ...args) => format(...args),
    inspect: Object.assign(inspect, { custom: Symbol.for("nodejs.util.inspect.custom"), defaultOptions: {} }),
    promisify,
    callbackify(original) {
      if (typeof original !== "function") throw argTypeError("original", "of type function", original);
      return function (...args) {
        const cb = args.pop();
        Reflect.apply(original, this, args).then((v) => queueMicrotask(() => cb(null, v)),
          (e) => queueMicrotask(() => cb(e ?? new Error("Promise was rejected with a falsy value"))));
      };
    },
    deprecate(fn, message, code) {
      return function (...args) {
        if (!warned.has(code ?? message)) {
          warned.add(code ?? message);
          process.emitWarning(message, "DeprecationWarning", code);
        }
        return new.target ? Reflect.construct(fn, args, new.target) : Reflect.apply(fn, this, args);
      };
    },
    inherits(ctor, superCtor) {
      Object.defineProperty(ctor, "super_", { value: superCtor, writable: true, configurable: true });
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    },
    isDeepStrictEqual: (a, b) => deepEqual(a, b, true),
    debuglog: (section) => {
      const enabled = (process.env.NODE_DEBUG ?? "").split(",").some((s) => s.trim().toLowerCase() === String(section).toLowerCase());
      const log = (...args) => { if (enabled) process.stderr.write(`${String(section).toUpperCase()} ${process.pid}: ${format(...args)}\n`); };
      log.enabled = enabled;
      return log;
    },
    stripVTControlCharacters: (s) => String(s).replace(/\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07/g, ""),
    styleText(style, text) {
      const codes = { reset: 0, bold: 1, dim: 2, italic: 3, underline: 4, inverse: 7, red: 31, green: 32,
        yellow: 33, blue: 34, magenta: 35, cyan: 36, white: 37, gray: 90, grey: 90 };
      let out = String(text);
      for (const s of Array.isArray(style) ? style : [style]) {
        if (!(s in codes)) throw new TypeError(`Invalid style: ${s}`);
        out = `\x1b[${codes[s]}m${out}\x1b[${s === "bold" || s === "dim" ? 22 : s === "italic" ? 23 : s === "underline" ? 24 : 39}m`;
      }
      return out;
    },
    types,
    TextEncoder,
    TextDecoder,
    isArray: Array.isArray,
    isBoolean: (v) => typeof v === "boolean",
    isNumber: (v) => typeof v === "number",
    isString: (v) => typeof v === "string",
    isFunction: (v) => typeof v === "function",
    isObject: (v) => v !== null && typeof v === "object",
    isPrimitive: (v) => v === null || (typeof v !== "object" && typeof v !== "function"),
    isError: (v) => v instanceof Error,
    isRegExp: (v) => v instanceof RegExp,
    isDate: (v) => v instanceof Date,
    isNullOrUndefined: (v) => v === null || v === undefined,
    isUndefined: (v) => v === undefined,
  };

  // ------------------------------------------------------------------
  // os
  // ------------------------------------------------------------------

  const info = () => native.osInfo();
  const os = {
    EOL: "\n",
    devNull: "/dev/null",
    platform: () => "linux",
    type: () => info().type ?? "Linux",
    arch: () => process.arch,
    machine: () => info().machine ?? process.arch,
    release: () => info().release ?? "",
    version: () => info().version ?? "",
    hostname: () => info().hostname,
    homedir: () => process.env.HOME || info().pwHome || "/",
    tmpdir: () => (process.env.TMPDIR || process.env.TMP || process.env.TEMP || "/tmp").replace(/(.)\/+$/, "$1"),
    endianness: () => (new Uint8Array(new Uint16Array([1]).buffer)[0] === 1 ? "LE" : "BE"),
    totalmem: () => info().totalmem ?? 0,
    freemem: () => info().freemem ?? 0,
    availableParallelism: () => native.cpuCount(),
    uptime: () => info().uptime ?? 0,
    loadavg: () => info().loadavg ?? [0, 0, 0],
    cpus() {
      const models = info().cpuModels;
      const n = native.cpuCount();
      return Array.from({ length: n }, (_, i) => ({
        model: models[i] ?? models[0] ?? "unknown", speed: 0, times: { user: 0, nice: 0, sys: 0, idle: 0, irq: 0 },
      }));
    },
    networkInterfaces: () => ({}),
    userInfo() {
      const i = info();
      return { uid: i.uid, gid: i.gid, username: i.username ?? process.env.USER ?? "", homedir: i.pwHome ?? os.homedir(), shell: i.shell ?? null };
    },
    constants: {
      signals: { SIGHUP: 1, SIGINT: 2, SIGQUIT: 3, SIGKILL: 9, SIGUSR1: 10, SIGSEGV: 11, SIGUSR2: 12, SIGPIPE: 13, SIGALRM: 14, SIGTERM: 15 },
      errno: {},
      priority: {},
    },
  };

  // ------------------------------------------------------------------
  // assert
  // ------------------------------------------------------------------

  class AssertionError extends Error {
    constructor(options = {}) {
      const { message, actual, expected, operator } = options;
      super(message ?? `${inspect(actual)} ${operator} ${inspect(expected)}`);
      this.name = "AssertionError";
      this.code = "ERR_ASSERTION";
      this.actual = actual;
      this.expected = expected;
      this.operator = operator;
      this.generatedMessage = message === undefined;
    }
  }

  function fail(actual, expected, message, operator, defaultMessage) {
    if (message instanceof Error) throw message;
    throw new AssertionError({ message: message ?? defaultMessage, actual, expected, operator });
  }

  // Does a thrown error match what assert.throws() was told to expect?
  function errorMatches(err, expected) {
    if (expected === undefined) return true;
    if (expected instanceof RegExp) return expected.test(String(err?.message ?? err)) || expected.test(String(err));
    if (typeof expected === "function") {
      if (expected.prototype !== undefined && err instanceof expected) return true;
      if (Error.isPrototypeOf(expected) || expected === Error) return false;
      return expected.call({}, err) === true;
    }
    if (typeof expected === "object" && expected !== null) {
      return Object.keys(expected).every((k) => (expected[k] instanceof RegExp && typeof err?.[k] === "string")
        ? expected[k].test(err[k]) : deepEqual(err?.[k], expected[k], true));
    }
    return false;
  }

  function makeAssert(strict) {
    function assert(value, message) {
      if (!value) fail(value, true, message, "==", arguments.length === 0 ? "No value argument passed to `assert.ok()`" : "The expression evaluated to a falsy value:\n\n  assert.ok(" + inspect(value) + ")\n");
    }
    const a = Object.assign(assert, {
      ok: assert,
      fail(message = "Failed") { fail(undefined, undefined, message, "fail"); },
      equal: strict ? undefined : (actual, expected, message) => {
        if (!(actual == expected || (actual !== actual && expected !== expected))) fail(actual, expected, message, "==", `${inspect(actual)} == ${inspect(expected)}`); // eslint-disable-line eqeqeq
      },
      notEqual: strict ? undefined : (actual, expected, message) => {
        if (actual == expected || (actual !== actual && expected !== expected)) fail(actual, expected, message, "!=", `${inspect(actual)} != ${inspect(expected)}`); // eslint-disable-line eqeqeq
      },
      strictEqual(actual, expected, message) {
        if (!Object.is(actual, expected)) {
          fail(actual, expected, message, "strictEqual", `Expected values to be strictly equal:\n\n${inspect(actual)} !== ${inspect(expected)}\n`);
        }
      },
      notStrictEqual(actual, expected, message) {
        if (Object.is(actual, expected)) fail(actual, expected, message, "notStrictEqual", `Expected "actual" to be strictly unequal to: ${inspect(expected)}`);
      },
      deepEqual: strict ? undefined : (actual, expected, message) => {
        if (!deepEqual(actual, expected, false)) fail(actual, expected, message, "deepEqual", `Expected values to be loosely deep-equal:\n\n${inspect(actual)}\n\nshould loosely deep-equal\n\n${inspect(expected)}`);
      },
      notDeepEqual: strict ? undefined : (actual, expected, message) => {
        if (deepEqual(actual, expected, false)) fail(actual, expected, message, "notDeepEqual", `"actual" deepEqual "expected"`);
      },
      deepStrictEqual(actual, expected, message) {
        if (!deepEqual(actual, expected, true)) fail(actual, expected, message, "deepStrictEqual", `Expected values to be strictly deep-equal:\n+ actual - expected\n\n+ ${inspect(actual)}\n- ${inspect(expected)}\n`);
      },
      notDeepStrictEqual(actual, expected, message) {
        if (deepEqual(actual, expected, true)) fail(actual, expected, message, "notDeepStrictEqual", `Expected "actual" not to be strictly deep-equal to: ${inspect(expected)}`);
      },
      throws(fn, expected, message) {
        if (typeof expected === "string") [message, expected] = [expected, undefined];
        try {
          fn();
        } catch (err) {
          if (!errorMatches(err, expected)) {
            if (expected === undefined || typeof expected === "function" && !(expected.prototype && Error.isPrototypeOf(expected))) throw err;
            fail(err, expected, message, "throws", `The error is expected to match ${inspect(expected)}. Received ${inspect(err)}`);
          }
          return;
        }
        fail(undefined, expected, message, "throws", "Missing expected exception" + (message ? `: ${message}` : "."));
      },
      doesNotThrow(fn, expected, message) {
        try {
          fn();
        } catch (err) {
          fail(err, expected, typeof expected === "string" ? expected : message, "doesNotThrow", `Got unwanted exception.\nActual message: "${err?.message}"`);
        }
      },
      async rejects(promiseOrFn, expected, message) {
        if (typeof expected === "string") [message, expected] = [expected, undefined];
        try {
          await (typeof promiseOrFn === "function" ? promiseOrFn() : promiseOrFn);
        } catch (err) {
          if (!errorMatches(err, expected)) fail(err, expected, message, "rejects", `The error is expected to match ${inspect(expected)}. Received ${inspect(err)}`);
          return;
        }
        fail(undefined, expected, message, "rejects", "Missing expected rejection" + (message ? `: ${message}` : "."));
      },
      async doesNotReject(promiseOrFn, expected, message) {
        try {
          await (typeof promiseOrFn === "function" ? promiseOrFn() : promiseOrFn);
        } catch (err) {
          fail(err, expected, typeof expected === "string" ? expected : message, "doesNotReject", `Got unwanted rejection.\nActual message: "${err?.message}"`);
        }
      },
      match(string, regexp, message) {
        if (typeof string !== "string" || !regexp.test(string)) fail(string, regexp, message, "match", `The input did not match the regular expression ${regexp}. Input:\n\n${inspect(string)}\n`);
      },
      doesNotMatch(string, regexp, message) {
        if (typeof string === "string" && regexp.test(string)) fail(string, regexp, message, "doesNotMatch", `The input was expected to not match the regular expression ${regexp}. Input:\n\n${inspect(string)}\n`);
      },
      ifError(value) {
        if (value !== null && value !== undefined) {
          if (value instanceof Error) throw value;
          fail(value, null, undefined, "ifError", `ifError got unwanted exception: ${inspect(value)}`);
        }
      },
      AssertionError,
    });
    if (strict) {
      a.equal = a.strictEqual;
      a.notEqual = a.notStrictEqual;
      a.deepEqual = a.deepStrictEqual;
      a.notDeepEqual = a.notDeepStrictEqual;
    }
    return a;
  }

  const assert = makeAssert(false);
  const strictAssert = makeAssert(true);
  assert.strict = strictAssert;
  strictAssert.strict = strictAssert;

  // ------------------------------------------------------------------
  // url
  // ------------------------------------------------------------------

  function fileURLToPath(url) {
    const u = typeof url === "string" ? new URL(url) : url;
    if (!(u instanceof URL)) throw argTypeError("path", "of type string or an instance of URL", url);
    if (u.protocol !== "file:") {
      const e = new TypeError("The URL must be of scheme file");
      e.code = "ERR_INVALID_URL_SCHEME";
      throw e;
    }
    return decodeURIComponent(u.pathname);
  }

  function pathToFileURL(p) {
    let abs = internal.path.resolve(String(p));
    if (String(p).endsWith("/") && !abs.endsWith("/")) abs += "/";
    return new URL("file://" + abs.split("/").map((s) => encodeURIComponent(s)).join("/"));
  }

  const url = {
    URL,
    URLSearchParams,
    fileURLToPath,
    pathToFileURL,
    format: (u) => (u instanceof URL ? u.href : String(u?.href ?? u)),
    domainToASCII: (d) => String(d).toLowerCase(),
    domainToUnicode: (d) => String(d),
  };

  // ------------------------------------------------------------------
  // crypto: hashes (sha1, sha256, md5), HMAC, random helpers, crypto.subtle.digest
  // ------------------------------------------------------------------

  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  const rotl = (x, n) => (x << n) | (x >>> (32 - n));

  // Pads like SHA-1/SHA-256/MD5: 0x80, zeros, 64-bit length (big- or little-endian).
  function pad(bytes, littleEndian) {
    const len = bytes.length;
    const n = (((len + 9) + 63) >> 6) << 6;
    const m = new Uint8Array(n);
    m.set(bytes);
    m[len] = 0x80;
    const dv = new DataView(m.buffer);
    const bits = len * 8;
    if (littleEndian) {
      dv.setUint32(n - 8, bits >>> 0, true);
      dv.setUint32(n - 4, Math.floor(bits / 2 ** 32), true);
    } else {
      dv.setUint32(n - 4, bits >>> 0);
      dv.setUint32(n - 8, Math.floor(bits / 2 ** 32));
    }
    return dv;
  }

  const K256 = Uint32Array.of(
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2);

  function sha256(bytes) {
    const dv = pad(bytes, false);
    const h = Uint32Array.of(0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19);
    const w = new Uint32Array(64);
    for (let off = 0; off < dv.byteLength; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      let [a, b, c, d, e, f, g, hh] = h;
      for (let i = 0; i < 64; i++) {
        const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i]) | 0;
        const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    const out = new Uint8Array(32);
    const o = new DataView(out.buffer);
    h.forEach((v, i) => o.setUint32(i * 4, v));
    return out;
  }

  function sha1(bytes) {
    const dv = pad(bytes, false);
    const h = Uint32Array.of(0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0);
    const w = new Uint32Array(80);
    for (let off = 0; off < dv.byteLength; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
      let [a, b, c, d, e] = h;
      for (let i = 0; i < 80; i++) {
        const [f, k] = i < 20 ? [(b & c) | (~b & d), 0x5a827999] : i < 40 ? [b ^ c ^ d, 0x6ed9eba1]
          : i < 60 ? [(b & c) | (b & d) | (c & d), 0x8f1bbcdc] : [b ^ c ^ d, 0xca62c1d6];
        const t = (rotl(a, 5) + f + e + k + w[i]) | 0;
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    const out = new Uint8Array(20);
    const o = new DataView(out.buffer);
    h.forEach((v, i) => o.setUint32(i * 4, v));
    return out;
  }

  const MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  const MD5_K = Uint32Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32));

  function md5(bytes) {
    const dv = pad(bytes, true);
    const h = Uint32Array.of(0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476);
    for (let off = 0; off < dv.byteLength; off += 64) {
      let [a, b, c, d] = h;
      for (let i = 0; i < 64; i++) {
        let f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        const t = d;
        d = c;
        c = b;
        b = (b + rotl((a + f + MD5_K[i] + dv.getUint32(off + g * 4, true)) | 0, MD5_S[(i >> 4) * 4 + (i % 4)])) | 0;
        a = t;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    const out = new Uint8Array(16);
    const o = new DataView(out.buffer);
    h.forEach((v, i) => o.setUint32(i * 4, v, true));
    return out;
  }

  const HASHES = { sha256, "sha-256": sha256, sha1, "sha-1": sha1, md5 };

  function toBytes(data, encoding) {
    if (typeof data === "string") return Buffer.from(data, encoding);
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    throw argTypeError("data", "of type string or an instance of Buffer, TypedArray, or DataView", data);
  }

  function hashFunction(algorithm) {
    const fn = HASHES[String(algorithm).toLowerCase()];
    if (!fn) {
      const e = new Error(`Digest method not supported: ${algorithm} (rtn has sha256, sha1 and md5)`);
      e.code = "ERR_CRYPTO_INVALID_DIGEST";
      throw e;
    }
    return fn;
  }

  class Hash {
    #fn;
    #chunks = [];
    #done = false;
    constructor(fn) { this.#fn = fn; }
    update(data, encoding) {
      if (this.#done) throw Object.assign(new Error("Digest already called"), { code: "ERR_CRYPTO_HASH_FINALIZED" });
      this.#chunks.push(toBytes(data, encoding).slice());
      return this;
    }
    digest(encoding) {
      if (this.#done) throw Object.assign(new Error("Digest already called"), { code: "ERR_CRYPTO_HASH_FINALIZED" });
      this.#done = true;
      const out = Buffer.from(this.#fn(Buffer.concat(this.#chunks)));
      return encoding ? out.toString(encoding) : out;
    }
    copy() {
      const h = new Hash(this.#fn);
      for (const c of this.#chunks) h.update(c);
      return h;
    }
  }

  class Hmac {
    #fn;
    #inner;
    #outerKey;
    constructor(fn, key) {
      this.#fn = fn;
      let k = toBytes(key);
      if (k.length > 64) k = fn(k);
      const padded = new Uint8Array(64);
      padded.set(k);
      this.#inner = new Hash(fn).update(padded.map((b) => b ^ 0x36));
      this.#outerKey = padded.map((b) => b ^ 0x5c);
    }
    update(data, encoding) {
      this.#inner.update(data, encoding);
      return this;
    }
    digest(encoding) {
      return new Hash(this.#fn).update(this.#outerKey).update(this.#inner.digest()).digest(encoding);
    }
  }

  const subtle = {
    async digest(algorithm, data) {
      const name = typeof algorithm === "string" ? algorithm : algorithm?.name;
      const fn = HASHES[String(name).toLowerCase()];
      if (!fn || name.toLowerCase() === "md5") {
        throw new DOMException(`Unrecognized algorithm name: ${name} (rtn supports SHA-1 and SHA-256)`, "NotSupportedError");
      }
      return fn(toBytes(data)).buffer;
    },
  };
  Object.defineProperty(globalThis.crypto, "subtle", { value: subtle, enumerable: true, configurable: true });

  const nodeCrypto = {
    createHash: (algorithm) => new Hash(hashFunction(algorithm)),
    createHmac: (algorithm, key) => new Hmac(hashFunction(algorithm), key),
    getHashes: () => ["md5", "sha1", "sha256"],
    hash: (algorithm, data, encoding = "hex") => new Hash(hashFunction(algorithm)).update(data).digest(encoding),
    randomBytes(size, callback) {
      const b = Buffer.alloc(size);
      for (let i = 0; i < size; i += 65536) crypto.getRandomValues(b.subarray(i, i + 65536));
      if (typeof callback === "function") {
        queueMicrotask(() => callback(null, b));
        return undefined;
      }
      return b;
    },
    randomInt(min, max, callback) {
      if (max === undefined || typeof max === "function") [min, max, callback] = [0, min, max];
      const range = max - min;
      if (!Number.isSafeInteger(min) || !Number.isSafeInteger(max) || range <= 0) {
        throw new RangeError(`The value of "max" is out of range. It must be greater than the value of "min" (${min}). Received ${max}`);
      }
      const limit = Math.floor(2 ** 48 / range) * range;  // reject bias
      let n;
      do {
        const b = crypto.getRandomValues(new Uint8Array(6));
        n = b.reduce((acc, x) => acc * 256 + x, 0);
      } while (n >= limit);
      const r = min + (n % range);
      if (typeof callback === "function") {
        queueMicrotask(() => callback(null, r));
        return undefined;
      }
      return r;
    },
    randomUUID: () => crypto.randomUUID(),
    getRandomValues: (a) => crypto.getRandomValues(a),
    timingSafeEqual(a, b) {
      if (a.byteLength !== b.byteLength) {
        throw Object.assign(new RangeError("Input buffers must have the same byte length"), { code: "ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH" });
      }
      const x = toBytes(a);
      const y = toBytes(b);
      let diff = 0;
      for (let i = 0; i < x.length; i++) diff |= x[i] ^ y[i];
      return diff === 0;
    },
    webcrypto: globalThis.crypto,
    subtle,
  };

  // ------------------------------------------------------------------
  // timers, timers/promises
  // ------------------------------------------------------------------

  const timers = { setTimeout, setInterval, clearTimeout, clearInterval, setImmediate, clearImmediate };
  const timersPromises = {
    setTimeout: (ms, value, options) => new Promise((resolve, reject) => {
      const signal = options?.signal;
      if (signal?.aborted) return reject(signal.reason);
      const id = setTimeout(() => resolve(value), ms);
      signal?.addEventListener("abort", () => { clearTimeout(id); reject(signal.reason); }, { once: true });
    }),
    setImmediate: (value) => new Promise((resolve) => setImmediate(() => resolve(value))),
    async *setInterval(ms, value) {
      while (true) {
        await new Promise((r) => setTimeout(r, ms));
        yield value;
      }
    },
  };
  timers.promises = timersPromises;

  // ------------------------------------------------------------------
  // tty
  // ------------------------------------------------------------------

  const tty = {
    isatty: (fd) => native.isatty(Number(fd)),
    ReadStream: class ReadStream { constructor(fd) { this.fd = fd; this.isTTY = native.isatty(fd); } },
    WriteStream: class WriteStream {
      constructor(fd) { this.fd = fd; this.isTTY = native.isatty(fd); }
      get columns() { return process.stdout.columns; }
      get rows() { return process.stdout.rows; }
      getColorDepth() { return process.stdout.getColorDepth(); }
      hasColors(count) { return process.stdout.hasColors(count); }
    },
  };

  Object.assign(internal.modules, {
    tty,
    events: EventEmitter,
    util,
    "util/types": types,
    os,
    assert,
    "assert/strict": strictAssert,
    url,
    crypto: nodeCrypto,
    timers,
    "timers/promises": timersPromises,
    process,
  });
});
