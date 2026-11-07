// web-next/src/lib/seqgroup.ts - the lines that select several notes on the Sidecar and edit them as one: the Rytm's
// NOTE EDIT selection, through the touch editor's nsel / nclr / nedit / ndel / noff lines. NOTE EDIT edits the whole
// selection at once (no note lands on another's old place), one undo step a line, and the view marks its notes
// (flags bit 1), so a selection made on the Rytm shows here too. The one place to change if group edits go another
// way. Pure (seqgroup.test.ts).
import { SUB, type SeqNote } from "./seq";
import { keyOf, sameKey, type NoteKey } from "./seqsel";

/** The view's notes in NOTE EDIT's selection. */
export const deviceSelection = (notes: SeqNote[]): NoteKey[] => notes.filter((n) => n.flags & 2).map(keyOf);

/** nedit's parameters. */
export const EDIT = { pos: 0, nudge: 1, len: 2, pitch: 3, vel: 4 } as const;
/** What one detent of nedit moves (core/seq NOTE EDIT, docs/SEQUENCER.md): a step, a semitone (a scale degree when
 * the pattern's QNT is on with a scale), 1/16 step of length, one of velocity. */
export const DETENT = { pos: 1, pitch: 1, len: SUB / 16, vel: 1 };

/** The notes an octave holds in each scale (core/seq/music.cpp kMasks, CHR..WHOLE). */
const DEGREES = [12, 7, 7, 7, 5, 7, 7, 7, 7, 7, 7, 5, 6, 6];
/** Scale degrees an octave when PTCH moves by degree (QNT on with a scale), else 0: PTCH moves by semitone. */
export const degreesPerOctave = (scale: number, qnt: number): number => (qnt && scale > 0 ? DEGREES[scale] ?? 7 : 0);
/** A pitch change in semitones as PTCH detents: degrees with a scale (an octave exactly, else the nearest, never 0). */
export function pitchDetents(dp: number, deg: number): number {
  if (!deg || !dp) return dp;
  const d = dp % 12 === 0 ? (dp / 12) * deg : Math.round((dp * deg) / 12);
  return d || Math.sign(dp);
}
/** The longest console line (pi/engine/shared.h TCmd) */
const MAX_LINE = 191;

const edit = (T: number, e: number, d: number) => `seqed=nedit:${T}:${e}:${Math.round(d)}`;

/** From the selection the Sidecar has (from) to the one wanted (to). One note or none is the page's own (a plain
 * selection as before): NOTE EDIT is then cleared and left. */
export function selectLines(T: number, from: NoteKey[], to: NoteKey[]): string[] {
  const want = to.length > 1 ? to : [];
  if (!want.length) return from.length ? [`seqed=nclr:${T}`, `seqed=noff:${T}`] : [];
  const off = from.filter((k) => !want.some((w) => sameKey(w, k)));
  const on = want.filter((k) => !from.some((f) => sameKey(f, k)));
  if (off.length + on.length === 0) return [];
  // one change: a note in or out
  if (off.length + on.length === 1)
    return off.length ? [`seqed=nsel:${T}:${off[0].step}:${off[0].pitch}:0`] : [`seqed=nsel:${T}:${on[0].step}:${on[0].pitch}:1`];
  // more: nset, the whole selection (or only the additions), as many lines as the console's length needs
  return setLines(T, off.length ? want : on, !off.length);
}

/** nset lines: the first replaces the selection (add false) or adds, the rest add. */
export function setLines(T: number, keys: NoteKey[], add: boolean): string[] {
  const out: string[] = [];
  let line = "";
  for (const k of keys) {
    const item = `${k.step}.${k.pitch}`;
    if (line && line.length + 1 + item.length > MAX_LINE) { out.push(line); line = ""; }
    line = line ? `${line},${item}` : `seqed=nset:${T}:${add || out.length ? 1 : 0}:${item}`;
  }
  if (line) out.push(line);
  return out;
}

/** The group edits: every line acts on the whole selection. */
export const groupLines = {
  move: (T: number, ds: number, dp: number, deg = 0) => [...(ds ? [edit(T, EDIT.pos, ds / DETENT.pos)] : []), ...(dp ? [edit(T, EDIT.pitch, pitchDetents(dp, deg) / DETENT.pitch)] : [])],
  vel: (T: number, d: number) => (Math.round(d / DETENT.vel) ? [edit(T, EDIT.vel, d / DETENT.vel)] : []),
  len: (T: number, d: number) => (Math.round(d / DETENT.len) ? [edit(T, EDIT.len, d / DETENT.len)] : []),
  del: (T: number) => [`seqed=ndel:${T}`],
};
