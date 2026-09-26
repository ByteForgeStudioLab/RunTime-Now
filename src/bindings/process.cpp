// The global `process` object: argv, env, exit, cwd, platform, pid, versions...

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "util.hpp"

extern char** environ;

namespace rtn {

namespace {

JSValue js_process_exit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int32_t code = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0])) JS_ToInt32(ctx, &code, argv[0]);
    std::fflush(stdout);
    std::fflush(stderr);
    std::exit(code);
}

JSValue js_process_cwd(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return throw_error(ctx, "cwd: " + ec.message());
    return JS_NewString(ctx, cwd.c_str());
}

JSValue js_process_chdir(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "chdir: path required");
    std::string dir = to_string(ctx, argv[0]);
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
    if (ec) return throw_error(ctx, "chdir '" + dir + "': " + ec.message());
    return JS_UNDEFINED;
}

JSValue js_process_uptime(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    static const auto start = Clock::now();
    return JS_NewFloat64(ctx, std::chrono::duration<double>(Clock::now() - start).count());
}

// process.stdout.write / process.stderr.write (magic: 1 = stdout, 2 = stderr)
JSValue js_stream_write(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    FILE* out = magic == 2 ? stderr : stdout;
    if (argc < 1) return JS_TRUE;
    size_t size = 0;
    if (!JS_IsString(argv[0]) && JS_GetTypedArrayType(argv[0]) >= 0) {
        if (uint8_t* bytes = JS_GetUint8Array(ctx, &size, argv[0])) {
            std::fwrite(bytes, 1, size, out);
            std::fflush(out);
            return JS_TRUE;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    const char* s = JS_ToCStringLen(ctx, &size, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::fwrite(s, 1, size, out);
    std::fflush(out);
    JS_FreeCString(ctx, s);
    return JS_TRUE;
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// process.hrtime([previous]) -> [seconds, nanoseconds]
JSValue js_process_hrtime(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int64_t t = now_ns();
    if (argc > 0 && JS_IsArray(argv[0])) {
        JSValue s = JS_GetPropertyUint32(ctx, argv[0], 0), n = JS_GetPropertyUint32(ctx, argv[0], 1);
        int64_t ps = 0, pn = 0;
        JS_ToInt64(ctx, &ps, s);
        JS_ToInt64(ctx, &pn, n);
        JS_FreeValue(ctx, s);
        JS_FreeValue(ctx, n);
        t -= ps * 1000000000LL + pn;
    }
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewInt64(ctx, t / 1000000000LL));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewInt64(ctx, t % 1000000000LL));
    return r;
}

JSValue js_process_hrtime_bigint(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBigInt64(ctx, now_ns());
}

JSValue js_process_memory_usage(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSMemoryUsage mu{};
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
    long pages = 0, resident = 0;
    std::ifstream statm("/proc/self/statm");
    statm >> pages >> resident;
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "rss", JS_NewInt64(ctx, resident * sysconf(_SC_PAGESIZE)));
    JS_SetPropertyStr(ctx, r, "heapTotal", JS_NewInt64(ctx, mu.malloc_size));
    JS_SetPropertyStr(ctx, r, "heapUsed", JS_NewInt64(ctx, mu.memory_used_size));
    JS_SetPropertyStr(ctx, r, "external", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, r, "arrayBuffers", JS_NewInt32(ctx, 0));
    return r;
}

// process.nextTick(fn, ...args): runs fn before any promise callbacks.
JSValue js_process_next_tick(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "The \"callback\" argument must be of type function");
    }
    std::vector<JSValue> args;
    for (int i = 1; i < argc; ++i) args.push_back(JS_DupValue(ctx, argv[i]));
    Runtime::from(ctx)->next_tick(JS_DupValue(ctx, argv[0]), std::move(args));
    return JS_UNDEFINED;
}

#if defined(__x86_64__)
#define RTN_ARCH "x64"
#elif defined(__aarch64__)
#define RTN_ARCH "arm64"
#else
#define RTN_ARCH "unknown"
#endif

const JSCFunctionListEntry kProcessFuncs[] = {
    JS_CFUNC_DEF("exit", 1, js_process_exit),
    JS_CFUNC_DEF("cwd", 0, js_process_cwd),
    JS_CFUNC_DEF("chdir", 1, js_process_chdir),
    JS_CFUNC_DEF("uptime", 0, js_process_uptime),
    JS_CFUNC_DEF("memoryUsage", 0, js_process_memory_usage),
    JS_CFUNC_DEF("nextTick", 1, js_process_next_tick),
    JS_PROP_STRING_DEF("platform", "linux", JS_PROP_CONFIGURABLE),
    JS_PROP_STRING_DEF("arch", RTN_ARCH, JS_PROP_CONFIGURABLE),
    JS_PROP_STRING_DEF("version", "v" RTN_VERSION, JS_PROP_CONFIGURABLE),
    JS_PROP_STRING_DEF("title", "rtn", JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE),
};

JSValue make_stream(JSContext* ctx, int fd) {
    JSValue stream = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stream, "write", JS_NewCFunctionMagic(ctx, js_stream_write, "write", 1, JS_CFUNC_generic_magic, fd));
    JS_SetPropertyStr(ctx, stream, "isTTY", JS_NewBool(ctx, isatty(fd)));
    JS_SetPropertyStr(ctx, stream, "fd", JS_NewInt32(ctx, fd));
    return stream;
}

}  // namespace

void install_process(JSContext* ctx) {
    Runtime* runtime = Runtime::from(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue process = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, process, kProcessFuncs,
                               sizeof(kProcessFuncs) / sizeof(kProcessFuncs[0]));

    // process.argv = ["/path/to/rtn", "script.js", ...args]
    JSValue argv = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& a : runtime->argv()) {
        JS_SetPropertyUint32(ctx, argv, i++, JS_NewString(ctx, a.c_str()));
    }
    JS_SetPropertyStr(ctx, process, "argv", argv);

    JSValue env = JS_NewObject(ctx);
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        JS_SetPropertyStr(ctx, env, kv.substr(0, eq).c_str(), JS_NewString(ctx, kv.c_str() + eq + 1));
    }
    JS_SetPropertyStr(ctx, process, "env", env);

    JSValue versions = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, versions, "rtn", JS_NewString(ctx, RTN_VERSION));
    JS_SetPropertyStr(ctx, versions, "quickjs", JS_NewString(ctx, JS_GetVersion()));
    JS_SetPropertyStr(ctx, process, "versions", versions);

    JS_SetPropertyStr(ctx, process, "pid", JS_NewInt32(ctx, getpid()));
    JS_SetPropertyStr(ctx, process, "ppid", JS_NewInt32(ctx, getppid()));
    JS_SetPropertyStr(ctx, process, "execPath",
                      JS_NewString(ctx, runtime->argv().empty() ? "" : runtime->argv()[0].c_str()));
    JS_SetPropertyStr(ctx, process, "stdout", make_stream(ctx, 1));
    JS_SetPropertyStr(ctx, process, "stderr", make_stream(ctx, 2));
    JSValue hrtime = JS_NewCFunction(ctx, js_process_hrtime, "hrtime", 1);
    JS_SetPropertyStr(ctx, hrtime, "bigint", JS_NewCFunction(ctx, js_process_hrtime_bigint, "bigint", 0));
    JS_SetPropertyStr(ctx, process, "hrtime", hrtime);
    JS_SetPropertyStr(ctx, process, "exitCode", JS_UNDEFINED);

    JS_SetPropertyStr(ctx, global, "process", process);
    JS_FreeValue(ctx, global);
}

}  // namespace rtn
