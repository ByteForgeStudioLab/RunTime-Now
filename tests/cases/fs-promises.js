// node:fs/promises: the operations run on the thread pool.
import fs, { readFile, writeFile, mkdir, rm } from "node:fs/promises";
import fsSync, { promises } from "fs";
const dir = "fs-promises-tmp";
await rm(dir, { recursive: true, force: true });
console.log("mkdir:", await mkdir(`${dir}/a/b`, { recursive: true }) !== undefined);
await writeFile(`${dir}/a/x.txt`, "héllo");
await fs.appendFile(`${dir}/a/x.txt`, new Uint8Array([33]));
console.log(await readFile(`${dir}/a/x.txt`, "utf8"), await readFile(`${dir}/a/x.txt`));
console.log((await fs.stat(`${dir}/a`)).isDirectory(), (await fs.lstat(`${dir}/a/x.txt`)).size);
await fs.copyFile(`${dir}/a/x.txt`, `${dir}/y.txt`);
await fs.rename(`${dir}/y.txt`, `${dir}/z.txt`);
console.log(await fs.readdir(dir), await fs.access(`${dir}/z.txt`, fs.constants.R_OK));
try { await readFile(`${dir}/missing`); } catch (e) { console.log(e.code, e.syscall, e.message); }
try { await rm(`${dir}/a`); } catch (e) { console.log(e.code); }
try { await readFile(42); } catch (e) { console.log(e.name, e.message); }
console.log(promises === fs, fsSync.promises === fs);
const all = await Promise.all(Array.from({ length: 50 }, (_, i) =>
  writeFile(`${dir}/f${i}`, String(i)).then(() => readFile(`${dir}/f${i}`, "utf8"))));
console.log("50 parallel:", all.every((v, i) => v === String(i)));
await rm(dir, { recursive: true });
console.log("cleaned up:", !fsSync.existsSync(dir));
