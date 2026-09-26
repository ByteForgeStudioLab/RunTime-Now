// setTimeout / setInterval / clearTimeout / clearInterval.
// The timers themselves live in Runtime (the event loop owns them).

#include <vector>

#include "bindings/bindings.hpp"
#include "runtime.hpp"

namespace rtn {

namespace {

// magic: 0 = setTimeout, 1 = setInterval
JSValue js_set_timer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "The \"callback\" argument must be a function");
    }
    double delay = 0;
    if (argc > 1 && JS_ToFloat64(ctx, &delay, argv[1]) < 0) return JS_EXCEPTION;
    if (!(delay >= 0)) delay = 0;  // also catches NaN

    std::vector<JSValue> args;
    for (int i = 2; i < argc; ++i) args.push_back(JS_DupValue(ctx, argv[i]));

    int64_t id = Runtime::from(ctx)->add_timer(JS_DupValue(ctx, argv[0]), std::move(args),
                                               static_cast<int64_t>(delay), magic == 1);
    return JS_NewInt64(ctx, id);
}

JSValue js_clear_timer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int64_t id = 0;
    if (argc > 0 && JS_IsNumber(argv[0]) && JS_ToInt64(ctx, &id, argv[0]) == 0) {
        Runtime::from(ctx)->clear_timer(id);
    }
    return JS_UNDEFINED;
}

const JSCFunctionListEntry kTimerFuncs[] = {
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, js_set_timer, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, js_set_timer, 1),
    JS_CFUNC_DEF("clearTimeout", 1, js_clear_timer),
    JS_CFUNC_DEF("clearInterval", 1, js_clear_timer),
};

}  // namespace

void install_timers(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyFunctionList(ctx, global, kTimerFuncs, sizeof(kTimerFuncs) / sizeof(kTimerFuncs[0]));
    JS_FreeValue(ctx, global);
}

}  // namespace rtn
