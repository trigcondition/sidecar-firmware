// pi/engine/tests/page_wifi.test.mjs - the Sidecar page's Wi-Fi settings (pi/engine/www/index.html, Status tab): its
// input check (as wifi_conf.cpp's check_join), and the /wifi/* flow against mock_web.py: the same-origin refusal, a
// scan of fake networks, a join that restarts and comes up, a wrong password that fails and reverts, forget. Only
// fake networks and passwords. node --test pi/engine/tests/page_wifi.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("function wfCheck"), b = page.indexOf('$("#wf-join").onclick');
assert.ok(a > 0 && b > a, "wfCheck in index.html");
const wfCheck = new Function(page.slice(a, b) + "; return wfCheck;")();

test("the page's input check", () => {
  assert.equal(wfCheck("Fake-Net", "not-a-real-pass", false), "");
  assert.equal(wfCheck("Fake-Net", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", false), "");
  assert.match(wfCheck("", "not-a-real-pass", false), /1-32 bytes/);
  assert.match(wfCheck("x".repeat(33), "not-a-real-pass", false), /1-32 bytes/);
  assert.match(wfCheck("ééééééééééééééééé", "not-a-real-pass", false), /1-32 bytes/);   // 34 bytes
  assert.match(wfCheck("Fake-Net", "short", false), /8-63/);
  assert.match(wfCheck("Fake-Net", "x".repeat(64), false), /8-63/);
  assert.equal(wfCheck("Fake-Net", "", true), "");
  assert.match(wfCheck("Fake-Net", "not-a-real-pass", true), /open network takes no password/);
  assert.equal(page.includes('value="') && /id="wf-psk"[^>]*value=/.test(page), false, "the password field is never pre-filled");
});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

test("the /wifi/* flow against the mock", async (t) => {
  // MOCK_PAIR=0 is cmdline.txt paircode=0: the pairing code is out of the way here, because this test is about
  // something else. The gate itself (every endpoint it covers, refusals, the lockout) is page_pair.test.mjs.
  const port = 19300 + Math.floor(Math.random() * 500);
  const srv = spawn("python3", [join(HERE, "mock_web.py"), String(port)], { stdio: "ignore", env: { ...process.env, MOCK_TRIAL: "1", MOCK_PAIR: "0" } });
  t.after(() => srv.kill());
  const base = `http://127.0.0.1:${port}`, origin = { Origin: base };
  const get = async () => (await fetch(base + "/wifi/status")).json();
  const post = async (op, f, h = origin) => (await fetch(base + "/wifi/" + op, { method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded", ...h }, body: new URLSearchParams(f).toString() })).json();
  let w;
  for (let i = 0; i < 50 && !w; i++) { try { w = await get(); } catch { await sleep(100); } }
  assert.ok(w, "mock_web answers");
  assert.equal(w.on, false);
  assert.equal(w.net, "ethernet");
  assert.equal((await post("power", { on: "1" }, {})).code, 403, "no Origin: refused");
  assert.equal((await post("power", { on: "1" }, { Origin: "http://evil.example" })).code, 403);
  assert.equal((await post("join", { ssid: "Fake-Studio", psk: "not-a-real-pass" }, { Referer: "http://evil.example/" })).code, 403);
  assert.equal((await (await fetch(base + "/wifi/scan")).json()).ok, false, "off: no scan");
  assert.equal((await post("power", { on: "1" })).ok, true);
  assert.equal((await (await fetch(base + "/wifi/scan")).json()).ok, true);
  await sleep(2200);
  w = await get();
  assert.ok(w.networks.length >= 3 && w.networks.every((n) => n.ssid.startsWith("Fake-")));
  const st = await (await fetch(base + "/status")).json();
  assert.equal(typeof st.wifi.networks, "number", "/status: a count, no names");
  assert.ok(!JSON.stringify(st).includes("Fake-"), "/status has no SSID");
  // validation: the error never quotes the input
  const bad = await post("join", { ssid: "Fake-Studio", psk: "short" });
  assert.equal(bad.ok, false);
  assert.ok(!bad.error.includes("short"));
  // a wrong password: restarts (503), tries, fails, the previous setting back
  assert.equal((await post("join", { ssid: "Fake-Studio", psk: "wrong-pass" })).ok, true);
  assert.equal((await fetch(base + "/wifi/status")).status, 503, "restarting");
  await sleep(4500);
  w = await get();
  assert.equal(w.join.state, "failed");
  assert.equal(w.join.reason, "wrong_password");
  assert.equal(w.ssid, null, "reverted");
  // a good one
  assert.equal((await post("join", { ssid: "Fake-Studio", psk: "not-a-real-pass" })).ok, true);
  await sleep(4500);
  w = await get();
  assert.equal(w.join.state, "up");
  assert.equal(w.net, "wifi");
  assert.equal(w.ssid, "Fake-Studio");
  assert.ok(!JSON.stringify(w).includes("not-a-real-pass"), "the password never comes back");
  const log = await (await fetch(base + "/log?since=0")).text();
  assert.ok(!log.includes("not-a-real-pass") && !log.includes("Fake-Studio"), "nor in the log, nor the SSID");
  assert.equal((await post("forget", {})).ok, true);
  w = await get();
  assert.equal(w.ssid, null);
});
