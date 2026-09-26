#include "modules.hpp"

#include <cstring>
#include <filesystem>

#include "bindings/bindings.hpp"
#include "typescript/strip.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace rtn {

namespace {

using NativeModuleFactory = JSModuleDef* (*)(JSContext*, const char*);

struct NativeModule {
    const char* name;
    NativeModuleFactory create;
};

// Built-in modules: import { readFileSync } from "rtn:fs"
constexpr NativeModule kNativeModules[] = {
    {"rtn:fs", create_fs_module},
    {"node:fs", create_fs_module},
};

const NativeModule* find_native(const char* name) {
    for (const auto& m : kNativeModules) {
        if (std::strcmp(m.name, name) == 0) return &m;
    }
    return nullptr;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// "./util" -> "/abs/dir/util.js" (tries the usual extensions like Node/Bun do).
std::string resolve_file(const fs::path& base_dir, const std::string& spec) {
    fs::path target = (base_dir / spec).lexically_normal();
    if (is_file(target)) return target.string();
    // TypeScript ESM style: `import "./util.js"` while the file is util.ts
    if (target.extension() == ".js" || target.extension() == ".mjs") {
        fs::path ts = target;
        ts.replace_extension(target.extension() == ".js" ? ".ts" : ".mts");
        if (is_file(ts)) return ts.string();
    }
    for (const char* ext : {".js", ".mjs", ".ts", ".mts", ".json"}) {
        fs::path p = target;
        p += ext;
        if (is_file(p)) return p.string();
    }
    for (const char* index : {"index.js", "index.ts"}) {
        fs::path p = target / index;
        if (is_file(p)) return p.string();
    }
    return {};
}

// Turns an import specifier into a unique module name (absolute path or "rtn:x").
char* normalize(JSContext* ctx, const char* base, const char* spec, void*) {
    if (find_native(spec)) return js_strdup(ctx, spec);

    std::string s = spec;
    bool relative = s.starts_with("./") || s.starts_with("../") || s.starts_with("/");
    if (!relative) {
        if (s.starts_with("rtn:") || s.starts_with("node:")) {
            throw_error(ctx, "Unknown built-in module '" + s + "'");
        } else {
            throw_error(ctx, "Cannot resolve '" + s +
                                 "': npm packages (bare specifiers) are not supported yet");
        }
        return nullptr;
    }

    std::string resolved = resolve_file(fs::path(base).parent_path(), s);
    if (resolved.empty()) {
        throw_error(ctx, "Cannot find module '" + s + "' imported from " + base);
        return nullptr;
    }
    return js_strdup(ctx, resolved.c_str());
}

JSModuleDef* loader(JSContext* ctx, const char* name, void*) {
    if (const NativeModule* native = find_native(name)) return native->create(ctx, name);

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

}  // namespace

void install_module_loader(JSRuntime* rt) {
    JS_SetModuleLoaderFunc(rt, normalize, loader, nullptr);
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
