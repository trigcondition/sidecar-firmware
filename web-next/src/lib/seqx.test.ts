// web-next/src/lib/seqx.test.ts - the seqtop lines, as the old page sends them.
import { describe, expect, it } from "vitest";
import { condText, fxText, humanLine, seedLine, stepFrom, stepLines, strumLine, trackLines } from "./seqx";
import { DIR, SPD } from "./seq";

describe("seqx", () => {
  it("names conditions and FX values", () => {
    expect([0, 1, 7, 9, 10, 11, 44, 143].map(condText)).toEqual(["---", "fill", "1st", "1:2", "2:2", "1:3", "1%", "100%"]);
    expect(fxText(13, 3)).toBe("ping");
    expect(fxText(11, (2 << 4) | 3)).toBe("4x3");
    expect(fxText(2, 7)).toBe("7");
  });
  it("builds a track's lines", () => {
    expect(trackLines(10, { spd: 3, dir: 0, rm: 1, rl: 48, ro: 2, rw: 2, ms: 3, mr: 8, mq: 2, ma: 3 }, SPD, DIR)).toEqual([
      "seq=spd:10:1/2", "seq=dir:10:fwd", "seq=rnd:10:mode=steps,low=48,oct=2,wght=chord", "seq=smod:10:src=3,range=8,quant=bar,aff=all",
    ]);
    expect(seedLine(42, false)).toBe("seq=seed:42");
    expect(seedLine(42, true)).toBe("seq=seed:free");
  });
  it("round-trips a step: read from seqx, unchanged locks not sent again", () => {
    const x = [{ t: 10, x: [{ s: 5, rt: [3, 2, 1, -2, 2, 2, 80], fx: [[13, 3], [0, 0]], r: 1, lc: [[4, 10]], sl: [4] }] }];
    const { f, was } = stepFrom(x, 10, 5);
    expect(f).toMatchObject({ rm: 2, rc: 3, rr: 2, rv: 1, rp: -2, ro: 2, rg: 80, f1: 13, v1: "ping", rnd: true, lp: 4, lc: "2:2", sl: true });
    expect(stepLines(10, 5, f, was)).toEqual([
      "seq=rtrg:10:5:cnt=3,rate=1,vcrv=up,pcrv=-2,mode=up,oct=2,gate=80", "seq=fx:10:5:1:DIR:ping", "seq=fx:10:5:2:off", "seq=rndstep:10:5:1",
    ]);
    expect(stepLines(10, 5, { ...f, lc: "1:2", sl: false, rm: -1 }, was)).toEqual([
      "seq=rtrg:10:5:off", "seq=fx:10:5:1:DIR:ping", "seq=fx:10:5:2:off", "seq=rndstep:10:5:1", "seq=lcond:10:5:4:1:2", "seq=slide:10:5:4:0",
    ]);
    expect(stepFrom(x, 10, 6).f).toMatchObject({ rm: -1, f1: 0, rnd: false, lp: 0 });
  });
  it("strum and humanise", () => {
    expect(strumLine(9, 0, 15, 12)).toBe("seqed=strum:9:0:15:12");
    expect(humanLine(9, 0, 31, 8, 20, 7)).toBe("seqed=human:9:0:31:8:20:7");
  });
});
