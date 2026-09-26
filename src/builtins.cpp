// Runs the JavaScript parts of the runtime (src/js/*.js, embedded at build time).
//
// Each file is `(function (native, internal) { ... })`:
//   native   — C++ functions (sockets, UTF-8...), never exposed to user code
//   internal — lets later files use helpers defined by earlier ones

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bindings/bindings.hpp"
#include "embedded_js.hpp"
#include "util.hpp"

namespace rtn {

void install_builtins(JSContext* ctx) {
    JSValue native = JS_NewObject(ctx);
    add_encoding_natives(ctx, native);
    add_http_natives(ctx, native);
    JS_SetPropertyStr(ctx, native, "version", JS_NewString(ctx, RTN_VERSION));
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
    JS_FreeValue(ctx, internal);
    JS_FreeValue(ctx, native);
}

}  // namespace rtn
