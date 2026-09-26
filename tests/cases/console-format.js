// How console.log prints values (should match Node's style)
console.log("text", 42, -0, 1.5, 10n, true, null, undefined, Symbol("s"));
console.log({ a: 1, b: "two", c: [1, 2, 3], d: { e: { f: { g: 1 } } } });
console.log([undefined, null, "x", [1, [2, [3, [4]]]]]);
console.log(new Map([["k", { v: 1 }]]), new Set([1, "a"]), new Map(), new Set());
class Point { constructor(x, y) { this.x = x; this.y = y; } }
console.log(new Point(1, 2), Object.create(null), [], {});
console.log(function named() {}, () => {}, class Foo {}, Math.max);
console.log(/re+gex/gi, new Date(0), new Uint8Array([1, 2, 3]));
const circular = { name: "loop" };
circular.self = circular;
console.log(circular);
console.log("%s is %d years, %i%% sure, %j", "Ali", 20.5, 99.9, { j: 1 });
console.log({ "needs-quotes": 1, valid_id: 2, 3: "num" });
console.log(Array.from({ length: 120 }, (_, i) => i).length, "items ok");
console.log({ long1: "aaaaaaaaaaaaaaaa", long2: "bbbbbbbbbbbbbbbbbbbb", long3: "cccccccccccccccccccc", long4: 1 });
console.log(Promise.resolve(5), new Promise(() => {}), new Error("in an object").message);
