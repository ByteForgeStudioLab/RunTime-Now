// Qiyin holatlar: har bir natija JSON sifatida chiqadi (deno bilan solishtirish uchun)
const out: unknown[] = [];
const log = (...a: unknown[]): void => { out.push(a); };

// 1. Generik chaqiruv vs taqqoslash
function id<T>(x: T): T { return x; }
const a = 1, b = 2, c = 3, d = 4;
log(id<number>(5), a < b && c > d, a < b ? "lt" : "ge", 1 << 2, 16 >> 1, -1 >>> 28, a >= b);

// 2. Ternar ichida arrow funksiya
const cond = true;
const f1 = cond ? (x: number): number => x * 2 : null;
const f2 = cond ? (a) : b;
log(f1!(21), f2);

// 3. as / as unknown as / <T> assertion / satisfies
const raw: any = "42";
const n1 = raw as unknown as string;
const n2 = <string>raw;
const cfg = { port: 8080 } satisfies { port: number };
log(n1, n2, cfg, `${raw as string}!`);

// 4. Non-null zanjiri
const deep: { x?: { y?: number[] } } = { x: { y: [7] } };
log(deep.x!.y![0]!, deep!.x?.y!.length);

// 5. Destrukturlash + turlar
const { p, q }: { p: number; q: string } = { p: 1, q: "q" };
const [first, ...rest]: number[] = [1, 2, 3];
function sum({ x, y = 10 }: { x: number; y?: number }, ...more: number[]): number {
  return x + y + more.reduce((s: number, v: number) => s + v, 0);
}
log(p, q, first, rest, sum({ x: 1 }, 2, 3));

// 6. this parametri, default arrow
function withThis(this: { k: number }, add: number = 0): number { return this.k + add; }
const cb = (fn: (v: number) => string = (v) => `v${v}`): string => fn(3);
log(withThis.call({ k: 5 }, 1), cb());

// 7. Obyekt literal: metodlar, getter, `type` kalit
const obj = {
  type: "box",
  size<T>(this: void, v: T): T { return v; },
  get label(): string { return "L"; },
  async load(): Promise<number> { return 1; },
};
const type = 3;
const as = 4;
log(obj.type, obj.size(9), obj.label, type + as);

// 8. Klass: index signature, overload, static block, #private, override, accessors
abstract class Base<T extends object = {}> {
  [key: string]: unknown;
  protected abstract name: string;
  constructor(protected readonly id: number) {}
  abstract kind(): string;
  describe(): string { return `${this.kind()}#${this.id}`; }
}
class Impl extends Base implements Iterable<number> {
  static count: number;
  static { Impl.count = 0; }
  #secret: string = "s";
  protected name!: string;
  declare extra: number;
  value?: number;
  handler = (e: string): string => e.toUpperCase();
  gen = <T,>(x: T): T[] => [x];
  constructor(id: number, public tag: string = "t") {
    super(id)
    Impl.count++;
  }
  override kind(): string { return "impl"; }
  area(w: number): number;
  area(w: number, h: number): number;
  area(w: number, h?: number): number { return w * (h ?? w); }
  get secret(): string { return this.#secret; }
  set secret(v: string) { this.#secret = v; }
  *[Symbol.iterator](): Iterator<number> { yield this.id; }
}
const im = new Impl(7);
im.secret = "new";
log(im.describe(), im.tag, im.area(3), im.area(2, 5), im.secret, [...im], Impl.count,
    "extra" in im, im.handler("hi"), im.gen(1), Object.keys(im));

// 9. Enumlar
enum E { A, B = 10, C, D = B * 2, "str-key" = 99 }
enum S { X = "x", Y = `y` }
export enum Exported { One = 1 }
enum Merged { M1 = 1 }
enum Merged { M2 = 2 }
log(E.A, E.B, E.C, E.D, E[10], E["str-key"], S.X, S.Y, Exported.One, Merged.M1, Merged.M2);

// 10. Turlar orasida ASI (nuqta-vergulsiz)
let asi = 1
type Alias = string
;(() => { asi = 2 })()
interface I1 { a: number }
log(asi)

// 11. Regex ichida < > : belgilari
const re = /<(\w+):\s*>/g;
log("<a: >".replace(re, "[$1]"), 10 / 2 / 1);

// 12. Label, switch, obyekt qaytarish
outer: for (const i of [1, 2]) {
  switch (i) {
    case 1: { continue outer; }
    default: log({ i } as { i: number });
  }
}
const mk = (): { v: number } => ({ v: 1 });
const mkAsync = async <T,>(x: T): Promise<T> => x;
log(mk(), await mkAsync<string>("async"));

// 13. for..of + as, keyof typeof, optional chaining + generik
const rec = { a: 1, b: 2 };
const keys = Object.keys(rec) as (keyof typeof rec)[];
let total = 0;
for (const [k, v] of Object.entries(rec) as [string, number][]) total += v;
const maybe: { m?<T>(x: T): T } = { m: (x) => x };
log(keys, total, maybe.m?.<number>(5) ?? "none", new Map<string, Array<number>>([["k", [1]]]).get("k"));

// 14. Kod ichida izohlar va ko'p qatorli turlar
type Multi =
  | { kind: "a" } // izoh
  | { kind: "b" /* ichki */ };
const m: Multi = { kind: "a" };
function longRet(
  x: number,
): {
  doubled: number;
} {
  return { doubled: x * 2 };
}
const arrowLong = (
  x: number
): Promise<
  number
> => Promise.resolve(x);
log(m, longRet(4), await arrowLong(8));

declare global { interface Window { foo: string } }
declare const __DEV__: boolean;
export type { Alias };
export {};

for (const line of out) console.log(JSON.stringify(line));
