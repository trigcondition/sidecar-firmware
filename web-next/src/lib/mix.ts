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
  n4?: string[]; // the TYPE's knobs' names (a bus slot's send TYPE; every TYPE from the 2026-11 Pi)
}
export interface Strip {
  id: string; // V1..V8, B1..B3, M
  chain: number; // the ins chain (0..10), -1 the master
  name: string; // the machine / bus
  slots: Slot[];
  peq: number[]; // HPF LPF LOW HIGH, 0..127
  bw?: number[]; // B1 / B2 (the send FX returns): HPF and LPF as BASE, WIDTH (0.5.0; the Rytm's SEND FX knobs G / H)
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
      bw: !voice && Array.isArray(c.bw) ? c.bw : undefined,
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

/**
 * A TYPE sent to a slot, read from the strip's next status: taken, not applied yet ("wait"), or refused because the
 * room filled meanwhile (another page took the last one). The /ws reply carries no console text, so the refusal is
 * worded here as the console words it.
 */
export function typeOutcome(s: Strip, slot: number, t: string): "taken" | "wait" | { refused: string } {
  if ((s.slots[slot]?.t ?? "OFF") === t) return "taken";
  if (!typeBlocked(s, slot, t) || !s.room) return "wait";
  return { refused: `${t}: ${s.room.cap} send FX inserts at most, the master and the buses together (${s.room.used} in use)` };
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


// ---- SIDECHAIN (0.5.0, core/fx/sidechain.h): one gain stage per chain (V1..V8, B1..B3, M1 M2), between its two slots and its
// POST EQ. PUMP: a trig-driven gain envelope (DPTH ATT HOLD REL SHAP THR); DUCK: the COMP keyed by the source (THR RAT ATT REL KBAS KWID: the key's band-pass; MON plays it).
// /status "ins" "sc": 13 entries, {"m":"OFF"} or {m, s: source, p: knobs, l: as they play, n: names, st: "OK" | "NO STEMS" | ..., mon: 0 | 1 (0.5.2)}
export const SC_MODES = ["OFF", "PUMP", "DUCK"] as const;
export type ScMode = (typeof SC_MODES)[number];
export const SC_CHAINS = [...Array.from({ length: 8 }, (_, i) => `V${i + 1}`), "B1", "B2", "B3", "M1", "M2"];
export const SC_TRACKS = ["BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"];
export const SC_SOURCES: { group: string; items: { value: string; text: string }[] }[] = [
  { group: "Rytm track (its stem)", items: SC_TRACKS.map((t) => ({ value: t, text: t })) },
  { group: "Audio in", items: [{ value: "AINL", text: "Audio in L" }, { value: "AINR", text: "Audio in R" }, { value: "AINLR", text: "Audio in L+R" }] },
  { group: "Sidecar voice", items: Array.from({ length: 8 }, (_, i) => ({ value: `V${i + 1}`, text: `Voice ${i + 1}` })) },
];
export const SC_NAMES: Record<string, string[]> = {
  PUMP: ["DPTH", "ATT", "HOLD", "REL", "SHAP", "THR"],
  DUCK: ["THR", "RAT", "ATT", "REL", "KBAS", "KWID"],
};
export interface Sidechain {
  id: string; // V1..V8, B1..B3, M1, M2
  chain: number; // 0..12
  mode: ScMode;
  src: string;
  p: number[]; // the knobs, 0..127 (6)
  l: number[]; // as they play (the LFO's offset on top)
  names: string[];
  status: string; // "OK", "NO STEMS", "NO AUDIO IN", ...
  mon: boolean; // 0.5.2 MON: the chain plays the filtered key (DUCK only; never saved)
}

/** The 13 stages from a status (an older Pi without "sc": all OFF, supported false). */
export function sidechains(st: Any | null): { supported: boolean; list: Sidechain[] } {
  const sc: Any[] | null = Array.isArray(st?.ins?.sc) ? st!.ins.sc : null;
  const list = SC_CHAINS.map((id, chain): Sidechain => {
    const x = sc?.[chain] ?? {};
    const mode = (SC_MODES as readonly string[]).includes(x.m) ? (x.m as ScMode) : "OFF";
    const six = (a: unknown) => (Array.isArray(a) ? [...a.map(Number), 0, 0, 0, 0, 0, 0].slice(0, 6) : [0, 0, 0, 0, 0, 0]);
    return { id, chain, mode, src: typeof x.s === "string" ? x.s : "BD", p: six(x.p), l: six(x.l ?? x.p), names: Array.isArray(x.n) ? x.n : SC_NAMES[mode] ?? [], status: typeof x.st === "string" ? x.st : "OFF", mon: x.mon === 1 };
  });
  return { supported: sc !== null, list };
}

/** KBAS: OFF or 20 Hz .. 19.6 kHz, log over knob 1..127 (core/fx/sidechain.h kbas_hz). */
export const kbasHz = (v: number) => (v <= 0 ? 0 : 20 * Math.pow(2, (Math.log2(980) * (Math.min(v, 127) - 1)) / 126));
const kbasText = (v: number) => {
  const h = Math.round(kbasHz(v));
  return h < 1000 ? `${h} Hz` : `${(h / 1000).toFixed(1)} kHz`;
};
const msText = (ms: number) => (ms < 100 ? `${ms.toFixed(1)} ms` : ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(2)} s`);
/** A knob's value as the Rytm's page shows it (core/fx/insert.cpp sc_vt). */
export function scText(mode: string, k: number, v: number): string {
  const p = v / 127;
  if (mode === "PUMP") {
    if (k === 0) return `${Math.round(p * 100)}%`;
    if (k === 1) return v ? msText(30 * p * p) : "instant";
    if (k === 2) return msText(250 * p * p);
    if (k === 3) return msText(10 + 1490 * p * p);
    if (k === 4) return v === 64 ? "linear" : `${v < 64 ? "-" : "+"}${Math.abs(v - 64)}`;
    if (k === 5) return `${Math.round(-60 * (1 - p))} dB`;
  } else if (mode === "DUCK") {
    if (k === 0) return `${-Math.round((v * 40) / 127)} dB`;
    if (k === 1) return `${(1 + 19 * p * p).toFixed(1)}:1`;
    if (k === 2) return msText(0.3 + 30 * p * p);
    if (k === 3) return msText(40 + 600 * p);
    if (k === 4) return v ? kbasText(v) : "off";
    if (k === 5) return v ? `${(0.5 + (9.4 * (v - 1)) / 126).toFixed(1)} oct` : "off";
  }
  return String(v);
}

/** PUMP's envelope as an SVG path in a w x h box (a drawing of the design: instant / linear attack, hold, the release's u^e curve). */
export function pumpPath(p: number[], w: number, h: number, points = 64): string {
  const q = (k: number) => p[k] / 127;
  const att = 0.03 * q(1) * q(1), hold = 0.25 * q(2) * q(2), rel = 0.01 + 1.49 * q(3) * q(3);
  const e = p[4] === 64 ? 1 : Math.pow(2, (p[4] - 64) / 32), depth = q(0), total = att + hold + rel;
  const lead = total * 0.08, span = total + lead * 2;
  const g = (t: number) => {
    if (t < lead) return 1;
    t -= lead;
    if (t < att) return 1 - depth * (att ? t / att : 1);
    t -= att;
    if (t < hold) return 1 - depth;
    t -= hold;
    return t >= rel ? 1 : 1 - depth + depth * Math.pow(t / rel, e);
  };
  let d = "";
  for (let i = 0; i <= points; i++) {
    const t = (i / points) * span;
    d += `${i ? " L" : "M"}${((i / points) * w).toFixed(1)} ${(1 + (1 - g(t)) * (h - 2)).toFixed(1)}`;
  }
  return d;
}

/** The web's MON lease (0.5.2): the Sidecar drops a MON from the web 3 s after the last line. Chains this page switched on are renewed
 * every second with a renew-only line (it never switches MON on); an OFF, or a chain whose status no longer shows MON, forgets the id. */
export function monLease(send: (cmd: string) => void, isOn: (id: string) => boolean) {
  const ids = new Set<string>();
  let timer: ReturnType<typeof setInterval> | undefined;
  return {
    set(id: string, on: boolean) {
      if (on) ids.add(id);
      else ids.delete(id);
      send(mixCmd.scMon(id, on));
    },
    off(id: string) {
      ids.delete(id);
      send(mixCmd.scMon(id, false));
    },
    start() {
      timer = setInterval(() => {
        for (const id of [...ids]) {
          if (isOn(id)) send(mixCmd.scMonRenew(id));
          else ids.delete(id);
        }
      }, 1000);
    },
    stop() {
      clearInterval(timer);
      ids.clear();
    },
  };
}

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v)));

// ---- command lines (core/fx/insert.cpp, out.cpp, master.cpp kHelp)
export const mixCmd = {
  type: (s: Strip, slot: number, t: string) => (s.chain < 0 ? `mfx=${slot + 1}:type:${t}` : `ins=${s.id}:${"AB"[slot]}:type:${t}`),
  param: (s: Strip, slot: number, k: number, v: number) => (s.chain < 0 ? `mfx=${slot + 1}:p${k + 1}:${clamp(v, 0, 127)}` : `ins=${s.id}:${"AB"[slot]}:p${k + 1}:${clamp(v, 0, 127)}`),
  eq: (s: Strip, k: number, v: number) => `${s.chain < 0 ? "mfx" : `ins=${s.id}`}${s.chain < 0 ? "=" : ":"}eq:${EQ_KEYS[k]}:${clamp(v, 0, 127)}`,
  eqOff: (s: Strip) => (s.chain < 0 ? "mfx=eq:off" : `ins=${s.id}:eq:off`),
  bw: (s: Strip, k: number, v: number) => `ins=${s.id}:eq:${k ? "width" : "base"}:${clamp(v, 0, 127)}`,
  send: (s: Strip, bus: number, v: number) => `ins=${s.id}:send:${bus + 1}:${clamp(v, 0, 127)}`,
  out: (s: Strip, key: "lev" | "pan" | "wid", v: number) => `out=${s.id}:${key}:${clamp(v, 0, 127)}`,
  route: (s: Strip, r: "trk" | "pre" | "post") => `out=${s.id}:${r}`,
  mfxLevel: (v: number) => `mfx=lev:${clamp(v, 0, 127)}`,
  mfxBypass: (on: boolean) => `mfx=byp:${on ? 1 : 0}`,
  mfxSource: (src: "ret" | "mix") => `mfx=src:${src}`,
  mfxTrim: (db: number) => `mfx=trim:${clamp(db, -24, 0)}`,
  mfxMix: (slot: number, v: number) => `mfx=${slot + 1}:mix:${clamp(v, 0, 127)}`,
  scMode: (id: string, m: ScMode) => `ins=${id}:sc:mode:${m.toLowerCase()}`,
  scSrc: (id: string, src: string) => `ins=${id}:sc:src:${src.toLowerCase()}`,
  scParam: (id: string, k: number, v: number) => `ins=${id}:sc:p${k + 1}:${clamp(v, 0, 127)}`,
  scMonRenew: (id: string) => `ins=${id}:sc:mon:renew`,
  scMon: (id: string, on: boolean) => `ins=${id}:sc:mon:${on ? "on" : "off"}`,
  mix: (s: Strip, slot: number, v: number) => (s.chain < 0 ? `mfx=${slot + 1}:mix:${clamp(v, 0, 127)}` : `ins=${s.id}:${"AB"[slot]}:mix:${clamp(v, 0, 127)}`),
};
