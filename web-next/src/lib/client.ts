// web-next/src/lib/client.ts - a typed client for the Sidecar's /ws (docs/web-ws.md): reconnect with backoff, the
// hello's version check, the pairing token sent as the first request, subscriptions kept across reconnects, requests
// with ids and timeouts, and the acks the server's flow control needs. No Vue and no DOM beyond a WebSocket class,
// which can be passed in (tests, a Tauri webview, Node).
import {
  PROTO,
  parseServerMessage,
  type ErrorBody,
  type Hello,
  type ReplyPayloads,
  type RequestPayloads,
  type RequestType,
  type ServerMessage,
} from "./protocol";

export type ClientState = "idle" | "connecting" | "open" | "closed" | "incompatible";

/** A request the server refused (its error code and message). */
export class RequestError extends Error {
  constructor(
    public readonly code: number,
    message: string,
    public readonly pair = false,
  ) {
    super(message);
    this.name = "RequestError";
  }
}
export class TimeoutError extends Error {
  constructor(type: string) {
    super(`${type}: no answer in time`);
    this.name = "TimeoutError";
  }
}
export class NotConnectedError extends Error {
  constructor() {
    super("not connected");
    this.name = "NotConnectedError";
  }
}

/** The part of the browser's WebSocket the client uses. */
export interface WebSocketLike {
  readonly readyState: number;
  onopen: ((ev: unknown) => void) | null;
  onclose: ((ev: { code: number; reason: string }) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  send(data: string): void;
  close(code?: number, reason?: string): void;
}
export type WebSocketFactory = (url: string) => WebSocketLike;

export interface ClientOptions {
  /** ws://host/ws */
  url: string;
  /** The pairing session token (pairing.ts); null or "" stays unpaired: the status topic still works. */
  getToken?: () => Promise<string | null> | string | null;
  /** A fresh token after the server refused the one getToken gave (a reboot, a revoke); asked once per connection. */
  refreshToken?: () => Promise<string | null> | string | null;
  socket?: WebSocketFactory;
  /** ms before a request is given up (default 5000) */
  timeoutMs?: number;
  /** reconnect delays: initial, factor, maximum, jitter (0..1) */
  backoff?: { initialMs?: number; factor?: number; maxMs?: number; jitter?: number };
  /** an app-level ping when nothing arrived for this long; twice as long without an answer: reconnect (default 15000) */
  pingMs?: number;
  /** for tests */
  random?: () => number;
}

type Handler<T> = (value: T) => void;
interface Events {
  state: ClientState;
  hello: Hello;
  paired: boolean;
  error: ErrorBody;
}

interface Pending {
  type: string;
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

interface Subscription {
  ms?: number;
  handlers: Set<Handler<unknown>>;
}

const OPEN = 1;

export class SidecarClient {
  private ws: WebSocketLike | null = null;
  private stateValue: ClientState = "idle";
  private helloValue: Hello | null = null;
  private pairedValue = false;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private subs = new Map<string, Subscription>();
  private listeners: Partial<Record<keyof Events, Set<Handler<never>>>> = {};
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private lastRx = 0;
  private attempt = 0;
  private wanted = false;
  private readonly opt: Required<Omit<ClientOptions, "getToken" | "refreshToken" | "backoff">> & {
    getToken?: ClientOptions["getToken"];
    refreshToken?: ClientOptions["refreshToken"];
    backoff: { initialMs: number; factor: number; maxMs: number; jitter: number };
  };

  constructor(options: ClientOptions) {
    this.opt = {
      url: options.url,
      getToken: options.getToken,
      refreshToken: options.refreshToken,
      socket: options.socket ?? ((url) => new WebSocket(url) as unknown as WebSocketLike),
      timeoutMs: options.timeoutMs ?? 5000,
      pingMs: options.pingMs ?? 15000,
      random: options.random ?? Math.random,
      backoff: { initialMs: 500, factor: 2, maxMs: 15000, jitter: 0.2, ...options.backoff },
    };
  }

  get state(): ClientState {
    return this.stateValue;
  }
  get hello(): Hello | null {
    return this.helloValue;
  }
  get paired(): boolean {
    return this.pairedValue;
  }

  on<K extends keyof Events>(event: K, fn: Handler<Events[K]>): () => void {
    const set = (this.listeners[event] ??= new Set()) as unknown as Set<Handler<Events[K]>>;
    set.add(fn);
    return () => void set.delete(fn);
  }

  /** Connects, and keeps reconnecting until close(). */
  connect(): void {
    this.wanted = true;
    if (this.ws || this.stateValue === "incompatible") return;
    this.open();
  }

  close(): void {
    this.wanted = false;
    this.clearRetry();
    const ws = this.ws;
    this.drop(new NotConnectedError());
    if (ws) ws.close(1000, "bye");
    this.setState("closed");
  }

  /** Subscribes a handler to a topic; the subscription survives reconnects. Returns the unsubscribe. */
  subscribe<T = unknown>(topic: string, handler: Handler<T>, opts: { ms?: number } = {}): () => void {
    let sub = this.subs.get(topic);
    if (!sub) {
      sub = { ms: opts.ms, handlers: new Set() };
      this.subs.set(topic, sub);
      if (this.stateValue === "open") this.request("sub", { topics: [topic], ms: opts.ms }).catch(() => undefined);
    }
    sub.handlers.add(handler as Handler<unknown>);
    return () => {
      const s = this.subs.get(topic);
      if (!s) return;
      s.handlers.delete(handler as Handler<unknown>);
      if (s.handlers.size === 0) {
        this.subs.delete(topic);
        if (this.stateValue === "open") this.request("unsub", { topics: [topic] }).catch(() => undefined);
      }
    };
  }

  /** One request; resolves with the reply's payload, rejects with RequestError / TimeoutError / NotConnectedError. */
  request<K extends RequestType>(type: K, payload?: RequestPayloads[K]): Promise<ReplyPayloads[K]> {
    const ws = this.ws;
    if (!ws || ws.readyState !== OPEN || !this.helloValue) return Promise.reject(new NotConnectedError());
    const id = this.nextId++;
    if (this.nextId > 0x7fffffff) this.nextId = 1;
    return new Promise<ReplyPayloads[K]>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new TimeoutError(type));
      }, this.opt.timeoutMs);
      this.pending.set(id, { type, resolve: resolve as (v: unknown) => void, reject, timer });
      ws.send(JSON.stringify(payload === undefined ? { type, id } : { type, id, payload }));
    });
  }

  /** A console command, as the old page's POST /cmd (needs the pairing). */
  command(c: string): Promise<ReplyPayloads["cmd"]> {
    return this.request("cmd", { c });
  }

  /** Sends the token again (after pairing in the page, say). */
  async authenticate(): Promise<boolean> {
    const first = this.opt.getToken ? await this.opt.getToken() : null;
    if (first && (await this.tryToken(first))) return true;
    // refused (or none): one fresh token, e.g. after the Sidecar rebooted and forgot its sessions
    const fresh = this.opt.refreshToken ? await this.opt.refreshToken() : null;
    if (fresh && fresh !== first) return this.tryToken(fresh);
    return false;
  }

  private async tryToken(tok: string): Promise<boolean> {
    try {
      await this.request("auth", { token: tok });
      this.setPaired(true);
      return true;
    } catch (e) {
      this.setPaired(false);
      if (e instanceof RequestError && e.code === 401) return false;
      throw e;
    }
  }

  // ---- internals
  private open(): void {
    this.setState("connecting");
    let ws: WebSocketLike;
    try {
      ws = this.opt.socket(this.opt.url);
    } catch {
      this.scheduleRetry();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      this.lastRx = Date.now();
    };
    ws.onmessage = (ev) => {
      if (this.ws !== ws) return;
      this.lastRx = Date.now();
      if (typeof ev.data !== "string") return;
      const m = parseServerMessage(ev.data);
      if (m) this.handle(m);
    };
    ws.onerror = () => undefined; // onclose follows
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.drop(new NotConnectedError());
      if (this.stateValue === "incompatible") return;
      if (this.wanted) this.scheduleRetry();
      else this.setState("closed");
    };
  }

  private handle(m: ServerMessage): void {
    switch (m.type) {
      case "hello":
        if (Math.floor(m.payload.proto) !== PROTO) {
          this.helloValue = null;
          this.wanted = false;
          this.setState("incompatible");
          this.ws?.close(1000, "protocol version");
          return;
        }
        this.helloValue = m.payload;
        this.attempt = 0;
        this.setState("open");
        this.emit("hello", m.payload);
        this.startPing();
        void this.afterHello();
        return;
      case "reply": {
        const p = this.pending.get(m.id);
        if (!p) return;
        this.pending.delete(m.id);
        clearTimeout(p.timer);
        if (m.ok) p.resolve(m.payload);
        else p.reject(new RequestError(m.error.code, m.error.msg, !!m.error.pair));
        return;
      }
      case "pub": {
        const sub = this.subs.get(m.topic);
        try {
          if (sub)
            for (const h of sub.handlers) {
              try {
                h(m.payload);
              } catch (e) {
                console.error(`sidecar: a "${m.topic}" handler threw`, e); // one bad handler must not stop the stream
              }
            }
        } finally {
          this.send({ type: "ack", seq: m.seq }); // the server's window (ws_session.h): without it pushes stop
        }
        return;
      }
      case "error":
        this.emit("error", m.error);
        return;
    }
  }

  private async afterHello(): Promise<void> {
    try {
      await this.authenticate();
    } catch {
      /* a timeout: the subscriptions still go out, the status topic needs no pairing */
    }
    const byMs = new Map<number | undefined, string[]>();
    for (const [topic, s] of this.subs) byMs.set(s.ms, [...(byMs.get(s.ms) ?? []), topic]);
    for (const [ms, topics] of byMs) this.request("sub", ms === undefined ? { topics } : { topics, ms }).catch(() => undefined);
  }

  private startPing(): void {
    this.stopPing();
    const every = Math.max(1000, Math.floor(this.opt.pingMs / 3));
    this.pingTimer = setInterval(() => {
      const idle = Date.now() - this.lastRx;
      if (idle > 2 * this.opt.pingMs) {
        this.ws?.close(4000, "no answer"); // onclose reconnects
        const ws = this.ws;
        if (ws) ws.onclose?.({ code: 4000, reason: "no answer" });
      } else if (idle > this.opt.pingMs) this.request("ping").catch(() => undefined);
    }, every);
  }
  private stopPing(): void {
    if (this.pingTimer) clearInterval(this.pingTimer);
    this.pingTimer = null;
  }

  private send(m: object): void {
    if (this.ws && this.ws.readyState === OPEN) this.ws.send(JSON.stringify(m));
  }

  private drop(err: Error): void {
    this.stopPing();
    this.helloValue = null;
    this.setPaired(false);
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pending.clear();
    if (this.ws) {
      const ws = this.ws;
      this.ws = null;
      ws.onclose = null;
      ws.onmessage = null;
    }
  }

  /** The delay before reconnect attempt n (0-based): initial * factor^n, capped, with +-jitter. */
  backoffMs(n: number): number {
    const b = this.opt.backoff;
    const base = Math.min(b.maxMs, b.initialMs * Math.pow(b.factor, n));
    return Math.round(base * (1 - b.jitter + 2 * b.jitter * this.opt.random()));
  }

  private scheduleRetry(): void {
    this.setState("connecting");
    this.clearRetry();
    const ms = this.backoffMs(this.attempt++);
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      if (this.wanted && !this.ws) this.open();
    }, ms);
  }
  private clearRetry(): void {
    if (this.retryTimer) clearTimeout(this.retryTimer);
    this.retryTimer = null;
  }

  private setState(s: ClientState): void {
    if (this.stateValue === s) return;
    this.stateValue = s;
    this.emit("state", s);
  }
  private setPaired(p: boolean): void {
    if (this.pairedValue === p) return;
    this.pairedValue = p;
    this.emit("paired", p);
  }
  private emit<K extends keyof Events>(event: K, value: Events[K]): void {
    const set = this.listeners[event] as unknown as Set<Handler<Events[K]>> | undefined;
    if (set) for (const fn of set) fn(value);
  }
}
