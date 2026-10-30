// pi/engine/tests/page_seg.test.mjs - the Sound tab's choice groups (pi/engine/www/index.html): the pending state, its
// confirmation by /status, the revert after 2 s, and mock_web.py's /cmd -> /status round trip that the groups follow.
// node --test pi/engine/tests/page_seg.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("const pend = {"), b = page.indexOf("function paintSeg");
assert.ok(a > 0 && b > a, "the pending helper in index.html");
const els = {};
const doc = { querySelector: (q) => (els[q] = els[q] || { textContent: "", _t: 0, classList: { toggle() {} } }) };
const mk = new Function("document", "paintSeg", "setTimeout", "clearTimeout", page.slice(a, b) + "; return { pend, droneVal, chosen: null };");

function rig() {
  const timers = [];
  let n = 0;
  const f = mk(doc, () => {}, (fn) => (timers.push({ fn, id: ++n }), n), (id) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); });
  return { ...f, timers, fb: (k) => els[`[data-fb="${k}"]`]?.textContent };
}

test("a click is pending, /status confirms it, an old state does not", () => {
  const { pend, timers, fb } = rig();
  pend.start("out", "main");
  assert.equal(pend.want("out"), "main");
  assert.equal(pend.report("out", "tracks"), false);
  assert.equal(pend.has("out"), true);
  assert.equal(pend.report("out", "main"), true);
  assert.equal(fb("out"), "saved ✓");
  assert.equal(timers.length, 1);   // only the feedback's own clear timer is left
});

test("no confirmation in 2 s: put back with a message; a refused command too", () => {
  const { pend, timers, fb } = rig();
  pend.start("mac", "false");
  timers.shift().fn();
  assert.equal(pend.has("mac"), false);
  assert.match(fb("mac"), /no answer/);
  pend.start("notes", "true");
  pend.fail("notes", "Sidecar refused the command");
  assert.equal(fb("notes"), "Sidecar refused the command");
});

test("drone: off, on with a voice, all", () => {
  const { droneVal } = rig();
  assert.deepEqual([-1, 3, 8].map(droneVal), ["off", "on:3", "on:all"]);
});

test("mock_web: commands change /status (machines per voice, fifo limit), MOCK_IGNORE leaves one unchanged", async (t) => {
  // MOCK_PAIR=0 is cmdline.txt paircode=0: the pairing code is out of the way here, because this test is about
  // something else. The gate itself (every endpoint it covers, refusals, the lockout) is page_pair.test.mjs.
  const port = 19300 + Math.floor(Math.random() * 500);
  const srv = spawn("python3", [join(HERE, "mock_web.py"), String(port)], { stdio: "ignore", env: { ...process.env, MOCK_IGNORE: "mac", MOCK_PAIR: "0" } });
  t.after(() => srv.kill());
  const base = `http://127.0.0.1:${port}`;
  let s;
  for (let i = 0; i < 50 && !s; i++) { try { s = await (await fetch(base + "/status")).json(); } catch { await new Promise((r) => setTimeout(r, 100)); } }
  assert.ok(s, "mock_web answers");
  assert.equal(s.machines.length, 8); assert.equal(s.machines[0], "PLAITS");
  const cmd = (c) => fetch(base + "/cmd", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: "c=" + encodeURIComponent(c) });
  for (const c of ["notes=1", "out=tracks", "mac=off", "drone=2", "fifo=8", "notemach=3:juno"]) await cmd(c);
  s = await (await fetch(base + "/status")).json();
  assert.equal(s.notes, true); assert.equal(s.out, "tracks"); assert.equal(s.mac, true);
  assert.equal(s.drone, 2); assert.equal(s.fifoTarget, 24); assert.equal(s.machines[3], "JUNO");
});
