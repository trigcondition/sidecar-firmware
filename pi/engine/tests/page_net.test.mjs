// pi/engine/tests/page_net.test.mjs - the Sidecar page's network side (pi/engine/www/index.html): the request gate
// (at most 2 requests at once from the page, a 503 tried again), and against mock_web.py the page as web.cpp sends it
// (gzip, no-cache; ?raw plain) and /status "power" (MOCK_POWER=weak). node --test pi/engine/tests/page_net.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// the gate's block, run against a stand-in fetch that counts what is in flight. The block also puts the pairing token
// on each request and watches for the "not paired" header (pi/engine/paircode.h), so the two are handed in: PAIR with
// no token (nothing added) and a pairAsk that must not be called.
function gate(fake, pair = { tok: "" }) {
  const a = page.indexOf("// ---- every request through one gate"), b = page.indexOf("\n}\n", a);
  assert.ok(a > 0 && b > a, "the gate in index.html");
  const win = { fetch: fake };
  const asked = [];
  // pairLost is what the wrapper calls now: it bumps the generation and asks, as the page's does
  const pairLost = (why) => { pair.gen++; pair.tok = ""; asked.push(why || 1); };
  new Function("window", "PAIR", "pairAsk", "pairLost", page.slice(a, b + 2))(win, pair, (w) => asked.push(w || 1), pairLost);
  win.fetch.asked = asked;
  return win.fetch;
}
const noHeaders = { get: () => null };

test("at most 2 requests at once, the rest in order", async () => {
  let now = 0, max = 0;
  const order = [];
  const f = gate(async (url) => { now++; max = Math.max(max, now); await sleep(20); now--; order.push(url); return { status: 200, headers: noHeaders }; });
  const rs = await Promise.all(Array.from({ length: 9 }, (_, i) => f("/r" + i)));
  assert.equal(max, 2);
  assert.equal(rs.length, 9);
  assert.deepEqual(order.slice(0, 2).sort(), ["/r0", "/r1"]);
  assert.equal(order.length, 9);
});

test("a 503 is tried again (3 times), a failure frees its slot", async () => {
  let n = 0;
  const f = gate(async () => (++n <= 2 ? { status: 503, headers: noHeaders } : { status: 200, headers: noHeaders }));
  assert.equal((await f("/status")).status, 200);
  assert.equal(n, 3);
  let m = 0;
  const g = gate(async () => { m++; return { status: 503, headers: noHeaders }; });
  assert.equal((await g("/status")).status, 503);			// given up: the caller sees the 503
  assert.equal(m, 4);
  const h = gate(async (url) => { if (url === "/bad") throw new Error("network"); return { status: 200, headers: noHeaders }; });
  await assert.rejects(h("/bad"));
  await assert.rejects(h("/bad"));
  await assert.rejects(h("/bad"));
  assert.equal((await h("/ok")).status, 200);				// the slots came back after the throws
});

// the pairing token's side of the gate (pi/engine/paircode.h): the header on this page's own requests only, and a
// "not paired" answer drops the token and asks again
test("the pairing token rides in X-Sidecar-Auth, and a refusal asks again", async () => {
  const seen = [];
  const pair = { tok: "a".repeat(32) };
  const f = gate(async (url, opt) => {
    seen.push([url, opt && opt.headers ? opt.headers.get("X-Sidecar-Auth") : null]);
    return { status: 200, headers: noHeaders };
  }, pair);
  await f("/cmd", { method: "POST", body: "c=notes%3D1" });
  await f("/status", { cache: "no-store" });
  await f("/status");                                   // no options at all
  await f("https://example.invalid/x");                 // another origin: no token on it
  assert.deepEqual(seen, [["/cmd", pair.tok], ["/status", pair.tok], ["/status", pair.tok],
                          ["https://example.invalid/x", null]]);
  // the header the engine sends when it refuses (web.cpp's PairRefused)
  const g = gate(async () => ({ status: 200, headers: { get: (k) => (k === "X-Sidecar-Pair" ? "required" : null) } }), pair);
  await g("/cmd", { method: "POST" });
  assert.equal(pair.tok, "", "the token is dropped");
  assert.equal(g.asked.length, 1, "and the dialog is asked for");
});

// The bug the user hit on hardware (2026-10-25): "it confirms it is correct and saved, but the pairing code dialog
// stays on top of the page". A request issued while the page was still unpaired lands after the code was accepted -
// it was queued behind the pairing request, or already in flight - and its refusal wiped the token that had just
// worked. The mock never produced it because it paired instantly with nothing outstanding.
test("a refusal from before a pairing cannot undo it", async () => {
  const pair = { tok: "", gen: 0 };
  let release;
  const held = new Promise((ok) => { release = ok; });
  const f = gate(async (url) => {
    if (url === "/slow") { await held; return { status: 200, headers: { get: (k) => (k === "X-Sidecar-Pair" ? "required" : null) } }; }
    return { status: 200, headers: noHeaders };
  }, pair);

  const inflight = f("/slow", { cache: "no-store" });   // sent while unpaired: it will be refused
  await new Promise((r) => setTimeout(r, 10));
  // ... and now the code is accepted, exactly as pairDone does it
  pair.gen++;
  pair.tok = "b".repeat(32);
  release();
  await inflight;
  assert.equal(pair.tok, "b".repeat(32), "the token that just worked survives the stale refusal");
  assert.equal(f.asked.length, 0, "and the dialog is not reopened");

  // a refusal of a request sent *after* the pairing is real, and still counts
  const g = gate(async () => ({ status: 200, headers: { get: (k) => (k === "X-Sidecar-Pair" ? "required" : null) } }), pair);
  await g("/cmd", { method: "POST" });
  assert.equal(pair.tok, "", "a current refusal is acted on");
  assert.equal(g.asked.length, 1);
});

// The store's shape check, which is what stops a junk entry ever reaching the Sidecar (hardware, 2026-10-25: a
// stale entry was answered "the pairing code is 8 digits" and burned a pairing attempt on every page load).
test("the remembered pairing is checked for shape, both ways", () => {
  const a = page.indexOf("const pairId ="), b = page.indexOf("\n};", page.indexOf("const pairStore = {"));
  assert.ok(a > 0 && b > a, "pairStore in index.html");
  const mem = new Map();
  const localStorage = {
    getItem: (k) => (mem.has(k) ? mem.get(k) : null),
    setItem: (k, v) => mem.set(k, String(v)),
    removeItem: (k) => mem.delete(k),
  };
  const store = new Function("localStorage", page.slice(a, b + 3) + "; return pairStore;")(localStorage);
  const ID = "0123abcd", SEC = "0123456789abcdef0123456789abcdef";

  store.set(ID, SEC);
  assert.deepEqual(store.get(), { c: ID, s: SEC });
  store.set(ID.toUpperCase(), SEC.toUpperCase());
  assert.deepEqual(store.get(), { c: ID, s: SEC }, "kept lower case, as the device writes it");

  // nothing that cannot be a credential is ever written ...
  for (const [c, s] of [[undefined, SEC], [ID, undefined], ["", ""], ["undefined", "undefined"], [ID, "short"],
                        ["zzzzzzzz", SEC], [ID, SEC + "f"], [ID, SEC.slice(1)], [null, null], [{}, []]]) {
    store.set(ID, SEC);                       // a good one first, so a bad set must clear it
    store.set(c, s);
    assert.deepEqual(store.get(), { c: "", s: "" }, `set(${String(c)}, ${String(s)})`);
  }
  // ... and a junk entry left by an older build is dropped on read, not sent
  for (const [c, s] of [["undefined", "undefined"], [ID, "nope"], ["", SEC], [ID, ""]]) {
    mem.set("scPairClient", c);
    mem.set("scPairSecret", s);
    assert.deepEqual(store.get(), { c: "", s: "" }, `get() with ${c}/${s}`);
    assert.equal(mem.has("scPairClient"), false, "and it is cleared, so it is not retried every load");
  }
  // storage that throws (private mode) is survivable
  const dead = { getItem: () => { throw new Error("no"); }, setItem: () => { throw new Error("no"); }, removeItem: () => {} };
  const s2 = new Function("localStorage", page.slice(a, b + 3) + "; return pairStore;")(dead);
  assert.deepEqual(s2.get(), { c: "", s: "" });
  s2.set(ID, SEC);
});

test("the page gzip-compressed and /status power against the mock", async (t) => {
  // MOCK_PAIR=0 is cmdline.txt paircode=0: the pairing code is out of the way here, because this test is about
  // something else. The gate itself (every endpoint it covers, refusals, the lockout) is page_pair.test.mjs.
  const port = 19900 + Math.floor(Math.random() * 90);
  const srv = spawn("python3", [join(HERE, "mock_web.py"), String(port)], { stdio: "ignore", env: { ...process.env, MOCK_POWER: "weak", MOCK_PAIR: "0" } });
  t.after(() => srv.kill());
  const base = `http://127.0.0.1:${port}`;
  for (let i = 0; i < 50; i++) { try { await fetch(base + "/status"); break; } catch (e) { await sleep(100); } }
  const r = await fetch(base + "/");
  assert.equal(r.headers.get("content-encoding"), "gzip");
  assert.equal(r.headers.get("cache-control"), "no-cache");
  const html = await r.text();						// fetch decodes it
  assert.ok(html.includes('id="pwarn"') && html.includes("window.fetch = async"));
  const raw = await fetch(base + "/?raw=1");
  assert.equal(raw.headers.get("content-encoding"), null);
  assert.equal(await raw.text(), html);
  const st = await (await fetch(base + "/status")).json();
  assert.match(st.power.warn, /too weak.*27 W \(5 V 5 A\) adapter directly/);
  assert.equal(st.power.maxCurrent, 3000);
  assert.equal(typeof st.web.idle, "number");
});
