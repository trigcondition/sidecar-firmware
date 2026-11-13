// web-next/src/lib/seqgroup.test.ts - the NOTE EDIT lines for several notes.
import { describe, expect, it } from "vitest";
import { SUB, type SeqNote } from "./seq";
import { degreesPerOctave, deviceSelection, groupLines, pitchDetents, selectLines, setLines } from "./seqgroup";

const note = (step: number, pitch: number, flags = 0): SeqNote => ({ step, pitch, flags, micro: 0, vel: 100, len: SUB });
const a = { step: 0, pitch: 60 }, b = { step: 4, pitch: 62 }, c = { step: 8, pitch: 64 };

describe("seqgroup", () => {
  it("reads NOTE EDIT's selection from the view's flags (bit 1; bit 0 is mute)", () => {
    expect(deviceSelection([note(0, 60, 2), note(4, 62, 1), note(8, 64, 3)])).toEqual([a, c]);
  });

  it("selects one change with nsel, more with one nset line", () => {
    expect(selectLines(9, [], [a, b])).toEqual(["seqed=nset:9:1:0.60,4.62"]);
    expect(selectLines(9, [a, b], [a, b, c])).toEqual(["seqed=nsel:9:8:64:1"]);
    expect(selectLines(9, [a, b, c], [a, c])).toEqual(["seqed=nsel:9:4:62:0"]);
    expect(selectLines(9, [a, b], [a, b])).toEqual([]);
  });

  it("replaces the selection in one line when notes leave it", () => {
    expect(selectLines(9, [a, b], [c, { step: 9, pitch: 64 }])).toEqual(["seqed=nset:9:0:8.64,9.64"]);
    expect(selectLines(9, [a, b, c], [a])).toEqual(["seqed=nclr:9", "seqed=noff:9"]);
  });

  it("splits a big box select into lines the console takes (191 characters)", () => {
    const many = Array.from({ length: 60 }, (_, i) => ({ step: 100 + i, pitch: 100 }));
    const lines = setLines(11, many, false);
    expect(lines.length).toBeGreaterThan(1);
    expect(lines.every((l) => l.length <= 191)).toBe(true);
    expect(lines[0].startsWith("seqed=nset:11:0:")).toBe(true);
    expect(lines.slice(1).every((l) => l.startsWith("seqed=nset:11:1:"))).toBe(true);
    expect(lines.join(",").match(/\d+\.100/g)).toHaveLength(60);
  });

  it("leaves NOTE EDIT for one note or none (the page's own plain selection)", () => {
    expect(selectLines(9, [a, b], [a])).toEqual(["seqed=nclr:9", "seqed=noff:9"]);
    expect(selectLines(9, [a, b], [])).toEqual(["seqed=nclr:9", "seqed=noff:9"]);
    expect(selectLines(9, [], [a])).toEqual([]);
  });

  it("edits the whole selection in detents", () => {
    expect(groupLines.move(9, 2, -12)).toEqual(["seqed=nedit:9:0:2", "seqed=nedit:9:3:-12"]);
    expect(groupLines.move(9, 0, 1)).toEqual(["seqed=nedit:9:3:1"]);
    expect(groupLines.vel(9, -8)).toEqual(["seqed=nedit:9:4:-8"]);
    expect(groupLines.vel(9, 0)).toEqual([]);
    expect(groupLines.len(9, SUB / 2)).toEqual(["seqed=nedit:9:2:8"]);
    expect(groupLines.del(9)).toEqual(["seqed=ndel:9"]);
  });

  it("moves pitch by scale degree when QNT is on with a scale", () => {
    expect(degreesPerOctave(0, 1)).toBe(0);
    expect(degreesPerOctave(1, 0)).toBe(0);
    expect(degreesPerOctave(1, 1)).toBe(7);
    expect(degreesPerOctave(4, 2)).toBe(5);
    expect(pitchDetents(12, 7)).toBe(7);
    expect(pitchDetents(-24, 5)).toBe(-10);
    expect(pitchDetents(1, 7)).toBe(1);
    expect(pitchDetents(5, 0)).toBe(5);
    expect(groupLines.move(9, 0, 12, 7)).toEqual(["seqed=nedit:9:3:7"]);
  });
});
