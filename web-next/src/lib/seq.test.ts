// web-next/src/lib/seq.test.ts - the sequencer's helpers and the exact command lines (docs/TOUCH-EDITOR.md).
import { describe, expect, it } from "vitest";
import { cmd, isBlack, noteAt, noteName, parseView, pitchWindow, playStep, trackName } from "./seq";

describe("seq", () => {
  it("parses the wire's note arrays", () => {
    const v = parseView({ valid: true, track: 9, notes: [[4, -12, 60, 100, 96, 0], [1, 2]], steps: [] });
    expect(v?.notes).toEqual([{ step: 4, micro: -12, pitch: 60, vel: 100, len: 96, flags: 0 }]);
    expect(parseView({ valid: false })).toBeNull();
    expect(parseView(null)).toBeNull();
  });

  it("names notes as the Rytm does", () => {
    expect(noteName(60)).toBe("C4");
    expect(noteName(61)).toBe("C#4");
    expect(noteName(0)).toBe("C-1");
    expect(isBlack(61) && !isBlack(60)).toBe(true);
    expect(trackName(9)).toBe("T10");
  });

  it("finds the note at a cell, also one held over it", () => {
    const n = [{ step: 2, micro: 0, pitch: 60, vel: 100, len: 96 * 3, flags: 0 }];
    expect(noteAt(n, 2, 60)).toBe(n[0]);
    expect(noteAt(n, 4, 60)).toBe(n[0]);
    expect(noteAt(n, 5, 60)).toBeUndefined();
    expect(noteAt(n, 2, 61)).toBeUndefined();
  });

  it("puts the notes in the pitch window", () => {
    const top = pitchWindow([{ step: 0, micro: 0, pitch: 48, vel: 1, len: 1, flags: 0 }, { step: 0, micro: 0, pitch: 72, vel: 1, len: 1, flags: 0 }], 24);
    expect(top).toBe(72);
    expect(pitchWindow([], 24, 60)).toBe(72);
    expect(pitchWindow([{ step: 0, micro: 0, pitch: 126, vel: 1, len: 1, flags: 0 }], 24)).toBe(127);
  });

  it("builds the touch editor's lines", () => {
    expect(cmd.add(9, 4, 60, 0, 100, 96)).toBe("seqed=add:9:4:60:0:100:96");
    expect(cmd.add(9, 4, 200, -500, 0, 0)).toBe("seqed=add:9:4:127:-96:1:1"); // clamped
    expect(cmd.del(9, 4, 60)).toBe("seqed=del:9:4:60");
    expect(cmd.move(9, 4, 60, 5, 62, 12)).toBe("seqed=move:9:4:60:5:62:12");
    expect(cmd.len(9, 4, 60, 48)).toBe("seqed=len:9:4:60:48");
    expect(cmd.vel(9, 4, 60, 140)).toBe("seqed=vel:9:4:60:127");
    expect(cmd.chord(9, 0, 60, 1, 0, 100, 96, 0)).toBe("seqed=chord:9:0:60:1:0:100:96:0");
    expect(cmd.undo(9)).toBe("seqed=undo:9");
    expect(cmd.show(8)).toBe("seqed=show:8");
  });

  it("builds the old page's seq= lines (tracks 1..12)", () => {
    expect(cmd.swing(9, 62)).toBe("seq=swing:10:62");
    expect(cmd.poly(9, { voices: 4, alloc: 1, unison: 1, detune: 0, spread: 0 })).toBe("seq=poly:10:voices=4,alloc=rotate,unison=1,detune=0,spread=0");
    expect(cmd.key(9, 2, 1)).toBe("seq=key:A:MIN:edit");
    expect(cmd.seed("free")).toBe("seq=seed:free");
    expect(cmd.spd(9, 3)).toBe("seq=spd:10:1/2");
    expect(cmd.dir(9, 3)).toBe("seq=dir:10:ping");
  });

  it("the play head only for the shown track", () => {
    expect(playStep({ pos: 4500, track: 9, transport: 1 }, 9)).toBe(4.5);
    expect(playStep({ pos: 4500, track: 8, transport: 1 }, 9)).toBeNull();
    expect(playStep({ pos: -1, track: 9, transport: 0 }, 9)).toBeNull();
  });
});
