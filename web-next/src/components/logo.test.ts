// web-next/src/components/logo.test.ts - the "Docked" geometry and the favicon file made from it.
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { DRIFT_ORDERS, LOGO_COLOURS, LOGO_REACH, dotDrift, faviconSvg, gradeColour, logoDots } from "./logo";
import { tokenBlock } from "../styles/readTokens";

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
  it("grades the dots coral at the centre to lavender at the rim", () => {
    const g = logoDots().map((p) => gradeColour(p.d));
    expect(new Set(g)).toEqual(new Set([1, 2, 3, 4]));
    expect(gradeColour(0)).toBe(1);
    expect(gradeColour(LOGO_REACH * 0.99)).toBe(4);
  });
  it("gives each dot its own drift: seeded, 6..14 s a fade, every order used", () => {
    const d = logoDots().map((_, i) => dotDrift(i));
    expect(dotDrift(3)).toEqual(dotDrift(3));
    for (const x of d) {
      expect(x.fade).toBeGreaterThanOrEqual(6);
      expect(x.fade).toBeLessThanOrEqual(14);
      expect(x.phase).toBeGreaterThanOrEqual(0);
      expect(x.phase).toBeLessThan(x.fade * LOGO_COLOURS);
    }
    expect(new Set(d.map((x) => x.order)).size).toBe(DRIFT_ORDERS.length);
    expect(new Set(d.map((x) => x.fade)).size).toBeGreaterThan(d.length / 2);
    for (const o of DRIFT_ORDERS) expect([...o].sort()).toEqual([1, 2, 3, 4, 5]);
  });
  it("public/favicon.svg is current (node scripts/favicon.mjs)", () => {
    const t = tokenBlock(readFileSync(new URL("../styles/tokens.css", import.meta.url), "utf8"), ':root[data-theme="standard"]');
    expect(readFileSync(new URL("../../public/favicon.svg", import.meta.url), "utf8")).toBe(faviconSvg({ bg: t.bg, accent: t.accent, logo: [1, 2, 3, 4].map((n) => t[`logo-${n}`]) }));
  });
});
