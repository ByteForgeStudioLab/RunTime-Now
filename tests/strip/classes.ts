abstract class Base<T = unknown> implements Iterable<T> {
  [key: string]: unknown;
  protected abstract name: string;
  declare tag: string;
  private readonly id?: number;
  static count: number = 0;
  constructor(public x: number, protected readonly y = 2) {}
  abstract area(): number;
  overload(a: string): void;
  overload(a: number): void;
  overload(a: unknown): void {}
  get size(): number { return 1; }
  *[Symbol.iterator](): Iterator<T> {}
}
class Child extends Base<number> {
  constructor(private z: number) {
    super(1)
    this.z;
  }
  override area(): number { return 0; }
}
