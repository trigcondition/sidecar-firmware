// node web/lfo.test.mjs - the LFO tab's pure parts (lfo.js): the tables against core/lfo.cpp's, the design's hex both
// ways and its curve (core/lfo.cpp design_at), drawing by drag, the rate labels, the parameter lists; the mock device's
// ':lfo' answers (mockdev.js, as core/lfo.cpp's command / json).
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { LFO_WAVES, LFO_TARGETS, LFO_DIVS, LFO_SHAPES, designDecode, designEncode, designAt,
  dragStep, lfoParams, lfoParamRows, lfoRateText, lfoHz, LFO_FULL, LFO_TRACKS, lfoTrack, lfoSlot, lfoTitle, lfoTargets } from "./lfo.js";
import { MockDevice } from "./mockdev.js";
import { parseReply } from "./proto.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const core = readFileSync(join(HERE, "..", "core", "lfo.cpp"), "utf8");
const names = (start, end) => [...core.slice(core.indexOf(start), core.indexOf(end, core.indexOf(start))).matchAll(/"([^"]+)"/g)].map((m) => m[1]);
assert.deepEqual(LFO_TARGETS, names("kTargetNames[kTargets][5] = {", "};"));
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
// 2026-11: the Pi's pns names every row (the owner: "it just says p1"), "-" an unused one
{
  const pns = ["ENG", "HRM", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV", "FM", "FIN", "AUX", "TMD", "MMD", "VEL", "GLD", "VOL"];
  const l = lfoParams("V2", 0, "ENG", pns);
  assert.deepEqual(l.slice(0, 3), ["P1 ENG", "P2 HRM", "P3 TIM"]);
  assert.equal(l[15], "P16 VOL");
  assert.equal(lfoParams("V1", 2, "TIM", ["ENG", "-", "TIM"])[1], "P2 -");
  assert.deepEqual(lfoParams("FX1", 0, "DRV", ["DRV", "SHPE", "TONE", "MIX"]), ["P2 DRV", "P3 SHPE", "P4 TONE", "P5 MIX"]);
  assert.equal(lfoParams("I1", 0, "ACUT", ["ACUT", "ARES", "AMOD", "ADRV", "B-", "B-", "B-", "B-", "QHPF", "QLPF", "QLOW", "QHIG"])[5], "B P2 -");
}
// 0.5.0 owner test: a voice's PAR list holds the params its machine names (core/lfo.cpp voice_params) - SAMPLER names
// P9..P16 only: no "P1 -".."P8 -" rows (the stored one kept while it is the stored one); a machine naming none: all 16
{
  const smp = ["", "", "", "", "", "", "", "", "TUNE", "FINE", "SMP", "PLAY", "STRT", "END", "LOOP", "LEV"].map((n) => n || "-");
  const rows = lfoParamRows("V1", 8, "TUNE", smp);
  assert.deepEqual(rows.map((r) => r.k), [8, 9, 10, 11, 12, 13, 14, 15]);
  assert.deepEqual(rows.map((r) => r.name), ["P9 TUNE", "P10 FINE", "P11 SMP", "P12 PLAY", "P13 STRT", "P14 END", "P15 LOOP", "P16 LEV"]);
  assert.deepEqual(lfoParamRows("V1", 2, "-", smp).map((r) => r.k), [2, 8, 9, 10, 11, 12, 13, 14, 15]);
  assert.equal(lfoParamRows("V1", 0, "", Array(16).fill("-")).length, 16);
  assert.deepEqual(lfoParamRows("I1", 5, "B-", ["ACUT", "ARES", "AMOD", "ADRV", "B-", "B-", "B-", "B-", "QHPF", "QLPF", "QLOW", "QHIG"]).map((r) => r.k),
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11], "not a voice: every row");
}
assert.deepEqual(lfoParams("BD", 0, "F.FRQ"), [], "no Rytm targets (1026)");
assert.ok(!LFO_TARGETS.includes("BD") && LFO_TARGETS.indexOf("O1") === 22);
// 0.5.0: the Rytm's own sound parameters after X8 (core/lfo.h kTRt 53, kTRfx 65): the words an offset moves, by name
assert.ok(LFO_TARGETS.indexOf("RT1") === 53 && LFO_TARGETS.at(-1) === "RFX" && LFO_TARGETS.length === 66);
assert.equal(lfoParams("RT3", 0, "").length, 43);
assert.equal(lfoParams("RT3", 23, "")[23], "FRQ");
assert.equal(lfoParams("RT1", 4, "TUN")[4], "P1 TUN");
assert.deepEqual(lfoParams("RFX", 0, "").slice(0, 4), ["SPD", "FAD", "DEP", "TIM"]);
assert.equal(lfoParams("RFX", 0, "").length, 25);
assert.deepEqual(lfoParams("RFX", 0, "").slice(21), ["THR", "MUP", "MIX", "CVOL"]);   // M0b: the compressor (appended)
// 0.5.0: the master, the sends, the voice sends after O8 (core/lfo.h kTMaster 30, kTMfx 32, kTSnd 33, kTIsnd 45)
assert.ok(LFO_TARGETS.indexOf("M1") === 30 && LFO_TARGETS.indexOf("MFX") === 32 && LFO_TARGETS.indexOf("S1") === 33 &&
          LFO_TARGETS.indexOf("X1") === 45);
assert.deepEqual(lfoParams("MFX", 0, ""), ["LEV", "MIX1", "MIX2", "MIX3", "MIX4", "B1A", "B1B", "B2A", "B2B", "B3A", "B3B"]);   // 0.5.0: the bus slots' send TYPE MIX
assert.deepEqual(lfoParams("S12", 1, ""), ["F1", "F2"]);
assert.deepEqual(lfoParams("X3", 0, ""), ["B1", "B2", "B3"]);
assert.equal(lfoParams("M2", 0, "").length, 12);
assert.deepEqual(lfoParams("O3", 1, "PAN"), ["LEV", "PAN", "WID"], "the OUT route's targets (docs/STEREO.md)");
// ins1112: an INSERT chain's 12 params - both slots, then the POST EQ after them
assert.equal(lfoParams("I3", 0, "ACUT").length, 12);
assert.equal(lfoParams("I3", 0, "ACUT")[0], "A P1 ACUT");
assert.deepEqual(lfoParams("B1", 9, "QLPF").slice(8), ["EQ HPF", "EQ LPF QLPF", "EQ LOW", "EQ HIGH"]);

// 0.5.0 LFO FINAL: the Pi's 26 are two a track (pt 1): LFO n is track (n-1)/2's A or B; a B has no Rytm target
assert.equal(LFO_FULL, "RYTM LINK FULL: LFOs update slower");
assert.equal(LFO_TRACKS.length, 13);
assert.deepEqual([lfoTrack(1, 1), lfoSlot(1, 1), lfoTrack(26, 1), lfoSlot(26, 1)], [0, 0, 12, 1]);
assert.deepEqual([lfoTitle(1, 1), lfoTitle(6, 1), lfoTitle(25, 1), lfoTitle(26, 1), lfoTitle(3, 0)], ["T1 LFO A", "T3 LFO B", "FX LFO A", "FX LFO B", "LFO 3"]);
// 0.5.0 owner test: no LFO on the Rytm - A and B (and the Mini's) list the Sidecar's targets only (core/lfo.cpp target_ok)
for (const n of [1, 2, 8, 25, 26]) {
  const l = lfoTargets(n, 1);
  assert.ok(!l.some((t) => /^RT|^RFX/.test(t)) && l.includes("MFX") && l.includes("X8") && l.at(-1) === "X8", "the Sidecar's only");
  assert.equal(l.length, LFO_TARGETS.indexOf("RT1"));   // (core/lfo.cpp target_max: the list ends before RT1)
}
assert.deepEqual(lfoTargets(1, 1), lfoTargets(2, 1), "A's list is B's");
assert.deepEqual(lfoTargets(2, 0), lfoTargets(2, 1), "the Mini: no A / B, no Rytm target either");
assert.match(readFileSync(join(HERE, "..", "core", "lfo.h"), "utf8"), /constexpr int kLfos = 26;/);

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
