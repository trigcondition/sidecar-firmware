// web-next/src/lib/voice.ts - the "voices" topic (pi/engine/voiceview_json.h): the touchscreen's VOICE page for the
// selected track, and what the page draws from it. Pure (voice.test.ts).
export const TRACKS = ["BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "T9", "T10", "T11", "T12"];

export interface FmPanel {
  name: string; alg: number; fb: number; cols: number; levels: number;
  col: number[]; lev: number[]; mod: number[]; car: number; fbSrc: number; fbDst: number; out: number[]; freq: string[];
}
export interface MultiPanel { inst: string; ready: boolean; lo: number[]; hi: number[]; root: number[]; vel: number[]; keys: number[] }
export interface DaisyPanel { model: number; ratio: number[]; ring: number[] }
export interface VoiceView {
  valid: boolean; track?: number; machine?: number; sidecar?: boolean; name?: string; val?: number[]; label?: string[];
  pattern?: number; locks?: number[]; trig?: number[]; mode?: "plain" | "fm" | "multi" | "daisy";
  fm?: FmPanel; multi?: MultiPanel; daisy?: DaisyPanel; gen?: number;
}

/** P1..P8 that have a label (an unused parameter has none). A Sidecar voice whose labels all came back empty (an older
 *  Pi) still shows its eight, named P1..P8. */
export function params(v: VoiceView): { n: number; label: string; value: number }[] {
  const named = (v.label ?? []).some((l) => l.trim() !== "");
  if (!named && v.sidecar && v.val?.length) return v.val.map((value, i) => ({ n: i + 1, label: `P${i + 1}`, value }));
  return (v.label ?? []).map((l, i) => ({ n: i + 1, label: l, value: v.val?.[i] ?? 0 })).filter((p) => p.label.trim() !== "");
}

/** The FM algorithm as boxes on a grid: each operator's box (column, row: carriers on the bottom row), its lines to the
 * operators it modulates, the feedback loop. Coordinates in grid units (the caller scales). */
export interface FmOp { n: number; x: number; y: number; carrier: boolean; out: number; freq: string }
export function fmLayout(f: FmPanel): { ops: FmOp[]; links: [number, number][]; fb: number | null; w: number; h: number } {
  const levels = Math.max(1, f.levels);
  const ops = f.col.map((c, k) => ({ n: k + 1, x: c, y: levels - 1 - (f.lev[k] ?? 0), carrier: !!(f.car & (1 << k)), out: f.out[k] ?? 0, freq: f.freq[k] ?? "" }));
  const links: [number, number][] = [];
  f.mod.forEach((bits, k) => { for (let m = 0; m < 6; m++) if (bits & (1 << m) && m !== k) links.push([m, k]); });
  return { ops, links, fb: f.fbSrc >= 0 ? f.fbSrc : null, w: Math.max(1, f.cols), h: levels };
}

const MODELS = ["MODAL", "STRING", "RESON"];
export const daisyModel = (m: number) => MODELS[m] ?? "?";
/** The resonator's modes as lines over 0..1: x where its frequency sits (log, up to 20x the fundamental), its height how
 * long it rings. */
export function daisyLines(d: DaisyPanel): { x: number; h: number }[] {
  return d.ratio.map((r, i) => ({ x: Math.min(1, Math.max(0, Math.log(Math.max(r, 100) / 100) / Math.log(20))), h: (d.ring[i] ?? 0) / 255 }));
}

/** MULTISAMPLE's key zones over the span they cover (0..1 each). */
export function zoneBars(m: MultiPanel): { lo: number; hi: number; x0: number; x1: number }[] {
  if (!m.lo.length) return [];
  const a = Math.min(...m.lo), b = Math.max(...m.hi) + 1;
  return m.lo.map((lo, i) => ({ lo, hi: m.hi[i], x0: (lo - a) / (b - a), x1: (m.hi[i] + 1 - a) / (b - a) }));
}

export const trackName = (t: number | undefined) => (t === undefined || t < 0 ? "–" : `T${t + 1} ${TRACKS[t] ?? ""}`.trim());
/** voicepick=T (0..11; -1: follow the track the Rytm selects) */
export const pickLine = (t: number) => `voicepick=${t >= 0 && t < 12 ? Math.round(t) : -1}`;
