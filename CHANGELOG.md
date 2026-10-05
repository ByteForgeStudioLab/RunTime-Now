# Changelog

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
