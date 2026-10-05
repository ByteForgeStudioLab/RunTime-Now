// fetch() client core: DNS lookup, connect, send the request, read the response.
//
// JavaScript side (src/js/fetch.js) builds the request head, follows redirects
// and turns the result into a Response, using two native functions:
//   fetchStart(hostname, port, head, body, isHead, callback) -> id
//       callback(error, status, statusText, headers: [[name, value], ...], body)
//   fetchAbort(id)    closes the connection; the callback is never called
//
// getaddrinfo() blocks, so the lookup runs on the runtime's thread pool; the
// socket itself is non-blocking and watched by epoll. Requests are sent with
// `connection: close`, so every fetch has its own connection and a response
// without a length simply ends when the server closes it.

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "util.hpp"

namespace rtn {

namespace {

constexpr size_t kMaxHeadBytes = 64 * 1024;
constexpr size_t kReadChunk = 64 * 1024;

struct Fetch;

// Everything lives in one runtime per process, so plain statics are enough.
std::unordered_map<uint64_t, Fetch*> g_fetches;
uint64_t g_next_fetch_id = 1;
bool g_shutdown_hook_installed = false;

struct Address {
    sockaddr_storage addr;
    socklen_t len;
};

struct DnsResult {
    int rc = 0;
    int sys_errno = 0;
    std::vector<Address> addrs;
};

std::string address_string(const Address& a) {
    char ip[INET6_ADDRSTRLEN] = "";
    if (a.addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&a.addr)->sin_addr, ip, sizeof ip);
    } else if (a.addr.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&a.addr)->sin6_addr, ip, sizeof ip);
    }
    return ip;
}

JSValue new_error(JSContext* ctx, const std::string& msg, const char* code) {
    JSValue e = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, e, "message", JS_NewStringLen(ctx, msg.data(), msg.size()));
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    return e;
}

// Node-style socket error: "connect ECONNREFUSED 127.0.0.1:3000"
JSValue socket_error(JSContext* ctx, int err, const char* syscall, const std::string& address, int port) {
    std::string msg = std::string(syscall) + " " + errno_name(err);
    if (!address.empty()) msg += " " + address + ":" + std::to_string(port);
    JSValue e = new_error(ctx, msg, errno_name(err));
    JS_DefinePropertyValueStr(ctx, e, "errno", JS_NewInt32(ctx, -err), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, syscall), JS_PROP_C_W_E);
    if (!address.empty()) {
        JS_DefinePropertyValueStr(ctx, e, "address", JS_NewString(ctx, address.c_str()), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, e, "port", JS_NewInt32(ctx, port), JS_PROP_C_W_E);
    }
    return e;
}

// "getaddrinfo ENOTFOUND example.invalid", like Node.
JSValue dns_error(JSContext* ctx, const DnsResult& r, const std::string& host) {
    const char* code = r.rc == EAI_NONAME   ? "ENOTFOUND"
                     : r.rc == EAI_AGAIN    ? "EAI_AGAIN"
                     : r.rc == EAI_FAIL     ? "EAI_FAIL"
                     : r.rc == EAI_SYSTEM   ? errno_name(r.sys_errno)
                                            : "EAI_FAIL";
#ifdef EAI_NODATA
    if (r.rc == EAI_NODATA) code = "ENOTFOUND";
#endif
    JSValue e = new_error(ctx, std::string("getaddrinfo ") + code + " " + host, code);
    JS_DefinePropertyValueStr(ctx, e, "syscall", JS_NewString(ctx, "getaddrinfo"), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "hostname", JS_NewString(ctx, host.c_str()), JS_PROP_C_W_E);
    return e;
}

bool is_tchar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("!#$%&'*+-.^_`|~", c);
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

struct Fetch : IoHandler {
    Runtime* rt;
    JSContext* ctx;
    uint64_t id;
    JSValue callback;
    std::string host;
    int port;
    bool head_request;

    std::vector<Address> addrs;
    size_t next_addr = 0;
    std::string address;  // the one we're connecting / connected to (for errors)
    int last_err = ECONNREFUSED;
    int fd = -1;
    uint64_t io_id = 0;
    bool connected = false;

    std::string out;
    size_t out_off = 0;
    int write_err = 0;  // the server stopped reading; its response may still be complete

    std::string in;
    bool have_head = false;
    int status = 0;
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    enum class Body { None, Length, Chunked, UntilClose } body_kind = Body::None;
    size_t content_length = 0;
    std::string body;
    size_t chunk_pos = 0;  // chunked decoder position in `in`
    bool in_trailers = false;
    bool done = false;

    Fetch(Runtime* r, uint64_t fid, JSValue cb) : rt(r), ctx(r->ctx()), id(fid), callback(cb) {}

    // --- connecting ---------------------------------------------------------

    void on_resolved(const DnsResult& r) {
        if (r.rc != 0 || r.addrs.empty()) {
            fail(dns_error(ctx, r, host));
            return;
        }
        addrs = r.addrs;
        // IPv4 first, like the HTTP server ("localhost" is usually served on 127.0.0.1).
        std::stable_partition(addrs.begin(), addrs.end(), [](const Address& a) { return a.addr.ss_family == AF_INET; });
        connect_next();
    }

    void connect_next() {
        while (next_addr < addrs.size()) {
            Address& a = addrs[next_addr++];
            address = address_string(a);
            fd = socket(a.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                last_err = errno;
                continue;
            }
            if (connect(fd, reinterpret_cast<sockaddr*>(&a.addr), a.len) == 0 || errno == EINPROGRESS) {
                io_id = rt->add_io(fd, EPOLLOUT | EPOLLIN | EPOLLRDHUP, this);
                if (io_id) return;
            }
            last_err = errno;
            ::close(fd);
            fd = -1;
        }
        fail(socket_error(ctx, last_err, "connect", address, port));
    }

    void close_socket() {
        if (fd < 0) return;
        if (io_id) rt->remove_io(io_id, fd);
        ::close(fd);
        fd = -1;
        io_id = 0;
    }

    void on_io(uint32_t events) override {
        if (done) return;
        if (!connected) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err) {  // try the next address (e.g. ::1 refused, 127.0.0.1 works)
                last_err = err;
                close_socket();
                connect_next();
                return;
            }
            if (!(events & (EPOLLOUT | EPOLLIN))) return;
            connected = true;
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        if (out_off < out.size()) write_some();
        if (!done && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))) read_some();
    }

    // --- sending ------------------------------------------------------------

    void write_some() {
        while (out_off < out.size()) {
            ssize_t n = send(fd, out.data() + out_off, out.size() - out_off, MSG_NOSIGNAL);
            if (n > 0) {
                out_off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            write_err = n < 0 ? errno : EPIPE;  // read what the server has to say, if anything
            break;
        }
        out.clear();
        out.shrink_to_fit();
        out_off = 0;
        rt->modify_io(io_id, fd, EPOLLIN | EPOLLRDHUP);
    }

    // --- receiving ----------------------------------------------------------

    void read_some() {
        char buf[kReadChunk];
        bool eof = false;
        while (true) {
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n > 0) {
                in.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n == 0) {
                eof = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (!parse() && !done) fail(socket_error(ctx, errno, "read", address, port));
            return;
        }
        if (parse() || done) return;
        if (!eof) return;
        if (have_head && body_kind == Body::UntilClose) {
            body = std::move(in);
            finish();
        } else if (write_err && !have_head) {
            fail(socket_error(ctx, write_err, "write", address, port));
        } else {
            fail(new_error(ctx, "The server closed the connection before the response was complete",
                           "ERR_RESPONSE_INCOMPLETE"));
        }
    }

    // Returns true once the response is complete (and delivered).
    bool parse() {
        while (!have_head) {
            size_t end = in.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (in.size() > kMaxHeadBytes) fail(new_error(ctx, "Response headers are too large", "ERR_RESPONSE_HEADERS_TOO_LARGE"));
                return false;
            }
            if (end > kMaxHeadBytes) {
                fail(new_error(ctx, "Response headers are too large", "ERR_RESPONSE_HEADERS_TOO_LARGE"));
                return false;
            }
            if (!parse_head(std::string_view(in).substr(0, end + 2))) {
                fail(new_error(ctx, "Invalid HTTP response from " + address + ":" + std::to_string(port),
                               "ERR_INVALID_HTTP_RESPONSE"));
                return false;
            }
            in.erase(0, end + 4);
            if (status == 101) {
                fail(new_error(ctx, "The server switched protocols (101), which fetch does not support",
                               "ERR_INVALID_HTTP_RESPONSE"));
                return false;
            }
            if (status < 200) {  // 100 Continue, 103 Early Hints: the real response follows
                headers.clear();
                continue;
            }
            have_head = true;
        }
        switch (body_kind) {
            case Body::None:
                finish();
                return true;
            case Body::Length:
                if (in.size() < content_length) return false;
                in.resize(content_length);
                body = std::move(in);
                finish();
                return true;
            case Body::Chunked: {
                int r = decode_chunks();
                if (r < 0) {
                    fail(new_error(ctx, "Invalid chunked encoding in the response", "ERR_INVALID_HTTP_RESPONSE"));
                    return false;
                }
                if (r == 0) return false;
                finish();
                return true;
            }
            case Body::UntilClose:
                return false;
        }
        return false;
    }

    // "HTTP/1.1 200 OK\r\nname: value\r\n..." (without the final empty line)
    bool parse_head(std::string_view head) {
        size_t eol = head.find("\r\n");
        std::string_view line = head.substr(0, eol);
        if (!line.starts_with("HTTP/1.") || line.size() < 12 || line[8] != ' ') return false;
        for (int i = 9; i < 12; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(line[i]))) return false;
        }
        status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        if (line.size() > 12 && line[12] != ' ') return false;
        status_text = line.size() > 13 ? std::string(line.substr(13)) : "";

        bool chunked = false;
        bool has_length = false;
        bool has_te = false;
        content_length = 0;
        headers.clear();
        size_t p = eol + 2;
        while (p < head.size()) {
            size_t e = head.find("\r\n", p);
            std::string_view h = head.substr(p, e - p);
            p = e + 2;
            size_t colon = h.find(':');
            if (colon == std::string_view::npos || colon == 0) return false;
            std::string name(h.substr(0, colon));
            for (char& c : name) {
                if (!is_tchar(c)) return false;  // also rejects obsolete line folding
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            std::string_view value = trim(h.substr(colon + 1));
            if (value.find_first_of(std::string_view("\0\r\n", 3)) != std::string_view::npos) return false;

            if (name == "content-length") {
                size_t n = 0;
                if (value.empty() || value.size() > 18) return false;
                for (char c : value) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
                    n = n * 10 + static_cast<size_t>(c - '0');
                }
                if (has_length && n != content_length) return false;
                has_length = true;
                content_length = n;
            } else if (name == "transfer-encoding") {
                has_te = true;
                size_t comma = value.rfind(',');
                chunked = ieq(trim(comma == std::string_view::npos ? value : value.substr(comma + 1)), "chunked");
            }
            headers.emplace_back(std::move(name), std::string(value));
        }

        if (head_request || status == 204 || status == 304 || status < 200) body_kind = Body::None;
        else if (chunked) body_kind = Body::Chunked;
        else if (has_te) body_kind = Body::UntilClose;  // e.g. "gzip" without chunked
        else if (has_length) body_kind = Body::Length;
        else body_kind = Body::UntilClose;
        return true;
    }

    // Moves complete chunks from `in` to `body`. 1 = done, 0 = need more, -1 = invalid.
    int decode_chunks() {
        while (true) {
            size_t eol = in.find("\r\n", chunk_pos);
            if (eol == std::string::npos) return in.size() - chunk_pos > 4096 ? -1 : 0;
            if (in_trailers) {  // trailer fields until an empty line (ignored)
                bool last = eol == chunk_pos;
                chunk_pos = eol + 2;
                if (last) return 1;
                continue;
            }
            std::string_view line(in.data() + chunk_pos, eol - chunk_pos);
            if (size_t semi = line.find(';'); semi != std::string_view::npos) line = line.substr(0, semi);
            line = trim(line);
            if (line.empty() || line.size() > 15) return -1;
            size_t size = 0;
            for (char c : line) {
                int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d < 0) return -1;
                size = size * 16 + static_cast<size_t>(d);
            }
            if (size == 0) {
                chunk_pos = eol + 2;
                in_trailers = true;
                continue;
            }
            size_t data = eol + 2;
            if (in.size() < data + size + 2) {
                // Drop what's been decoded so the buffer doesn't hold the body twice.
                if (chunk_pos > 0) {
                    in.erase(0, chunk_pos);
                    chunk_pos = 0;
                }
                return 0;
            }
            if (in.compare(data + size, 2, "\r\n") != 0) return -1;
            body.append(in, data, size);
            chunk_pos = data + size + 2;
        }
    }

    // --- results ------------------------------------------------------------

    void finish() {
        done = true;
        close_socket();
        JSValue hs = JS_NewArray(ctx);
        uint32_t i = 0;
        for (auto& [k, v] : headers) {
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewStringLen(ctx, k.data(), k.size()));
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewStringLen(ctx, v.data(), v.size()));
            JS_SetPropertyUint32(ctx, hs, i++, pair);
        }
        JSValue args[] = {
            JS_NULL,
            JS_NewInt32(ctx, status),
            JS_NewStringLen(ctx, status_text.data(), status_text.size()),
            hs,
            body_kind == Body::None
                ? JS_NULL
                : JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(body.data()), body.size()),
        };
        deliver(5, args);
    }

    void fail(JSValue error) {
        done = true;
        close_socket();
        JSValue args[] = {error};
        deliver(1, args);
    }

    void deliver(int argc, JSValue* args) {
        JSValue cb = callback;
        callback = JS_UNDEFINED;
        release();
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, argc, args);
        if (JS_IsException(ret)) dump_pending_exception(ctx);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, cb);
        for (int i = 0; i < argc; ++i) JS_FreeValue(ctx, args[i]);
    }

    // Forgets this fetch; the object is freed once it's off the stack.
    void release() {
        done = true;
        close_socket();
        g_fetches.erase(id);
        JS_FreeValue(ctx, callback);
        callback = JS_UNDEFINED;
        Fetch* self = this;
        rt->defer([self] { delete self; });
    }
};

void shutdown_all() {
    std::vector<Fetch*> list;
    for (auto& [id, f] : g_fetches) list.push_back(f);
    for (Fetch* f : list) f->release();
}

// fetchStart(hostname, port, head, body, isHead, callback) -> id
JSValue js_fetch_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6 || !JS_IsFunction(ctx, argv[5])) return JS_ThrowTypeError(ctx, "fetchStart: bad arguments");
    std::string host = to_string(ctx, argv[0]);
    int32_t port = 0;
    if (JS_ToInt32(ctx, &port, argv[1]) < 0) return JS_EXCEPTION;
    if (port <= 0 || port > 65535) return JS_ThrowRangeError(ctx, "Invalid port: %d", port);

    Runtime* rt = Runtime::from(ctx);
    if (!g_shutdown_hook_installed) {
        g_shutdown_hook_installed = true;
        rt->on_shutdown(shutdown_all);
    }
    uint64_t id = g_next_fetch_id++;
    auto* f = new Fetch(rt, id, JS_DupValue(ctx, argv[5]));
    f->host = host;
    f->port = port;
    f->head_request = JS_ToBool(ctx, argv[4]) > 0;
    f->out = to_string(ctx, argv[2]);
    if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
        size_t size = 0;
        uint8_t* bytes = JS_GetTypedArrayType(argv[3]) >= 0 ? JS_GetUint8Array(ctx, &size, argv[3]) : nullptr;
        if (!bytes) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, f->callback);
            delete f;
            return JS_ThrowTypeError(ctx, "fetchStart: body must be a Uint8Array");
        }
        f->out.append(reinterpret_cast<const char*>(bytes), size);
    }
    g_fetches[id] = f;

    auto result = std::make_shared<DnsResult>();
    rt->queue_work(
        [result, host, port] {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICSERV;
            addrinfo* res = nullptr;
            result->rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
            result->sys_errno = errno;
            if (result->rc != 0) return;
            for (addrinfo* ai = res; ai; ai = ai->ai_next) {
                if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
                Address a{};
                std::memcpy(&a.addr, ai->ai_addr, ai->ai_addrlen);
                a.len = ai->ai_addrlen;
                result->addrs.push_back(a);
            }
            freeaddrinfo(res);
        },
        [result, id](bool cancelled) {
            auto it = g_fetches.find(id);  // gone if it was aborted meanwhile
            if (cancelled || it == g_fetches.end()) return;
            it->second->on_resolved(*result);
        });
    return JS_NewInt64(ctx, static_cast<int64_t>(id));
}

JSValue js_fetch_abort(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int64_t id = 0;
    if (argc > 0) JS_ToInt64(ctx, &id, argv[0]);
    auto it = g_fetches.find(static_cast<uint64_t>(id));
    if (it != g_fetches.end()) it->second->release();
    return JS_UNDEFINED;
}

const JSCFunctionListEntry kFetchFuncs[] = {
    JS_CFUNC_DEF("fetchStart", 6, js_fetch_start),
    JS_CFUNC_DEF("fetchAbort", 1, js_fetch_abort),
};

}  // namespace

void add_fetch_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyFunctionList(ctx, native, kFetchFuncs, sizeof(kFetchFuncs) / sizeof(kFetchFuncs[0]));
}

}  // namespace rtn
