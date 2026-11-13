// web-next/src/lib/seqsel.test.ts - several notes selected in the piano roll.
import { describe, expect, it } from "vitest";
import { SUB, type SeqNote } from "./seq";
import { addKeys, groupDelta, inBox, sameSet, serial, toggleKey } from "./seqsel";

const note = (step: number, pitch: number, len = SUB, micro = 0): SeqNote => ({ step, pitch, len, micro, vel: 100, flags: 0 });

describe("seqsel", () => {
  it("finds the notes a rectangle touches, either way it was drawn", () => {
    const notes = [note(0, 60), note(2, 62), note(4, 64, SUB * 3), note(8, 70)];
    expect(inBox(notes, { s: 0.5, p: 60.5 }, { s: 2.2, p: 62.2 })).toEqual([notes[0], notes[1]]);
    expect(inBox(notes, { s: 2.2, p: 62.2 }, { s: 0.5, p: 60.5 })).toEqual([notes[0], notes[1]]);
    // a long note is caught by its tail
    expect(inBox(notes, { s: 6.5, p: 64.2 }, { s: 6.8, p: 64.8 })).toEqual([notes[2]]);
    // touching an edge is not touching: the row and the step end there
    expect(inBox(notes, { s: 1, p: 61 }, { s: 2, p: 62 })).toEqual([]);
  });

  it("counts a note's micro offset", () => {
    const late = note(3, 60, SUB / 4, 80); // 3.83 .. 4.08
    expect(inBox([late], { s: 3, p: 60 }, { s: 3.5, p: 61 })).toEqual([]);
    expect(inBox([late], { s: 4, p: 60 }, { s: 4.05, p: 61 })).toEqual([late]);
  });

  it("toggles and adds selection keys without doubles", () => {
    const a = { step: 0, pitch: 60 }, b = { step: 1, pitch: 61 };
    expect(toggleKey([a], b)).toEqual([a, b]);
    expect(toggleKey([a, b], a)).toEqual([b]);
    expect(addKeys([a], [b, a, b])).toEqual([a, b]);
  });

  it("keeps a group move inside the track and 0..127", () => {
    const notes = [note(1, 10), note(14, 120)];
    expect(groupDelta(notes, 3, 2, 16)).toEqual({ ds: 1, dp: 2 });
    expect(groupDelta(notes, -5, -20, 16)).toEqual({ ds: -1, dp: -10 });
    expect(groupDelta(notes, 0, 12, 16)).toEqual({ ds: 0, dp: 7 });
    expect(groupDelta([], 3, 3, 16)).toEqual({ ds: 0, dp: 0 });
  });

  it("sends a group's lines one at a time and in order, losing none", async () => {
    const sent: string[] = [];
    const gates: (() => void)[] = [];
    const q = serial((c) => (sent.push(c), new Promise<void>((ok) => gates.push(ok))));
    const done = q.push(["seqed=del:0:3:60", "seqed=del:0:3:62", "seqed=del:0:3:64"]);
    expect(q.busy).toBe(true);
    expect(sent).toEqual(["seqed=del:0:3:60"]);
    while (gates.length) {
      gates.shift()!();
      await Promise.resolve();
      await Promise.resolve();
    }
    await done;
    expect(sent).toEqual(["seqed=del:0:3:60", "seqed=del:0:3:62", "seqed=del:0:3:64"]);
    expect(q.busy).toBe(false);
  });

  it("compares selections as sets", () => {
    const a = { step: 0, pitch: 60 }, b = { step: 1, pitch: 61 };
    expect(sameSet([a, b], [b, a])).toBe(true);
    expect(sameSet([a], [a, b])).toBe(false);
    expect(sameSet([], [])).toBe(true);
  });
});
