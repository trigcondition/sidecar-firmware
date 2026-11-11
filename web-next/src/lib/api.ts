// web-next/src/lib/api.ts - the Sidecar's HTTP endpoints that have no WebSocket topic (Wi-Fi, pairing, /um, /pool,
// /fs, /bundle, ...), with the old page's rules (pi/engine/www/index.html's fetch wrapper):
// - the pairing token in the X-Sidecar-Auth header (never the URL), written with its case kept (a plain object); with
//   no token the header still goes, as NO_TOKEN: on the USB-C cable the Sidecar wants the header (any value) and skips
//   the pairing, from the first request on (docs/usb-network.md); elsewhere a NO_TOKEN is refused as no header would be
// - at most 2 requests at once (the Pi serves 4 connections; the WebSocket has its own)
// - a 503 ("busy") tried again after 0.3, 0.6 and 1.2 s
// - a reply with X-Sidecar-Pair means the pairing is gone: onPairLost
// Refusals come back as HTTP 200 with a body: {"ok":false,"code":N,"error":...} (JSON endpoints) or "<code> <msg>"
// (/fs/*, /cmd); json() and text() leave them to the caller.

/** the X-Sidecar-Auth value without a pairing token: never a valid token (those are 64 hex digits) */
export const NO_TOKEN = "usb";

export interface ApiOptions {
  base?: string;
  token: () => string | null;
  onPairLost?: () => void;
  fetchFn?: typeof fetch;
  sleep?: (ms: number) => Promise<void>;
  maxInFlight?: number;
}

export class Api {
  private busy = 0;
  private wait: (() => void)[] = [];
  private readonly base: string;
  private readonly fetchFn: typeof fetch;
  private readonly sleep: (ms: number) => Promise<void>;
  private readonly max: number;

  constructor(private readonly opt: ApiOptions) {
    this.base = opt.base ?? "";
    this.fetchFn = opt.fetchFn ?? ((...a) => fetch(...a));
    this.sleep = opt.sleep ?? ((ms) => new Promise((ok) => setTimeout(ok, ms)));
    this.max = opt.maxInFlight ?? 2;
  }

  /** One request through the gate. path is the Sidecar's ("/wifi/status"). */
  async request(path: string, init: RequestInit = {}): Promise<Response> {
    const headers: Record<string, string> = {};
    if (init.headers) for (const [k, v] of Object.entries(init.headers as Record<string, string>)) if (k.toLowerCase() !== "x-sidecar-auth") headers[k] = v;
    headers["X-Sidecar-Auth"] = this.opt.token() || NO_TOKEN;
    for (let i = 0; ; i++) {
      await this.take();
      let r: Response;
      try {
        r = await this.fetchFn(this.base + path, { ...init, headers });
      } finally {
        this.give();
      }
      if (r.status !== 503 || i >= 3 || init.signal?.aborted) {
        if (r.headers.get("X-Sidecar-Pair")) this.opt.onPairLost?.();
        return r;
      }
      await this.sleep(300 << i);
    }
  }

  async json<T = Record<string, unknown>>(path: string, init?: RequestInit): Promise<T> {
    const r = await this.request(path, init);
    if (!r.ok) throw new Error(`HTTP ${r.status} on ${path.split("?")[0]}`);
    return (await r.json()) as T;
  }

  async text(path: string, init?: RequestInit): Promise<string> {
    const r = await this.request(path, init);
    if (!r.ok) throw new Error(`HTTP ${r.status} on ${path.split("?")[0]}`);
    return r.text();
  }

  /** A form POST (the body, never the URL: Wi-Fi keys and codes go here). */
  post<T = Record<string, unknown>>(path: string, form: Record<string, string | number> = {}): Promise<T> {
    const body = new URLSearchParams(Object.entries(form).map(([k, v]) => [k, String(v)])).toString();
    return this.json<T>(path, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body });
  }

  private take(): Promise<void> {
    if (this.busy < this.max) {
      this.busy++;
      return Promise.resolve();
    }
    return new Promise((ok) => this.wait.push(ok));
  }
  private give(): void {
    const next = this.wait.shift();
    if (next) next();
    else this.busy--;
  }
}

/** A path with query parameters, encoded. */
export function q(path: string, params: Record<string, string | number | undefined>): string {
  const p = Object.entries(params).filter(([, v]) => v !== undefined) as [string, string | number][];
  return p.length ? `${path}?${new URLSearchParams(p.map(([k, v]) => [k, String(v)])).toString()}` : path;
}
