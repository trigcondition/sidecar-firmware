import { describe, expect, it } from "vitest";
import { Api } from "./api";
import { collectBlackbox, fileName, readBlackboxFile } from "./blackbox";
import { maskSecrets } from "./prevboot";

const enc = new TextEncoder();
// what the engine really sends to an unpaired client (web.cpp PairRefused): HTTP 200, a JSON body, X-Sidecar-Pair
const refusal = () => new Response('{"ok":false,"code":401,"pair":true,"wait":0,"error":"not paired"}', { headers: { "Content-Type": "application/json", "X-Sidecar-Pair": "required" } });
const refusalBodyOnly = () => new Response('{"ok":false,"code":401,"pair":true,"wait":0,"error":"not paired"}');

// a fake /blackbox: files by name; every reply is "SIZE FROM\n" + up to `chunk` bytes from `off`
function fake(files: Record<string, string | null>, opt: { chunk?: number; fail?: (n: number) => number | null; onReq?: (url: string, n: number) => void } = {}) {
  let n = 0;
  const urls: string[] = [];
  const fetchFn = (async (url: string) => {
    n++;
    urls.push(url);
    opt.onReq?.(url, n);
    const f = opt.fail?.(n);
    if (f) return new Response("", { status: f });
    const u = new URL(url, "http://x");
    if (u.pathname === "/update/state") return new Response(JSON.stringify({ board: 5 }));
    const body = files[u.pathname === "/prevboot" ? "prevboot" : u.searchParams.get("old") ? "old" : "new"];
    if (u.pathname === "/prevboot") return body == null ? new Response("", { status: 404 }) : new Response(body);
    const bytes = enc.encode(body ?? "");
    const q = u.searchParams.get("off");
    const from = q == null ? Math.max(0, bytes.length - (opt.chunk ?? 65536)) : Math.min(Number(q), bytes.length);
    const part = bytes.slice(from, from + (opt.chunk ?? 65536));
    return new Response(new Blob([enc.encode(`${bytes.length} ${from}\n`), part]));
  }) as unknown as typeof fetch;
  return { api: new Api({ token: () => "t", fetchFn }), urls, count: () => n };
}

describe("reading one black box file", () => {
  it("reads it in chunks, from the start, until the whole file is there", async () => {
    const text = "line one\nline two\nline three\n";
    const f = fake({ new: text }, { chunk: 7 });
    const seen: number[] = [];
    const r = await readBlackboxFile(f.api, false, (done, size) => seen.push(done / size));
    expect(r).toEqual({ kind: "ok", text, bytes: text.length });
    expect(f.urls[0]).toBe("/blackbox?off=0");
    expect(seen[seen.length - 1]).toBe(1);
  });
  it("asks for blackbox.1.log with old=1", async () => {
    const f = fake({ old: "older\n" });
    expect(await readBlackboxFile(f.api, true)).toMatchObject({ kind: "ok", text: "older\n" });
    expect(f.urls[0]).toBe("/blackbox?old=1&off=0");
  });
  it("keeps multi-byte text whole when a chunk ends inside a character", async () => {
    const text = "café ♪ ok\n";
    const f = fake({ new: text }, { chunk: 4 });
    expect(await readBlackboxFile(f.api, false)).toMatchObject({ kind: "ok", text });
  });
  it("says none for a missing or empty file", async () => {
    expect(await readBlackboxFile(fake({ new: null }).api, true)).toEqual({ kind: "none" });
    expect(await readBlackboxFile(fake({ old: "" }).api, true)).toEqual({ kind: "none" });
    expect(await readBlackboxFile(fake({}, { fail: () => 404 }).api, true)).toEqual({ kind: "none" });
  });
  it("tells to pair on 401 and 403", async () => {
    for (const s of [401, 403]) expect(await readBlackboxFile(fake({}, { fail: () => s }).api, false)).toMatchObject({ kind: "error", text: expect.stringMatching(/Pair this device/) });
  });
  it("tells to pair on the engine's real refusal (HTTP 200 + JSON + X-Sidecar-Pair, or the JSON alone)", async () => {
    for (const mk of [refusal, refusalBodyOnly]) {
      const api = new Api({ token: () => "t", fetchFn: (async () => mk()) as unknown as typeof fetch });
      expect(await readBlackboxFile(api, false)).toMatchObject({ kind: "error", text: expect.stringMatching(/Pair this device/) });
    }
  });
  it("gives a clear error when a chunk fails, saying how far it got", async () => {
    const f = fake({ new: "x".repeat(40) }, { chunk: 10, fail: (n) => (n === 3 ? 500 : null) });
    const r = await readBlackboxFile(f.api, false);
    expect(r).toMatchObject({ kind: "error", text: expect.stringMatching(/500.*20 of 40/) });
    expect(f.count()).toBe(3);
  });
  it("turns a network failure into an error", async () => {
    const api = new Api({ token: () => "t", fetchFn: (async () => { throw new Error("offline"); }) as unknown as typeof fetch });
    expect(await readBlackboxFile(api, false)).toEqual({ kind: "error", text: "offline" });
  });
  it("follows a file that grows while it is read", async () => {
    let text = "a".repeat(20);
    let calls = 0;
    const fetchFn = (async (url: string) => {
      const off = Number(new URL(url, "http://x").searchParams.get("off"));
      if (++calls === 2) text += "b".repeat(10);
      const bytes = enc.encode(text);
      return new Response(new Blob([enc.encode(`${bytes.length} ${off}\n`), bytes.slice(off, off + 10)]));
    }) as unknown as typeof fetch;
    const r = await readBlackboxFile(new Api({ token: () => "t", fetchFn }), false);
    expect(r).toMatchObject({ kind: "ok", text: "a".repeat(20) + "b".repeat(10) });
  });
  it("stops (no endless loop) when the file never ends, or the Sidecar sends nothing", async () => {
    let grow = 0;
    const endless = (async (url: string) => {
      const off = Number(new URL(url, "http://x").searchParams.get("off"));
      grow += 10;
      return new Response(new Blob([enc.encode(`${off + 20} ${off}\n`), enc.encode("y".repeat(10))]));
    }) as unknown as typeof fetch;
    const r = await readBlackboxFile(new Api({ token: () => "t", fetchFn: endless }), false, undefined, 50);
    expect(r).toMatchObject({ kind: "ok" });
    expect(grow).toBeLessThanOrEqual(500);
    const silent = (async () => new Response(new Blob([enc.encode("100 0\n")]))) as unknown as typeof fetch;
    expect(await readBlackboxFile(new Api({ token: () => "t", fetchFn: silent }), false)).toMatchObject({ kind: "error", text: expect.stringMatching(/stopped/) });
  });
  it("notes a rotation (the file got shorter than what was read) and keeps what it has", async () => {
    let calls = 0;
    const fetchFn = (async () => {
      calls++;
      if (calls === 1) return new Response(new Blob([enc.encode("30 0\n"), enc.encode("z".repeat(10))]));
      return new Response(new Blob([enc.encode(`5 5\n`)]));   // rotated: tiny new file, offset clamped
    }) as unknown as typeof fetch;
    const r = await readBlackboxFile(new Api({ token: () => "t", fetchFn }), false);
    expect(r).toMatchObject({ kind: "ok", text: expect.stringMatching(/^z{10}\n\[rotated while read; later lines are missing/) });
  });
  it("refuses a reply without the SIZE FROM line", async () => {
    const f = (async () => new Response("<html>")) as unknown as typeof fetch;
    expect(await readBlackboxFile(new Api({ token: () => "t", fetchFn: f }), false)).toMatchObject({ kind: "error" });
  });
});

describe("the download", () => {
  const now = new Date(2026, 10, 19, 9, 5);
  it("names the file by local date and time", () => {
    expect(fileName(now)).toBe("sidecar-blackbox-20261119-0905.txt");
  });
  it("joins header, prevboot, blackbox.1.log and blackbox.log, and masks secrets", async () => {
    const f = fake({ prevboot: "crash report\n", old: "old line\n", new: "new line paircode=12345678\n" });
    const r = await collectBlackbox(f.api, { version: "0.5.1", commit: "abc1234", now });
    expect(r.kind).toBe("ok");
    if (r.kind !== "ok") return;
    expect(r.text).toMatch(/^Sidecar black box/);
    expect(r.text).toContain("Sidecar version: 0.5.1 (abc1234)");
    expect(r.text).toContain("Board: Raspberry Pi 5");
    expect(r.text).toContain("Downloaded: 2026-11-19 09:05");
    const i = (s: string) => r.text.indexOf(s);
    expect(i("crash report")).toBeGreaterThan(0);
    expect(i("crash report")).toBeLessThan(i("old line"));
    expect(i("old line")).toBeLessThan(i("new line"));
    expect(r.text).toContain("paircode=********");
    expect(r.text).not.toContain("12345678");
    expect(r.name).toBe("sidecar-blackbox-20261119-0905.txt");
  });
  it("leaves out a missing prevboot and an empty old file, but says so", async () => {
    const f = fake({ prevboot: null, old: "", new: "only\n" });
    const r = await collectBlackbox(f.api, { version: "0.5.1", now });
    expect(r).toMatchObject({ kind: "ok", text: expect.stringContaining("only\n") });
    if (r.kind === "ok") expect(r.text).toMatch(/blackbox\.1\.log: none/);
  });
  it("fails as a whole on the real refusal", async () => {
    const api = new Api({ token: () => "t", fetchFn: (async () => refusal()) as unknown as typeof fetch });
    expect(await collectBlackbox(api, { version: "0.5.1", now })).toMatchObject({ kind: "error", text: expect.stringMatching(/Pair this device/) });
  });
  it("fails as a whole when pairing is missing", async () => {
    const f = fake({}, { fail: (n) => (n >= 1 ? 401 : null) });
    expect(await collectBlackbox(f.api, { version: "0.5.1", now })).toMatchObject({ kind: "error", text: expect.stringMatching(/Pair this device/) });
  });
});

describe("masking what a black box could hold", () => {
  it("masks Wi-Fi keys and joins, tokens and the device names given at pairing", () => {
    expect(maskSecrets("wifi=join:HomeNet:hunter22")).toBe("wifi=join:********");
    expect(maskSecrets("network={ ssid=\"Home\" psk=\"hunter22\" }")).toBe("network={ ssid=\"Home\" psk=******** }");
    expect(maskSecrets("passphrase: hunter22 ok")).toBe("passphrase: ******** ok");
    expect(maskSecrets("password=abc")).toBe("password=********");
    const tok = "a".repeat(64);
    expect(maskSecrets(`X-Sidecar-Auth: ${tok}`)).toBe("X-Sidecar-Auth: ********");
    expect(maskSecrets(`token=${tok} done`)).toBe("token=******** done");
    expect(maskSecrets("a device paired with the code (\"Anna's laptop\"): 2 paired")).toBe("a device paired with the code (\"********\"): 2 paired");
  });
  it("leaves checksums and ordinary numbers alone", () => {
    const sha = "b".repeat(64);
    expect(maskSecrets(`image sha256 ${sha}`)).toBe(`image sha256 ${sha}`);
    expect(maskSecrets("key stored in /.rytm/wifi.cfg (its key, not the passphrase)")).toBe("key stored in /.rytm/wifi.cfg (its key, not the passphrase)");
  });
});
