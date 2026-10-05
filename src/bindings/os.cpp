// System information for node:os (src/js/node.js): osInfo() -> { hostname, release, ... }

#include <cstdlib>
#include <fstream>
#include <pwd.h>
#include <string>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "bindings/bindings.hpp"

namespace rtn {

namespace {

void set_str(JSContext* ctx, JSValueConst obj, const char* key, const std::string& v) {
    JS_SetPropertyStr(ctx, obj, key, JS_NewStringLen(ctx, v.data(), v.size()));
}

JSValue js_os_info(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue o = JS_NewObject(ctx);

    char host[256] = "";
    gethostname(host, sizeof host - 1);
    set_str(ctx, o, "hostname", host);

    utsname u{};
    if (uname(&u) == 0) {
        set_str(ctx, o, "type", u.sysname);
        set_str(ctx, o, "release", u.release);
        set_str(ctx, o, "version", u.version);
        set_str(ctx, o, "machine", u.machine);
    }

    struct sysinfo si{};
    if (sysinfo(&si) == 0) {
        double unit = si.mem_unit ? si.mem_unit : 1;
        JS_SetPropertyStr(ctx, o, "totalmem", JS_NewFloat64(ctx, static_cast<double>(si.totalram) * unit));
        JS_SetPropertyStr(ctx, o, "freemem", JS_NewFloat64(ctx, static_cast<double>(si.freeram) * unit));
        JS_SetPropertyStr(ctx, o, "uptime", JS_NewFloat64(ctx, static_cast<double>(si.uptime)));
        JSValue load = JS_NewArray(ctx);
        for (uint32_t i = 0; i < 3; ++i) {
            JS_SetPropertyUint32(ctx, load, i, JS_NewFloat64(ctx, static_cast<double>(si.loads[i]) / (1 << SI_LOAD_SHIFT)));
        }
        JS_SetPropertyStr(ctx, o, "loadavg", load);
    }
    // MemAvailable is what Node reports as free memory on Linux.
    std::ifstream meminfo("/proc/meminfo");
    for (std::string line; std::getline(meminfo, line);) {
        if (line.starts_with("MemAvailable:")) {
            JS_SetPropertyStr(ctx, o, "freemem", JS_NewFloat64(ctx, std::atof(line.c_str() + 13) * 1024));
            break;
        }
    }

    uid_t uid = getuid();
    JS_SetPropertyStr(ctx, o, "uid", JS_NewInt64(ctx, uid));
    JS_SetPropertyStr(ctx, o, "gid", JS_NewInt64(ctx, getgid()));
    if (passwd* pw = getpwuid(uid)) {
        set_str(ctx, o, "username", pw->pw_name ? pw->pw_name : "");
        set_str(ctx, o, "pwHome", pw->pw_dir ? pw->pw_dir : "");
        set_str(ctx, o, "shell", pw->pw_shell ? pw->pw_shell : "");
    }

    // CPU models from /proc/cpuinfo ("model name" on x86, "Model"/"CPU part" elsewhere).
    JSValue cpus = JS_NewArray(ctx);
    std::ifstream cpuinfo("/proc/cpuinfo");
    uint32_t n = 0;
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.starts_with("model name") || line.starts_with("Model")) {
            size_t colon = line.find(':');
            std::string model = colon == std::string::npos ? "" : line.substr(colon + 1);
            while (!model.empty() && model.front() == ' ') model.erase(0, 1);
            JS_SetPropertyUint32(ctx, cpus, n++, JS_NewStringLen(ctx, model.data(), model.size()));
        }
    }
    JS_SetPropertyStr(ctx, o, "cpuModels", cpus);
    return o;
}

// isatty(fd) -> boolean (node:tty)
JSValue js_isatty(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int32_t fd = -1;
    if (argc > 0) JS_ToInt32(ctx, &fd, argv[0]);
    return JS_NewBool(ctx, fd >= 0 && isatty(fd));
}

}  // namespace

void add_os_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyStr(ctx, native, "osInfo", JS_NewCFunction(ctx, js_os_info, "osInfo", 0));
    JS_SetPropertyStr(ctx, native, "isatty", JS_NewCFunction(ctx, js_isatty, "isatty", 1));
}

}  // namespace rtn
