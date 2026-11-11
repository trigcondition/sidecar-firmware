// web-next/src/lib/mix.ts - the mixer's data: 12 strips (V1..V8 the voices, B1..B3 the buses, M the master) from
// /status "ins" (core/fx/insert.cpp json: each chain's two insert slots, its POST EQ and, for a voice, its 3 sends),
// "out" (core/fx/out.cpp: each voice's route, level, pan, width), "mfx" (core/fx/master.cpp), "fx" (the two send FX),
// "machines"; the POST EQ's curve; the command lines (ins=, out=, mfx=). Pure (mix.test.ts).

// the insert types a voice slot takes (core/fx/insert.cpp kLabels; AMP: voices and the master only), the send FX types a
// bus or master slot also takes (0.5.0, core/fx/master.h: 4 at once over the master and the buses together), a bus's
// list (no AMP) and the master's (all 17)
export const INS_TYPES = ["OFF", "FILTER", "CRUSH", "DRIVE", "EQ", "COMP", "DELAY", "TREMOLO", "RINGMOD", "AMP", "SAT"];
export const SEND_TYPES = ["CHORUS", "COMB", "SPACE", "RINGS", "CLOUD", "HALO"];
export const BUS_TYPES = [...INS_TYPES.filter((t) => t !== "AMP"), ...SEND_TYPES];
export const MFX_TYPES = [...INS_TYPES, ...SEND_TYPES];
export const INS_LABELS: Record<string, string[]> = {
  FILTER: ["CUT", "RES", "MODE", "DRV"],
  CRUSH: ["RATE", "BITS", "DRV", "MIX"],
  DRIVE: ["DRV", "SHPE", "TONE", "MIX"],
  EQ: ["LOW", "MID", "HIGH", "FREQ"],
  COMP: ["THR", "RAT", "SPD", "GAIN"],
  DELAY: ["TIME", "FDBK", "MOD", "MIX"],
  TREMOLO: ["RATE", "DPTH", "SHPE", "PAN"],
  RINGMOD: ["FREQ", "SHPE", "MOD", "MIX"],
  AMP: ["MODL", "GAIN", "TONE", "LEV"],
  SAT: ["CHAR", "DRV", "TONE", "MIX"],
};
export const EQ_KEYS = ["hpf", "lpf", "low", "high"] as const;
export const EQ_NAMES = ["HPF", "LPF", "LOW", "HIGH"];
export const EQ_FLAT = [0, 0, 64, 64];

export interface Slot {
  t: string;
  p: number[];
  l?: (number | string)[];
  v?: string[];
  mix?: number;
  pair?: number;
  n4?: string[]; // a bus slot's send TYPE: its knobs' names
}
export interface Strip {
  id: string; // V1..V8, B1..B3, M
  chain: number; // the ins chain (0..10), -1 the master
  name: string; // the machine / bus
  slots: Slot[];
  peq: number[]; // HPF LPF LOW HIGH, 0..127
  sends?: number[]; // a voice's to B1..B3
  out?: { route: string; lev: number; pan: number; wid: number };
  level: number | null; // the meter, 0..1 (null: no meter source)
  room?: { used: number; cap: number }; // the send TYPEs' room, the master and the buses together (mfx fxused / fxcap)
}

type Any = Record<string, any>;
const VOICE_NAMES = ["BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB"];

/** The 12 strips from a status and the newest levels (12 values, null where there is no meter). */
export function strips(st: Any | null, levels?: (number | null)[] | null): Strip[] {
  const ins: Any = st?.ins ?? {};
  const chains: Any[] = Array.isArray(ins.c) ? ins.c : [];
  const out: Any[] = Array.isArray(ins.out) ? ins.out : Array.isArray(st?.out) ? st!.out : [];
  const machines: string[] = Array.isArray(st?.machines) ? st!.machines : [];
  const fx: Any[] = Array.isArray(st?.fx) ? st!.fx : [];
  const mfx: Any = ins.mfx ?? {};
  const lv = (i: number) => (levels && typeof levels[i] === "number" ? (levels[i] as number) : null);
  const room = typeof mfx.fxcap === "number" ? { used: Number(mfx.fxused) || 0, cap: mfx.fxcap as number } : undefined;
  const list: Strip[] = [];
  for (let i = 0; i < 11; i++) {
    const c = chains[i] ?? {};
    const voice = i < 8;
    const fxName = !voice && i < 10 ? fx[i - 8]?.type : undefined;
    list.push({
      id: voice ? `V${i + 1}` : `B${i - 7}`,
      chain: i,
      name: voice ? `${VOICE_NAMES[i]} ${(machines[i] ?? "").toUpperCase()}`.trim() : i < 10 ? `Send ${i - 7}${fxName ? ` · ${fxName}` : ""}` : "FX bus",
      slots: Array.isArray(c.s) ? c.s : [],
      peq: Array.isArray(c.peq) ? c.peq : EQ_FLAT,
      sends: voice && Array.isArray(c.send) ? c.send : undefined,
      out: voice && out[i] ? { route: out[i].route, lev: out[i].lev, pan: out[i].pan, wid: out[i].wid } : undefined,
      level: lv(i),
      room: voice ? undefined : room,
    });
  }
  list.push({ id: "M", chain: -1, name: "Master", slots: Array.isArray(mfx.s) ? mfx.s : [], peq: Array.isArray(mfx.peq) ? mfx.peq : EQ_FLAT, level: lv(11), room });
  return list;
}

/** The TYPEs a strip's slot lists: a voice the inserts, a bus no AMP but the send TYPEs, the master all. */
export function slotTypes(s: Strip): string[] {
  return s.chain < 0 ? MFX_TYPES : s.chain < 8 ? INS_TYPES : BUS_TYPES;
}

/** A send TYPE the slot cannot take now: the room is full and the slot holds none (it would be refused). */
export function typeBlocked(s: Strip, slot: number, t: string): boolean {
  if (!SEND_TYPES.includes(t) || !s.room) return false;
  return s.room.used >= s.room.cap && !SEND_TYPES.includes(s.slots[slot]?.t ?? "OFF");
}

/** "FILTER · COMP", "—" when both slots are off. */
export function insertText(s: Strip): string {
  const t = s.slots.map((x) => x.t).filter((t) => t && t !== "OFF");
  return t.length ? t.join(" · ") : "—";
}

// ---- the POST EQ's curve (core/fx/insert.h ins1112): HPF 20 Hz x 2^(10 v/127), LPF 20 kHz x 2^(-10 v/127), 0 = off;
// LOW a shelf at 250 Hz and HIGH a shelf at 4 kHz, 64 flat, +-15 dB. A drawing, not the filter's exact response.
export function eqDb(f: number, peq: number[]): number {
  const [hp, lp, lo, hi] = peq;
  let db = 0;
  if (hp > 0) {
    const r = f / (20 * Math.pow(2, (10 * hp) / 127));
    db += 20 * Math.log10((r * r) / Math.sqrt(1 + r * r * r * r));
  }
  if (lp > 0) {
    const r = f / (20000 * Math.pow(2, (-10 * lp) / 127));
    db += 20 * Math.log10(1 / Math.sqrt(1 + r * r * r * r));
  }
  const g = (v: number) => ((v - 64) / 64) * 15;
  const rl = f / 250, rh = f / 4000;
  db += g(lo) / (1 + rl * rl);
  db += (g(hi) * rh * rh) / (1 + rh * rh);
  return db;
}

/** An SVG path of the curve in a w x h box, 20 Hz .. 20 kHz across, +-range dB up and down. */
export function eqPath(peq: number[], w: number, h: number, range = 18, points = 48): string {
  let d = "";
  for (let i = 0; i <= points; i++) {
    const f = 20 * Math.pow(1000, i / points);
    const db = Math.max(-range, Math.min(range, eqDb(f, peq)));
    const x = (i / points) * w, y = h / 2 - (db / range) * (h / 2 - 1);
    d += `${i ? " L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`;
  }
  return d;
}

/** The EQ in words, for screen readers (the curve is a drawing). */
export function eqText(peq: number[]): string {
  const [hp, lp, lo, hi] = peq;
  const hz = (v: number) => (v >= 1000 ? `${(v / 1000).toFixed(1)} kHz` : `${Math.round(v)} Hz`);
  const sh = (v: number) => `${v >= 64 ? "+" : ""}${(((v - 64) / 64) * 15).toFixed(1)} dB`;
  const parts = [hp ? `high-pass ${hz(20 * Math.pow(2, (10 * hp) / 127))}` : "", lp ? `low-pass ${hz(20000 * Math.pow(2, (-10 * lp) / 127))}` : "", lo !== 64 ? `low shelf ${sh(lo)}` : "", hi !== 64 ? `high shelf ${sh(hi)}` : ""];
  return parts.filter(Boolean).join(", ") || "flat";
}

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v)));

// ---- command lines (core/fx/insert.cpp, out.cpp, master.cpp kHelp)
export const mixCmd = {
  type: (s: Strip, slot: number, t: string) => (s.chain < 0 ? `mfx=${slot + 1}:type:${t}` : `ins=${s.id}:${"AB"[slot]}:type:${t}`),
  param: (s: Strip, slot: number, k: number, v: number) => (s.chain < 0 ? `mfx=${slot + 1}:p${k + 1}:${clamp(v, 0, 127)}` : `ins=${s.id}:${"AB"[slot]}:p${k + 1}:${clamp(v, 0, 127)}`),
  eq: (s: Strip, k: number, v: number) => `${s.chain < 0 ? "mfx" : `ins=${s.id}`}${s.chain < 0 ? "=" : ":"}eq:${EQ_KEYS[k]}:${clamp(v, 0, 127)}`,
  eqOff: (s: Strip) => (s.chain < 0 ? "mfx=eq:off" : `ins=${s.id}:eq:off`),
  send: (s: Strip, bus: number, v: number) => `ins=${s.id}:send:${bus + 1}:${clamp(v, 0, 127)}`,
  out: (s: Strip, key: "lev" | "pan" | "wid", v: number) => `out=${s.id}:${key}:${clamp(v, 0, 127)}`,
  route: (s: Strip, r: "trk" | "pre" | "post") => `out=${s.id}:${r}`,
  mfxLevel: (v: number) => `mfx=lev:${clamp(v, 0, 127)}`,
  mfxBypass: (on: boolean) => `mfx=byp:${on ? 1 : 0}`,
  mfxSource: (src: "ret" | "mix") => `mfx=src:${src}`,
  mfxTrim: (db: number) => `mfx=trim:${clamp(db, -24, 0)}`,
  mfxMix: (slot: number, v: number) => `mfx=${slot + 1}:mix:${clamp(v, 0, 127)}`,
  mix: (s: Strip, slot: number, v: number) => (s.chain < 0 ? `mfx=${slot + 1}:mix:${clamp(v, 0, 127)}` : `ins=${s.id}:${"AB"[slot]}:mix:${clamp(v, 0, 127)}`),
};
