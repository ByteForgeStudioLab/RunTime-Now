// Built-in modules written in JavaScript:
//   import path from "node:path"            (also "rtn:path", "path")
//   import fs from "node:fs"                (sync API from src/bindings/fs.cpp + fs.promises)
//   import fs from "node:fs/promises"       (also "rtn:fs/promises", "fs/promises", fs.promises)
//
// They are registered on internal.modules; src/modules.cpp turns each one
// into an ES module (named exports + default export).
(function (native, internal) {
  "use strict";

  const { fsAsync } = native;
  const SLASH = 47;
  const DOT = 46;

  // How Node describes a wrong argument: "type number (1)", "an instance of Array", "null".
  function describe(value) {
    if (value === null || value === undefined) return String(value);
    if (typeof value === "function") return `function ${value.name}`;
    if (typeof value === "object") return value.constructor?.name ? `an instance of ${value.constructor.name}` : "an object";
    let shown = typeof value === "string" ? `'${value}'` : String(value);
    if (typeof value === "bigint") shown += "n";
    if (shown.length > 28) shown = shown.slice(0, 25) + "...";
    return `type ${typeof value} (${shown})`;
  }

  function validateString(value, name) {
    if (typeof value !== "string") {
      const err = new TypeError(`The "${name}" argument must be of type string. Received ${describe(value)}`);
      err.code = "ERR_INVALID_ARG_TYPE";
      throw err;
    }
  }

  // ------------------------------------------------------------------
  // path (POSIX), the same algorithms as Node's lib/path.js
  // ------------------------------------------------------------------

  // Resolves "." and ".." segments; `allowAboveRoot` keeps leading "..".
  function normalizeString(path, allowAboveRoot) {
    let res = "";
    let lastSegmentLength = 0;
    let lastSlash = -1;
    let dots = 0;
    let code = 0;
    for (let i = 0; i <= path.length; ++i) {
      if (i < path.length) code = path.charCodeAt(i);
      else if (code === SLASH) break;
      else code = SLASH;

      if (code === SLASH) {
        if (lastSlash === i - 1 || dots === 1) {
          // empty segment or "."
        } else if (dots === 2) {
          if (res.length < 2 || lastSegmentLength !== 2 ||
              res.charCodeAt(res.length - 1) !== DOT || res.charCodeAt(res.length - 2) !== DOT) {
            if (res.length > 2) {
              const lastSlashIndex = res.lastIndexOf("/");
              if (lastSlashIndex === -1) {
                res = "";
                lastSegmentLength = 0;
              } else {
                res = res.slice(0, lastSlashIndex);
                lastSegmentLength = res.length - 1 - res.lastIndexOf("/");
              }
              lastSlash = i;
              dots = 0;
              continue;
            } else if (res.length !== 0) {
              res = "";
              lastSegmentLength = 0;
              lastSlash = i;
              dots = 0;
              continue;
            }
          }
          if (allowAboveRoot) {
            res += res.length > 0 ? "/.." : "..";
            lastSegmentLength = 2;
          }
        } else {
          if (res.length > 0) res += "/" + path.slice(lastSlash + 1, i);
          else res = path.slice(lastSlash + 1, i);
          lastSegmentLength = i - lastSlash - 1;
        }
        lastSlash = i;
        dots = 0;
      } else if (code === DOT && dots !== -1) {
        ++dots;
      } else {
        dots = -1;
      }
    }
    return res;
  }

  // Shared by extname() and parse(): where the extension starts, scanning from the end.
  function scanName(path, start) {
    let startDot = -1;
    let startPart = 0;
    let end = -1;
    let matchedSlash = true;
    let preDotState = 0;  // 0: no dot yet, 1: dot right after the name start, -1: name before the dot
    for (let i = path.length - 1; i >= start; --i) {
      const code = path.charCodeAt(i);
      if (code === SLASH) {
        if (!matchedSlash) {
          startPart = i + 1;
          break;
        }
        continue;
      }
      if (end === -1) {
        matchedSlash = false;
        end = i + 1;
      }
      if (code === DOT) {
        if (startDot === -1) startDot = i;
        else if (preDotState !== 1) preDotState = 1;
      } else if (startDot !== -1) {
        preDotState = -1;
      }
    }
    const noExt = startDot === -1 || end === -1 || preDotState === 0 ||
      (preDotState === 1 && startDot === end - 1 && startDot === startPart + 1);
    return { startDot: noExt ? -1 : startDot, startPart, end };
  }

  const path = {
    sep: "/",
    delimiter: ":",

    resolve(...args) {
      let resolved = "";
      let absolute = false;
      for (let i = args.length - 1; i >= -1 && !absolute; i--) {
        const p = i >= 0 ? args[i] : process.cwd();
        validateString(p, `paths[${i}]`);
        if (p.length === 0) continue;
        resolved = `${p}/${resolved}`;
        absolute = p.charCodeAt(0) === SLASH;
      }
      resolved = normalizeString(resolved, !absolute);
      if (absolute) return `/${resolved}`;
      return resolved.length > 0 ? resolved : ".";
    },

    normalize(p) {
      validateString(p, "path");
      if (p.length === 0) return ".";
      const absolute = p.charCodeAt(0) === SLASH;
      const trailing = p.charCodeAt(p.length - 1) === SLASH;
      p = normalizeString(p, !absolute);
      if (p.length === 0) return absolute ? "/" : trailing ? "./" : ".";
      if (trailing) p += "/";
      return absolute ? `/${p}` : p;
    },

    isAbsolute(p) {
      validateString(p, "path");
      return p.length > 0 && p.charCodeAt(0) === SLASH;
    },

    join(...args) {
      let joined;
      for (const arg of args) {
        validateString(arg, "path");
        if (arg.length > 0) joined = joined === undefined ? arg : `${joined}/${arg}`;
      }
      return joined === undefined ? "." : path.normalize(joined);
    },

    relative(from, to) {
      validateString(from, "from");
      validateString(to, "to");
      if (from === to) return "";
      from = path.resolve(from);
      to = path.resolve(to);
      if (from === to) return "";
      const fromStart = 1;
      const fromEnd = from.length;
      const fromLen = fromEnd - fromStart;
      const toStart = 1;
      const toLen = to.length - toStart;
      const length = fromLen < toLen ? fromLen : toLen;
      let lastCommonSep = -1;
      let i = 0;
      for (; i < length; i++) {
        const code = from.charCodeAt(fromStart + i);
        if (code !== to.charCodeAt(toStart + i)) break;
        if (code === SLASH) lastCommonSep = i;
      }
      if (i === length) {
        if (toLen > length) {
          if (to.charCodeAt(toStart + i) === SLASH) return to.slice(toStart + i + 1);  // from is a prefix of to
          if (i === 0) return to.slice(toStart + i);  // from is "/"
        } else if (fromLen > length) {
          if (from.charCodeAt(fromStart + i) === SLASH) lastCommonSep = i;
          else if (i === 0) lastCommonSep = 0;
        }
      }
      let out = "";
      for (i = fromStart + lastCommonSep + 1; i <= fromEnd; ++i) {
        if (i === fromEnd || from.charCodeAt(i) === SLASH) out += out.length === 0 ? ".." : "/..";
      }
      return `${out}${to.slice(toStart + lastCommonSep)}`;
    },

    dirname(p) {
      validateString(p, "path");
      if (p.length === 0) return ".";
      const hasRoot = p.charCodeAt(0) === SLASH;
      let end = -1;
      let matchedSlash = true;
      for (let i = p.length - 1; i >= 1; --i) {
        if (p.charCodeAt(i) === SLASH) {
          if (!matchedSlash) {
            end = i;
            break;
          }
        } else {
          matchedSlash = false;
        }
      }
      if (end === -1) return hasRoot ? "/" : ".";
      if (hasRoot && end === 1) return "//";
      return p.slice(0, end);
    },

    basename(p, suffix) {
      if (suffix !== undefined) validateString(suffix, "suffix");
      validateString(p, "path");
      let start = 0;
      let end = -1;
      let matchedSlash = true;
      if (suffix !== undefined && suffix.length > 0 && suffix.length <= p.length) {
        if (suffix === p) return "";
        let extIdx = suffix.length - 1;
        let firstNonSlashEnd = -1;
        for (let i = p.length - 1; i >= 0; --i) {
          const code = p.charCodeAt(i);
          if (code === SLASH) {
            if (!matchedSlash) {
              start = i + 1;
              break;
            }
          } else {
            if (firstNonSlashEnd === -1) {
              matchedSlash = false;
              firstNonSlashEnd = i + 1;
            }
            if (extIdx >= 0) {
              if (code === suffix.charCodeAt(extIdx)) {
                if (--extIdx === -1) end = i;
              } else {
                extIdx = -1;
                end = firstNonSlashEnd;
              }
            }
          }
        }
        if (start === end) end = firstNonSlashEnd;
        else if (end === -1) end = p.length;
        return p.slice(start, end);
      }
      for (let i = p.length - 1; i >= 0; --i) {
        if (p.charCodeAt(i) === SLASH) {
          if (!matchedSlash) {
            start = i + 1;
            break;
          }
        } else if (end === -1) {
          matchedSlash = false;
          end = i + 1;
        }
      }
      return end === -1 ? "" : p.slice(start, end);
    },

    extname(p) {
      validateString(p, "path");
      const { startDot, end } = scanName(p, 0);
      return startDot === -1 ? "" : p.slice(startDot, end);
    },

    parse(p) {
      validateString(p, "path");
      const ret = { root: "", dir: "", base: "", ext: "", name: "" };
      if (p.length === 0) return ret;
      const absolute = p.charCodeAt(0) === SLASH;
      if (absolute) ret.root = "/";
      const { startDot, startPart, end } = scanName(p, absolute ? 1 : 0);
      if (end !== -1) {
        const start = startPart === 0 && absolute ? 1 : startPart;
        if (startDot === -1) {
          ret.base = ret.name = p.slice(start, end);
        } else {
          ret.name = p.slice(start, startDot);
          ret.base = p.slice(start, end);
          ret.ext = p.slice(startDot, end);
        }
      }
      if (startPart > 0) ret.dir = p.slice(0, startPart - 1);
      else if (absolute) ret.dir = "/";
      return ret;
    },

    format(obj) {
      if (obj === null || typeof obj !== "object") {
        throw new TypeError(`The "pathObject" argument must be of type object. Received ${obj === null ? "null" : typeof obj}`);
      }
      const dir = obj.dir || obj.root;
      const ext = obj.ext ? (obj.ext[0] === "." ? "" : ".") + obj.ext : "";
      const base = obj.base || `${obj.name || ""}${ext}`;
      if (!dir) return base;
      return dir === obj.root ? `${dir}${base}` : `${dir}/${base}`;
    },

    toNamespacedPath(p) {
      return p;
    },
  };
  path.posix = path;

  // ------------------------------------------------------------------
  // fs/promises: the operations run on the thread pool (src/bindings/fs.cpp)
  // ------------------------------------------------------------------

  function call(op, p, arg, flags) {
    validateString(p, "path");
    return new Promise((resolve, reject) => {
      fsAsync(op, p, arg, flags, (err, value) => (err ? reject(err) : resolve(value)));
    });
  }

  const isText = (options) => typeof options === "string" || typeof options?.encoding === "string";
  const toData = (data) => {
    if (typeof data === "string" || data instanceof Uint8Array) return data;
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    return String(data);
  };
  const recursiveFlags = (options) => (options?.recursive ? 1 : 0) | (options?.force ? 2 : 0);

  const constants = Object.freeze({ F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1 });

  const promises = {
    async readFile(p, options) { return call("readFile", p, undefined, isText(options) ? 1 : 0); },
    async writeFile(p, data) { return call("writeFile", p, toData(data), 0); },
    async appendFile(p, data) { return call("appendFile", p, toData(data), 0); },
    async readdir(p) { return call("readdir", p, undefined, 0); },
    async mkdir(p, options) { return call("mkdir", p, undefined, recursiveFlags(options)); },
    async rm(p, options) { return call("rm", p, undefined, recursiveFlags(options)); },
    async rmdir(p) { return call("rmdir", p, undefined, 0); },
    async unlink(p) { return call("unlink", p, undefined, 0); },
    async rename(from, to) {
      validateString(to, "newPath");
      return call("rename", from, to, 0);
    },
    async copyFile(from, to) {
      validateString(to, "dest");
      return call("copyFile", from, to, 0);
    },
    async stat(p) { return call("stat", p, undefined, 0); },
    async lstat(p) { return call("lstat", p, undefined, 0); },
    async access(p, mode = constants.F_OK) { return call("access", p, undefined, mode | 0); },
    async realpath(p) { return call("realpath", p, undefined, 0); },
    constants,
  };

  // The C functions are non-enumerable on native.fsSync; copy them as normal properties.
  const fsSync = Object.fromEntries(Object.getOwnPropertyNames(native.fsSync).map((k) => [k, native.fsSync[k]]));
  const fsModule = {
    ...fsSync,
    promises,
    constants: Object.freeze({ ...constants, O_RDONLY: 0, O_WRONLY: 1, O_RDWR: 2 }),
  };

  internal.path = path;
  internal.modules = { "path": path, "fs/promises": promises, "fs": fsModule };
});
