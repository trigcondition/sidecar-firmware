// web-next/src/lib/spectrum.ts - the "spectrum" topic (docs/web-ws.md, pi/engine/spectrum.h): a strip's 40 bands after
// its POST EQ, drawn behind the EQ curve as columns of dots. The payload, the meter ballistics (the Pi sends 5..20
// frames a second; the page moves smoothly between them) and the columns' path. Pure (spectrum.test.ts).

export interface SpectrumFrame {
  strip: string | null;
  /** band edges: hz[0] .. hz[1], log-spaced */
  hz: [number, number];
  /** each band's level in dB (db[0] .. db[1]) */
  bands: number[];
}

/** The payload, or null when it is not one: bytes 0..255 across "db" become dB. */
export function parseSpectrum(p: unknown): SpectrumFrame | null {
  if (typeof p !== "object" || p === null) return null;
  const o = p as Record<string, unknown>;
  const b = o.b, hz = o.hz, db = o.db;
  if (!Array.isArray(b) || !b.length || !b.every((v) => typeof v === "number")) return null;
  const [lo, hi] = Array.isArray(db) && db.length === 2 && db.every((v) => typeof v === "number") ? (db as number[]) : [-90, 0];
  const range = Array.isArray(hz) && hz.length === 2 && hz.every((v) => typeof v === "number" && v > 0) ? (hz as [number, number]) : [20, 20000];
  return {
    strip: typeof o.strip === "string" ? o.strip : null,
    hz: [range[0], range[1]],
    bands: (b as number[]).map((v) => lo + (Math.max(0, Math.min(255, v)) / 255) * (hi - lo)),
  };
}

/** Meter ballistics: a rise is quick (a 25 ms glide, so a slow feed does not jump), a fall is 20 dB a second. */
export const RISE_MS = 25;
export const FALL_DB_S = 20;
export const FLOOR_DB = -90;

export class Ballistics {
  shown: number[] = [];
  private target: number[] = [];

  /** a new frame from the Pi: what the bands move toward */
  set(bands: number[]): void {
    this.target = bands.slice();
    if (this.shown.length !== bands.length) this.shown = bands.map(() => FLOOR_DB);
  }

  /** dt ms later; true while anything still moves */
  step(dt: number): boolean {
    let moving = false;
    const rise = 1 - Math.exp(-Math.max(0, dt) / RISE_MS), fall = (FALL_DB_S * Math.max(0, dt)) / 1000;
    for (let i = 0; i < this.shown.length; i++) {
      const t = this.target[i] ?? FLOOR_DB, s = this.shown[i];
      let n = t > s ? s + (t - s) * rise : Math.max(t, s - fall);
      if (t > s && t - n < 0.05) n = t;
      if (n !== s) moving = true;
      this.shown[i] = n;
    }
    return moving;
  }

  /** straight to the frame (reduced motion: no animation) */
  jump(): void {
    this.shown = this.target.slice();
  }
}

/** The columns as one path: band i across the EQ's axis (fLo..fHi over the width w, log), its height from its level
 *  (top dB at the top, bottom dB at the bottom of h), snapped to the dot rows (step) so each column is whole dots. */
export function columnsPath(levels: number[], hz: [number, number], w: number, h: number, step: number, axis: [number, number] = [20, 20000], db: [number, number] = [-78, -6]): string {
  if (!levels.length) return "";
  const lx = (f: number) => (Math.log(f / axis[0]) / Math.log(axis[1] / axis[0])) * w;
  const n = levels.length, r = Math.log(hz[1] / hz[0]);
  let d = "";
  for (let i = 0; i < n; i++) {
    const x0 = lx(hz[0] * Math.exp((r * i) / n)), x1 = lx(hz[0] * Math.exp((r * (i + 1)) / n));
    const v = Math.max(0, Math.min(1, (levels[i] - db[0]) / (db[1] - db[0])));
    const rows = Math.floor((v * h) / step);
    if (rows <= 0) continue;
    const y = h - rows * step;
    d += `M${x0.toFixed(1)} ${y.toFixed(1)}H${x1.toFixed(1)}V${h}H${x0.toFixed(1)}Z`;
  }
  return d;
}
