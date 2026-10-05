# Changelog

## Unreleased

- **`rtn init` asks TypeScript or JavaScript**: an arrow-key menu (↑/↓ or j/k, `1`/`2`, Enter; Esc or Ctrl+C cancels) picks the template. The JavaScript project has `index.js`, `greet.js` with JSDoc types, `greet.test.js` and a `jsconfig.json` with `checkJs`. `--ts` / `--js` (or `-y` for TypeScript) skip the question, and without a terminal — scripts, CI — TypeScript is chosen as before.

## 2.0.0 — 2026-10-05

The biggest release so far: rtn now runs npm packages, CommonJS code and your tests.

- **npm packages**: imports and `require()` resolve through `node_modules` with `package.json` `"exports"` (subpaths, `*` patterns, conditions), `"imports"` (`#internal`), `"main"`, scoped packages and nested `node_modules`. Checked with lodash, zod, dayjs, chalk 5, date-fns, uuid, nanoid, yaml, semver, picocolors, ms, kleur, mitt and eventemitter3.
- **CommonJS**: `require()`, `module.exports`, `__dirname` / `__filename`, `require.resolve` / `cache` / `main`, `createRequire()`, `.cjs` / `.cts` files and CommonJS entry points. ES modules import CommonJS (`module.exports` = default export, its properties = named exports), and `require()` loads ES modules without top-level `await`.
- **Node built-in modules**: `Buffer` / `node:buffer`, `node:events` (EventEmitter), `node:util`, `node:assert` (+ `assert/strict`), `node:os`, `node:url`, `node:crypto` (`createHash`/`createHmac` with sha256, sha1, md5, `randomBytes`, `randomInt`, …), `node:timers` (+ `timers/promises`), `node:module`, `node:tty`, `node:process`; `crypto.subtle.digest`. All built-ins work with or without `node:`. `tests/cases/node-compat.mjs` prints exactly what Node 22 prints.
- **`rtn test`**: a built-in test runner with the Jest/Bun API — `describe` / `test` / `it`, `.skip` / `.only` / `.todo` / `.each`, hooks, async and `done` tests, timeouts, 30+ `expect` matchers, `.resolves` / `.rejects`, asymmetric matchers, `mock()` and `spyOn()`. Calm black/white/blue output, exit code 1 on failure.
- **`rtn init`**: creates a TypeScript project that runs and tests out of the box.
- **Faster startup**: the built-in JavaScript is compiled to QuickJS bytecode at build time — `rtn -e 0` takes ~6 ms (1.6: ~10 ms) even though rtn has ~4,000 more lines of built-in JS.
- `process` is an `EventEmitter` (`process.on("exit")`), plus `process.emitWarning`, `global`, `setImmediate` / `clearImmediate`.
- `console.log` shows deep objects like Node (`[Map]`, `[MyClass]` instead of `[Object]`).

**Breaking changes**

- `process.version` now reports the Node version whose APIs rtn follows (`v22.12.0`) and `process.versions.node` exists, so npm packages take their Node code paths. rtn's own version: `process.versions.rtn` or `rtn.version`.
- A `.js` / `.ts` file that uses `require()` / `module.exports` and no `import` / `export` now runs as CommonJS (previously always an ES module).
- Bare specifiers (`import x from "pkg"`) now resolve npm packages instead of throwing; `fs`, `path`, `events`… without `node:` are the built-in modules.

## 1.6.2 — 2026-10-05

- **A calmer download screen** for `rtn upgrade` / `rtn update` and the installer: the terminal's own black/white text with a single blue accent, instead of the multi-color gradient. Works on dark and light terminals alike.
- **Smoother animation**: the progress bar and percentage glide toward the real value, a soft light-blue glint moves along the bar, and the bar fills to 100% before the step is checked off.
- **Installer** (`curl … | bash`): the same look, now with a live progress bar while the release downloads; the cursor is always restored, even on errors or Ctrl+C.

## 1.6.1 — 2026-10-05

- **`navigator`** global, like in Node, Deno and Bun: `userAgent` (`rtn/1.6.1`), `hardwareConcurrency`, `platform`, `language` / `languages` (from `LANG`).
- **`rtn --help`** now lists the built-in modules, the main globals and a link to the docs.

## 1.6.0 — 2026-10-05

- **`fetch()`**: HTTP/1.1 client for `http:` and `data:` URLs — redirects (`follow` / `error` / `manual`), `AbortSignal` and `AbortSignal.timeout()`, chunked / length / close-delimited responses, Node-style `TypeError: fetch failed` with a `cause`. `Response.url` / `redirected`, `Request.signal` / `redirect`. (`https:` needs TLS, not built in yet.)
- **Events**: `EventTarget`, `Event`, `CustomEvent`, `AbortController`, `AbortSignal` (`abort`, `timeout`, `any`, `throwIfAborted`).
- **`crypto.randomUUID()`, `crypto.getRandomValues()`, `structuredClone()`**.
- **`node:path`** (output identical to Node) and **`node:fs/promises`** (runs on a thread pool, never blocks the event loop); `fs`, `path`, `fs/promises` also work without the `node:` prefix, and `fs.promises` exists.
- **Runtime**: a small thread pool for blocking work (DNS, file I/O); `console` prints `DOMException` like Node.
- **`rtn upgrade` / `rtn update`**: animated in a terminal — gradient progress bar with speed and ETA, a checkmark per step, a summary box. Connect timeouts, clearer download errors, and Ctrl+C now cancels cleanly (temporary files removed, cursor restored, nothing changed).
- **Releases**: bumping the version in `CMakeLists.txt` on `main` creates the tag and publishes the release automatically.
- **Tests**: new `events`, `fetch`, `crypto`, `path`, `fs-promises` cases and `tests/fetch_test.py`; the HTTP and upgrade suites no longer use fixed ports (random failures).

## 1.5.0 — 2026-09-26

First public release.

- **Runtime**: ES modules, top-level `await`, event loop (microtasks → `process.nextTick` → timers → epoll I/O), REPL with top-level `await` and TypeScript syntax.
- **TypeScript**: built-in type stripper written in C++ (positions preserved, `enum`, parameter properties, import elision).
- **HTTP server**: `rtn.serve()` with keep-alive, pipelining, chunked bodies, `Expect: 100-continue`, idle/request timeouts and request-size limits.
- **Web APIs**: `URL`, `URLSearchParams`, `Headers`, `Request`, `Response`, `TextEncoder`, `TextDecoder`.
- **Node-style APIs**: `console` (incl. `table`, `group`, `count`, `trace`), `process`, `rtn:fs` / `node:fs` (sync).
- **Install & upgrade**: one-line installer (`curl -fsSL https://byteforgestudiolab.github.io/RunTime-Now/install | bash`), project page on GitHub Pages, `rtn upgrade` / `rtn update -r` with SHA-256 verification, static Linux x64/arm64 release binaries built by GitHub Actions.
- **Tooling**: `rtn strip` (show TS → JS output), `tools/loadgen` benchmark, test suite + GitHub Actions CI.
