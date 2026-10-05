// navigator: the same small subset Node, Deno and Bun expose.
(function (native, internal) {
  "use strict";

  const INSPECT = Symbol.for("rtn.inspect");
  const ILLEGAL = Symbol("Navigator constructor");

  // "uz_UZ.UTF-8" -> "uz-UZ"; "C" / "POSIX" / unset -> "en-US"
  function languageFromEnv() {
    const raw = process.env.LC_ALL || process.env.LC_MESSAGES || process.env.LANG || "";
    const m = /^([a-z]{2,3})(?:_([A-Z]{2}))?/.exec(raw);
    return m ? (m[2] ? `${m[1]}-${m[2]}` : m[1]) : "en-US";
  }

  class Navigator {
    constructor(token) {
      if (token !== ILLEGAL) throw new TypeError("Illegal constructor");
    }
    get userAgent() { return `rtn/${native.version}`; }
    get hardwareConcurrency() { return native.cpuCount(); }
    get platform() { return "linux"; }
    get language() { return languageFromEnv(); }
    get languages() { return [languageFromEnv()]; }
    get [Symbol.toStringTag]() { return "Navigator"; }
    [INSPECT]() {
      return { userAgent: this.userAgent, hardwareConcurrency: this.hardwareConcurrency, platform: this.platform, language: this.language };
    }
  }

  Object.defineProperty(globalThis, "navigator", {
    value: new Navigator(ILLEGAL), writable: true, configurable: true, enumerable: true,
  });
  Object.defineProperty(globalThis, "Navigator", { value: Navigator, writable: true, configurable: true, enumerable: false });
});
