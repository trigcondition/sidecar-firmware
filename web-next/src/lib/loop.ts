// web-next/src/lib/loop.ts - the master looper (0.5.0, core/loop/loop.h): the Rytm's MAIN kept in a ring, a grab takes the last
// 1/2..16 bars on the last bar line and plays them from the next, up to 8 grabs held, KEEP writes one to the card. /status "loop" in,
// the console's loop= lines out. Pure (loop.test.ts).
export const LENGTHS: { hb: number; label: string; arg: string }[] = [
  { hb: 1, label: "1/2", arg: "0.5" },
  { hb: 2, label: "1", arg: "1" },
  { hb: 4, label: "2", arg: "2" },
  { hb: 8, label: "4", arg: "4" },
  { hb: 16, label: "8", arg: "8" },
  { hb: 32, label: "16", arg: "16" },
];
export const FADES = ["1/4", "1/2", "1", "2", "4"]; // bars, the FADE setting's index 0..4
export const MAX_GRABS = 8;
export const STATES: Record<string, string> = { ring: "Listening: the ring holds the last bars", wait: "Starts at the next bar line", playing: "Playing", fading: "Fading back" };

export interface LoopGrabStatus { id: number; hb: number; bpm: number; kept: number; gap: number; copied: number; playing: number }
export interface LoopStatus {
  on: number; state?: string; ring_s?: number; bal?: number; lev?: number; fade?: number; mute?: number; live?: number; post_lat?: number;
  pending?: number; cal?: number; bar?: number; bars?: number; msg?: string; msg_n?: number; grabs?: LoopGrabStatus[];
}
export interface Grab { id: number; hb: number; label: string; bpm: number; kept: boolean; gap: boolean; copied: boolean; playing: boolean }
export interface LoopView {
  on: boolean; state: string; ringSeconds: number; bal: number; lev: number; fade: number; mute: boolean; live: number; postLat: number;
  pending: boolean; calibrating: boolean; bar: number | null; bars: number; msg: string; msgN: number; grabs: Grab[];
}

export const lengthLabel = (hb: number) => LENGTHS.find((l) => l.hb === hb)?.label ?? String(hb / 2);

/** The looper from /status; null: this build has none (0.4.x). `on` false: it is there but off (loopram=0, or no RAM for it). */
export function loopView(st: Record<string, unknown> | null | undefined): LoopView | null {
  const s = st?.loop as LoopStatus | undefined;
  if (!s || typeof s !== "object") return null;
  const bars = s.bars ?? 0;
  return {
    on: !!s.on, state: s.state ?? "ring", ringSeconds: s.ring_s ?? 0, bal: s.bal ?? 64, lev: s.lev ?? 100, fade: s.fade ?? 2, mute: !!s.mute,
    live: s.live ?? 0, postLat: s.post_lat ?? 0, pending: !!s.pending, calibrating: !!s.cal, bar: s.bar === undefined || !bars ? null : s.bar,
    bars, msg: s.msg ?? "", msgN: s.msg_n ?? 0,
    grabs: (s.grabs ?? []).map((g) => ({ id: g.id, hb: g.hb, label: lengthLabel(g.hb), bpm: g.bpm, kept: !!g.kept, gap: !!g.gap, copied: !!g.copied, playing: !!g.playing })),
  };
}

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v || 0)));
export const grabLine = (hb: number) => `loop=grab:${(LENGTHS.find((l) => l.hb === hb) ?? LENGTHS[1]).arg}`;
/** a grab is named by its id (stable while newer grabs land), not its place in the list */
export const playLine = (id: number) => `loop=play:#${Math.max(0, Math.round(id || 0))}`;
/** STOP: a fade over the FADE time at the next bar line; a cut: 10 ms, at once */
export const stopLine = (cut = false) => (cut ? "loop=stop:cut:now" : "loop=stop");
export const keepLine = (id: number) => `loop=keep:#${Math.max(0, Math.round(id || 0))}`;
export const balLine = (v: number) => `loop=bal:${clamp(v, 0, 127)}`;
export const levLine = (v: number) => `loop=lev:${clamp(v, 0, 127)}`;
export const fadeLine = (i: number) => `loop=fade:${FADES[clamp(i, 0, FADES.length - 1)]}`;
export const muteLine = (on: boolean) => `loop=mute:${on ? "on" : "off"}`;
/** LIVE VIA: 0 off, 1..12 the Rytm's PERF macro the balance drives */
export const liveLine = (m: number) => (m > 0 ? `loop=live:perf${clamp(m, 1, 12)}` : "loop=live:off");
export const calLine = "loop=cal";

/** What the live Rytm does at this balance, as the percentage LIVE VIA's macro gets (spec 3.8: full to BAL 64, 0 at 127). */
export const livePercent = (bal: number) => (bal <= 64 ? 100 : Math.round(((127 - Math.floor(((bal - 64) * 127) / 63)) / 127) * 100));
/** The loop's and the Sidecar's own level at this balance, 0..1 (equal power each side of 64). */
export const balGains = (bal: number): { loop: number; own: number } => ({
  loop: bal >= 64 ? 1 : Math.sin((Math.PI / 2) * (clamp(bal, 0, 64) / 64)),
  own: bal <= 64 ? 1 : bal >= 127 ? 0 : Math.cos((Math.PI / 2) * ((bal - 64) / 63)),
});
/** the position "2.5 / 4 bars" or "0.2 / 1/2 bar" */
export function positionText(v: LoopView): string {
  if (v.bar === null || !v.bars) return "";
  return `${v.bar.toFixed(1)} / ${v.bars < 1 ? "1/2" : v.bars} bar${v.bars > 1 ? "s" : ""}`;
}
