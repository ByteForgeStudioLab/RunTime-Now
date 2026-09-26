import { type User, DEFAULT_ROLE } from "../fixtures/lib/types.ts";
import { Stack } from "../fixtures/lib/stack.js"; // resolves to stack.ts
import { greet, PI } from "../fixtures/lib/math";  // extension-less
import config from "../fixtures/lib/config.json";
import fs, { existsSync } from "rtn:fs";
import * as nodeFs from "node:fs";

const u: User = { id: 1, name: "Ali" };
const s = new Stack<number>();
s.push(1);
s.push(2);
console.log(u.name, DEFAULT_ROLE, s.pop(), s.size, greet("World"), PI, config);
console.log(typeof fs.readFileSync, existsSync("/"), typeof nodeFs.statSync);
console.log(import.meta.main, import.meta.filename.endsWith("tests/cases/modules.ts"), import.meta.url.startsWith("file:///"));
const lazy = await import("../fixtures/lib/math.js");
console.log("dynamic import:", lazy.greet("lazy"));
try {
  await import("./does-not-exist.js");
} catch (e) {
  console.log("missing module:", (e as Error).message.split(" imported from")[0]);
}
try {
  await import("some-npm-package");
} catch (e) {
  console.log((e as Error).message);
}
