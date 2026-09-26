#!/usr/bin/env python3
"""HTTP server tests: starts tests/fixtures/server.ts and talks to it over raw sockets."""
import http.client, json, os, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RTN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "rtn")
PORT = 38000 + os.getpid() % 1000
passed = failed = 0

def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok    {name}")
    else:
        failed += 1
        print(f"  FAIL  {name} {detail}")

def raw(data, read_timeout=2.0, shutdown=True):
    """Sends raw bytes, returns everything the server sends back until it closes / times out."""
    s = socket.create_connection(("127.0.0.1", PORT))
    s.sendall(data)
    if shutdown:
        s.shutdown(socket.SHUT_WR)
    s.settimeout(read_timeout)
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    s.close()
    return out

def request(method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r, data

env = dict(os.environ, PORT=str(PORT))
proc = subprocess.Popen([RTN, os.path.join(ROOT, "tests/fixtures/server.ts")], env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
assert proc.stdout.readline().strip() == "ready", "server did not start"

print("http:")
r, d = request("GET", "/")
check("GET / -> 200 text", r.status == 200 and d == b"hello")
check("content-type / content-length / date headers",
      r.getheader("content-type") == "text/plain;charset=UTF-8" and r.getheader("content-length") == "5" and r.getheader("date"))

r, d = request("GET", "/json?name=%D0%9E%D0%BB%D0%B5%D0%B3&x=1+2")
check("JSON + UTF-8 query params", json.loads(d) == {"query": {"name": "Олег", "x": "1 2"}, "method": "GET"})

blob = bytes(range(256)) * 1000
r, d = request("POST", "/echo", body=blob, headers={"content-type": "application/x-test"})
check("POST binary body echo (256 KB)", d == blob and r.getheader("content-type") == "application/x-test")

r, d = request("POST", "/echo-json", body=json.dumps({"a": [1, "b"]}), headers={"content-type": "application/json"})
check("POST JSON body", json.loads(d) == {"a": [1, "b"]})

r, d = request("GET", "/headers")
check("204 + custom headers, no body", r.status == 204 and d == b"" and r.getheader("x-custom") == "yes")

r, d = request("GET", "/multi-cookie")
check("multiple set-cookie headers", r.msg.get_all("set-cookie") == ["a=1", "b=2"])

r, d = request("GET", "/redirect")
check("Response.redirect", r.status == 307 and r.getheader("location") == "http://example.com/")

r, d = request("GET", "/nope")
check("404", r.status == 404 and d == b"not found")

r, d = request("GET", "/throw")
check("handler throws -> 500, server keeps running", r.status == 500 and request("GET", "/")[0].status == 200)

t = time.time()
r, d = request("GET", "/slow")
check("async handler with timer", d == b"slow" and time.time() - t >= 0.14)

r, d = request("HEAD", "/")
check("HEAD: headers only", r.status == 200 and d == b"" and r.getheader("content-length") == "5")

r, d = request("GET", "/url", headers={"Host": "my.host:1234"})
check("req.url uses the Host header", d == b"http://my.host:1234/url")

c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
socks = set()
for _ in range(3):
    c.request("GET", "/"); c.getresponse().read(); socks.add(id(c.sock))
check("keep-alive: 3 requests on one connection", len(socks) == 1)
c.close()

out = raw(b"GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /json?a=1 HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\n\r\n")
check("pipelining + half-close: 3 answers in order",
      out.count(b"HTTP/1.1 ") == 3 and out.index(b"hello") < out.index(b'"a":"1"') < out.index(b"not found"))

out = raw(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
check("chunked request body", out.endswith(b"hello world"))

s = socket.create_connection(("127.0.0.1", PORT))
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n")
s.settimeout(2)
first = s.recv(100)
s.sendall(b"data")
rest = s.recv(1000)
s.close()
check("Expect: 100-continue", first.startswith(b"HTTP/1.1 100 Continue") and rest.endswith(b"data"))

check("malformed request -> 400", raw(b"NONSENSE\r\n\r\n").startswith(b"HTTP/1.1 400"))
check("HTTP/2.0 request line -> 505", raw(b"GET / HTTP/2.0\r\n\r\n").startswith(b"HTTP/1.1 505"))
check("100 KB header -> 431", raw(b"GET / HTTP/1.1\r\nX: " + b"a" * 100000 + b"\r\n\r\n").startswith(b"HTTP/1.1 431"))
check("conflicting Content-Length -> 400",
      raw(b"POST /echo HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab").startswith(b"HTTP/1.1 400"))

s = socket.create_connection(("127.0.0.1", PORT))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
s.recv(4096)
s.settimeout(3)
t = time.time()
closed = s.recv(10) == b""
check("idle keep-alive connection closed after keepAliveTimeout", closed and 0.3 < time.time() - t < 2.5)
s.close()

out = raw(b"GET / HTTP/1.1\r\nHost: x\r\n", read_timeout=3, shutdown=False)
check("slow/incomplete request -> 408 after requestTimeout", out.startswith(b"HTTP/1.1 408"))

results = []
def worker():
    for _ in range(20):
        results.append(request("GET", "/")[1] == b"hello")
threads = [threading.Thread(target=worker) for _ in range(20)]
for th in threads: th.start()
for th in threads: th.join()
check("400 requests from 20 threads", len(results) == 400 and all(results))

s = socket.create_connection(("127.0.0.1", PORT))
s.sendall(b"GET /slow HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.05)
request("GET", "/stop")
s.settimeout(3)
slow = s.recv(4096)
check("stop(): in-flight request still answered", slow.endswith(b"slow"))
try:
    code = proc.wait(timeout=5)
except subprocess.TimeoutExpired:
    proc.kill(); code = None
err = proc.stderr.read()
check("process exits cleanly after stop()", code == 0, f"exit={code}")
check("thrown error was logged to stderr", "Error: boom" in err)

print(f"http: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
