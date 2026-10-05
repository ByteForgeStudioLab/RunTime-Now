#!/usr/bin/env python3
"""fetch() tests: a raw-socket HTTP server sends exact bytes, rtn fetches them."""
import json, os, socket, subprocess, sys, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RTN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "rtn")
passed = failed = 0

def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok    {name}")
    else:
        failed += 1
        print(f"  FAIL  {name} {detail}")

def big_chunked():
    out = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    for i in range(2000):
        out += b"400\r\n" + bytes([65 + i % 26]) * 1024 + b"\r\n"
    return out + b"0\r\n\r\n"

RESPONSES = {
    "/chunked": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-A: 1\r\n\r\n"
                b"5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\nTrailer: x\r\n\r\n",
    "/big-chunked": big_chunked(),
    "/until-close": b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nbody ends when the connection does",
    "/continue": b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\n"
                 b"HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
    "/no-reason": b"HTTP/1.1 299\r\nContent-Length: 0\r\n\r\n",
    "/extra-bytes": b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcEXTRA",
    "/garbage": b"garbage\r\n\r\n",
    "/truncated": b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a bit",
    "/bad-chunk": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n",
    "/empty": b"",
    "/huge-headers": b"HTTP/1.1 200 OK\r\nX: " + b"a" * 70000 + b"\r\n\r\n",
    "/two-lengths": b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc",
    "/header-injection": b"HTTP/1.1 200 OK\r\nX: a\rb\r\nContent-Length: 0\r\n\r\n",
    "/early-413": b"HTTP/1.1 413 Content Too Large\r\nContent-Length: 8\r\n\r\ntoo much",
}
requests_seen = {}

def handle(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    path = data.split(b" ")[1].decode() if data.count(b" ") >= 2 else ""
    requests_seen[path] = data.split(b"\r\n\r\n")[0].decode()
    if path == "/head":
        body = data.split(b"\r\n\r\n")[0]
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
    else:
        try:
            conn.sendall(RESPONSES.get(path, b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"))
        except OSError:
            pass
    conn.close()

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0))
srv.listen(64)
port = srv.getsockname()[1]

def serve():
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        threading.Thread(target=handle, args=(conn,), daemon=True).start()

threading.Thread(target=serve, daemon=True).start()

paths = list(RESPONSES) + ["/head"]
paths[paths.index("/early-413")] = "/early-413|POST"
out = subprocess.run([RTN, os.path.join(ROOT, "tests/fixtures/fetch-client.js"), str(port)] + paths,
                     capture_output=True, text=True, timeout=30)
results = {}
for line in out.stdout.splitlines():
    r = json.loads(line)
    results[r["path"]] = r
srv.close()

def ok(path, status=200, body=None):
    r = results.get(path, {})
    return r.get("status") == status and (body is None or r.get("body") == body)

def err(path, code):
    return results.get(path, {}).get("code") == code and results[path]["error"] == "fetch failed"

print("fetch:")
check("chunked response (extensions, trailers)", ok("/chunked", body="hello, world") and results["/chunked"]["headers"].get("x-a") == "1")
check("2 MB chunked response", ok("/big-chunked") and len(results["/big-chunked"]["body"]) == 2000 * 1024)
check("body delimited by connection close", ok("/until-close", body="body ends when the connection does"))
check("1xx responses are skipped", ok("/continue", 201, "ok"))
check("status line without a reason phrase", ok("/no-reason", 299, "") and results["/no-reason"]["statusText"] == "")
check("bytes after Content-Length are ignored", ok("/extra-bytes", body="abc"))
check("garbage response -> ERR_INVALID_HTTP_RESPONSE", err("/garbage", "ERR_INVALID_HTTP_RESPONSE"))
check("truncated body -> ERR_RESPONSE_INCOMPLETE", err("/truncated", "ERR_RESPONSE_INCOMPLETE"))
check("invalid chunk size -> ERR_INVALID_HTTP_RESPONSE", err("/bad-chunk", "ERR_INVALID_HTTP_RESPONSE"))
check("connection closed without a response", err("/empty", "ERR_RESPONSE_INCOMPLETE"))
check("70 KB of headers -> ERR_RESPONSE_HEADERS_TOO_LARGE", err("/huge-headers", "ERR_RESPONSE_HEADERS_TOO_LARGE"))
check("conflicting Content-Length -> rejected", err("/two-lengths", "ERR_INVALID_HTTP_RESPONSE"))
check("CR inside a header value -> rejected", err("/header-injection", "ERR_INVALID_HTTP_RESPONSE"))
check("server answers before reading the whole body", ok("/early-413", 413, "too much"),
      json.dumps(results.get("/early-413")))
head = requests_seen.get("/head", "").split("\r\n")
check("request head", head[0] == "GET /head HTTP/1.1" and f"host: 127.0.0.1:{port}" in head
      and "connection: close" in head and "accept-encoding: identity" in head, head)
check("rtn exits cleanly", out.returncode == 0, out.stderr)

print(f"fetch: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
