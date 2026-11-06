// web-next/src/lib/mock.e2e.test.ts - the client and the pairing helper against the mock server (mock/server.mjs)
// over a real socket on 127.0.0.1: the demo path without a Pi.
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import WebSocket from "ws";
import { startMock as startMockJs } from "../../mock/server.mjs";
import { RequestError, SidecarClient, type WebSocketLike } from "./client";
import { Pairing, type Storage } from "./pairing";
import type { Status } from "./protocol";

// (the mock is plain JS: its options typed here)
const startMock = startMockJs as unknown as (o: { port: number; quiet: boolean; code: string }) => Promise<typeof mock>;
let mock: { port: number; code: string; close: () => Promise<void> };
const base = () => `http://127.0.0.1:${mock.port}`;
const socket = (origin: string) => (url: string) => new WebSocket(url, { origin }) as unknown as WebSocketLike;
const memStore = (): Storage => {
  const m = new Map<string, string>();
  return { getItem: (k) => m.get(k) ?? null, setItem: (k, v) => void m.set(k, v), removeItem: (k) => void m.delete(k) };
};
const until = async (f: () => boolean, ms = 3000) => {
  const t0 = Date.now();
  while (!f()) {
    if (Date.now() - t0 > ms) throw new Error("timed out");
    await new Promise((r) => setTimeout(r, 10));
  }
};

beforeAll(async () => {
  mock = await startMock({ port: 0, quiet: true, code: "12345678" });
});
afterAll(async () => mock.close());

describe("against the mock server", () => {
  it("pairs, resumes, gets live status, sends a command", async () => {
    const store = memStore();
    const pairing = new Pairing(base(), store);
    await expect(pairing.pair("0000 0000")).rejects.toThrow();
    const tok = await pairing.pair("1234 5678");
    expect(tok).toMatch(/^[0-9a-f]{32}$/);
    expect(pairing.saved()).not.toBeNull();
    const again = new Pairing(base(), store); // a reload: the saved pairing trades for a new token
    const tok2 = await again.resume();
    expect(tok2).toMatch(/^[0-9a-f]{32}$/);
    expect(tok2).not.toBe(tok);

    const c = new SidecarClient({ url: `ws://127.0.0.1:${mock.port}/ws`, socket: socket(base()), getToken: () => again.current });
    const seen: Status[] = [];
    c.subscribe<Status>("status", (s) => seen.push(s), { ms: 250 });
    c.connect();
    await until(() => c.paired && seen.length >= 1);
    expect(c.hello?.proto).toBe(1);
    expect(seen[0].host).toBe("sidecar-mock");
    await expect(c.command("notes=1")).resolves.toEqual({});
    await until(() => seen.some((s) => s.notes === true));
    const e = await c.command("midibridge=on").catch((x) => x);
    expect(e).toBeInstanceOf(RequestError);
    expect(e.code).toBe(403);
    const sub = await c.request("sub", { topics: ["status", "scenes", "nope"], ms: 1 });
    expect(sub).toEqual({ topics: ["status"], later: ["scenes"], unknown: ["nope"], ms: 250 });
    // the VOICE page follows voicepick=
    const views: { track?: number; mode?: string }[] = [];
    c.subscribe<{ track?: number; mode?: string }>("voices", (v) => views.push(v), { ms: 100 });
    await until(() => views.length >= 1);
    expect(views[0]).toMatchObject({ track: 4, mode: "fm" });
    await c.command("voicepick=2");
    await until(() => views.some((v) => v.track === 2 && v.mode === "daisy"));
    // scenes: a lock, A / B and the fader come back on /status
    await c.command("scene=lock:3:v4:p2:50.5");
    await c.command("scene=ab:3:1");
    await c.command("scene=fader:16383");
    await until(() => seen.some((s) => {
      const sc = (s as Record<string, any>).scene;
      return sc?.a === 3 && sc.b === 1 && sc.fader === 16383 && sc.s.some((x: { n: number; l: [string, number][] }) => x.n === 3 && x.l[0][0] === "V4:P2" && x.l[0][1] === 6464);
    }));
    c.close();
  });

  it("serves status without pairing but refuses commands", async () => {
    const c = new SidecarClient({ url: `ws://127.0.0.1:${mock.port}/ws`, socket: socket(base()), getToken: () => null });
    let got = 0;
    c.subscribe("status", () => got++);
    c.connect();
    await until(() => got > 0);
    const e = await c.command("version").catch((x) => x);
    expect(e).toBeInstanceOf(RequestError);
    expect(e.code).toBe(401);
    expect(e.pair).toBe(true);
    c.close();
  });

  it("refuses an upgrade from another origin", async () => {
    const ws = new WebSocket(`ws://127.0.0.1:${mock.port}/ws`, { origin: "http://evil.example" });
    const status = await new Promise<number>((ok) => {
      ws.on("unexpected-response", (_req, res) => ok(res.statusCode ?? 0));
      ws.on("error", () => ok(-1));
    });
    expect(status).toBe(403);
  });

  it("holds pushes while the client does not ack (the window)", async () => {
    const ws = new WebSocket(`ws://127.0.0.1:${mock.port}/ws`, { origin: base() });
    const pubs: number[] = [];
    ws.on("message", (d) => {
      const m = JSON.parse(d.toString());
      if (m.type === "pub") pubs.push(m.seq);
    });
    await new Promise((ok) => ws.on("open", ok));
    ws.send(JSON.stringify({ type: "sub", id: 1, payload: { topics: ["levels"] } }));
    // the levels change every push (40 ms): ~25 pushes in a second without the window
    await new Promise((r) => setTimeout(r, 1000));
    const held = pubs.length;
    expect(held).toBe(4);
    ws.send(JSON.stringify({ type: "ack", seq: pubs[pubs.length - 1] }));
    await until(() => pubs.length > held, 3000);
    ws.close();
  });
});
