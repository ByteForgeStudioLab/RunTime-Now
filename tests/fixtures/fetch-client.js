// Used by tests/fetch_test.py: fetches each path given on the command line and
// prints one JSON line per request with what came back.
const [port, ...paths] = process.argv.slice(2);
for (const path of paths) {
  const [p, method = "GET"] = path.split("|");
  const init = { method };
  if (method === "POST") init.body = "x".repeat(300_000);
  try {
    const res = await fetch(`http://127.0.0.1:${port}${p}`, init);
    const body = await res.text();
    console.log(JSON.stringify({ path: p, status: res.status, statusText: res.statusText, headers: Object.fromEntries(res.headers), body }));
  } catch (e) {
    console.log(JSON.stringify({ path: p, error: e.message, code: e.cause?.code ?? null }));
  }
}
