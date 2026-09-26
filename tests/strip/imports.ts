import type { OnlyType } from "./a";
import { type T1, value, UsedAsTypeOnly } from "./b";
import Default, { named } from "./c";
import * as ns from "./d";
import "./side-effect";
interface Local { a: number }
type Alias = Local;
declare const ambient: number;
declare module "m" { export const x: number; }
namespace TypesOnly { export type X = number; }
export type { Alias };
export { Local, value };
export default interface Hidden {}
const x: UsedAsTypeOnly = value(named, ns);
