// crypto.getRandomValues / randomUUID and structuredClone.
const uuid = crypto.randomUUID();
console.log("uuid v4:", /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(uuid), uuid !== crypto.randomUUID());
const arr = new Uint32Array(8);
console.log("getRandomValues returns its argument:", crypto.getRandomValues(arr) === arr, arr.some((x) => x !== 0));
for (const bad of [new Float64Array(2), new Uint8Array(65537)]) {
  try { crypto.getRandomValues(bad); } catch (e) { console.log(e.name); }
}

const source = {
  n: 1, s: "x", big: 10n, d: new Date(0), r: /a+/gi, m: new Map([["k", { deep: [1, 2] }]]), set: new Set([1, "two"]),
  bytes: new Uint8Array([1, 2, 3]), err: new RangeError("bad", { cause: "why" }), boxed: Object("str"),
};
source.self = source;
const copy = structuredClone(source);
console.log(copy.self === copy, copy !== source, copy.m.get("k") !== source.m.get("k"), copy.m.get("k").deep);
console.log(copy.d instanceof Date, copy.r.flags, copy.err instanceof RangeError, copy.err.message, copy.err.cause);
console.log(copy.bytes, copy.set, typeof copy.boxed, copy.big);
class Point { x = 1; y = 2; }
console.log("class instance -> plain object:", structuredClone(new Point()), Object.getPrototypeOf(structuredClone(new Point())) === Object.prototype);
for (const bad of [{ fn() {} }, Symbol("s"), new URL("http://a/"), new WeakMap()]) {
  try { structuredClone(bad); } catch (e) { console.log(e.name); }
}
const buffer = new Uint8Array([9, 8, 7]).buffer;
const moved = structuredClone({ buffer }, { transfer: [buffer] });
console.log("transfer:", buffer.byteLength, new Uint8Array(moved.buffer));
console.log(structuredClone(new DOMException("gone", "AbortError")).name);
