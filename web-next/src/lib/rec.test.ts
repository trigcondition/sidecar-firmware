// web-next/src/lib/rec.test.ts
import { describe, expect, it } from "vitest";
import { REC_GROUPS, asampLines, hms, natCmp, recBytesPerSec, recCmd, recDestinations, recLoad, recWarn, sampLines, takeFile, takeNameOk, takeRoots } from "./rec";

describe("rec", () => {
  it("has the old page's 25 channels", () => {
    const ids = REC_GROUPS.flatMap((g) => g.ch.map((c) => c[0])).sort((a, b) => a - b);
    expect(ids).toEqual(Array.from({ length: 25 }, (_, i) => i));
  });
  it("works out what a selection needs", () => {
    expect(recBytesPerSec([0], 24)).toBe(2 * 3 * 48000); // stereo master
    expect(recBytesPerSec([1, 2, 3], 16)).toBe(3 * 2 * 48000);
    expect(recLoad(1024, 2048)).toBe(50);
    expect(recLoad(1024, 0)).toBe(0);
    expect(recWarn(1500, 2000)).toBe(75);
    expect(recWarn(1000, 2000)).toBe(0);
  });
  it("lists destinations and take roots", () => {
    const vols = [{ name: "SD", state: "ok" }, { name: "USB", state: "mounted" }, { name: "USB2", state: "gone" }];
    expect(recDestinations(vols, "SD")).toEqual([{ name: "SD", ok: true }, { name: "USB", ok: true }]);
    expect(recDestinations(vols, "USB2")).toEqual([{ name: "SD", ok: true }, { name: "USB", ok: true }, { name: "USB2", ok: false }]);
    expect(takeRoots(vols)).toEqual([["SD", "/Recordings", ""], ["USB", "/USB/Recordings", "USB:"]]);
  });
  it("names, times and files", () => {
    expect(hms(3725)).toBe("1:02:05");
    expect(takeNameOk("set_1-b") && takeNameOk("") && !takeNameOk("a b") && !takeNameOk("x".repeat(25))).toBe(true);
    expect(["take_10", "take_2", "take_1"].sort(natCmp)).toEqual(["take_1", "take_2", "take_10"]);
    expect(takeFile("03_BD.wav")).toEqual({ nn: 3, ch: "BD" });
    expect(takeFile("notes.txt")).toBeNull();
  });
  it("builds the command lines", () => {
    expect(recCmd.ch([3, 0, 1])).toBe("rec=ch:0,1,3");
    expect(recCmd.ch([])).toBe("rec=ch:none");
    expect(recCmd.play("USB:take_004", 3)).toBe("play=start:USB:take_004/3");
    expect(sampLines({ src: "in", start: "thr", thr: -30, len: 0, slices: "trans" })).toEqual(["samp=src:in", "samp=start:thr:30", "samp=len:0", "samp=slices:trans", "samp=arm"]);
    expect(asampLines({ src: " fm:0", notes: "c2:c6:3", vel: "100", hold: 1500, tail: 2500, name: "" })).toEqual([
      "asamp=src:fm:0", "asamp=notes:c2:c6:3", "asamp=vel:100", "asamp=hold:1500", "asamp=tail:2500", "asamp=name:auto", "asamp=start",
    ]);
  });
});
