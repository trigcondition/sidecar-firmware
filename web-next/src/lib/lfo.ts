// web-next/src/lib/lfo.ts - the LFOs: the pure parts are web/lfo.js's, shared with the old page and the Sidecar Mini's
// page (one source: its own tests in web/lfo.test.mjs); this adds the state's type and the command lines.
// (a plain .js module: its exports are given their types below)
import * as shared from "../../../web/lfo.js";

export interface LfoState {
  tgt: string;
  par: number;
  pn: string;
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
  kit: number;
  l: LfoState[];
}

export const LFO_WAVES: string[] = shared.LFO_WAVES;
export const LFO_RETRIGS: string[] = shared.LFO_RETRIGS;
export const LFO_TARGETS: string[] = shared.LFO_TARGETS;
export const LFO_PRESETS: string[] = shared.LFO_PRESETS;
export const lfoParams: (tgt: string, par: number, pn: string) => string[] = shared.lfoParams;
export const lfoRateText: (sync: number, rate: number) => string = shared.lfoRateText;
export const designDecode: (hex: string) => { v: number; s: number }[] = shared.designDecode;
export const designAt: (steps: { v: number; s: number }[], ph: number) => number = shared.designAt;

/** lfo=N:KEY:VALUE (N 1..8, core/lfo.cpp; the old page's lines). */
export const lfoCmd = {
  target: (n: number, tgt: string) => (tgt === "OFF" ? `lfo=${n}:off` : `lfo=${n}:tgt:${tgt.toLowerCase()}`),
  set: (n: number, key: "par" | "wav" | "sync" | "trig" | "rate" | "dep", v: number) => `lfo=${n}:${key}:${Math.round(v)}`,
  preset: (n: number, name: string) => `lfo=${n}:pre:${name}`,
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
