// CommonJS entry point.
const assert = require("node:assert");
const { EventEmitter } = require("events");
const pkg = require("cjs-pkg");
console.log("main.cjs:", require.main === module, module.id, typeof __filename, pkg.double(5));
console.log("json:", require("./data.json").answer, "| esm from cjs:", require("esm-only").answer);
assert.deepStrictEqual(require("dual"), require("dual"));
console.log("cache:", require.cache[require.resolve("./config.cjs")] === undefined, require("./config.cjs") === require("./config.cjs"));
const e = new EventEmitter();
e.on("tick", (n) => console.log("tick", n));
e.emit("tick", 1);
process.on("exit", (code) => console.log("exit event, code", code));
