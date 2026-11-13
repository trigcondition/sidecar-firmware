// web-next/src/lib/voice.test.ts
import { describe, expect, it } from "vitest";
import { daisyLines, daisyModel, fmLayout, params, pickLine, trackName, zoneBars } from "./voice";

describe("voice", () => {
  it("lists the labelled parameters", () => {
    expect(params({ valid: true, label: ["LEV", "", "TUN"], val: [1, 2, 3] })).toEqual([{ n: 1, label: "LEV", value: 1 }, { n: 3, label: "TUN", value: 3 }]);
    expect(params({ valid: false })).toEqual([]);
  });
  it("lays out an FM algorithm", () => {
    const l = fmLayout({ name: "", alg: 4, fb: 6, cols: 3, levels: 2, col: [0, 0, 1, 1, 2, 2], lev: [0, 1, 0, 1, 0, 1], mod: [2, 0, 8, 0, 32, 0], car: 21, fbSrc: 5, fbDst: 5, out: [99, 1, 2, 3, 4, 5], freq: ["1.00"] });
    expect(l.ops[0]).toEqual({ n: 1, x: 0, y: 1, carrier: true, out: 99, freq: "1.00" });
    expect(l.ops[1]).toMatchObject({ x: 0, y: 0, carrier: false, freq: "" });
    expect(l.links).toEqual([[1, 0], [3, 2], [5, 4]]);
    expect(l.fb).toBe(5);
    expect([l.w, l.h]).toEqual([3, 2]);
  });
  it("draws DAISYSP's modes and MULTISAMPLE's zones", () => {
    expect(daisyModel(1)).toBe("STRING");
    const d = daisyLines({ model: 0, ratio: [100, 2000, 5000], ring: [255, 0, 51] });
    expect(d[0]).toEqual({ x: 0, h: 1 });
    expect(d[1].x).toBeCloseTo(1);
    expect(d[2]).toEqual({ x: 1, h: 0.2 });
    expect(zoneBars({ inst: "", ready: true, lo: [36, 48], hi: [47, 59], root: [], vel: [], keys: [] })).toEqual([
      { lo: 36, hi: 47, x0: 0, x1: 0.5 }, { lo: 48, hi: 59, x0: 0.5, x1: 1 },
    ]);
  });
  it("names tracks and picks one", () => {
    expect(trackName(4)).toBe("T5 LT");
    expect(trackName(undefined)).toBe("–");
    expect(pickLine(3)).toBe("voicepick=3");
    expect(pickLine(-1)).toBe("voicepick=-1");
    expect(pickLine(12)).toBe("voicepick=-1");
  });
});

describe("params without labels", () => {
  it("names a Sidecar voice's eight P1..P8 when every label is empty", () => {
    const v = { valid: true, sidecar: true, val: [45, 64, 64, 64, 64, 64, 64, 100], label: ["", "", "", "", "", "", "", ""] };
    expect(params(v).map((p) => p.label)).toEqual(["P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"]);
    expect(params(v)[7].value).toBe(100);
  });
  it("keeps a Rytm machine's unlabelled parameters hidden", () => {
    expect(params({ valid: true, sidecar: false, val: [1, 2], label: ["", ""] })).toEqual([]);
  });
});
