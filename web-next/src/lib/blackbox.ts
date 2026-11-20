// The black box as one downloadable text (GET /blackbox, GET /blackbox?old=1, GET /prevboot; pi/engine/web.cpp, blackbox_pi.h).
// /blackbox answers with at most 64 KB: a first line "SIZE FROM" (the file's size now; the offset the bytes start at), then
// those bytes. All three are paired-only, so the api adds the pairing token as a header (a plain link would be refused).
// The file is read from off=0 in chunks until it is whole; one that grows while it is read is followed, one that is rotated
// meanwhile (the offset comes back smaller than asked) is cut with a note, and a reply that does not advance ends the
// download with an error, so no case loops. The joined text goes through maskSecrets (prevboot.ts) before it is offered.
import type { Api } from "./api";
import { PAIR_FIRST as PAIR, loadPrevBoot, maskSecrets, pairRefused } from "./prevboot";

export type BlackboxPart = { kind: "ok"; text: string; bytes: number } | { kind: "none" } | { kind: "error"; text: string };
export type Collected = { kind: "ok"; text: string; name: string; bytes: number } | { kind: "error"; text: string };

const MAX_CHUNKS = 2000; // 2000 x 64 KB: far above the rotation size, so only a file that never stops growing reaches it

export async function readBlackboxFile(
  api: Pick<Api, "request">,
  old: boolean,
  onProgress?: (done: number, size: number) => void,
  maxChunks = MAX_CHUNKS,
): Promise<BlackboxPart> {
  let all = new Uint8Array(0); // filled as the chunks come (no list of pieces to join at the end)
  let off = 0;
  let size = 0;
  let note = "";
  try {
    for (let i = 0; ; i++) {
      const r = await api.request(`/blackbox?${old ? "old=1&" : ""}off=${off}`);
      if (r.status === 404) return { kind: "none" };
      if (r.status === 401 || r.status === 403) return { kind: "error", text: PAIR };
      if (!r.ok) return { kind: "error", text: `The Sidecar answered ${r.status}${size ? ` after ${off} of ${size} bytes` : ""}.` };
      const buf = new Uint8Array(await r.arrayBuffer());
      if (pairRefused(r, new TextDecoder().decode(buf.subarray(0, 64)))) return { kind: "error", text: PAIR };
      const nl = buf.subarray(0, 64).indexOf(10);
      const m = nl > 0 ? /^(\d+) (\d+)$/.exec(new TextDecoder().decode(buf.subarray(0, nl))) : null;
      if (!m) return { kind: "error", text: "The Sidecar's answer was not a black box chunk." };
      size = Number(m[1]);
      const from = Number(m[2]);
      if (from !== off) {
        note = "\n[rotated while read; later lines are missing]\n";
        break;
      }
      const body = buf.subarray(nl + 1);
      if (off + body.length > all.length) {
        const bigger = new Uint8Array(Math.max(size, off + body.length));
        bigger.set(all.subarray(0, off));
        all = bigger;
      }
      all.set(body, off);
      off += body.length;
      onProgress?.(Math.min(off, size), size);
      if (off >= size) break;
      if (!body.length) return { kind: "error", text: `The download stopped: the Sidecar sent nothing at ${off} of ${size} bytes.` };
      if (i + 1 >= maxChunks) {
        note = `\n[stopped after ${i + 1} chunks: the file was still growing]\n`;
        break;
      }
    }
  } catch (e) {
    return { kind: "error", text: (e as Error).message };
  }
  if (!off && !note) return { kind: "none" };
  const text = new TextDecoder().decode(all.subarray(0, off)) + note;
  return { kind: "ok", text, bytes: off };
}

const two = (n: number) => String(n).padStart(2, "0");
const stamp = (d: Date) => `${d.getFullYear()}${two(d.getMonth() + 1)}${two(d.getDate())}-${two(d.getHours())}${two(d.getMinutes())}`;
export const fileName = (d: Date) => `sidecar-blackbox-${stamp(d)}.txt`;

export interface CollectOptions {
  version: string;
  commit?: string;
  now?: Date;
  /** which file is being read ("blackbox.log"), and how far */
  onProgress?: (label: string, done: number, size: number) => void;
}

export async function collectBlackbox(api: Api, opt: CollectOptions): Promise<Collected> {
  const now = opt.now ?? new Date();
  let board = "";
  try {
    board = String((await api.json<{ board?: number }>("/update/state")).board ?? "");
  } catch { /* the header just goes without */ }
  const sections: string[] = [];
  const pre = await loadPrevBoot(api);
  if (pre.kind === "error" && pre.text === PAIR) return { kind: "error", text: PAIR };
  sections.push(pre.kind === "ok" ? `===== prevboot.txt (the last crashed boot) =====\n${pre.text}` : `===== prevboot.txt: ${pre.kind === "none" ? "none" : `not available (${pre.text})`} =====`);
  for (const [label, old] of [["blackbox.1.log", true], ["blackbox.log", false]] as const) {
    const r = await readBlackboxFile(api, old, (d, s) => opt.onProgress?.(label, d, s));
    if (r.kind === "error") return { kind: "error", text: `${label}: ${r.text}` };
    sections.push(r.kind === "ok" ? `===== ${label} =====\n${r.text}` : `===== ${label}: none =====`);
  }
  const head = [
    "Sidecar black box",
    `Sidecar version: ${opt.version || "unknown"}${opt.commit ? ` (${opt.commit})` : ""}`,
    `Board: ${board ? `Raspberry Pi ${board}` : "unknown"}`,
    `Downloaded: ${now.getFullYear()}-${two(now.getMonth() + 1)}-${two(now.getDate())} ${two(now.getHours())}:${two(now.getMinutes())}`,
  ].join("\n");
  const text = maskSecrets(`${head}\n\n${sections.join("\n")}`);
  return { kind: "ok", text, name: fileName(now), bytes: new TextEncoder().encode(text).length };
}
