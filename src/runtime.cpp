#include "runtime.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <csignal>
#include <iostream>
#include <mutex>
#include <system_error>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

#include "bindings/bindings.hpp"
#include "modules.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace rtn {

namespace {
// Same default as libuv's thread pool.
constexpr int kMaxWorkerThreads = 4;
// epoll data for the thread pool's eventfd; ids from add_io() start at 1.
constexpr uint64_t kWorkPoolIoId = 0;
}  // namespace

// State shared between the event loop and the worker threads. Workers are
// detached and hold a shared_ptr, so a lookup that's still running when the
// process ends can't touch freed memory (or write to a reused fd).
struct Runtime::WorkPool {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<uint64_t, std::function<void()>>> queue;
    std::vector<uint64_t> finished;  // ids whose `work` has returned
    int event_fd = -1;               // written by workers to wake up epoll
    int threads = 0;
    int idle = 0;
    bool stopping = false;

    ~WorkPool() {
        if (event_fd >= 0) close(event_fd);
    }

    static void worker(std::shared_ptr<WorkPool> pool) {
        std::unique_lock lock(pool->mu);
        while (true) {
            ++pool->idle;
            pool->cv.wait(lock, [&] { return pool->stopping || !pool->queue.empty(); });
            --pool->idle;
            if (pool->stopping) return;
            auto [id, work] = std::move(pool->queue.front());
            pool->queue.pop_front();
            lock.unlock();
            work();
            work = nullptr;  // free captures outside the lock
            lock.lock();
            pool->finished.push_back(id);
            uint64_t one = 1;
            [[maybe_unused]] ssize_t n = write(pool->event_fd, &one, sizeof one);
        }
    }
};

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
    // Work still running in the pool: let its owner free what it holds.
    auto pending = std::move(work_done_);
    work_done_.clear();
    for (auto& [id, done] : pending) done(true);
    pending.clear();
    if (pool_) {
        std::lock_guard lock(pool_->mu);
        pool_->stopping = true;
        pool_->cv.notify_all();
    }
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
    if (is_commonjs_file(ctx_, abs.string())) {
        // A CommonJS entry point: a tiny ES module imports it, which runs it through require().
        JSValue p = JS_NewString(ctx_, abs.c_str());
        JS_FreeValue(ctx_, call_internal(ctx_, "setMainPath", 1, &p));
        JS_FreeValue(ctx_, p);
        std::string quoted = abs.string();
        for (size_t i = 0; i < quoted.size(); ++i) {
            if (quoted[i] == '\\' || quoted[i] == '"') quoted.insert(i++, 1, '\\');
        }
        return eval_module("import \"" + quoted + "\";\n", (abs.parent_path() / "[main]").string());
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

    // process.on("exit", ...) listeners; they may still change process.exitCode.
    JSValue code_val = JS_NewInt32(ctx_, code_out);
    JSValue r = call_internal(ctx_, "emitExit", 1, &code_val);
    if (JS_IsException(r)) {
        dump_pending_exception(ctx_);
        code_out = code_out ? code_out : 1;
    }
    JS_FreeValue(ctx_, r);
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
        bool timers_keep_alive = timers_.size() + native_timers_.size() > unref_timers_.size();
        if (!timers_keep_alive && io_handlers_.empty() && work_done_.empty() && refs_ == 0) return true;

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
        if (events[i].data.u64 == kWorkPoolIoId) {
            if (!run_finished_work() && errors_fatal_) return false;
            continue;
        }
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

void Runtime::queue_work(std::function<void()> work, std::function<void(bool)> done) {
    if (!pool_) {
        pool_ = std::make_shared<WorkPool>();
        pool_->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = kWorkPoolIoId;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, pool_->event_fd, &ev);
    }
    uint64_t id = next_work_id_++;
    work_done_[id] = std::move(done);
    std::unique_lock lock(pool_->mu);
    pool_->queue.emplace_back(id, std::move(work));
    if (pool_->queue.size() > static_cast<size_t>(pool_->idle) && pool_->threads < kMaxWorkerThreads) {
        try {
            std::thread(WorkPool::worker, pool_).detach();
            ++pool_->threads;
        } catch (const std::system_error&) {
            if (pool_->threads == 0) {  // no thread at all: do the work right here
                auto job = std::move(pool_->queue.back());
                pool_->queue.pop_back();
                lock.unlock();
                job.second();
                lock.lock();
                pool_->finished.push_back(job.first);
                uint64_t one = 1;
                [[maybe_unused]] ssize_t n = write(pool_->event_fd, &one, sizeof one);
                return;
            }
        }
    }
    pool_->cv.notify_one();
}

bool Runtime::run_finished_work() {
    uint64_t count;
    [[maybe_unused]] ssize_t n = read(pool_->event_fd, &count, sizeof count);
    std::vector<uint64_t> ids;
    {
        std::lock_guard lock(pool_->mu);
        ids.swap(pool_->finished);
    }
    for (uint64_t id : ids) {
        auto it = work_done_.find(id);
        if (it == work_done_.end()) continue;
        auto done = std::move(it->second);
        work_done_.erase(it);
        done(false);
        if (!drain_microtasks() && errors_fatal_) return false;
        run_deferred();
    }
    return true;
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
            unref_timers_.erase(entry.id);
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
            unref_timers_.erase(entry.id);
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
    unref_timers_.erase(id);
}

void Runtime::unref_timer(int64_t id) {
    if (timers_.contains(id) || native_timers_.contains(id)) unref_timers_.insert(id);
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
