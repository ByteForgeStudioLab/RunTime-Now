// node:child_process — output checked against Node 22 (`node child-process.mjs`).
import { spawn, exec, execFile, execSync, execFileSync, spawnSync } from "node:child_process";
import cp from "child_process";
import { promisify } from "node:util";

console.log("sync:", execSync("echo hello && echo err >&2").toString().trim());
console.log("execFileSync:", execFileSync("printf", ["%s-%s", "a", "b"], { encoding: "utf8" }));
const r = spawnSync("sh", ["-c", "cat; exit 3"], { input: "piped in", encoding: "utf8" });
console.log("spawnSync:", r.status, JSON.stringify(r.stdout), r.signal);
console.log("ENOENT sync:", spawnSync("no-such-cmd-xyz").error.code);
try { execSync("exit 2", { stdio: "pipe" }); } catch (e) { console.log("execSync fail:", e.status, e.message.split("\n")[0]); }
const t = spawnSync("sleep", ["5"], { timeout: 100 });
console.log("timeout:", t.error.code, t.signal);

const { stdout } = await promisify(exec)("echo $FOO", { env: { ...process.env, FOO: "bar" } });
console.log("promisify exec:", stdout.trim());
try { await promisify(execFile)("sh", ["-c", "echo oops >&2; exit 4"]); } catch (e) { console.log("execFile reject:", e.code, e.stderr.trim()); }

await new Promise((resolve) => {
  const child = spawn("cat");
  let out = "";
  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (d) => (out += d));
  child.on("spawn", () => console.log("spawned pid>0:", child.pid > 0));
  child.on("exit", (code, sig) => console.log("exit:", code, sig));
  child.on("close", (code) => { console.log("close:", code, JSON.stringify(out)); resolve(); });
  child.stdin.write("line 1\n");
  child.stdin.end("line 2\n");
});

await new Promise((resolve) => {
  const child = spawn("no-such-cmd-xyz", ["a"]);
  child.on("error", (e) => { console.log("error:", e.message, e.code, e.syscall, e.path, e.spawnargs); });
  child.on("close", resolve);
});

await new Promise((resolve) => {
  const child = spawn("sleep", ["10"]);
  child.on("exit", (code, sig) => { console.log("killed:", code, sig, child.killed); resolve(); });
  setTimeout(() => child.kill(), 50);
});

await new Promise((resolve) => {
  const child = spawn("echo", ["$HOME is not expanded"], { stdio: "inherit" });
  child.on("close", resolve);
});
const sh = spawn("echo $((1+2))", { shell: true });
for await (const chunk of sh.stdout) console.log("shell:", chunk.toString().trim());
console.log("cwd:", execSync("pwd", { cwd: "/tmp", encoding: "utf8" }).trim());
console.log(typeof cp.spawn, typeof cp.ChildProcess);
// a big output through pipes
const big = execSync("head -c 3000000 /dev/zero | tr '\\\\0' a", { maxBuffer: 10e6 });
console.log("big:", big.length);
exec("head -c 2000000 /dev/zero", (err) => console.log("maxBuffer:", err?.code));
