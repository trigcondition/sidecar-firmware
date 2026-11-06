// web-next/src/lib/logbuf.ts - the "log" topic's pushes ({from,to,gap,text}) as whole lines: a push may end in the
// middle of a line (kept until the rest comes), a gap is marked with a line of its own, at most `max` lines are kept
// (the old page's 3000), and the newest stats line of each source is kept for the Home tiles (tiles.ts).
import { takeStats, type StatsLines } from "./tiles";

export interface LogPush {
  from: number;
  to: number;
  gap: boolean;
  text: string;
}
export interface LogLine {
  n: number; // a running number (a stable key for lists)
  text: string;
  gap?: boolean;
}

export class LogBuffer {
  lines: LogLine[] = [];
  stats: StatsLines = {};
  bytes = 0; // the log's total, as the Sidecar counts it (the newest "to")
  private partial = "";
  private next = 0;

  constructor(private readonly max = 3000) {}

  /** One push; returns the lines it completed. */
  add(p: LogPush): LogLine[] {
    const added: LogLine[] = [];
    if (p.gap) {
      this.partial = "";
      added.push({ n: this.next++, text: "… (log lines lost: the Sidecar's log ring moved on)", gap: true });
    }
    const parts = (this.partial + p.text).split("\n");
    this.partial = parts.pop() ?? "";
    for (const t of parts) added.push({ n: this.next++, text: t });
    this.bytes = p.to;
    takeStats(added.filter((l) => !l.gap).map((l) => l.text), this.stats);
    this.lines.push(...added);
    if (this.lines.length > this.max) this.lines.splice(0, this.lines.length - this.max);
    return added;
  }

  clear(): void {
    this.lines = [];
  }
}
