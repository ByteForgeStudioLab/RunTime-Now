// Module resolution and loading for `import` (ES modules).
//
//   import fs from "node:fs"        built-in modules (src/js/modules.js etc.)
//   import x from "./x.ts"          files, with Node/Bun-style extension lookup
//   import _ from "lodash"          npm packages in node_modules (resolved in src/js/cjs.js)
//   import cjs from "./old.cjs"     CommonJS files become synthetic ES modules
//
// The CommonJS loader and the package resolver are written in JS
// (src/js/cjs.js); this file connects them to the engine and gives them a few
// native helpers (add_module_natives).

#include "modules.hpp"

#include <cstring>
#include <filesystem>
#include <map>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "typescript/strip.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace rtn {

namespace {

// Node's built-in modules that also work without the "node:" prefix.
constexpr const char* kUnprefixedBuiltins[] = {
    "fs", "fs/promises", "path", "path/posix", "events", "util", "os", "assert", "assert/strict",
    "module", "buffer", "url", "process", "timers", "timers/promises", "crypto", "util/types", "tty",
};

// "node:path/posix" -> "path" (the key in internal.modules).
std::string builtin_key(std::string name) {
    for (const char* prefix : {"rtn:", "node:"}) {
        if (name.starts_with(prefix)) name.erase(0, std::strlen(prefix));
    }
    if (name == "path/posix") name = "path";
    return name;
}

bool has_builtin(JSContext* ctx, const std::string& key) {
    JSValue v = builtin_module_exports(ctx, key);
    bool found = !JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    return found;
}

// "fs" / "node:fs" / "rtn:fs" -> "node:fs"; "" if `spec` is not a built-in module.
std::string builtin_name(JSContext* ctx, const std::string& spec) {
    bool prefixed = spec.starts_with("node:") || spec.starts_with("rtn:");
    if (!prefixed) {
        bool known = false;
        for (const char* b : kUnprefixedBuiltins) known |= spec == b;
        if (!known) return {};
    }
    std::string key = builtin_key(spec);
    if (!has_builtin(ctx, key)) return {};
    return key == "test" ? "rtn:test" : "node:" + key;
}

// A built-in module written in JS: every property of its exports object
// becomes a named export, and the object itself is the default export.
int js_module_init(JSContext* ctx, JSModuleDef* m) {
    JSAtom atom = JS_GetModuleName(ctx, m);
    const char* name = JS_AtomToCString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    JSValue exports = builtin_module_exports(ctx, builtin_key(name ? name : ""));
    JS_FreeCString(ctx, name);
    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &len, exports, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < len; ++i) {
            const char* key = JS_AtomToCString(ctx, props[i].atom);
            if (std::strcmp(key, "default") != 0) {
                JS_SetModuleExport(ctx, m, key, JS_GetProperty(ctx, exports, props[i].atom));
            }
            JS_FreeCString(ctx, key);
        }
        JS_FreePropertyEnum(ctx, props, len);
    }
    return JS_SetModuleExport(ctx, m, "default", exports);
}

// Declares one export per own enumerable property of `exports` (+ "default").
void add_exports(JSContext* ctx, JSModuleDef* m, JSValueConst exports) {
    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    if (JS_IsObject(exports) &&
        JS_GetOwnPropertyNames(ctx, &props, &len, exports, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < len; ++i) {
            const char* key = JS_AtomToCString(ctx, props[i].atom);
            if (std::strcmp(key, "default") != 0 && std::strcmp(key, "__esModule") != 0) {
                JS_AddModuleExport(ctx, m, key);
            }
            JS_FreeCString(ctx, key);
        }
        JS_FreePropertyEnum(ctx, props, len);
    }
    JS_AddModuleExport(ctx, m, "default");
}

JSModuleDef* create_js_module(JSContext* ctx, const char* name) {
    JSValue exports = builtin_module_exports(ctx, builtin_key(name));
    JSModuleDef* m = JS_NewCModule(ctx, name, js_module_init);
    if (m) add_exports(ctx, m, exports);
    JS_FreeValue(ctx, exports);
    return m;
}

// --- CommonJS files imported from ES modules ---------------------------------
//
// The file runs (through require) when it is loaded; its module.exports is
// the default export, and its own enumerable properties are named exports —
// the same as Node.

std::map<std::string, JSValue> g_cjs_exports;  // module name -> module.exports, until init runs
bool g_cjs_hook_installed = false;

int cjs_module_init(JSContext* ctx, JSModuleDef* m) {
    JSAtom atom = JS_GetModuleName(ctx, m);
    const char* name = JS_AtomToCString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    auto it = g_cjs_exports.find(name ? name : "");
    JS_FreeCString(ctx, name);
    if (it == g_cjs_exports.end()) return -1;
    JSValue exports = it->second;
    g_cjs_exports.erase(it);

    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    if (JS_IsObject(exports) &&
        JS_GetOwnPropertyNames(ctx, &props, &len, exports, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < len; ++i) {
            const char* key = JS_AtomToCString(ctx, props[i].atom);
            if (std::strcmp(key, "default") != 0 && std::strcmp(key, "__esModule") != 0) {
                JS_SetModuleExport(ctx, m, key, JS_GetProperty(ctx, exports, props[i].atom));
            }
            JS_FreeCString(ctx, key);
        }
        JS_FreePropertyEnum(ctx, props, len);
    }
    return JS_SetModuleExport(ctx, m, "default", exports);
}

JSModuleDef* create_cjs_module(JSContext* ctx, const char* name) {
    JSValue path = JS_NewString(ctx, name);
    JSValue exports = call_internal(ctx, "requireFromImport", 1, &path);
    JS_FreeValue(ctx, path);
    if (JS_IsException(exports)) return nullptr;
    JSModuleDef* m = JS_NewCModule(ctx, name, cjs_module_init);
    if (!m) {
        JS_FreeValue(ctx, exports);
        return nullptr;
    }
    add_exports(ctx, m, exports);
    if (!g_cjs_hook_installed) {
        g_cjs_hook_installed = true;
        Runtime::from(ctx)->on_shutdown([ctx] {
            for (auto& [n, v] : g_cjs_exports) JS_FreeValue(ctx, v);
            g_cjs_exports.clear();
        });
    }
    if (auto old = g_cjs_exports.find(name); old != g_cjs_exports.end()) JS_FreeValue(ctx, old->second);
    g_cjs_exports[name] = exports;
    return m;
}

bool is_commonjs(JSContext* ctx, const std::string& path) {
    JSValue p = JS_NewStringLen(ctx, path.data(), path.size());
    JSValue r = call_internal(ctx, "isCommonJS", 1, &p);
    JS_FreeValue(ctx, p);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    bool cjs = JS_ToBool(ctx, r) > 0;
    JS_FreeValue(ctx, r);
    return cjs;
}

// --- files ---------------------------------------------------------------------

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

// "./util" -> "/abs/dir/util.js" (tries the usual extensions like Node/Bun do).
std::string resolve_file(const fs::path& base_dir, const std::string& spec) {
    fs::path target = (base_dir / spec).lexically_normal();
    if (is_file(target)) return target.string();
    // TypeScript ESM style: `import "./util.js"` while the file is util.ts
    if (target.extension() == ".js" || target.extension() == ".mjs" || target.extension() == ".cjs") {
        fs::path ts = target;
        ts.replace_extension(target.extension() == ".js" ? ".ts" : target.extension() == ".mjs" ? ".mts" : ".cts");
        if (is_file(ts)) return ts.string();
    }
    for (const char* ext : {".js", ".mjs", ".cjs", ".ts", ".mts", ".cts", ".json"}) {
        fs::path p = target;
        p += ext;
        if (is_file(p)) return p.string();
    }
    for (const char* index : {"index.js", "index.ts", "index.mjs", "index.cjs", "index.json"}) {
        fs::path p = target / index;
        if (is_file(p)) return p.string();
    }
    return {};
}

// Turns an import specifier into a unique module name (absolute path or "node:x").
char* normalize(JSContext* ctx, const char* base, const char* spec, void*) {
    std::string s = spec;
    if (std::string b = builtin_name(ctx, s); !b.empty()) return js_strdup(ctx, b.c_str());

    bool relative = s.starts_with("./") || s.starts_with("../") || s.starts_with("/") || s == "." || s == "..";
    if (s.starts_with("file://")) {
        s.erase(0, 7);
        relative = true;
    }
    if (!relative) {
        if (s.starts_with("rtn:") || s.starts_with("node:")) {
            throw_error(ctx, "Unknown built-in module '" + s + "'");
            return nullptr;
        }
        // npm package (or "#internal" import): resolved by src/js/cjs.js.
        JSValue args[] = {JS_NewString(ctx, spec), JS_NewString(ctx, base)};
        JSValue r = call_internal(ctx, "resolveImport", 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(r)) return nullptr;
        std::string resolved = to_string(ctx, r);
        JS_FreeValue(ctx, r);
        return js_strdup(ctx, resolved.c_str());
    }

    std::string resolved = resolve_file(fs::path(base).parent_path(), s);
    if (resolved.empty()) {
        throw_error(ctx, "Cannot find module '" + s + "' imported from " + base);
        return nullptr;
    }
    return js_strdup(ctx, resolved.c_str());
}

JSModuleDef* loader(JSContext* ctx, const char* name, void*) {
    std::string n = name;
    if (n.starts_with("node:") || n.starts_with("rtn:")) return create_js_module(ctx, name);
    if (!ends_with(n, ".json") && is_commonjs(ctx, n)) return create_cjs_module(ctx, name);

    std::string code;
    if (!load_source(ctx, name, code)) return nullptr;

    JSValue fn = JS_Eval(ctx, code.c_str(), code.size(), name,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) return nullptr;
    set_import_meta(ctx, fn, name, false);

    // The engine already holds a reference to the module, so drop ours.
    auto* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(fn));
    JS_FreeValue(ctx, fn);
    return m;
}

// --- natives for src/js/cjs.js ---------------------------------------------------

std::string arg_string(JSContext* ctx, int argc, JSValueConst* argv, int i) {
    return i < argc ? to_string(ctx, argv[i]) : std::string();
}

// resolveFile(dir, "./x") -> "/abs/x.js" | null
JSValue js_resolve_file(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string r = resolve_file(arg_string(ctx, argc, argv, 0), arg_string(ctx, argc, argv, 1));
    return r.empty() ? JS_NULL : JS_NewString(ctx, r.c_str());
}

// pathKind(path) -> "file" | "dir" | null
JSValue js_path_kind(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    fs::path p = arg_string(ctx, argc, argv, 0);
    if (is_file(p)) return JS_NewString(ctx, "file");
    if (is_dir(p)) return JS_NewString(ctx, "dir");
    return JS_NULL;
}

// readText(path) -> string | null  (never throws: used for probing package.json)
JSValue js_read_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto src = read_file(arg_string(ctx, argc, argv, 0));
    return src ? JS_NewStringLen(ctx, src->data(), src->size()) : JS_NULL;
}

// loadSource(path) -> JavaScript (TypeScript types stripped); throws on errors
JSValue js_load_source(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path = arg_string(ctx, argc, argv, 0), code;
    if (ends_with(path, ".cts")) {  // load_source() only knows .ts / .mts
        auto src = read_file(path);
        if (!src) return throw_error(ctx, "Could not read file '" + path + "'");
        ts::StripResult r = ts::strip_types(*src);
        if (!r.ok) return JS_ThrowSyntaxError(ctx, "%s:%d:%d: %s", path.c_str(), r.line, r.column, r.error.c_str());
        return JS_NewStringLen(ctx, r.code.data(), r.code.size());
    }
    if (!load_source(ctx, path, code)) return JS_EXCEPTION;
    return JS_NewStringLen(ctx, code.data(), code.size());
}

// compileFunction(code, filename) -> the value of `code` as a script (sloppy mode, like Node).
JSValue js_compile_function(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string code = arg_string(ctx, argc, argv, 0), file = arg_string(ctx, argc, argv, 1);
    return JS_Eval(ctx, code.c_str(), code.size(), file.c_str(), JS_EVAL_TYPE_GLOBAL);
}

// requireESM(path) -> module namespace. Loads and evaluates the module synchronously
// (Node's require(esm)); fails for modules that use top-level await.
JSValue js_require_esm(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path = arg_string(ctx, argc, argv, 0);
    JSValue promise = JS_LoadModule(ctx, path.c_str(), path.c_str());
    if (JS_IsException(promise)) return promise;
    // The error is thrown to the caller below; this handler keeps it from also being
    // reported as an unhandled rejection.
    JSValue catch_fn = JS_GetPropertyStr(ctx, promise, "catch");
    JSValue noop = JS_NewCFunction(ctx, [](JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }, "", 1);
    JS_FreeValue(ctx, JS_Call(ctx, catch_fn, promise, 1, &noop));
    JS_FreeValue(ctx, noop);
    JS_FreeValue(ctx, catch_fn);
    // The module itself ran synchronously; settling the promise only needs a few jobs.
    JSContext* job_ctx = nullptr;
    for (int i = 0; i < 10000 && JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING; ++i) {
        if (JS_ExecutePendingJob(JS_GetRuntime(ctx), &job_ctx) <= 0) break;
    }
    JSValue result;
    switch (JS_PromiseState(ctx, promise)) {
        case JS_PROMISE_FULFILLED:
            result = JS_PromiseResult(ctx, promise);
            break;
        case JS_PROMISE_REJECTED:
            result = JS_Throw(ctx, JS_PromiseResult(ctx, promise));
            break;
        default: {
            JSValue e = JS_NewError(ctx);
            std::string msg = "require() of an ES module that uses top-level await is not supported: " + path +
                              " (use import() instead)";
            JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg.c_str()));
            JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, "ERR_REQUIRE_ASYNC_MODULE"), JS_PROP_C_W_E);
            result = JS_Throw(ctx, e);
        }
    }
    JS_FreeValue(ctx, promise);
    return result;
}

const JSCFunctionListEntry kModuleFuncs[] = {
    JS_CFUNC_DEF("resolveFile", 2, js_resolve_file),
    JS_CFUNC_DEF("pathKind", 1, js_path_kind),
    JS_CFUNC_DEF("readText", 1, js_read_text),
    JS_CFUNC_DEF("loadSource", 1, js_load_source),
    JS_CFUNC_DEF("compileFunction", 2, js_compile_function),
    JS_CFUNC_DEF("requireESM", 1, js_require_esm),
};

}  // namespace

void install_module_loader(JSRuntime* rt) {
    JS_SetModuleLoaderFunc(rt, normalize, loader, nullptr);
}

void add_module_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyFunctionList(ctx, native, kModuleFuncs, sizeof(kModuleFuncs) / sizeof(kModuleFuncs[0]));
}

bool is_commonjs_file(JSContext* ctx, const std::string& path) {
    return !ends_with(path, ".json") && is_commonjs(ctx, path);
}

bool load_source(JSContext* ctx, const std::string& path, std::string& out) {
    auto src = read_file(path);
    if (!src) {
        throw_error(ctx, "Could not read file '" + path + "'");
        return false;
    }
    if (ends_with(path, ".tsx")) {
        throw_error(ctx, "TSX/JSX is not supported yet: " + path);
        return false;
    }
    if (ends_with(path, ".ts") || ends_with(path, ".mts")) {
        ts::StripResult r = ts::strip_types(*src);
        if (!r.ok) {
            JS_ThrowSyntaxError(ctx, "%s:%d:%d: %s", path.c_str(), r.line, r.column, r.error.c_str());
            return false;
        }
        out = std::move(r.code);
        return true;
    }
    if (ends_with(path, ".json")) {
        out = "export default " + *src + "\n;";
        return true;
    }
    // A "#!/usr/bin/env rtn" line is allowed at the top of a script.
    if (src->starts_with("#!")) src->replace(0, 2, "//");
    out = std::move(*src);
    return true;
}

void set_import_meta(JSContext* ctx, JSValueConst module_fn, const std::string& path, bool is_main) {
    auto* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module_fn));
    JSValue meta = JS_GetImportMeta(ctx, m);
    std::string dir = fs::path(path).parent_path().string();
    JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, ("file://" + path).c_str()));
    JS_SetPropertyStr(ctx, meta, "filename", JS_NewString(ctx, path.c_str()));
    JS_SetPropertyStr(ctx, meta, "dirname", JS_NewString(ctx, dir.c_str()));
    JS_SetPropertyStr(ctx, meta, "main", JS_NewBool(ctx, is_main));
    JS_FreeValue(ctx, meta);
}

}  // namespace rtn
