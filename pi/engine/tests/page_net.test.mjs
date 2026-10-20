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

// the gate's block, run against a stand-in fetch that counts what is in flight
function gate(fake) {
  const a = page.indexOf("// ---- every request through one gate"), b = page.indexOf("\n}\n", a);
  assert.ok(a > 0 && b > a, "the gate in index.html");
  const win = { fetch: fake };
  new Function("window", page.slice(a, b + 2))(win);
  return win.fetch;
}

test("at most 2 requests at once, the rest in order", async () => {
  let now = 0, max = 0;
  const order = [];
  const f = gate(async (url) => { now++; max = Math.max(max, now); await sleep(20); now--; order.push(url); return { status: 200 }; });
  const rs = await Promise.all(Array.from({ length: 9 }, (_, i) => f("/r" + i)));
  assert.equal(max, 2);
  assert.equal(rs.length, 9);
  assert.deepEqual(order.slice(0, 2).sort(), ["/r0", "/r1"]);
  assert.equal(order.length, 9);
});

test("a 503 is tried again (3 times), a failure frees its slot", async () => {
  let n = 0;
  const f = gate(async () => (++n <= 2 ? { status: 503 } : { status: 200 }));
  assert.equal((await f("/status")).status, 200);
  assert.equal(n, 3);
  let m = 0;
  const g = gate(async () => { m++; return { status: 503 }; });
  assert.equal((await g("/status")).status, 503);			// given up: the caller sees the 503
  assert.equal(m, 4);
  const h = gate(async (url) => { if (url === "/bad") throw new Error("network"); return { status: 200 }; });
  await assert.rejects(h("/bad"));
  await assert.rejects(h("/bad"));
  await assert.rejects(h("/bad"));
  assert.equal((await h("/ok")).status, 200);				// the slots came back after the throws
});

test("the page gzip-compressed and /status power against the mock", async (t) => {
  const port = 19900 + Math.floor(Math.random() * 90);
  const srv = spawn("python3", [join(HERE, "mock_web.py"), String(port)], { stdio: "ignore", env: { ...process.env, MOCK_POWER: "weak" } });
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
