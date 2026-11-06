// web-next/src/components/logo.test.ts - the "Docked" geometry and the favicon file made from it.
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { LOGO_REACH, faviconSvg, logoDots } from "./logo";

describe("Docked logo", () => {
  it("keeps the dots within reach of the centre, larger towards it", () => {
    const dots = logoDots();
    expect(dots.length).toBeGreaterThan(20);
    expect(dots.length).toBeLessThan(49);
    for (const p of dots) expect(p.d).toBeLessThan(LOGO_REACH);
    const inner = dots.reduce((a, b) => (a.d < b.d ? a : b)), outer = dots.reduce((a, b) => (a.d > b.d ? a : b));
    expect(inner.r).toBeGreaterThan(outer.r);
    expect(Math.min(...logoDots(true).map((p) => p.r))).toBeGreaterThanOrEqual(0.8);
  });
  it("public/favicon.svg is current (node scripts/favicon.mjs)", () => {
    expect(readFileSync(new URL("../../public/favicon.svg", import.meta.url), "utf8")).toBe(faviconSvg());
  });
});
