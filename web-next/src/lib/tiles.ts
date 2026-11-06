// web-next/src/lib/tiles.ts - the stats lines the Home page reads (the engine writes them; /status does not carry their
// figures) and the attached box's name. Home's model is health.ts. Pure (tiles.test.ts).
import type { Status } from "./protocol";

/** The stats lines Home reads, by their source: the newest line of each. */
export interface StatsLines {
  engine?: string;
  /** every 10 s: each core's asleep % (core 0's busy share comes from here) */
  idle?: string;
  rytmob?: string;
  sidecar?: string;
}

/** The periodic lines (the log view can hide them), as the old page's STATS. */
export const STATS_LINE = /(rytmob: \d+ms IN|engine: engine: fifo|engine: idle: asleep|sidecar: \w+ alt|midigadget:)/;

/** Keeps the newest stats line of each source from a batch of log lines. */
export function takeStats(lines: string[], into: StatsLines = {}): StatsLines {
  for (const l of lines) {
    if (l.includes("engine: engine: fifo")) into.engine = l;
    else if (/idle: asleep \d+ \d+ \d+ \d+ %/.test(l)) into.idle = l;
    else if (/rytmob: \d+ms IN/.test(l)) into.rytmob = l;
    else if (/sidecar: \w+ alt/.test(l)) into.sidecar = l;
  }
  return into;
}

/** A box's USB product string, short: "Elektron Analog Rytm MKII" -> "Rytm", "Elektron Digitakt II" -> "Digitakt II". */
export function shortBoxName(name: string): string {
  const s = name.replace(/^Elektron\s+/i, "").replace(/\s+MK\s*I+$/i, "").trim();
  return /^Analog Rytm$/i.test(s) ? "Rytm" : s;
}

/** The attached box's short name ("Rytm", "Digitakt"), for the words the page uses. /status "boxes" rows carry the USB
 * product string in name; their n is a count of boxes of the same type, not a name (pi/engine/box_list.h). */
export function boxName(st: Status | null): string {
  const b = (st as Record<string, any> | null)?.boxes;
  const inUse = Array.isArray(b) ? b.find((x: any) => x && x.on) ?? b[0] : null;
  const name = inUse && typeof inUse.name === "string" ? shortBoxName(inUse.name) : "";
  return name || "Rytm";
}
