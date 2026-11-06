// web-next/src/lib/client.test.ts - the WebSocket client against a scripted fake socket (no network).
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { NotConnectedError, RequestError, SidecarClient, TimeoutError, type WebSocketLike } from "./client";
import { PROTO, type Hello } from "./protocol";

class FakeSocket implements WebSocketLike {
  static all: FakeSocket[] = [];
  readyState = 0;
  sent: Record<string, unknown>[] = [];
  closed: { code?: number; reason?: string } | null = null;
  onopen: ((ev: unknown) => void) | null = null;
  onclose: ((ev: { code: number; reason: string }) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  constructor(public url: string) {
    FakeSocket.all.push(this);
  }
  send(data: string) {
    this.sent.push(JSON.parse(data));
  }
  close(code?: number, reason?: string) {
    this.closed = { code, reason };
    this.readyState = 3;
  }
  // the server's side
  accept(hello: Partial<Hello> = {}) {
    this.readyState = 1;
    this.onopen?.({});
    this.push({ type: "hello", payload: { ...HELLO, ...hello } });
  }
  push(m: unknown) {
    this.onmessage?.({ data: JSON.stringify(m) });
  }
  drop(code = 1006) {
    this.readyState = 3;
    this.onclose?.({ code, reason: "" });
  }
  last(type?: string) {
    const l = type ? this.sent.filter((m) => m.type === type) : this.sent;
    return l[l.length - 1];
  }
  reply(id: unknown, payload: unknown = {}) {
    this.push({ type: "reply", id, ok: true, payload });
  }
}

const HELLO: Hello = {
  proto: PROTO, server: "sidecar", version: "0.4.2", topics: ["status"], later: ["voices"],
  caps: ["auth", "sub", "unsub", "cmd", "ping", "ack"], auth: "token", window: 4, maxMsg: 4096, minMs: 250, maxMs: 10000,
};

const flush = () => new Promise<void>((r) => queueMicrotask(r)).then(() => new Promise<void>((r) => queueMicrotask(r)));

function make(opts: { token?: string | null; timeoutMs?: number } = {}) {
  return new SidecarClient({
    url: "ws://sidecar.local/ws",
    socket: (u) => new FakeSocket(u),
    getToken: () => (opts.token === undefined ? "tok" : opts.token),
    timeoutMs: opts.timeoutMs ?? 1000,
    random: () => 0.5, // no jitter
  });
}

beforeEach(() => {
  FakeSocket.all = [];
  vi.useFakeTimers();
});
afterEach(() => vi.useRealTimers());

describe("SidecarClient", () => {
  it("connects, checks the hello, authenticates, then subscribes", async () => {
    const c = make();
    const states: string[] = [];
    c.on("state", (s) => states.push(s));
    const got: unknown[] = [];
    c.subscribe("status", (s) => got.push(s), { ms: 500 });
    c.connect();
    const ws = FakeSocket.all[0];
    expect(ws.url).toBe("ws://sidecar.local/ws");
    ws.accept();
    expect(c.state).toBe("open");
    await flush();
    const auth = ws.last("auth")!;
    expect(auth).toMatchObject({ type: "auth", payload: { token: "tok" } });
    ws.reply(auth.id, { paired: true });
    await flush();
    expect(c.paired).toBe(true);
    const sub = ws.last("sub")!;
    expect(sub).toMatchObject({ payload: { topics: ["status"], ms: 500 } });
    ws.push({ type: "pub", topic: "status", seq: 1, payload: { uptime: 5 } });
    expect(got).toEqual([{ uptime: 5 }]);
    expect(ws.last("ack")).toEqual({ type: "ack", seq: 1 }); // the server's window needs it
    expect(states).toEqual(["connecting", "open"]);
  });

  it("never puts the token in the URL", () => {
    const c = make({ token: "secret-token" });
    c.connect();
    expect(FakeSocket.all[0].url).not.toContain("secret-token");
  });

  it("stays unpaired without a token and still subscribes", async () => {
    const c = make({ token: null });
    c.subscribe("status", () => undefined);
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    await flush();
    expect(ws.last("auth")).toBeUndefined();
    expect(ws.last("sub")).toMatchObject({ payload: { topics: ["status"] } });
    expect(c.paired).toBe(false);
  });

  it("refuses a server of another protocol major and does not retry", () => {
    const c = make();
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept({ proto: 2 });
    expect(c.state).toBe("incompatible");
    expect(ws.closed).not.toBeNull();
    vi.advanceTimersByTime(60000);
    expect(FakeSocket.all.length).toBe(1);
  });

  it("matches replies by id and maps errors", async () => {
    const c = make({ token: null });
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    const a = c.command("version");
    const b = c.command("midibridge=on");
    const [ra, rb] = ws.sent.filter((m) => m.type === "cmd");
    expect(ra).toMatchObject({ payload: { c: "version" } });
    ws.push({ type: "reply", id: rb.id, ok: false, error: { code: 401, msg: "not paired", pair: true } });
    ws.reply(ra.id);
    await expect(a).resolves.toEqual({});
    const e = await b.catch((x) => x);
    expect(e).toBeInstanceOf(RequestError);
    expect(e.code).toBe(401);
    expect(e.pair).toBe(true);
  });

  it("times out a request with no answer", async () => {
    const c = make({ token: null, timeoutMs: 300 });
    c.connect();
    FakeSocket.all[0].accept();
    const p = c.request("ping");
    vi.advanceTimersByTime(301);
    await expect(p).rejects.toBeInstanceOf(TimeoutError);
  });

  it("rejects requests when not connected, and pending ones on a drop", async () => {
    const c = make({ token: null });
    await expect(c.request("ping")).rejects.toBeInstanceOf(NotConnectedError);
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    const p = c.request("ping");
    ws.drop();
    await expect(p).rejects.toBeInstanceOf(NotConnectedError);
  });

  it("reconnects with exponential backoff and resubscribes", async () => {
    const c = make({ token: null });
    c.subscribe("status", () => undefined);
    c.connect();
    FakeSocket.all[0].drop(); // never opened
    expect(c.state).toBe("connecting");
    vi.advanceTimersByTime(499);
    expect(FakeSocket.all.length).toBe(1);
    vi.advanceTimersByTime(1);
    expect(FakeSocket.all.length).toBe(2);
    FakeSocket.all[1].drop();
    vi.advanceTimersByTime(999);
    expect(FakeSocket.all.length).toBe(2);
    vi.advanceTimersByTime(1);
    expect(FakeSocket.all.length).toBe(3);
    const ws = FakeSocket.all[2];
    ws.accept();
    await flush();
    expect(ws.last("sub")).toMatchObject({ payload: { topics: ["status"] } });
    ws.drop(); // a hello resets the backoff
    vi.advanceTimersByTime(500);
    expect(FakeSocket.all.length).toBe(4);
  });

  it("caps the backoff and applies jitter", () => {
    const c = new SidecarClient({ url: "ws://x/ws", socket: (u) => new FakeSocket(u), random: () => 1 });
    expect(c.backoffMs(0)).toBe(600); // 500 * 1.2
    expect(c.backoffMs(20)).toBe(18000); // 15000 * 1.2
    const d = new SidecarClient({ url: "ws://x/ws", socket: (u) => new FakeSocket(u), random: () => 0 });
    expect(d.backoffMs(1)).toBe(800); // 1000 * 0.8
  });

  it("unsubscribes when the last handler goes", async () => {
    const c = make({ token: null });
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    await flush();
    const off1 = c.subscribe("status", () => undefined);
    const off2 = c.subscribe("status", () => undefined);
    expect(ws.sent.filter((m) => m.type === "sub").length).toBe(1);
    off1();
    expect(ws.last("unsub")).toBeUndefined();
    off2();
    expect(ws.last("unsub")).toMatchObject({ payload: { topics: ["status"] } });
  });

  it("pings when idle and reconnects when the server stays silent", async () => {
    const c = new SidecarClient({ url: "ws://x/ws", socket: (u) => new FakeSocket(u), pingMs: 3000, random: () => 0.5 });
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    await flush();
    vi.advanceTimersByTime(4000);
    expect(ws.last("ping")).toBeDefined();
    vi.advanceTimersByTime(3000);
    expect(ws.closed).not.toBeNull();
    expect(c.state).toBe("connecting");
    vi.advanceTimersByTime(600);
    expect(FakeSocket.all.length).toBe(2);
  });

  it("close() stops for good", () => {
    const c = make();
    c.connect();
    FakeSocket.all[0].accept();
    c.close();
    expect(c.state).toBe("closed");
    vi.advanceTimersByTime(60000);
    expect(FakeSocket.all.length).toBe(1);
  });

  it("ignores malformed server messages", () => {
    const c = make({ token: null });
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    ws.onmessage?.({ data: "not json" });
    ws.onmessage?.({ data: JSON.stringify({ type: "pub" }) });
    expect(c.state).toBe("open");
  });
});

describe("unauthenticated 401", () => {
  it("reports paired=false when the token is refused", async () => {
    const c = make({ token: "old" });
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    await flush();
    const auth = ws.last("auth")!;
    ws.push({ type: "reply", id: auth.id, ok: false, error: { code: 401, msg: "not paired", pair: true } });
    await flush();
    expect(c.paired).toBe(false);
  });
});

describe("robustness", () => {
  it("acks even when a handler throws, and other handlers still run", () => {
    const c = make({ token: null });
    const seen: unknown[] = [];
    const err = vi.spyOn(console, "error").mockImplementation(() => undefined);
    c.subscribe("status", () => {
      throw new Error("boom");
    });
    c.subscribe("status", (s) => seen.push(s));
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    ws.push({ type: "pub", topic: "status", seq: 1, payload: 1 });
    expect(ws.last("ack")).toEqual({ type: "ack", seq: 1 });
    expect(seen).toEqual([1]);
    expect(err).toHaveBeenCalled();
    err.mockRestore();
  });

  it("asks for a fresh token once when the first is refused", async () => {
    let refreshed = 0;
    const c = new SidecarClient({
      url: "ws://x/ws",
      socket: (u) => new FakeSocket(u),
      getToken: () => "old",
      refreshToken: () => (refreshed++, "new"),
    });
    c.connect();
    const ws = FakeSocket.all[0];
    ws.accept();
    await flush();
    const a1 = ws.last("auth")!;
    expect(a1).toMatchObject({ payload: { token: "old" } });
    ws.push({ type: "reply", id: a1.id, ok: false, error: { code: 401, msg: "no", pair: true } });
    await flush();
    await flush();
    const a2 = ws.last("auth")!;
    expect(a2).toMatchObject({ payload: { token: "new" } });
    ws.reply(a2.id, { paired: true });
    await flush();
    expect(c.paired).toBe(true);
    expect(refreshed).toBe(1);
  });
});
