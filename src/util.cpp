#include "util.hpp"

#include "bindings/bindings.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace rtn {

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string to_string(JSContext* ctx, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return "<unprintable>";
    }
    std::string out(s, len);
    JS_FreeCString(ctx, s);
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void print_exception(JSContext* ctx, JSValueConst exc, const char* prefix) {
    bool color = isatty(STDERR_FILENO);
    const char* red = color ? "\x1b[31m" : "";
    const char* reset = color ? "\x1b[0m" : "";
    // Errors print as "Name: message + stack + extra fields"; other thrown values are inspected.
    std::string text = inspect(ctx, exc, false);
    std::fprintf(stderr, "%s%s %s%s\n", red, prefix, text.c_str(), reset);
    std::fflush(stderr);
}

void dump_pending_exception(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    print_exception(ctx, exc);
    JS_FreeValue(ctx, exc);
}

JSValue throw_error(JSContext* ctx, const std::string& msg) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewStringLen(ctx, msg.data(), msg.size()));
    return JS_Throw(ctx, err);
}

const char* errno_name(int err) {
    switch (err) {
        case ENOENT: return "ENOENT";
        case EACCES: return "EACCES";
        case EPERM: return "EPERM";
        case EEXIST: return "EEXIST";
        case EISDIR: return "EISDIR";
        case ENOTDIR: return "ENOTDIR";
        case ENOTEMPTY: return "ENOTEMPTY";
        case EBUSY: return "EBUSY";
        case EMFILE: return "EMFILE";
        case ENFILE: return "ENFILE";
        case ENOSPC: return "ENOSPC";
        case EROFS: return "EROFS";
        case EINVAL: return "EINVAL";
        case ELOOP: return "ELOOP";
        case ENAMETOOLONG: return "ENAMETOOLONG";
        case EXDEV: return "EXDEV";
        case EAGAIN: return "EAGAIN";
        case EADDRINUSE: return "EADDRINUSE";
        case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
        case ECONNREFUSED: return "ECONNREFUSED";
        case ECONNRESET: return "ECONNRESET";
        case EPIPE: return "EPIPE";
        case ETIMEDOUT: return "ETIMEDOUT";
        default: return "EUNKNOWN";
    }
}

JSValue throw_errno(JSContext* ctx, int err, const char* syscall, const std::string& path) {
    // Same wording as Node (libuv), e.g. "no such file or directory".
    std::string desc;
    switch (err) {
        case EEXIST: desc = "file already exists"; break;
        case EADDRINUSE: desc = "address already in use"; break;
        case ENOTEMPTY: desc = "directory not empty"; break;
        case EISDIR: desc = "illegal operation on a directory"; break;
        default:
            desc = std::strerror(err);
            if (!desc.empty()) desc[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(desc[0])));
    }
    std::string msg = std::string(errno_name(err)) + ": " + desc + ", " + syscall;
    if (!path.empty()) msg += " '" + path + "'";

    JSValue e = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, e, "message", JS_NewStringLen(ctx, msg.data(), msg.size()));
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, errno_name(err)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "errno", JS_NewInt32(ctx, -err), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, syscall), JS_PROP_C_W_E);
    if (!path.empty()) JS_DefinePropertyValueStr(ctx, e, "path", JS_NewString(ctx, path.c_str()), JS_PROP_C_W_E);
    return JS_Throw(ctx, e);
}

}  // namespace rtn
