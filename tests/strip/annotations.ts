let count: number = 0, name: string = "x";
const list: Array<Map<string, number[]>> = [];
function add<T extends number>(a: T, b?: T, ...rest: T[]): T { return a; }
const arrow = async <T,>(x: T): Promise<T> => x;
const value = (input as unknown as string[])!.length;
const cfg = { port: 1 } satisfies Record<string, number>;
const modes = ["a", "b"] as const;
let later!: number;
const shift = 1 << 2, cmp = count < 2 && count > 0;
const pick = count ? (x: number): number => x : null;
function guard(v: unknown): v is string { return typeof v === "string"; }
function f(this: Window, a = <string>"b") {}
const call = add<number>(1), inst = new Map<string, number>();
const multi = (
  x: number
): Promise<
  number
> => Promise.resolve(x);
type A = string
;(() => later)()
