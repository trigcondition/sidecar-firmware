// web-next/src/lib/update.ts - Settings › Update (docs/web-update.md): the update file's header read and checked in the
// page before it goes up (tools/make_update.py writes it, pi/engine/update.cpp checks it again), the verdict against
// the running version, and what /update/state says after the reboot. Framework-free (update.test.ts).

export const MAGIC = "SCUPDAT1";
export const HEADER = 512;
export const PART = { pi4: 1, pi5: 2, web: 3 } as const;
const KIND: Record<number, string> = { 1: "Pi 4 engine", 2: "Pi 5 engine", 3: "web app" };

// ---- SHA-256 (crypto.subtle is missing on plain http, which is how the page reaches the Sidecar) ----
const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
  0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
  0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

/** Incremental SHA-256: update() with pieces, digest() once. */
export class Sha256 {
  private h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  private buf = new Uint8Array(64);
  private n = 0;
  private len = 0;
  private w = new Uint32Array(64);

  private block(b: Uint8Array, o: number) {
    const w = this.w, h = this.h;
    for (let i = 0; i < 16; i++) w[i] = (b[o + 4 * i] << 24) | (b[o + 4 * i + 1] << 16) | (b[o + 4 * i + 2] << 8) | b[o + 4 * i + 3];
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15], c = w[i - 2];
      const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
      const s1 = ((c >>> 17) | (c << 15)) ^ ((c >>> 19) | (c << 13)) ^ (c >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let A = h[0], B = h[1], C = h[2], D = h[3], E = h[4], F = h[5], G = h[6], H = h[7];
    for (let i = 0; i < 64; i++) {
      const S1 = ((E >>> 6) | (E << 26)) ^ ((E >>> 11) | (E << 21)) ^ ((E >>> 25) | (E << 7));
      const t1 = (H + S1 + ((E & F) ^ (~E & G)) + K[i] + w[i]) | 0;
      const S0 = ((A >>> 2) | (A << 30)) ^ ((A >>> 13) | (A << 19)) ^ ((A >>> 22) | (A << 10));
      const t2 = (S0 + ((A & B) ^ (A & C) ^ (B & C))) | 0;
      H = G; G = F; F = E; E = (D + t1) | 0; D = C; C = B; B = A; A = (t1 + t2) | 0;
    }
    h[0] += A; h[1] += B; h[2] += C; h[3] += D; h[4] += E; h[5] += F; h[6] += G; h[7] += H;
  }

  update(b: Uint8Array): this {
    let i = 0;
    this.len += b.length;
    if (this.n) {
      while (this.n < 64 && i < b.length) this.buf[this.n++] = b[i++];
      if (this.n < 64) return this;
      this.block(this.buf, 0);
      this.n = 0;
    }
    for (; i + 64 <= b.length; i += 64) this.block(b, i);
    while (i < b.length) this.buf[this.n++] = b[i++];
    return this;
  }

  digest(): Uint8Array {
    const bits = this.len * 8;
    const pad = new Uint8Array((this.n < 56 ? 56 : 120) - this.n + 8);
    pad[0] = 0x80;
    const dv = new DataView(pad.buffer);
    dv.setUint32(pad.length - 8, Math.floor(bits / 2 ** 32));
    dv.setUint32(pad.length - 4, bits >>> 0);
    this.update(pad);
    const out = new Uint8Array(32);
    const o = new DataView(out.buffer);
    for (let i = 0; i < 8; i++) o.setUint32(4 * i, this.h[i]);
    return out;
  }
}
export const sha256 = (b: Uint8Array) => new Sha256().update(b).digest();
export const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");

// ---- the header ----
export interface UpdatePart { kind: number; what: string; off: number; size: number; sha: string }
export interface UpdateHeader { version: string; commit: string; dirty: boolean; parts: UpdatePart[] }

const cstr = (b: Uint8Array) => {
  const z = b.indexOf(0);
  return new TextDecoder().decode(z < 0 ? b : b.subarray(0, z));
};

/** The 512-byte header, checked as the Pi checks it (its own sha256, parts back to back up to fileSize). Throws a
 *  sentence for the page. */
export function parseHeader(h: Uint8Array, fileSize: number): UpdateHeader {
  if (h.length < HEADER || cstr(h.subarray(0, 8)) !== MAGIC) throw new Error("This is not a Sidecar update file (it ends in .scu and comes from the release page).");
  const dv = new DataView(h.buffer, h.byteOffset, HEADER);
  const n = dv.getUint32(16, true);
  if (dv.getUint32(8, true) !== HEADER || dv.getUint32(12, true) !== 1) throw new Error("This update file is of a newer format: update the Sidecar from a card in a computer, or use a newer page.");
  if (hex(sha256(h.subarray(0, 480))) !== hex(h.subarray(480, 512))) throw new Error("The update file is damaged (its header): download it again.");
  if (n < 1 || n > 6) throw new Error("The update file is damaged (its parts): download it again.");
  const parts: UpdatePart[] = [];
  let at = HEADER;
  for (let i = 0; i < n; i++) {
    const o = 80 + 64 * i;
    const kind = dv.getUint32(o, true);
    const off = dv.getUint32(o + 8, true) + dv.getUint32(o + 12, true) * 2 ** 32;
    const size = dv.getUint32(o + 16, true) + dv.getUint32(o + 20, true) * 2 ** 32;
    if (off !== at || off + size > fileSize) throw new Error(fileSize < off + size ? "The update file is incomplete: download it again." : "The update file is damaged (its parts): download it again.");
    parts.push({ kind, what: KIND[kind] ?? `part ${kind}`, off, size, sha: hex(h.subarray(o + 24, o + 56)) });
    at = off + size;
  }
  if (at !== fileSize) throw new Error("The update file has extra bytes at its end: download it again.");
  const version = cstr(h.subarray(24, 48)), commit = cstr(h.subarray(48, 72));
  if (!version || !commit) throw new Error("The update file is damaged (its version): download it again.");
  return { version, commit, dirty: (dv.getUint32(20, true) & 1) !== 0, parts };
}

/** What a slice of the file needs (a browser File, or a Blob in tests). */
export interface Source { size: number; slice(a: number, b: number): Blob }

/** Every part's sha256 checked in the page (1 MB at a time), so a damaged download is refused before it goes up. */
export async function checkParts(f: Source, h: UpdateHeader, onProgress?: (frac: number) => void): Promise<void> {
  const total = h.parts.reduce((s, p) => s + p.size, 0) || 1;
  let done = 0;
  for (const p of h.parts) {
    const s = new Sha256();
    for (let x = 0; x < p.size; x += 1 << 20) {
      const b = new Uint8Array(await f.slice(p.off + x, p.off + Math.min(p.size, x + (1 << 20))).arrayBuffer());
      s.update(b);
      done += b.length;
      onProgress?.(done / total);
    }
    if (hex(s.digest()) !== p.sha) throw new Error(`The update file is damaged (the ${p.what}): download it again.`);
  }
}

// ---- versions (update.cpp's cmp_version: MAJOR.MINOR.PATCH[-pre], a release above its pre-releases, -dev lowest) ----
function parseVersion(v: string): [number, number, number, string | null] | null {
  const m = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$/.exec(v.trim());
  return m ? [+m[1], +m[2], +m[3], m[4] ?? null] : null;
}
/** -1 / 0 / 1; -2 when either is not a version. */
export function cmpVersion(a: string, b: string): number {
  const x = parseVersion(a), y = parseVersion(b);
  if (!x || !y) return a === b ? 0 : -2;
  for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return (x[i] as number) < (y[i] as number) ? -1 : 1;
  if (x[3] === y[3]) return 0;
  if (x[3] === null) return 1;
  if (y[3] === null) return -1;
  if ((x[3] === "dev") !== (y[3] === "dev")) return x[3] === "dev" ? -1 : 1;   // -dev below every other pre-release
  return x[3] < y[3] ? -1 : 1;
}

// ---- /update/state ----
export interface UpdateState {
  api?: string; enabled?: boolean; why?: string; board?: number; version?: string; commit?: string; trial?: boolean;
  state?: string; kind?: string; target?: string; from?: string; msg?: string; prev?: boolean; prevVersion?: string;
  uploaded?: number; commitIn?: number; uptime?: number;
}

export type Verdict =
  | { ok: true; force: false; text: string }
  | { ok: true; force: true; text: string }
  | { ok: false; text: string };

/** The file against the Sidecar that runs: its board in it, newer (or the force box for the same or an older one). */
export function verdict(h: UpdateHeader, s: UpdateState | null): Verdict {
  if (!s || !s.api) return { ok: false, text: "This Sidecar's firmware cannot update from the page: update it once from a card in a computer (docs: Updating)." };
  if (!s.enabled) return { ok: false, text: `Updates from the page are off: ${s.why || "webupdate=0 in cmdline.txt"}.` };
  const want = s.board === 5 ? PART.pi5 : PART.pi4;
  if (!h.parts.some((p) => p.kind === want)) return { ok: false, text: `This file has no engine for this Sidecar's board (Raspberry Pi ${s.board ?? "?"}).` };
  if (s.trial || s.state === "trying" || s.state === "committing") return { ok: false, text: "An update is still being tried: wait until it is permanent (about half a minute after the restart)." };
  const to = `${h.version} (${h.commit})`, now = `${s.version ?? "?"} (${s.commit ?? "?"})`;
  if (h.commit === s.commit && h.version === s.version) return { ok: true, force: true, text: `The Sidecar already runs ${to}. Install it again only to repair it.` };
  const c = cmpVersion(h.version, s.version ?? "");
  if (c > 0) return { ok: true, force: false, text: `Update from ${now} to ${to}.` };
  if (c === 0) return { ok: true, force: true, text: `${to} is the same version as the running ${now} (another build of it).` };
  if (c === -1) return { ok: true, force: true, text: `${to} is older than the running ${now}: that is a downgrade.` };
  return { ok: true, force: true, text: `The versions cannot be compared (${to} against ${now}).` };
}

/** What the page shows once the Sidecar answers again after the reboot. */
export type After = { phase: "waiting" | "trying" | "committed" | "reverted" | "failed" | "other"; text: string; done: boolean };
export function afterReboot(s: UpdateState | null, target: { version: string; commit: string } | null): After {
  if (!s) return { phase: "waiting", text: "The Sidecar is restarting…", done: false };
  const st = s.state ?? "idle";
  if (st === "trying" && s.trial) return { phase: "trying", text: `${s.version} (${s.commit}) is on trial: it becomes permanent in ${Math.max(0, s.commitIn ?? 0)} s. Do not switch the Sidecar off now.`, done: false };
  if (st === "committing" || st === "trying") return { phase: "trying", text: "Making the update permanent…", done: false };
  if (st === "committed") {
    const hit = !target || (s.version === target.version && s.commit === target.commit);
    return { phase: hit ? "committed" : "other", text: hit ? `Done: the Sidecar runs ${s.version} (${s.commit}).` : `The Sidecar runs ${s.version} (${s.commit}).`, done: true };
  }
  if (st === "reverted") return { phase: "reverted", text: s.msg || `The new firmware did not start: the Sidecar went back to ${s.version} (${s.commit}) by itself.`, done: true };
  if (st === "failed") return { phase: "failed", text: s.msg || "The update did not finish: the Sidecar runs its old firmware.", done: true };
  return { phase: "other", text: s.msg || `The Sidecar runs ${s.version} (${s.commit}).`, done: true };
}

/** /update/install, /previous, /discard answer this. */
export interface UpdateReply { ok: boolean; code: number; msg: string; reboot: "none" | "plain" | "tryboot" }

/** An answer from after the reboot (not the old engine still up for its last half second): its uptime is shorter than
 *  the time since the install's answer, give or take. */
export const rebooted = (s: UpdateState | null, secondsSinceReply: number) => !!s && typeof s.uptime === "number" && s.uptime <= secondsSinceReply + 1;

/** An engine without the web update (0.4.x after a direct "previous") has no /update/state: its /status (open on every
 *  version) stands in, so the page can still tell that it came back and what it runs. */
export function stateFromStatus(st: Record<string, unknown> | null): UpdateState | null {
  if (!st || typeof st.uptime !== "number" || typeof st.version !== "string") return null;
  const v = `${st.version} (${typeof st.commit === "string" ? st.commit : "?"})`;
  return { uptime: st.uptime, version: st.version, commit: typeof st.commit === "string" ? st.commit : "", state: "noupdate", msg: `The Sidecar runs ${v}. This version cannot update from the page: the next update goes by the SD card.` };
}

/** /update/previous's 409 for a previous version without the web update (update.cpp previous()): asks for force=1. */
export const asksForce = (r: UpdateReply) => r.code === 409 && r.reboot === "none" && /anyway\?$/.test(r.msg.trim());
