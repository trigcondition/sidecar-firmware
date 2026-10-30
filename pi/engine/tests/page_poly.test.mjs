// pi/engine/tests/page_poly.test.mjs - the Sidecar page's Poly tracks section (seqx E0): the markup, the console line a
// row's Set sends (seq=poly:T:voices=..,alloc=..,unison=..,detune=..,spread=..; core/inst_alloc.h parse_cfg reads it),
// a row drawn from /status "poly", and the instances line from /status "inst".
// node --test pi/engine/tests/page_poly.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const raw = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");

test("the section and its /status wiring", () => {
  assert.match(raw, /<section id="poly-sec">/);
  assert.match(raw, /id="poly-rows"/);
  assert.match(raw, /id="poly-inst"/);
  assert.match(raw, /showPoly\(st\);/, "/status feeds the section");
});

function load() {
  const a = raw.indexOf("const POLY_ALLOC");
  const b = raw.indexOf("$(\"#poly-rows\").addEventListener");
  assert.ok(a > 0 && b > a);
  const el = {};
  const $ = (s) => (el[s] ||= { innerHTML: "", textContent: "" });
  const document = { activeElement: null };
  return { ...new Function("$", "document", raw.slice(a, b) + "; return { polyLine, polyRow, showPoly };")($, document), el };
}

test("a row's console line: clamped values, the allocation by name", () => {
  const { polyLine } = load();
  assert.equal(polyLine(2, { v: 4, a: 1, u: 2, d: 30, s: 64 }), "seq=poly:2:voices=4,alloc=rotate,unison=2,detune=30,spread=64");
  assert.equal(polyLine(12, { v: 0, a: 0, u: 1, d: 0, s: 0 }), "seq=poly:12:voices=0,alloc=reassign,unison=1,detune=0,spread=0");
  assert.equal(polyLine(1, { v: 99, a: 2, u: 0, d: 500, s: -3 }), "seq=poly:1:voices=8,alloc=reset,unison=1,detune=127,spread=0");
});

test("rows from /status poly, the instances line from inst", () => {
  const { showPoly, el } = load();
  showPoly({
    poly: [{ t: 2, v: 3, a: 1, u: 1, d: 0, s: 0, g: 0, own: 1, info: "PLAITS 3 INST 3 NOTES 2.19%" },
           { t: 5, v: 0, a: 0, u: 0, d: 0, s: 0, g: 0, own: 0, info: "FM 6 INST 6 NOTES 7.32%" }],
    inst: { total: 32, bytes: 36168, noroom: 0, v: [{ path: "inst", on: 3 }, { path: "pool", on: 2 }] },
  });
  const h = el["#poly-rows"].innerHTML;
  assert.match(h, /data-t="2"/);
  assert.match(h, /<option value="3" selected>3<\/option>/);
  assert.match(h, /<option value="1" selected>ROTATE<\/option>/);
  assert.match(h, /PLAITS 3 INST 3 NOTES 2\.19%/);
  assert.match(h, /data-t="5"[\s\S]*<option value="0" selected>AUTO<\/option>[\s\S]*the track's default/);
  assert.equal(el["#poly-inst"].textContent, "Instances 3 of 32, 35 KB each");
  showPoly({ poly: [] });
  assert.match(el["#poly-rows"].innerHTML, /No seq track/);
});
