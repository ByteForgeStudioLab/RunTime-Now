// node:buffer, events, util, assert, crypto, os, url, timers — output checked against Node 22.
import { Buffer } from "node:buffer";
import EventEmitter, { once } from "node:events";
import util from "node:util";
import assert from "node:assert";
import { createHash, createHmac, randomBytes, randomInt, timingSafeEqual } from "node:crypto";
import os from "node:os";
import { fileURLToPath, pathToFileURL } from "node:url";
import { setTimeout as sleep } from "node:timers/promises";

// Buffer
const b = Buffer.from("héllo wörld");
console.log(b.length, b.toString("hex"), b.toString("base64"), b.toString("base64url"));
console.log(Buffer.from("aGVsbG8=", "base64").toString(), Buffer.from("68656c6c6f", "hex").toString("latin1"));
console.log(Buffer.from([1, 2, 3]), Buffer.alloc(4, "ab"), Buffer.concat([Buffer.from("a"), Buffer.from("bc")]).toString());
console.log(Buffer.byteLength("héllo"), Buffer.isBuffer(b), Buffer.isBuffer(new Uint8Array(1)), b instanceof Uint8Array);
const n = Buffer.alloc(8);
n.writeUInt32BE(0xdeadbeef, 0);
n.writeInt16LE(-2, 4);
console.log(n.readUInt32BE(0).toString(16), n.readInt16LE(4), n.readUInt8(0), n.toJSON().data.length);
console.log(b.indexOf("wörld"), b.includes("xyz"), b.slice(0, 5).toString(), b.subarray(1, 3) instanceof Buffer);
console.log(Buffer.from("abc").equals(Buffer.from("abc")), Buffer.compare(Buffer.from("a"), Buffer.from("b")), JSON.stringify(Buffer.from("hi")));
console.log(Buffer.from("€").toString("utf16le").length, Buffer.from("hi", "utf16le"), Buffer.from("ÿ", "latin1"));

// events
const e = new EventEmitter();
const seen = [];
e.on("x", (v) => seen.push(`on:${v}`));
e.prependListener("x", (v) => seen.push(`first:${v}`));
e.once("x", (v) => seen.push(`once:${v}`));
e.emit("x", 1);
e.emit("x", 2);
console.log(seen.join(" "), e.listenerCount("x"), e.eventNames());
try { e.emit("error", new Error("boom")); } catch (err) { console.log("unhandled error event:", err.message); }
setTimeout(() => e.emit("ready", "a", "b"), 1);
console.log("once():", await once(e, "ready"));
class Child extends EventEmitter {}
const c = new Child();
c.on("hi", function () { console.log("subclass this ok:", this === c); });
c.emit("hi");

// util
console.log(util.format("%s=%d %j %%", "a", 42, { k: [1] }), util.format("x", { y: 1 }, 3));
console.log(util.inspect({ a: [1, { b: new Map([[1, 2]]) }] }), util.inspect("str"));
const readLater = util.promisify((x, cb) => setTimeout(() => cb(null, x * 2), 1));
console.log("promisify:", await readLater(21));
console.log(util.isDeepStrictEqual({ a: [1, 2] }, { a: [1, 2] }), util.isDeepStrictEqual([1], ["1"]), util.types.isPromise(Promise.resolve()));
function Old() { EventEmitter.call(this); }
util.inherits(Old, EventEmitter);
console.log("inherits:", new Old() instanceof EventEmitter, Old.super_ === EventEmitter);

// assert
assert.strictEqual(1, 1);
assert.deepStrictEqual({ a: new Set([1]), d: new Date(0) }, { a: new Set([1]), d: new Date(0) });
assert.throws(() => { throw new TypeError("bad"); }, TypeError);
assert.throws(() => { throw new Error("nope"); }, /nope/);
await assert.rejects(Promise.reject(new Error("r")), { message: "r" });
for (const fn of [() => assert.strictEqual(1, 2), () => assert.deepStrictEqual([1], [2]), () => assert.ok(0), () => assert.throws(() => {})]) {
  try { fn(); } catch (err) { console.log(err.name, err.code, err.message.split("\n")[0]); }
}
console.log(assert.equal === assert.strict.equal, typeof assert.strict.equal);

// crypto
console.log(createHash("sha256").update("abc").digest("hex"));
console.log(createHash("sha1").update("abc").digest("hex"), createHash("md5").update("abc").digest("hex"));
console.log(createHash("sha256").update("").digest("base64"), createHash("sha256").update("a".repeat(1000)).digest("hex").slice(0, 16));
console.log(createHmac("sha256", "key").update("The quick brown fox jumps over the lazy dog").digest("hex"));
console.log(randomBytes(16).length, randomInt(5, 6), timingSafeEqual(Buffer.from("a"), Buffer.from("a")));
console.log(Buffer.from(await crypto.subtle.digest("SHA-256", new TextEncoder().encode("abc"))).toString("hex").slice(0, 16));

// os, url, timers
console.log(os.EOL === "\n", os.platform(), typeof os.cpus().length, os.totalmem() > 0, typeof os.hostname(), os.endianness());
console.log(fileURLToPath("file:///tmp/a%20b.txt"), pathToFileURL("/tmp/a b.txt").href);
console.log("timers/promises:", await sleep(1, "slept"));
console.log(typeof setImmediate, global === globalThis, typeof process.on, process.versions.node.split(".")[0] >= 22);
