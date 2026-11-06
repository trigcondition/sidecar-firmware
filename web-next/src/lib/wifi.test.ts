// web-next/src/lib/wifi.test.ts
import { describe, expect, it } from "vitest";
import { bars, checkJoin } from "./wifi";

describe("checkJoin (wifi_conf.cpp check_join)", () => {
  it("accepts good input", () => {
    expect(checkJoin("Studio", "12345678", false)).toBeNull();
    expect(checkJoin("x".repeat(32), "a".repeat(63), false)).toBeNull();
    expect(checkJoin("Café", "f".repeat(64), false)).toBeNull();
    expect(checkJoin("Guest", "", true)).toBeNull();
  });
  it("refuses bad input", () => {
    expect(checkJoin("", "12345678", false)).toMatch(/1 to 32/);
    expect(checkJoin("é".repeat(17), "12345678", false)).toMatch(/1 to 32/); // 34 bytes
    expect(checkJoin("a\tb", "12345678", false)).toMatch(/control/);
    expect(checkJoin("a", "1234567", false)).toMatch(/8 to 63/);
    expect(checkJoin("a", "g".repeat(64), false)).toMatch(/8 to 63/); // 64 but not hex
    expect(checkJoin("a", "pässwörd", false)).toMatch(/printable/);
    expect(checkJoin("a", "12345678", true)).toMatch(/no password/);
  });
  it("bars", () => {
    expect([-40, -60, -70, -80, -90].map(bars)).toEqual([4, 3, 2, 1, 0]);
  });
});
