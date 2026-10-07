// rtn --watch <file | test | run ...>: runs rtn again with the same arguments
// whenever a source file changes.
//
// The directory tree is watched with inotify (node_modules and hidden
// directories skipped). A change to a .js / .ts / .json / .env… file restarts
// the child after a short quiet period; Ctrl+C or SIGTERM stops both.

#include "watch.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "term.hpp"

namespace fs = std::filesystem;

namespace rtn {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kWatchMask = IN_CLOSE_WRITE | IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                IN_DELETE_SELF | IN_ONLYDIR;
constexpr size_t kMaxDirectories = 8192;
constexpr auto kQuietPeriod = std::chrono::milliseconds(100);
constexpr auto kStopGrace = std::chrono::seconds(2);

bool skip_directory(const std::string& name) {
    return name == "node_modules" || (!name.empty() && name[0] == '.');
}

// Files whose change restarts the program.
bool is_source(const std::string& name) {
    if (name.starts_with(".env")) return true;
    if (name.empty() || name[0] == '.' || name.ends_with("~")) return false;  // editor temp files
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (const char* e : {".js", ".mjs", ".cjs", ".ts", ".mts", ".cts", ".jsx", ".tsx", ".json"}) {
        if (ext == e) return true;
    }
    return false;
}

struct Watcher {
    int fd = -1;
    std::unordered_map<int, fs::path> dirs;  // watch descriptor -> directory

    void add_tree(const fs::path& root) {
        add(root);
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (dirs.size() >= kMaxDirectories) return;
            std::error_code e2;
            if (!it->is_directory(e2) || it->is_symlink(e2)) continue;
            if (skip_directory(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            add(it->path());
        }
    }

    void add(const fs::path& dir) {
        int wd = inotify_add_watch(fd, dir.c_str(), kWatchMask);
        if (wd >= 0) dirs[wd] = dir;
    }

    // Reads pending events; returns the first changed source file ("" if none).
    std::string read_events() {
        alignas(inotify_event) char buf[16384];
        std::string changed;
        while (true) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;
            for (char* p = buf; p < buf + n;) {
                auto* ev = reinterpret_cast<inotify_event*>(p);
                p += sizeof(inotify_event) + ev->len;
                auto dir = dirs.find(ev->wd);
                if (ev->mask & IN_IGNORED) {
                    if (dir != dirs.end()) dirs.erase(dir);
                    continue;
                }
                if (dir == dirs.end() || ev->len == 0) continue;
                std::string name = ev->name;
                fs::path full = dir->second / name;
                if (ev->mask & IN_ISDIR) {
                    if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && !skip_directory(name)) add_tree(full);
                    continue;
                }
                if (is_source(name) && changed.empty()) changed = full.string();
            }
        }
        return changed;
    }
};

std::string relative_to_cwd(const std::string& p) {
    std::error_code ec;
    auto rel = fs::relative(p, fs::current_path(ec), ec);
    return ec || rel.empty() || rel.string().starts_with("..") ? p : rel.string();
}

void say(const std::string& what, const std::string& detail) {
    std::fflush(stdout);
    std::fprintf(stderr, "%s●%s %s%s%s %s%s%s\n", term::accent(), term::reset(), term::bold(), what.c_str(),
                 term::reset(), term::dim(), detail.c_str(), term::reset());
    std::fflush(stderr);
}

pid_t start_child(const std::string& self, const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(self.c_str()));
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        execv(self.c_str(), argv.data());
        std::fprintf(stderr, "error: could not start %s: %s\n", self.c_str(), std::strerror(errno));
        _exit(127);
    }
    return pid;
}

// Waits for the child, sending SIGKILL if it outlives the grace period.
int stop_child(pid_t pid) {
    kill(pid, SIGTERM);
    auto deadline = Clock::now() + kStopGrace;
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return status;
        if (Clock::now() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::string describe_exit(int status) {
    if (WIFSIGNALED(status)) return std::string("program killed by ") + strsignal(WTERMSIG(status));
    int code = WEXITSTATUS(status);
    return code == 0 ? "program exited" : "program exited with code " + std::to_string(code);
}

}  // namespace

int run_watch(const std::string& self, const std::vector<std::string>& args) {
    term::init(isatty(STDERR_FILENO));
    Watcher w;
    w.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w.fd < 0) {
        std::fprintf(stderr, "error: --watch: inotify: %s\n", std::strerror(errno));
        return 1;
    }
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    w.add_tree(cwd);
    // An entry file outside the current directory: watch its directory too.
    for (auto& a : args) {
        if (a.empty() || a[0] == '-' || !fs::is_regular_file(a, ec)) continue;
        fs::path dir = fs::absolute(a, ec).parent_path();
        auto rel = fs::relative(dir, cwd, ec);
        if (ec || rel.string().starts_with("..")) w.add_tree(dir);
        break;
    }

    // Signals arrive through a signalfd, so we can stop the child before leaving.
    sigset_t mask;
    sigemptyset(&mask);
    for (int s : {SIGCHLD, SIGINT, SIGTERM, SIGHUP}) sigaddset(&mask, s);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    std::string shown;
    for (auto& a : args) shown += (shown.empty() ? "" : " ") + a;
    say("rtn --watch", shown + " · " + std::to_string(w.dirs.size()) + (w.dirs.size() == 1 ? " directory" : " directories"));

    pid_t child = start_child(self, args);
    bool running = child > 0;
    std::string pending;  // a changed file waiting for the quiet period to end
    Clock::time_point restart_at{};

    while (true) {
        pollfd fds[2] = {{w.fd, POLLIN, 0}, {sfd, POLLIN, 0}};
        int timeout = -1;
        if (!pending.empty()) {
            auto left = std::chrono::ceil<std::chrono::milliseconds>(restart_at - Clock::now()).count();
            timeout = static_cast<int>(std::max<int64_t>(0, left));
        }
        int n = poll(fds, 2, timeout);
        if (n < 0 && errno != EINTR) break;

        if (fds[1].revents & POLLIN) {
            signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
                int sig = static_cast<int>(si.ssi_signo);
                if (sig == SIGCHLD) {
                    int status = 0;
                    if (running && waitpid(child, &status, WNOHANG) == child) {
                        running = false;
                        say("rtn --watch", describe_exit(status) + " · waiting for changes…");
                    }
                    continue;
                }
                // Ctrl+C reaches the child too (same process group); SIGTERM / SIGHUP may not.
                if (running) {
                    if (sig != SIGINT) kill(child, sig);
                    stop_child(child);
                }
                return 128 + sig;
            }
        }
        if (fds[0].revents & POLLIN) {
            std::string changed = w.read_events();
            if (!changed.empty()) {
                if (pending.empty()) pending = changed;
                restart_at = Clock::now() + kQuietPeriod;  // editors write in several steps
            }
        }
        if (!pending.empty() && Clock::now() >= restart_at) {
            w.read_events();  // what arrived meanwhile belongs to the same restart
            if (running) stop_child(child);
            say("rtn --watch", "restarting · " + relative_to_cwd(pending) + " changed");
            pending.clear();
            child = start_child(self, args);
            running = child > 0;
        }
    }
    if (running) stop_child(child);
    return 1;
}

}  // namespace rtn
