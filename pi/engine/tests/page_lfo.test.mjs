// pi/engine/tests/page_lfo.test.mjs - the Sidecar page's LFO tab: web/seg.js and web/lfo.js put in place of the page's
// include lines (as build.sh and mock_web.py do), the result a script that runs, the tab and its /status wiring.
// node --test pi/engine/tests/page_lfo.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const raw = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const page = raw.split("\n").map((l) => {
  const m = /^\s*\/\*@include (\S+)@\*\/\s*$/.exec(l);
  return m ? readFileSync(join(REPO, m[1]), "utf8").replace(/^export /gm, "") : l;
}).join("\n");

test("the includes and the tab", () => {
  assert.match(raw, /\/\*@include web\/seg\.js@\*\//);
  assert.match(raw, /\/\*@include web\/lfo\.js@\*\//);
  assert.ok(!/^export /m.test(page), "no export left");
  assert.match(page, /class Pending/);
  assert.match(page, /function initLfo/);
  assert.match(raw, /data-tab="lfo">LFO<\/a>/);
  assert.match(raw, /id="lfo-root"/);
  assert.match(raw, /if \(st\.lfo\) lfoUi\.show\(st\.lfo\)/, "/status \"lfo\" feeds the tab");
  const build = readFileSync(join(HERE, "..", "build.sh"), "utf8");
  assert.match(build, /@include/, "build.sh expands the includes");
});

test("the inlined code runs: a card per LFO from /status's JSON, a click sends one console line", () => {
  const a = page.indexOf("/*@include") >= 0 ? -1 : page.indexOf("// Choice groups (segmented toggles)");
  const b = page.indexOf("function initLfo");
  assert.ok(a > 0 && b > a);
  const end = page.indexOf("\n}\n", b) + 3;
  const sent = [];
  // a DOM just big enough: root.innerHTML is kept, queries answer from a tiny model
  const listeners = {};
  const root = { innerHTML: "", addEventListener: (k, f) => { listeners[k] = f; }, querySelectorAll: () => [], querySelector: () => null };
  const initLfo = new Function("document", "getComputedStyle", page.slice(a, end) + "; return { initLfo, Pending };")(
    { activeElement: null }, () => ({ color: "#fff" }));
  const ui = initLfo.initLfo({ root, send: (l) => sent.push(l), Pending: initLfo.Pending });
  ui.show({ n: 8, l: Array.from({ length: 8 }, () => ({ tgt: "OFF", par: 0, wav: 0, rate: 64, sync: 0, trig: 0, dep: 32, base: 64, des: "0".repeat(48), ph: 0 })) });
  assert.equal((root.innerHTML.match(/class="lfo"/g) || []).length, 8, "eight cards on the Pi");
  assert.match(root.innerHTML, /data-pre="steps"/);
  const btn = { dataset: { v: "7" }, closest: (q) => (q === "button" ? btn : q.startsWith(".seg") ? { dataset: { lfo: "3", k: "wav" } } : null) };
  listeners.click({ target: btn });
  assert.deepEqual(sent, ["lfo=3:wav:7"]);
  listeners.change({ target: { dataset: { lfo: "2", k: "tgt" }, value: "FX1" } });
  assert.deepEqual(sent.at(-1), "lfo=2:tgt:fx1");
  assert.ok(!/<option>BD<\/option>/.test(root.innerHTML), "no Rytm targets (1026)");
});

test("the Pi's 26: one track's pair at a time, B without Rytm targets, the link-full line, the Rytm's track followed", () => {
  const a = page.indexOf("// Choice groups (segmented toggles)");
  const b = page.indexOf("function initLfo");
  const end = page.indexOf("\n}\n", b) + 3;
  const sent = [];
  const listeners = {};
  const root = { innerHTML: "", addEventListener: (k, f) => { listeners[k] = f; }, querySelectorAll: () => [], querySelector: () => null };
  const m = new Function("document", "getComputedStyle", page.slice(a, end) + "; return { initLfo, Pending, LFO_FULL };")(
    { activeElement: null }, () => ({ color: "#fff" }));
  const ui = m.initLfo({ root, send: (l) => sent.push(l), Pending: m.Pending });
  const l = Array.from({ length: 26 }, () => ({ tgt: "OFF", par: 0, wav: 0, rate: 64, sync: 0, trig: 0, dep: 32, base: 64, des: "0".repeat(48), ph: 0 }));
  ui.show({ n: 26, pt: 1, kit: 1, full: 0, trk: 4, l });
  const cards = root.innerHTML.match(/<section class="lfo"[^>]*>/g) || [];
  assert.equal(cards.length, 26);
  assert.equal(cards.filter((c) => !/ hidden/.test(c)).length, 2, "one track's pair shown");
  assert.match(root.innerHTML, /<section class="lfo" data-i="9"><h2>T5 LFO A /, "the Rytm's selected track (trk 4): T5");
  assert.match(root.innerHTML, /<section class="lfo" data-i="10"><h2>T5 LFO B /);
  assert.match(root.innerHTML, /data-k="lfotrk"/);
  assert.ok(root.innerHTML.includes(m.LFO_FULL) && /data-lfofull role="status" hidden/.test(root.innerHTML));
  const sel = (i) => root.innerHTML.slice(root.innerHTML.indexOf(`<select data-lfo="${i}" data-k="tgt">`)).split("</select>")[0];
  for (const i of [9, 10])   // 0.5.0 owner test: no LFO on the Rytm, A or B
    assert.ok(!/<option>RT|<option>RFX/.test(sel(i)) && /<option>X8<\/option>/.test(sel(i)), "the Sidecar's only");
  // the track bar is a view choice, never a command
  const btn = { dataset: { v: "12" }, closest: (q) => (q === "button" ? btn : q.startsWith(".seg") ? { dataset: { k: "lfotrk" } } : null) };
  listeners.click({ target: btn });
  assert.equal(sent.length, 0);
  assert.equal(ui.track(), 12);
  // the Rytm selects another track: followed; the same track again: the viewer's choice kept
  ui.show({ n: 26, pt: 1, kit: 1, full: 1, trk: 4, l });
  assert.equal(ui.track(), 12);
  ui.show({ n: 26, pt: 1, kit: 1, full: 1, trk: 7, l });
  assert.equal(ui.track(), 7);
  listeners.change({ target: { dataset: { lfo: "26", k: "tgt" }, value: "MFX" } });
  assert.equal(sent.at(-1), "lfo=26:tgt:mfx");
});
