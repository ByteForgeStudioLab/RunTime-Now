# Changelog

## 0.1.0 — 2026-09-25

First public version.

- **Runtime**: ES modules, top-level `await`, event loop (microtasks → `process.nextTick` → timers → epoll I/O), REPL with top-level `await` and TypeScript syntax.
- **TypeScript**: built-in type stripper written in C++ (positions preserved, `enum`, parameter properties, import elision).
- **HTTP server**: `rtn.serve()` with keep-alive, pipelining, chunked bodies, `Expect: 100-continue`, idle/request timeouts and request-size limits.
- **Web APIs**: `URL`, `URLSearchParams`, `Headers`, `Request`, `Response`, `TextEncoder`, `TextDecoder`.
- **Node-style APIs**: `console` (incl. `table`, `group`, `count`, `trace`), `process`, `rtn:fs` / `node:fs` (sync).
- **Tooling**: `rtn strip` (show TS → JS output), `tools/loadgen` benchmark, test suite + GitHub Actions CI.
