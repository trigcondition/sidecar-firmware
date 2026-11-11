// web-next/src/lib/mix.test.ts
import { describe, expect, it } from "vitest";
import { EQ_FLAT, eqDb, eqPath, eqText, insertText, mixCmd, slotTypes, strips, typeBlocked, typeOutcome } from "./mix";

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

  it("offers the send TYPEs on a bus and the master, never a voice; blocks them while the room is full", () => {
    const st = JSON.parse(JSON.stringify(status));
    st.ins.c[10] = { n: "B3", s: [{ t: "RINGS", p: [50, 0, 80, 60], l: [50, 0, 80, 60], mix: 77, n4: ["NOTE", "STRC", "BRIT", "DAMP"] }, { t: "OFF", p: [0, 0, 0, 0], l: [0, 0, 0, 0] }], peq: [0, 0, 64, 64] };
    st.ins.mfx.fxcap = 4;
    st.ins.mfx.fxused = 4;
    const s = strips(st, null);
    const [v1, b3, m] = [s[0], s[10], s[11]];
    expect(slotTypes(v1)).not.toContain("RINGS");
    expect(slotTypes(b3)).toContain("RINGS");
    expect(slotTypes(b3)).not.toContain("AMP");
    expect(slotTypes(m)).toContain("AMP");
    expect(b3.room).toEqual({ used: 4, cap: 4 });
    expect(typeBlocked(b3, 1, "CLOUD")).toBe(true); // full, B holds none
    expect(typeBlocked(b3, 0, "CLOUD")).toBe(false); // A holds one: a change keeps its room
    expect(typeBlocked(b3, 1, "COMP")).toBe(false);
    expect(mixCmd.mix(b3, 0, 90)).toBe("ins=B3:A:mix:90");
    expect(mixCmd.mix(m, 2, 300)).toBe("mfx=3:mix:127");
    st.ins.mfx.fxused = 3;
    expect(typeBlocked(strips(st, null)[10], 1, "CLOUD")).toBe(false);
    expect(typeBlocked(strips(status, null)[10], 1, "CLOUD")).toBe(false); // an older build: no room reported
    // a TYPE sent while the room had space, then another page took the last one: the refusal, as the console words it
    const full = strips({ ...st, ins: { ...st.ins, mfx: { ...st.ins.mfx, fxused: 4 } } }, null)[10];
    expect(typeOutcome(full, 1, "CLOUD")).toEqual({ refused: "CLOUD: 4 send FX inserts at most, the master and the buses together (4 in use)" });
    expect(typeOutcome(full, 0, "RINGS")).toBe("taken");
    expect(typeOutcome(strips(st, null)[10], 1, "CLOUD")).toBe("wait"); // room left: not applied yet
    expect(typeOutcome(full, 1, "COMP")).toBe("wait"); // not a send TYPE: no room to refuse it for
  });
});
