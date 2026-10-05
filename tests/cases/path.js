// node:path (checked against Node 22)
import path from "node:path";
import { join, basename } from "path";
import posix from "rtn:path";
console.log(path.join("/a/b", "../c", "./d.txt"), join("a", "", "b/"), basename("/x/y.tar.gz", ".gz"));
console.log(path.resolve("/base", "x/../y"), path.relative("/a/b/c", "/a/d"), path.normalize("a//b/../c/."));
console.log(path.dirname("/a/b/"), path.extname("archive.tar.gz"), path.extname(".bashrc"), path.isAbsolute("./x"));
console.log(path.parse("/home/user/file.txt"), path.format({ dir: "/tmp", name: "f", ext: "js" }));
console.log(path.sep, path.delimiter, posix === path, path.posix === path);
try { path.join(1); } catch (e) { console.log(e.code, e.message); }
