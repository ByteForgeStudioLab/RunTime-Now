import { test, it, describe, expect, beforeAll, afterAll, beforeEach, afterEach, mock, spyOn } from "rtn:test";

const log: string[] = [];
beforeAll(() => { log.push("beforeAll"); });
afterAll(() => { log.push("afterAll"); });

describe("math", () => {
  beforeEach(() => { log.push("beforeEach"); });
  afterEach(() => { log.push("afterEach"); });
  test("equality", () => {
    expect(1 + 1).toBe(2);
    expect({ a: [1, { b: 2 }], u: undefined }).toEqual({ a: [1, { b: 2 }] });
    expect({ a: 1 }).not.toStrictEqual({ a: 1, b: undefined });
    expect([1, 2, 3]).toContain(2);
    expect("hello world").toMatch(/wor/);
    expect({ x: { y: [5] } }).toHaveProperty("x.y[0]", 5);
    expect(0.1 + 0.2).toBeCloseTo(0.3);
    expect({ id: 1, name: "a", extra: true }).toMatchObject({ name: "a" });
    expect({ when: new Date(), id: 7 }).toEqual({ when: expect.any(Date), id: expect.anything() });
    expect(() => { throw new TypeError("bad input"); }).toThrow(TypeError);
    expect(() => { throw new Error("bad input"); }).toThrow("bad");
  });
  it.each([[1, 2, 3], [2, 3, 5]])("adds %i + %i = %i", (a, b, sum) => {
    expect(a + b).toBe(sum);
  });
  describe("nested", () => {
    test("async", async () => {
      await expect(Promise.resolve(5)).resolves.toBe(5);
      await expect(Promise.reject(new Error("no"))).rejects.toThrow("no");
    });
    test("done callback", (done) => { setTimeout(done, 5); });
  });
});

test("mocks", () => {
  const fn = mock((x: number) => x * 2);
  fn(2); fn(5);
  expect(fn).toHaveBeenCalledTimes(2);
  expect(fn).toHaveBeenCalledWith(5);
  expect(fn.mock.results[0].value).toBe(4);
  const obj = { hi: () => "real" };
  const spy = spyOn(obj, "hi").mockReturnValue("fake");
  expect(obj.hi()).toBe("fake");
  expect(spy).toHaveBeenCalled();
});

test("this one fails", () => {
  expect({ a: 1, b: [1, 2] }).toEqual({ a: 1, b: [1, 3] });
});
test("timeout", async () => { await new Promise((r) => setTimeout(r, 200)); }, 50);
test.skip("skipped", () => {});
test.todo("write more tests");
test("hooks ran in order", () => {
  expect(log.slice(0, 3)).toEqual(["beforeAll", "beforeEach", "afterEach"]);
});
