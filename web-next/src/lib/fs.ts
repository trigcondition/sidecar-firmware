// web-next/src/lib/fs.ts - the card's files through /fs (pi/engine/fsweb.h): list, download in 64 KB pieces, delete,
// rename, make a folder. /fs answers "<code> <msg>" as text with HTTP 200 (ls: JSON); this turns that into results.
import { q, type Api } from "./api";

export interface FsEntry { n: string; d?: boolean | number; s: number; t?: string; ro?: number }
export interface FsList { path: string; free?: number; total?: number; more?: boolean; entries: FsEntry[] }
export interface FsResult { code: number; msg: string; ok: boolean }

/** "<code> <msg>" (fsweb's answers) as a result. */
export function fsResult(text: string): FsResult {
  const m = /^(\d{3})\s*([\s\S]*)$/.exec(text.trim());
  const code = m ? +m[1] : 500;
  return { code, msg: m ? m[2] : text.trim(), ok: code === 200 };
}

export function createFs(api: Api) {
  return {
    async ls(path: string): Promise<FsList | null> {
      const r = await api.request(q("/fs/ls", { path }));
      const t = await r.text();
      try {
        return JSON.parse(t) as FsList;
      } catch {
        return null; // "<code> <msg>": not there, or not reachable
      }
    },
    async post(op: "del" | "mkdir" | "mv", params: Record<string, string>): Promise<FsResult> {
      const r = await api.request(q(`/fs/${op}`, params), { method: "POST" });
      return fsResult(await r.text());
    },
    del(path: string) {
      return this.post("del", { path });
    },
    /** The whole file, in 64 KB pieces (the old page's loop); onProgress gets bytes so far. */
    async get(path: string, size: number, onProgress?: (n: number) => void, signal?: AbortSignal): Promise<Blob> {
      const parts: ArrayBuffer[] = [];
      let off = 0;
      while (off < size) {
        if (signal?.aborted) throw new Error("cancelled");
        const r = await api.request(q("/fs/get", { path, off, len: 65536 }), { signal });
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        const b = await r.arrayBuffer();
        if (b.byteLength === 0) break;
        parts.push(b);
        off += b.byteLength;
        onProgress?.(off);
      }
      return new Blob(parts);
    },
  };
}

/** Hands a Blob to the browser as a download. */
export function saveBlob(blob: Blob, name: string): void {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

/** Bytes as KB / MB / GB. */
export function hb(n: number): string {
  return n >= 1 << 30 ? `${(n / (1 << 30)).toFixed(1)} GB` : n >= 1 << 20 ? `${(n / (1 << 20)).toFixed(1)} MB` : n >= 1024 ? `${Math.round(n / 1024)} KB` : `${n} B`;
}
