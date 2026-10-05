#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <string>
#include <vector>

#include "quickjs.h"

namespace rtn {

using Clock = std::chrono::steady_clock;

// Something that waits for a file descriptor (socket) to become readable/writable.
class IoHandler {
public:
    virtual ~IoHandler() = default;
    virtual void on_io(uint32_t events) = 0;  // EPOLLIN / EPOLLOUT / EPOLLERR ...
};

struct Timer {
    JSValue callback;
    std::vector<JSValue> args;
    int64_t interval_ms;  // 0 = setTimeout, >0 = setInterval
};

// Owns the JS engine (JSRuntime + JSContext) and the event loop.
class Runtime {
public:
    Runtime(int argc, char** argv);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Loads `path` as an ES module and runs the event loop until it's empty.
    // Returns the process exit code.
    int run_file(const std::string& path);
    // Runs `code` as an ES module (for `rtn -e`).
    int run_code(const std::string& code);
    // Reads a script from stdin and runs it (`echo 'code' | rtn`).
    int run_stdin();
    // Interactive read-eval-print loop (src/repl.cpp).
    int repl();
    // Runs internal[fn](args) (e.g. "runTests" for `rtn test`) and the event loop;
    // the exit code is the number the returned promise resolves to.
    int run_internal(const char* fn, const std::vector<std::string>& args);

    static Runtime* from(JSContext* ctx) {
        return static_cast<Runtime*>(JS_GetContextOpaque(ctx));
    }

    JSContext* ctx() const { return ctx_; }
    const std::vector<std::string>& argv() const { return argv_; }

    // process.nextTick(fn, ...args): runs before promise jobs, like Node.
    void next_tick(JSValue fn, std::vector<JSValue> args) { ticks_.push_back({fn, std::move(args)}); }

    // --- timers (used by bindings/timers.cpp) ---
    int64_t add_timer(JSValue cb, std::vector<JSValue> args, int64_t delay_ms, bool repeat);
    void clear_timer(int64_t id);
    // One-shot timer for C++ code (e.g. HTTP idle timeouts).
    int64_t add_native_timer(int64_t delay_ms, std::function<void()> fn);
    void cancel_native_timer(int64_t id) {
        native_timers_.erase(id);
        unref_timers_.erase(id);
    }
    // The timer still fires, but doesn't keep the process alive (Node's timer.unref()).
    void unref_timer(int64_t id);

    // --- thread pool (DNS lookups for fetch, fs/promises) ---
    // Runs `work` on a background thread, then `done(false)` on the event loop thread.
    // `work` must not touch the JS engine. If the runtime shuts down first, `done(true)`
    // runs instead (so it can free the JSValues it holds). Pending work keeps the loop alive.
    void queue_work(std::function<void()> work, std::function<void(bool cancelled)> done);

    // Makes run_event_loop() return at its next iteration (REPL .exit / Ctrl+D).
    void request_stop() { stop_requested_ = true; }
    // Keeps the loop alive even without timers or sockets (REPL waiting on a promise).
    void ref() { ++refs_; }
    void unref() { --refs_; }
    // REPL mode: uncaught errors are printed but don't end the event loop.
    void set_errors_fatal(bool fatal) { errors_fatal_ = fatal; }

    // --- I/O (used by bindings/http.cpp) ---
    // While any fd is registered the event loop keeps running.
    // Returns 0 if the fd can't be watched (e.g. a regular file).
    uint64_t add_io(int fd, uint32_t events, IoHandler* handler);
    void modify_io(uint64_t id, int fd, uint32_t events);
    void remove_io(uint64_t id, int fd);
    // Runs `fn` later at a safe point (e.g. to delete an object whose method is on the stack).
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }
    // Runs before the JS engine is destroyed (free JSValues held by native code).
    void on_shutdown(std::function<void()> fn) { shutdown_hooks_.push_back(std::move(fn)); }

    // Runs everything: microtasks, timers, I/O. Returns false on a fatal error.
    bool run_event_loop();

private:
    int eval_module(const std::string& code, const std::string& filename);
    // Runs nextTick callbacks and promise jobs. Returns false if one threw.
    // jobs_first: promise jobs before ticks (right after the main module body,
    // which in Node runs inside a promise job itself).
    bool drain_microtasks(bool jobs_first = false);
    // Fires timers that are due. Returns false on a fatal error.
    bool run_due_timers();
    // Waits up to timeout_ms for I/O and dispatches it. Returns false on a fatal error.
    bool poll_io(int timeout_ms);
    void run_deferred();
    // Calls `done` for work the thread pool has finished. Returns false on a fatal error.
    bool run_finished_work();
    bool report_unhandled_rejections();
    void free_timer(Timer& t);

    static void on_promise_rejection(JSContext* ctx, JSValueConst promise,
                                     JSValueConst reason, bool is_handled, void* opaque);

    int epoll_fd_ = -1;
    uint64_t next_io_id_ = 1;
    std::unordered_map<uint64_t, IoHandler*> io_handlers_;
    std::vector<std::function<void()>> deferred_;
    std::vector<std::function<void()>> shutdown_hooks_;

    struct WorkPool;  // runtime.cpp; shared with the worker threads, which outlive us if busy
    std::shared_ptr<WorkPool> pool_;
    std::unordered_map<uint64_t, std::function<void(bool)>> work_done_;
    uint64_t next_work_id_ = 1;

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    std::vector<std::string> argv_;

    struct TimerEntry {
        Clock::time_point due;
        uint64_t seq;  // keeps timers with the same `due` in FIFO order
        int64_t id;
        bool operator>(const TimerEntry& o) const {
            return due != o.due ? due > o.due : seq > o.seq;
        }
    };
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>> timer_queue_;
    std::map<int64_t, Timer> timers_;
    std::map<int64_t, std::function<void()>> native_timers_;
    std::unordered_set<int64_t> unref_timers_;
    std::vector<Timer> ticks_;  // pending process.nextTick callbacks (interval_ms unused)
    bool stop_requested_ = false;
    int refs_ = 0;
    bool errors_fatal_ = true;
    int64_t next_timer_id_ = 1;
    uint64_t timer_seq_ = 0;

    // Rejected promises nobody has handled yet: promise -> reason.
    std::vector<std::pair<JSValue, JSValue>> pending_rejections_;
    // The main module's evaluation promise: its rejection is a plain "Uncaught" error.
    void* main_promise_ = nullptr;
};

}  // namespace rtn
