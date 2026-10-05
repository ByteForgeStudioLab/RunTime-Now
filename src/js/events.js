// DOM events and cancellation: Event, CustomEvent, EventTarget,
// AbortController, AbortSignal.
//
// Runs before web.js and fetch.js, which use AbortSignal through `internal`:
//   internal.onAbort(signal, fn) -> remove()   run fn(reason) when signal aborts
//   internal.abortSignal(signal, reason)        abort a signal (e.g. a timeout)
(function (native, internal) {
  "use strict";

  const INSPECT = Symbol.for("rtn.inspect");

  function define(name, value) {
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  }

  // ------------------------------------------------------------------
  // Event
  // ------------------------------------------------------------------

  let beginDispatch;  // (event, target) -> void
  let endDispatch;    // (event) -> void
  let eventState;     // (event) -> { stopImmediate, passive } (mutable)

  class Event {
    #type;
    #bubbles;
    #cancelable;
    #composed;
    #target = null;
    #currentTarget = null;
    #phase = 0;
    #canceled = false;
    #stop = false;
    #state = { stopImmediate: false, passive: false, dispatching: false, trusted: false };
    #timeStamp = performance.now();

    static {
      beginDispatch = (e, target) => {
        e.#target = target;
        e.#currentTarget = target;
        e.#phase = 2;  // AT_TARGET: there is no tree to capture or bubble through
        e.#state.dispatching = true;
      };
      endDispatch = (e) => {
        e.#currentTarget = null;
        e.#phase = 0;
        e.#state.dispatching = false;
        e.#state.stopImmediate = false;
        e.#stop = false;
      };
      eventState = (e) => e.#state;
    }

    constructor(type, options = {}) {
      if (arguments.length === 0) throw new TypeError("Failed to construct 'Event': 1 argument required, but only 0 present.");
      options = options ?? {};
      this.#type = String(type);
      this.#bubbles = Boolean(options.bubbles);
      this.#cancelable = Boolean(options.cancelable);
      this.#composed = Boolean(options.composed);
    }

    get type() { return this.#type; }
    get target() { return this.#target; }
    get srcElement() { return this.#target; }
    get currentTarget() { return this.#currentTarget; }
    get eventPhase() { return this.#phase; }
    get bubbles() { return this.#bubbles; }
    get cancelable() { return this.#cancelable; }
    get composed() { return this.#composed; }
    get defaultPrevented() { return this.#canceled; }
    get isTrusted() { return this.#state.trusted; }
    get timeStamp() { return this.#timeStamp; }
    get returnValue() { return !this.#canceled; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this.#stop; }
    set cancelBubble(v) { if (v) this.#stop = true; }

    composedPath() { return this.#state.dispatching && this.#currentTarget ? [this.#currentTarget] : []; }
    preventDefault() { if (this.#cancelable && !this.#state.passive) this.#canceled = true; }
    stopPropagation() { this.#stop = true; }
    stopImmediatePropagation() { this.#stop = true; this.#state.stopImmediate = true; }

    get [Symbol.toStringTag]() { return "Event"; }
    [INSPECT]() {
      return { type: this.#type, defaultPrevented: this.#canceled, cancelable: this.#cancelable, timeStamp: this.#timeStamp };
    }
  }
  for (const [name, value] of [["NONE", 0], ["CAPTURING_PHASE", 1], ["AT_TARGET", 2], ["BUBBLING_PHASE", 3]]) {
    Object.defineProperty(Event, name, { value, enumerable: true });
    Object.defineProperty(Event.prototype, name, { value, enumerable: true });
  }

  class CustomEvent extends Event {
    #detail;
    constructor(type, options = {}) {
      super(type, options);
      this.#detail = options?.detail ?? null;
    }
    get detail() { return this.#detail; }
    get [Symbol.toStringTag]() { return "CustomEvent"; }
  }

  // ------------------------------------------------------------------
  // EventTarget
  // ------------------------------------------------------------------

  function flattenOptions(options) {
    if (typeof options === "boolean") return { capture: options };
    if (options === null || typeof options !== "object") return { capture: false };
    return {
      capture: Boolean(options.capture),
      once: Boolean(options.once),
      passive: Boolean(options.passive),
      signal: options.signal,
    };
  }

  class EventTarget {
    #listeners = new Map();  // type -> [{ callback, capture, once, passive, removed }]

    addEventListener(type, callback, options) {
      if (callback === null || callback === undefined) return;
      if (typeof callback !== "function" && typeof callback !== "object") {
        throw new TypeError("The \"listener\" argument must be a function or an object with a handleEvent method");
      }
      type = String(type);
      const { capture, once, passive, signal } = flattenOptions(options);
      if (signal !== undefined && !(signal instanceof AbortSignal)) {
        throw new TypeError("The \"options.signal\" property must be an instance of AbortSignal");
      }
      if (signal?.aborted) return;
      let list = this.#listeners.get(type);
      if (!list) this.#listeners.set(type, (list = []));
      if (list.some((l) => l.callback === callback && l.capture === capture)) return;
      list.push({ callback, capture, once, passive, removed: false });
      if (signal) onAbort(signal, () => this.removeEventListener(type, callback, { capture }));
    }

    removeEventListener(type, callback, options) {
      type = String(type);
      const { capture } = flattenOptions(options);
      const list = this.#listeners.get(type);
      if (!list) return;
      const i = list.findIndex((l) => l.callback === callback && l.capture === capture);
      if (i < 0) return;
      list[i].removed = true;
      list.splice(i, 1);
    }

    dispatchEvent(event) {
      if (!(event instanceof Event)) {
        throw new TypeError("The \"event\" argument must be an instance of Event");
      }
      const state = eventState(event);
      if (state.dispatching) throw new DOMException("The event is already being dispatched.", "InvalidStateError");
      beginDispatch(event, this);
      try {
        const list = this.#listeners.get(event.type);
        for (const l of list ? list.slice() : []) {  // listeners added during dispatch don't run
          if (l.removed) continue;
          if (l.once) this.removeEventListener(event.type, l.callback, { capture: l.capture });
          state.passive = l.passive;
          try {
            if (typeof l.callback === "function") l.callback.call(this, event);
            else if (typeof l.callback.handleEvent === "function") l.callback.handleEvent(event);
          } catch (e) {
            // Like Node and browsers: report it as an uncaught error, keep dispatching.
            queueMicrotask(() => { throw e; });
          }
          state.passive = false;
          if (state.stopImmediate) break;
        }
      } finally {
        endDispatch(event);
      }
      return !event.defaultPrevented;
    }

    get [Symbol.toStringTag]() { return "EventTarget"; }
  }

  // `onabort = fn` style handlers: one listener, registered when first set.
  function defineEventHandler(proto, type) {
    const handlers = new WeakMap();  // target -> { fn, listener }
    Object.defineProperty(proto, "on" + type, {
      configurable: true,
      enumerable: true,
      get() { return handlers.get(this)?.fn ?? null; },
      set(fn) {
        let h = handlers.get(this);
        if (typeof fn !== "function") fn = null;
        if (fn === null) {
          if (h) this.removeEventListener(type, h.listener);
          handlers.delete(this);
          return;
        }
        if (!h) {
          h = { fn, listener: (e) => h.fn.call(this, e) };
          handlers.set(this, h);
          this.addEventListener(type, h.listener);
        }
        h.fn = fn;
      },
    });
  }

  // ------------------------------------------------------------------
  // AbortSignal / AbortController
  // ------------------------------------------------------------------

  const ILLEGAL = Symbol("AbortSignal constructor");
  let onAbort;      // (signal, fn) -> remove()
  let abortSignal;  // (signal, reason) -> void

  class AbortSignal extends EventTarget {
    #aborted = false;
    #reason = undefined;
    #algorithms = new Set();
    #dependents = new Set();  // signals made by AbortSignal.any() from this one

    static {
      onAbort = (signal, fn) => {
        if (signal.#aborted) {
          fn(signal.#reason);
          return () => {};
        }
        const entry = () => fn(signal.#reason);
        signal.#algorithms.add(entry);
        return () => signal.#algorithms.delete(entry);
      };
      abortSignal = (signal, reason) => {
        if (signal.#aborted) return;
        signal.#aborted = true;
        signal.#reason = reason === undefined ? new DOMException("This operation was aborted", "AbortError") : reason;
        const algorithms = [...signal.#algorithms];
        signal.#algorithms.clear();
        for (const fn of algorithms) fn();
        const event = new Event("abort");
        eventState(event).trusted = true;
        signal.dispatchEvent(event);
        const dependents = [...signal.#dependents];
        signal.#dependents.clear();
        for (const d of dependents) abortSignal(d, signal.#reason);
      };
    }

    constructor(token) {
      if (token !== ILLEGAL) throw new TypeError("Illegal constructor");
      super();
    }

    get aborted() { return this.#aborted; }
    get reason() { return this.#reason; }
    throwIfAborted() {
      if (this.#aborted) throw this.#reason;
    }

    static abort(reason) {
      const s = new AbortSignal(ILLEGAL);
      abortSignal(s, reason);
      return s;
    }

    static timeout(ms) {
      ms = Number(ms);
      if (!Number.isFinite(ms) || ms < 0) {
        throw new RangeError(`The value of "delay" is out of range. It must be >= 0. Received ${ms}`);
      }
      const s = new AbortSignal(ILLEGAL);
      const id = setTimeout(() => {
        abortSignal(s, new DOMException("The operation was aborted due to timeout", "TimeoutError"));
      }, ms);
      native.unrefTimer(id);  // a pending timeout must not keep the process alive
      return s;
    }

    static any(signals) {
      const s = new AbortSignal(ILLEGAL);
      const list = [...signals];
      for (const source of list) {
        if (!(source instanceof AbortSignal)) {
          throw new TypeError("The \"signals\" argument must be an iterable of AbortSignal objects");
        }
      }
      for (const source of list) {
        if (source.#aborted) {
          abortSignal(s, source.#reason);
          return s;
        }
      }
      for (const source of list) source.#dependents.add(s);
      return s;
    }

    get [Symbol.toStringTag]() { return "AbortSignal"; }
    [INSPECT]() { return { aborted: this.#aborted }; }
  }
  defineEventHandler(AbortSignal.prototype, "abort");

  class AbortController {
    #signal = new AbortSignal(ILLEGAL);
    get signal() { return this.#signal; }
    abort(reason) { abortSignal(this.#signal, reason); }
    get [Symbol.toStringTag]() { return "AbortController"; }
    [INSPECT]() { return { signal: this.#signal }; }
  }

  define("Event", Event);
  define("CustomEvent", CustomEvent);
  define("EventTarget", EventTarget);
  define("AbortSignal", AbortSignal);
  define("AbortController", AbortController);

  internal.onAbort = onAbort;
  internal.abortSignal = abortSignal;
});
