import fs from "rtn:fs";
const dir = `/tmp/rtn-test-fs-${process.pid}`;
const show = (s) => s.replaceAll(dir, "<dir>");
const attempt = (label, fn) => {
  try {
    const r = fn();
    console.log(label, "->", r === undefined ? "ok" : show(String(r)));
  } catch (e) {
    console.log(label, "->", e.code, "|", e.syscall, "|", show(e.message));
  }
};
fs.rmSync(dir, { recursive: true, force: true });
attempt("read missing", () => fs.readFileSync("/no/such/file"));
attempt("read dir", () => fs.readFileSync("/tmp"));
attempt("mkdir", () => fs.mkdirSync(dir));
attempt("mkdir again", () => fs.mkdirSync(dir));
attempt("mkdir -p", () => fs.mkdirSync(`${dir}/a/b/c`, { recursive: true }));
attempt("write", () => fs.writeFileSync(`${dir}/a.txt`, "salom"));
attempt("append", () => fs.appendFileSync(`${dir}/a.txt`, " dunyo"));
attempt("read text", () => fs.readFileSync(`${dir}/a.txt`, "utf8"));
attempt("read bytes", () => [...fs.readFileSync(`${dir}/a.txt`).slice(0, 3)].join(","));
attempt("write bytes", () => fs.writeFileSync(`${dir}/b.bin`, new Uint8Array([0, 255, 10])));
attempt("size", () => fs.statSync(`${dir}/b.bin`).size);
attempt("rename", () => fs.renameSync(`${dir}/b.bin`, `${dir}/c.bin`));
attempt("copy", () => fs.copyFileSync(`${dir}/a.txt`, `${dir}/d.txt`));
attempt("readdir", () => fs.readdirSync(dir).join(","));
const st = fs.statSync(dir);
attempt("stat dir", () => [st.isDirectory(), st.isFile(), st.mtime instanceof Date, typeof st.mtimeMs].join(","));
attempt("rm dir w/o recursive", () => fs.rmSync(dir));
attempt("rm missing", () => fs.rmSync(`${dir}/nope`));
attempt("rm missing force", () => fs.rmSync(`${dir}/nope`, { force: true }));
attempt("write into missing dir", () => fs.writeFileSync("/no/such/dir/x", "x"));
attempt("rm -r", () => fs.rmSync(dir, { recursive: true }));
attempt("exists after", () => String(fs.existsSync(dir)));
