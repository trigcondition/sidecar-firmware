// web-next/src/lib/status.ts - small readers over the /status JSON (the status topic), so pages don't each dig into it.
// Every reader tolerates a missing member: older firmware, or the mock.
import type { Status } from "./protocol";

type Obj = Record<string, unknown>;
const obj = (v: unknown): Obj | null => (v && typeof v === "object" && !Array.isArray(v) ? (v as Obj) : null);
const num = (v: unknown): number | null => (typeof v === "number" && Number.isFinite(v) ? v : null);
const str = (v: unknown): string | null => (typeof v === "string" ? v : null);

/** A pattern number 0..127 as the Rytm writes it: A01..H16. */
export function patternName(n: number | null | undefined): string {
  if (n == null || n < 0 || n > 127) return "";
  return `${"ABCDEFGH"[n >> 4]}${String((n & 15) + 1).padStart(2, "0")}`;
}

/** "A04 · 120.0 BPM · Playing", or "" when the status says nothing about the transport. */
export function transportText(s: Status | null): string {
  if (!s) return "";
  const t = obj(s.transport) ?? obj(s.seq) ?? {};
  const pat = patternName(num(t.pattern) ?? num(s.pattern));
  const bpm = num(t.bpm) ?? num(s.bpm) ?? (num(t.bpm10) != null ? (num(t.bpm10) as number) / 10 : null);
  const play = str(t.state) ?? (t.playing === true ? "playing" : t.playing === false ? "stopped" : null);
  const parts = [pat, bpm != null ? `${bpm.toFixed(1)} BPM` : "", play ? play[0].toUpperCase() + play.slice(1) : ""];
  return parts.filter(Boolean).join(" · ");
}

export function uptimeText(sec: number | null | undefined): string {
  const s = Math.max(0, Math.floor(sec ?? 0));
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m ${s % 60}s`;
}

export function dbText(linear: number): string {
  return linear > 0 ? `${(20 * Math.log10(linear)).toFixed(1)} dB` : "-inf dB";
}

export { obj, num, str };
