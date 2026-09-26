// loadgen — a tiny HTTP/1.1 keep-alive load generator (like wrk, much simpler).
//
//   ./build/loadgen <host> <port> [path=/] [seconds=5] [connections=64] [threads=4]
//
// Each connection sends one GET, waits for the full response
// (Content-Length based), then sends the next one.

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Conn {
    int fd = -1;
    std::string in;
    Clock::time_point sent_at;
};

struct Result {
    uint64_t ok = 0;
    uint64_t errors = 0;
    std::vector<uint32_t> latency_us;
};

static bool send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Returns the size of one complete response at the start of `in`, or 0.
static size_t complete_response(const std::string& in) {
    size_t head = in.find("\r\n\r\n");
    if (head == std::string::npos) return 0;
    size_t len = 0;
    size_t p = 0;
    while ((p = in.find('\n', p)) != std::string::npos && p < head) {
        ++p;
        if (strncasecmp(in.c_str() + p, "content-length:", 15) == 0) {
            len = std::strtoul(in.c_str() + p + 15, nullptr, 10);
            break;
        }
    }
    size_t total = head + 4 + len;
    return in.size() >= total ? total : 0;
}

static void worker(sockaddr_in addr, std::string request, int conns, Clock::time_point end, Result& res) {
    int ep = epoll_create1(0);
    std::vector<Conn> cs(static_cast<size_t>(conns));
    for (int i = 0; i < conns; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            std::perror("connect");
            std::exit(1);
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        cs[i].fd = fd;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u32 = static_cast<uint32_t>(i);
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
        cs[i].sent_at = Clock::now();
        send_all(fd, request);
    }
    res.latency_us.reserve(1 << 20);
    char buf[65536];
    epoll_event events[256];
    while (Clock::now() < end) {
        int n = epoll_wait(ep, events, 256, 100);
        for (int e = 0; e < n; ++e) {
            Conn& c = cs[events[e].data.u32];
            ssize_t r = recv(c.fd, buf, sizeof buf, 0);
            if (r <= 0) {
                ++res.errors;
                epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr);
                close(c.fd);
                continue;
            }
            c.in.append(buf, static_cast<size_t>(r));
            while (size_t used = complete_response(c.in)) {
                auto now = Clock::now();
                bool good = c.in.compare(0, 12, "HTTP/1.1 200") == 0;
                good ? ++res.ok : ++res.errors;
                res.latency_us.push_back(static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(now - c.sent_at).count()));
                c.in.erase(0, used);
                c.sent_at = now;
                if (!send_all(c.fd, request)) ++res.errors;
            }
        }
    }
    for (auto& c : cs) close(c.fd);
    close(ep);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <host> <port> [path=/] [seconds=5] [connections=64] [threads=4]\n", argv[0]);
        return 1;
    }
    const char* host = argv[1];
    int port = std::atoi(argv[2]);
    std::string path = argc > 3 ? argv[3] : "/";
    int seconds = argc > 4 ? std::atoi(argv[4]) : 5;
    int conns = argc > 5 ? std::atoi(argv[5]) : 64;
    int threads = argc > 6 ? std::atoi(argv[6]) : 4;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n";

    auto start = Clock::now();
    auto end = start + std::chrono::seconds(seconds);
    std::vector<Result> results(static_cast<size_t>(threads));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        int n = conns / threads + (t < conns % threads ? 1 : 0);
        pool.emplace_back(worker, addr, request, n, end, std::ref(results[t]));
    }
    for (auto& th : pool) th.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    uint64_t ok = 0, errors = 0;
    std::vector<uint32_t> lat;
    for (auto& r : results) {
        ok += r.ok;
        errors += r.errors;
        lat.insert(lat.end(), r.latency_us.begin(), r.latency_us.end());
    }
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat.empty() ? 0.0 : lat[static_cast<size_t>(p * (lat.size() - 1))] / 1000.0; };
    std::printf("%.0f req/s  ok=%llu errors=%llu  p50=%.2fms p99=%.2fms\n", ok / elapsed,
                static_cast<unsigned long long>(ok), static_cast<unsigned long long>(errors), pct(0.50), pct(0.99));
    return 0;
}
