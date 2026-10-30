// pi/engine/tests/page_seqx.test.mjs - the Sidecar page's Sequencer section (seqtop): the markup, the console lines each
// Set sends (core/seq's seq=key / seed / spd / dir / rnd / smod / rtrg / fx / rndstep / lcond / slide), the rows drawn
// from /status "seqx" and the step editor filled from a step's extras.
// node --test pi/engine/tests/page_seqx.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const raw = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");

test("the section and its /status wiring", () => {
  assert.match(raw, /<section id="seqx-sec">/);
  for (const id of ["seqx-pat", "seqx-rows", "sx-t", "sx-s", "sx-rm", "sx-f1", "sx-f2", "sx-rnd", "sx-lp", "sx-lc", "sx-sl", "sx-set"])
    assert.match(raw, new RegExp('id="' + id + '"'), id);
  assert.match(raw, /showSeqx\(st\);/, "/status feeds the section");
});

function load() {
  const a = raw.indexOf("const SX_KEYS");
  const b = raw.indexOf("$(\"#sx-rm\").innerHTML");
  assert.ok(a > 0 && b > a);
  const el = {};
  const $ = (s) => (el[s] ||= { innerHTML: "", textContent: "", value: "", checked: false });
  const document = { activeElement: null };
  return { ...new Function("$", "document", raw.slice(a, b) + "; return { seqxPatLines, seqxTrackLines, seqxStepLines, showSeqx, seqxFillStep };")($, document), el };
}

test("the pattern's lines: key / scale / QNT, the seed (or FREE)", () => {
  const { seqxPatLines } = load();
  assert.deepEqual(seqxPatLines({ key: 2, scale: 3, qnt: 2, seed: 41, free: false }), ["seq=key:D:DOR:play", "seq=seed:41"]);
  assert.deepEqual(seqxPatLines({ key: 5, scale: 0, qnt: 0, seed: 7, free: true }), ["seq=key:F:CHR:off", "seq=seed:free"]);
  assert.deepEqual(seqxPatLines({ key: 30, scale: 99, qnt: 9, seed: 123456, free: false }), ["seq=key:B:WHOLE:play", "seq=seed:9999"]);
});

test("a track's lines: speed, direction, random notes, SEED MOD (clamped)", () => {
  const { seqxTrackLines } = load();
  assert.deepEqual(seqxTrackLines(3, { spd: 4, dir: 5, rm: 2, rl: 50, ro: 3, rw: 2, ms: 9, mr: 8, mq: 2, ma: 1 }),
    ["seq=spd:3:3/4", "seq=dir:3:brown", "seq=rnd:3:mode=track,low=50,oct=3,wght=chord", "seq=smod:3:src=9,range=8,quant=bar,aff=chance"]);
  assert.deepEqual(seqxTrackLines(1, { spd: 0, dir: 0, rm: 0, rl: 999, ro: 0, rw: 0, ms: 0, mr: 1, mq: 1, ma: 0 })[2], "seq=rnd:1:mode=off,low=127,oct=1,wght=flat");
});

test("a step's lines: ratchet / arp, the FX columns, random notes, a lock's condition and slide", () => {
  const { seqxStepLines } = load();
  const l = seqxStepLines(2, 5, { rm: 4, rc: 0, rr: 6, rv: 1, rp: -3, ro: 2, rg: 50, f1: 9, v1: "9", f2: 4, v2: "30", rnd: true, lp: 3, lc: "1:4", sl: true });
  assert.deepEqual(l, ["seq=rtrg:2:5:cnt=0,rate=1/4,vcrv=up,pcrv=-3,mode=updn,oct=2,gate=50", "seq=fx:2:5:1:JMP:9",
    "seq=fx:2:5:2:CHN:30", "seq=rndstep:2:5:1", "seq=lcond:2:5:3:1:4", "seq=slide:2:5:3:1"]);
  const o = seqxStepLines(1, 1, { rm: -1, f1: 0, f2: 0, rnd: false, lp: 0 });
  assert.deepEqual(o, ["seq=rtrg:1:1:off", "seq=fx:1:1:1:off", "seq=fx:1:1:2:off", "seq=rndstep:1:1:0"]);
  // the lock's condition and slide as loaded: not sent again (a ratchet change must not clear them)
  const w = seqxStepLines(1, 1, { rm: -1, f1: 0, f2: 0, rnd: false, lp: 2, lc: "1:4", sl: true }, { lp: 2, lc: "1:4", sl: true });
  assert.ok(!w.some((x) => x.includes("lcond") || x.includes("slide")));
});

test("rows from /status seqx, the step editor from a step's extras", () => {
  const { showSeqx, el } = load();
  el["#sx-t"] = { innerHTML: "", value: "1" };
  el["#sx-s"] = { value: "3" };
  showSeqx({ seqx: { key: 2, scale: 3, qnt: 1, seed: 41, fixed: 1, t: [{ t: 1, spd: 4, dir: 2, rnd: [1, 48, 3, 0], smod: [9, 16, 1, 0], off: 3,
    x: [{ s: 3, rt: [4, 0, 1, 0, 0, 0, 0], fx: [[7, -1], [0, 0]], r: 1, lc: [[2, 14]], sl: [2] }] }] } });
  assert.match(el["#seqx-pat"].innerHTML, /<option value="2" selected>D<\/option>/);
  assert.match(el["#seqx-pat"].innerHTML, /<option value="3" selected>DOR<\/option>/);
  const h = el["#seqx-rows"].innerHTML;
  assert.match(h, /data-t="1"/);
  assert.match(h, /<option value="4" selected>3\/4<\/option>/);
  assert.match(h, /<option value="2" selected>PEND<\/option>/);
  assert.match(h, /seed \+3/);
  assert.equal(el["#sx-rc"].value, 4);
  assert.equal(el["#sx-rm"].value, 0);   // CHORD (the ratchet on)
  assert.equal(el["#sx-f1"].value, 7);
  assert.equal(el["#sx-v1"].value, "-1");
  assert.equal(el["#sx-lc"].value, "1:4");   // (code 14: A:B 1:4)
  assert.equal(el["#sx-rnd"].checked, true);
  assert.equal(el["#sx-lp"].value, 2);
  assert.equal(el["#sx-sl"].checked, true);
  showSeqx({ seqx: { key: 0, scale: 0, qnt: 0, seed: 0, fixed: 0, t: [] } });
  assert.match(el["#seqx-rows"].innerHTML, /No seq track/);
});
