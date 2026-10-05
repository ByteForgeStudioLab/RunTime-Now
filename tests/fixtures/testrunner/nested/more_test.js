import { test, expect, describe } from "rtn:test";
import assert from "node:assert";

describe.each([{ n: 1, sq: 1 }, { n: 3, sq: 9 }])("square($n)", ({ n, sq }) => {
  test("matches", () => expect(n * n).toBe(sq));
});
test("node:assert works inside rtn test", () => {
  assert.deepStrictEqual({ a: 1 }, { a: 1 });
});
test("expect.assertions counts", async () => {
  expect.assertions(2);
  expect(1).toBeTruthy();
  await expect(Promise.resolve([1, 2])).resolves.toHaveLength(2);
});
test("rejects with a thrown non-Error", () => {
  throw "just a string";
});
