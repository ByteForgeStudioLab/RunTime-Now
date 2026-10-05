// import { readFileSync, writeFileSync, ... } from "node:fs"   (also "fs", "rtn:fs")
// File system API modelled on Node's `fs`. Errors carry Node-style
// `code` / `errno` / `syscall` / `path` properties.
//
// The operations themselves are plain C++ functions (no JS engine), so the
// same code serves the synchronous API here and fs/promises, which runs them
// on the runtime's thread pool through the native `fsAsync()` (src/js/modules.js).

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace rtn {

namespace {

// ---------------------------------------------------------------------------
// Operations (thread-safe, no JS)
// ---------------------------------------------------------------------------

struct Result {
    int err = 0;                 // errno, 0 = success
    const char* syscall = "";
    bool path_in_message = true;
    bool rm_directory = false;   // rm() of a directory without { recursive }
};

Result fail(int err, const char* syscall, bool with_path = true) { return {err, syscall, with_path}; }

Result read_all(const std::string& path, std::string& data) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail(errno, "open");
    struct stat st{};
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        return fail(EISDIR, "read", false);  // Node's message has no path here
    }
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
            return fail(err, "read", false);
        }
    }
    close(fd);
    return {};
}

Result write_all(const std::string& path, const char* bytes, size_t len, bool append) {
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    int fd = open(path.c_str(), flags, 0666);
    if (fd < 0) return fail(errno, "open");
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int err = errno;
            close(fd);
            return fail(err, "write", false);
        }
        off += static_cast<size_t>(n);
    }
    close(fd);
    return {};
}

Result list_dir(const std::string& path, std::vector<std::string>& names) {
    std::error_code ec;
    fs::directory_iterator it(path, ec);
    if (ec) return fail(ec.value(), "scandir");
    for (const auto& entry : it) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return {};
}

// With `recursive`, `first` gets the first directory created (like Node).
Result make_dir(const std::string& path, bool recursive, std::string& first) {
    if (!recursive) return mkdir(path.c_str(), 0777) == 0 ? Result{} : fail(errno, "mkdir");
    std::error_code ec;
    fs::path target = fs::absolute(path, ec).lexically_normal();
    if (!target.empty() && !target.has_filename()) target = target.parent_path();  // trailing '/'
    fs::path created;
    for (fs::path p = target; !p.empty() && !fs::exists(p, ec); p = p.parent_path()) {
        created = p;
        if (p == p.parent_path()) break;
    }
    fs::create_directories(target, ec);
    if (ec) return fail(ec.value(), "mkdir");
    first = created.string();
    return {};
}

Result remove_path(const std::string& path, bool recursive, bool force) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (force && errno == ENOENT) return {};
        return fail(errno, "lstat");
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            Result r = fail(EISDIR, "rm");
            r.rm_directory = true;
            return r;
        }
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec) return fail(ec.value(), "rm");
        return {};
    }
    return unlink(path.c_str()) == 0 ? Result{} : fail(errno, "rm");
}

Result copy(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return ec ? fail(ec.value(), "copyfile") : Result{};
}

// ---------------------------------------------------------------------------
// JS helpers
// ---------------------------------------------------------------------------

JSValue to_error(JSContext* ctx, const Result& r, const std::string& path) {
    if (r.rm_directory) {  // same error as Node
        JSValue e = JS_NewError(ctx);
        std::string msg = "Path is a directory: rm returned EISDIR (is a directory) " + path;
        JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg.c_str()));
        JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, "ERR_FS_EISDIR"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, "rm"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "path", JS_NewString(ctx, path.c_str()), JS_PROP_C_W_E);
        return e;
    }
    return errno_error(ctx, r.err, r.syscall, r.path_in_message ? path : "");
}

JSValue throw_result(JSContext* ctx, const Result& r, const std::string& path) {
    return JS_Throw(ctx, to_error(ctx, r, path));
}

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

// A string or the bytes of a typed array.
bool get_data(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v) && JS_GetTypedArrayType(v) >= 0) {
        size_t len = 0;
        if (uint8_t* bytes = JS_GetUint8Array(ctx, &len, v)) {
            out.assign(reinterpret_cast<const char*>(bytes), len);
            return true;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    out = to_string(ctx, v);
    return true;
}

JSValue string_array(JSContext* ctx, const std::vector<std::string>& names) {
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < names.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewStringLen(ctx, names[i].data(), names[i].size()));
    }
    return arr;
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

JSValue stats_object(JSContext* ctx, const struct stat& st) {
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

// ---------------------------------------------------------------------------
// Synchronous API
// ---------------------------------------------------------------------------

// readFileSync(path)          -> Uint8Array
// readFileSync(path, "utf8")  -> string
JSValue js_read_file(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path, data;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    if (Result r = read_all(path, data); r.err) return throw_result(ctx, r, path);
    if (wants_text(ctx, argc, argv, 1)) return JS_NewStringLen(ctx, data.data(), data.size());
    return JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// magic: 0 = writeFileSync, 1 = appendFileSync
JSValue js_write_file(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string path, data;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "The \"data\" argument is required");
    get_data(ctx, argv[1], data);
    if (Result r = write_all(path, data.data(), data.size(), magic == 1); r.err) return throw_result(ctx, r, path);
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
    std::vector<std::string> names;
    if (Result r = list_dir(path, names); r.err) return throw_result(ctx, r, path);
    return string_array(ctx, names);
}

// mkdirSync(path, { recursive: true }) -> first directory created (like Node), or undefined
JSValue js_mkdir(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path, first;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    if (Result r = make_dir(path, get_bool_option(ctx, argc, argv, 1, "recursive"), first); r.err) {
        return throw_result(ctx, r, path);
    }
    return first.empty() ? JS_UNDEFINED : JS_NewString(ctx, first.c_str());
}

// rmSync(path, { recursive: true, force: true })
JSValue js_rm(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    Result r = remove_path(path, get_bool_option(ctx, argc, argv, 1, "recursive"),
                           get_bool_option(ctx, argc, argv, 1, "force"));
    return r.err ? throw_result(ctx, r, path) : JS_UNDEFINED;
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
    if (Result r = copy(from, to); r.err) return throw_result(ctx, r, from);
    return JS_UNDEFINED;
}

// statSync / lstatSync (magic: 1 = lstat)
JSValue js_stat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string path;
    if (!get_path(ctx, argc, argv, path)) return JS_EXCEPTION;
    struct stat st{};
    int rc = magic ? lstat(path.c_str(), &st) : stat(path.c_str(), &st);
    if (rc != 0) return throw_errno(ctx, errno, magic ? "lstat" : "stat", path);
    return stats_object(ctx, st);
}

// ---------------------------------------------------------------------------
// Asynchronous API (fs/promises): fsAsync(op, path, arg, flags, callback)
//   arg:   the second path (rename, copyFile) or the data (writeFile, appendFile)
//   flags: 1 = text / recursive, 2 = force; the mode for access()
//   callback(error, value) runs on the event loop thread
// ---------------------------------------------------------------------------

enum class Op { ReadFile, WriteFile, AppendFile, Readdir, Mkdir, Rm, Rename, CopyFile, Stat, Lstat, Access,
                Unlink, Rmdir, Realpath };

struct Job {
    Op op;
    std::string path, arg;
    int flags = 0;
    Result result;
    std::string text;  // file contents, realpath, first created directory
    std::vector<std::string> names;
    struct stat st{};
};

void run_job(Job& j) {
    switch (j.op) {
        case Op::ReadFile: j.result = read_all(j.path, j.text); break;
        case Op::WriteFile:
        case Op::AppendFile: j.result = write_all(j.path, j.arg.data(), j.arg.size(), j.op == Op::AppendFile); break;
        case Op::Readdir: j.result = list_dir(j.path, j.names); break;
        case Op::Mkdir: j.result = make_dir(j.path, j.flags & 1, j.text); break;
        case Op::Rm: j.result = remove_path(j.path, j.flags & 1, j.flags & 2); break;
        case Op::Rename:
            if (rename(j.path.c_str(), j.arg.c_str()) != 0) j.result = fail(errno, "rename");
            break;
        case Op::CopyFile: j.result = copy(j.path, j.arg); break;
        case Op::Stat:
            if (stat(j.path.c_str(), &j.st) != 0) j.result = fail(errno, "stat");
            break;
        case Op::Lstat:
            if (lstat(j.path.c_str(), &j.st) != 0) j.result = fail(errno, "lstat");
            break;
        case Op::Access:
            if (access(j.path.c_str(), j.flags) != 0) j.result = fail(errno, "access");
            break;
        case Op::Unlink:
            if (unlink(j.path.c_str()) != 0) j.result = fail(errno, "unlink");
            break;
        case Op::Rmdir:
            if (rmdir(j.path.c_str()) != 0) j.result = fail(errno, "rmdir");
            break;
        case Op::Realpath: {
            char buf[PATH_MAX];
            if (realpath(j.path.c_str(), buf)) j.text = buf;
            else j.result = fail(errno, "realpath");
            break;
        }
    }
}

JSValue job_value(JSContext* ctx, const Job& j) {
    switch (j.op) {
        case Op::ReadFile:
            if (j.flags & 1) return JS_NewStringLen(ctx, j.text.data(), j.text.size());
            return JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(j.text.data()), j.text.size());
        case Op::Readdir: return string_array(ctx, j.names);
        case Op::Mkdir: return j.text.empty() ? JS_UNDEFINED : JS_NewString(ctx, j.text.c_str());
        case Op::Stat:
        case Op::Lstat: return stats_object(ctx, j.st);
        case Op::Realpath: return JS_NewString(ctx, j.text.c_str());
        default: return JS_UNDEFINED;
    }
}

JSValue js_fs_async(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    static const std::pair<const char*, Op> kOps[] = {
        {"readFile", Op::ReadFile}, {"writeFile", Op::WriteFile}, {"appendFile", Op::AppendFile},
        {"readdir", Op::Readdir}, {"mkdir", Op::Mkdir}, {"rm", Op::Rm}, {"rename", Op::Rename},
        {"copyFile", Op::CopyFile}, {"stat", Op::Stat}, {"lstat", Op::Lstat}, {"access", Op::Access},
        {"unlink", Op::Unlink}, {"rmdir", Op::Rmdir}, {"realpath", Op::Realpath},
    };
    if (argc < 5 || !JS_IsFunction(ctx, argv[4])) return JS_ThrowTypeError(ctx, "fsAsync: bad arguments");
    auto job = std::make_shared<Job>();
    std::string op = to_string(ctx, argv[0]);
    auto it = std::find_if(std::begin(kOps), std::end(kOps), [&](auto& p) { return op == p.first; });
    if (it == std::end(kOps)) return JS_ThrowTypeError(ctx, "fsAsync: unknown operation '%s'", op.c_str());
    job->op = it->second;
    if (!get_path(ctx, argc, argv, job->path, 1)) return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[2])) get_data(ctx, argv[2], job->arg);
    JS_ToInt32(ctx, &job->flags, argv[3]);

    JSValue callback = JS_DupValue(ctx, argv[4]);
    Runtime::from(ctx)->queue_work([job] { run_job(*job); }, [job, ctx, callback](bool cancelled) {
        if (!cancelled) {
            JSValue args[2];
            if (job->result.err) {
                args[0] = to_error(ctx, job->result, job->path);
                args[1] = JS_UNDEFINED;
            } else {
                args[0] = JS_NULL;
                args[1] = job_value(ctx, *job);
            }
            JSValue ret = JS_Call(ctx, callback, JS_UNDEFINED, 2, args);
            if (JS_IsException(ret)) dump_pending_exception(ctx);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
        }
        JS_FreeValue(ctx, callback);
    });
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// The module
// ---------------------------------------------------------------------------

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

}  // namespace

// native.fsAsync for fs/promises, native.fsSync = { readFileSync, ... } for the "fs" module
// (src/js/modules.js builds the module object from it).
void add_fs_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyStr(ctx, native, "fsAsync", JS_NewCFunction(ctx, js_fs_async, "fsAsync", 5));
    JSValue sync = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sync, kFsFuncs, kFsFuncCount);
    JS_SetPropertyStr(ctx, native, "fsSync", sync);
}

}  // namespace rtn
