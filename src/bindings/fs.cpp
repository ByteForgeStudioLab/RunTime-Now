// import { readFileSync, writeFileSync, ... } from "rtn:fs"   (also "node:fs")
// Synchronous file system API, modelled on Node's `fs`. Errors carry
// Node-style `code` / `errno` / `syscall` / `path` properties.

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "bindings/bindings.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace rtn {

namespace {

bool get_path(JSContext* ctx, int argc, JSValueConst* argv, std::string& out, int idx = 0) {
    if (argc <= idx || !JS_IsString(argv[idx])) {
        JS_ThrowTypeError(ctx, "The \"path\" argument must be of type string");
        return false;
    }
    out = to_string(ctx, argv[idx]);
    return true;
}

// Reads `{ recursive: true }` style boolean options.
bool get_bool_option(JSContext* ctx, int argc, JSValueConst* argv, int idx, const char* name) {
    if (argc <= idx || !JS_IsObject(argv[idx])) return false;
    JSValue v = JS_GetPropertyStr(ctx, argv[idx], name);
    bool b = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return b;
}

// "utf8" / { encoding: "utf8" } -> text; anything else -> bytes
bool wants_text(JSContext* ctx, int argc, JSValueConst* argv, int idx) {
    if (argc <= idx) return false;
    if (JS_IsString(argv[idx])) return true;
    if (!JS_IsObject(argv[idx])) return false;
    JSValue enc = JS_GetPropertyStr(ctx, argv[idx], "encoding");
    bool text = JS_IsString(enc);
    JS_FreeValue(ctx, enc);
    return text;
}

// readFileSync(path)          -> Uint8Array
// readFileSync(path, "utf8")  -> string
JSValue js_read_file(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;

    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return throw_errno(ctx, errno, "open", path);
    struct stat st{};
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        return throw_errno(ctx, EISDIR, "read");
    }
    std::string data;
    if (st.st_size > 0) data.reserve(static_cast<size_t>(st.st_size));
    char buf[65536];
    while (true) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            data.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            int err = errno;
            close(fd);
            return throw_errno(ctx, err, "read");
        }
    }
    close(fd);
    if (wants_text(ctx, argc, argv, 1)) return JS_NewStringLen(ctx, data.data(), data.size());
    return JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// magic: 0 = writeFileSync, 1 = appendFileSync
JSValue js_write_file(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "The \"data\" argument is required");

    std::string text;
    const char* bytes = nullptr;
    size_t len = 0;
    if (!JS_IsString(argv[1]) && JS_GetTypedArrayType(argv[1]) >= 0) {
        bytes = reinterpret_cast<const char*>(JS_GetUint8Array(ctx, &len, argv[1]));
        if (!bytes) JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (!bytes) {
        text = to_string(ctx, argv[1]);
        bytes = text.data();
        len = text.size();
    }

    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (magic ? O_APPEND : O_TRUNC);
    int fd = open(path.c_str(), flags, 0666);
    if (fd < 0) return throw_errno(ctx, errno, "open", path);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int err = errno;
            close(fd);
            return throw_errno(ctx, err, "write");
        }
        off += static_cast<size_t>(n);
    }
    close(fd);
    return JS_UNDEFINED;
}

JSValue js_exists(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) return JS_FALSE;
    struct stat st{};
    return JS_NewBool(ctx, stat(to_string(ctx, argv[0]).c_str(), &st) == 0);
}

JSValue js_readdir(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    std::error_code ec;
    fs::directory_iterator it(path, ec);
    if (ec) return throw_errno(ctx, ec.value(), "scandir", path);

    std::vector<std::string> names;
    for (const auto& entry : it) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());

    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < names.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, names[i].c_str()));
    }
    return arr;
}

// mkdirSync(path, { recursive: true }) -> first directory created (like Node), or undefined
JSValue js_mkdir(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    if (get_bool_option(ctx, argc, argv, 1, "recursive")) {
        std::error_code ec;
        fs::path target = fs::absolute(path, ec).lexically_normal();
        if (!target.empty() && !target.has_filename()) target = target.parent_path();  // trailing '/'
        fs::path first;
        for (fs::path p = target; !p.empty() && !fs::exists(p, ec); p = p.parent_path()) {
            first = p;
            if (p == p.parent_path()) break;
        }
        fs::create_directories(target, ec);
        if (ec) return throw_errno(ctx, ec.value(), "mkdir", path);
        return first.empty() ? JS_UNDEFINED : JS_NewString(ctx, first.c_str());
    } else if (mkdir(path.c_str(), 0777) != 0) {
        return throw_errno(ctx, errno, "mkdir", path);
    }
    return JS_UNDEFINED;
}

// rmSync(path, { recursive: true, force: true })
JSValue js_rm(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    bool recursive = get_bool_option(ctx, argc, argv, 1, "recursive");
    bool force = get_bool_option(ctx, argc, argv, 1, "force");

    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (force && errno == ENOENT) return JS_UNDEFINED;
        return throw_errno(ctx, errno, "lstat", path);
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {  // same error as Node
            JSValue e = JS_NewError(ctx);
            std::string msg = "Path is a directory: rm returned EISDIR (is a directory) " + path;
            JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg.c_str()));
            JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, "ERR_FS_EISDIR"), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, "rm"), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, e, "path", JS_NewString(ctx, path.c_str()), JS_PROP_C_W_E);
            return JS_Throw(ctx, e);
        }
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec) return throw_errno(ctx, ec.value(), "rm", path);
    } else if (unlink(path.c_str()) != 0) {
        return throw_errno(ctx, errno, "rm", path);
    }
    return JS_UNDEFINED;
}

JSValue js_rename(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string from, to;
    if (!get_path(ctx, argc, argv, from, 0) || !get_path(ctx, argc, argv, to, 1)) return JS_EXCEPTION;
    if (rename(from.c_str(), to.c_str()) != 0) return throw_errno(ctx, errno, "rename", from);
    return JS_UNDEFINED;
}

JSValue js_copy_file(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string from, to;
    if (!get_path(ctx, argc, argv, from, 0) || !get_path(ctx, argc, argv, to, 1)) return JS_EXCEPTION;
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return throw_errno(ctx, ec.value(), "copyfile", from);
    return JS_UNDEFINED;
}

// Stats methods: isFile() / isDirectory() / isSymbolicLink()
constexpr mode_t kStatTypes[] = {S_IFREG, S_IFDIR, S_IFLNK};
constexpr const char* kStatMethods[] = {"isFile", "isDirectory", "isSymbolicLink"};

JSValue js_stats_is(JSContext* ctx, JSValueConst this_val, int, JSValueConst*, int magic) {
    JSValue mode = JS_GetPropertyStr(ctx, this_val, "mode");
    uint32_t m = 0;
    JS_ToUint32(ctx, &m, mode);
    JS_FreeValue(ctx, mode);
    return JS_NewBool(ctx, (m & S_IFMT) == kStatTypes[magic]);
}

double ms(const timespec& t) { return static_cast<double>(t.tv_sec) * 1000.0 + t.tv_nsec / 1e6; }

// statSync / lstatSync (magic: 1 = lstat)
JSValue js_stat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    struct stat st{};
    int rc = magic ? lstat(path.c_str(), &st) : stat(path.c_str(), &st);
    if (rc != 0) return throw_errno(ctx, errno, magic ? "lstat" : "stat", path);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, st.st_size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewUint32(ctx, st.st_mode));
    JS_SetPropertyStr(ctx, obj, "uid", JS_NewUint32(ctx, st.st_uid));
    JS_SetPropertyStr(ctx, obj, "gid", JS_NewUint32(ctx, st.st_gid));
    JS_SetPropertyStr(ctx, obj, "atimeMs", JS_NewFloat64(ctx, ms(st.st_atim)));
    JS_SetPropertyStr(ctx, obj, "mtimeMs", JS_NewFloat64(ctx, ms(st.st_mtim)));
    JS_SetPropertyStr(ctx, obj, "ctimeMs", JS_NewFloat64(ctx, ms(st.st_ctim)));
    JS_SetPropertyStr(ctx, obj, "atime", JS_NewDate(ctx, ms(st.st_atim)));
    JS_SetPropertyStr(ctx, obj, "mtime", JS_NewDate(ctx, ms(st.st_mtim)));
    JS_SetPropertyStr(ctx, obj, "ctime", JS_NewDate(ctx, ms(st.st_ctim)));
    // methods are non-enumerable, like on Node's Stats prototype
    for (int i = 0; i < 3; ++i) {
        JSValue fn = JS_NewCFunctionMagic(ctx, js_stats_is, kStatMethods[i], 0, JS_CFUNC_generic_magic, i);
        JS_DefinePropertyValueStr(ctx, obj, kStatMethods[i], fn, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    }
    return obj;
}

const JSCFunctionListEntry kFsFuncs[] = {
    JS_CFUNC_DEF("readFileSync", 2, js_read_file),
    JS_CFUNC_MAGIC_DEF("writeFileSync", 2, js_write_file, 0),
    JS_CFUNC_MAGIC_DEF("appendFileSync", 2, js_write_file, 1),
    JS_CFUNC_DEF("existsSync", 1, js_exists),
    JS_CFUNC_DEF("readdirSync", 1, js_readdir),
    JS_CFUNC_DEF("mkdirSync", 2, js_mkdir),
    JS_CFUNC_DEF("rmSync", 2, js_rm),
    JS_CFUNC_DEF("renameSync", 2, js_rename),
    JS_CFUNC_DEF("copyFileSync", 2, js_copy_file),
    JS_CFUNC_MAGIC_DEF("statSync", 1, js_stat, 0),
    JS_CFUNC_MAGIC_DEF("lstatSync", 1, js_stat, 1),
};
constexpr int kFsFuncCount = sizeof(kFsFuncs) / sizeof(kFsFuncs[0]);

int fs_module_init(JSContext* ctx, JSModuleDef* m) {
    if (JS_SetModuleExportList(ctx, m, kFsFuncs, kFsFuncCount) < 0) return -1;
    // Also `import fs from "rtn:fs"` (default export with everything on it).
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, def, kFsFuncs, kFsFuncCount);
    return JS_SetModuleExport(ctx, m, "default", def);
}

}  // namespace

JSModuleDef* create_fs_module(JSContext* ctx, const char* name) {
    JSModuleDef* m = JS_NewCModule(ctx, name, fs_module_init);
    if (!m) return nullptr;
    JS_AddModuleExportList(ctx, m, kFsFuncs, kFsFuncCount);
    JS_AddModuleExport(ctx, m, "default");
    return m;
}

}  // namespace rtn
