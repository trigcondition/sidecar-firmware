// web-next/src/lib/scene.ts - the morph scenes (0.5.0, core/mod/scene.h): twelve scenes a kit, each up to 64 locks
// of a modulation target at a value, two of them (A, B) on the ends of one fader 0..16383. /status "scene" in, the
// console's scene= lines out. Pure (scene.test.ts).
export const SCENES = 12, LOCKS = 64, FADER_MAX = 16383;
export const SOURCES = ["", "the Rytm's page", "the touchscreen", "the web", "MIDI learn", "the QUICK PERF knob", "the console"];

export interface SceneStatus {
  a: number; b: number; fader: number; played: number; sel: number; locked: number; edit: number;
  s: { n: number; l: [string, number][] }[];
  src?: number; rscn?: number;
}
export interface Lock { name: string; v: number } // v: the value x 128 (0..16383)
export interface SceneView {
  a: number; b: number; fader: number; played: number; sel: number; locked: boolean; edit: boolean;
  locks: Lock[][]; // [scene 0..11]
  src: number; rscn: number;
}

/** The state from /status (1-based A / B / sel there, 0 none; here 0-based, -1 none); null: this build has no scenes. */
export function sceneView(st: Record<string, unknown> | null | undefined): SceneView | null {
  const s = st?.scene as SceneStatus | undefined;
  if (!s || typeof s !== "object") return null;
  const locks: Lock[][] = Array.from({ length: SCENES }, () => []);
  for (const sc of s.s ?? []) if (sc.n >= 1 && sc.n <= SCENES) locks[sc.n - 1] = sc.l.map(([name, v]) => ({ name, v }));
  return {
    a: s.a - 1, b: s.b - 1, fader: s.fader, played: s.played ?? s.fader, sel: Math.max(0, s.sel - 1), locked: !!s.locked, edit: !!s.edit,
    locks, src: s.src ?? 0, rscn: s.rscn ?? -1,
  };
}

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v || 0)));
/** A lock's value as the console takes it and the knobs show it: 0..127.99 (the stored value / 128), two decimals. */
export const valueText = (v128: number) => String(+(clamp(v128, 0, FADER_MAX) / 128).toFixed(2));

export const abLine = (a: number, b: number) => `scene=ab:${clamp(a, -1, 11) + 1}:${clamp(b, -1, 11) + 1}`;
export const faderLine = (f: number) => `scene=fader:${clamp(f, 0, FADER_MAX)}`;
export const selLine = (n: number) => `scene=sel:${clamp(n, 0, 11) + 1}`;
export const clearLine = (n: number) => `scene=clear:${clamp(n, 0, 11) + 1}`;
export const copyLine = (from: number, to: number) => `scene=copy:${clamp(from, 0, 11) + 1}:${clamp(to, 0, 11) + 1}`;
export const editLine = (on: boolean) => `scene=edit:${on ? "on" : "off"}`;
/** A target name as core/mod/target.h writes it ("V3:P5", "MFX:LEV"); null when it cannot be one. */
export function targetName(s: string): string | null {
  const t = s.trim().toUpperCase();
  return /^[A-Z]+\d*:[A-Z]+\d*$/.test(t) ? t : null;
}
export const lockLine = (n: number, name: string, v128: number) => `scene=lock:${clamp(n, 0, 11) + 1}:${name}:${valueText(v128)}`;
export const unlockLine = (n: number, name: string) => `scene=unlock:${clamp(n, 0, 11) + 1}:${name}`;

/** Where the morph sits for a lock in A and / or B, as the Sidecar plays it (scene.h): a + (b - a) x f; one side
 * only morphs against the target's own value, which the page does not know (null). */
export function morphed(a: number | undefined, b: number | undefined, f: number): number | null {
  if (a === undefined || b === undefined) return null;
  return Math.round(a + (b - a) * (clamp(f, 0, FADER_MAX) / FADER_MAX));
}

/** Common targets for the add-a-lock list (any name the build knows is taken). */
export const SUGGEST: string[] = [
  ...Array.from({ length: 8 }, (_, v) => Array.from({ length: 8 }, (_, p) => `V${v + 1}:P${p + 1}`)).flat(),
  ...Array.from({ length: 8 }, (_, v) => [`O${v + 1}:LEV`, `O${v + 1}:PAN`]).flat(),
  "MFX:LEV", "FX1:P1", "FX1:P2", "FX2:P1", "FX2:P2", "RYTM:PERF1", "RYTM:PERF2", "RYTM:PERF3", "RYTM:PERF4",
];
