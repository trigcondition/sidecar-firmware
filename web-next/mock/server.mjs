// web-next/mock/server.mjs - a stand-in Sidecar for developing the app without a Pi: GET /status, the pairing
// endpoints (/pair/status, POST /pair, POST /pair/resume) and the WebSocket /ws with the same protocol and rules as
// pi/engine/ws_session.h (docs/web-ws.md): hello, auth, sub/unsub, cmd, ping, acks with a window of 4, unchanged
// status not resent, a 20 requests/s limit, the strict same-origin check on the upgrade.
//
//   node mock/server.mjs [port]        (default 8787; the pairing code is printed here, as the Rytm would show it)
import http from "node:http";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";

const PROTO = 1, WINDOW = 4, MIN_MS = 250, DEFAULT_MS = 1000, MAX_MS = 10000, MAX_MSG = 4096, BURST = 20, REFILL_MS = 50;
// "levels" is the Pi's later topic (docs/web-ws.md): the mock serves it already, so the header's signal tide has data
const TOPICS = ["status", "levels"];
const LATER = ["usbMode", "voices", "scenes", "log"];
// per topic: the fastest and the default push interval
const RATE = { status: [MIN_MS, DEFAULT_MS], levels: [33, 40] };
const NOT_PAIRED = "not paired: pair this browser first (the code is on the Rytm's screen)";

// web_cmd.h's list
export function refused(c) {
  if (!c) return "an empty command";
  if (c === "wdtest") return "wdtest is the serial console's only";
  if (/^(midibridge=|usbmidi=|osupdate=)/.test(c)) return "not taken on /cmd (midibridge=, usbmidi=, osupdate=)";
  return null;
}

export function startMock({ port = 8787, host = "127.0.0.1", quiet = false, code } = {}) {
  const log = (...a) => { if (!quiet) console.log("[mock]", ...a); };
  const started = Date.now();
  const pairCode = code ?? String(crypto.randomInt(0, 1e8)).padStart(8, "0");
  const clients = new Map();          // id -> secret
  const tokens = new Set();
  const dev = { notes: false, commands: 0, usbMode: "class", level: 0 };

  const status = () => ({
    host: "sidecar-mock",
    uptime: Math.floor((Date.now() - started) / 1000),
    version: "0.4.2-mock",
    web: { requests: 0, commands: dev.commands, open: wss.clients.size },
    notes: dev.notes,
    usbMode: { set: dev.usbMode, from: "card", runs: dev.usbMode },
    level: dev.level,
  });

  // a slow swell with some jitter, so the tide moves
  const levels = () => {
    const t = (Date.now() - started) / 1000, m = 0.35 + 0.3 * Math.sin(t * 1.3) + 0.15 * Math.random();
    const r = (x) => Math.round(Math.max(0, Math.min(1, x)) * 1000) / 1000;
    return { master: [r(m), r(m * 0.95)], strips: Array.from({ length: 12 }, (_, i) => r(m * (0.4 + 0.6 * Math.abs(Math.sin(t + i))))) };
  };
  const json = (res, obj) => { res.writeHead(200, { "Content-Type": "application/json" }); res.end(JSON.stringify(obj)); };
  const body = (req) => new Promise((ok) => { let b = ""; req.on("data", (d) => (b += d)); req.on("end", () => ok(new URLSearchParams(b))); });
  const tok = () => crypto.randomBytes(16).toString("hex");

  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, "http://x");
    if (url.pathname === "/status") return json(res, status());
    if (url.pathname === "/pair/status")
      return json(res, { gate: "on", paired: tokens.has(req.headers["x-sidecar-auth"]), clients: clients.size, sessions: tokens.size, wait: 0, digits: 8, hdr: !!req.headers["x-sidecar-auth"], rng: true, host: "sidecar-mock" });
    if (url.pathname === "/pair" && req.method === "POST") {
      const f = await body(req);
      if (f.get("code") !== pairCode) return json(res, { ok: false, code: 403, wait: 0, error: "wrong code" });
      const id = crypto.randomBytes(4).toString("hex"), secret = crypto.randomBytes(16).toString("hex"), t = tok();
      clients.set(id, secret);
      tokens.add(t);
      log(`a device paired ("${f.get("name") ?? ""}")`);
      return json(res, { ok: true, token: t, client: id, secret });
    }
    if (url.pathname === "/pair/resume" && req.method === "POST") {
      const f = await body(req);
      if (clients.get(f.get("client")) !== f.get("secret")) return json(res, { ok: false, code: 403, stale: true, wait: 0, error: "unknown device" });
      const t = tok();
      tokens.add(t);
      return json(res, { ok: true, token: t });
    }
    res.writeHead(404, { "Content-Type": "text/plain" });
    res.end("404 mock\n");
  });

  const wss = new WebSocketServer({ noServer: true, maxPayload: MAX_MSG });
  server.on("upgrade", (req, socket, head) => {
    const url = new URL(req.url, "http://x");
    const origin = req.headers.origin, hostHdr = req.headers.host;
    let why = null;
    if (url.pathname !== "/ws") why = "404 Not Found";
    else if (!origin || origin.replace(/^https?:\/\//, "") !== hostHdr) why = "403 Forbidden";   // the strict same-origin check
    else if (wss.clients.size >= 2) why = "503 Service Unavailable";                              // MaxWs
    if (why) {
      log(`ws refused: ${why} (Origin ${origin ?? "-"})`);
      socket.end(`HTTP/1.1 ${why}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n`);
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => session(ws));
  });

  function session(ws) {
    log(`ws: a client connected (${wss.clients.size} open)`);
    const send = (m) => ws.readyState === 1 && ws.send(JSON.stringify(m));
    const s = { authed: false, token: "", seq: 0, acked: 0, bad: 0, authFail: 0, bucket: BURST, refillAt: Date.now(), subs: new Map() };
    const reply = (id, payload) => send({ type: "reply", id, ok: true, payload });
    const error = (id, code, msg) => send(id ? { type: "reply", id, ok: false, error: { code, msg, ...(code === 401 ? { pair: true } : {}) } } : { type: "error", error: { code, msg } });
    const bad = (id, code, msg) => { error(id, code, msg); if (++s.bad >= 10) ws.close(1008, "too many bad requests"); };
    send({ type: "hello", payload: { proto: PROTO, server: "sidecar", version: "0.4.2-mock", topics: TOPICS, later: LATER, caps: ["auth", "sub", "unsub", "cmd", "ping", "ack"], auth: "token", window: WINDOW, maxMsg: MAX_MSG, minMs: MIN_MS, maxMs: MAX_MS } });

    ws.on("message", (data, isBinary) => {
      if (isBinary) return ws.close(1003, "text only");
      let m;
      try { m = JSON.parse(data.toString()); } catch { return bad(0, 400, "not a JSON object"); }
      if (typeof m !== "object" || m === null || Array.isArray(m)) return bad(0, 400, "not a JSON object");
      if (typeof m.type !== "string") return bad(0, 400, "no type");
      if (m.type === "ack") {
        if (!Number.isInteger(m.seq) || m.seq < 0) return bad(0, 400, "ack: no seq");
        if (m.seq > s.seq) return bad(0, 400, "ack: a seq not sent yet");
        s.acked = Math.max(s.acked, m.seq);
        return;
      }
      const id = m.id;
      if (!Number.isInteger(id) || id <= 0) return bad(0, 400, "a request needs an id");
      const now = Date.now(), n = Math.floor((now - s.refillAt) / REFILL_MS);
      if (n > 0) { s.refillAt += n * REFILL_MS; s.bucket = Math.min(BURST, s.bucket + n); }
      if (s.bucket === 0) return error(id, 429, "too many requests");
      s.bucket--;
      const p = m.payload ?? {};
      switch (m.type) {
        case "ping": return reply(id, { t: now - started });
        case "auth":
          if (typeof p.token === "string" && tokens.has(p.token)) { s.authed = true; s.token = p.token; return reply(id, { paired: true }); }
          s.authed = false; s.token = "";
          error(id, 401, NOT_PAIRED);
          if (++s.authFail >= 5) ws.close(1008, "too many bad requests");
          return;
        case "sub": case "unsub": {
          const on = m.type === "sub";
          if (p.topics !== undefined && !Array.isArray(p.topics)) return bad(id, 400, "topics: not an array");
          if (on && !p.topics) return bad(id, 400, "sub: no topics");
          if (on && p.ms !== undefined && (!Number.isInteger(p.ms) || p.ms < 0)) return bad(id, 400, "ms: not an unsigned integer");
          let ms = DEFAULT_MS;
          const out = { topics: [], later: [], unknown: [] };
          if (!on && !p.topics) s.subs.clear();
          for (const t of p.topics ?? []) {
            if (typeof t !== "string") return bad(id, 400, "topics: not strings");
            if (TOPICS.includes(t)) {
              const [lo, def] = RATE[t];
              ms = p.ms === undefined ? def : Math.min(MAX_MS, Math.max(lo, p.ms));
              if (on) s.subs.set(t, { ms, at: 0, sent: 0, last: null }); else s.subs.delete(t);
              out.topics.push(t);
            }
            else (LATER.includes(t) ? out.later : out.unknown).push(t);
          }
          return reply(id, on ? { ...out, ms } : out);
        }
        case "cmd": {
          if (!s.authed || !tokens.has(s.token)) { s.authed = false; return error(id, 401, NOT_PAIRED); }
          if (typeof p.c !== "string" || !p.c || p.c.length > 199) return bad(id, 400, "cmd: no c (or longer than 199 bytes)");
          const why = refused(p.c);
          if (why) return error(id, 403, why);
          dev.commands++;
          log(`command :${p.c.startsWith("wifi=join") ? "wifi=join:..." : p.c} (ws)`);
          if (p.c === "notes=1" || p.c === "notes=0") dev.notes = p.c.endsWith("1");
          if (p.c.startsWith("usbmode=")) dev.usbMode = p.c.slice(8);
          return reply(id, {});
        }
        default: return bad(id, 404, "unknown type");
      }
    });

    const timer = setInterval(() => {
      const now = Date.now();
      for (const [topic, sub] of s.subs) {
        if (sub.at && now - sub.at < sub.ms) continue;
        if (s.seq - s.acked >= WINDOW) return;          // coalesced: the newest goes out after the ack
        sub.at = now;
        const body = JSON.stringify(topic === "levels" ? levels() : status());
        if (sub.last === body && now - sub.sent < MAX_MS) continue;
        sub.last = body;
        sub.sent = now;
        ws.send(`{"type":"pub","topic":"${topic}","seq":${++s.seq},"payload":${body}}`);
      }
    }, 10);
    ws.on("close", (code) => { clearInterval(timer); log(`ws: closed (${code})`); });
  }

  const wobble = setInterval(() => { dev.level = Math.round(Math.random() * 100); }, 700);
  return new Promise((ok) => {
    server.listen(port, host, () => {
      const addr = server.address();
      log(`listening on http://${host}:${addr.port}  pairing code: ${pairCode.slice(0, 4)} ${pairCode.slice(4)}`);
      ok({
        port: addr.port,
        code: pairCode,
        close: () => new Promise((done) => { clearInterval(wobble); for (const c of wss.clients) c.terminate(); wss.close(); server.close(() => done()); }),
      });
    });
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) startMock({ port: Number(process.argv[2]) || 8787 });
