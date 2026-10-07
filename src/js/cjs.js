// npm packages and CommonJS.
//
//   const _ = require("lodash")            CommonJS: require(), module.exports, __dirname
//   import { z } from "zod"                packages from node_modules ("exports", "main", "type")
//   import cjs from "./legacy.cjs"         ES modules can import CommonJS (module.exports = default)
//   const esm = require("./esm.mjs")       ...and CommonJS can require ES modules (no top-level await)
//
// Resolution follows Node: node_modules lookup up the directory tree,
// package.json "exports" (conditions: rtn, node, import/require, default) and
// "imports" (#specifiers), "main", then index files. Whether a .js/.ts file is
// CommonJS comes from the nearest package.json "type"; without one, from its
// syntax (like Node 22). src/modules.cpp calls the internal.* hooks below.
(function (native, internal) {
  "use strict";

  const { resolveFile, pathKind, readText, loadSource, compileFunction, requireESM } = native;
  const path = internal.path;

  // Node's built-in modules that work without the "node:" prefix (as in src/modules.cpp).
  const UNPREFIXED = new Set(["fs", "fs/promises", "path", "path/posix", "events", "util", "os", "assert",
    "assert/strict", "module", "buffer", "url", "process", "timers", "timers/promises", "crypto", "util/types", "tty", "child_process"]);

  function builtinKey(spec) {
    let key;
    if (spec.startsWith("node:")) key = spec.slice(5);
    else if (spec.startsWith("rtn:")) key = spec.slice(4);
    else if (UNPREFIXED.has(spec)) key = spec;
    else return null;
    if (key === "path/posix") key = "path";
    return Object.hasOwn(internal.modules, key) ? key : null;
  }

  function codedError(Ctor, message, code) {
    const e = new Ctor(message);
    e.code = code;
    return e;
  }

  // ------------------------------------------------------------------
  // package.json
  // ------------------------------------------------------------------

  const packageJsonCache = new Map();  // directory -> parsed package.json | null

  function readPackageJson(dir) {
    if (packageJsonCache.has(dir)) return packageJsonCache.get(dir);
    const file = path.join(dir, "package.json");
    const text = readText(file);
    let pkg = null;
    if (text !== null) {
      try {
        pkg = JSON.parse(text);
      } catch (e) {
        throw codedError(SyntaxError, `Invalid package.json ${file}: ${e.message}`, "ERR_INVALID_PACKAGE_CONFIG");
      }
      if (pkg === null || typeof pkg !== "object") pkg = {};
    }
    packageJsonCache.set(dir, pkg);
    return pkg;
  }

  // The nearest package.json above `file`: { dir, pkg } or null.
  function packageScope(file) {
    for (let dir = path.dirname(file); ; dir = path.dirname(dir)) {
      if (path.basename(dir) === "node_modules") return null;
      const pkg = readPackageJson(dir);
      if (pkg) return { dir, pkg };
      if (dir === "/" || dir === ".") return null;
    }
  }

  // ------------------------------------------------------------------
  // CommonJS or ES module?
  // ------------------------------------------------------------------

  // Static import/export statements or import.meta: an ES module.
  const ESM_SYNTAX = /^[ \t]*(?:import[ \t]*(?:[\w$]+[ \t]*(?:,|from\b)|\{|\*|["'])|export[ \t]+(?:default\b|const\b|let\b|var\b|function\b|class\b|async\b|\{|\*))|\bimport\.meta\b/m;
  const CJS_SYNTAX = /\brequire[ \t]*\(|\bmodule\.exports\b|\bexports\.[\w$]+[ \t]*=|\b__dirname\b|\b__filename\b/;
  const kindCache = new Map();

  function isCommonJS(file) {
    if (/\.c[jt]s$/.test(file)) return true;
    if (/\.(?:m[jt]s|json)$/.test(file)) return false;
    if (kindCache.has(file)) return kindCache.get(file);
    const type = packageScope(file)?.pkg.type;
    let cjs;
    if (type === "module") cjs = false;
    else if (type === "commonjs") cjs = true;
    else {
      const src = readText(file) ?? "";
      cjs = !ESM_SYNTAX.test(src) && (CJS_SYNTAX.test(src) || file.includes("/node_modules/"));
    }
    kindCache.set(file, cjs);
    return cjs;
  }

  // ------------------------------------------------------------------
  // Resolution
  // ------------------------------------------------------------------

  const CONDITIONS = {
    import: ["rtn", "node", "import", "default"],
    require: ["rtn", "node", "require", "default"],
  };

  // One "exports" / "imports" target: a string, an array (first that works), or conditions.
  function resolveTarget(target, match, conditions) {
    if (typeof target === "string") {
      if (!target.startsWith("./")) return undefined;
      return match === null ? target : target.replaceAll("*", match);
    }
    if (Array.isArray(target)) {
      for (const t of target) {
        const r = resolveTarget(t, match, conditions);
        if (r) return r;
      }
      return undefined;
    }
    if (target !== null && typeof target === "object") {
      for (const key of Object.keys(target)) {
        if (key === "default" || conditions.includes(key)) {
          const r = resolveTarget(target[key], match, conditions);
          if (r !== undefined) return r;
        }
      }
      return undefined;
    }
    return null;  // null: explicitly not available
  }

  // Looks `key` ("." / "./sub" / "#name") up in an exports or imports map.
  function resolveMap(map, key, conditions) {
    if (Object.hasOwn(map, key) && !key.includes("*")) return resolveTarget(map[key], null, conditions);
    let best = null;
    for (const k of Object.keys(map)) {
      const star = k.indexOf("*");
      if (star >= 0) {
        const prefix = k.slice(0, star);
        const suffix = k.slice(star + 1);
        if (key.startsWith(prefix) && key.endsWith(suffix) && key.length >= k.length - 1 &&
            (!best || prefix.length > best.length)) {
          best = { k, length: prefix.length, match: key.slice(prefix.length, key.length - suffix.length) };
        }
      } else if (k.endsWith("/") && key.startsWith(k) && (!best || k.length > best.length)) {
        best = { k, length: k.length, folder: key.slice(k.length) };  // old "./dir/" mappings
      }
    }
    if (!best) return undefined;
    if (best.folder !== undefined) {
      const t = resolveTarget(map[best.k], null, conditions);
      return t ? t + best.folder : t;
    }
    return resolveTarget(map[best.k], best.match, conditions);
  }

  function resolveExports(exports, sub, conditions) {
    const isMap = exports !== null && typeof exports === "object" && !Array.isArray(exports) &&
      Object.keys(exports).some((k) => k.startsWith("."));
    return resolveMap(isMap ? exports : { ".": exports }, sub, conditions);
  }

  function notFound(spec, parent, kind) {
    if (kind === "require") {
      return codedError(Error, `Cannot find module '${spec}'\nRequire stack:\n- ${parent}`, "MODULE_NOT_FOUND");
    }
    const what = spec.startsWith(".") || spec.startsWith("/") ? "module" : "package";
    return codedError(Error, `Cannot find ${what} '${spec}' imported from ${parent}`, "ERR_MODULE_NOT_FOUND");
  }

  const EXTENSIONS = [".js", ".mjs", ".cjs", ".ts", ".mts", ".cts", ".json"];

  // A path inside a package or a relative require: file, file + extension, directory.
  function resolvePath(target, spec, parent, kind) {
    const k = pathKind(target);
    if (k === "file") return target;
    for (const ext of EXTENSIONS) {
      if (pathKind(target + ext) === "file") return target + ext;
    }
    if (k === "dir") {
      const pkg = readPackageJson(target);
      if (pkg && typeof pkg.main === "string" && pkg.main) {
        const main = resolveFile(target, pkg.main);
        if (main) return main;
      }
      const index = resolveFile(target, "./index");
      if (index) return index;
    }
    const r = resolveFile(path.dirname(target), "./" + path.basename(target));  // .js -> .ts and friends
    if (r) return r;
    throw notFound(spec, parent, kind);
  }

  function fromPackage(pkgDir, pkg, sub, spec, parent, kind) {
    const conditions = CONDITIONS[kind];
    if (pkg && pkg.exports !== undefined && pkg.exports !== null) {
      const target = resolveExports(pkg.exports, sub, conditions);
      if (!target) {
        throw codedError(Error, `Package subpath '${sub}' is not defined by "exports" in ` +
          `${path.join(pkgDir, "package.json")} imported from ${parent}`, "ERR_PACKAGE_PATH_NOT_EXPORTED");
      }
      const file = path.join(pkgDir, target);
      if (pathKind(file) === "file") return file;
      throw notFound(spec, parent, kind);
    }
    if (sub === ".") {
      for (const field of kind === "import" ? ["main", "module"] : ["main"]) {
        if (pkg && typeof pkg[field] === "string" && pkg[field]) {
          const r = resolveFile(pkgDir, pkg[field]);
          if (r) return r;
        }
      }
      const index = resolveFile(pkgDir, "./index");
      if (index) return index;
      throw notFound(spec, parent, kind);
    }
    return resolvePath(path.join(pkgDir, sub), spec, parent, kind);
  }

  function resolveBare(spec, parent, kind) {
    if (spec.startsWith("#")) {  // package.json "imports"
      const scope = packageScope(parent);
      const imports = scope?.pkg.imports;
      const target = imports && typeof imports === "object" ? resolveMap(imports, spec, CONDITIONS[kind]) : undefined;
      if (!target) {
        throw codedError(TypeError, `Package import specifier "${spec}" is not defined in ` +
          `${scope ? path.join(scope.dir, "package.json") : "package.json"} imported from ${parent}`,
          "ERR_PACKAGE_IMPORT_NOT_DEFINED");
      }
      return resolvePath(path.join(scope.dir, target), spec, parent, kind);
    }
    const parts = spec.split("/");
    const n = spec.startsWith("@") ? 2 : 1;
    if (parts.length < n || parts.slice(0, n).some((p) => !p)) throw notFound(spec, parent, kind);
    const name = parts.slice(0, n).join("/");
    const sub = parts.length > n ? "./" + parts.slice(n).join("/") : ".";

    // A package can import itself by name ("exports" required, as in Node).
    const scope = packageScope(parent);
    if (scope && scope.pkg.name === name && scope.pkg.exports !== undefined) {
      return fromPackage(scope.dir, scope.pkg, sub, spec, parent, kind);
    }
    for (let dir = path.dirname(parent); ; dir = path.dirname(dir)) {
      if (path.basename(dir) !== "node_modules") {
        const pkgDir = path.join(dir, "node_modules", name);
        if (pathKind(pkgDir) === "dir") return fromPackage(pkgDir, readPackageJson(pkgDir), sub, spec, parent, kind);
      }
      if (dir === "/" || dir === ".") break;
    }
    for (const dir of (process.env.NODE_PATH ?? "").split(":").filter(Boolean)) {
      const pkgDir = path.join(path.resolve(dir), name);
      if (pathKind(pkgDir) === "dir") return fromPackage(pkgDir, readPackageJson(pkgDir), sub, spec, parent, kind);
    }
    throw notFound(spec, parent, kind);
  }

  function resolveRequire(spec, parent) {
    if (typeof spec !== "string" || spec === "") {
      throw codedError(TypeError, `The "id" argument must be a non-empty string`, "ERR_INVALID_ARG_VALUE");
    }
    if (spec.startsWith("file://")) spec = decodeURIComponent(new URL(spec).pathname);
    if (spec.startsWith("./") || spec.startsWith("../") || spec.startsWith("/") || spec === "." || spec === "..") {
      return resolvePath(path.resolve(path.dirname(parent), spec), spec, parent, "require");
    }
    return resolveBare(spec, parent, "require");
  }

  // ------------------------------------------------------------------
  // Module and require()
  // ------------------------------------------------------------------

  function nodeModulePaths(dir) {
    const paths = [];
    for (let d = dir; ; d = path.dirname(d)) {
      if (path.basename(d) !== "node_modules") paths.push(path.join(d, "node_modules"));
      if (d === "/" || d === ".") break;
    }
    return paths;
  }

  let mainModule;

  class Module {
    constructor(id = "", parent = null) {
      this.id = id;
      this.filename = id || null;
      this.path = id ? path.dirname(id) : ".";
      this.exports = {};
      this.loaded = false;
      this.children = [];
      this.paths = nodeModulePaths(this.path);
      Object.defineProperty(this, "parent", { value: parent, writable: true, enumerable: false });
      if (parent) parent.children.push(this);
    }

    require(id) {
      return requireFrom(this, id);
    }
  }

  const cache = Object.create(null);

  function requireFrom(parent, spec) {
    if (typeof spec !== "string") {
      throw codedError(TypeError, `The "id" argument must be of type string. Received ${typeof spec}`, "ERR_INVALID_ARG_TYPE");
    }
    const key = builtinKey(spec);
    if (key !== null) return internal.modules[key];
    const from = parent.filename ?? path.join(process.cwd(), "[eval]");
    return loadFile(resolveRequire(spec, from), parent);
  }

  function loadFile(file, parent) {
    const cached = cache[file];
    if (cached) return cached.exports;
    const module = new Module(file, parent);
    if (file === internal.mainPath && !mainModule) {
      mainModule = module;
      module.id = ".";
    }
    cache[file] = module;
    let ok = false;
    try {
      if (file.endsWith(".json")) {
        try {
          module.exports = JSON.parse(readText(file));
        } catch (e) {
          e.message = `${file}: ${e.message}`;
          throw e;
        }
      } else if (isCommonJS(file)) {
        const code = loadSource(file);
        // One line for the wrapper head keeps line numbers right in stack traces.
        const fn = compileFunction(`(function (exports, require, module, __filename, __dirname) {${code}\n})`, file);
        fn.call(module.exports, module.exports, makeRequire(module), module, file, path.dirname(file));
      } else {
        module.exports = requireESM(file);
      }
      ok = true;
    } finally {
      if (!ok) delete cache[file];
    }
    module.loaded = true;
    return module.exports;
  }

  function makeRequire(module) {
    const require = (id) => requireFrom(module, id);
    require.resolve = (id) => {
      const key = builtinKey(id);
      if (key !== null) return id;
      return resolveRequire(id, module.filename ?? path.join(process.cwd(), "[eval]"));
    };
    require.resolve.paths = (id) => (builtinKey(id) !== null ? null : nodeModulePaths(module.path));
    require.cache = cache;
    require.extensions = Object.create(null);
    Object.defineProperty(require, "main", { get: () => mainModule, enumerable: true });
    return require;
  }

  function createRequire(filename) {
    if (filename instanceof URL) filename = decodeURIComponent(filename.pathname);
    else if (typeof filename === "string" && filename.startsWith("file://")) filename = decodeURIComponent(new URL(filename).pathname);
    if (typeof filename !== "string" || !path.isAbsolute(filename)) {
      throw codedError(TypeError, "createRequire() needs an absolute path or a file: URL, e.g. createRequire(import.meta.url)",
        "ERR_INVALID_ARG_VALUE");
    }
    return makeRequire(new Module(filename, null));
  }

  const builtinModules = Object.keys(internal.modules).filter((k) => k !== "test").sort();

  Object.assign(Module, {
    createRequire,
    builtinModules,
    isBuiltin: (name) => builtinKey(String(name)) !== null && !String(name).startsWith("rtn:"),
    _cache: cache,
    _nodeModulePaths: nodeModulePaths,
    Module,
  });

  internal.modules.module = Module;

  // Hooks for src/modules.cpp and src/runtime.cpp.
  internal.isCommonJS = isCommonJS;
  internal.resolveImport = (spec, parent) => resolveBare(spec, parent, "import");
  internal.requireFromImport = (file) => loadFile(file, null);
  internal.setMainPath = (file) => { internal.mainPath = file; };
});
