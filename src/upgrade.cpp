// `rtn upgrade` (alias `rtn update`): replaces this binary with a release
// from GitHub, the same way `bun upgrade` works.
//
// Release layout (created by .github/workflows/release.yml):
//   <releases>/latest/download/VERSION              "1.6.0"
//   <releases>/download/v1.6.0/rtn-linux-x64.tar.gz  (contains rtn-linux-x64/rtn)
//   <releases>/download/v1.6.0/SHA256SUMS
// where <releases> = https://github.com/<RTN_REPO>/releases
// (override with the RTN_RELEASES_URL environment variable, e.g. for a mirror).
//
// Downloads use curl (or wget), extraction uses tar. The archive is verified
// against SHA256SUMS before anything is replaced, and the new binary is moved
// into place with an atomic rename().

#include "upgrade.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace rtn {

namespace {

// ---------------------------------------------------------------------------
// Terminal output
// ---------------------------------------------------------------------------

bool g_color = false;
const char* c(const char* code) { return g_color ? code : ""; }
#define BOLD c("\x1b[1m")
#define DIM c("\x1b[2m")
#define GREEN c("\x1b[32m")
#define RED c("\x1b[31m")
#define CYAN c("\x1b[36m")
#define RESET c("\x1b[0m")

int fail(const std::string& msg) {
    std::fprintf(stderr, "%serror%s: %s\n", RED, RESET, msg.c_str());
    return 1;
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), so we don't depend on sha256sum being installed
// ---------------------------------------------------------------------------

class Sha256 {
public:
    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            buf_[buf_len_++] = data[i];
            if (buf_len_ == 64) {
                block(buf_);
                buf_len_ = 0;
            }
        }
        total_ += len;
    }

    std::string hex() {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buf_len_ != 56) update(&zero, 1);
        for (int i = 7; i >= 0; --i) {
            uint8_t b = static_cast<uint8_t>(bits >> (i * 8));
            update(&b, 1);
        }
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h_[i]);
        return std::string(out, 64);
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], cc = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
            h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += cc; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

std::optional<std::string> sha256_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    Sha256 sha;
    std::array<char, 65536> buf;
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0) {
        sha.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(in.gcount()));
    }
    return sha.hex();
}

// ---------------------------------------------------------------------------
// Running helper programs (curl / wget / tar) without a shell
// ---------------------------------------------------------------------------

bool have(const char* prog) {
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (!dir.empty() && access((dir + "/" + prog).c_str(), X_OK) == 0) return true;
    }
    return false;
}

// Runs a program and waits. Returns its exit code (or -1). stdout is captured when `out` is set.
int run(const std::vector<std::string>& args, std::string* out = nullptr, bool quiet_stderr = false) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int pipefd[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out) {
        if (pipe(pipefd) != 0) return -1;
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    }
    if (quiet_stderr) posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (out) close(pipefd[1]);
    if (rc != 0) {
        if (out) close(pipefd[0]);
        return -1;
    }
    if (out) {
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out->append(buf, static_cast<size_t>(n));
        close(pipefd[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Downloads url -> file with curl, falling back to wget. Returns false on any HTTP/network error.
bool download(const std::string& url, const std::string& file) {
    if (have("curl")) return run({"curl", "-fsSL", "--retry", "2", "-o", file, url}, nullptr, true) == 0;
    if (have("wget")) return run({"wget", "-q", "-O", file, url}, nullptr, true) == 0;
    return false;
}

std::optional<std::string> fetch_text(const std::string& url, const std::string& tmp_file) {
    if (!download(url, tmp_file)) return std::nullopt;
    std::ifstream in(tmp_file);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string strip_v(std::string v) {
    v = trim(v);
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    return v;
}

// "1.5.0" -> {1, 5, 0}; nullopt if it isn't a plain x.y.z version.
std::optional<std::array<int, 3>> parse_version(const std::string& v) {
    std::array<int, 3> parts{};
    int idx = 0;
    std::string cur;
    for (char ch : v + ".") {
        if (ch == '.') {
            if (cur.empty() || idx > 2) return std::nullopt;
            parts[idx++] = std::atoi(cur.c_str());
            cur.clear();
        } else if (std::isdigit(static_cast<unsigned char>(ch))) {
            cur += ch;
        } else {
            return std::nullopt;
        }
    }
    if (idx != 3) return std::nullopt;
    return parts;
}

// ---------------------------------------------------------------------------
// Platform
// ---------------------------------------------------------------------------

std::optional<std::string> platform_asset() {
    utsname u{};
    if (uname(&u) != 0) return std::nullopt;
    std::string os = u.sysname, machine = u.machine;
    if (os != "Linux") return std::nullopt;
    if (machine == "x86_64" || machine == "amd64") return std::string("linux-x64");
    if (machine == "aarch64" || machine == "arm64") return std::string("linux-arm64");
    return std::nullopt;
}

// A binary that sits in a CMake build directory was built from source:
// replacing it would surprise a developer.
bool is_source_build(const fs::path& exe) {
    std::error_code ec;
    return fs::exists(exe.parent_path() / "CMakeCache.txt", ec) ||
           fs::exists(exe.parent_path().parent_path() / "CMakeCache.txt", ec);
}

void print_help() {
    std::printf(
        "Usage: rtn upgrade [options]      (alias: rtn update)\n"
        "\n"
        "Upgrade rtn to the latest release from GitHub.\n"
        "\n"
        "Options:\n"
        "  -r, --release          Upgrade to the latest stable release (default)\n"
        "  --version <x.y.z>      Install a specific version (upgrade or downgrade)\n"
        "  --check                Only check whether a newer version exists\n"
        "  --force                Reinstall even if already up to date\n"
        "  -h, --help             Show this help\n"
        "\n"
        "Environment:\n"
        "  RTN_RELEASES_URL       Where to download releases from\n"
        "                         (default: https://github.com/%s/releases)\n",
        RTN_REPO);
}

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/rtn-upgrade-XXXXXX";
        if (mkdtemp(tmpl)) path = tmpl;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

}  // namespace

int run_upgrade(int argc, char** argv, const std::string& self_path) {
    g_color = isatty(STDOUT_FILENO) && isatty(STDERR_FILENO);

    bool check_only = false, force = false;
    std::string wanted;  // empty = latest
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_help();
            return 0;
        } else if (a == "-r" || a == "--release" || a == "--stable") {
            wanted.clear();
        } else if (a == "--check") {
            check_only = true;
        } else if (a == "--force" || a == "-f") {
            force = true;
        } else if ((a == "--version" || a == "--to") && i + 1 < argc) {
            wanted = strip_v(argv[++i]);
        } else if (a.rfind("--version=", 0) == 0) {
            wanted = strip_v(a.substr(10));
        } else if (parse_version(strip_v(a))) {
            wanted = strip_v(a);  // rtn upgrade 1.6.0
        } else {
            return fail("unknown option '" + a + "' (see: rtn upgrade --help)");
        }
    }
    if (!wanted.empty() && !parse_version(wanted)) return fail("'" + wanted + "' is not a version like 1.5.0");

    auto asset = platform_asset();
    if (!asset) return fail("prebuilt binaries are only available for Linux x64 and arm64 (build from source instead)");
    if (!have("curl") && !have("wget")) return fail("curl or wget is required to download releases");
    if (!have("tar")) return fail("tar is required to unpack releases");

    const char* env_url = std::getenv("RTN_RELEASES_URL");
    std::string releases = env_url && *env_url ? env_url : std::string("https://github.com/") + RTN_REPO + "/releases";
    while (!releases.empty() && releases.back() == '/') releases.pop_back();

    TempDir tmp;
    if (tmp.path.empty()) return fail("could not create a temporary directory");

    // 1. Which version?
    std::string current = RTN_VERSION;
    std::string target = wanted;
    if (target.empty()) {
        auto latest = fetch_text(releases + "/latest/download/VERSION", tmp.path + "/VERSION");
        if (!latest || !parse_version(strip_v(*latest))) {
            return fail("could not find the latest release at " + releases + " (check your internet connection)");
        }
        target = strip_v(*latest);
    }
    auto cur_v = parse_version(current);
    auto tgt_v = parse_version(target);

    if (check_only) {
        if (wanted.empty() && cur_v && tgt_v && *tgt_v > *cur_v) {
            std::printf("A new version of rtn is available: %s%s%s → %s%s%s\nRun %srtn upgrade%s to install it.\n",
                        DIM, current.c_str(), RESET, GREEN, target.c_str(), RESET, CYAN, RESET);
        } else {
            std::printf("rtn %s is up to date (latest release: %s).\n", current.c_str(), target.c_str());
        }
        return 0;
    }
    if (!force && target == current) {
        std::printf("%srtn is already on version %s%s%s — nothing to do.\n", GREEN, BOLD, current.c_str(), RESET);
        return 0;
    }
    if (!force && wanted.empty() && cur_v && tgt_v && *tgt_v < *cur_v) {
        std::printf("You're on rtn %s, which is newer than the latest release (%s). Nothing to do.\n",
                    current.c_str(), target.c_str());
        return 0;
    }

    fs::path exe = self_path;
    if (!force && is_source_build(exe)) {
        return fail("this rtn was built from source (" + exe.string() + ").\n"
                    "       Update it with: git pull && cmake --build build   (or pass --force)");
    }

    // 2. Download and verify.
    std::string name = "rtn-" + *asset;
    std::string base = releases + "/download/v" + target;
    std::printf("%sUpgrading rtn%s %s → %s%s%s %s(%s)%s\n", BOLD, RESET, current.c_str(), GREEN, target.c_str(), RESET,
                DIM, asset->c_str(), RESET);
    std::printf("  %sdownloading%s %s/%s.tar.gz\n", DIM, RESET, base.c_str(), name.c_str());
    std::string archive = tmp.path + "/" + name + ".tar.gz";
    if (!download(base + "/" + name + ".tar.gz", archive)) {
        return fail("download failed: version " + target + " may not exist, or has no " + *asset + " build");
    }
    auto sums = fetch_text(base + "/SHA256SUMS", tmp.path + "/SHA256SUMS");
    if (!sums) return fail("could not download SHA256SUMS for version " + target);
    std::string expected;
    std::istringstream lines(*sums);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream parts(line);
        std::string hash, file;
        parts >> hash >> file;
        if (!file.empty() && file[0] == '*') file.erase(0, 1);
        if (file == name + ".tar.gz") expected = hash;
    }
    auto actual = sha256_file(archive);
    if (expected.empty() || !actual || *actual != expected) {
        return fail("checksum mismatch for " + name + ".tar.gz — the download is corrupted or was tampered with. "
                    "Nothing was changed.");
    }
    std::printf("  %sverified%s  sha256 %s…\n", DIM, RESET, actual->substr(0, 16).c_str());

    // 3. Unpack and smoke-test the new binary.
    if (run({"tar", "-xzf", archive, "-C", tmp.path}) != 0) return fail("could not unpack " + archive);
    std::string fresh = tmp.path + "/" + name + "/rtn";
    if (access(fresh.c_str(), F_OK) != 0) return fail("the archive does not contain " + name + "/rtn");
    chmod(fresh.c_str(), 0755);
    std::string out;
    if (run({fresh, "--version"}, &out) != 0 || out.find(target) == std::string::npos) {
        return fail("the downloaded binary does not run on this system (got: " + trim(out) + ")");
    }

    // 4. Swap it in: copy next to the current binary, then rename() over it (atomic).
    fs::path staged = exe.parent_path() / (".rtn-upgrade-" + std::to_string(getpid()));
    std::error_code ec;
    fs::copy_file(fresh, staged, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return fail("cannot write to " + exe.parent_path().string() + " (" + ec.message() + ").\n"
                    "       Try: sudo rtn upgrade");
    }
    chmod(staged.c_str(), 0755);
    if (std::rename(staged.c_str(), exe.c_str()) != 0) {
        std::string why = std::strerror(errno);
        fs::remove(staged, ec);
        return fail("cannot replace " + exe.string() + " (" + why + "). Try: sudo rtn upgrade");
    }

    std::printf("%s✓%s Upgraded to %srtn %s%s — %s\n", GREEN, RESET, BOLD, target.c_str(), RESET, exe.c_str());
    std::printf("  What's new: https://github.com/%s/releases/tag/v%s\n", RTN_REPO, target.c_str());
    return 0;
}

}  // namespace rtn
