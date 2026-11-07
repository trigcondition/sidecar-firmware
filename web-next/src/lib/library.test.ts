// web-next/src/lib/library.test.ts - the pool's helpers and the upload protocol (against a fake /fs/put).
import { describe, expect, it } from "vitest";
import { Api } from "./api";
import { answerText, poolAnswer, poolRow, rowName, rowText, unloadLines } from "./pool";
import { CHUNK, crc32, uploadFile } from "./upload";

describe("pool", () => {
  it("reads rows and codes", () => {
    const r = poolRow([5, "sample", "kick.wav", "EXACT", "Drums/kick.wav", 3, 1, 88000, 0, 1, 0]);
    expect(r).toMatchObject({ row: 5, name: "kick.wav", state: "EXACT", users: 1, ch: 1, ram: false });
    expect(rowName(2, 5)).toBe("C005");
    expect(rowText((2 << 7) | 42)).toBe("C042");
  });
  it("finds the log line that answers", () => {
    expect(poolAnswer("12.3 pool: smp C005 kick.wav: SET", "smp")).toEqual({ row: "C005", res: "SET", n: -1, text: "C005 kick.wav: SET" });
    expect(poolAnswer("pool: smp: 3 unloaded", "smp")?.n).toBe(3);
    expect(poolAnswer("pool: wt C001 x: SET", "smp")).toBeNull();
    expect(poolAnswer("pool: smp C009 x.wav: PLAYING: STOP FIRST", "smp")?.res).toBe("PLAYING: STOP FIRST");
    expect(answerText("replace", "C005", poolAnswer("pool: smp C005 k.wav: SET", "smp"), "k.wav")).toEqual(["C005 = k.wav (Undo takes it back)", false]);
    expect(answerText("unload", "C005", null)[1]).toBe(true);
    expect(answerText("accept", "A002", poolAnswer("pool: smp A002 snare.wav: SET", "smp"))).toEqual(["A002 snare.wav: SET", false]);
  });
  it("cuts unload lines at 190 characters", () => {
    const rows = Array.from({ length: 60 }, (_, i) => rowName(0, i + 1));
    const lines = unloadLines("smp", rows, new Set(["A001"]));
    expect(lines.length).toBeGreaterThan(1);
    for (const l of lines) expect(l.length).toBeLessThanOrEqual(190);
    expect(lines[0]).toMatch(/^pool=smp:unload:A001:yes,A002,/);
    expect(lines.join(",").match(/A\d{3}/g)).toHaveLength(60);
  });
});

describe("upload", () => {
  it("crc32 is zlib's", () => {
    expect(crc32(new TextEncoder().encode("123456789"))).toBe(0xcbf43926);
  });

  it("sends 48 KB pieces, resumes on 409, finishes on done", async () => {
    const data = new Uint8Array(CHUNK * 2 + 100).map((_, i) => i & 255);
    let have = 0, first409 = true;
    const seen: string[] = [];
    const fetchFn = (async (url: string, init: RequestInit) => {
      const u = new URL(url, "http://x");
      seen.push(`${u.pathname} ${u.searchParams.get("off") ?? ""}`);
      if (u.pathname === "/fs/del") return new Response("200 deleted");
      const off = +u.searchParams.get("off")!;
      expect(+u.searchParams.get("crc")!).toBe(crc32(data));
      if (off === CHUNK && first409) {
        first409 = false;
        return new Response(`409 expected ${have}`); // the Pi lost a piece: resume where it is
      }
      const part = (init.body as FormData).get("f") as Blob;
      have = off + part.size;
      return new Response(have >= data.length ? `200 done ${have}` : `200 ok ${have}`);
    }) as unknown as typeof fetch;
    const api = new Api({ token: () => "t", fetchFn });
    const blob = new Blob([data]);
    const res = await uploadFile(api, { size: blob.size, name: "x.wav", slice: (a, b) => blob.slice(a, b) }, "/Samples/x.wav");
    expect(res).toBe(`done, ${data.length} bytes`);
    expect(seen).toEqual(["/fs/put 0", `/fs/put ${CHUNK}`, `/fs/put ${CHUNK}`, `/fs/put ${CHUNK * 2}`]);
  });

  it("removes the .part on a failure, but not for a full card", async () => {
    const run = async (answer: string) => {
      const dels: string[] = [];
      const fetchFn = (async (url: string) => {
        const u = new URL(url, "http://x");
        if (u.pathname === "/fs/del") return dels.push(u.searchParams.get("path")!), new Response("200");
        return new Response(answer);
      }) as unknown as typeof fetch;
      const blob = new Blob([new Uint8Array(10)]);
      await expect(uploadFile(new Api({ token: () => null, fetchFn }), { size: 10, name: "a", slice: (a, b) => blob.slice(a, b) }, "/Samples/a")).rejects.toThrow();
      return dels;
    };
    expect(await run("400 bad path")).toEqual(["/Samples/a.part"]);
    expect(await run("507 card full")).toEqual([]);
  });
});
