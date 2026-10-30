// pi/engine/tests/page_slots.test.mjs - the Sidecar page's slot list (pi/engine/www/index.html, User machines): where a
// slot's machine came from, export, import (its console lines and the missing machines), against mock_web.py's /um and
// /cmd (which keep the slots as core/user/um.cpp does). node --test pi/engine/tests/page_slots.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("const SRCTXT"), b = page.indexOf('$("#um-export")');
assert.ok(a > 0 && b > a, "the slot list helpers in index.html");
const F = new Function(page.slice(a, b) + "; return { slotSrc, slotListExport, slotListParse, slotListCmds, slotListMissing };")();

test("where a slot's machine came from", () => {
  assert.equal(F.slotSrc({ src: "sounds", state: "READY" }).text, "from the project's sounds");
  assert.equal(F.slotSrc({ src: "saved", updated: true, state: "READY" }).text, "updated from the project's sounds");
  assert.equal(F.slotSrc({ src: "inherited", state: "READY" }).text, "inherited from the previous project");
  assert.equal(F.slotSrc({ src: "none", unknown: true, want_fp: 4711, want_name: "WOBBLE", state: "EMPTY" }).text,
    "needs a machine with fingerprint 4711 (WOBBLE)");
  assert.match(F.slotSrc({ src: "imported", state: "MISSING", name: "WOBBLE", id: "ext.wobble", ver: "0.3.0" }).text, /^missing: install WOBBLE 0.3.0/);
  assert.match(F.slotSrc({ src: "sounds", collision: true, want_fp: 99 }).text, /several machines/);
  assert.equal(F.slotSrc({ src: "empty", state: "EMPTY" }).text, "empty (cleared)");
});

test("export and parse round trip; a slot map file parses too; junk refused", () => {
  const S = { 1: { id: "sc.acid", ver: "1.0.0", name: "ACID", fp: 1000 }, 2: { id: "", src: "none" }, 5: { id: "", src: "empty" } };
  const l = F.slotListExport("MY SET", S);
  assert.deepEqual(l, { format: "sidecar-slots", version: 1, project: "MY SET",
    slots: [{ slot: 1, id: "sc.acid", ver: "1.0.0", name: "ACID", fp: 1000 }, { slot: 5, empty: true }] });
  const p = F.slotListParse(JSON.stringify(l));
  assert.equal(p.project, "MY SET");
  assert.deepEqual(F.slotListCmds(p), ["um=put:1:sc.acid:1.0.0:ACID", "um=put:5:-:0.0.0"]);
  assert.deepEqual(F.slotListParse("U2 sc.metal 1.2.0\nU4 x.y 0.0.1\n").slots, [{ slot: 2, id: "sc.metal", ver: "1.2.0" }, { slot: 4, id: "x.y", ver: "0.0.1" }]);
  assert.throws(() => F.slotListParse("hello"));
  assert.throws(() => F.slotListParse('{"format":"other"}'));
  const odd = F.slotListParse(JSON.stringify({ format: "sidecar-slots", slots: [{ slot: 9, id: "a" }, { slot: 3, id: "has space" },
    { slot: 2, id: "ok.id", ver: "bad", name: "N:AéME LONG NAME" }] }));
  assert.deepEqual(odd.slots, [{ slot: 2, id: "ok.id", ver: "0.0.0", name: "N_A_ME LON", fp: 0 }]);   // no ':' in a console line
  assert.deepEqual(F.slotListMissing(p, [{ id: "sc.metal", fit: true }]), ["U1 ACID 1.0.0 (sc.acid)"]);
});

test("import against the mock: put lines change /um, missing reported by name", async (t) => {
  // MOCK_PAIR=0 is cmdline.txt paircode=0: the pairing code is out of the way here, because this test is about
  // something else. The gate itself (every endpoint it covers, refusals, the lockout) is page_pair.test.mjs.
  const port = 18700 + Math.floor(Math.random() * 500);
  const srv = spawn("python3", [join(HERE, "mock_web.py"), String(port)], { stdio: "ignore", env: { ...process.env, MOCK_PAIR: "0" } });
  t.after(() => srv.kill());
  const base = `http://127.0.0.1:${port}`;
  let um;
  for (let i = 0; i < 50 && !um; i++) {
    try { um = await (await fetch(base + "/um")).json(); } catch { await new Promise((r) => setTimeout(r, 100)); }
  }
  assert.ok(um, "mock_web answers");
  const S = {};
  um.filter((x) => x.t === "slot").forEach((x) => { S[x.slot] = x; });
  assert.equal(S[3].unknown, true);
  assert.equal(F.slotSrc(S[3]).text, "needs a machine with fingerprint 4711 (WOBBLE)");
  const exp = F.slotListExport("AR PERC PATCH", S);
  assert.deepEqual(exp.slots.map((x) => x.slot), [1, 2, 4, 5]);
  const list = F.slotListParse(JSON.stringify({ format: "sidecar-slots", version: 1, project: "AR PERC PATCH",
    slots: [{ slot: 3, id: "ext.wobble", ver: "0.3.0", name: "WOBBLE", fp: 4711 }, { slot: 4, empty: true }] }));
  for (const c of F.slotListCmds(list))
    await fetch(base + "/cmd", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: "c=" + encodeURIComponent(c) });
  const um2 = await (await fetch(base + "/um")).json();
  const S2 = {};
  um2.filter((x) => x.t === "slot").forEach((x) => { S2[x.slot] = x; });
  assert.equal(S2[3].state, "MISSING");
  assert.equal(S2[3].src, "imported");
  assert.equal(S2[4].src, "empty");
  assert.deepEqual(F.slotListMissing(list, um2.filter((x) => x.t === "plugin")), ["U3 WOBBLE 0.3.0 (ext.wobble)"]);
  await fetch(base + "/cmd", { method: "POST", body: "c=um%3Dempty", headers: { "Content-Type": "application/x-www-form-urlencoded" } });
  const um3 = await (await fetch(base + "/um")).json();
  assert.deepEqual(um3.filter((x) => x.t === "slot").map((x) => x.src), ["empty", "empty", "empty", "empty", "empty"]);
});
