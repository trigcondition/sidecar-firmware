// web-next/src/lib/protocol.ts - the /ws messages (docs/web-ws.md, pi/engine/ws_session.h). Shared by the Vue app,
// the mock server's tests and, later, the Tauri app. No DOM, no Vue.

/** The protocol major this client speaks; a server hello with another one is refused. */
export const PROTO = 1;

/** Topics a server of this protocol may serve today, and names reserved for later ones. */
export type Topic = "status" | "usbMode" | "voices" | "levels" | "scenes" | "log";

export interface Hello {
  proto: number;
  server: string;
  version: string;
  topics: string[];
  later: string[];
  caps: string[];
  auth: "token";
  window: number;
  maxMsg: number;
  minMs: number;
  maxMs: number;
}

export interface ErrorBody {
  code: number;
  msg: string;
  /** true on a 401: the pairing is missing or stale */
  pair?: boolean;
}

/** What GET /status answers; only the members the app reads are typed, the rest passes through. */
export interface Status {
  host?: string;
  uptime?: number;
  version?: string;
  [key: string]: unknown;
}

/** The "levels" topic (docs/web-ws.md "Topics"; the mock serves it, the Pi later): peaks 0..1 since the last push. */
export interface Levels {
  /** master L/R peak */
  master: [number, number];
  /** V1..V8, B1..B3, M (12 strips) */
  strips: number[];
}

export interface SubReply {
  topics: string[];
  later: string[];
  unknown: string[];
  ms?: number;
}

// ---- server -> client
export type ServerMessage =
  | { type: "hello"; payload: Hello }
  | { type: "reply"; id: number; ok: true; payload: unknown }
  | { type: "reply"; id: number; ok: false; error: ErrorBody }
  | { type: "pub"; topic: string; seq: number; payload: unknown }
  | { type: "error"; error: ErrorBody };

// ---- client -> server
export interface RequestPayloads {
  auth: { token: string };
  sub: { topics: string[]; ms?: number };
  unsub: { topics?: string[] };
  cmd: { c: string };
  ping: Record<string, never>;
}
export interface ReplyPayloads {
  auth: { paired: boolean };
  sub: SubReply;
  unsub: SubReply;
  cmd: Record<string, never>;
  ping: { t: number };
}
export type RequestType = keyof RequestPayloads;

export type ClientMessage =
  | { [K in RequestType]: { type: K; id: number; payload?: RequestPayloads[K] } }[RequestType]
  | { type: "ack"; seq: number };

/** A JSON text from the server, checked for the envelope's shape (the payloads are trusted to the server). */
export function parseServerMessage(text: string): ServerMessage | null {
  let m: unknown;
  try {
    m = JSON.parse(text);
  } catch {
    return null;
  }
  if (typeof m !== "object" || m === null) return null;
  const o = m as Record<string, unknown>;
  switch (o.type) {
    case "hello":
      return typeof o.payload === "object" && o.payload !== null ? (o as unknown as ServerMessage) : null;
    case "reply":
      return typeof o.id === "number" && typeof o.ok === "boolean" ? (o as unknown as ServerMessage) : null;
    case "pub":
      return typeof o.topic === "string" && typeof o.seq === "number" ? (o as unknown as ServerMessage) : null;
    case "error":
      return typeof o.error === "object" && o.error !== null ? (o as unknown as ServerMessage) : null;
    default:
      return null;
  }
}
