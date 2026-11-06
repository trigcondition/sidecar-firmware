// web-next/src/lib/health.ts - the Home page's status, as a model the page draws: the health pills at the top and the
// tiles' figures, grouped Engine / Connections / System. From /status and the newest stats lines in the log (tiles.ts
// takeStats: the engine's once-a-second line for CPU, FIFO and block times; its every-10-s "idle" line for core 0;
// rytmob and sidecar for Overbridge and USB-C). Every member is read defensively: older firmware, or the mock. Pure
// (health.test.ts).
import type { Status } from "./protocol";
import type { StatsLines } from "./tiles";
import { boxName } from "./tiles";

type S = Status & Record<string, any>;

const num = (re: RegExp, s: string | undefined, i = 1): number | null => {
  const m = s ? s.match(re) : null;
  return m ? +m[i] : null;
};

/** The thermal limits (pi/engine/thermal.h): the clock may be lowered from 78 °C, the warning comes on at 80. */
export const TEMP = { min: 20, max: 90, throttle: 78, warn: 80 } as const;

export interface Engine {
  /** busy % of cores 0..3: core 0 (system) from the idle line, 1..3 (render) from the engine line; null not seen yet */
  cores: (number | null)[];
  /** the busiest render core, % */
  busiest: number | null;
  blockMax: number | null;
  passMax: number | null;
  /** the voice budget, us (/status cpu.budgetUs) */
  budget: number | null;
  mhz: number | null;
  fifo: { min: number; max: number; target: number } | null;
}

export function readEngine(status: Status | null, last: StatsLines): Engine {
  const st = (status ?? {}) as S, e = last.engine;
  const c = e?.match(/cpu c1 ([\d.]+) c2 ([\d.]+) c3 ([\d.]+)/);
  const render = c ? [+c[1], +c[2], +c[3]] : [null, null, null];
  const asleep0 = num(/asleep (\d+) \d+ \d+ \d+ %/, last.idle);
  const f = e?.match(/fifo min (\d+) max (\d+) target (\d+)/);
  return {
    cores: [asleep0 === null ? null : Math.max(0, 100 - asleep0), ...render],
    busiest: c ? Math.max(...(render as number[])) : null,
    blockMax: num(/max (\d+) us \|/, e),
    passMax: num(/pass max (\d+)/, e),
    budget: typeof st.cpu?.budgetUs === "number" ? st.cpu.budgetUs : null,
    mhz: typeof st.cpu?.mhz === "number" ? st.cpu.mhz : null,
    fifo: f ? { min: +f[1], max: +f[2], target: +f[3] } : null,
  };
}

export interface Pill {
  key: "audio" | "cpu" | "temp" | "power" | "box";
  /** what the pill says, short: "Audio ok", "CPU 41 %", "58 °C", "Power too weak", "Rytm connected" */
  text: string;
  /** the same for a screen reader, whole words */
  spoken: string;
  warn: boolean;
}

/** The health row: one pill per area, coral with a mark when it needs looking at. Areas the status lacks are left out. */
export function pills(status: Status | null, eng: Engine): Pill[] {
  const st = (status ?? {}) as S, out: Pill[] = [];
  if (typeof st.underruns === "number") {
    const n = st.underruns;
    out.push({ key: "audio", text: n ? `${n} underrun${n > 1 ? "s" : ""}` : "Audio ok", spoken: n ? `Audio: ${n} underruns while playing` : "Audio: ok", warn: n > 0 });
  }
  if (eng.busiest !== null) {
    const p = Math.round(eng.busiest), over = eng.budget !== null && eng.blockMax !== null && eng.blockMax > eng.budget;
    out.push({ key: "cpu", text: `CPU ${p} %`, spoken: `CPU: busiest core ${p} percent${over ? ", a block went over the voice budget" : ""}`, warn: p >= 90 || over });
  }
  if (typeof st.temp === "number") {
    const hot = !!st.tempWarn || (st.clockLow ?? 0) > 0;
    out.push({ key: "temp", text: `${st.temp} °C`, spoken: `Temperature: ${st.temp} degrees${hot ? ", hot" : ""}`, warn: hot });
  }
  if (st.power) {
    const p = st.power, w = !!p.warn;
    const t = !p.known ? "Power unknown" : w ? "Power too weak" : "Power ok";
    out.push({ key: "power", text: t, spoken: t, warn: w });
  }
  if (st.streaming !== undefined) {
    const on = !!(st.streaming || st.rytmMidi), b = boxName(st);
    out.push({ key: "box", text: `${b} ${on ? "connected" : "not seen"}`, spoken: `${b}: ${on ? "connected" : "not seen"}`, warn: false });
  }
  return out;
}

export interface Ob {
  streaming: boolean;
  link: string;
  rate: number | null;
  gap: number | null;
  lost: number | null;
  /** a gap or a lost packet in the last second */
  bad: boolean;
}
export function readOb(status: Status | null, last: StatsLines): Ob | null {
  const st = (status ?? {}) as S;
  if (st.streaming === undefined && st.link === undefined) return null;
  const ob = st.streaming ? last.rytmob : undefined;
  const gap = num(/gap (\d+)/, ob), lost = num(/lost (\d+)/, ob);
  return { streaming: !!st.streaming, link: String(st.link ?? ""), rate: num(/IN (\d+)\/s/, ob), gap, lost, bad: !!(gap || lost) };
}

export interface UsbC { mode: string; detail: string; more: string[] }
export function readUsbC(status: Status | null, last: StatsLines): UsbC | null {
  const st = (status ?? {}) as S;
  if (st.gadget === undefined) return null;
  const g = String(st.gadget);
  const mode = g === "sidecar" ? "Sidecar audio" : g === "midi" ? "MIDI echo" : "Off";
  if (g !== "sidecar") return { mode, detail: g === "midi" ? "USB MIDI only" : "nothing on USB-C", more: [] };
  if (st.usbHost === false) return { mode, detail: "no computer on USB-C", more: [] };
  const sc = last.sidecar, alt = sc?.match(/alt (\d+\/\d+)/), late = sc?.match(/late (\d+\/\d+\/\d+)/);
  return { mode, detail: `computer audio ${st.mac ? "on" : "off"}`, more: [alt && `interface alt ${alt[1]}`, late && `late ${late[1]}`].filter((x): x is string => !!x) };
}

export interface Power { chip: "ok" | "too weak" | "unknown"; amps: number | null; uv: string; more: string[] }
export function readPower(status: Status | null): Power | null {
  const p = ((status ?? {}) as S).power;
  if (!p) return null;
  return {
    chip: !p.known ? "unknown" : p.warn ? "too weak" : "ok",
    amps: typeof p.maxCurrent === "number" ? +(p.maxCurrent / 1000).toFixed(2) : null,
    uv: p.uvNow ? "under-voltage now" : p.uvBoot ? "under-voltage since boot" : "no under-voltage",
    more: [(p.throttledNow || p.cappedNow) && "throttled now", (p.throttledBoot || p.cappedBoot) && !(p.throttledNow || p.cappedNow) && "throttled since boot", p.note].filter((x): x is string => !!x),
  };
}

/** A sparkline's points over w x h (values 0..max, oldest first; a flat line at 0 for fewer than two). */
export function sparkPoints(v: number[], w: number, h: number, max = 100): string {
  if (v.length < 2) return `0,${h} ${w},${h}`;
  const dx = w / (v.length - 1);
  return v.map((x, i) => `${+(i * dx).toFixed(1)},${+(h - (Math.min(max, Math.max(0, x)) / max) * h).toFixed(1)}`).join(" ");
}

/** Keeps the last n samples (the sparkline's minute). */
export function pushSample(a: number[], v: number, n = 60): number[] {
  const out = a.length >= n ? a.slice(a.length - n + 1) : a.slice();
  out.push(v);
  return out;
}
