// HTTP/1.1 server core: sockets, request parsing, response writing.
//
// JavaScript side (src/js/http.js) builds Request/Response objects on top of
// three native functions:
//   httpListen(hostname, port, onRequest) -> [serverId, port]
//   httpRespond(requestId, status, statusText, headers[], body)
//   httpClose(serverId)
//
// Every socket is non-blocking and watched by the runtime's epoll loop.
// A connection handles one request at a time: it parses a request, hands it
// to JS, waits for httpRespond(), writes the response, then parses the next
// one (keep-alive / pipelining).

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "util.hpp"

namespace rtn {

namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 64 * 1024 * 1024;
constexpr size_t kReadChunk = 64 * 1024;
// Defaults; rtn.serve({ keepAliveTimeout, requestTimeout }) can change them.
// An idle keep-alive connection is closed after this (Node uses 5s too).
constexpr int64_t kDefaultKeepAliveMs = 5000;
// A request must arrive completely within this time (slowloris protection).
constexpr int64_t kDefaultRequestMs = 60000;

struct Server;
struct Conn;

// Everything lives in one runtime per process, so plain statics are enough.
std::unordered_map<uint64_t, Server*> g_servers;
std::unordered_map<uint64_t, Conn*> g_requests;  // request id -> connection waiting for a response
uint64_t g_next_server_id = 1;
uint64_t g_next_request_id = 1;
bool g_shutdown_hook_installed = false;
int g_spare_fd = -1;  // released when we run out of fds, to accept + reject a client

const char* reason_phrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 418: return "I'm a Teapot";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return "";
    }
}

// "Date: Thu, 25 Sep 2026 12:00:00 GMT" — regenerated at most once per second.
const std::string& http_date() {
    static std::string cached;
    static time_t cached_at = 0;
    time_t now = time(nullptr);
    if (now != cached_at) {
        char buf[64];
        tm t{};
        gmtime_r(&now, &t);
        strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &t);
        cached = buf;
        cached_at = now;
    }
    return cached;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool has_token(std::string_view list, std::string_view token) {  // "keep-alive, Upgrade"
    size_t p = 0;
    while (p <= list.size()) {
        size_t e = list.find(',', p);
        if (e == std::string_view::npos) e = list.size();
        std::string_view item = list.substr(p, e - p);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (ieq(item, token)) return true;
        p = e + 1;
    }
    return false;
}

bool is_tchar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("!#$%&'*+-.^_`|~", c);
}

// Host header -> safe to put in a URL? (letters, digits, - . : [ ] _)
bool valid_host(std::string_view h) {
    if (h.empty() || h.size() > 255) return false;
    for (char c : h) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && !std::strchr("-.:[]_", c)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Request parser
// ---------------------------------------------------------------------------

struct Request {
    std::string method;
    std::string target;
    bool http10 = false;
    std::vector<std::pair<std::string, std::string>> headers;  // names lower-cased
    std::string body;
    bool has_body = false;
    bool keep_alive = true;
    bool expect_continue = false;
};

enum class Parse { Incomplete, Done, Error };

struct ParseResult {
    Parse state;
    size_t consumed = 0;  // bytes of the input used by this request
    int error_status = 400;
    bool chunked = false;  // incomplete because a chunked body is still arriving
};

// Decodes a chunked body starting at `p`. Returns bytes used, 0 if incomplete, SIZE_MAX on error.
size_t decode_chunked(std::string_view in, size_t p, std::string& out) {
    while (true) {
        size_t eol = in.find("\r\n", p);
        if (eol == std::string_view::npos) return 0;
        std::string_view line = in.substr(p, eol - p);
        size_t semi = line.find(';');  // chunk extensions are ignored
        if (semi != std::string_view::npos) line = line.substr(0, semi);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (line.empty() || line.size() > 15) return SIZE_MAX;
        size_t size = 0;
        for (char c : line) {
            int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return SIZE_MAX;
            size = size * 16 + static_cast<size_t>(d);
        }
        p = eol + 2;
        if (size == 0) {  // last chunk, then optional trailers, then an empty line
            while (true) {
                size_t e = in.find("\r\n", p);
                if (e == std::string_view::npos) return 0;
                if (e == p) return e + 2;
                p = e + 2;
            }
        }
        if (out.size() + size > kMaxBodyBytes) return SIZE_MAX - 1;
        if (in.size() < p + size + 2) return 0;
        if (in.substr(p + size, 2) != "\r\n") return SIZE_MAX;
        out.append(in.substr(p, size));
        p += size + 2;
    }
}

ParseResult parse_request(std::string_view in, Request& req) {
    size_t head_end = in.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        if (in.size() > kMaxHeaderBytes) return {Parse::Error, 0, 431};
        return {Parse::Incomplete};
    }
    if (head_end > kMaxHeaderBytes) return {Parse::Error, 0, 431};

    // Request line: METHOD SP target SP HTTP/1.x
    size_t line_end = in.find("\r\n");
    std::string_view line = in.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == sp1) return {Parse::Error};
    std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);
    if (method.empty() || target.empty()) return {Parse::Error};
    for (char c : method) {
        if (!is_tchar(c)) return {Parse::Error};
    }
    for (char c : target) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return {Parse::Error};
    }
    if (version == "HTTP/1.1") req.http10 = false;
    else if (version == "HTTP/1.0") req.http10 = true;
    else return {Parse::Error, 0, version.starts_with("HTTP/") ? 505 : 400};
    req.method = method;
    req.target = target;
    req.keep_alive = !req.http10;

    // Headers
    bool chunked = false;
    bool has_length = false;
    size_t content_length = 0;
    size_t p = line_end + 2;
    while (p < head_end + 2) {
        size_t e = in.find("\r\n", p);
        std::string_view h = in.substr(p, e - p);
        p = e + 2;
        if (h.empty()) break;
        if (h[0] == ' ' || h[0] == '\t') return {Parse::Error};  // obsolete line folding
        size_t colon = h.find(':');
        if (colon == std::string_view::npos || colon == 0) return {Parse::Error};
        std::string name(h.substr(0, colon));
        for (char& c : name) {
            if (!is_tchar(c)) return {Parse::Error};
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string_view value = h.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        for (char c : value) {
            if (c == '\0' || c == '\r' || c == '\n') return {Parse::Error};
        }

        if (name == "content-length") {
            size_t n = 0;
            if (value.empty() || value.size() > 18) return {Parse::Error};
            for (char c : value) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return {Parse::Error};
                n = n * 10 + static_cast<size_t>(c - '0');
            }
            if (has_length && n != content_length) return {Parse::Error};  // request smuggling guard
            has_length = true;
            content_length = n;
        } else if (name == "transfer-encoding") {
            if (!ieq(value, "chunked")) return {Parse::Error, 0, 501};
            chunked = true;
        } else if (name == "connection") {
            if (has_token(value, "close")) req.keep_alive = false;
            else if (has_token(value, "keep-alive")) req.keep_alive = true;
        } else if (name == "expect") {
            if (ieq(value, "100-continue")) req.expect_continue = true;
        }
        req.headers.emplace_back(std::move(name), std::string(value));
    }
    if (chunked && has_length) return {Parse::Error};

    // Body
    size_t body_start = head_end + 4;
    if (chunked) {
        req.body.clear();
        size_t used = decode_chunked(in, body_start, req.body);
        if (used == SIZE_MAX) return {Parse::Error};
        if (used == SIZE_MAX - 1) return {Parse::Error, 0, 413};
        if (used == 0) return {Parse::Incomplete, 0, 400, true};
        req.has_body = true;
        return {Parse::Done, used};
    }
    if (has_length) {
        if (content_length > kMaxBodyBytes) return {Parse::Error, 0, 413};
        if (in.size() < body_start + content_length) return {Parse::Incomplete};
        req.body.assign(in.substr(body_start, content_length));
        req.has_body = content_length > 0;
        return {Parse::Done, body_start + content_length};
    }
    return {Parse::Done, body_start};
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

struct Server : IoHandler {
    Runtime* rt;
    JSContext* ctx;
    uint64_t id;
    int fd;
    uint64_t io_id = 0;
    JSValue on_request;
    std::unordered_set<Conn*> conns;
    bool closed = false;
    bool delete_scheduled = false;
    int64_t sweep_timer = 0;
    std::chrono::milliseconds keep_alive_timeout{kDefaultKeepAliveMs};
    std::chrono::milliseconds request_timeout{kDefaultRequestMs};

    Server(Runtime* r, uint64_t sid, int sock, JSValue cb)
        : rt(r), ctx(r->ctx()), id(sid), fd(sock), on_request(cb) {}

    void on_io(uint32_t events) override;
    void close();
    void schedule_sweep();
    void sweep();
    // A closed server is freed only after its last connection is gone.
    void maybe_delete() {
        if (!closed || !conns.empty() || delete_scheduled) return;
        delete_scheduled = true;
        if (sweep_timer) rt->cancel_native_timer(sweep_timer);
        Server* self = this;
        rt->defer([self] { delete self; });
    }
};

struct Conn : IoHandler {
    Server* server;
    int fd;
    uint64_t io_id = 0;
    std::string remote_ip;
    int remote_port = 0;

    std::string in;
    std::string out;
    size_t out_off = 0;
    bool watching_write = false;

    uint64_t pending_request = 0;  // waiting for JS to respond
    bool keep_alive = true;
    bool http10 = false;
    bool head_request = false;
    bool processing = false;       // inside process(): don't recurse, loop instead
    bool reprocess = false;
    Clock::time_point last_activity = Clock::now();
    Clock::time_point request_started{};  // first byte of the request being received
    bool receiving = false;
    size_t chunked_checked = 0;           // buffer size at the last failed chunked-body parse
    bool continue_sent = false;
    bool closing = false;      // close once `out` is flushed
    bool peer_closed = false;  // client finished sending (half-close): answer what we have, then close
    bool closed = false;

    Conn(Server* s, int sock) : server(s), fd(sock) {}

    void on_io(uint32_t events) override {
        if (closed) return;
        if (events & EPOLLIN) {
            if (!read_all()) return;
        } else if (events & (EPOLLERR | EPOLLHUP)) {
            close();
            return;
        }
        if (events & EPOLLOUT) flush();
    }

    // Returns false if the connection got closed.
    bool read_all() {
        char buf[kReadChunk];
        while (true) {
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n > 0) {
                last_activity = Clock::now();
                if (!receiving) {
                    receiving = true;
                    request_started = last_activity;
                }
                in.append(buf, static_cast<size_t>(n));
                if (in.size() > kMaxHeaderBytes + kMaxBodyBytes) {
                    send_error(413);
                    return !closed;
                }
                continue;
            }
            if (n == 0) {  // client shut down its sending side
                peer_closed = true;
                update_events();
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close();
            return false;
        }
        process();
        if (peer_closed && !closed && is_idle()) close();
        return !closed;
    }

    bool is_idle() const { return !pending_request && out_off >= out.size(); }

    void update_events() {
        uint32_t ev = peer_closed ? 0 : (EPOLLIN | EPOLLRDHUP);
        if (watching_write) ev |= EPOLLOUT;
        server->rt->modify_io(io_id, fd, ev);
    }

    // Parse the next request, if we're free to handle one. A handler may respond
    // synchronously (flush() calls process() again), so re-entry becomes a loop.
    void process() {
        if (processing) {
            reprocess = true;
            return;
        }
        processing = true;
        do {
            reprocess = false;
            process_one();
        } while (reprocess && !closed);
        processing = false;
    }

    void process_one() {
        if (closed || closing || pending_request || out_off < out.size()) return;
        if (server->closed) {  // stop() was called: no new requests
            close();
            return;
        }
        if (in.empty()) return;
        // Re-parsing a big chunked body on every read would be O(n^2): retry only
        // when the buffer ends like a finished message or has doubled in size.
        if (chunked_checked && !in.ends_with("\r\n\r\n") && in.size() < chunked_checked * 2) return;

        Request req;
        ParseResult r = parse_request(in, req);
        chunked_checked = 0;
        if (r.state == Parse::Incomplete) {
            if (r.chunked) chunked_checked = in.size();
            if (req.expect_continue && !continue_sent && !req.method.empty()) {
                continue_sent = true;
                out += "HTTP/1.1 100 Continue\r\n\r\n";
                flush();
            }
            return;
        }
        if (r.state == Parse::Error) {
            send_error(r.error_status);
            return;
        }
        in.erase(0, r.consumed);
        continue_sent = false;
        receiving = !in.empty();
        if (receiving) request_started = Clock::now();
        keep_alive = req.keep_alive && !server->closed;
        http10 = req.http10;
        head_request = req.method == "HEAD";
        dispatch(req);
    }

    void dispatch(Request& req) {
        JSContext* ctx = server->ctx;
        uint64_t rid = g_next_request_id++;
        pending_request = rid;
        g_requests[rid] = this;

        // rawHeaders: [[name, value], ...] (names already lower-cased)
        JSValue headers = JS_NewArray(ctx);
        uint32_t i = 0;
        for (auto& [k, v] : req.headers) {
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewStringLen(ctx, k.data(), k.size()));
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewStringLen(ctx, v.data(), v.size()));
            JS_SetPropertyUint32(ctx, headers, i++, pair);
        }
        JSValue body = req.has_body
            ? JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size())
            : JS_NULL;

        std::string_view host;
        for (auto& [k, v] : req.headers) {
            if (k == "host") {
                if (valid_host(v)) host = v;
                break;
            }
        }
        JSValue args[] = {
            JS_NewInt64(ctx, static_cast<int64_t>(rid)),
            JS_NewStringLen(ctx, req.method.data(), req.method.size()),
            JS_NewStringLen(ctx, req.target.data(), req.target.size()),
            headers,
            body,
            JS_NewString(ctx, remote_ip.c_str()),
            JS_NewInt32(ctx, remote_port),
            JS_NewStringLen(ctx, host.data(), host.size()),
        };
        // Hold our own reference: the handler may call server.stop(), which frees on_request.
        JSValue cb = JS_DupValue(ctx, server->on_request);
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 8, args);
        JS_FreeValue(ctx, cb);
        if (JS_IsException(ret)) {
            dump_pending_exception(ctx);
            if (!closed && pending_request == rid) respond(500, "", {}, "Internal Server Error");
        }
        JS_FreeValue(ctx, ret);
        for (JSValue a : args) JS_FreeValue(ctx, a);
    }

    void respond(int status, std::string_view status_text,
                 const std::vector<std::pair<std::string, std::string>>& headers, std::string_view body) {
        g_requests.erase(pending_request);
        pending_request = 0;

        std::string& o = out;
        o += "HTTP/1.1 ";
        o += std::to_string(status);
        o += ' ';
        o += status_text.empty() ? reason_phrase(status) : status_text;
        o += "\r\n";
        for (auto& [k, v] : headers) {
            if (k == "content-length" || k == "transfer-encoding" || k == "date") continue;
            if (k == "connection") {
                if (has_token(v, "close")) keep_alive = false;
                continue;
            }
            o += k;
            o += ": ";
            o += v;
            o += "\r\n";
        }
        bool no_body = status == 204 || status == 304 || (status >= 100 && status < 200);
        if (!no_body) {
            o += "content-length: ";
            o += std::to_string(body.size());
            o += "\r\n";
        }
        o += "date: ";
        o += http_date();
        o += "\r\n";
        if (!keep_alive) o += "connection: close\r\n";
        else if (http10) o += "connection: keep-alive\r\n";
        o += "\r\n";
        if (!no_body && !head_request) o += body;

        if (!keep_alive) closing = true;
        flush();
    }

    void send_error(int status) {
        keep_alive = false;
        head_request = false;
        in.clear();
        std::string msg = std::to_string(status) + " " + reason_phrase(status);
        respond(status, "", {{"content-type", "text/plain;charset=UTF-8"}}, msg);
    }

    void flush() {
        while (out_off < out.size()) {
            ssize_t n = send(fd, out.data() + out_off, out.size() - out_off, MSG_NOSIGNAL);
            if (n > 0) {
                out_off += static_cast<size_t>(n);
                last_activity = Clock::now();
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!watching_write) {
                    watching_write = true;
                    update_events();
                }
                return;
            }
            close();
            return;
        }
        out.clear();
        out_off = 0;
        if (watching_write) {
            watching_write = false;
            update_events();
        }
        if (closing) {
            close();
            return;
        }
        process();  // a pipelined request may already be waiting in `in`
        if (peer_closed && !closed && is_idle()) close();
    }

    void close() {
        if (closed) return;
        closed = true;
        if (pending_request) g_requests.erase(pending_request);
        server->rt->remove_io(io_id, fd);
        ::close(fd);
        Conn* self = this;
        server->rt->defer([self] { delete self; });
        server->conns.erase(this);
        server->maybe_delete();
    }
};

void Server::on_io(uint32_t) {
    while (!closed) {
        sockaddr_storage addr{};
        socklen_t len = sizeof addr;
        int cfd = accept4(fd, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if ((errno == EMFILE || errno == ENFILE) && g_spare_fd >= 0) {
                // Out of file descriptors: the pending client would make epoll wake us
                // forever (100% CPU). Free the spare fd, accept and close it, re-reserve.
                ::close(g_spare_fd);
                int victim = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (victim >= 0) ::close(victim);
                g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                continue;
            }
            return;  // EAGAIN: no more pending connections (or a transient error)
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        auto* c = new Conn(this, cfd);
        char ip[INET6_ADDRSTRLEN] = "";
        if (addr.ss_family == AF_INET) {
            auto* a = reinterpret_cast<sockaddr_in*>(&addr);
            inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
            c->remote_port = ntohs(a->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
            inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
            c->remote_port = ntohs(a->sin6_port);
        }
        c->remote_ip = ip;
        c->io_id = rt->add_io(cfd, EPOLLIN | EPOLLRDHUP, c);
        conns.insert(c);
    }
}

// Stop accepting; idle connections close now, busy ones after their response.
void Server::schedule_sweep() {
    auto interval = std::min(keep_alive_timeout, request_timeout) / 4;
    int64_t ms = std::clamp<int64_t>(interval.count(), 10, 1000);
    sweep_timer = rt->add_native_timer(ms, [this] {
        sweep_timer = 0;
        sweep();
        if (!closed || !conns.empty()) schedule_sweep();
    });
}

// Closes idle keep-alive connections and requests that take too long to arrive.
void Server::sweep() {
    auto now = Clock::now();
    std::vector<Conn*> list(conns.begin(), conns.end());
    for (Conn* c : list) {
        if (c->closed || c->pending_request || c->out_off < c->out.size()) continue;
        if (c->receiving && now - c->request_started > request_timeout) {
            c->send_error(408);
        } else if (!c->receiving && now - c->last_activity > keep_alive_timeout) {
            c->close();
        }
    }
}

void Server::close() {
    if (closed) return;
    closed = true;
    rt->remove_io(io_id, fd);
    ::close(fd);
    std::vector<Conn*> list(conns.begin(), conns.end());
    for (Conn* c : list) {
        if (c->pending_request || c->out_off < c->out.size()) {
            c->keep_alive = false;
            c->closing = c->pending_request == 0;
        } else {
            c->close();
        }
    }
    g_servers.erase(id);
    JS_FreeValue(ctx, on_request);
    on_request = JS_UNDEFINED;
    maybe_delete();
}

void shutdown_all() {
    std::vector<Server*> servers;
    for (auto& [id, s] : g_servers) servers.push_back(s);
    for (Server* s : servers) {
        std::vector<Conn*> list(s->conns.begin(), s->conns.end());
        for (Conn* c : list) c->close();
        s->close();
    }
}

// ---------------------------------------------------------------------------
// Natives
// ---------------------------------------------------------------------------

// httpListen(hostname, port, onRequest, keepAliveMs, requestMs) -> [serverId, actualPort]
JSValue js_http_listen(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3 || !JS_IsFunction(ctx, argv[2])) return JS_ThrowTypeError(ctx, "httpListen: bad arguments");
    std::string host = to_string(ctx, argv[0]);
    int32_t port = 0;
    if (JS_ToInt32(ctx, &port, argv[1]) < 0) return JS_EXCEPTION;
    if (port < 0 || port > 65535) return JS_ThrowRangeError(ctx, "port must be between 0 and 65535");

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res); rc != 0) {
        return throw_error(ctx, "Invalid hostname '" + host + "': " + gai_strerror(rc));
    }

    // "localhost" resolves to both ::1 and 127.0.0.1: prefer IPv4, which is what
    // most clients (curl, browsers) try first.
    std::vector<addrinfo*> candidates;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) candidates.push_back(ai);
    }
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) candidates.push_back(ai);
    }
    int fd = -1;
    int err = 0;
    for (addrinfo* ai : candidates) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            err = errno;
            continue;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, SOMAXCONN) == 0) break;
        err = errno;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return throw_errno(ctx, err, "listen", host + ":" + port_str);
    if (g_spare_fd < 0) g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);

    sockaddr_storage bound{};
    socklen_t len = sizeof bound;
    getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    int actual = bound.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port)
                                              : ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);

    Runtime* rt = Runtime::from(ctx);
    if (!g_shutdown_hook_installed) {
        g_shutdown_hook_installed = true;
        rt->on_shutdown(shutdown_all);
    }
    uint64_t sid = g_next_server_id++;
    auto* server = new Server(rt, sid, fd, JS_DupValue(ctx, argv[2]));
    int64_t keep_ms = kDefaultKeepAliveMs, req_ms = kDefaultRequestMs;
    if (argc > 3 && JS_IsNumber(argv[3])) JS_ToInt64(ctx, &keep_ms, argv[3]);
    if (argc > 4 && JS_IsNumber(argv[4])) JS_ToInt64(ctx, &req_ms, argv[4]);
    server->keep_alive_timeout = std::chrono::milliseconds(std::max<int64_t>(keep_ms, 1));
    server->request_timeout = std::chrono::milliseconds(std::max<int64_t>(req_ms, 1));
    server->io_id = rt->add_io(fd, EPOLLIN, server);
    server->schedule_sweep();
    g_servers[sid] = server;

    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewInt64(ctx, static_cast<int64_t>(sid)));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewInt32(ctx, actual));
    return r;
}

bool strip_crlf_ok(const std::string& s) {
    return s.find_first_of("\r\n") == std::string::npos;
}

// httpRespond(requestId, status, statusText, headers: [[k, v], ...], body: string | Uint8Array)
JSValue js_http_respond(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "httpRespond: bad arguments");
    int64_t rid = 0;
    int32_t status = 200;
    JS_ToInt64(ctx, &rid, argv[0]);
    JS_ToInt32(ctx, &status, argv[1]);
    auto it = g_requests.find(static_cast<uint64_t>(rid));
    if (it == g_requests.end()) return JS_UNDEFINED;  // client already went away
    Conn* conn = it->second;
    if (status < 100 || status > 999) status = 500;  // e.g. Response.error()

    std::string status_text = to_string(ctx, argv[2]);
    if (!strip_crlf_ok(status_text)) status_text.clear();

    std::vector<std::pair<std::string, std::string>> headers;
    uint32_t n = 0;
    JSValue len = JS_GetPropertyStr(ctx, argv[3], "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (uint32_t i = 0; i < n; ++i) {
        JSValue pair = JS_GetPropertyUint32(ctx, argv[3], i);
        JSValue k = JS_GetPropertyUint32(ctx, pair, 0);
        JSValue v = JS_GetPropertyUint32(ctx, pair, 1);
        std::string ks = to_string(ctx, k), vs = to_string(ctx, v);
        if (strip_crlf_ok(ks) && strip_crlf_ok(vs)) headers.emplace_back(std::move(ks), std::move(vs));
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, pair);
    }

    size_t size = 0;
    uint8_t* bytes = nullptr;
    // Check the type first: a failed JS_GetUint8Array builds an Error object (slow).
    if (!JS_IsString(argv[4]) && JS_GetTypedArrayType(argv[4]) >= 0) {
        bytes = JS_GetUint8Array(ctx, &size, argv[4]);
        if (!bytes) JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (bytes) {
        conn->respond(status, status_text, headers, std::string_view(reinterpret_cast<char*>(bytes), size));
    } else {
        const char* s = JS_ToCStringLen(ctx, &size, argv[4]);
        if (!s) return JS_EXCEPTION;
        conn->respond(status, status_text, headers, std::string_view(s, size));
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

JSValue js_http_close(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int64_t sid = 0;
    if (argc > 0) JS_ToInt64(ctx, &sid, argv[0]);
    auto it = g_servers.find(static_cast<uint64_t>(sid));
    if (it != g_servers.end()) it->second->close();
    return JS_UNDEFINED;
}

const JSCFunctionListEntry kHttpFuncs[] = {
    JS_CFUNC_DEF("httpListen", 3, js_http_listen),
    JS_CFUNC_DEF("httpRespond", 5, js_http_respond),
    JS_CFUNC_DEF("httpClose", 1, js_http_close),
};

}  // namespace

void add_http_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyFunctionList(ctx, native, kHttpFuncs, sizeof(kHttpFuncs) / sizeof(kHttpFuncs[0]));
}

}  // namespace rtn
