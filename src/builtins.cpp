// Runs the JavaScript parts of the runtime (src/js/*.js, embedded at build time).
//
// Each file is `(function (native, internal) { ... })`:
//   native   — C++ functions (sockets, UTF-8...), never exposed to user code
//   internal — lets later files use helpers defined by earlier ones

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "bindings/bindings.hpp"
#include "embedded_js.hpp"
#include "runtime.hpp"
#include "util.hpp"

namespace rtn {

namespace {
// internal.modules: { "path": {...}, "fs/promises": {...} }, set by src/js/modules.js.
JSValue g_modules = JS_UNDEFINED;
JSValue g_internal = JS_UNDEFINED;

// cpuCount() -> online CPUs (navigator.hardwareConcurrency)
JSValue js_cpu_count(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return JS_NewInt32(ctx, n > 0 ? static_cast<int32_t>(n) : 1);
}
}  // namespace

JSValue builtin_module_exports(JSContext* ctx, const std::string& name) {
    if (!JS_IsObject(g_modules)) return JS_UNDEFINED;
    // Own properties only: "constructor" or "toString" are not modules.
    JSAtom atom = JS_NewAtomLen(ctx, name.data(), name.size());
    JSPropertyDescriptor desc;
    int found = JS_GetOwnProperty(ctx, &desc, g_modules, atom);
    JS_FreeAtom(ctx, atom);
    if (found <= 0) return JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return desc.value;
}

JSValue call_internal(JSContext* ctx, const char* fn, int argc, JSValueConst* argv) {
    JSValue f = JS_GetPropertyStr(ctx, g_internal, fn);
    JSValue r = JS_Call(ctx, f, JS_UNDEFINED, argc, argv);
    JS_FreeValue(ctx, f);
    return r;
}

void install_builtins(JSContext* ctx) {
    JSValue native = JS_NewObject(ctx);
    add_encoding_natives(ctx, native);
    add_console_natives(ctx, native);
    add_timer_natives(ctx, native);
    add_http_natives(ctx, native);
    add_fetch_natives(ctx, native);
    add_crypto_natives(ctx, native);
    add_fs_natives(ctx, native);
    add_module_natives(ctx, native);
    add_os_natives(ctx, native);
    JS_SetPropertyStr(ctx, native, "version", JS_NewString(ctx, RTN_VERSION));
    JS_SetPropertyStr(ctx, native, "cpuCount", JS_NewCFunction(ctx, js_cpu_count, "cpuCount", 0));
    JSValue internal = JS_NewObject(ctx);

    for (size_t i = 0; i < kEmbeddedJsCount; ++i) {
        const EmbeddedJs& file = kEmbeddedJs[i];
        std::string name = std::string("rtn:internal/") + file.name;
        JSValue fn = JS_Eval(ctx, file.source, std::strlen(file.source), name.c_str(),
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
        JSValue ret = JS_EXCEPTION;
        if (!JS_IsException(fn)) {
            JSValue args[] = {native, internal};
            ret = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        }
        if (JS_IsException(ret)) {  // a bug in our own JS: nothing sensible to do
            dump_pending_exception(ctx);
            std::fprintf(stderr, "fatal: failed to initialize %s\n", name.c_str());
            std::exit(70);
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, fn);
    }
    g_modules = JS_GetPropertyStr(ctx, internal, "modules");
    g_internal = JS_DupValue(ctx, internal);
    Runtime::from(ctx)->on_shutdown([ctx] {
        JS_FreeValue(ctx, g_modules);
        JS_FreeValue(ctx, g_internal);
        g_modules = JS_UNDEFINED;
        g_internal = JS_UNDEFINED;
    });
    JS_FreeValue(ctx, internal);
    JS_FreeValue(ctx, native);
}

}  // namespace rtn
