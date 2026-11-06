// web-next/mock/server.mjs - a stand-in Sidecar for developing the app without a Pi: GET /status, the pairing and
// settings endpoints, and the WebSocket /ws with the same protocol and rules as pi/engine/ws_session.h (docs/web-ws.md):
// hello (with the topics' rates), auth, sub/unsub, cmd, ping, acks with a window of 4 shared by all topics, unchanged
// topics not resent, a 20 requests/s limit, the strict same-origin check on the upgrade. The device itself (the status,
// the commands, the log, the sequencer) is mock/device.mjs.
//
//   node mock/server.mjs [port]        (default 8787; the pairing code is printed here, as the Rytm would show it)
import http from "node:http";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";
import { createDevice } from "./device.mjs";

const PROTO = 1, WINDOW = 4, MAX_MS = 10000, MAX_MSG = 4096, BURST = 20, REFILL_MS = 50;
// the Pi's (ws_session.h kRate): fastest, default, longest silence of an unchanged topic
const RATE = { status: [250, 1000, 10000], levels: [33, 40, 1000], seq: [100, 250, 10000], seqpos: [33, 50, 1000], log: [250, 500, 10000], voices: [100, 200, 10000] };
const TOPICS = Object.keys(RATE);
const LATER = ["usbMode", "scenes"];
const NOT_PAIRED = "not paired: pair this browser first (the code is on the Rytm's screen)";
// what GET needs no pairing for (web.cpp's Open)
const OPEN = new Set(["/", "/index.html", "/health", "/status", "/log", "/pair", "/pair/status", "/pair/resume", "/pair/show"]);

// web_cmd.h's list
export function refused(c) {
  if (!c) return "an empty command";
  if (c === "wdtest") return "wdtest is the serial console's only";
  if (/^(midibridge=|usbmidi=|osupdate=)/.test(c)) return "not taken on /cmd (midibridge=, usbmidi=, osupdate=)";
  return null;
}

export function startMock({ port = 8787, host = "127.0.0.1", quiet = false, code, version = "0.4.2-mock", scenario = "" } = {}) {
  const say = (...a) => { if (!quiet) console.log("[mock]", ...a); };
  const started = Date.now();
  const pairCode = code ?? String(crypto.randomInt(0, 1e8)).padStart(8, "0");
  const clients = new Map();          // id -> secret
  const tokens = new Set();
  const dev = createDevice({ scenario });
  let requests = 0;

  // a slow swell with some jitter, so the tide moves
  const levels = () => {
    const t = (Date.now() - started) / 1000, m = 0.35 + 0.3 * Math.sin(t * 1.3) + 0.15 * Math.random();
    const r = (x) => Math.round(Math.max(0, Math.min(1, x)) * 1000) / 1000;
    return { master: [r(m), r(m * 0.95)], strips: Array.from({ length: 12 }, (_, i) => r(m * (0.4 + 0.6 * Math.abs(Math.sin(t + i))))) };
  };
  const json = (res, obj, status = 200) => { res.writeHead(status, { "Content-Type": "application/json" }); res.end(JSON.stringify(obj)); };
  const body = (req) => new Promise((ok) => { let b = ""; req.on("data", (d) => (b += d)); req.on("end", () => ok(new URLSearchParams(b))); });
  const tok = () => crypto.randomBytes(16).toString("hex");
  const okOrErr = (err) => (err ? { ok: false, code: 400, error: err } : { ok: true });

  const server = http.createServer(async (req, res) => {
    requests++;
    const url = new URL(req.url, "http://x");
    const p = url.pathname;
    if (!OPEN.has(p) && !tokens.has(req.headers["x-sidecar-auth"])) {
      res.writeHead(200, { "Content-Type": "application/json", "X-Sidecar-Pair": "required" });
      return res.end(JSON.stringify({ ok: false, code: 401, pair: true, wait: 0, error: NOT_PAIRED }));
    }
    if (p === "/status") return json(res, dev.status({ requests, open: wss.clients.size }));
    if (p === "/health") return json(res, { alive: true, version: "0.4.2", commit: "mock000", dirty: false, uptime: Math.floor((Date.now() - started) / 1000) });
    if (p === "/pair/status")
      return json(res, { gate: "on", paired: tokens.has(req.headers["x-sidecar-auth"]), clients: clients.size, sessions: tokens.size, wait: 0, digits: 8, hdr: !!req.headers["x-sidecar-auth"], rng: true, host: "sidecar-mock" });
    if (p === "/pair" && req.method === "POST") {
      const f = await body(req);
      if (f.get("code") !== pairCode) return json(res, { ok: false, code: 403, wait: 0, error: "wrong code" });
      const id = crypto.randomBytes(4).toString("hex"), secret = crypto.randomBytes(16).toString("hex"), t = tok();
      clients.set(id, secret);
      tokens.add(t);
      dev.pairClients = clients.size;
      say(`a device paired ("${f.get("name") ?? ""}")`);
      return json(res, { ok: true, token: t, client: id, secret });
    }
    if (p === "/pair/resume" && req.method === "POST") {
      const f = await body(req);
      if (clients.get(f.get("client")) !== f.get("secret")) return json(res, { ok: false, code: 403, stale: true, wait: 0, error: "unknown device" });
      const t = tok();
      tokens.add(t);
      return json(res, { ok: true, token: t });
    }
    if (p === "/pair/show" && req.method === "POST") { say(`pairing code on the "Rytm": ${pairCode}`); return json(res, { ok: true, shown: true, wait: 0 }); }
    if (p === "/pair/forget" && req.method === "POST") { tokens.delete(req.headers["x-sidecar-auth"]); return json(res, { ok: true }); }
    if (p === "/pair/revoke" && req.method === "POST") { tokens.clear(); clients.clear(); dev.pairClients = 0; dev.bridge.on = false; return json(res, { ok: true }); }
    if (p === "/time" && req.method === "POST") return json(res, { ok: true });
    // Wi-Fi (pi/engine/wifi.h): the scan's networks; a join "restarts" and comes up after 4 s ("wrong-pass" fails)
    const w = dev.wifi;
    if (p === "/wifi/status")
      return json(res, { mode: "sta", chip: "ok", starting: false, on: w.on, net: w.on && w.ssid && w.join.state === "up" ? "wifi" : "ethernet", up: true, ip: "169.254.20.230", cmdline: false, ssid: w.ssid, open: false, connected: w.on && !!w.ssid && w.join.state !== "trying", scanning: false, scans: 1, scanAge: 3, trialSecs: 90, join: w.join, networks: w.on ? w.networks : [] });
    if (p === "/wifi/scan") return json(res, w.on ? { ok: true } : { ok: false, code: 409, error: "Wi-Fi is off" });
    if (p === "/wifi/power" && req.method === "POST") { const f = await body(req); w.on = f.get("on") === "1"; return json(res, { ok: true }); }
    if (p === "/wifi/forget" && req.method === "POST") { w.ssid = ""; w.join = { state: "none", reason: "", left: 0 }; return json(res, { ok: true }); }
    if (p === "/wifi/join" && req.method === "POST") {
      const f = await body(req), ssid = f.get("ssid") ?? "", psk = f.get("psk") ?? "", open = f.get("open") === "1";
      if (!ssid || ssid.length > 32) return json(res, okOrErr("the network name is 1 to 32 bytes"));
      if (!open && !(psk.length >= 8 && psk.length <= 63) && !/^[0-9a-f]{64}$/i.test(psk)) return json(res, okOrErr("the password is 8 to 63 characters"));
      w.join = { state: "restarting", reason: "", left: 90 };
      setTimeout(() => { w.join = psk === "wrong-pass" ? { state: "failed", reason: "wrong_password", left: 0 } : { state: "up", reason: "", left: 0 }; if (w.join.state === "up") { w.ssid = ssid; w.on = true; } }, 4000);
      return json(res, { ok: true });
    }
    if (p === "/um") return json(res, dev.um());
    if (p === "/pool") { const k = url.searchParams.get("l") ?? "smp", b = +(url.searchParams.get("b") ?? 0) & 7; return json(res, dev.poolJson(k, b, url.searchParams.get("u") === "1")); }
    if (p === "/bundle/projects") return json(res, dev.projects);
    if (p === "/bundle/list") return json(res, url.searchParams.get("src") === "card" ? dev.bundles : []);
    if (p === "/bundle/trash") return json(res, dev.trash);
    if (p === "/bundle/status") return json(res, dev.bundleStatus());
    if (p.startsWith("/bundle/") && req.method === "POST") {
      const f = await body(req);
      if (p === "/bundle/export") { const pr = dev.projects.find((x) => x.key === f.get("key")); if (!pr) return json(res, { ok: false, error: "no such project" }); Object.assign(dev.job, { phase: "COLLECT", done: 0, total: pr.bytes, startedAt: Date.now(), key: pr.rytm.replace(/ /g, "_") }); return json(res, { ok: true }); }
      if (p === "/bundle/cancel") { dev.job.phase = "IDLE"; return json(res, { ok: true }); }
      if (p === "/bundle/delete") { const i = dev.projects.findIndex((x) => x.key === f.get("key") && f.get("confirm") === x.key && !x.current); if (i < 0) return json(res, { ok: false, error: "refused" }); const [pr] = dev.projects.splice(i, 1); dev.trash.unshift({ id: Date.now() % 1000, key: pr.key, name: pr.rytm, why: "deleted", items: pr.files, date: new Date().toISOString() }); return json(res, { ok: true }); }
      if (p === "/bundle/remove") { dev.bundles = dev.bundles.filter((b) => `/Bundles/${b.name}` !== f.get("path")); return json(res, { ok: true }); }
      if (p === "/bundle/trash/restore") { const i = dev.trash.findIndex((t) => String(t.id) === f.get("id")); if (i < 0) return json(res, { ok: false, error: "no such entry" }); const [t] = dev.trash.splice(i, 1); dev.projects.push({ key: t.key, rytm: t.name, files: t.items, bytes: 10_000_000, used: 0, current: 0, imported: 0 }); return json(res, { ok: true }); }
      if (p === "/bundle/trash/remove") { dev.trash = dev.trash.filter((t) => String(t.id) !== f.get("id")); return json(res, { ok: true }); }
    }
    // the card (mock/fs.mjs; fsweb.h's answers)
    const text = (t, status = 200) => { res.writeHead(status, { "Content-Type": "text/plain; charset=utf-8" }); res.end(t); };
    const qp = url.searchParams;
    if (p === "/fs/ls") { const r = dev.fs.ls(qp.get("path") ?? "/"); return typeof r === "string" ? text(r) : json(res, r); }
    if (p === "/fs/get") { const b = dev.fs.get(qp.get("path") ?? "", +(qp.get("off") ?? 0), Math.min(65536, +(qp.get("len") ?? 65536))); if (!b) return text("404 not found\n", 404); res.writeHead(200, { "Content-Type": "application/octet-stream" }); return res.end(b); }
    if (p === "/fs/del" && req.method === "POST") return text(dev.fs.del(qp.get("path") ?? ""));
    if (p === "/fs/mkdir" && req.method === "POST") return text(dev.fs.mkdir(qp.get("path") ?? "", qp.get("p") === "1"));
    if (p === "/fs/mv" && req.method === "POST") return text(dev.fs.mv(qp.get("path") ?? "", qp.get("to") ?? ""));
    if (p === "/fs/put" && req.method === "POST") {
      const chunks = [];
      for await (const c of req) chunks.push(c);
      const raw = Buffer.concat(chunks), ct = req.headers["content-type"] ?? "";
      const bd = /boundary=(.+)$/.exec(ct)?.[1];
      let data = raw;
      if (bd) { const s = raw.indexOf("\r\n\r\n") + 4, e = raw.lastIndexOf(Buffer.from(`\r\n--${bd}`)); data = raw.subarray(s, e); }
      return text(dev.fs.put(qp.get("path") ?? "", +(qp.get("off") ?? 0), +(qp.get("total") ?? 0), data));
    }
    if (p === "/lists") return json(res, { project: "SHARED SET", ...dev.lists, voices: dev.machines.map((m, v) => ({ v, m: m.toUpperCase(), pat: m === "juno" || m === "fm" ? 0 : -1 })) });
    if (p === "/midibridge") return json(res, { ...dev.status().midibridge, token: dev.bridge.on ? "mock-bridge-token" : "", client: "" });
    if (p === "/midibridge/set" && req.method === "POST") {
      const f = await body(req);
      if (f.has("on")) dev.bridge.on = f.get("on") === "1";
      if (f.has("os")) dev.bridge.os = f.get("os") === "1";
      if (f.has("data")) dev.bridge.data = f.get("data") === "1";
      return json(res, { ...dev.status().midibridge, token: dev.bridge.on ? "mock-bridge-token" : "", client: "" });
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
      say(`ws refused: ${why} (Origin ${origin ?? "-"})`);
      socket.end(`HTTP/1.1 ${why}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n`);
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => session(ws));
  });

  function session(ws) {
    say(`ws: a client connected (${wss.clients.size} open)`);
    const send = (m) => ws.readyState === 1 && ws.send(JSON.stringify(m));
    const s = { authed: false, token: "", seq: 0, acked: 0, bad: 0, authFail: 0, bucket: BURST, refillAt: Date.now(), subs: new Map(), logAt: -1 };
    const reply = (id, payload) => send({ type: "reply", id, ok: true, payload });
    const error = (id, code, msg) => send(id ? { type: "reply", id, ok: false, error: { code, msg, ...(code === 401 ? { pair: true } : {}) } } : { type: "error", error: { code, msg } });
    const bad = (id, code, msg) => { error(id, code, msg); if (++s.bad >= 10) ws.close(1008, "too many bad requests"); };
    const rates = Object.fromEntries(TOPICS.map((t) => [t, RATE[t].slice(0, 2)]));
    send({ type: "hello", payload: { proto: PROTO, server: "sidecar", version, topics: TOPICS, later: LATER, caps: ["auth", "sub", "unsub", "cmd", "ping", "ack"], auth: "token", window: WINDOW, maxMsg: MAX_MSG, minMs: RATE.status[0], maxMs: MAX_MS, rates } });

    // a topic's payload now (the log advances this connection's cursor, as web.cpp's WsBuild)
    const build = (topic) => {
      if (topic === "status") return dev.status({ requests, open: wss.clients.size });
      if (topic === "levels") return levels();
      if (topic === "seq") return dev.seq;
      if (topic === "seqpos") return dev.seqpos();
      if (topic === "voices") return dev.voices();
      const since = s.logAt < 0 ? Math.max(0, dev.logTotal - 8192) : s.logAt;
      const l = dev.logSince(since);
      const out = { from: l.from, to: l.to, gap: s.logAt >= 0 && l.from > since, text: l.text };
      s.logAt = l.to;
      return out;
    };

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
          const out = { topics: [], later: [], unknown: [] };
          let first;
          if (!on && !p.topics) s.subs.clear();
          for (const t of p.topics ?? []) {
            if (typeof t !== "string") return bad(id, 400, "topics: not strings");
            if (TOPICS.includes(t)) {
              const [lo, def] = RATE[t];
              const ms = !p.ms ? def : Math.min(MAX_MS, Math.max(lo, p.ms));
              first ??= ms;
              if (on) { s.subs.set(t, { ms, at: 0, sent: 0, last: null }); if (t === "log") s.logAt = -1; } else s.subs.delete(t);
              out.topics.push(t);
            } else (LATER.includes(t) ? out.later : out.unknown).push(t);
          }
          return reply(id, on && first !== undefined ? { ...out, ms: first } : out);
        }
        case "cmd": {
          if (!s.authed || !tokens.has(s.token)) { s.authed = false; return error(id, 401, NOT_PAIRED); }
          if (typeof p.c !== "string" || !p.c || p.c.length > 199) return bad(id, 400, "cmd: no c (or longer than 199 bytes)");
          const why = refused(p.c);
          if (why) return error(id, 403, why);
          dev.command(p.c);
          say(`command :${p.c.startsWith("wifi=join") ? "wifi=join:..." : p.c} (ws)`);
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
        const body = JSON.stringify(build(topic));
        if (sub.last === body && now - sub.sent < RATE[topic][2]) continue;
        if (topic === "log" && sub.last !== null && JSON.parse(body).text === "" && now - sub.sent < RATE.log[2]) continue;
        sub.last = body;
        sub.sent = now;
        ws.send(`{"type":"pub","topic":"${topic}","seq":${++s.seq},"payload":${body}}`);
      }
    }, 10);
    ws.on("close", (code) => { clearInterval(timer); say(`ws: closed (${code})`); });
  }

  const ticker = setInterval(() => dev.tick(), 1000);
  return new Promise((ok) => {
    server.listen(port, host, () => {
      const addr = server.address();
      say(`listening on http://${host}:${addr.port}  pairing code: ${pairCode.slice(0, 4)} ${pairCode.slice(4)}`);
      ok({
        port: addr.port,
        code: pairCode,
        device: dev,
        close: () => new Promise((done) => { clearInterval(ticker); for (const c of wss.clients) c.terminate(); wss.close(); server.close(() => done()); }),
      });
    });
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) startMock({ port: Number(process.argv[2]) || 8787 });
