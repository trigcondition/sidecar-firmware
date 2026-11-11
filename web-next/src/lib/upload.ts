// web-next/src/lib/upload.ts - an upload to the card, the old page's way (fsweb.h /fs/put): the whole file's CRC-32
// (zlib/IEEE) first, then 48 KB pieces, each answered "200 ok <next>" / "200 done <bytes>" / "409 expected <off>"
// (resumed, at most 6 times); on a failure the .part is removed (not for 403 / 507). Framework-free (upload.test.ts).
import { q, type Api } from "./api";
import { fsResult } from "./fs";

export const CHUNK = 49152;
let table: Uint32Array | null = null;
/** CRC-32 over bytes, continuing from c (start with 0xffffffff, finish with ^ 0xffffffff >>> 0). */
export function crc32Update(c: number, b: Uint8Array): number {
  if (!table) {
    table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let x = i;
      for (let k = 0; k < 8; k++) x = x & 1 ? 0xedb88320 ^ (x >>> 1) : x >>> 1;
      table[i] = x;
    }
  }
  for (let i = 0; i < b.length; i++) c = table[(c ^ b[i]) & 255] ^ (c >>> 8);
  return c;
}
export const crc32 = (b: Uint8Array) => ((crc32Update(0xffffffff, b) ^ 0xffffffff) >>> 0);

/** What an upload needs of a file (a browser File, or a Blob with a name in tests). */
export interface Source { size: number; name: string; slice(a: number, b: number): Blob }

export async function fileCrc(f: Source, onProgress?: (frac: number) => void, cancelled?: () => boolean): Promise<number> {
  let c = 0xffffffff;
  for (let p = 0; p < f.size; p += 1 << 20) {
    if (cancelled?.()) throw new Error("cancelled");
    const b = new Uint8Array(await f.slice(p, p + (1 << 20)).arrayBuffer());
    c = crc32Update(c, b);
    onProgress?.((p + b.length) / f.size);
  }
  return (c ^ 0xffffffff) >>> 0;
}

export interface UploadProgress { phase: "checking" | "sending" | "resuming" | "done"; frac: number; text: string }

/** Another endpoint with the same protocol (the web update's /update/put: path is ignored there) and its own cleanup. */
export interface UploadTarget { put: string; cleanup: (api: Api) => Promise<unknown> }

/** One file to path on the card. Resolves with "done, N bytes"; throws "<code> <msg>" or "cancelled". */
export async function uploadFile(api: Api, f: Source, path: string, onProgress?: (p: UploadProgress) => void, cancelled?: () => boolean, target?: UploadTarget): Promise<string> {
  onProgress?.({ phase: "checking", frac: 0, text: "checking" });
  const crc = await fileCrc(f, (x) => onProgress?.({ phase: "checking", frac: x, text: `checking ${Math.floor(100 * x)} %` }), cancelled);
  let off = 0, resumes = 0;
  try {
    for (;;) {
      if (cancelled?.()) throw new Error("cancelled");
      const fd = new FormData();
      fd.append("f", f.slice(off, off + CHUNK), f.name);
      const r = await api.request(q(target?.put ?? "/fs/put", { path, off, total: f.size, crc }), { method: "POST", body: fd });
      const a = fsResult(await r.text());
      let m: RegExpExecArray | null;
      if (a.code === 200 && (m = /^(ok|done)\s+(\d+)/.exec(a.msg))) {
        if (m[1] === "done") {
          onProgress?.({ phase: "done", frac: 1, text: "done" });
          return `done, ${m[2]} bytes`;
        }
        off = +m[2];
        onProgress?.({ phase: "sending", frac: off / Math.max(1, f.size), text: `${Math.floor((100 * off) / Math.max(1, f.size))} %` });
      } else if (a.code === 409 && (m = /^expected\s+(\d+)/.exec(a.msg))) {
        if (++resumes > 6) throw new Error(`409 ${a.msg} (gave up after 6 resumes)`);
        off = +m[1];
        onProgress?.({ phase: "resuming", frac: off / Math.max(1, f.size), text: `resuming at ${off}` });
      } else throw new Error(`${a.code} ${a.msg}`);
    }
  } catch (e) {
    const msg = (e as Error).message;
    if (!/^(403|507) /.test(msg)) await (target ? target.cleanup(api) : api.request(q("/fs/del", { path: `${path}.part` }), { method: "POST" })).catch(() => undefined);
    throw e;
  }
}
