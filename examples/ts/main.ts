import { type User, type Role, DEFAULT_ROLE, Result } from "./types.ts";
import { Stack } from "./stack.js"; // TS ESM uslubi: aslida stack.ts
import fs from "rtn:fs";

// --- oddiy turlar ---
const name: string = "RunTime-Now";
let count: number = 0, flag: boolean = true;
const nums: Array<number> = [1, 2, 3];
const tuple: [string, number] = ["a", 1];

// --- funksiyalar ---
function add(a: number, b: number = 10): number {
  return a + b;
}
function greet(user: User, role?: Role): string {
  return `${user.name} (${role ?? DEFAULT_ROLE})`;
}
const square = (x: number): number => x * x;
const identity = <T,>(value: T): T => value;
async function load(id: number): Promise<User> {
  return { id, name: "Ali" };
}

// overloads
function format(x: string): string;
function format(x: number): string;
function format(x: unknown): string {
  return String(x);
}

// --- klasslar ---
abstract class Shape {
  abstract area(): number;
  describe(): string {
    return `${this.constructor.name}: ${this.area().toFixed(2)}`;
  }
}

class Circle extends Shape implements Iterable<number> {
  static readonly PI: number = 3.14159;
  private cache?: number;
  declare tag: string;

  constructor(public readonly radius: number, private label: string = "circle") {
    super();
  }
  area(): number {
    return (this.cache ??= Circle.PI * this.radius ** 2);
  }
  *[Symbol.iterator](): Iterator<number> {
    yield this.radius;
  }
  get info(): string {
    return this.label;
  }
}

// --- enum ---
enum Color { Red, Green = 5, Blue }
enum Direction { Up = "UP", Down = "DOWN" }
const enum Flags { None = 0, A = 1 << 0, B = 1 << 1, AB = A | B }

// --- as / satisfies / non-null ---
const data: unknown = { x: 1 };
const x = (data as { x: number }).x;
const cfg = { port: 3000 } satisfies Record<string, number>;
const el = [1, 2, 3].find((n) => n > 1)!;
const modes = ["a", "b"] as const;

// --- generiklar ---
function first<T extends { length: number }>(items: T[]): T | undefined {
  return items[0];
}
const map = new Map<string, number[]>();
map.set("a", [1]);

function ok<T>(value: T): Result<T> {
  return { ok: true, value };
}

// --- type guard ---
function isString(v: unknown): v is string {
  return typeof v === "string";
}

let later!: number;
later = 5;

try {
  JSON.parse("{");
} catch (e: unknown) {
  count++;
}

const s = new Stack<number>();
s.push(1); s.push(2);

console.log(name, count, flag, nums, tuple);
console.log(add(2), greet({ id: 1, name: "Vali" }), square(4), identity<string>("id"));
console.log(format(42), await load(7));
const c = new Circle(2);
console.log(c.describe(), c.radius, [...c], c.info);
console.log(Color.Red, Color.Green, Color.Blue, Color[5], Direction.Up, Flags.AB);
console.log(x, cfg, el, modes, first(["q", "w"]), map, ok(1), isString("s"), later);
console.log("stack:", s.pop(), s.size);
console.log(typeof fs.readFileSync);
