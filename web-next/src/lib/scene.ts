// web-next/src/lib/scene.ts - the morph scenes (0.5.0, core/mod/scene.h): twelve scenes a kit, each up to 64 locks
// of a modulation target at a value, two of them (A, B) on the ends of one fader 0..16383. /status "scene" in, the
// console's scene= lines out. Pure (scene.test.ts).
export const SCENES = 12, LOCKS = 64, FADER_MAX = 16383;
export const SOURCES = ["", "the Rytm's page", "the touchscreen", "the web", "MIDI learn", "the QUICK PERF knob", "the console"];

export interface SceneStatus {
  a: number; b: number; fader: number; played: number; sel: number; locked: number; edit: number; // edit: 0, 1 A, 2 B held
  s: { n: number; l: [string, number][] }[];
  src?: number; rscn?: number;
  pl?: Record<string, string>; // 2026-11: a locked param's name as the screens show it ("V1:P2" -> "V1:HRM")
  vl?: string[][]; // every voice's P1..P16 as its machine labels them ("" unused)
  gone?: string[]; // v2 Task 7: locked targets that no longer resolve (an unused P, an RT word the track lacks)
}
export interface Lock { name: string; v: number } // v: the value x 128 (0..16383)
export interface SceneView {
  a: number; b: number; fader: number; played: number; sel: number; locked: boolean; edit: boolean;
  side: number; // 0.5.0 v2: the side PAGE + T15 / T16 holds on the Rytm (0 A, 1 B), -1 none
  locks: Lock[][]; // [scene 0..11]
  src: number; rscn: number;
  labels: Record<string, string>; vl: string[][];
  gone: string[];
}

/** The state from /status (1-based A / B / sel there, 0 none; here 0-based, -1 none); null: this build has no scenes. */
export function sceneView(st: Record<string, unknown> | null | undefined): SceneView | null {
  const s = st?.scene as SceneStatus | undefined;
  if (!s || typeof s !== "object") return null;
  const locks: Lock[][] = Array.from({ length: SCENES }, () => []);
  for (const sc of s.s ?? []) if (sc.n >= 1 && sc.n <= SCENES) locks[sc.n - 1] = sc.l.map(([name, v]) => ({ name, v }));
  return {
    a: s.a - 1, b: s.b - 1, fader: s.fader, played: s.played ?? s.fader, sel: Math.max(0, s.sel - 1), locked: !!s.locked, edit: !!s.edit, side: (s.edit ?? 0) - 1,
    locks, src: s.src ?? 0, rscn: s.rscn ?? -1, labels: s.pl ?? {}, vl: s.vl ?? [], gone: s.gone ?? [],
  };
}

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v || 0)));
/** A lock's value as the console takes it and the knobs show it: 0..127.99 (the stored value / 128), two decimals. */
export const valueText = (v128: number) => String(+(clamp(v128, 0, FADER_MAX) / 128).toFixed(2));

export const abLine = (a: number, b: number) => `scene=ab:${clamp(a, -1, 11) + 1}:${clamp(b, -1, 11) + 1}`;
/** Scene n on one end, the other end kept (v2 Task 7: A = B allowed, as on the Rytm and the touchscreen). */
export const abSet = (v: { a: number; b: number }, side: "a" | "b", n: number) => abLine(side === "a" ? n : v.a, side === "b" ? n : v.b);
export const faderLine = (f: number) => `scene=fader:${clamp(f, 0, FADER_MAX)}`;
export const selLine = (n: number) => `scene=sel:${clamp(n, 0, 11) + 1}`;
export const clearLine = (n: number) => `scene=clear:${clamp(n, 0, 11) + 1}`;
export const copyLine = (from: number, to: number) => `scene=copy:${clamp(from, 0, 11) + 1}:${clamp(to, 0, 11) + 1}`;
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

/** A lock's name as the screens show it: the param as its voice's machine / FX TYPE names it ("V1:HRM"), else the name. */
export const lockLabel = (v: SceneView | null | undefined, name: string) => v?.labels[name] ?? name;

/** Common targets for the add-a-lock list (any name the build knows is taken). (Send FX params are P2..P5.) */
export const SUGGEST: string[] = [
  ...Array.from({ length: 8 }, (_, v) => Array.from({ length: 8 }, (_, p) => `V${v + 1}:P${p + 1}`)).flat(),
  ...Array.from({ length: 8 }, (_, v) => [`O${v + 1}:LEV`, `O${v + 1}:PAN`]).flat(),
  "MFX:LEV", "FX1:P2", "FX1:P3", "FX2:P2", "FX2:P3", // (0.5.0 v2: the Rytm's own scene and PERF macros are no scene target)
  // 0.5.0: the Rytm's own sound parameters (docs/RYTM-PARAMS.md; a Rytm without its 0.5 mod ignores them)
  "RT1:FRQ", "RT1:RES", "RT1:DEC", "RT2:FRQ", "RT2:DEC", "RFX:TIM", "RFX:FDB", "RFX:RDEC",
];
/** The add-a-lock list with names: every voice's P1..P16 its machine uses ("V1:P2", shown "V1:P2 HRM"; vl from
 *  /status, without it SUGGEST's P1..P8), then the rest of SUGGEST. */
export function suggest(vl?: string[][]): { value: string; label: string }[] {
  const voices = vl?.length
    ? vl.slice(0, 8).flatMap((ls, v) => ls.map((l, p) => ({ value: `V${v + 1}:P${p + 1}`, label: l ? `V${v + 1}:P${p + 1} ${l}` : "" })).filter((o) => o.label))
    : SUGGEST.filter((t) => t[0] === "V").map((t) => ({ value: t, label: t }));
  return [...voices, ...SUGGEST.filter((t) => t[0] !== "V").map((t) => ({ value: t, label: t }))];
}

/** The ADD picker (0.5.0 v2, as the MORPH page's ADD row: core/ui/page_morph.cpp): a track, its page, a param. */
export const ADD_TRACKS: string[] = [...Array.from({ length: 12 }, (_, t) => `RT${t + 1}`), "RFX", ...Array.from({ length: 8 }, (_, v) => `V${v + 1}`)];
const RT_PAGES: Record<string, string[]> = {
  SRC: ["P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"], SMPL: ["TUN", "FIN", "SMP", "BR", "STA", "END", "LOP", "LEV"],
  FLTR: ["FENV", "FATK", "FDEC", "FSUS", "FREL", "FRQ", "RES", "FTYP"], AMP: ["ATK", "HLD", "DEC", "OVR", "VOL", "PAN", "ACC", "DEL", "REV"],
  LFO: ["MUL", "WAV", "MOD", "SPD", "FAD", "SPH", "DEP"],
};
const RFX_PAGES: Record<string, string[]> = {
  DELAY: ["TIM", "X", "WID", "FDB", "DHPF", "DLPF", "DREV", "DVOL", "DOV"], REVERB: ["PRE", "RDEC", "RFRQ", "GAI", "RHPF", "RLPF", "RVOL"],
  DIST: ["AMT", "SYM"], COMP: ["THR", "CATK", "CREL", "RAT", "SEQ", "MUP", "MIX", "CVOL"], LFO: ["SPD", "FAD", "DEP"],
};
const V_PAGES = ["P1-8", "P9-16"];
export function addPages(track: string): string[] {
  if (track === "RFX") return Object.keys(RFX_PAGES);
  return track.startsWith("RT") ? Object.keys(RT_PAGES) : V_PAGES;
}
/** A page's params: value the target's name ("RT1:FRQ"), label as shown (a voice's by its machine: vl). */
export function addParams(track: string, page: string, vl?: string[][]): { value: string; label: string }[] {
  if (track === "RFX") return (RFX_PAGES[page] ?? []).map((p) => ({ value: `RFX:${p}`, label: p }));
  if (track.startsWith("RT")) return (RT_PAGES[page] ?? []).map((p) => ({ value: `${track}:${p}`, label: p }));
  const v = Number(track.slice(1)) - 1, k0 = page === "P9-16" ? 8 : 0;
  return Array.from({ length: 8 }, (_, i) => {
    const l = vl?.[v]?.[k0 + i];
    return { value: `${track}:P${k0 + i + 1}`, label: vl?.length ? (l ? `P${k0 + i + 1} ${l}` : "") : `P${k0 + i + 1}` };
  }).filter((o) => o.label);
}
