// web-next/src/lib/pairing.ts - the pairing code's HTTP side, as the old page does it (pi/engine/www/index.html,
// docs/pi-engine.md "Pairing code"): the code is entered once (POST /pair, in the body), the browser keeps a client
// id and a secret in localStorage under the old page's keys (so a browser paired there is paired here, on the same
// origin), and trades them for a session token at each load (POST /pair/resume). The token lives in memory only and
// goes to the Sidecar in the WebSocket's first "auth" message: never in a URL, never in a log.

const KEY_CLIENT = "scPairClient";
const KEY_SECRET = "scPairSecret";

const isId = (v: unknown): v is string => typeof v === "string" && /^[0-9a-f]{8}$/i.test(v);
const isSecret = (v: unknown): v is string => typeof v === "string" && /^[0-9a-f]{32}$/i.test(v);

export interface PairStatus {
  gate: "on" | "off" | "fixed";
  paired: boolean;
  wait: number;
  digits: number;
  host: string;
}

export interface Storage {
  getItem(k: string): string | null;
  setItem(k: string, v: string): void;
  removeItem(k: string): void;
}

export class Pairing {
  private token: string | null = null;

  constructor(
    private readonly base = "",
    private readonly store: Storage | null = typeof localStorage !== "undefined" ? localStorage : null,
    private readonly fetchFn: typeof fetch = (...a) => fetch(...a),
  ) {}

  /** The session token from the last resume or pair (memory only). */
  get current(): string | null {
    return this.token;
  }

  saved(): { client: string; secret: string } | null {
    try {
      const client = this.store?.getItem(KEY_CLIENT), secret = this.store?.getItem(KEY_SECRET);
      if (isId(client) && isSecret(secret)) return { client: client.toLowerCase(), secret: secret.toLowerCase() };
    } catch {
      /* storage blocked */
    }
    return null;
  }

  forgetLocal(): void {
    this.token = null;
    try {
      this.store?.removeItem(KEY_CLIENT);
      this.store?.removeItem(KEY_SECRET);
    } catch {
      /* storage blocked */
    }
  }

  async status(): Promise<PairStatus> {
    const r = await this.fetchFn(`${this.base}/pair/status`);
    return (await r.json()) as PairStatus;
  }

  /** A token for this browser from its saved pairing; null if there is none or it is stale (then it is dropped). */
  async resume(): Promise<string | null> {
    const s = this.saved();
    if (!s) return null;
    const r = await this.post("/pair/resume", { client: s.client, secret: s.secret });
    if (r.ok && typeof r.token === "string") return (this.token = r.token);
    if (r.stale) this.forgetLocal();
    return null;
  }

  /** Pairs with the code shown on the Rytm's screen; keeps the client id and secret, returns the token. */
  async pair(code: string, name = "web-next"): Promise<string> {
    const r = await this.post("/pair", { code: code.replace(/\s+/g, ""), name, remember: "1" });
    if (!r.ok || typeof r.token !== "string") throw new Error(typeof r.error === "string" ? r.error : "pairing refused");
    if (isId(r.client) && isSecret(r.secret)) {
      try {
        this.store?.setItem(KEY_CLIENT, r.client);
        this.store?.setItem(KEY_SECRET, r.secret);
      } catch {
        /* storage blocked: paired for this tab only */
      }
    }
    return (this.token = r.token);
  }

  private async post(path: string, form: Record<string, string>): Promise<Record<string, unknown>> {
    const r = await this.fetchFn(`${this.base}${path}`, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams(form).toString(),
    });
    try {
      return (await r.json()) as Record<string, unknown>;
    } catch {
      return { ok: false, error: `HTTP ${r.status}` };
    }
  }
}
