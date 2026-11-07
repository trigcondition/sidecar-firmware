// web-next/src/lib/pool.ts - the project's POOL (core/lib/pools.h, GET /pool as poolweb.cpp, pool= as pools::command),
// as the old page's Pool tab: a bank's rows, the row codes, the answers (which come back in the log, not the reply).
export const POOLS: Record<string, string> = { smp: "SAMPLES", wt: "WAVETABLES", inst: "INSTRUMENTS" };
export const BANKS = "ABCDEFGH";
// pools.cpp result_text (the ones with ": " first)
export const POOL_RES = ["IN USE: REPLACE?", "PLAYING: STOP FIRST", "NOT FOR THIS POOL", "POOL LOCKED", "NO SUCH FILE", "NO SUCH ROW", "000 IS OFF", "BUILT-IN", "BANK FULL", "IN POOL", "READING", "BUSY", "EMPTY", "SET"];
export const STATE_TONE: Record<string, "ok" | "bad" | "warn" | ""> = { EXACT: "ok", MOVED: "ok", MISSING: "bad", CHANGED: "warn" };

export interface PoolRow {
  row: number; type: string; name: string; state: string; path: string; used: number; users: number; size: number; members: number; ch: number; ram: boolean;
}
/** bank_json's row: [row, type, name, state, path, used_count, users, size, members, channels, always_ram] */
export const poolRow = (a: unknown[]): PoolRow => ({
  row: a[0] as number, type: a[1] as string, name: a[2] as string, state: a[3] as string, path: a[4] as string, used: (a[5] as number) || 0,
  users: (a[6] as number) || 0, size: (a[7] as number) || 0, members: (a[8] as number) || 0, ch: (a[9] as number) || 0, ram: !!a[10],
});
/** "C042" for bank 2 row 42 (and a code from /pool?u=1: bank << 7 | row). */
export const rowName = (bank: number, row: number) => `${BANKS[bank & 7]}${String(row).padStart(3, "0")}`;
export const rowText = (code: number) => rowName((code >> 7) & 7, code & 127);

export interface PoolAnswer { row: string; res: string; n: number; text: string }
/** The log line that answers a pool= command for list k, or null. */
export function poolAnswer(line: string, k: string): PoolAnswer | null {
  const m = new RegExp(`\\bpool: ${k}([ :].*)$`).exec(line || "");
  if (!m) return null;
  const t = m[1], res = POOL_RES.find((r) => t.endsWith(`: ${r}`)) || "";
  const r = /^ ([A-H]\d{3}|-) /.exec(t), u = /^: (\d+) unloaded/.exec(t);
  return { row: r && r[1] !== "-" ? r[1] : "", res, n: u ? +u[1] : -1, text: t.replace(/^:?\s*/, "") };
}
/** P3: what a row in use asks before it is replaced or unloaded. */
export function inUseQuestion(row: string, e: PoolRow, verb: "REPLACE" | "UNLOAD"): string {
  if (e.users > 0) return `Row ${row} is used by ${e.users} sound${e.users === 1 ? "" : "s"}. ${verb === "REPLACE" ? "Replace" : "Unload"} it?`;
  return `Row ${row} was played in this project${e.used ? ` (${e.used}x)` : ""}. ${verb === "REPLACE" ? "Replace" : "Unload"} it?`;
}
export const inUse = (e: PoolRow) => e.users > 0 || e.used > 0;

/** The rows to unload as console lines of at most 190 characters (the console's 192); ":yes" for the confirmed ones. */
export function unloadLines(k: string, rows: string[], yes: Set<string>): string[] {
  const head = `pool=${k}:unload:`, out: string[] = [];
  let cur = "";
  for (const r of rows) {
    const item = r + (yes.has(r) ? ":yes" : "");
    if (cur && head.length + cur.length + 1 + item.length > 190) {
      out.push(head + cur);
      cur = "";
    }
    cur += (cur ? "," : "") + item;
  }
  if (cur) out.push(head + cur);
  return out;
}

/** What an answer means, in words; bad: it did not happen. */
export function answerText(op: "replace" | "unload" | "accept" | "undo" | "add", row: string, a: PoolAnswer | null, name = ""): [string, boolean] {
  if (!a) return [`${row}: sent, no answer yet (see the log)`, true];
  if (a.res === "PLAYING: STOP FIRST") return [`${row} is in use: not changed while the Rytm plays (stop it first)`, true];
  if (op === "unload") return a.n > 0 ? [`${a.n} unloaded (Undo takes it back)`, false] : [`${row}: ${a.res || a.text}`, true];
  if (op === "undo" || op === "accept") return [a.text, !!a.res && a.res !== "SET"];
  if (a.res === "SET") return [`${a.row || row} = ${name} (Undo takes it back)`, false];
  if (a.res === "IN POOL") return [`${name} is in ${a.row} already: one row per file`, true];
  if (a.res === "READING") return [`${row}: reading the .sfz (the row shows it when done)`, false];
  return [`${row}: ${a.res || a.text}`, !!a.res && a.res !== "SET"];
}
