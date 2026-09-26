// Interactive REPL.
//
// stdin is watched by the event loop like any socket, so timers, promises
// and even rtn.serve() keep working while the prompt waits for input.
// Each input is evaluated as an async script: `await` works at the top level.
// TypeScript syntax is accepted too (types are stripped first).

#include <cstdio>
#include <deque>
#include <string>
#include <sys/epoll.h>
#include <unistd.h>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "typescript/strip.hpp"
#include "util.hpp"

namespace rtn {

namespace {

class Repl;
Repl* g_repl = nullptr;  // one REPL per process

// True if brackets, strings, templates or comments are still open:
// the user is typing a multi-line statement.
bool is_incomplete(const std::string& code) {
    int depth = 0;
    std::vector<int> template_depth;  // depth at which each `${` was opened
    char quote = 0;
    bool in_template = false;
    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        char next = i + 1 < code.size() ? code[i + 1] : 0;
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            else if (c == '\n') return false;  // broken string: let the parser report it
            continue;
        }
        if (in_template) {
            if (c == '\\') ++i;
            else if (c == '`') in_template = false;
            else if (c == '$' && next == '{') {
                template_depth.push_back(++depth);
                in_template = false;
                ++i;
            }
            continue;
        }
        if (c == '/' && next == '/') {
            while (i < code.size() && code[i] != '\n') ++i;
        } else if (c == '/' && next == '*') {
            size_t e = code.find("*/", i + 2);
            if (e == std::string::npos) return true;
            i = e + 1;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '`') {
            in_template = true;
        } else if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            if (c == '}' && !template_depth.empty() && template_depth.back() == depth) {
                template_depth.pop_back();
                in_template = true;
            }
            --depth;
        }
    }
    return depth > 0 || in_template || quote;
}

class Repl : public IoHandler {
public:
    explicit Repl(Runtime& rt) : rt_(rt), ctx_(rt.ctx()) {
        colors_ = isatty(STDOUT_FILENO);
    }

    int run() {
        g_repl = this;
        on_ok_ = JS_NewCFunction(ctx_, js_on_ok, "onReplResult", 1);
        on_err_ = JS_NewCFunction(ctx_, js_on_err, "onReplError", 1);
        rt_.set_errors_fatal(false);

        if (isatty(STDIN_FILENO)) {
            std::printf("RunTime-Now v%s (QuickJS-ng %s)\n", RTN_VERSION, JS_GetVersion());
            std::printf("Type .help for help, .exit or Ctrl+D to quit.\n");
        }
        io_id_ = rt_.add_io(STDIN_FILENO, EPOLLIN, this);
        prompt();
        if (!io_id_) {  // stdin is a regular file: epoll can't watch it, read it all now
            char buf[65536];
            ssize_t n;
            while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0) feed(std::string_view(buf, static_cast<size_t>(n)));
            eof_ = true;
            pump();
        }
        rt_.run_event_loop();

        if (io_id_ && !done_) rt_.remove_io(io_id_, STDIN_FILENO);
        JS_FreeValue(ctx_, on_ok_);
        JS_FreeValue(ctx_, on_err_);
        g_repl = nullptr;
        return 0;
    }

    void on_io(uint32_t) override {
        char buf[4096];
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n > 0) {
            feed(std::string_view(buf, static_cast<size_t>(n)));
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            eof_ = true;
            rt_.remove_io(io_id_, STDIN_FILENO);
            io_id_ = 0;
        }
        pump();
    }

private:
    static JSValue js_on_ok(JSContext*, JSValueConst, int argc, JSValueConst* argv) {
        if (g_repl) g_repl->settled(true, argc > 0 ? argv[0] : JS_UNDEFINED);
        return JS_UNDEFINED;
    }
    static JSValue js_on_err(JSContext*, JSValueConst, int argc, JSValueConst* argv) {
        if (g_repl) g_repl->settled(false, argc > 0 ? argv[0] : JS_UNDEFINED);
        return JS_UNDEFINED;
    }

    void feed(std::string_view data) {
        for (char c : data) {
            if (c == '\n') {
                lines_.push_back(std::move(partial_));
                partial_.clear();
            } else if (c != '\r') {
                partial_ += c;
            }
        }
    }

    void pump() {
        while (!busy_ && !done_ && !lines_.empty()) {
            std::string line = std::move(lines_.front());
            lines_.pop_front();
            handle_line(line);
        }
        if (!busy_ && !done_ && eof_ && lines_.empty()) {
            if (!partial_.empty()) {  // last line without '\n'
                std::string line = std::move(partial_);
                partial_.clear();
                handle_line(line);
                if (busy_) return;
            }
            finish();
        }
    }

    void handle_line(const std::string& line) {
        if (buffer_.empty()) {
            std::string cmd = line;
            while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t')) cmd.pop_back();
            while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == '\t')) cmd.erase(0, 1);
            if (cmd == ".exit") {
                finish();
                return;
            }
            if (cmd == ".help") {
                std::printf(".exit     Exit the REPL (or press Ctrl+D)\n"
                            ".help     Show this help\n"
                            "_         The result of the last expression\n"
                            "Top-level await and TypeScript syntax are supported.\n");
                prompt();
                return;
            }
            if (cmd.empty()) {
                prompt();
                return;
            }
        }
        buffer_ += line;
        buffer_ += '\n';
        if (is_incomplete(buffer_)) {
            prompt(true);
            return;
        }
        std::string code = std::move(buffer_);
        buffer_.clear();
        eval(code);
        if (!busy_) prompt();
    }

    void eval(const std::string& code) {
        std::string js = code;
        ts::StripResult stripped = ts::strip_types(code);
        if (stripped.ok) js = std::move(stripped.code);

        JSValue promise = JS_Eval(ctx_, js.c_str(), js.size(), "<repl>",
                                  JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC | JS_EVAL_FLAG_BACKTRACE_BARRIER);
        if (JS_IsException(promise)) {  // syntax error
            dump_pending_exception(ctx_);
            return;
        }
        busy_ = true;
        rt_.ref();
        JSValue then = JS_GetPropertyStr(ctx_, promise, "then");
        JSValue args[] = {on_ok_, on_err_};
        JS_FreeValue(ctx_, JS_Call(ctx_, then, promise, 2, args));
        JS_FreeValue(ctx_, then);
        JS_FreeValue(ctx_, promise);
    }

    // The async eval resolves to { value } (or rejects with the thrown error).
    void settled(bool ok, JSValueConst result) {
        if (ok) {
            JSValue value = JS_GetPropertyStr(ctx_, result, "value");
            if (!JS_IsUndefined(value)) std::printf("%s\n", inspect(ctx_, value, colors_).c_str());
            JSValue global = JS_GetGlobalObject(ctx_);
            JS_SetPropertyStr(ctx_, global, "_", value);  // takes ownership of value
            JS_FreeValue(ctx_, global);
        } else {
            print_exception(ctx_, result);
        }
        std::fflush(stdout);
        busy_ = false;
        rt_.unref();
        if (done_) return;
        prompt();
        pump();
    }

    void prompt(bool continuation = false) {
        if (done_ || eof_ || !isatty(STDIN_FILENO)) return;
        std::fputs(continuation ? "... " : "> ", stdout);
        std::fflush(stdout);
    }

    void finish() {
        if (done_) return;
        done_ = true;
        if (io_id_) {
            rt_.remove_io(io_id_, STDIN_FILENO);
            io_id_ = 0;
        }
        if (isatty(STDIN_FILENO)) std::printf("\n");
        rt_.request_stop();
    }

    Runtime& rt_;
    JSContext* ctx_;
    bool colors_ = false;
    uint64_t io_id_ = 0;
    JSValue on_ok_ = JS_UNDEFINED;
    JSValue on_err_ = JS_UNDEFINED;

    std::deque<std::string> lines_;
    std::string partial_;  // bytes after the last '\n'
    std::string buffer_;   // a multi-line statement being typed
    bool busy_ = false;    // waiting for an evaluation to finish
    bool eof_ = false;
    bool done_ = false;
};

}  // namespace

int Runtime::repl() {
    Repl r(*this);
    return r.run();
}

}  // namespace rtn
