// pi/engine/tests/page_pair.test.mjs - the pairing code's endpoint flows (pi/engine/paircode.h, web.cpp's gate)
// against mock_web.py: an unpaired client refused on every gated endpoint while /status, /log and the page itself stay
// open; a wrong code refused and then rate-limited (per address); a malformed one never counted; the right one
// accepted; the token in the header and in a body tok= (never the query); the code on the Rytm only when the page
// asks (POST /pair/show, rate-limited), not on a page load; a remembered device surviving a reboot of the mock; revocation; cmdline.txt paircode=0 both ways; and that the
// code is in no answer and no log line. The reference for the rules themselves is make -C pi/engine/tests
// (test_paircode); this checks that the endpoints are wired the way the page expects.
// node --test pi/engine/tests/page_pair.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, readdirSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const MOCK = join(HERE, "mock_web.py");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const WEB_CPP = readFileSync(join(HERE, "..", "web.cpp"), "utf8");
const BUNDLE_H = readFileSync(join(HERE, "..", "bundleweb.h"), "utf8");
const MOCK_PY = readFileSync(MOCK, "utf8");

// The open set, read out of web.cpp's Open() rather than copied here: the previous version of this test listed the
// GATED set by hand, as a verbatim copy of the C++ array, so it proved the list matched itself and could not notice
// that /fs/get - which served the Wi-Fi key and the pairing code - was in neither list.
function listIn(text, marker) {
  const a = text.indexOf(marker);
  assert.ok(a > 0, `${marker} in the source`);
  const b = text.indexOf("}", a);
  return [...text.slice(a, b).matchAll(/"([^"]+)"/g)].map((m) => m[1]);
}
const OPEN = listIn(WEB_CPP, "kOpen[] = {");

// Every concrete path the engine answers, dug out of the sources: the top-level ones web.cpp compares, the sub-ops of
// /wifi/ and /fs/, and the /bundle, /usb and /time endpoints bundleweb.h documents. Anything here that is not in OPEN
// must refuse a client with no pairing code - that is the check the old test could not make.
function routes() {
  const body = WEB_CPP.slice(WEB_CPP.indexOf("THTTPStatus CWebServer::GetContent"));
  const out = new Set();
  for (const m of body.matchAll(/strn?cmp \(pPath, "([^"]+)"/g)) out.add(m[1]);
  const sub = (branch, prefix) => {
    const a = body.indexOf(branch);
    assert.ok(a > 0, branch);
    const b = body.indexOf("\n\telse if", a + 1);
    for (const m of body.slice(a, b).matchAll(/strcmp \(op, "([^"]+)"\)/g)) out.add(prefix + m[1]);
  };
  sub('strncmp (pPath, "/wifi/", 6)', "/wifi/");
  sub('strncmp (pPath, "/fs/", 4)', "/fs/");
  sub('strncmp (pPath, "/pair", 5)', "/pair/");
  for (const l of BUNDLE_H.split("\n")) {                       // its header comment lists every /bundle, /usb, /time
    if (!/^\/\/\s+(GET|POST)\s/.test(l)) continue;
    for (const m of l.matchAll(/(\/[A-Za-z0-9/._-]+)/g))          // its own namespaces, not the example paths
      if (/^\/(bundle|usb)\//.test(m[1]) || m[1] === "/time" || m[1] === "/bundle-sw.js") out.add(m[1]);
  }
  // the prefixes themselves are not endpoints, and /bundle-dl/* never reaches the engine (the service worker)
  for (const p of ["/wifi/", "/fs/", "/pair", "/bundle", "/usb/"]) out.delete(p);
  return [...out].filter((p) => p.startsWith("/") && !p.includes("-dl/")).sort();
}
const ROUTES = routes();
const MUST_REFUSE = ROUTES.filter((p) => !OPEN.includes(p));

// what each one needs to be asked properly (a body where it wants one); everything else is a bare GET
const BODY = {
  "/cmd": { c: "notes=1" }, "/wifi/power": { on: "1" }, "/wifi/forget": {},
  "/wifi/join": { ssid: "Fake-Studio", psk: "not-a-real-pass" },
  "/fs/put": {}, "/fs/del": {}, "/fs/mkdir": {}, "/fs/mv": {}, "/usb/eject": {}, "/time": { epoch: "1790000000" },
  "/bundle/export": { key: "DEMO PERC" }, "/bundle/import": {}, "/bundle/verify": { path: "/x.zip" },
  "/bundle/delete": { key: "A_B", confirm: "A_B" }, "/bundle/cancel": {}, "/bundle/finish": {},
  "/pair/forget": {}, "/pair/revoke": {}, "/midibridge/set": { on: "1" },
};
const QUERY = {
  "/fs/ls": "?path=/", "/fs/get": "?path=/.rytm/wifi.txt", "/fs/del": "?path=/Samples/Kit/kick.wav",
  "/fs/mkdir": "?path=/Bundles", "/fs/mv": "?path=/Samples/Kit/tom.wav&to=/Samples/Kit/tom2.wav", "/fs/put": "?path=/x.bin&off=0&total=1&crc=0",
  "/bundle/zip": "?off=0&len=64", "/bundle/list": "?src=card",
};

let nextPort = 19800 + Math.floor(Math.random() * 300);

async function start(t, card, env = {}) {
  const port = nextPort++;
  const screen = join(card, "..", `screen-${port}.txt`);     // the Rytm's / the touchscreen's stand-in, off the card
  const srv = spawn("python3", [MOCK, String(port), card],
    { stdio: "ignore", env: { ...process.env, MOCK_TRIAL: "1", MOCK_PAIR_OUT: screen, ...env } });
  const base = `http://127.0.0.1:${port}`;
  const pi = {
    base,
    srv,
    screen,
    stop: () => new Promise((ok) => { srv.once("exit", ok); srv.kill(); }),
    // the page's own calls: the token in the X-Sidecar-Auth header
    async post(path, form, { tok, origin = base, headers = {} } = {}) {
      const h = { "Content-Type": "application/x-www-form-urlencoded", ...headers };
      if (origin) h.Origin = origin;
      if (tok) h["X-Sidecar-Auth"] = tok;
      const r = await fetch(base + path, { method: "POST", headers: h,
        body: form == null ? "" : new URLSearchParams(form).toString() });
      return { r, text: await r.text() };
    },
    // a browser sends a Referer on its own same-origin GETs, and /fs/* reads are strict about it (DNS rebinding)
    async get(path, tok, headers = { Referer: base + "/" }) {
      const h = { ...headers };
      if (tok) h["X-Sidecar-Auth"] = tok;
      const r = await fetch(base + path, { cache: "no-store", headers: h });
      return { r, text: await r.text() };
    },
    async json(path, form, opt) {
      const { text } = form === undefined ? await this.get(path, opt && opt.tok, opt && opt.headers)
                                          : await this.post(path, form, opt);
      try { return JSON.parse(text); } catch (e) { throw new Error(`${path} did not answer JSON: ${text.slice(0, 200)}`); }
    },
  };
  t.after(() => srv.kill());
  for (let i = 0; i < 60; i++) {
    try { await pi.json("/pair/status"); return pi; } catch { await sleep(100); }
  }
  throw new Error("mock_web did not answer");
}

// The code as the owner reads it off the Rytm's screen or the touchscreen. The device writes it to no file at all,
// so the mock is told where to put it for the test (MOCK_PAIR_OUT), outside the card - and the card is then checked
// for it below.
function codeOnScreen(screen) {
  const m = readFileSync(screen, "utf8").match(/(\d{4}) ?(\d{4})/);
  assert.ok(m, "the mock put the code on its stand-in screen");
  return m[1] + m[2];
}
// The only file on the card that may hold the code is the recourse file a unit with no display writes
// (paircode.h kRecoursePath) - and /fs/* cannot reach the card's root at all, so it is not readable over the network.
function cardHasNoCode(card, code) {
  const walk = (d) => readdirSync(d, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? walk(join(d, e.name)) : [join(d, e.name)]);
  for (const f of walk(card)) {
    if (f.endsWith("sidecar-paircode.txt")) continue;   // the screenless recourse file, checked separately
    let t;
    try { t = readFileSync(f, "latin1"); } catch { continue; }
    assert.ok(!t.includes(code) && !t.includes(`${code.slice(0, 4)} ${code.slice(4)}`),
      `${f} on the card holds the pairing code`);
  }
}

// a gated call's answer, however that endpoint replies (JSON or "<code> <message>")
function refusal(text) {
  try { const j = JSON.parse(text); return { code: j.code, error: j.error || "" }; }
  catch { const m = text.match(/^(\d{3})\s+([\s\S]*)$/); return { code: m ? +m[1] : 0, error: m ? m[2] : text }; }
}

// The perimeter itself, before any device is involved: what the engine declares open, and that the two
// implementations of it agree. This fails if anyone widens the open set without saying so here.
test("the open set is exactly these, in web.cpp and in the mock", () => {
  assert.deepEqual([...OPEN].sort(),
    ["/", "/bundle-sw.js", "/health", "/index.html", "/log", "/pair", "/pair/resume", "/pair/show", "/pair/status",
     "/status"],
    "web.cpp's Open(): the page and its service worker, the health view, /status (open by the user's decision, " +
    "2026-10-26), the log, and pairing itself");
  const mockOpen = [...MOCK_PY.slice(MOCK_PY.indexOf("PAIR_OPEN = {"), MOCK_PY.indexOf("}", MOCK_PY.indexOf("PAIR_OPEN = {")))
    .matchAll(/"([^"]+)"/g)].map((m) => m[1]);
  assert.deepEqual(mockOpen.sort(), [...OPEN].sort(), "mock_web.py mirrors web.cpp's open set");
  assert.ok(ROUTES.length >= 35, `the route list came out at ${ROUTES.length}: the extraction broke, fix it`);
  assert.ok(MUST_REFUSE.includes("/fs/get") && MUST_REFUSE.includes("/wifi/status"));
});

// The Rytm MIDI bridge's two routes (pi/engine/midibridge_pi.h): never open, so the test below proves an unpaired
// client is refused them; and both behind the strict same-origin check (FromPage's last argument TRUE), read out of
// web.cpp's own branch, so a change to the lenient check there fails here
test("the Rytm MIDI bridge's routes are paired and same-origin only", () => {
  for (const p of ["/midibridge", "/midibridge/set"]) {
    assert.ok(ROUTES.includes(p), `${p} is in the route list`);
    assert.ok(MUST_REFUSE.includes(p), `${p} is not in the open set`);
  }
  const body = WEB_CPP.slice(WEB_CPP.indexOf("THTTPStatus CWebServer::GetContent"));
  const a = body.indexOf('strcmp (pPath, "/midibridge") == 0');
  assert.ok(a > 0, "web.cpp's /midibridge branch");
  const branch = body.slice(a, body.indexOf("\n\telse if", a + 1));
  assert.match(branch, /FromPage \(m_pNet, GetRequestHost \(\), GetRequestOrigin \(\), GetRequestReferer \(\), TRUE\)/);
  assert.doesNotMatch(branch, /GetRequestReferer \(\), FALSE\)/);
  assert.match(branch, /FormValue \(pFormData, "on"/, "on= from the body only");
  assert.doesNotMatch(branch, /FormValue \(pParams/, "never from the URL");
  // /cmd refuses midibridge= (its origin check is the lenient one)
  assert.match(body, /strncmp \(Cmd, "midibridge=", 11\) == 0\)/);
});

test("unpaired, wrong codes, pairing, and what stays open", async (t) => {
  const card = mkdtempSync(join(tmpdir(), "sidecar-pair-"));
  const pi = await start(t, card, { MOCK_PAIR_LOCK: "2", MOCK_PAIR_GAP: "0.2" });
  const code = codeOnScreen(pi.screen);

  // ---- nothing is paired yet
  let st = await pi.json("/pair/status");
  assert.equal(st.gate, "on");
  assert.equal(st.paired, false);
  assert.equal(st.clients, 0);
  assert.equal(st.digits, 8);

  // ---- every endpoint that is not in the open set refuses, and says how (the page watches for X-Sidecar-Pair)
  for (const path of MUST_REFUSE) {
    const url = path + (QUERY[path] || "");
    const form = BODY[path];
    const { r, text } = form === undefined ? await pi.get(url) : await pi.post(url, form);
    const f = refusal(text);
    assert.equal(f.code, 401, `${path} must refuse an unpaired client, got ${text.slice(0, 120)}`);
    assert.match(f.error, /not paired/, path);
    assert.equal(r.headers.get("x-sidecar-pair"), "required", path);
    // the body and the Content-Type must agree, or the page cannot read the refusal (it could not, for /pair/forget)
    const ct = r.headers.get("content-type") || "";
    if (ct.startsWith("application/json")) JSON.parse(text);
    else assert.match(text, /^401 /, `${path}: ${ct} with a non-JSON body`);
  }

  cardHasNoCode(card, code);   // nothing on the card holds it, before anything has paired

  // ---- the recourse file a screenless unit writes: on the card, and unreachable over the network
  assert.equal(existsSync(join(card, "sidecar-paircode.txt")), true, "a unit with no display writes the code down");
  assert.ok(readFileSync(join(card, "sidecar-paircode.txt"), "utf8").includes(code.slice(0, 4)));

  // ---- the two that mattered: the card's credentials are not readable without the code
  for (const bad of ["/fs/get?path=/.rytm/wifi.txt", "/fs/get?path=/sidecar-paircode.txt", "/fs/ls?path=/.rytm",
                     "/fs/get?path=/cmdline.txt", "/fs/get?path=/.rytm/pair.txt"]) {
    const { r, text } = await pi.get(bad);
    assert.equal(refusal(text).code, 401, bad);
    assert.equal(r.headers.get("x-sidecar-pair"), "required", bad);
    assert.ok(!text.includes("psk=") && !text.includes("ssid="), `${bad} leaked the Wi-Fi settings`);
  }
  // and nothing happened: the file the /fs/del above named is still there (once we are paired we can look)
  // ---- what stays open, with no code at all: the page, its worker, the health view, the log, pairing
  for (const path of ["/", "/index.html", "/bundle-sw.js", "/health", "/status", "/log?since=0", "/pair/status"]) {
    const { r } = await pi.get(path);
    assert.equal(r.status, 200, path);
    assert.equal(r.headers.get("x-sidecar-pair"), null, path);
  }
  // the health view: enough to tell whether a unit is working, and nothing of the unit's contents
  const health = await pi.json("/health");
  assert.equal(health.alive, true);
  for (const k of ["version", "commit", "uptime", "temp", "streaming", "rytmMidi", "display", "underruns", "power"])
    assert.ok(k in health, `/health has ${k}`);
  assert.equal(health.pair, "on");
  const hs = JSON.stringify(health);
  assert.ok(!hs.includes(code), "/health has no pairing code in it");
  for (const k of ["ssid", "psk", "machines", "host", "ip", "clients", "sessions", "stor", "library", "rec"])
    assert.ok(!(k in health), `/health must not carry ${k}: that is /status, which is gated`);

  // ---- a wrong code: refused, and the answer never quotes it
  const wrong = code === "11111111" ? "22222222" : "11111111";
  let p = await pi.json("/pair", { code: wrong });
  assert.equal(p.ok, false);
  assert.match(p.error, /not the pairing code/);
  assert.ok(!JSON.stringify(p).includes(code), "the answer never carries the real code");

  // ---- one attempt a second (MOCK_PAIR_GAP here), then a lockout after five
  p = await pi.json("/pair", { code });
  assert.equal(p.ok, false, "even the right code waits its turn");
  assert.ok(p.wait >= 1);
  for (let i = 0; i < 4; i++) {
    await sleep(250);
    p = await pi.json("/pair", { code: wrong });
    assert.equal(p.ok, false);
  }
  // the fifth wrong one is what starts the lockout, so it still answers "wrong code" - with a wait
  assert.ok(p.wait >= 1, "five wrong ones lock pairing, and it says for how long");
  await sleep(250);
  p = await pi.json("/pair", { code });
  assert.equal(p.ok, false, "locked: the right code is refused too");
  assert.match(p.error, /locked/);
  assert.equal((await pi.json("/pair/status")).wait > 0, true);

  // ---- another site's page cannot even try the code
  await sleep(2300);
  p = await pi.json("/pair", { code }, { origin: "http://evil.example" });
  assert.equal(p.ok, false);
  assert.equal(p.code, 403);
  assert.match(p.error, /same-origin/);

  // ---- the right code, once the lockout is over
  await sleep(300);
  p = await pi.json("/pair", { code, name: "the test", remember: "1" });
  assert.equal(p.ok, true, JSON.stringify(p));
  assert.match(p.token, /^[0-9a-f]{32}$/);
  assert.match(p.client, /^[0-9a-f]{8}$/);
  assert.match(p.secret, /^[0-9a-f]{32}$/);
  assert.notEqual(p.secret, code);
  const tok = p.token, client = p.client, secret = p.secret;
  st = await pi.json("/pair/status", undefined, { tok });
  assert.equal(st.paired, true);
  assert.equal(st.hdr, true, "a working kernel reports that the header arrived");
  assert.equal(st.clients, 1);
  assert.equal((await pi.json("/pair/status")).paired, false, "without the token it is still unpaired");

  // ---- /fs/* is the one group that had no origin check at all before: even with a session, a request that is not
  // from this page (a rebound page's Host is not ours, and it may send no Referer at all) is turned away
  for (const [path, hdr] of [["/fs/ls?path=/", {}], ["/fs/get?path=/config.txt", {}],
                             ["/fs/ls?path=/", { Referer: "http://evil.example/" }],
                             ["/fs/get?path=/config.txt", { Origin: "http://evil.example" }]]) {
    const { text } = await pi.get(path, tok, hdr);
    assert.equal(refusal(text).code, 403, `${path} with ${JSON.stringify(hdr)}: ${text.slice(0, 80)}`);
    assert.match(refusal(text).error, /same-origin/);
  }
  // the four writes are lenient, so a tool with neither header still works
  const mkTool = await pi.post("/fs/mkdir?path=/Samples/tool", {}, { tok, origin: "" });
  assert.equal(refusal(mkTool.text).code, 200, mkTool.text);
  const mkEvil = await pi.post("/fs/mkdir?path=/Samples/evil", {}, { tok, origin: "http://evil.example" });
  assert.equal(refusal(mkEvil.text).code, 403, "but another site's page is still refused");

  // ---- the reads work once paired, and nothing an unpaired client asked for had actually happened
  const ls = await pi.json("/fs/ls?path=/Samples/Kit", undefined, { tok });
  assert.ok(ls.entries.some((e) => e.n === "kick.wav"), "the unpaired /fs/del changed nothing");
  const kick = await pi.get("/fs/get?path=/Samples/Kit/kick.wav", tok);
  assert.equal(kick.r.status, 200, "a paired client reads the content folders as before");
  // ---- and the confinement holds even for a paired client: /fs/* is content, 7310 is firmware, /.rytm is neither
  for (const p of ["/.rytm/wifi.txt", "/.rytm/pair.txt", "/config.txt", "/cmdline.txt", "/engine.img",
                   "/sidecar-paircode.txt", "/overlays/x.dtbo"]) {
    const g = await pi.get(`/fs/get?path=${encodeURIComponent(p)}`, tok);
    assert.match(g.text, /^404 no such path/, `paired /fs/get ${p}`);
    const l = await pi.get(`/fs/ls?path=${encodeURIComponent(p)}`, tok);
    assert.match(l.text, /^404 no such path/, `paired /fs/ls ${p}`);
    const d = await pi.post(`/fs/del?path=${encodeURIComponent(p)}`, {}, { tok });
    assert.match(d.text, /^404 no such path/, `paired /fs/del ${p}`);
    const w = await pi.post(`/fs/put?path=${encodeURIComponent(p)}&off=0&total=1&crc=0`, {}, { tok });
    assert.match(w.text, /^404 no such path/, `paired /fs/put ${p}`);
    const m = await pi.post(`/fs/mv?path=/Samples/Kit/kick.wav&to=${encodeURIComponent(p)}`, {}, { tok });
    assert.match(m.text, /^404 no such path/, `paired /fs/mv into ${p}`);
  }
  // the Wi-Fi key and the paired devices are still there, untouched
  assert.match(readFileSync(join(card, ".rytm", "pair.txt"), "utf8"), /^v=1$/m);
  // the card's root lists the content folders only
  const root = await pi.json("/fs/ls?path=/", undefined, { tok });
  const names = root.entries.map((e) => e.n);
  assert.ok(names.includes("Samples") && names.includes("Plugins"), names.join(","));
  for (const hid of ["config.txt", "cmdline.txt", ".rytm", "overlays", "sidecar-paircode.txt", "kernel8-rpi4.img"])
    assert.ok(!names.includes(hid), `the root must not list ${hid}`);

  // ---- now every gated endpoint takes the token, in the header or as tok= in a POST's body
  const ok1 = await pi.post("/cmd", { c: "notes=1" }, { tok });
  assert.equal(refusal(ok1.text).code, 0, ok1.text);
  assert.equal(ok1.text.trim(), "ok");
  const ok2 = await pi.post("/cmd", { c: "notes=0", tok });
  assert.equal(ok2.text.trim(), "ok", "a tool can put the token in the body");
  assert.equal((await pi.json("/wifi/power", { on: "1" }, { tok })).ok, true);
  const mk = await pi.post("/fs/mkdir?path=/Bundles/incoming&p=1", {}, { tok });
  assert.equal(refusal(mk.text).code, 200, mk.text);

  // never in the query string (BACKLOG F7): a URL ends up in history, proxy logs and Referer headers
  const q = await pi.post(`/fs/mkdir?path=${encodeURIComponent("/Bundles/q")}&tok=${tok}`, {});
  assert.equal(refusal(q.text).code, 401, "a token in the URL is not a token");
  const qg = await pi.get(`/fs/ls?path=/Samples&tok=${tok}`);
  assert.equal(refusal(qg.text).code, 401, "nor on a GET");

  // ---- a client's own name cannot forge a log line (it is logged when it pairs)
  await sleep(250);
  const nasty = await pi.json("/pair", { code, name: "ok\n99:99:99.99 engine: FORGED", remember: "0" });
  assert.equal(nasty.ok, true);
  const nlog = (await pi.get("/log?since=0")).text;
  assert.ok(!nlog.includes("FORGED\n") || !/^\d\d:\d\d:[\d.]+ engine: FORGED/m.test(nlog), "no forged log line");
  assert.ok(!/\n\s*99:99:99/.test(nlog), "the newline in the name never reached the log");

  // ---- a wrong token is no better than none
  const bad = await pi.post("/cmd", { c: "notes=1" }, { tok: "f".repeat(32) });
  assert.equal(refusal(bad.text).code, 401);

  // ---- the code is in no answer, in no log line and in nothing /status says
  const log = (await pi.get("/log?since=0")).text;
  assert.ok(!log.includes(code), "the code is never logged");
  assert.ok(!log.includes(secret), "nor is a device's secret");
  assert.ok(!log.includes(tok), "nor is a session token");
  assert.ok(!JSON.stringify(await pi.json("/status", undefined, { tok })).includes(code), "/status has no code in it");
  assert.ok(!JSON.stringify(await pi.json("/pair/status", undefined, { tok })).includes(code));
  // the card's file keeps only a hash, and the code is on no file of the card at all
  cardHasNoCode(card, code);
  const onCard = readFileSync(join(card, ".rytm", "pair.txt"), "utf8");
  assert.ok(onCard.includes(client) && !onCard.includes(secret) && !onCard.includes(code));
  assert.match(onCard, /c=[0-9a-f]{8} [0-9a-f]{64} the test/);

  // ---- a reboot: a new code, the session gone, the remembered device resumes without the code
  await pi.stop();
  const pi2 = await start(t, card, { MOCK_PAIR_LOCK: "2", MOCK_PAIR_GAP: "0.2" });
  const code2 = codeOnScreen(pi2.screen);
  assert.notEqual(code2, code, "the code changes at every boot");
  cardHasNoCode(card, code2);
  assert.equal((await pi2.json("/pair/status", undefined, { tok })).paired, false, "the old session did not survive");
  assert.equal(refusal((await pi2.post("/cmd", { c: "notes=1" }, { tok })).text).code, 401);
  assert.equal((await pi2.json("/pair/status")).clients, 1, "the device is still remembered");
  let rs = await pi2.json("/pair/resume", { client, secret });
  assert.equal(rs.ok, true, JSON.stringify(rs));
  assert.notEqual(rs.token, tok, "and it gets a new token, not the old one");
  const tok2 = rs.token;
  assert.equal((await pi2.post("/cmd", { c: "notes=1" }, { tok: tok2 })).text.trim(), "ok");

  // ---- a wrong secret is refused and counted
  await sleep(250);
  rs = await pi2.json("/pair/resume", { client, secret: "0".repeat(32) });
  assert.equal(rs.ok, false);
  await sleep(250);
  rs = await pi2.json("/pair/resume", { client: "deadbeef", secret });
  assert.equal(rs.ok, false);

  // ---- revoking: this device, then every device
  assert.equal((await pi2.json("/pair/revoke", {}, { tok: tok2 })).ok, true);
  assert.equal((await pi2.json("/pair/status")).clients, 0);
  assert.equal(refusal((await pi2.post("/cmd", { c: "notes=1" }, { tok: tok2 })).text).code, 401,
    "revoking ends the session too");
  await sleep(250);
  assert.equal((await pi2.json("/pair/resume", { client, secret })).ok, false, "and the secret is worthless now");
  // revoking needs a session of its own
  assert.equal((await pi2.json("/pair/revoke", {}, { tok: "" })).code, 401);
  await sleep(2300);
  const p3 = await pi2.json("/pair", { code: code2, remember: "1" });
  assert.equal(p3.ok, true);
  assert.equal((await pi2.json("/pair/forget", {}, { tok: p3.token })).ok, true);
  assert.equal((await pi2.json("/pair/status")).clients, 0);
  await pi2.stop();
});

// A browser (or sdboot.py) holding a stale saved pairing retries it on every load. On the device that was reported
// as "the pairing code is 8 digits" - about a code nobody had mistyped - and each retry burned a pairing attempt, so
// five reloads locked the owner out of pairing and, because the limiter is shared, out of `sdboot.py install` too.
test("a stale saved pairing cannot lock the owner out, and is not called a bad code", async (t) => {
  const card = mkdtempSync(join(tmpdir(), "sidecar-pair-stale-"));
  const pi = await start(t, card, { MOCK_PAIR_LOCK: "60", MOCK_PAIR_GAP: "1" });
  const code = codeOnScreen(pi.screen);

  // every shape a stale or junk store can hold, including the "undefined" a page writes for a missing field
  const junk = [["undefined", "undefined"], ["", ""], ["0123abcd", "short"], ["zzzzzzzz", "f".repeat(32)],
                ["0123abc", "f".repeat(32)], ["0123abcd", "f".repeat(33)], ["0123abcd", "f".repeat(32)]];
  for (let reload = 0; reload < 3; reload++) {         // three page loads' worth, well past the five-attempt lockout
    for (const [client, secret] of junk) {
      const r = await pi.json("/pair/resume", { client, secret });
      assert.equal(r.ok, false);
      assert.doesNotMatch(r.error, /8 digits/, `${client}/${secret}: not a complaint about the code`);
      assert.match(r.error, /saved pairing/, "it says what is actually wrong");
      assert.equal(r.wait, 0, "and it does not start a lockout");
    }
  }
  assert.equal((await pi.json("/pair/status")).wait, 0, "pairing is not locked after 21 stale resumes");
  // so the owner can still pair with the code on the screen
  const p = await pi.json("/pair", { code, remember: "1" });
  assert.equal(p.ok, true, JSON.stringify(p));
  assert.equal((await pi.post("/cmd", { c: "notes=1" }, { tok: p.token })).text.trim(), "ok");

  // a real wrong code still locks, as before: only the code's own path counts
  for (let i = 0; i < 5; i++) { await sleep(1100); await pi.json("/pair", { code: "00000000" }); }
  assert.ok((await pi.json("/pair/status")).wait > 0, "a wrong code still locks pairing");
  // ... and the owner's remembered device can still come back, which lifts it
  const back = await pi.json("/pair/resume", { client: p.client, secret: p.secret });
  assert.equal(back.ok, true, "a lockout does not block a device that is already paired");
  assert.equal((await pi.json("/pair/status")).wait, 0, "and only the owner can do that, so it lifts the lockout");
  await pi.stop();
});

// A kernel built without pi/circle/circle-http-auth.patch never sees X-Sidecar-Auth, so the code is accepted and
// then nothing stays paired. On the device that looked like "the dialog comes back and survives a reload"; the page
// must be able to tell the user what is actually wrong instead of asking for the code forever.
test("a kernel that cannot read the pairing header says so", async (t) => {
  const card = mkdtempSync(join(tmpdir(), "sidecar-pair-nohdr-"));
  const pi = await start(t, card, { MOCK_PAIR_NOHDR: "1" });
  const code = codeOnScreen(pi.screen);
  const st = await pi.json("/pair/status");
  assert.equal(st.gate, "on");
  assert.equal(st.hdr, false, "it reports that the header did not arrive");
  assert.equal(typeof st.host, "string");
  // the code is still accepted - the body carries it - and a token comes back
  const p = await pi.json("/pair", { code, remember: "1" });
  assert.equal(p.ok, true);
  // ... but the token buys nothing, and /pair/status says so rather than leaving the page to guess
  const after = await pi.json("/pair/status", undefined, { tok: p.token });
  assert.equal(after.paired, false, "the token is not recognised");
  assert.equal(after.hdr, false, "and the reason is reported");
  assert.equal(refusal((await pi.post("/cmd", { c: "notes=1" }, { tok: p.token })).text).code, 401);
  // the body form still works, which is what keeps a tool going on such a kernel
  assert.equal((await pi.post("/cmd", { c: "notes=1", tok: p.token })).text.trim(), "ok");
  await pi.stop();
});

test("cmdline.txt paircode=0 (a dev card) and a fixed code", async (t) => {
  // ---- the gate off: nothing needs a token, and it says so loudly
  const card = mkdtempSync(join(tmpdir(), "sidecar-pair-off-"));
  const off = await start(t, card, { MOCK_PAIR: "0" });
  const st = await off.json("/pair/status");
  assert.equal(st.gate, "off");
  assert.equal(st.paired, true, "with the gate off every client counts as paired");
  assert.equal((await off.json("/health")).pair, "off", "/health says the gate is off");
  assert.equal((await off.json("/health")).pair, "off", "/health says the gate is off too");
  assert.equal((await off.json("/pair", { code: "12345678" })).ok, false, "there is nothing to pair with");
  // /wifi/join is left out: with the gate off it really joins, and the mock then plays "the Pi is away" for a few
  // seconds, which would kill the rest of the loop. The gated case covers it in the test above.
  for (const path of MUST_REFUSE.filter((p) => p !== "/wifi/join")) {
    const url = path + (QUERY[path] || "");
    const form = BODY[path];
    const { r, text } = form === undefined ? await off.get(url) : await off.post(url, form);
    assert.equal(r.headers.get("x-sidecar-pair"), null, path);
    assert.notEqual(refusal(text).code, 401, `${path} needs no code with the gate off`);
  }
  await off.stop();

  // ---- the gate on again on the same card: it is gated once more (the bypass is not sticky)
  const on = await start(t, card, {});
  assert.equal((await on.json("/pair/status")).gate, "on");
  assert.equal(refusal((await on.post("/cmd", { c: "notes=1" })).text).code, 401);
  await on.stop();

  // ---- a fixed code from cmdline.txt: still gated, and the page is told it is fixed
  const card2 = mkdtempSync(join(tmpdir(), "sidecar-pair-fix-"));
  const fix = await start(t, card2, { MOCK_PAIR: "13572468" });
  assert.equal((await fix.json("/pair/status")).gate, "fixed");
  assert.equal((await fix.json("/health")).pair, "fixed");
  assert.equal(refusal((await fix.post("/cmd", { c: "notes=1" })).text).code, 401, "a fixed code is still a code");
  assert.equal(codeOnScreen(fix.screen), "13572468");
  const p = await fix.json("/pair", { code: "13572468" });
  assert.equal(p.ok, true);
  assert.equal((await fix.post("/cmd", { c: "notes=1" }, { tok: p.token })).text.trim(), "ok");
  await fix.stop();
});

// The owner's normal flow on a shared Wi-Fi (audit 2026-10-26): someone else guessing locks only their own address; a
// malformed code or a bare POST /pair never counts; the code goes on the Rytm when the page asks for it, not on every
// page load (a paired owner's reload flashed it), and at most once every 30 s whoever asks.
test("someone else's guesses do not lock the owner out, and the code is shown only when asked for", async (t) => {
  const card = mkdtempSync(join(tmpdir(), "sidecar-pair-peer-"));
  const pi = await start(t, card, { MOCK_PAIR_LOCK: "60", MOCK_PAIR_GAP: "1" });
  const code = codeOnScreen(pi.screen);
  const wrong = code === "11111111" ? "22222222" : "11111111";
  const other = { headers: { "X-Mock-Peer": "192.168.1.102" } };

  // ---- a bare POST /pair, a 7-digit typo, a paste with a letter: refused, never counted, no wait
  for (const form of [{}, { code: "" }, { code: "1234567" }, { code: "1234 5678" }, { code: "1234567a" }]) {
    const r = await pi.json("/pair", form);
    assert.equal(r.ok, false);
    assert.match(r.error, /8 digits/);
    assert.equal(r.wait, 0, `${JSON.stringify(form)} is not a guess`);
  }
  assert.equal((await pi.get("/pair")).r.status, 200, "a bare GET /pair answers");
  assert.equal((await pi.json("/pair/status")).wait, 0, "and nothing of it started a lockout");

  // ---- another address guesses five times: it is locked, the owner is not
  for (let i = 0; i < 5; i++) { await pi.json("/pair", { code: wrong }, other); await sleep(1050); }
  const theirs = await pi.json("/pair", { code }, other);
  assert.equal(theirs.ok, false, "the guesser is locked, even with the right code");
  assert.match(theirs.error, /locked/);
  assert.equal((await pi.json("/pair/status")).wait, 0, "the owner's address is not");
  const mine = await pi.json("/pair", { code, remember: "1" });
  assert.equal(mine.ok, true, JSON.stringify(mine));
  // ... and the owner's resume does not lift the guesser's lockout (a reload is not a reset for an attacker)
  assert.equal((await pi.json("/pair/resume", { client: mine.client, secret: mine.secret })).ok, true);
  assert.ok((await pi.json("/pair/status", undefined, other)).wait > 0, "the guesser is still locked");

  // ---- the code on the Rytm: never because a page loaded, once when the page asks, then rate-limited
  const shows = async () => ((await pi.get("/log?since=0")).text.match(/the code is on the Rytm's screen/g) || []).length;
  await pi.get("/");
  await pi.get("/index.html");
  assert.equal(await shows(), 0, "a page load does not put the code on the Rytm");
  const first = await pi.json("/pair/show", {});
  assert.equal(first.ok, true);
  assert.equal(first.shown, true, "the answer says it is up");
  assert.equal(await shows(), 1, "the dialog's request does");
  for (let i = 0; i < 5; i++) {
    const again = await pi.json("/pair/show", {}, other);
    assert.equal(again.shown, false);
    assert.ok(again.wait > 0 && again.wait <= 10, `refused with the gap's wait (fixed 10 s): ${JSON.stringify(again)}`);
    assert.deepEqual(Object.keys(again).sort(), ["ok", "shown", "wait"], "nothing else in the answer (no code)");
  }
  assert.equal(await shows(), 1, "at most once every 10 s, whoever asks");
  assert.ok(!(await pi.get("/log?since=0")).text.includes(code), "and the code itself is in no log line");
  const evil = await pi.post("/pair/show", {}, { origin: "http://evil.example" });
  assert.notEqual(evil.r.status, 200, "another site's page cannot ask");
  await pi.stop();
});

// The page's side of it: it asks for the code on the Rytm (POST /pair/show) only where it has no usable saved pairing,
// and it has a button for it; read out of the page itself
test("the page asks for the code on the Rytm only when it is really unpaired", () => {
  const html = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
  const boot = html.slice(html.indexOf("async function pairBoot()"), html.indexOf("$(\"#pair-go\").onclick"));
  const resume = boot.indexOf("pairJson(\"/pair/resume\""), paired = boot.indexOf("st.paired");
  const firstShow = boot.indexOf("pairShowCode()");
  assert.ok(paired > 0 && resume > 0 && firstShow > resume, "after the paired check and after a resume was tried");
  assert.match(html, /id="pair-showrytm"/);
  assert.match(html, /fetch\("\/pair\/show", \{ method: "POST"/);
  // the button says what happened: up, or the wait the answer carries
  const btn = html.slice(html.indexOf('$("#pair-showrytm").onclick'), html.indexOf('$("#pair-go").onclick'));
  assert.match(btn, /r\.shown/);
  assert.match(btn, /Shown on the Rytm/);
  assert.match(btn, /Wait \$\{r\.wait\} s/);
  const web2 = readFileSync(join(HERE, "..", "web.cpp"), "utf8");
  assert.match(web2, /\\"shown\\":%s,\\"wait\\":%u/, "the engine's /pair/show answers shown and wait");
  assert.doesNotMatch(html.slice(html.indexOf("function pairLost"), html.indexOf("async function pairJson")), /pairShowCode/,
    "a lost session (a reboot, an hour idle) resumes first: it does not flash the code");
  const web = readFileSync(join(HERE, "..", "web.cpp"), "utf8");
  const page = web.slice(web.indexOf('strcmp (pPath, "/") == 0 || strcmp (pPath, "/index.html") == 0'));
  assert.doesNotMatch(page.slice(0, page.indexOf("\n\telse if")), /ShowUnpaired/, "GET / does not show the code");
});

// The Rytm's SIDECAR MENU > WEB UI off: every HTTP path refuses first (before the pairing gate, the open paths too)
// and port 7310 from the network refuses every line as unauthenticated (sdboot.py's probe then says why); the serial
// :sdf is physical access and stays. Read out of the engine's source: neither runs on the Mac.
test("WEB UI off on the Rytm: the web page and port 7310 refuse, the serial :sdf does not", () => {
  const web = readFileSync(join(HERE, "..", "web.cpp"), "utf8");
  const get = web.slice(web.indexOf("THTTPStatus CWebServer::GetContent"));
  const off = get.indexOf("if (!paircode_pi::WebOn ())"), gate = get.indexOf("if (!Open (pPath) && !paircode_pi::Gate ().check");
  assert.ok(off > 0 && gate > off, "the WEB UI check comes before anything else answers");
  assert.match(get.slice(off, gate), /return HTTPOK;/);
  assert.match(get.slice(off, gate), /kWebOff/);
  const sdf = readFileSync(join(HERE, "..", "sdfiles.cpp"), "utf8");
  const serve = sdf.slice(sdf.indexOf("void CSdFiles::Serve"));
  assert.match(serve.slice(0, serve.indexOf("if (!m_bAuth)")),
    /if \(m_pSerial == 0 && !paircode_pi::WebOn \(\)\)[\s\S]*ERR unauthenticated: %s/);
  const pi = readFileSync(join(HERE, "..", "paircode_pi.h"), "utf8");
  assert.match(pi, /kWebOff = "the web UI is off on the Rytm \(SIDECAR MENU > WEB UI\)"/);
  const sdboot = readFileSync(join(HERE, "..", "..", "circle", "sdboot.py"), "utf8");
  assert.match(sdboot, /probe\.startswith\("ERR unauthenticated"\)/, "sdboot.py takes the gated path on it");
});

// 0.3.0 ships with the gate ON: the release card's scripts never write paircode= (a dev card's owner adds it by hand)
test("the release card's cmdline.txt leaves the pairing code on", () => {
  const dir = join(HERE, "..", "..", "circle");
  for (const f of readdirSync(dir).filter((n) => /^make-sd.*\.sh$/.test(n))) {
    const t = readFileSync(join(dir, f), "utf8");
    assert.doesNotMatch(t, /paircode=[0-9]/, `${f} must not set paircode= on a release card`);
  }
  // and a builder's environment cannot slip one in: make-sd-dual.sh refuses a CMDLINE with paircode= unless DEV=1
  const dual = join(dir, "make-sd-dual.sh");
  const run = (env) => spawnSync("bash", [dual, "/nonexistent-k4"], { env: { ...process.env, ...env }, encoding: "utf8" });
  const r = run({ CMDLINE: "sdport=0 paircode=0" });
  assert.notEqual(r.status, 0);
  assert.match(r.stdout + r.stderr, /paircode=: not on a release card/);
  const d = run({ CMDLINE: "sdport=0 paircode=0", DEV: "1" });
  assert.doesNotMatch(d.stdout + d.stderr, /not on a release card/, "DEV=1 is the deliberate dev card");
  assert.doesNotMatch(run({ CMDLINE: "sdport=0" }).stdout, /not on a release card/);
});
