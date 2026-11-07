// web-next/src/lib/seqsel.ts - several notes selected in the piano roll: the rubber band's hit test, the selection's
// keys, a group move kept inside the track and 0..127, and a queue that sends lines one after another. The lines
// themselves are lib/seqgroup.ts's. Pure (seqsel.test.ts).
import { SUB, type SeqNote } from "./seq";

/** A note's place: its step and pitch (as the Sidecar's per-note command lines name it). */
export interface NoteKey { step: number; pitch: number }
export const keyOf = (n: NoteKey): NoteKey => ({ step: n.step, pitch: n.pitch });
export const sameKey = (a: NoteKey, b: NoteKey) => a.step === b.step && a.pitch === b.pitch;

/** A point in the roll: s in steps (fractional), p in pitch (row p spans p..p+1). */
export interface RollPoint { s: number; p: number }

/** The notes the rectangle between a and b touches (any part of the note, its micro offset and length counted). */
export function inBox(notes: SeqNote[], a: RollPoint, b: RollPoint): SeqNote[] {
  const s0 = Math.min(a.s, b.s), s1 = Math.max(a.s, b.s), p0 = Math.min(a.p, b.p), p1 = Math.max(a.p, b.p);
  return notes.filter((n) => {
    const start = n.step + n.micro / SUB, end = start + n.len / SUB;
    return start < s1 && end > s0 && n.pitch < p1 && n.pitch + 1 > p0;
  });
}

/** keys with k taken out, or put in at the end when it was not there. */
export function toggleKey(keys: NoteKey[], k: NoteKey): NoteKey[] {
  return keys.some((x) => sameKey(x, k)) ? keys.filter((x) => !sameKey(x, k)) : [...keys, keyOf(k)];
}

/** keys and more, without doubles (the order kept: the newest last). */
export function addKeys(keys: NoteKey[], more: NoteKey[]): NoteKey[] {
  const out = [...keys];
  for (const k of more) if (!out.some((x) => sameKey(x, k))) out.push(keyOf(k));
  return out;
}

/** A group move by ds steps and dp pitches, cut short so no note leaves the track (length steps) or 0..127: the
 * notes keep their shape. */
export function groupDelta(notes: NoteKey[], ds: number, dp: number, length: number): { ds: number; dp: number } {
  if (!notes.length) return { ds: 0, dp: 0 };
  const s = notes.map((n) => n.step), p = notes.map((n) => n.pitch);
  const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
  return {
    ds: clamp(ds, -Math.min(...s), length - 1 - Math.max(...s)) || 0,
    dp: clamp(dp, -Math.min(...p), 127 - Math.max(...p)) || 0,
  };
}

/** Lines sent one after another, each awaited, none dropped: a selection's lines and the group edits after them must
 * all arrive and in order (lib/coalesce.ts keeps only the newest of a control and sends different controls side by
 * side). push() resolves when everything pushed so far has been sent. */
export function serial(send: (c: string) => Promise<void>) {
  const waiting: string[] = [];
  let running: Promise<void> | null = null;
  async function run() {
    for (let next = waiting.shift(); next !== undefined; next = waiting.shift()) await send(next);
    running = null;
  }
  return {
    push(lines: string[]) {
      waiting.push(...lines);
      running ??= run();
      return running;
    },
    get busy() {
      return running !== null;
    },
  };
}

/** The same notes, either order. */
export const sameSet = (a: NoteKey[], b: NoteKey[]) => a.length === b.length && a.every((k) => b.some((x) => sameKey(x, k)));
