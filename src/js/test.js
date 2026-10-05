// rtn test: a built-in test runner with the Jest / Bun API.
//
//   import { test, expect, describe } from "rtn:test";
//   test("adds", () => expect(1 + 2).toBe(3));
//
// `rtn test [files | dirs | filters] [-t name-pattern] [--timeout ms]` finds
// *.test.*, *_test.* and *.spec.* files, imports them one after another and
// runs the tests each one registered (src/main.cpp calls internal.runTests).
(function (native, internal) {
  "use strict";

  const path = internal.path;
  const fs = internal.modules.fs;
  const inspect = (v) => native.inspect(v, false);

  // ------------------------------------------------------------------
  // Registration
  // ------------------------------------------------------------------

  function newSuite(name, parent, mode = "run") {
    return { name, parent, mode, children: [], hooks: { beforeAll: [], afterAll: [], beforeEach: [], afterEach: [] } };
  }

  let root = newSuite("", null);
  let current = root;
  let defaultTimeout = 5000;

  function addTest(name, fn, options, mode) {
    if (typeof name === "function") [name, fn] = [name.name || "anonymous", name];
    if (mode !== "todo" && typeof fn !== "function") throw new TypeError(`test("${name}"): the second argument must be a function`);
    const timeout = typeof options === "number" ? options : options?.timeout ?? undefined;
    current.children.push({ kind: "test", name: String(name), fn, timeout, mode, suite: current });
  }

  function addSuite(name, fn, mode) {
    if (typeof fn !== "function") throw new TypeError(`describe("${name}"): the second argument must be a function`);
    const suite = newSuite(String(name), current, mode);
    current.children.push(suite);
    const parent = current;
    current = suite;
    try {
      const r = fn();
      if (r && typeof r.then === "function") throw new TypeError(`describe("${name}"): the callback must not be async`);
    } finally {
      current = parent;
    }
  }

  // test.each([[1, 2, 3], ...])("adds %i + %i", (a, b, sum) => ...)
  function each(register) {
    return (table) => (name, fn, options) => {
      table.forEach((row, i) => {
        const args = Array.isArray(row) ? row : [row];
        let title = String(name).replace(/%#/g, String(i));
        let k = 0;
        title = title.replace(/%[sdifjoOp%]/g, (m) => (m === "%%" ? "%" : k < args.length ? (typeof args[k] === "string" && m === "%s" ? args[k++] : inspect(args[k++])) : m));
        if (!Array.isArray(row) && row !== null && typeof row === "object") {
          title = title.replace(/\$([\w.]+)/g, (_, key) => inspect(key.split(".").reduce((o, p) => o?.[p], row)));
        }
        register(title, () => fn(...args), options);
      });
    };
  }

  const test = (name, fn, options) => addTest(name, fn, options, "run");
  test.skip = (name, fn, options) => addTest(name, fn, options, "skip");
  test.only = (name, fn, options) => addTest(name, fn, options, "only");
  test.todo = (name, fn, options) => addTest(name, fn, options, "todo");
  test.if = (cond) => (cond ? test : test.skip);
  test.skipIf = (cond) => (cond ? test.skip : test);
  test.each = each(test);
  test.only.each = each(test.only);
  test.skip.each = each(test.skip);

  const describe = (name, fn) => addSuite(name, fn, "run");
  describe.skip = (name, fn) => addSuite(name, fn, "skip");
  describe.only = (name, fn) => addSuite(name, fn, "only");
  describe.todo = (name) => addSuite(name, () => {}, "skip");
  describe.if = (cond) => (cond ? describe : describe.skip);
  describe.each = each((name, fn) => describe(name, fn));

  const hook = (kind) => (fn, timeout) => {
    if (typeof fn !== "function") throw new TypeError(`${kind}() needs a function`);
    current.hooks[kind].push({ fn, timeout });
  };

  // ------------------------------------------------------------------
  // Mocks
  // ------------------------------------------------------------------

  function mock(impl = () => undefined) {
    let implementation = impl;
    const once = [];
    const fn = function (...args) {
      fn.mock.calls.push(args);
      fn.mock.contexts.push(this);
      fn.mock.lastCall = args;
      const use = once.length ? once.shift() : implementation;
      try {
        const value = new.target ? Reflect.construct(use, args, new.target) : Reflect.apply(use, this, args);
        fn.mock.results.push({ type: "return", value });
        if (new.target) fn.mock.instances.push(value);
        return value;
      } catch (e) {
        fn.mock.results.push({ type: "throw", value: e });
        throw e;
      }
    };
    fn.mock = { calls: [], results: [], instances: [], contexts: [], lastCall: undefined };
    fn._isMockFunction = true;
    Object.assign(fn, {
      mockImplementation(f) { implementation = f; return fn; },
      mockImplementationOnce(f) { once.push(f); return fn; },
      mockReturnValue(v) { implementation = () => v; return fn; },
      mockReturnValueOnce(v) { once.push(() => v); return fn; },
      mockResolvedValue(v) { implementation = () => Promise.resolve(v); return fn; },
      mockResolvedValueOnce(v) { once.push(() => Promise.resolve(v)); return fn; },
      mockRejectedValue(v) { implementation = () => Promise.reject(v); return fn; },
      mockRejectedValueOnce(v) { once.push(() => Promise.reject(v)); return fn; },
      mockClear() { Object.assign(fn.mock, { calls: [], results: [], instances: [], contexts: [], lastCall: undefined }); return fn; },
      mockReset() { fn.mockClear(); implementation = () => undefined; once.length = 0; return fn; },
      getMockName() { return "mock"; },
    });
    return fn;
  }

  const spies = [];
  function spyOn(object, method) {
    const original = object[method];
    if (typeof original !== "function") throw new TypeError(`spyOn: ${String(method)} is not a function`);
    const spy = mock(function (...args) { return Reflect.apply(original, this, args); });
    spy.mockRestore = () => { object[method] = original; };
    object[method] = spy;
    spies.push(spy);
    return spy;
  }

  // ------------------------------------------------------------------
  // expect()
  // ------------------------------------------------------------------

  class Asymmetric {
    constructor(name, match) {
      this.name = name;
      this.asymmetricMatch = match;
    }
    [Symbol.for("rtn.inspect")]() { return this.name; }
  }

  // toEqual(): recursive, ignores undefined properties and prototypes; Asymmetric matchers allowed.
  function equals(a, b, strict, seen = new Map()) {
    if (b instanceof Asymmetric) return b.asymmetricMatch(a);
    if (a instanceof Asymmetric) return a.asymmetricMatch(b);
    if (Object.is(a, b)) return true;
    if (strict) return internal.deepEqual(a, b, true);
    if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
    if (seen.get(a) === b) return true;
    seen.set(a, b);
    if (a instanceof Date || b instanceof Date) return a instanceof Date && b instanceof Date && a.getTime() === b.getTime();
    if (a instanceof RegExp || b instanceof RegExp) return String(a) === String(b);
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    if (a instanceof Map || a instanceof Set || ArrayBuffer.isView(a) || a instanceof ArrayBuffer || a instanceof Error) {
      return internal.deepEqual(a, b, true);
    }
    if (Array.isArray(a)) {
      if (a.length !== b.length) return false;
      for (let i = 0; i < a.length; i++) if (!equals(a[i], b[i], false, seen)) return false;
      return true;
    }
    const ka = Object.keys(a).filter((k) => a[k] !== undefined);
    const kb = Object.keys(b).filter((k) => b[k] !== undefined);
    if (ka.length !== kb.length) return false;
    return ka.every((k) => Object.hasOwn(b, k) && equals(a[k], b[k], false, seen));
  }

  // toMatchObject(): every property of `subset` matches.
  function matchesObject(obj, subset) {
    if (subset instanceof Asymmetric) return subset.asymmetricMatch(obj);
    if (typeof subset !== "object" || subset === null) return equals(obj, subset, false);
    if (typeof obj !== "object" || obj === null) return false;
    if (Array.isArray(subset)) {
      return Array.isArray(obj) && obj.length === subset.length && subset.every((v, i) => matchesObject(obj[i], v));
    }
    return Object.keys(subset).every((k) => k in obj && matchesObject(obj[k], subset[k]));
  }

  class AssertionError extends Error {
    constructor(message, matcherResult) {
      super(message);
      this.name = "AssertionError";
      this.matcherResult = matcherResult;
    }
  }

  function getPath(obj, keyPath) {
    const parts = Array.isArray(keyPath) ? keyPath : String(keyPath).replace(/\[(\w+)\]/g, ".$1").split(".").filter(Boolean);
    let cur = obj;
    for (const p of parts) {
      if (cur === null || cur === undefined || !(Object(cur) instanceof Object) || !(p in Object(cur))) return { found: false };
      cur = cur[p];
    }
    return { found: true, value: cur };
  }

  function thrownMatches(err, expected) {
    if (expected === undefined) return true;
    const message = err instanceof Error ? err.message : String(err);
    if (typeof expected === "string") return message.includes(expected);
    if (expected instanceof RegExp) return expected.test(message);
    if (typeof expected === "function") return err instanceof expected;
    if (expected instanceof Error) return message === expected.message;
    if (typeof expected === "object" && expected !== null) return matchesObject(err, expected);
    return false;
  }

  const MATCHERS = {
    toBe: (r, e) => [Object.is(r, e), () => `Expected: ${inspect(e)}\nReceived: ${inspect(r)}` +
      (typeof r === "object" && r !== null && equals(r, e, false) ? "\n\nIf it should pass with deep equality, use toEqual()" : "")],
    toEqual: (r, e) => [equals(r, e, false), () => `Expected: ${inspect(e)}\nReceived: ${inspect(r)}`],
    toStrictEqual: (r, e) => [equals(r, e, true), () => `Expected: ${inspect(e)}\nReceived: ${inspect(r)}`],
    toBeTruthy: (r) => [Boolean(r), () => `Received: ${inspect(r)}`],
    toBeFalsy: (r) => [!r, () => `Received: ${inspect(r)}`],
    toBeNull: (r) => [r === null, () => `Received: ${inspect(r)}`],
    toBeUndefined: (r) => [r === undefined, () => `Received: ${inspect(r)}`],
    toBeDefined: (r) => [r !== undefined, () => `Received: ${inspect(r)}`],
    toBeNaN: (r) => [Number.isNaN(r), () => `Received: ${inspect(r)}`],
    toBeGreaterThan: (r, e) => [r > e, () => `Expected: > ${inspect(e)}\nReceived:   ${inspect(r)}`],
    toBeGreaterThanOrEqual: (r, e) => [r >= e, () => `Expected: >= ${inspect(e)}\nReceived:    ${inspect(r)}`],
    toBeLessThan: (r, e) => [r < e, () => `Expected: < ${inspect(e)}\nReceived:   ${inspect(r)}`],
    toBeLessThanOrEqual: (r, e) => [r <= e, () => `Expected: <= ${inspect(e)}\nReceived:    ${inspect(r)}`],
    toBeCloseTo: (r, e, digits = 2) => [Math.abs(r - e) < 10 ** -digits / 2, () => `Expected: ${inspect(e)} (±${10 ** -digits / 2})\nReceived: ${inspect(r)}`],
    toBeInstanceOf: (r, e) => [r instanceof e, () => `Expected constructor: ${e?.name}\nReceived: ${inspect(r)}`],
    toBeTypeOf: (r, e) => [typeof r === e, () => `Expected type: ${e}\nReceived type: ${typeof r}`],
    toContain: (r, e) => [r != null && typeof r.includes === "function" ? r.includes(e) : r instanceof Set ? r.has(e) : false,
      () => `Expected to contain: ${inspect(e)}\nReceived: ${inspect(r)}`],
    toContainEqual: (r, e) => [Array.from(r ?? []).some((x) => equals(x, e, false)), () => `Expected to contain equal: ${inspect(e)}\nReceived: ${inspect(r)}`],
    toHaveLength: (r, e) => [r != null && r.length === e, () => `Expected length: ${e}\nReceived length: ${r?.length}\nReceived: ${inspect(r)}`],
    toHaveProperty: (r, keyPath, ...value) => {
      const { found, value: v } = getPath(r, keyPath);
      const pass = found && (value.length === 0 || equals(v, value[0], false));
      return [pass, () => `Expected path: ${inspect(keyPath)}${value.length ? `\nExpected value: ${inspect(value[0])}` : ""}\nReceived: ${found ? inspect(v) : "(not found)"}`];
    },
    toMatch: (r, e) => [typeof r === "string" && (e instanceof RegExp ? e.test(r) : r.includes(e)), () => `Expected pattern: ${inspect(e)}\nReceived: ${inspect(r)}`],
    toMatchObject: (r, e) => [matchesObject(r, e), () => `Expected to match: ${inspect(e)}\nReceived: ${inspect(r)}`],
    toThrow: (r, e) => {
      if (typeof r !== "function") return [false, () => "Received value must be a function"];
      let threw = false;
      let err;
      try {
        r();
      } catch (x) {
        threw = true;
        err = x;
      }
      return [threw && thrownMatches(err, e), () => threw ? `Expected: ${inspect(e)}\nThrown: ${inspect(err)}` : "Received function did not throw"];
    },
    toHaveBeenCalled: (r) => [r?.mock?.calls.length > 0, () => `Number of calls: ${r?.mock?.calls.length}`],
    toHaveBeenCalledTimes: (r, n) => [r?.mock?.calls.length === n, () => `Expected calls: ${n}\nReceived calls: ${r?.mock?.calls.length}`],
    toHaveBeenCalledWith: (r, ...args) => [r?.mock?.calls.some((c) => equals(c, args, false)),
      () => `Expected call: ${inspect(args)}\nReceived calls: ${inspect(r?.mock?.calls)}`],
    toHaveBeenLastCalledWith: (r, ...args) => [equals(r?.mock?.lastCall, args, false), () => `Expected: ${inspect(args)}\nLast call: ${inspect(r?.mock?.lastCall)}`],
    toHaveBeenNthCalledWith: (r, n, ...args) => [equals(r?.mock?.calls[n - 1], args, false), () => `Expected call #${n}: ${inspect(args)}\nReceived: ${inspect(r?.mock?.calls[n - 1])}`],
    toHaveReturnedWith: (r, v) => [r?.mock?.results.some((x) => x.type === "return" && equals(x.value, v, false)), () => `Expected return value: ${inspect(v)}`],
  };
  MATCHERS.toThrowError = MATCHERS.toThrow;
  MATCHERS.toBeCalled = MATCHERS.toHaveBeenCalled;
  MATCHERS.toBeCalledTimes = MATCHERS.toHaveBeenCalledTimes;
  MATCHERS.toBeCalledWith = MATCHERS.toHaveBeenCalledWith;

  let assertionCount = 0;

  function makeExpect(received, negate, promiseMode) {
    const api = {};
    for (const [name, matcher] of Object.entries(MATCHERS)) {
      const check = (value, args) => {
        assertionCount++;
        const [pass, describeFailure] = matcher(value, ...args);
        if (pass === negate) {
          const argText = args.map((a) => inspect(a)).join(", ");
          const head = `expect(received)${promiseMode ? `.${promiseMode}` : ""}${negate ? ".not" : ""}.${name}(${argText})`;
          const body = negate ? `Expected the opposite.\n${describeFailure()}` : describeFailure();
          const err = new AssertionError(`${head}\n\n${body}`);
          if (Error.captureStackTrace) Error.captureStackTrace(err, check);
          throw err;
        }
      };
      api[name] = (...args) => {
        // A function given to .resolves / .rejects is called now, not when expect() runs.
        const settle = () => (typeof received === "function" ? Promise.resolve().then(received) : Promise.resolve(received));
        if (promiseMode === "resolves") {
          return settle().then((v) => check(v, args),
            (e) => { throw new AssertionError(`expect(received).resolves.${name}()\n\nReceived promise rejected: ${inspect(e)}`); });
        }
        if (promiseMode === "rejects") {
          return settle().then(
            (v) => { throw new AssertionError(`expect(received).rejects.${name}()\n\nReceived promise resolved: ${inspect(v)}`); },
            (e) => check(name === "toThrow" || name === "toThrowError" ? () => { throw e; } : e, args));
        }
        return check(received, args);
      };
    }
    return api;
  }

  function expect(received) {
    const api = makeExpect(received, false, null);
    api.not = makeExpect(received, true, null);
    api.resolves = makeExpect(received, false, "resolves");
    api.resolves.not = makeExpect(received, true, "resolves");
    api.rejects = makeExpect(received, false, "rejects");
    api.rejects.not = makeExpect(received, true, "rejects");
    return api;
  }
  expect.anything = () => new Asymmetric("Anything", (v) => v !== null && v !== undefined);
  expect.any = (C) => new Asymmetric(`Any<${C?.name}>`, (v) =>
    C === Number ? typeof v === "number" || v instanceof Number
      : C === String ? typeof v === "string" || v instanceof String
        : C === Boolean ? typeof v === "boolean" || v instanceof Boolean
          : C === Function ? typeof v === "function"
            : C === Object ? typeof v === "object" && v !== null
              : C === BigInt ? typeof v === "bigint"
                : C === Symbol ? typeof v === "symbol" : v instanceof C);
  expect.stringContaining = (s) => new Asymmetric(`StringContaining ${inspect(s)}`, (v) => typeof v === "string" && v.includes(s));
  expect.stringMatching = (re) => new Asymmetric(`StringMatching ${re}`, (v) => typeof v === "string" && new RegExp(re).test(v));
  expect.objectContaining = (o) => new Asymmetric(`ObjectContaining ${inspect(o)}`, (v) => matchesObject(v, o));
  expect.arrayContaining = (arr) => new Asymmetric(`ArrayContaining ${inspect(arr)}`, (v) => Array.isArray(v) && arr.every((x) => v.some((y) => equals(y, x, false))));
  expect.assertions = (n) => { expectedAssertions = n; };
  expect.hasAssertions = () => { expectedAssertions = -1; };
  let expectedAssertions = null;

  // ------------------------------------------------------------------
  // Running
  // ------------------------------------------------------------------

  const color = Boolean(process.stdout.isTTY) && !process.env.NO_COLOR && process.env.TERM !== "dumb";
  const paint = (code) => (s) => (color ? `${code}${s}\x1b[0m` : s);
  const blue = paint(/truecolor|24bit/.test(process.env.COLORTERM ?? "") ? "\x1b[38;2;59;130;246m" : "\x1b[38;5;33m");
  const red = paint("\x1b[31m");
  const dim = paint("\x1b[2m");
  const bold = paint("\x1b[1m");
  const write = (s) => process.stdout.write(s);
  const ms = (t) => (t < 1 ? `${t.toFixed(2)}ms` : t < 1000 ? `${t.toFixed(1)}ms` : `${(t / 1000).toFixed(2)}s`);

  // Runs fn with a timeout; supports async functions and `done` callbacks.
  function runWithTimeout(fn, timeout, label) {
    return new Promise((resolve, reject) => {
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        reject(new Error(`${label} timed out after ${timeout}ms`));
      }, timeout);
      const finish = (err) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        if (err) reject(err);
        else resolve();
      };
      try {
        if (fn.length > 0) {  // done-style: test("x", (done) => { ...; done(); })
          const done = (err) => finish(err ? (err instanceof Error ? err : new Error(String(err))) : undefined);
          done.fail = (err) => finish(err instanceof Error ? err : new Error(String(err ?? "done.fail()")));
          const r = fn(done);
          if (r && typeof r.then === "function") r.then(null, finish);
        } else {
          Promise.resolve(fn()).then(() => finish(), finish);
        }
      } catch (e) {
        finish(e);
      }
    });
  }

  function hasOnly(suite) {
    return suite.children.some((c) => c.mode === "only" || (c.kind !== "test" && hasOnly(c)));
  }

  function fullName(t) {
    const names = [];
    for (let s = t.suite; s && s.parent; s = s.parent) names.unshift(s.name);
    names.push(t.name);
    return names.join(" › ");
  }

  // Lines of the stack that point at user code (not rtn's internals).
  function userStack(err, file) {
    const lines = String(err?.stack ?? "").split("\n").filter((l) => /^\s*at /.test(l) && !l.includes("rtn:internal"));
    const own = lines.filter((l) => l.includes(file));
    return (own.length ? own : lines).slice(0, 3).map((l) => l.trim().replace(process.cwd() + "/", ""));
  }

  function errorText(err) {
    if (err instanceof AssertionError) return err.message;
    if (err instanceof Error) return `${err.name}: ${err.message}`;
    return `Thrown: ${inspect(err)}`;
  }

  async function runSuite(suite, ctx, inheritedSkip, onlyMode, beforeEachChain, afterEachChain) {
    const skip = inheritedSkip || suite.mode === "skip";
    const tests = [];
    const collect = (s) => s.children.forEach((c) => (c.kind === "test" ? tests.push(c) : collect(c)));
    collect(suite);
    const willRun = (t) => !skip && t.mode !== "skip" && t.mode !== "todo" &&
      (!onlyMode || t.mode === "only" || isInOnlySuite(t)) && (!ctx.namePattern || ctx.namePattern.test(fullName(t)));
    const anyRuns = tests.some(willRun);

    if (anyRuns) {
      for (const h of suite.hooks.beforeAll) {
        try {
          await runWithTimeout(h.fn, h.timeout ?? ctx.timeout, "beforeAll hook");
        } catch (e) {
          ctx.report({ name: `${suite.name || "(file)"} › beforeAll`, status: "fail", error: e, duration: 0 });
          return;
        }
      }
    }
    const beforeEach = [...beforeEachChain, ...suite.hooks.beforeEach];
    const afterEach = [...suite.hooks.afterEach, ...afterEachChain];

    for (const child of suite.children) {
      if (child.kind !== "test") {
        await runSuite(child, ctx, skip, onlyMode, beforeEach, afterEach);
        continue;
      }
      const name = fullName(child);
      if (ctx.namePattern && !ctx.namePattern.test(name)) continue;
      if (child.mode === "todo") { ctx.report({ name, status: "todo" }); continue; }
      if (!willRun(child)) { ctx.report({ name, status: "skip" }); continue; }

      const start = performance.now();
      let error = null;
      assertionCount = 0;
      expectedAssertions = null;
      try {
        for (const h of beforeEach) await runWithTimeout(h.fn, h.timeout ?? ctx.timeout, "beforeEach hook");
        await runWithTimeout(child.fn, child.timeout ?? ctx.timeout, `Test "${child.name}"`);
        if (expectedAssertions === -1 && assertionCount === 0) throw new AssertionError("expect.hasAssertions()\n\nExpected at least one assertion to be called");
        if (expectedAssertions !== null && expectedAssertions >= 0 && assertionCount !== expectedAssertions) {
          throw new AssertionError(`expect.assertions(${expectedAssertions})\n\nExpected ${expectedAssertions} assertions, received ${assertionCount}`);
        }
      } catch (e) {
        error = e;
      }
      for (const h of afterEach) {
        try {
          await runWithTimeout(h.fn, h.timeout ?? ctx.timeout, "afterEach hook");
        } catch (e) {
          error ??= e;
        }
      }
      for (const spy of spies.splice(0)) spy.mockRestore();
      ctx.report({ name, status: error ? "fail" : "pass", error, duration: performance.now() - start });
    }

    if (anyRuns) {
      for (const h of suite.hooks.afterAll) {
        try {
          await runWithTimeout(h.fn, h.timeout ?? ctx.timeout, "afterAll hook");
        } catch (e) {
          ctx.report({ name: `${suite.name || "(file)"} › afterAll`, status: "fail", error: e, duration: 0 });
        }
      }
    }
  }

  function isInOnlySuite(t) {
    for (let s = t.suite; s; s = s.parent) if (s.mode === "only") return true;
    return false;
  }

  const TEST_FILE = /(?:\.test|_test|\.spec|_spec)\.(?:[cm]?[jt]s)$/;

  function findTestFiles(dir, out) {
    let names;
    try {
      names = fs.readdirSync(dir);
    } catch {
      return out;
    }
    for (const name of names) {
      if (name === "node_modules" || name.startsWith(".")) continue;
      const full = path.join(dir, name);
      let st;
      try {
        st = fs.statSync(full);
      } catch {
        continue;
      }
      if (st.isDirectory()) findTestFiles(full, out);
      else if (TEST_FILE.test(name)) out.push(full);
    }
    return out;
  }

  internal.runTests = async (args) => {
    const cwd = process.cwd();
    const roots = [];
    const filters = [];
    let namePattern = null;
    let timeout = 5000;
    for (let i = 0; i < args.length; i++) {
      const a = args[i];
      if (a === "-t" || a === "--test-name-pattern" || a === "--filter") namePattern = new RegExp(args[++i] ?? "");
      else if (a.startsWith("--test-name-pattern=")) namePattern = new RegExp(a.slice(20));
      else if (a === "--timeout") timeout = Number(args[++i]);
      else if (a.startsWith("--timeout=")) timeout = Number(a.slice(10));
      else if (a.startsWith("-")) {
        process.stderr.write(`rtn test: unknown option ${a}\n`);
        return 1;
      } else {
        const full = path.resolve(cwd, a);
        let st = null;
        try { st = fs.statSync(full); } catch {}
        if (st?.isDirectory()) roots.push({ dir: full });
        else if (st?.isFile()) roots.push({ file: full });
        else filters.push(a);
      }
    }
    if (!Number.isFinite(timeout) || timeout <= 0) timeout = 5000;
    defaultTimeout = timeout;

    let files = [];
    if (roots.length === 0) findTestFiles(cwd, files);
    for (const r of roots) {
      if (r.file) files.push(r.file);
      else findTestFiles(r.dir, files);
    }
    if (filters.length) files = files.filter((f) => filters.some((p) => path.relative(cwd, f).includes(p)));
    files = [...new Set(files)].sort();

    write(`\n${blue("●")} ${bold("rtn test")} ${dim(`v${rtn.version}`)}\n`);
    if (files.length === 0) {
      write(`\n${red("No test files found")} ${dim("(looking for *.test.*, *_test.*, *.spec.* in " + (roots.length ? "the given paths" : cwd) + ")")}\n\n`);
      return 1;
    }

    const totals = { pass: 0, fail: 0, skip: 0, todo: 0 };
    const failures = [];
    const started = performance.now();

    for (const file of files) {
      const rel = path.relative(cwd, file);
      write(`\n${rel}:\n`);
      root = newSuite("", null);
      current = root;
      defaultTimeout = timeout;
      try {
        await import(file);
      } catch (e) {
        totals.fail++;
        failures.push({ file: rel, name: "(loading the file)", error: e });
        write(`  ${red("✗")} ${red("failed to load")}\n${indent(errorText(e), 6)}\n`);
        for (const line of userStack(e, file)) write(`      ${dim(line)}\n`);
        continue;
      }
      const ctx = {
        timeout: defaultTimeout,  // --timeout, or setDefaultTimeout() in the file
        namePattern,
        report(r) {
          totals[r.status]++;
          if (r.status === "pass") write(`  ${blue("✓")} ${r.name} ${dim(`[${ms(r.duration)}]`)}\n`);
          else if (r.status === "skip") write(`  ${dim("» " + r.name)}\n`);
          else if (r.status === "todo") write(`  ${dim("✎ todo: " + r.name)}\n`);
          else {
            failures.push({ file: rel, name: r.name, error: r.error });
            write(`  ${red("✗")} ${r.name} ${dim(`[${ms(r.duration)}]`)}\n`);
            write(indent(errorText(r.error), 6) + "\n");
            for (const line of userStack(r.error, file)) write(`      ${dim(line)}\n`);
          }
        },
      };
      await runSuite(root, ctx, false, hasOnly(root), [], []);
    }

    const elapsed = performance.now() - started;
    const parts = [blue(`${totals.pass} pass`)];
    if (totals.fail) parts.push(red(`${totals.fail} fail`));
    if (totals.skip) parts.push(dim(`${totals.skip} skip`));
    if (totals.todo) parts.push(dim(`${totals.todo} todo`));
    const count = totals.pass + totals.fail + totals.skip + totals.todo;
    write(`\n ${parts.join(dim("  ·  "))}\n`);
    write(dim(` Ran ${count} test${count === 1 ? "" : "s"} across ${files.length} file${files.length === 1 ? "" : "s"}. [${ms(elapsed)}]`) + "\n");
    if (failures.length) {
      write(`\n${red("Failed:")}\n`);
      for (const f of failures) write(`  ${red("✗")} ${f.file} ${dim("›")} ${f.name}\n`);
    }
    write("\n");
    return totals.fail > 0 ? 1 : 0;
  };

  function indent(text, n) {
    const pad = " ".repeat(n);
    return String(text).split("\n").map((l) => (l ? pad + l : l)).join("\n");
  }

  internal.modules.test = {
    test,
    it: test,
    describe,
    expect,
    beforeAll: hook("beforeAll"),
    afterAll: hook("afterAll"),
    beforeEach: hook("beforeEach"),
    afterEach: hook("afterEach"),
    mock,
    fn: mock,
    spyOn,
    setDefaultTimeout: (ms) => { defaultTimeout = ms; },
  };
});
