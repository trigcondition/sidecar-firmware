// node web/lfo.test.mjs - the LFO tab's pure parts (lfo.js): the tables against core/lfo.cpp's, the design's hex both
// ways and its curve (core/lfo.cpp design_at), drawing by drag, the rate labels, the parameter lists; the mock device's
// ':lfo' answers (mockdev.js, as core/lfo.cpp's command / json).
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { LFO_WAVES, LFO_TARGETS, LFO_DIVS, LFO_SHAPES, designDecode, designEncode, designAt,
  dragStep, lfoParams, lfoRateText, lfoHz } from "./lfo.js";
import { MockDevice } from "./mockdev.js";
import { parseReply } from "./proto.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const core = readFileSync(join(HERE, "..", "core", "lfo.cpp"), "utf8");
const names = (start, end) => [...core.slice(core.indexOf(start), core.indexOf(end, core.indexOf(start))).matchAll(/"([^"]+)"/g)].map((m) => m[1]);
assert.deepEqual(LFO_TARGETS, names("kTargetNames[kTargets][4] = {", "};"));
assert.deepEqual(LFO_WAVES, names("kWaveNames[kWaves][7] = {", "};"));
assert.deepEqual(LFO_DIVS, names("kDivNames[kDivs][6] = {", "};"));
assert.deepEqual(LFO_SHAPES, names("kShapeNames[kShapes][5] = {", "};"));

// the design: hex both ways, the curve (HOLD, RAMP, SMTH), drawing
const hex = "000111222333444555666777888999aaabbbcccdddeeefff";
const d = designDecode(hex);
assert.deepEqual(d.slice(0, 2), [{ v: -64, s: 0 }, { v: 17 - 64, s: 1 }]);
assert.equal(d[15].v, 63);   // 0xff: clamped as the core does
assert.equal(designEncode(designDecode("0011012013014015016017017f1701601501401301201101")), "0011012013014015016017017f1701601501401301201101");
const s = Array.from({ length: 16 }, () => ({ v: 0, s: 0 }));
s[0] = { v: -64, s: 0 }; s[1] = { v: 63, s: 1 }; s[2] = { v: 0, s: 2 }; s[3] = { v: 32, s: 0 };
const st = 1 / 16;
assert.equal(designAt(s, 0.5 * st), -1);
assert.ok(Math.abs(designAt(s, 1.5 * st) - 63 / 128) < 0.03);
assert.ok(Math.abs(designAt(s, 2.5 * st) - 0.25) < 0.01 && designAt(s, 2.1 * st) < 0.05);
assert.equal(dragStep(s, 319, 0, 320, 96, 2), 15);
assert.deepEqual(s[15], { v: 63, s: 2 });   // the top: +63, the brush's shape
assert.equal(dragStep(s, 0, 96, 320, 96), 0);
assert.deepEqual(s[0], { v: -64, s: 0 });   // the bottom, the step's own shape kept
assert.equal(dragStep(s, 100, 48, 320, 96, 1), 5);
assert.deepEqual(s[5], { v: 0, s: 1 });
assert.ok(designEncode(s).length === 48 && ("lfo=8:des:" + designEncode(s)).length <= 63, "one console line");

// rates, parameter lists
assert.equal(lfoRateText(1, 8), "1/4");
assert.equal(lfoRateText(0, 127), "20 Hz");
assert.equal(lfoRateText(0, 0), "0.02 Hz");
assert.ok(Math.abs(lfoHz(64) - 0.65) < 0.01);
assert.deepEqual(lfoParams("OFF", 0, "-"), []);
assert.equal(lfoParams("V1", 1, "HRM")[1], "P2 HRM");
assert.equal(lfoParams("V1", 1, "HRM").length, 16);
assert.deepEqual(lfoParams("FX2", 0, "TIME"), ["P2 TIME", "P3", "P4", "P5"]);
assert.deepEqual(lfoParams("BD", 0, "F.FRQ"), [], "no Rytm targets (1026)");
assert.ok(!LFO_TARGETS.includes("BD") && LFO_TARGETS.at(-1) === "O8" && LFO_TARGETS.indexOf("O1") === 22);
assert.deepEqual(lfoParams("O3", 1, "PAN"), ["LEV", "PAN", "WID"], "the OUT route's targets (docs/STEREO.md)");
// ins1112: an INSERT chain's 12 params - both slots, then the POST EQ after them
assert.equal(lfoParams("I3", 0, "ACUT").length, 12);
assert.equal(lfoParams("I3", 0, "ACUT")[0], "A P1 ACUT");
assert.deepEqual(lfoParams("B1", 9, "QLPF").slice(8), ["EQ HPF", "EQ LPF QLPF", "EQ LOW", "EQ HIGH"]);

// the mock device: settings kept, the JSON as the core's
{
  const replies = [];
  const dev = new MockDevice((l) => { const o = parseReply(l); if (o) replies.push(o); }, { rate: 0 });
  const cmd = (c) => { dev.feed(new TextEncoder().encode(":" + c + "\n")); return replies.filter((o) => o.t === "lfo").at(-1); };
  let r = cmd("lfo");
  assert.equal(r.lfo.n, 4);
  assert.equal(r.lfo.l[0].tgt, "OFF");
  r = cmd("lfo=2:tgt:bd");
  assert.equal(r.lfo.l[1].tgt, "OFF", "BD: no target (the device says cannot)");
  r = cmd("lfo=2:tgt:fx1");
  assert.equal(r.lfo.l[1].tgt, "FX1"); assert.equal(r.lfo.l[1].np, 4); assert.equal(r.lfo.l[1].pn, "P2");
  r = cmd("lfo=2:pre:steps");
  assert.equal(r.lfo.l[1].wav, 7);
  assert.equal(designDecode(r.lfo.l[1].des)[1].v, -16);
  r = cmd("lfo=2:sync:1");
  assert.equal(r.lfo.l[1].sync, 1); assert.equal(r.lfo.l[1].rate, 8);
  r = cmd("lfo=2:des:" + hex);
  assert.equal(r.lfo.l[1].des, hex);
  r = cmd("lfo=2:off");
  assert.equal(r.lfo.l[1].tgt, "OFF");
}
console.log("lfo.test: PASS");
