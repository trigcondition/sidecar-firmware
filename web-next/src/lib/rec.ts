// web-next/src/lib/rec.ts - the recorder's logic, as the old page's Record tab (pi/engine/www/index.html; recorder.h,
// rec_pi.cpp, rec_play.h, core/samp): the channels, what a selection needs of the card, the destinations, where the
// takes are, their order, the command lines. Pure (rec.test.ts).

// the channel ids (rec=ch:) in the old page's groups; the stereo ones write one 2-channel file
export const REC_GROUPS: { name: string; ch: [number, string][] }[] = [
  { name: "Master", ch: [[0, "MASTER (stereo)"]] },
  { name: "Drum machine inputs", ch: [[1, "MAIN L"], [2, "MAIN R"], [3, "BD"], [4, "SD"], [5, "RS/CP"], [6, "BT"], [7, "LT"], [8, "MT/HT"], [9, "CH/OH"], [10, "CY/CB"], [11, "AUDIO IN L"], [12, "AUDIO IN R"]] },
  { name: "Sidecar voices", ch: [[13, "BD"], [15, "SD"], [20, "RS/CP"], [14, "BT"], [16, "LT"], [18, "MT/HT"], [17, "CH/OH"], [19, "CY/CB"]] },
  { name: "Send FX returns", ch: [[21, "FX1 (stereo)"], [22, "FX2 (stereo)"]] },
  { name: "OUT route pairs", ch: [[23, "PRE (stereo)"], [24, "POST (stereo)"]] },
];
export const STEREO = new Set([0, 21, 22, 23, 24]);
export const REC_STATES = ["idle", "armed", "recording", "closing"];
export const SAMP_STATES = ["idle", "armed", "recording", "captured", "writing", "done", "failed"];
export const ASAMP_STATES = ["idle", "taking", "writing", "done", "failed", "cancelled"];

/** Bytes a second a selection writes (48 kHz). */
export function recBytesPerSec(ch: number[], bits: 16 | 24): number {
  return ch.reduce((a, id) => a + (STEREO.has(id) ? 2 : 1), 0) * (bits / 8) * 48000;
}
/** The selection's load on the destination in %, 0 when its speed is unknown. */
export function recLoad(needKBps: number, speedKBps: number): number {
  return speedKBps > 0 ? Math.round((needKBps * 100) / speedKBps) : 0;
}
/** Over 70 %: the old page asks before recording (the card may fall behind). */
export const recWarn = (needKBps: number, speedKBps: number, pct = 70) => (speedKBps > 0 && needKBps * 100 > speedKBps * pct ? recLoad(needKBps, speedKBps) : 0);

export const hms = (s: number) => `${Math.floor(s / 3600)}:${String(Math.floor(s / 60) % 60).padStart(2, "0")}:${String(Math.floor(s % 60)).padStart(2, "0")}`;
export const takeNameOk = (n: string) => /^[A-Za-z0-9_-]{0,24}$/.test(n);

interface Vol { name: string; state: string }
/** The destinations: the SD, each mounted drive, and the chosen one even while it is not mounted. */
export function recDestinations(vols: Vol[] | undefined, cur: string): { name: string; ok: boolean }[] {
  const up = (n: string) => n === "SD" || (vols ?? []).some((v) => v.name === n && v.state === "mounted");
  const names = ["SD", ...(vols ?? []).filter((v) => v.name !== "SD" && v.state === "mounted").map((v) => v.name)];
  if (cur && !names.includes(cur)) names.push(cur);
  return names.map((n) => ({ name: n, ok: up(n) }));
}
/** Where takes are listed from: [volume, folder (through /fs), the take id's prefix]. */
export function takeRoots(vols: Vol[] | undefined): [string, string, string][] {
  return [["SD", "/Recordings", ""], ...(vols ?? []).filter((v) => v.name !== "SD" && v.state === "mounted").map((v) => [v.name, `/${v.name}/Recordings`, `${v.name}:`] as [string, string, string])];
}

/** Names as text, their runs of digits as numbers ("_2" before "_10"). */
export function natCmp(a: string, b: string): number {
  const x = a.match(/\d+|\D+/g) ?? [], y = b.match(/\d+|\D+/g) ?? [];
  for (let i = 0; i < x.length && i < y.length; i++) {
    if (x[i] === y[i]) continue;
    const dx = /^\d/.test(x[i]), dy = /^\d/.test(y[i]);
    if (dx && dy && +x[i] !== +y[i]) return +x[i] < +y[i] ? -1 : 1;
    return x[i] < y[i] ? -1 : 1;
  }
  return x.length - y.length;
}

/** A take file "03_BD.wav": its number (play=start:<take>/<nn>) and its channel's name; null for other files. */
export function takeFile(n: string): { nn: number; ch: string } | null {
  const m = /^(\d\d)_(.*)\.wav$/i.exec(n);
  return m ? { nn: +m[1], ch: m[2] } : null;
}

export const recCmd = {
  ch: (ids: number[]) => (ids.length ? `rec=ch:${[...ids].sort((a, b) => a - b).join(",")}` : "rec=ch:none"),
  bits: (b: 16 | 24) => `rec=bits:${b}`,
  name: (n: string) => `rec=name:${n}`,
  vol: (v: string) => `rec=vol:${v}`,
  go: (arm: boolean) => (arm ? "rec=arm" : "rec=start"),
  stop: "rec=stop",
  play: (take: string, nn: number) => `play=start:${take}/${nn}`,
  playStop: "play=stop",
};

export interface SampSettings { src: string; start: "now" | "thr" | "play" | "bar"; thr: number; len: number; slices: string }
/** The sampler's lines, in the old page's order (the last arms it). */
export function sampLines(s: SampSettings): string[] {
  return [`samp=src:${s.src}`, `samp=start:${s.start === "thr" ? `thr:${Math.abs(Math.round(s.thr))}` : s.start}`, `samp=len:${Math.max(0, Math.round(s.len))}`, `samp=slices:${s.slices}`, "samp=arm"];
}
export interface AsampSettings { src: string; notes: string; vel: string; hold: number; tail: number; name: string }
export function asampLines(a: AsampSettings): string[] {
  return [`asamp=src:${a.src.trim()}`, `asamp=notes:${a.notes.trim()}`, `asamp=vel:${a.vel.trim()}`, `asamp=hold:${Math.round(a.hold)}`, `asamp=tail:${Math.round(a.tail)}`, `asamp=name:${a.name.trim() || "auto"}`, "asamp=start"];
}
