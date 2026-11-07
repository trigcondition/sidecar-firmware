// web-next/src/lib/seqx.ts - the sequencer's "seqtop" settings (core/seq; /status "seqx"), as the old page's Sound ›
// Sequencer: the pattern's seed, each seq track's random notes and SEED MOD, a step's ratchet / arp, FX columns,
// random notes, lock condition and slide. Track numbers here are 1..12 (seq=), steps 1..128. Pure (seqx.test.ts).
export const RND = ["off", "steps", "track"];
export const WEIGHT = ["flat", "root", "chord", "low", "high", "step"];
export const SEED_QUANT = ["step", "loop", "bar"];
export const AFFECTS = ["notes", "chance", "order", "all"];
export const RT_MODES = ["chord", "walk", "up", "down", "updn", "rnd", "played", "conv"];
export const RATES = ["auto", "2", "1", "2/3", "1/2", "1/3", "1/4", "1/6", "1/8"];
export const VCURVES = ["flat", "up", "down", "updn"];
export const FX_CMDS = ["---", "RTG", "DLY", "CUT", "CHN", "RND", "RNV", "OCT", "TRS", "JMP", "JRN", "LOP", "SKP", "DIR", "FIL"];
const DIR = ["fwd", "rev", "pend", "ping", "rnd", "brown"];

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v || 0)));

/** A lock's COND code as seq=lcond takes it (core/seq cond codes). */
export function condText(c: number): string {
  if (c >= 44) return `${c - 43}%`;
  if (c >= 9) {
    let i = c - 9, b = 2;
    while (i >= b) { i -= b; b++; }
    return `${i + 1}:${b}`;
  }
  return ["---", "fill", "!fill", "pre", "!pre", "nei", "!nei", "1st", "!1st"][c] ?? "---";
}
/** An FX value as seq=fx takes it (DIR by name, LOP as NxX). */
export const fxText = (c: number, v: number) => (c === 13 ? DIR[v] ?? "fwd" : c === 11 ? `${(v & 15) + 1}x${(v >> 4) + 1}` : String(v));

export interface TrackSettings { spd: number; dir: number; rm: number; rl: number; ro: number; rw: number; ms: number; mr: number; mq: number; ma: number }
export function trackLines(t: number, f: TrackSettings, spd: string[], dir: string[]): string[] {
  return [
    `seq=spd:${t}:${spd[clamp(f.spd, 0, spd.length - 1)]}`,
    `seq=dir:${t}:${dir[clamp(f.dir, 0, dir.length - 1)]}`,
    `seq=rnd:${t}:mode=${RND[clamp(f.rm, 0, 2)]},low=${clamp(f.rl, 0, 127)},oct=${clamp(f.ro, 1, 4)},wght=${WEIGHT[clamp(f.rw, 0, 5)]}`,
    `seq=smod:${t}:src=${clamp(f.ms, 0, 16)},range=${clamp(f.mr, 2, 64)},quant=${SEED_QUANT[clamp(f.mq, 0, 2)]},aff=${AFFECTS[clamp(f.ma, 0, 3)]}`,
  ];
}
export const seedLine = (seed: number, free: boolean) => (free ? "seq=seed:free" : `seq=seed:${clamp(seed, 0, 9999)}`);

export interface StepSettings {
  rm: number; rc: number; rr: number; rv: number; rp: number; ro: number; rg: number; // ratchet / arp: mode (-1 off), count, rate, vel curve, pitch curve, octaves, gate
  f1: number; v1: string; f2: number; v2: string; // FX columns: command, value
  rnd: boolean; lp: number; lc: string; sl: boolean; // random notes; a lock's P, its condition, slide
}
/** The step editor's lines; was: the step as loaded (a lock's condition and slide are sent only when changed). */
export function stepLines(t: number, s: number, f: StepSettings, was: { lp: number; lc: string; sl: boolean } | null): string[] {
  const ts = `${t}:${clamp(s, 1, 128)}:`, l: string[] = [];
  if (f.rm < 0) l.push(`seq=rtrg:${ts}off`);
  else l.push(`seq=rtrg:${ts}cnt=${clamp(f.rc, 0, 16)},rate=${RATES[clamp(f.rr, 0, 8)]},vcrv=${VCURVES[clamp(f.rv, 0, 3)]},pcrv=${clamp(f.rp, -24, 24)},mode=${RT_MODES[clamp(f.rm, 0, 7)]},oct=${clamp(f.ro, 1, 4)},gate=${clamp(f.rg, 1, 100)}`);
  for (const c of [1, 2] as const) {
    const cmd = FX_CMDS[clamp(c === 1 ? f.f1 : f.f2, 0, 14)], v = String(c === 1 ? f.v1 : f.v2).trim();
    l.push(`seq=fx:${ts}${c}:${cmd === "---" ? "off" : `${cmd}:${v || "0"}`}`);
  }
  l.push(`seq=rndstep:${ts}${f.rnd ? 1 : 0}`);
  const same = !!was && was.lp === f.lp;
  if (f.lp >= 1 && !(same && was!.lc === f.lc.trim())) l.push(`seq=lcond:${ts}${clamp(f.lp, 1, 32)}:${f.lc.trim() || "---"}`);
  if (f.lp >= 1 && !(same && was!.sl === f.sl)) l.push(`seq=slide:${ts}${clamp(f.lp, 1, 32)}:${f.sl ? 1 : 0}`);
  return l;
}

/** The step editor's fields from /status "seqx" (a step without extras: the defaults). */
export function stepFrom(x: { t: number; x: { s: number; rt: number[]; fx: number[][]; r: number; lc: number[][]; sl: number[] }[] }[] | undefined, t: number, s: number): { f: StepSettings; was: { lp: number; lc: string; sl: boolean } } {
  const e = x?.find((q) => q.t === t)?.x.find((q) => q.s === s);
  const rt = e?.rt ?? [0, 0, 0, 0, 0, 0, 0], on = rt[0] >= 2 || rt[4] >= 2;
  const fx = e?.fx ?? [[0, 0], [0, 0]];
  const lp = e?.lc.length ? e.lc[0][0] : e?.sl.length ? e.sl[0] : 0;
  const lc = e?.lc.find((q) => q[0] === lp);
  const f: StepSettings = {
    rm: on ? rt[4] : -1, rc: rt[0], rr: rt[1], rv: rt[2], rp: rt[3], ro: rt[5] || 1, rg: rt[6] || 100,
    f1: fx[0][0], v1: fx[0][0] ? fxText(fx[0][0], fx[0][1]) : "", f2: fx[1][0], v2: fx[1][0] ? fxText(fx[1][0], fx[1][1]) : "",
    rnd: !!e?.r, lp, lc: lc ? condText(lc[1]) : "", sl: !!e?.sl.includes(lp),
  };
  return { f, was: { lp, lc: f.lc, sl: f.sl } };
}

/** The touch editor's strum and humanise (docs/TOUCH-EDITOR.md; T = Rytm track 0..11, steps 0-based). */
export const strumLine = (T: number, s0: number, s1: number, amount: number) => `seqed=strum:${T}:${s0}:${s1}:${clamp(amount, -96, 96)}`;
export const humanLine = (T: number, s0: number, s1: number, time: number, vel: number, seed: number) => `seqed=human:${T}:${s0}:${s1}:${clamp(time, 0, 96)}:${clamp(vel, 0, 127)}:${clamp(seed, 0, 9999)}`;
