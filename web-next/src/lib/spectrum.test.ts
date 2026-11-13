import { describe, expect, it } from "vitest";
import { Ballistics, FALL_DB_S, FLOOR_DB, columnsPath, parseSpectrum } from "./spectrum";

describe("parseSpectrum", () => {
  it("reads the bytes as dB across the payload's range", () => {
    const f = parseSpectrum({ strip: "V3", hz: [20, 20000], db: [-90, 0], b: [0, 255, 51] });
    expect(f?.strip).toBe("V3");
    expect(f?.hz).toEqual([20, 20000]);
    expect(f?.bands[0]).toBe(-90);
    expect(f?.bands[1]).toBe(0);
    expect(f?.bands[2]).toBeCloseTo(-72, 5);
  });
  it("defaults the ranges, clamps the bytes and takes a null strip", () => {
    const f = parseSpectrum({ strip: null, b: [300, -4] });
    expect(f).toEqual({ strip: null, hz: [20, 20000], bands: [0, -90] });
  });
  it("refuses what is not a frame", () => {
    for (const p of [null, 3, {}, { b: [] }, { b: ["x"] }, { b: "00ff" }]) expect(parseSpectrum(p)).toBeNull();
  });
});

describe("Ballistics", () => {
  it("rises fast and falls at 20 dB a second", () => {
    const b = new Ballistics();
    b.set([-10, -60]);
    expect(b.shown).toEqual([FLOOR_DB, FLOOR_DB]);
    b.step(100); // four rise times: within a fraction of a dB
    expect(b.shown[0]).toBeGreaterThan(-12);
    b.step(100);
    expect(b.shown[0]).toBe(-10);
    b.set([-80, -60]);
    b.step(500);
    expect(b.shown[0]).toBeCloseTo(-10 - FALL_DB_S / 2, 5);
    expect(b.step(3000)).toBe(true);
    expect(b.shown[0]).toBe(-80); // stops at the target
  });
  it("moves smoothly between slow frames (5 a second): no step bigger than the fall per frame", () => {
    const b = new Ballistics();
    b.set([-20]);
    b.jump();
    let last = b.shown[0];
    let worst = 0;
    for (let t = 0; t < 2000; t += 16) {
      if (t % 200 === 0) b.set([t % 400 === 0 ? -50 : -20]);
      b.step(16);
      if (b.shown[0] < last) worst = Math.max(worst, last - b.shown[0]);
      last = b.shown[0];
    }
    expect(worst).toBeLessThanOrEqual((FALL_DB_S * 16) / 1000 + 1e-9);
  });
  it("settles: step says nothing moves once there", () => {
    const b = new Ballistics();
    b.set([-30, -40]);
    b.jump();
    expect(b.step(16)).toBe(false);
  });
  it("starts again from the floor when the band count changes", () => {
    const b = new Ballistics();
    b.set([-3]);
    b.jump();
    b.set([-3, -3]);
    expect(b.shown).toEqual([FLOOR_DB, FLOOR_DB]);
  });
});

describe("columnsPath", () => {
  it("one whole-dot column per band that shows, across the EQ's axis", () => {
    const d = columnsPath([-6, -90, -42], [20, 20000], 360, 140, 5);
    const cols = d.split("M").filter(Boolean);
    expect(cols.length).toBe(2); // the silent band draws nothing
    expect(cols[0]).toBe("0.0 0.0H120.0V140H0.0Z"); // full: 28 rows of 5
    expect(cols[1]).toMatch(/^240\.0 70\.0H360\.0V140H240\.0Z$/); // half way: 14 rows
  });
  it("is empty without bands", () => {
    expect(columnsPath([], [20, 20000], 360, 140, 5)).toBe("");
  });
});
