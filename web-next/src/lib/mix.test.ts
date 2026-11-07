// web-next/src/lib/mix.test.ts
import { describe, expect, it } from "vitest";
import { EQ_FLAT, eqDb, eqPath, eqText, insertText, mixCmd, strips } from "./mix";

const status = {
  machines: ["plaits", "juno", "daisy", "dsps", "fm", "dd", "daisy", "plaits"],
  fx: [{ type: "RINGS", post: 1, send: [] }, { type: "CLOUD", post: 0, send: [] }],
  ins: {
    kit: 1,
    route3: 0,
    c: [
      { n: "V1", s: [{ t: "FILTER", p: [80, 20, 0, 0], l: [80, 20, 0, 0] }, { t: "OFF", p: [0, 0, 0, 0], l: [0, 0, 0, 0] }], peq: [0, 0, 80, 64], send: [10, 0, 0] },
      ...Array.from({ length: 10 }, (_, i) => ({ n: `X${i}`, s: [], peq: [0, 0, 64, 64], ...(i < 7 ? { send: [0, 0, 0] } : {}) })),
    ],
    out: Array.from({ length: 8 }, () => ({ route: "trk", lev: 100, pan: 64, wid: 64, dec: 0, open: 0 })),
    mfx: { byp: 0, src: "ret", lev: 100, trim: 0, gr: 0, s: [{ t: "COMP", p: [1, 2, 3, 4], l: ["THR", "RAT", "SPD", "GAIN"], v: ["1", "2", "3", "4"], mix: 127, pair: 0 }], peq: [0, 0, 64, 64] },
  },
};

describe("mix", () => {
  it("builds the 12 strips", () => {
    const s = strips(status, [0.5, ...Array(7).fill(0), null, null, null, 0.8]);
    expect(s.map((x) => x.id)).toEqual(["V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "B1", "B2", "B3", "M"]);
    expect(s[0].name).toBe("BD PLAITS");
    expect(s[8].name).toBe("Send 1 · RINGS");
    expect(s[10].name).toBe("FX bus");
    expect(s[0].sends).toEqual([10, 0, 0]);
    expect(s[0].out?.lev).toBe(100);
    expect(s[0].level).toBe(0.5);
    expect(s[8].level).toBeNull();
    expect(s[11].slots[0].t).toBe("COMP");
    expect(insertText(s[0])).toBe("FILTER");
    expect(insertText(s[1])).toBe("—");
  });

  it("survives a status without ins (an older build)", () => {
    const s = strips({ machines: [] }, null);
    expect(s).toHaveLength(12);
    expect(s[0].peq).toEqual(EQ_FLAT);
  });

  it("draws the post EQ", () => {
    expect(eqDb(1000, EQ_FLAT)).toBeCloseTo(0, 5);
    expect(eqDb(40, [0, 0, 128, 64])).toBeGreaterThan(14); // a full low shelf
    expect(eqDb(20000, [0, 0, 64, 0])).toBeLessThan(-14); // a cut high shelf
    expect(eqDb(25, [64, 0, 64, 64])).toBeLessThan(-20); // the HPF at ~640 Hz takes 25 Hz off
    expect(eqDb(20000, [0, 64, 64, 64])).toBeLessThan(-20);
    const p = eqPath(EQ_FLAT, 60, 24);
    expect(p.startsWith("M0.0 12.0")).toBe(true);
    expect(eqText(EQ_FLAT)).toBe("flat");
    expect(eqText([64, 0, 96, 64])).toMatch(/^high-pass \d+ Hz, low shelf \+7.5 dB$/);
  });

  it("builds the command lines", () => {
    const [v1, , , , , , , , b1, , , m] = strips(status, null);
    expect(mixCmd.type(v1, 1, "COMP")).toBe("ins=V1:B:type:COMP");
    expect(mixCmd.param(v1, 0, 2, 200)).toBe("ins=V1:A:p3:127");
    expect(mixCmd.eq(v1, 0, 40)).toBe("ins=V1:eq:hpf:40");
    expect(mixCmd.eq(b1, 1, 90)).toBe("ins=B1:eq:lpf:90");
    expect(mixCmd.eq(m, 2, 70)).toBe("mfx=eq:low:70");
    expect(mixCmd.eqOff(v1)).toBe("ins=V1:eq:off");
    expect(mixCmd.send(v1, 2, 64)).toBe("ins=V1:send:3:64");
    expect(mixCmd.out(v1, "lev", 90)).toBe("out=V1:lev:90");
    expect(mixCmd.route(v1, "post")).toBe("out=V1:post");
    expect(mixCmd.type(m, 0, "RINGS")).toBe("mfx=1:type:RINGS");
    expect(mixCmd.param(m, 3, 0, 5)).toBe("mfx=4:p1:5");
    expect(mixCmd.mfxTrim(-30)).toBe("mfx=trim:-24");
  });
});
