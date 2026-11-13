// web-next/src/lib/seq.ts - the sequencer's data and its command lines. The view is the "seq" topic
// (pi/engine/seqview_json.h: the touch pattern editor's view of the shown seq track); every edit is one of the touch
// editor's own command lines (docs/TOUCH-EDITOR.md, pi/ui/seq_touch.h) or the old page's seq= lines. T in seqed= is the
// Rytm track 0..11; t in seq= is 1..12. Lengths and micro offsets are in 1/96 step. Pure (seq.test.ts).

export const SUB = 96; // 1/96 step

export interface SeqNote {
  step: number;
  micro: number;
  pitch: number;
  vel: number;
  len: number;
  flags: number;
}
export interface SeqView {
  valid: boolean;
  track: number;
  tracks: number[];
  pattern: number;
  project: string;
  machine: string;
  length: number;
  pages: number;
  defaults: { note: number; vel: number; len: number };
  transport: number;
  bpm10: number;
  undo: number;
  redo: number;
  gen: number;
  chords: string[];
  swing: number;
  poly: { voices: number; alloc: number; unison: number; info: string };
  speed: number;
  dir: number;
  key: number;
  scale: number;
  qnt: number;
  notes: SeqNote[];
  steps: number[][];
}
export interface SeqPos {
  pos: number; // steps x 1000, -1 stopped
  track: number;
  transport: number;
  pattern?: number;
  bpm10?: number;
}

/** The topic's view with its notes as objects (the wire has [step, micro, pitch, vel, len, flags] arrays). */
export function parseView(raw: unknown): SeqView | null {
  const o = raw as Record<string, unknown> | null;
  if (!o || typeof o !== "object" || !o.valid) return null;
  const notes = Array.isArray(o.notes)
    ? (o.notes as number[][]).filter((n) => Array.isArray(n) && n.length >= 6).map(([step, micro, pitch, vel, len, flags]) => ({ step, micro, pitch, vel, len, flags }))
    : [];
  return { ...(o as unknown as SeqView), notes };
}

const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
/** MIDI 60 = C4 (the Rytm's and the touchscreen's naming). */
export function noteName(p: number): string {
  return `${NAMES[((p % 12) + 12) % 12]}${Math.floor(p / 12) - 1}`;
}
export const isBlack = (p: number) => [1, 3, 6, 8, 10].includes(((p % 12) + 12) % 12);

/** "T10" for Rytm track 9. */
export const trackName = (t: number) => `T${t + 1}`;

/** The note that sounds at (step, pitch): starting there, or held over it. */
export function noteAt(notes: SeqNote[], step: number, pitch: number): SeqNote | undefined {
  return (
    notes.find((n) => n.pitch === pitch && n.step === step) ??
    notes.find((n) => n.pitch === pitch && n.step < step && n.step * SUB + n.micro + n.len > step * SUB)
  );
}

/** A window of `rows` pitches (top first) that holds the notes, or centres on the default note. */
export function pitchWindow(notes: SeqNote[], rows: number, fallback = 60): number {
  if (!notes.length) return Math.min(127, fallback + Math.floor(rows / 2));
  const lo = Math.min(...notes.map((n) => n.pitch)), hi = Math.max(...notes.map((n) => n.pitch));
  const mid = Math.round((lo + hi) / 2);
  return Math.max(rows - 1, Math.min(127, mid + Math.floor(rows / 2)));
}

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v)));

// ---- the touch editor's command lines (seqed=..., T = Rytm track 0..11)
export const cmd = {
  show: (T: number) => `seqed=show:${T}`,
  add: (T: number, s: number, p: number, micro: number, vel: number, len: number) =>
    `seqed=add:${T}:${s}:${clamp(p, 0, 127)}:${clamp(micro, -96, 96)}:${clamp(vel, 1, 127)}:${Math.max(1, Math.round(len))}`,
  del: (T: number, s: number, p: number) => `seqed=del:${T}:${s}:${p}`,
  move: (T: number, s: number, p: number, s2: number, p2: number, micro2: number) => `seqed=move:${T}:${s}:${p}:${s2}:${clamp(p2, 0, 127)}:${clamp(micro2, -96, 96)}`,
  len: (T: number, s: number, p: number, len: number) => `seqed=len:${T}:${s}:${p}:${Math.max(1, Math.round(len))}`,
  vel: (T: number, s: number, p: number, vel: number) => `seqed=vel:${T}:${s}:${p}:${clamp(vel, 1, 127)}`,
  chord: (T: number, s: number, root: number, chd: number, inv: number, vel: number, len: number, strum: number) =>
    `seqed=chord:${T}:${s}:${clamp(root, 0, 127)}:${chd}:${inv}:${clamp(vel, 1, 127)}:${Math.max(1, Math.round(len))}:${strum}`,
  undo: (T: number) => `seqed=undo:${T}`,
  redo: (T: number) => `seqed=redo:${T}`,
  // the old page's seq= lines (t = 1..12)
  swing: (T: number, n: number) => `seq=swing:${T + 1}:${clamp(n, 50, 80)}`,
  poly: (T: number, f: { voices: number; alloc: number; unison: number; detune: number; spread: number }) =>
    `seq=poly:${T + 1}:voices=${clamp(f.voices, 0, 8)},alloc=${POLY_ALLOC[f.alloc] ?? "reassign"},unison=${clamp(f.unison, 1, 8)},detune=${clamp(f.detune, 0, 127)},spread=${clamp(f.spread, 0, 127)}`,
  key: (key: number, scale: number, qnt: number) => `seq=key:${KEYS[clamp(key, 0, 11)]}:${SCALES[clamp(scale, 0, 13)]}:${QNT[clamp(qnt, 0, 2)]}`,
  seed: (seed: number | "free") => (seed === "free" ? "seq=seed:free" : `seq=seed:${clamp(seed, 0, 9999)}`),
  spd: (T: number, i: number) => `seq=spd:${T + 1}:${SPD[clamp(i, 0, 9)]}`,
  dir: (T: number, i: number) => `seq=dir:${T + 1}:${DIR[clamp(i, 0, 5)]}`,
};

// the old page's value lists (pi/engine/www/index.html SX_*, POLY_ALLOC)
export const KEYS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
export const SCALES = ["CHR", "MAJ", "MIN", "DOR", "PENT", "PHR", "LYD", "MIX", "LOC", "HMIN", "MMIN", "PMAJ", "BLUES", "WHOLE"];
export const QNT = ["off", "edit", "play"];
export const SPD = ["follow", "1/8", "1/4", "1/2", "3/4", "3/2", "2", "2/3", "5/4", "7/8"];
export const DIR = ["fwd", "rev", "pend", "ping", "rnd", "brown"];
export const POLY_ALLOC = ["reassign", "rotate", "reset"];

/** The play head in steps (float), or null when stopped / another track. */
export function playStep(p: SeqPos | null, track: number): number | null {
  if (!p || p.pos < 0 || p.track !== track) return null;
  return p.pos / 1000;
}

/** A drag on a note in the piano roll (dx, dy: how far the pointer went, in cells; y down): "move" by whole steps and
 * pitches (its micro offset kept), "len" its end in quarter steps (a quarter step at least). Kept inside the track
 * (length steps) and 0..127. null: it did not get anywhere. */
export type DragMode = "move" | "len";
export function dragResult(n: SeqNote, mode: DragMode, dx: number, dy: number, length: number):
  { ds: number; dp: number } | { len: number } | null {
  if (mode === "len") {
    const q = SUB / 4, len = Math.max(q, Math.round((n.len + dx * SUB) / q) * q);
    return len === n.len ? null : { len };
  }
  const s2 = Math.max(0, Math.min(length - 1, n.step + Math.round(dx))), p2 = Math.max(0, Math.min(127, n.pitch - Math.round(dy)));
  return s2 === n.step && p2 === n.pitch ? null : { ds: s2 - n.step, dp: p2 - n.pitch };
}
/** Whether a press at stepSub (in 1/96 step) grabs the note's end (its last quarter, at least a third of a step). */
export const grabsEnd = (n: SeqNote, stepSub: number) => stepSub >= n.step * SUB + n.micro + n.len - Math.max(SUB / 3, n.len / 4);
