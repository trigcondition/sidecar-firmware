// web-next/src/lib/update.test.ts - Settings › Update's checks (update.ts): SHA-256, the file's header as
// tools/make_update.py writes it, the verdict against the running Sidecar, the answers after the reboot, the upload to
// /update/put.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { Api } from "./api";
import { uploadFile } from "./upload";
import { Follower, HEADER, PART, Sha256, afterReboot, asksForce, stateFromStatus, checkParts, cmpVersion, hex, parseHeader, rebooted, sha256, verdict, type UpdateState } from "./update";

const enc = (s: string) => new TextEncoder().encode(s);
const nodeSha = (b: Uint8Array) => createHash("sha256").update(b).digest("hex");

/** An update file as make_update.py build() makes it (the format in docs/web-update.md). */
function scu(parts: [number, Uint8Array][], version = "0.5.1", commit = "abcdef0", dirty = false): Uint8Array {
  const total = HEADER + parts.reduce((s, [, b]) => s + b.length, 0);
  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);
  out.set(enc("SCUPDAT1"), 0);
  dv.setUint32(8, HEADER, true);
  dv.setUint32(12, 1, true);
  dv.setUint32(16, parts.length, true);
  dv.setUint32(20, dirty ? 1 : 0, true);
  out.set(enc(version), 24);
  out.set(enc(commit), 48);
  let off = HEADER;
  parts.forEach(([kind, b], i) => {
    const o = 80 + 64 * i;
    dv.setUint32(o, kind, true);
    dv.setUint32(o + 8, off, true);
    dv.setUint32(o + 16, b.length, true);
    out.set(sha256(b), o + 24);
    out.set(b, off);
    off += b.length;
  });
  out.set(sha256(out.subarray(0, 480)), 480);
  return out;
}
const blob = (b: Uint8Array) => new Blob([b as Uint8Array<ArrayBuffer>]);
const rnd = (n: number, seed = 1) => {
  const b = new Uint8Array(n);
  let x = seed;
  for (let i = 0; i < n; i++) b[i] = (x = (x * 1103515245 + 12345) >>> 0) >>> 24;
  return b;
};

describe("sha256", () => {
  it("the FIPS 180-2 vectors", () => {
    expect(hex(sha256(new Uint8Array()))).toBe("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect(hex(sha256(enc("abc")))).toBe("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect(hex(sha256(enc("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))).toBe("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  });
  it("matches node's over every length around the block and in uneven pieces", () => {
    const b = rnd(3000, 7);
    for (const n of [55, 56, 63, 64, 65, 119, 120, 128, 1000, 3000]) expect(hex(sha256(b.subarray(0, n)))).toBe(nodeSha(b.subarray(0, n)));
    const s = new Sha256();
    for (let i = 0; i < b.length; i += 37) s.update(b.subarray(i, i + 37));
    expect(hex(s.digest())).toBe(nodeSha(b));
  });
});

describe("parseHeader", () => {
  const pi4 = rnd(5000, 2), pi5 = rnd(7000, 3), web = rnd(900, 4);
  const good = scu([[PART.pi4, pi4], [PART.pi5, pi5], [PART.web, web]]);

  it("reads the version, commit and parts", () => {
    const h = parseHeader(good.subarray(0, HEADER), good.length);
    expect(h.version).toBe("0.5.1");
    expect(h.commit).toBe("abcdef0");
    expect(h.dirty).toBe(false);
    expect(h.parts.map((p) => [p.kind, p.size])).toEqual([[1, 5000], [2, 7000], [3, 900]]);
    expect(h.parts[1].off).toBe(HEADER + 5000);
    expect(h.parts[2].sha).toBe(nodeSha(web));
  });

  it("refuses another file, a damaged header, a truncated or padded file", () => {
    expect(() => parseHeader(enc("PK\x03\x04 a zip".padEnd(600)), 600)).toThrow(/not a Sidecar update file/);
    const bad = good.slice();
    bad[30] ^= 1;
    expect(() => parseHeader(bad, bad.length)).toThrow(/damaged \(its header\)/);
    expect(() => parseHeader(good.subarray(0, HEADER), good.length - 1)).toThrow(/incomplete/);
    expect(() => parseHeader(good.subarray(0, HEADER), good.length + 4)).toThrow(/extra bytes/);
    const fmt = good.slice();
    new DataView(fmt.buffer).setUint32(12, 2, true);
    expect(() => parseHeader(fmt, fmt.length)).toThrow(/newer format/);
  });

  it("checkParts finds a flipped byte in any part", async () => {
    const h = parseHeader(good, good.length);
    let last = 0;
    await checkParts(blob(good), h, (x) => (last = x));
    expect(last).toBe(1);
    for (const at of [HEADER + 10, HEADER + 5000 + 6999, good.length - 1]) {
      const bad = good.slice();
      bad[at] ^= 0x40;
      await expect(checkParts(blob(bad), h)).rejects.toThrow(/damaged \(the (Pi 4 engine|Pi 5 engine|web app)\)/);
    }
  });

  // the same bytes as tools/make_update.py's build() (the Pi's test reads that one: pi/engine/tests/test_update.cpp)
  const tools = resolve(__dirname, "../../../tools");
  it.skipIf(!existsSync(`${tools}/make_update.py`))("reads make_update.py's file", () => {
    const py = `import sys; sys.path.insert(0, ${JSON.stringify(tools)}); import make_update as m
sys.stdout.buffer.write(m.build([(1, bytes(range(256)) * 9), (2, b"k5" * 1000), (3, b"SCWEB001" + bytes(4))], "0.6.0-rc1", "0123abc", True))`;
    const b = new Uint8Array(execFileSync("python3", ["-c", py]));
    const h = parseHeader(b, b.length);
    expect([h.version, h.commit, h.dirty]).toEqual(["0.6.0-rc1", "0123abc", true]);
    expect(h.parts.map((p) => [p.kind, p.size])).toEqual([[1, 2304], [2, 2000], [3, 12]]);
    expect(b).toEqual(scu([[1, b.subarray(512, 512 + 2304)], [2, b.subarray(2816, 4816)], [3, b.subarray(4816)]], "0.6.0-rc1", "0123abc", true));
  });
});

describe("cmpVersion (update.cpp's cmp_version)", () => {
  it("orders", () => {
    expect(cmpVersion("0.5.1", "0.5.0")).toBe(1);
    expect(cmpVersion("0.5.0", "0.10.0")).toBe(-1);
    expect(cmpVersion("0.5.0", "0.5.0")).toBe(0);
    expect(cmpVersion("0.5.0", "0.5.0-rc1")).toBe(1);
    expect(cmpVersion("0.5.0-rc2", "0.5.0-rc1")).toBe(1);
    expect(cmpVersion("0.5.0-dev", "0.5.0-alpha")).toBe(-1);
    expect(cmpVersion("x", "0.5.0")).toBe(-2);
    expect(cmpVersion("x", "x")).toBe(0);
  });
});

describe("verdict", () => {
  const h = parseHeader(scu([[PART.pi4, rnd(10)], [PART.pi5, rnd(10)]], "0.5.1", "bbbbbbb"), HEADER + 20);
  const on: UpdateState = { api: "sidecar-webupdate-1 v=0.5.0 c=aaaaaaa", enabled: true, board: 4, version: "0.5.0", commit: "aaaaaaa", state: "idle" };
  it("a newer file installs without the box", () => {
    expect(verdict(h, on)).toEqual({ ok: true, force: false, text: "Update from 0.5.0 (aaaaaaa) to 0.5.1 (bbbbbbb)." });
    expect(verdict(h, { ...on, board: 5 }).ok).toBe(true);
  });
  it("the same, the same version, an older one: the box", () => {
    expect(verdict(h, { ...on, version: "0.5.1", commit: "bbbbbbb" })).toMatchObject({ ok: true, force: true, text: expect.stringMatching(/already runs/) });
    expect(verdict(h, { ...on, version: "0.5.1", commit: "ccccccc" })).toMatchObject({ ok: true, force: true, text: expect.stringMatching(/same version/) });
    expect(verdict(h, { ...on, version: "0.6.0" })).toMatchObject({ ok: true, force: true, text: expect.stringMatching(/downgrade/) });
  });
  it("refuses: an old engine, updates off, no part for the board, a trial running", () => {
    expect(verdict(h, {}).ok).toBe(false);
    expect(verdict(h, null).ok).toBe(false);
    expect(verdict(h, { ...on, enabled: false, why: "webupdate=0" }).text).toMatch(/off: webupdate=0/);
    const only4 = parseHeader(scu([[PART.pi4, rnd(10)]]), HEADER + 10);
    expect(verdict(only4, { ...on, board: 5 }).text).toMatch(/no engine for this Sidecar's board \(Raspberry Pi 5\)/);
    expect(verdict(h, { ...on, trial: true, state: "trying" }).ok).toBe(false);
    expect(verdict(h, { ...on, state: "committing" }).ok).toBe(false);
  });
  it("accepts a new install on a trial boot that has committed (trial:true stays until the restart)", () => {
    expect(verdict(h, { ...on, trial: true, state: "committed" })).toMatchObject({ ok: true });
  });
});

describe("after the reboot", () => {
  const t = { version: "0.5.1", commit: "bbbbbbb" };
  it("waits, follows the trial, ends committed / reverted / failed", () => {
    expect(afterReboot(null, t)).toMatchObject({ phase: "waiting", done: false });
    expect(afterReboot({ state: "trying", trial: true, version: "0.5.1", commit: "bbbbbbb", commitIn: 12 }, t)).toMatchObject({ phase: "trying", done: false, text: expect.stringMatching(/permanent in 12 s/) });
    expect(afterReboot({ state: "committing" }, t).done).toBe(false);
    expect(afterReboot({ state: "committed", version: "0.5.1", commit: "bbbbbbb" }, t)).toMatchObject({ phase: "committed", done: true });
    expect(afterReboot({ state: "reverted", version: "0.5.0", commit: "aaaaaaa", msg: "0.5.1 did not start" }, t)).toMatchObject({ phase: "reverted", done: true, text: expect.stringMatching(/^Failed: back on 0\.5\.0.*0\.5\.1 did not start/) });
    expect(afterReboot({ state: "failed", msg: "" }, t)).toMatchObject({ phase: "failed", done: true });
    expect(afterReboot({ state: "committed", version: "0.4.4", commit: "x" }, null)).toMatchObject({ phase: "committed", done: true });
  });
  it("tells the old engine's last answer from the new one's", () => {
    expect(rebooted({ uptime: 3600 }, 3)).toBe(false);
    expect(rebooted({ uptime: 9 }, 10)).toBe(true);
    expect(rebooted({}, 10)).toBe(false);
    expect(rebooted(null, 10)).toBe(false);
  });
});

describe("the card after the restart", () => {
  const t = { version: "0.5.1", commit: "bbbbbbb" };
  it("says Trying ... permanent in N s, then Updated to, or Failed: back on", () => {
    expect(afterReboot({ state: "trying", trial: true, version: "0.5.1", commit: "bbbbbbb", commitIn: 12 }, t).text).toBe("Trying 0.5.1 (bbbbbbb): permanent in 12 s. Do not switch the Sidecar off now.");
    expect(afterReboot({ state: "committed", version: "0.5.1", commit: "bbbbbbb" }, t).text).toBe("Updated to 0.5.1 (bbbbbbb).");
    expect(afterReboot({ state: "reverted", version: "0.5.0", commit: "aaaaaaa", msg: "it hung" }, t).text).toBe("Failed: back on 0.5.0 (aaaaaaa). it hung");
    expect(afterReboot({ state: "failed", version: "0.5.0", commit: "aaaaaaa", msg: "" }, t).text).toMatch(/^Failed: back on 0\.5\.0 \(aaaaaaa\)/);
  });
  it("follows from the first answer after the gap, even when the uptime test is missed, through to Updated", () => {
    const f = new Follower(t);
    const trying = { api: "x", state: "trying", trial: true, version: "0.5.1", commit: "bbbbbbb", commitIn: 20, uptime: 100 };   // uptime > seconds since reply: rebooted() says no
    expect(f.step({ api: "x", state: "trying", uptime: 4000 }, 3).after.phase).toBe("waiting");   // the old engine's last answer
    expect(f.step(null, 8).after.phase).toBe("waiting");                                         // the gap
    const a = f.step(trying, 60);
    expect(a.state).toBe(trying);
    expect(a.after).toMatchObject({ phase: "trying", done: false, text: expect.stringMatching(/Trying 0\.5\.1 \(bbbbbbb\): permanent in 20 s/) });
    expect(f.step({ ...trying, commitIn: 8 }, 70).after.text).toMatch(/permanent in 8 s/);
    expect(f.step({ ...trying, state: "committed", commitIn: 0 }, 90).after).toMatchObject({ phase: "committed", done: true });
  });
  it("without a gap the uptime test decides; a page opened during a trial follows at once", () => {
    expect(new Follower(t).step({ api: "x", state: "trying", trial: true, uptime: 3600 }, 3).state).toBeNull();
    expect(new Follower(t).step({ api: "x", state: "trying", trial: true, uptime: 9, commitIn: 5 }, 12).after.phase).toBe("trying");
    expect(new Follower(null, true).step({ api: "x", state: "trying", trial: true, uptime: 3600, commitIn: 5, version: "1", commit: "c" }, 0).after.phase).toBe("trying");
  });
  it("a failed install that never restarts is not mistaken for a result", () => {
    const f = new Follower(t);
    expect(f.step({ api: "x", state: "reverted", uptime: 9000 }, 5).state).toBeNull();
  });
});

describe("the upload to /update/put", () => {
  it("sends the pieces there and asks /update/discard on a failure", async () => {
    const calls: string[] = [];
    let fail = false;
    const api = new Api({
      token: () => "t",
      fetchFn: (async (url: string, init?: RequestInit) => {
        calls.push(`${init?.method ?? "GET"} ${url.split("?")[0]}`);
        if (url.startsWith("/update/put")) {
          const off = +new URL(url, "http://x").searchParams.get("off")!;
          if (fail) return new Response("400 not a Sidecar update file\n");
          return new Response(off + 49152 >= 70000 ? "200 done 70000\n" : `200 ok ${off + 49152}\n`);
        }
        return new Response('{"ok":true,"code":200,"msg":"deleted","reboot":"none"}', { headers: { "Content-Type": "application/json" } });
      }) as typeof fetch,
    });
    const f = Object.assign(new Blob([rnd(70000) as Uint8Array<ArrayBuffer>]), { name: "sidecar-0.5.1.scu" });
    const target = { put: "/update/put", cleanup: (a: Api) => a.post("/update/discard") };
    expect(await uploadFile(api, f, "update.scu", undefined, undefined, target)).toBe("done, 70000 bytes");
    expect(calls).toEqual(["POST /update/put", "POST /update/put"]);
    calls.length = 0;
    fail = true;
    await expect(uploadFile(api, f, "update.scu", undefined, undefined, target)).rejects.toThrow(/400 not a Sidecar update file/);
    expect(calls).toEqual(["POST /update/put", "POST /update/discard"]);
  });
});

describe("the way back to a version without the web update", () => {
  it("knows the Pi's question for force=1 (update.cpp previous()) and nothing else", () => {
    const msg = "the previous version has no web update: going back swaps it in directly, without a trial, and coming back needs the SD card. Go back anyway?";
    expect(asksForce({ ok: false, code: 409, msg, reboot: "none" })).toBe(true);
    expect(asksForce({ ok: false, code: 409, msg: "a trial is running: wait for it to finish", reboot: "none" })).toBe(false);
    expect(asksForce({ ok: false, code: 404, msg: "no previous version on the card", reboot: "none" })).toBe(false);
  });
  it("after the swap, an engine without /update/state is followed by its /status", () => {
    const s = stateFromStatus({ uptime: 7, version: "0.4.4", commit: "9999999", host: "sidecar" });
    expect(rebooted(s, 20)).toBe(true);
    expect(afterReboot(s, null)).toMatchObject({ phase: "other", done: true, text: expect.stringMatching(/runs 0\.4\.4 \(9999999\).*SD card/) });
    expect(stateFromStatus({ ok: false, pair: true })).toBeNull();
    expect(stateFromStatus(null)).toBeNull();
  });
});
