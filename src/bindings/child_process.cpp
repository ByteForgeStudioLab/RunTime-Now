// Native side of node:child_process (src/js/child_process.js).
//
//   spawnChild(file, args, cwd, env, stdio, onEvent) -> { id, pid } | { errno, code }
//   childWrite(id, bytes) / childEnd(id) / childKill(id, signal)
//   spawnSync(file, args, cwd, env, stdio, input, timeoutMs, killSignal, maxBuffer)
//
// stdio is three strings: "pipe", "inherit" or "ignore". A child's stdout and
// stderr pipes and a pidfd for its exit are watched by the event loop, so a
// running child keeps the process alive. onEvent(kind, a, b) gets
// 0 = data (fd, Uint8Array), 1 = end of a stream (fd), 2 = exit (code, signal).

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "util.hpp"

namespace rtn {

namespace {

enum class Stdio { Pipe, Inherit, Ignore };

int open_pidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));  // Linux 5.3+
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

void close_fd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}

// Everything the child needs, prepared before fork(): after fork only
// async-signal-safe calls are allowed (the thread pool may hold malloc's lock).
struct SpawnPlan {
    std::vector<std::string> candidates;  // paths to try with execve, in order
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::string cwd;
    Stdio stdio[3] = {Stdio::Pipe, Stdio::Pipe, Stdio::Pipe};
};

struct Spawned {
    pid_t pid = -1;
    int err = 0;
    int fds[3] = {-1, -1, -1};  // our ends of the pipes
};

// Where to look for `file`: as is if it has a slash, else every PATH entry
// (the child's PATH if its environment has one, like libuv).
std::vector<std::string> exec_candidates(const std::string& file, const std::vector<std::string>& env) {
    if (file.find('/') != std::string::npos) return {file};
    const char* path = nullptr;
    for (auto& kv : env) {
        if (kv.starts_with("PATH=")) path = kv.c_str() + 5;
    }
    if (!path) path = std::getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    std::vector<std::string> out;
    std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string dir = p.substr(start, end - start);
        out.push_back((dir.empty() ? "." : dir) + "/" + file);
        start = end + 1;
    }
    return out;
}

Spawned spawn_process(const SpawnPlan& plan) {
    Spawned s;
    int child_fds[3] = {-1, -1, -1};
    auto cleanup = [&] {
        for (int i = 0; i < 3; ++i) {
            close_fd(child_fds[i]);
            close_fd(s.fds[i]);
        }
    };
    for (int i = 0; i < 3; ++i) {
        if (plan.stdio[i] == Stdio::Pipe) {
            int p[2];
            if (pipe2(p, O_CLOEXEC) != 0) {
                s.err = errno;
                cleanup();
                return s;
            }
            // fd 0: the child reads p[0]; fds 1/2: the child writes p[1].
            child_fds[i] = i == 0 ? p[0] : p[1];
            s.fds[i] = i == 0 ? p[1] : p[0];
        } else if (plan.stdio[i] == Stdio::Ignore) {
            child_fds[i] = open("/dev/null", (i == 0 ? O_RDONLY : O_WRONLY) | O_CLOEXEC);
        }
    }
    int err_pipe[2];  // the child reports a failed exec through this; closed by a successful one
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        s.err = errno;
        cleanup();
        return s;
    }

    std::vector<char*> argv, envp, paths;
    for (auto& a : plan.args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (auto& e : plan.env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    for (auto& c : plan.candidates) paths.push_back(const_cast<char*>(c.c_str()));
    const char* cwd = plan.cwd.empty() ? nullptr : plan.cwd.c_str();

    std::fflush(stdout);  // an inherited stdout must not get our buffered text after the child's
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        s.err = errno;
        close(err_pipe[0]);
        close(err_pipe[1]);
        cleanup();
        return s;
    }
    if (pid == 0) {
        // --- child: async-signal-safe calls only ---
        auto die = [&](int e) {
            [[maybe_unused]] ssize_t n = write(err_pipe[1], &e, sizeof e);
            _exit(127);
        };
        // dup2 clears O_CLOEXEC on the new fd. Move pipe ends out of 0..2 first, so
        // that setting up fd 1 can't overwrite an end fd 2 still needs.
        for (int i = 0; i < 3; ++i) {
            if (child_fds[i] >= 0 && child_fds[i] <= 2) {
                int moved = fcntl(child_fds[i], F_DUPFD_CLOEXEC, 3);
                if (moved < 0) die(errno);
                child_fds[i] = moved;
            }
        }
        for (int i = 0; i < 3; ++i) {
            if (child_fds[i] >= 0 && dup2(child_fds[i], i) < 0) die(errno);
        }
        // rtn ignores SIGPIPE; an ignored signal stays ignored across exec, so reset it.
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        for (int sig = 1; sig < NSIG; ++sig) sigaction(sig, &sa, nullptr);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        if (cwd && chdir(cwd) != 0) die(errno);
        int err = ENOENT;
        bool denied = false;
        for (char* p : paths) {
            execve(p, argv.data(), envp.data());
            err = errno;
            if (err == EACCES) denied = true;
            if (err != ENOENT && err != ENOTDIR && err != EACCES) break;
        }
        die(denied && (err == ENOENT || err == ENOTDIR) ? EACCES : err);
    }

    // --- parent ---
    close(err_pipe[1]);
    for (int i = 0; i < 3; ++i) close_fd(child_fds[i]);
    int child_err = 0;
    ssize_t n;
    do {
        n = read(err_pipe[0], &child_err, sizeof child_err);
    } while (n < 0 && errno == EINTR);
    close(err_pipe[0]);
    if (n == static_cast<ssize_t>(sizeof child_err)) {  // exec failed: no process to talk to
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        s.err = child_err;
        for (int i = 0; i < 3; ++i) close_fd(s.fds[i]);
        return s;
    }
    s.pid = pid;
    return s;
}

// Reads spawn arguments shared by spawnChild and spawnSync. Returns false (with a
// pending exception) on bad input.
bool read_plan(JSContext* ctx, JSValueConst* argv, SpawnPlan& plan) {
    std::string file = to_string(ctx, argv[0]);
    auto read_strings = [&](JSValueConst arr, std::vector<std::string>& out) {
        int64_t len = 0;
        if (!JS_IsArray(arr)) return true;
        if (JS_GetLength(ctx, arr, &len) < 0) return false;
        for (int64_t i = 0; i < len; ++i) {
            JSValue v = JS_GetPropertyInt64(ctx, arr, i);
            out.push_back(to_string(ctx, v));
            JS_FreeValue(ctx, v);
        }
        return true;
    };
    if (!read_strings(argv[1], plan.args) || !read_strings(argv[3], plan.env)) return false;
    for (auto& s : plan.args) {
        if (s.find('\0') != std::string::npos) {
            JS_ThrowTypeError(ctx, "The argument 'args' must be a string without null bytes");
            return false;
        }
    }
    if (!JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) plan.cwd = to_string(ctx, argv[2]);
    for (uint32_t i = 0; i < 3; ++i) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[4], i);
        std::string kind = JS_IsString(v) ? to_string(ctx, v) : "pipe";
        JS_FreeValue(ctx, v);
        plan.stdio[i] = kind == "inherit" ? Stdio::Inherit : kind == "ignore" ? Stdio::Ignore : Stdio::Pipe;
    }
    plan.candidates = exec_candidates(file, plan.env);
    return true;
}

JSValue spawn_error(JSContext* ctx, int err) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "errno", JS_NewInt32(ctx, -err));
    JS_SetPropertyStr(ctx, r, "code", JS_NewString(ctx, errno_name(err)));
    JS_SetPropertyStr(ctx, r, "message", JS_NewString(ctx, std::strerror(err)));
    return r;
}

// (code, signal) from a waitpid status, as JS values.
void exit_values(JSContext* ctx, int status, JSValue& code, JSValue& signal) {
    code = JS_NULL;
    signal = JS_NULL;
    if (WIFEXITED(status)) code = JS_NewInt32(ctx, WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) signal = JS_NewInt32(ctx, WTERMSIG(status));
}

// ---------------------------------------------------------------------------
// Asynchronous children
// ---------------------------------------------------------------------------

struct Child;
std::unordered_map<uint64_t, Child*> g_children;
uint64_t g_next_child_id = 1;
bool g_shutdown_hook_installed = false;

struct Child {
    // One watched fd: a pipe end or the pidfd.
    struct Watch : IoHandler {
        Child* child = nullptr;
        int index = 0;  // 0..2 = stdio, 3 = pidfd
        int fd = -1;
        uint64_t io_id = 0;
        void on_io(uint32_t events) override { child->on_io(index, events); }
    };

    Runtime* rt;
    JSContext* ctx;
    uint64_t id;
    pid_t pid;
    JSValue callback;
    Watch watches[4];
    std::string pending_in;  // bytes for stdin that the pipe didn't take yet
    bool end_requested = false;
    bool exited = false;
    int64_t poll_timer = 0;  // waitpid polling when pidfd_open isn't available

    Child(Runtime* r, uint64_t cid, pid_t p, JSValue cb) : rt(r), ctx(r->ctx()), id(cid), pid(p), callback(cb) {
        for (int i = 0; i < 4; ++i) {
            watches[i].child = this;
            watches[i].index = i;
        }
    }

    bool watch(int index, int fd, uint32_t events) {
        Watch& w = watches[index];
        w.fd = fd;
        w.io_id = rt->add_io(fd, events, &w);
        return w.io_id != 0;
    }

    void unwatch(int index) {
        Watch& w = watches[index];
        if (w.fd < 0) return;
        if (w.io_id) rt->remove_io(w.io_id, w.fd);
        close(w.fd);
        w.fd = -1;
        w.io_id = 0;
    }

    void emit(int kind, JSValue a, JSValue b) {
        if (!JS_IsFunction(ctx, callback)) {
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, b);
            return;
        }
        JSValue cb = JS_DupValue(ctx, callback);  // the handler may finish us
        JSValue args[] = {JS_NewInt32(ctx, kind), a, b};
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 3, args);
        if (JS_IsException(ret)) dump_pending_exception(ctx);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, cb);
        for (JSValue v : args) JS_FreeValue(ctx, v);
    }

    void on_io(int index, uint32_t events) {
        if (index == 3) {
            reap();
            return;
        }
        if (index == 0) {
            if (events & (EPOLLERR | EPOLLHUP)) {  // the child closed its stdin
                pending_in.clear();
                unwatch(0);
                return;
            }
            flush_stdin();
            return;
        }
        char buf[65536];
        while (watches[index].fd >= 0) {
            ssize_t n = read(watches[index].fd, buf, sizeof buf);
            if (n > 0) {
                emit(0, JS_NewInt32(ctx, index), JS_NewUint8ArrayCopy(ctx, reinterpret_cast<uint8_t*>(buf), static_cast<size_t>(n)));
                if (static_cast<size_t>(n) < sizeof buf) return;  // drained (level-triggered: more wakes us again)
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            unwatch(index);  // EOF or error
            emit(1, JS_NewInt32(ctx, index), JS_UNDEFINED);
            finish_if_done();
            return;
        }
    }

    void write_stdin(const char* data, size_t size) {
        if (watches[0].fd < 0 || end_requested) return;
        pending_in.append(data, size);
        flush_stdin();
    }

    void flush_stdin() {
        Watch& w = watches[0];
        if (w.fd < 0) return;
        size_t off = 0;
        while (off < pending_in.size()) {
            ssize_t n = write(w.fd, pending_in.data() + off, pending_in.size() - off);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            pending_in.clear();  // EPIPE: the child doesn't read stdin any more
            unwatch(0);
            return;
        }
        pending_in.erase(0, off);
        if (pending_in.empty() && end_requested) {
            unwatch(0);
            return;
        }
        // Watch for room only while something is queued: an idle stdin that nobody
        // ends must not keep the process alive.
        if (!pending_in.empty() && !w.io_id) {
            w.io_id = rt->add_io(w.fd, EPOLLOUT, &w);
        } else if (pending_in.empty() && w.io_id) {
            rt->remove_io(w.io_id, w.fd);
            w.io_id = 0;
        }
    }

    void end_stdin() {
        end_requested = true;
        if (pending_in.empty()) unwatch(0);
    }

    void reap() {
        if (exited) return;
        int status = 0;
        pid_t r;
        do {
            r = waitpid(pid, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r == 0) return;  // still running
        exited = true;
        unwatch(3);
        if (poll_timer) {
            rt->cancel_native_timer(poll_timer);
            poll_timer = 0;
        }
        JSValue code, signal;
        if (r < 0) {
            code = JS_NULL;
            signal = JS_NULL;
        } else {
            exit_values(ctx, status, code, signal);
        }
        emit(2, code, signal);
        finish_if_done();
    }

    void start_polling() {
        poll_timer = rt->add_native_timer(20, [this] {
            poll_timer = 0;
            reap();
            if (!exited) start_polling();
        });
    }

    void finish_if_done() {
        if (!exited || watches[1].fd >= 0 || watches[2].fd >= 0) return;
        release();
    }

    // Forgets this child (it may still run); freed once it's off the stack.
    void release() {
        if (!g_children.erase(id)) return;
        for (int i = 0; i < 4; ++i) unwatch(i);
        if (poll_timer) rt->cancel_native_timer(poll_timer);
        poll_timer = 0;
        JS_FreeValue(ctx, callback);
        callback = JS_UNDEFINED;
        Child* self = this;
        rt->defer([self] { delete self; });
    }
};

void shutdown_all() {
    std::vector<Child*> list;
    for (auto& [id, c] : g_children) list.push_back(c);
    for (Child* c : list) c->release();
}

Child* find_child(JSContext* ctx, JSValueConst v) {
    int64_t id = 0;
    JS_ToInt64(ctx, &id, v);
    auto it = g_children.find(static_cast<uint64_t>(id));
    return it == g_children.end() ? nullptr : it->second;
}

JSValue js_spawn_child(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6 || !JS_IsFunction(ctx, argv[5])) return JS_ThrowTypeError(ctx, "spawnChild: bad arguments");
    SpawnPlan plan;
    if (!read_plan(ctx, argv, plan)) return JS_EXCEPTION;
    Spawned s = spawn_process(plan);
    if (s.pid < 0) return spawn_error(ctx, s.err);

    Runtime* rt = Runtime::from(ctx);
    if (!g_shutdown_hook_installed) {
        g_shutdown_hook_installed = true;
        rt->on_shutdown(shutdown_all);
    }
    uint64_t id = g_next_child_id++;
    auto* c = new Child(rt, id, s.pid, JS_DupValue(ctx, argv[5]));
    g_children[id] = c;
    if (s.fds[0] >= 0) {
        fcntl(s.fds[0], F_SETFL, fcntl(s.fds[0], F_GETFL) | O_NONBLOCK);
        c->watches[0].fd = s.fds[0];  // watched (EPOLLOUT) only while writes are queued
    }
    for (int i = 1; i < 3; ++i) {
        if (s.fds[i] < 0) continue;
        fcntl(s.fds[i], F_SETFL, fcntl(s.fds[i], F_GETFL) | O_NONBLOCK);
        c->watch(i, s.fds[i], EPOLLIN);
    }
    int pidfd = open_pidfd(s.pid);
    if (pidfd < 0 || !c->watch(3, pidfd, EPOLLIN)) {
        if (pidfd >= 0) close(pidfd);
        c->watches[3].fd = -1;
        c->start_polling();
    }

    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "id", JS_NewInt64(ctx, static_cast<int64_t>(id)));
    JS_SetPropertyStr(ctx, r, "pid", JS_NewInt32(ctx, s.pid));
    return r;
}

JSValue js_child_write(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_FALSE;
    Child* c = find_child(ctx, argv[0]);
    if (!c) return JS_FALSE;
    size_t size = 0;
    if (uint8_t* bytes = JS_GetTypedArrayType(argv[1]) >= 0 ? JS_GetUint8Array(ctx, &size, argv[1]) : nullptr) {
        c->write_stdin(reinterpret_cast<const char*>(bytes), size);
        return JS_TRUE;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    std::string s = to_string(ctx, argv[1]);
    c->write_stdin(s.data(), s.size());
    return JS_TRUE;
}

JSValue js_child_end(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    if (Child* c = find_child(ctx, argv[0])) c->end_stdin();
    return JS_UNDEFINED;
}

// childKill(id, signal) -> true if the signal was sent
JSValue js_child_kill(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_FALSE;
    Child* c = find_child(ctx, argv[0]);
    if (!c || c->exited) return JS_FALSE;
    int32_t sig = SIGTERM;
    JS_ToInt32(ctx, &sig, argv[1]);
    return JS_NewBool(ctx, kill(c->pid, sig) == 0);
}

// ---------------------------------------------------------------------------
// spawnSync
// ---------------------------------------------------------------------------

JSValue js_spawn_sync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx, "spawnSync: bad arguments");
    SpawnPlan plan;
    if (!read_plan(ctx, argv, plan)) return JS_EXCEPTION;
    std::string input;
    if (!JS_IsNull(argv[5]) && !JS_IsUndefined(argv[5])) {
        size_t size = 0;
        if (uint8_t* bytes = JS_GetTypedArrayType(argv[5]) >= 0 ? JS_GetUint8Array(ctx, &size, argv[5]) : nullptr) {
            input.assign(reinterpret_cast<const char*>(bytes), size);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            input = to_string(ctx, argv[5]);
        }
    }
    double timeout_ms = 0, max_buffer = 0;
    int32_t kill_signal = SIGTERM;
    JS_ToFloat64(ctx, &timeout_ms, argv[6]);
    JS_ToInt32(ctx, &kill_signal, argv[7]);
    JS_ToFloat64(ctx, &max_buffer, argv[8]);

    Spawned s = spawn_process(plan);
    if (s.pid < 0) {
        JSValue r = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, r, "error", spawn_error(ctx, s.err));
        return r;
    }
    for (int i = 0; i < 3; ++i) {
        if (s.fds[i] >= 0) fcntl(s.fds[i], F_SETFL, fcntl(s.fds[i], F_GETFL) | O_NONBLOCK);
    }
    if (s.fds[0] >= 0 && input.empty()) close_fd(s.fds[0]);
    int pidfd = open_pidfd(s.pid);

    std::string out[3];
    size_t in_off = 0;
    bool exited = false, timed_out = false, overflow = false;
    int status = 0;
    auto deadline = Clock::now() + std::chrono::microseconds(static_cast<int64_t>(timeout_ms * 1000));
    auto try_reap = [&] {
        if (exited) return;
        pid_t r = waitpid(s.pid, &status, WNOHANG);
        if (r == s.pid || (r < 0 && errno != EINTR)) exited = true;
    };
    while (true) {
        try_reap();
        if (exited && s.fds[1] < 0 && s.fds[2] < 0) break;
        pollfd pfds[4];
        int map[4];
        int n = 0;
        if (s.fds[0] >= 0) {
            pfds[n] = {s.fds[0], POLLOUT, 0};
            map[n++] = 0;
        }
        for (int i = 1; i < 3; ++i) {
            if (s.fds[i] < 0) continue;
            pfds[n] = {s.fds[i], POLLIN, 0};
            map[n++] = i;
        }
        if (pidfd >= 0 && !exited) {
            pfds[n] = {pidfd, POLLIN, 0};
            map[n++] = 3;
        }
        int wait_ms = (pidfd >= 0 || exited) ? -1 : 20;  // without a pidfd, poll waitpid
        if (timeout_ms > 0 && !timed_out) {
            auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) {
                timed_out = true;
                kill(s.pid, kill_signal);
                continue;
            }
            wait_ms = wait_ms < 0 ? static_cast<int>(left) : std::min<int>(wait_ms, static_cast<int>(left));
        }
        int ready = poll(pfds, n, wait_ms);
        if (ready < 0 && errno != EINTR) break;
        for (int k = 0; k < n && ready > 0; ++k) {
            if (!pfds[k].revents) continue;
            int i = map[k];
            if (i == 3) continue;  // try_reap() at the top
            if (i == 0) {
                ssize_t w = write(s.fds[0], input.data() + in_off, input.size() - in_off);
                if (w > 0) in_off += static_cast<size_t>(w);
                if ((w < 0 && errno != EAGAIN && errno != EINTR) || in_off >= input.size()) close_fd(s.fds[0]);
                continue;
            }
            char buf[65536];
            ssize_t r = read(s.fds[i], buf, sizeof buf);
            if (r > 0) {
                out[i].append(buf, static_cast<size_t>(r));
                if (max_buffer > 0 && out[i].size() > max_buffer && !overflow) {
                    overflow = true;
                    out[i].resize(static_cast<size_t>(max_buffer));
                    kill(s.pid, kill_signal);
                }
            } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                close_fd(s.fds[i]);
            }
        }
    }
    for (int i = 0; i < 3; ++i) close_fd(s.fds[i]);
    if (pidfd >= 0) close(pidfd);
    if (!exited) {
        while (waitpid(s.pid, &status, 0) < 0 && errno == EINTR) {}
    }

    JSValue r = JS_NewObject(ctx);
    JSValue code, signal;
    exit_values(ctx, status, code, signal);
    JS_SetPropertyStr(ctx, r, "pid", JS_NewInt32(ctx, s.pid));
    JS_SetPropertyStr(ctx, r, "status", code);
    JS_SetPropertyStr(ctx, r, "signal", signal);
    for (int i = 1; i < 3; ++i) {
        if (plan.stdio[i] != Stdio::Pipe) continue;
        JS_SetPropertyStr(ctx, r, i == 1 ? "stdout" : "stderr",
                          JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(out[i].data()), out[i].size()));
    }
    if (timed_out) JS_SetPropertyStr(ctx, r, "error", spawn_error(ctx, ETIMEDOUT));
    else if (overflow) JS_SetPropertyStr(ctx, r, "error", spawn_error(ctx, ENOBUFS));
    return r;
}

const JSCFunctionListEntry kChildFuncs[] = {
    JS_CFUNC_DEF("spawnChild", 6, js_spawn_child),
    JS_CFUNC_DEF("childWrite", 2, js_child_write),
    JS_CFUNC_DEF("childEnd", 1, js_child_end),
    JS_CFUNC_DEF("childKill", 2, js_child_kill),
    JS_CFUNC_DEF("spawnSync", 9, js_spawn_sync),
};

}  // namespace

void add_child_process_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyFunctionList(ctx, native, kChildFuncs, sizeof(kChildFuncs) / sizeof(kChildFuncs[0]));
}

}  // namespace rtn
