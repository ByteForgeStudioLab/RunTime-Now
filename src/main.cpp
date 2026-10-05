// rtn — RunTime-Now: a small JavaScript & TypeScript runtime written in C++.
//
//   rtn <file.js|file.ts> [args...]   run a file
//   rtn run <file>                     same thing
//   rtn -e "<code>" [args...]          run code from the command line
//   rtn strip <file.ts>                print the JavaScript produced from a .ts file
//   rtn upgrade [-r] [--check]         update rtn to the latest release (alias: update)
//   rtn                                REPL (or run stdin as a script when it's piped)

#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "quickjs.h"
#include "runtime.hpp"
#include "typescript/strip.hpp"
#include "upgrade.hpp"
#include "util.hpp"

namespace {

void print_usage() {
    std::printf(
        "RunTime-Now v%s — a JavaScript & TypeScript runtime (C++ / QuickJS-ng)\n"
        "\n"
        "Usage:\n"
        "  rtn <file> [args...]        Run a .js / .mjs / .ts / .mts file\n"
        "  rtn run <file> [args...]    Same as above\n"
        "  rtn -e \"<code>\" [args...]   Evaluate code\n"
        "  rtn strip <file.ts>         Print the JavaScript made from a TypeScript file\n"
        "  rtn upgrade                 Upgrade rtn to the latest release (alias: rtn update -r)\n"
        "  rtn                         Start the REPL (runs stdin as a script if it's piped)\n"
        "  rtn -i                      Force the REPL even when stdin is piped\n"
        "  rtn -                       Run a script read from stdin\n"
        "\n"
        "Options:\n"
        "  -h, --help                  Show this help\n"
        "  -v, --version               Show version\n"
        "\n"
        "Built-in modules:\n"
        "  node:fs, node:fs/promises, node:path   (also rtn:..., or without a prefix)\n"
        "\n"
        "Globals:\n"
        "  fetch, Request, Response, Headers, URL, AbortController, EventTarget,\n"
        "  crypto, structuredClone, navigator, TextEncoder, TextDecoder, rtn.serve()\n"
        "\n"
        "Docs: https://github.com/%s#readme\n",
        RTN_VERSION, RTN_REPO);
}

// Absolute path of the running binary (for process.argv[0] / process.execPath).
std::string self_path(const char* argv0) {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return std::string(buf, static_cast<size_t>(n));
    return argv0;
}

// Builds process.argv: [rtn, ...rest]
std::vector<char*> make_argv(std::string& self, char** begin, char** end) {
    std::vector<char*> v{self.data()};
    for (char** p = begin; p < end; ++p) v.push_back(*p);
    return v;
}

int run_repl_or_stdin(std::string& self, bool force_repl) {
    std::vector<char*> args{self.data()};
    rtn::Runtime runtime(static_cast<int>(args.size()), args.data());
    if (force_repl || isatty(STDIN_FILENO)) return runtime.repl();
    return runtime.run_stdin();
}

}  // namespace

int main(int argc, char** argv) {
    std::string self = self_path(argv[0]);
    if (argc < 2) return run_repl_or_stdin(self, false);

    std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") {
        print_usage();
        return 0;
    }
    if (cmd == "-v" || cmd == "--version") {
        std::printf("rtn %s (QuickJS-ng %s)\n", RTN_VERSION, JS_GetVersion());
        return 0;
    }
    if (cmd == "-i" || cmd == "--interactive") return run_repl_or_stdin(self, true);
    if (cmd == "upgrade" || cmd == "update") return rtn::run_upgrade(argc - 2, argv + 2, self);
    if (cmd == "-") {
        auto args = make_argv(self, argv + 2, argv + argc);
        rtn::Runtime runtime(static_cast<int>(args.size()), args.data());
        return runtime.run_stdin();
    }
    if (cmd == "-e" || cmd == "--eval") {
        if (argc < 3) {
            std::fprintf(stderr, "error: -e requires code\n");
            return 1;
        }
        auto args = make_argv(self, argv + 3, argv + argc);  // process.argv = [rtn, ...rest]
        rtn::Runtime runtime(static_cast<int>(args.size()), args.data());
        return runtime.run_code(argv[2]);
    }
    if (cmd == "strip") {
        if (argc < 3) {
            std::fprintf(stderr, "error: strip requires a file\n");
            return 1;
        }
        auto src = rtn::read_file(argv[2]);
        if (!src) {
            std::fprintf(stderr, "error: could not read '%s'\n", argv[2]);
            return 1;
        }
        rtn::ts::StripResult r = rtn::ts::strip_types(*src);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d:%d: error: %s\n", argv[2], r.line, r.column, r.error.c_str());
            return 1;
        }
        std::fwrite(r.code.data(), 1, r.code.size(), stdout);
        return 0;
    }

    int file_idx = (cmd == "run") ? 2 : 1;
    if (file_idx >= argc) {
        std::fprintf(stderr, "error: missing file name\n");
        return 1;
    }
    if (argv[file_idx][0] == '-') {
        std::fprintf(stderr, "error: unknown option '%s'\n\n", argv[file_idx]);
        print_usage();
        return 1;
    }

    // process.argv = [/abs/path/rtn, /abs/path/script, ...args]  (like Node)
    std::string script = std::filesystem::absolute(argv[file_idx]).lexically_normal().string();
    std::vector<char*> args{self.data(), script.data()};
    for (int i = file_idx + 1; i < argc; ++i) args.push_back(argv[i]);
    rtn::Runtime runtime(static_cast<int>(args.size()), args.data());
    return runtime.run_file(argv[file_idx]);
}
