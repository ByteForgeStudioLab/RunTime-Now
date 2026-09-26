#include "runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <csignal>
#include <iostream>
#include <sys/epoll.h>
#include <unistd.h>

#include "bindings/bindings.hpp"
#include "modules.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace rtn {

Runtime::Runtime(int argc, char** argv) : argv_(argv, argv + argc) {
    std::signal(SIGPIPE, SIG_IGN);  // writing to a closed socket must not kill us
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);

    rt_ = JS_NewRuntime();
    ctx_ = JS_NewContext(rt_);
    JS_SetContextOpaque(ctx_, this);

    install_module_loader(rt_);
    JS_SetHostPromiseRejectionTracker(rt_, &Runtime::on_promise_rejection, this);

    install_console(ctx_);
    install_timers(ctx_);
    install_process(ctx_);
    install_builtins(ctx_);  // Web APIs + rtn.serve (written in JS, see src/js/)
}

Runtime::~Runtime() {
    for (auto& hook : shutdown_hooks_) hook();
    run_deferred();
    for (auto& [id, t] : timers_) free_timer(t);
    timers_.clear();
    for (auto& t : ticks_) free_timer(t);
    ticks_.clear();
    for (auto& [p, r] : pending_rejections_) {
        JS_FreeValue(ctx_, p);
        JS_FreeValue(ctx_, r);
    }
    JS_FreeContext(ctx_);
    JS_FreeRuntime(rt_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

int Runtime::run_file(const std::string& path) {
    std::error_code ec;
    fs::path abs = fs::canonical(fs::absolute(path), ec);
    if (ec || !fs::is_regular_file(abs)) {
        std::fprintf(stderr, "error: Module not found \"%s\"\n", path.c_str());
        return 1;
    }
    std::string code;
    if (!load_source(ctx_, abs.string(), code)) {
        dump_pending_exception(ctx_);
        return 1;
    }
    return eval_module(code, abs.string());
}

int Runtime::run_stdin() {
    std::string code;
    char buf[65536];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0) code.append(buf, static_cast<size_t>(n));
    return eval_module(code, (fs::current_path() / "[stdin]").string());
}

int Runtime::run_code(const std::string& code) {
    // A fake file name in the cwd so relative imports work from `-e`.
    return eval_module(code, (fs::current_path() / "[eval]").string());
}

int Runtime::eval_module(const std::string& code, const std::string& filename) {
    JSValue fn = JS_Eval(ctx_, code.c_str(), code.size(), filename.c_str(),
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) {
        dump_pending_exception(ctx_);
        return 1;
    }
    set_import_meta(ctx_, fn, filename, true);

    // Evaluating a module returns a promise (modules may use top-level await).
    JSValue promise = JS_EvalFunction(ctx_, fn);
    if (JS_IsException(promise)) {
        dump_pending_exception(ctx_);
        return 1;
    }
    main_promise_ = JS_VALUE_GET_PTR(promise);

    int code_out = 0;
    if (!drain_microtasks(true) || !run_event_loop()) {
        code_out = 1;
    } else {
        switch (JS_PromiseState(ctx_, promise)) {
            case JS_PROMISE_REJECTED: {
                JSValue reason = JS_PromiseResult(ctx_, promise);
                print_exception(ctx_, reason);
                JS_FreeValue(ctx_, reason);
                code_out = 1;
                break;
            }
            case JS_PROMISE_PENDING:
                // Same as Node: the loop is empty but top-level await never finished.
                std::fprintf(stderr, "Warning: Detected unsettled top-level await\n");
                code_out = 13;
                break;
            default:
                break;
        }
    }
    JS_FreeValue(ctx_, promise);
    main_promise_ = nullptr;

    if (code_out == 0) {
        // Respect `process.exitCode = n`.
        JSValue global = JS_GetGlobalObject(ctx_);
        JSValue process = JS_GetPropertyStr(ctx_, global, "process");
        JSValue exit_code = JS_GetPropertyStr(ctx_, process, "exitCode");
        int32_t n = 0;
        if (JS_IsNumber(exit_code) && JS_ToInt32(ctx_, &n, exit_code) == 0) code_out = n;
        JS_FreeValue(ctx_, exit_code);
        JS_FreeValue(ctx_, process);
        JS_FreeValue(ctx_, global);
    }
    return code_out;
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------
//
//   ┌─> run all microtasks (promise callbacks, queueMicrotask)
//   │   report unhandled promise rejections
//   │   no timers and no open sockets?  -> exit
//   │   epoll_wait: sleep until a socket is ready or the next timer is due
//   │   handle socket events (draining microtasks after each one)
//   └── fire due timers (draining microtasks after each one)

bool Runtime::run_event_loop() {
    stop_requested_ = false;
    while (true) {
        if (!drain_microtasks() && errors_fatal_) return false;
        if (!report_unhandled_rejections() && errors_fatal_) return false;
        run_deferred();
        if (stop_requested_) return true;

        // Drop queue entries for timers that were cleared.
        while (!timer_queue_.empty() && !timers_.contains(timer_queue_.top().id) &&
               !native_timers_.contains(timer_queue_.top().id)) {
            timer_queue_.pop();
        }
        bool has_timers = !timer_queue_.empty();
        if (!has_timers && io_handlers_.empty() && refs_ == 0) return true;

        int timeout_ms = -1;  // no timers: wait for I/O forever
        if (has_timers) {
            auto wait = timer_queue_.top().due - Clock::now();
            auto ms = std::chrono::ceil<std::chrono::milliseconds>(wait).count();
            timeout_ms = static_cast<int>(std::max<int64_t>(0, ms));
        }
        if (!poll_io(timeout_ms) && errors_fatal_) return false;
        if (!run_due_timers() && errors_fatal_) return false;
    }
}

bool Runtime::poll_io(int timeout_ms) {
    epoll_event events[128];
    int n = epoll_wait(epoll_fd_, events, 128, timeout_ms);
    if (n < 0) return errno == EINTR;
    for (int i = 0; i < n; ++i) {
        // Look the handler up by id: an earlier event in this batch may have closed it.
        auto it = io_handlers_.find(events[i].data.u64);
        if (it == io_handlers_.end()) continue;
        it->second->on_io(events[i].events);
        if (!drain_microtasks() && errors_fatal_) return false;
        run_deferred();
    }
    return true;
}

uint64_t Runtime::add_io(int fd, uint32_t events, IoHandler* handler) {
    uint64_t id = next_io_id_++;
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = id;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) return 0;
    io_handlers_[id] = handler;
    return id;
}

void Runtime::modify_io(uint64_t id, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = id;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void Runtime::remove_io(uint64_t id, int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    io_handlers_.erase(id);
}

void Runtime::run_deferred() {
    while (!deferred_.empty()) {
        auto fns = std::move(deferred_);
        deferred_.clear();
        for (auto& fn : fns) fn();
    }
}

// Same order as Node: all nextTick callbacks, then all promise jobs, repeat.
bool Runtime::drain_microtasks(bool jobs_first) {
    JSContext* job_ctx = nullptr;
    if (jobs_first) {
        while (true) {
            int r = JS_ExecutePendingJob(rt_, &job_ctx);
            if (r == 0) break;
            if (r < 0) {
                dump_pending_exception(job_ctx);
                return false;
            }
        }
    }
    do {
        for (size_t i = 0; i < ticks_.size(); ++i) {  // callbacks may queue more ticks
            Timer tick = std::move(ticks_[i]);
            JSValue r = JS_Call(ctx_, tick.callback, JS_UNDEFINED, static_cast<int>(tick.args.size()), tick.args.data());
            bool ok = !JS_IsException(r);
            if (!ok) dump_pending_exception(ctx_);
            JS_FreeValue(ctx_, r);
            free_timer(tick);
            if (!ok) {
                for (size_t j = i + 1; j < ticks_.size(); ++j) free_timer(ticks_[j]);
                ticks_.clear();
                return false;
            }
        }
        ticks_.clear();
        while (true) {
            int r = JS_ExecutePendingJob(rt_, &job_ctx);
            if (r == 0) break;
            if (r < 0) {
                dump_pending_exception(job_ctx);
                return false;
            }
        }
    } while (!ticks_.empty());
    return true;
}

bool Runtime::run_due_timers() {
    auto now = Clock::now();
    while (!timer_queue_.empty() && timer_queue_.top().due <= now) {
        TimerEntry entry = timer_queue_.top();
        timer_queue_.pop();
        if (auto nt = native_timers_.find(entry.id); nt != native_timers_.end()) {
            auto fn = std::move(nt->second);
            native_timers_.erase(nt);
            fn();
            if (!drain_microtasks() && errors_fatal_) return false;
            run_deferred();
            continue;
        }
        auto it = timers_.find(entry.id);
        if (it == timers_.end()) continue;  // cleared

        // Hold our own references: the callback may clear its own timer.
        Timer& t = it->second;
        JSValue cb = JS_DupValue(ctx_, t.callback);
        std::vector<JSValue> args;
        for (JSValue a : t.args) args.push_back(JS_DupValue(ctx_, a));

        if (t.interval_ms > 0) {
            timer_queue_.push({now + std::chrono::milliseconds(t.interval_ms), timer_seq_++, entry.id});
        } else {
            free_timer(t);
            timers_.erase(it);
        }

        JSValue ret = JS_Call(ctx_, cb, JS_UNDEFINED, static_cast<int>(args.size()), args.data());
        bool ok = !JS_IsException(ret);
        if (!ok) dump_pending_exception(ctx_);
        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, cb);
        for (JSValue a : args) JS_FreeValue(ctx_, a);

        bool drained = drain_microtasks();
        if ((!ok || !drained) && errors_fatal_) return false;
    }
    return true;
}

int64_t Runtime::add_timer(JSValue cb, std::vector<JSValue> args, int64_t delay_ms, bool repeat) {
    delay_ms = std::max<int64_t>(delay_ms, repeat ? 1 : 0);
    int64_t id = next_timer_id_++;
    timers_[id] = Timer{cb, std::move(args), repeat ? delay_ms : 0};
    timer_queue_.push({Clock::now() + std::chrono::milliseconds(delay_ms), timer_seq_++, id});
    return id;
}

int64_t Runtime::add_native_timer(int64_t delay_ms, std::function<void()> fn) {
    int64_t id = next_timer_id_++;
    native_timers_[id] = std::move(fn);
    timer_queue_.push({Clock::now() + std::chrono::milliseconds(delay_ms), timer_seq_++, id});
    return id;
}

void Runtime::clear_timer(int64_t id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) return;
    free_timer(it->second);
    timers_.erase(it);  // the stale queue entry is skipped later
}

void Runtime::free_timer(Timer& t) {
    JS_FreeValue(ctx_, t.callback);
    for (JSValue a : t.args) JS_FreeValue(ctx_, a);
    t.args.clear();
}

// ---------------------------------------------------------------------------
// Unhandled promise rejections
// ---------------------------------------------------------------------------

void Runtime::on_promise_rejection(JSContext* ctx, JSValueConst promise, JSValueConst reason,
                                   bool is_handled, void* opaque) {
    auto* self = static_cast<Runtime*>(opaque);
    auto& list = self->pending_rejections_;
    if (!is_handled) {
        list.emplace_back(JS_DupValue(ctx, promise), JS_DupValue(ctx, reason));
        return;
    }
    // A handler was attached later (e.g. `p.catch(...)` after the rejection).
    auto it = std::find_if(list.begin(), list.end(), [&](auto& pr) {
        return JS_VALUE_GET_PTR(pr.first) == JS_VALUE_GET_PTR(promise);
    });
    if (it != list.end()) {
        JS_FreeValue(ctx, it->first);
        JS_FreeValue(ctx, it->second);
        list.erase(it);
    }
}

bool Runtime::report_unhandled_rejections() {
    if (pending_rejections_.empty()) return true;
    for (auto& [p, reason] : pending_rejections_) {
        bool is_main = JS_VALUE_GET_PTR(p) == main_promise_;
        print_exception(ctx_, reason, is_main ? "Uncaught" : "Uncaught (in promise)");
        JS_FreeValue(ctx_, p);
        JS_FreeValue(ctx_, reason);
    }
    pending_rejections_.clear();
    return false;
}

}  // namespace rtn
