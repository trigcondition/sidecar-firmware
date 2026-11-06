// web-next/src/lib/tiles.ts - the Home page's status tiles, as the old page builds them (pi/engine/www/index.html
// renderStats): from /status and, for CPU, FIFO, the Overbridge gap / lost and the USB-C alt / late, from the newest
// stats lines in the log (the engine writes them once a second; /status does not carry them). Pure (tiles.test.ts).
import type { Status } from "./protocol";

export type Tone = "" | "good" | "bad";
export interface Tile {
  key: string;
  name: string;
  value: string;
  detail: string[];
  tone: Tone;
}

/** The stats lines the tiles read, by their source: the newest line of each. */
export interface StatsLines {
  engine?: string;
  rytmob?: string;
  sidecar?: string;
}

/** The periodic lines (the log view can hide them), as the old page's STATS. */
export const STATS_LINE = /(rytmob: \d+ms IN|engine: engine: fifo|sidecar: \w+ alt|midigadget:)/;

/** Keeps the newest stats line of each source from a batch of log lines. */
export function takeStats(lines: string[], into: StatsLines = {}): StatsLines {
  for (const l of lines) {
    if (l.includes("engine: engine: fifo")) into.engine = l;
    else if (/rytmob: \d+ms IN/.test(l)) into.rytmob = l;
    else if (/sidecar: \w+ alt/.test(l)) into.sidecar = l;
  }
  return into;
}

const num = (re: RegExp, s: string | undefined, i = 1): string | null => {
  const m = s ? s.match(re) : null;
  return m ? m[i] : null;
};

type S = Status & Record<string, any>; // (the status's members are read defensively below)

export function buildTiles(status: Status | null, last: StatsLines): Tile[] {
  const st = (status ?? {}) as S;
  const out: Tile[] = [];
  const add = (key: string, name: string, value: string, detail: (string | null | false | undefined)[], tone: Tone = "") =>
    out.push({ key, name, value, detail: detail.filter((d): d is string => !!d), tone });

  const e = last.engine;
  const c = e?.match(/cpu c1 ([\d.]+) c2 ([\d.]+) c3 ([\d.]+)/);
  add("cpu", "CPU (cores 1/2/3)", c ? `${c[1]} / ${c[2]} / ${c[3]} %` : "–", [
    e && `block max ${num(/max (\d+) us \|/, e)} us`,
    e && `pass max ${num(/pass max (\d+)/, e)} us`,
    st.cpu?.mhz && `${st.cpu.mhz} MHz, voice budget ${st.cpu.budgetUs} us`,
  ]);
  const f = e?.match(/fifo min (\d+) max (\d+) target (\d+) under (\d+)/);
  add("fifo", "FIFO", f ? `${f[1]}..${f[2]}` : "–", [f && `target ${f[3]} frames`], f ? (+f[1] === 0 ? "bad" : "") : "");

  if (st.underruns !== undefined)
    add("under", "Underruns", String(st.underruns), [st.restartUnderruns !== undefined && `restart ${st.restartUnderruns}`, st.restartUnderruns !== undefined && `streams ${st.streams}`, "glitches while playing"], st.underruns ? "bad" : "good");

  const ob = last.rytmob;
  if (st.streaming) {
    const g = num(/gap (\d+)/, ob), l = num(/lost (\d+)/, ob);
    add("ob", "Overbridge", "streaming", ob ? [`IN ${num(/IN (\d+)\/s/, ob)}/s`, `gap ${g}`, `lost ${l}`, `link ${st.link}`] : [`link ${st.link}`], +(g ?? 0) || +(l ?? 0) ? "bad" : "good");
  } else if (st.link !== undefined) add("ob", "Overbridge", "idle", [`link ${st.link}`, st.link === "usb" && "no Overbridge stream"]);

  if (st.streaming !== undefined) {
    const on = st.streaming || st.rytmMidi;
    add("rytm", boxName(st), on ? "attached" : "not seen", [st.streaming ? "Overbridge" : "no stream", st.rytmMidi ? "USB MIDI" : "no MIDI"], on ? "good" : "");
  }

  if (st.gadget !== undefined) {
    const sc = last.sidecar;
    const comp = `computer audio ${st.mac ? "on" : "off"}`;
    let d: string[] = [st.gadget === "sidecar" ? "UAC2 12/12 + MIDI" : st.gadget === "midi" ? "MIDI echo" : "off"];
    if (st.gadget === "sidecar" && st.usbHost === false) d = ["no computer on USB-C", comp];
    else if (sc) d = [`alt ${num(/alt (\d+\/\d+)/, sc)}`, comp, `late ${num(/late (\d+\/\d+\/\d+)/, sc)}`];
    add("gadget", "USB-C", st.gadget === "0" ? "off" : String(st.gadget), d);
  }

  if (st.temp !== undefined) {
    const low = st.clockLow > 0 ? `clock lowered to ${st.clockLow} MHz` : "";
    add("temp", "Temperature", `${st.temp} °C`, st.tempWarn ? ["SoC", low, "warning until below 75 °C"] : ["SoC", low && `${low} (hot)`], st.tempWarn ? "bad" : "");
  }

  if (st.power) {
    const p = st.power, bad = !!p.warn;
    add("power", "Power", !p.known ? "unknown" : bad ? "too weak" : "ok", [
      p.maxCurrent != null && `${p.maxCurrent / 1000} A`,
      p.uvNow ? "under-voltage now" : p.uvBoot ? "under-voltage since boot" : "no under-voltage",
      (p.throttledNow || p.cappedNow) && "throttled",
      p.note,
    ], bad ? "bad" : p.known ? "good" : "");
  }

  if (st.web) add("web", "This page", `${st.web.requests} req`, [`max ${st.web.maxus} us`, `${st.web.commands} commands`, st.web.refused && `${st.web.refused} busy`]);
  return out;
}

/** The attached box's short name ("Rytm", "Digitakt"), for the words the page uses. */
export function boxName(st: Status | null): string {
  const b = (st as S | null)?.boxes;
  const inUse = Array.isArray(b) ? b.find((x: any) => x && x.on) ?? b[0] : null;
  return (inUse && (inUse.n || inUse.name)) || "Rytm";
}
