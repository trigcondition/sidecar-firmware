// web-next/src/lib/lfo.ts - the LFOs: the pure parts are web/lfo.js's, shared with the old page and the Sidecar Mini's
// page (one source: its own tests in web/lfo.test.mjs); this adds the state's type and the command lines.
// (a plain .js module: its exports are given their types below)
import * as shared from "../../../web/lfo.js";

export interface LfoState {
  tgt: string;
  par: number;
  pn: string;
  pns?: string[]; // every param's name ("-" unused; the 2026-11 Pi): shown on every row of PAR
  np: number;
  wav: number;
  rate: number;
  rn: string;
  sync: number;
  trig: number;
  dep: number;
  base: number;
  des: string;
  ph: number; // 0..1000
}
export interface Lfos {
  n: number;
  pt?: number; // 1: two a track (the Pi's 26, LFO FINAL): l[2t] track t's A, l[2t + 1] its B
  kit: number;
  full?: number; // 1: the Rytm link is full (LFO_FULL)
  trk?: number; // the Rytm's selected track 0..12 (12 FX), -1 none
  l: LfoState[];
}

export const LFO_WAVES: string[] = shared.LFO_WAVES;
export const LFO_RETRIGS: string[] = shared.LFO_RETRIGS;
export const LFO_TARGETS: string[] = shared.LFO_TARGETS;
export const LFO_PRESETS: string[] = shared.LFO_PRESETS;
export const LFO_FULL: string = shared.LFO_FULL;
export const LFO_TRACKS: string[] = shared.LFO_TRACKS;
/** LFO n's (1-based) track 0..12, its slot (0 A, 1 B), its title, the targets it may have (a B: no Rytm target) */
export const lfoTrack: (n: number, pt?: number) => number = shared.lfoTrack;
export const lfoSlot: (n: number, pt?: number) => number = shared.lfoSlot;
export const lfoTitle: (n: number, pt?: number) => string = shared.lfoTitle;
export const lfoTargets: (n: number, pt?: number) => string[] = shared.lfoTargets;
export const lfoParams: (tgt: string, par: number, pn: string, pns?: string[]) => string[] = shared.lfoParams;
/** The PAR list's rows: a voice's the params its machine names (the stored one kept; none named: all 16), else every one */
export const lfoParamRows: (tgt: string, par: number, pn: string, pns?: string[]) => { k: number; name: string }[] = shared.lfoParamRows;
export const lfoRateText: (sync: number, rate: number) => string = shared.lfoRateText;
export const designDecode: (hex: string) => { v: number; s: number }[] = shared.designDecode;
export const designAt: (steps: { v: number; s: number }[], ph: number) => number = shared.designAt;
export type DesignStep = { v: number; s: number };
export const LFO_SHAPES: string[] = shared.LFO_SHAPES;
export const designEncode: (steps: DesignStep[]) => string = shared.designEncode;
/** a drag point (x, y in a w x h box) sets its step's value (-64..63) and the brush's shape; returns the step */
export const dragStep: (steps: DesignStep[], x: number, y: number, w: number, h: number, shape?: number) => number = shared.dragStep;

/** lfo=N:KEY:VALUE (N 1..26, core/lfo.cpp; the old page's lines). */
export const lfoCmd = {
  target: (n: number, tgt: string) => (tgt === "OFF" ? `lfo=${n}:off` : `lfo=${n}:tgt:${tgt.toLowerCase()}`),
  set: (n: number, key: "par" | "wav" | "sync" | "trig" | "rate" | "dep", v: number) => `lfo=${n}:${key}:${Math.round(v)}`,
  preset: (n: number, name: string) => `lfo=${n}:pre:${name}`,
  /** the DESIGN wave's 16 steps (designEncode's hex) */
  design: (n: number, hex: string) => `lfo=${n}:des:${hex}`,
};

/** The wave's shape over one cycle as an SVG path (w x h), for the card; DESIGN draws its 16 steps. */
export function wavePath(l: LfoState, w: number, h: number): string {
  const steps = l.wav === 7 ? designDecode(l.des) : null;
  const at = (p: number): number => {
    switch (l.wav) {
      case 0: return Math.sin(2 * Math.PI * p);
      case 1: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
      case 2: return 2 * p - 1;
      case 3: return 1 - 2 * p;
      case 4: return p < 0.5 ? 1 : -1;
      case 5: case 6: return [0.6, -0.4, 0.9, -0.8, 0.2, -0.1, 0.7, -0.6][Math.floor(p * 8) % 8];
      default: return steps ? designAt(steps, p) : 0;
    }
  };
  let d = "";
  for (let i = 0; i <= 64; i++) {
    const p = i / 64, y = h / 2 - at(p) * (h / 2 - 2);
    d += `${i ? " L" : "M"}${((p * w)).toFixed(1)} ${y.toFixed(1)}`;
  }
  return d;
}
