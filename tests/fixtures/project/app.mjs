// ES module side: packages, exports conditions, CommonJS interop.
import { double, name, dir, cycle } from "cjs-pkg";
import cjsDefault from "cjs-pkg";
import greet, { kind } from "dual";
import { feature } from "dual/feature";
import { pad } from "dual/utils/pad";
import esmDefault, { answer } from "esm-only";
import tools, { version } from "@scope/tools";
import typed from "typed-mod";
import config from "#config";
import { helper } from "#lib/helper";
import { self } from "fixture-app/self";
import legacy from "legacy-folder";
import outer from "outer";
import inner from "inner";
import { createRequire } from "node:module";

console.log("cjs named:", double(21), name, dir, cycle);
console.log("cjs default is module.exports:", cjsDefault.double === double);
console.log("dual (import):", greet("ali"), kind);
console.log("conditions:", feature, "| pattern:", pad("x"));
console.log("esm-only:", esmDefault, answer);
console.log("__esModule interop:", typeof tools.default, tools.default(), version);
console.log("typescript in node_modules:", typed.len({ x: 3, y: 4 }));
console.log("#imports:", config.env, config.nested.answer, helper());
console.log("self-reference:", self);
console.log("folder/index:", legacy, "| nested node_modules:", outer, "|", inner);

const require = createRequire(import.meta.url);
console.log("dual (require):", require("dual").greet("ali"), require("dual").kind);
console.log("require(esm):", require("esm-only").answer, require("esm-only").default);
console.log("require.resolve:", require.resolve("dual").split("/node_modules/")[1], require.resolve("fs"));
console.log("subpath file:", require("legacy-folder/lib/extra"));
for (const spec of ["dual/internal/secret", "dual/missing", "no-such-package", "#nope"]) {
  try {
    await import(spec);
  } catch (e) {
    console.log(`import ${spec}:`, e.code);
  }
}
try { require("no-such-package"); } catch (e) { console.log("require missing:", e.code, e.message.split("\n")[0]); }
