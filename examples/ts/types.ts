// Faqat turlar: runtime'da hech narsa qolmasligi kerak
export interface User {
  id: number;
  name: string;
  email?: string;
}

export type Role = "admin" | "user" | "guest";

export type Result<T, E = Error> =
  | { ok: true; value: T }
  | { ok: false; error: E };

export const DEFAULT_ROLE: Role = "user";
