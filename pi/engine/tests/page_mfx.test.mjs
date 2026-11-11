// pi/engine/tests/page_mfx.test.mjs - the Sidecar page's Master FX section (Send FX tab): what it draws from /status "ins"
// "mfx", the plain text about what the chain holds. node --test pi/engine/tests/page_mfx.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("const MFX_TYPES"), b = page.indexOf("function showMfx");
assert.ok(a > 0 && b > a, "the section's code in index.html");
const { mfxHtml, MFX_NOTE, MFX_TYPES } = new Function(page.slice(a, b) + "; return { mfxHtml, MFX_NOTE, MFX_TYPES };")();
const slot = (t, o = {}) => ({ t, p: [1, 2, 3, 4], l: ["THR", "RAT", "SPD", "GAIN"], v: ["-1DB", "2", "3", "+4DB"], mix: 40, pair: 0, send: 0, ...o });
const m = { byp: 0, src: "ret", lev: 100, trim: -6, gr: 0.0, on: 1, s: [slot("COMP"), slot("AMP", { pair: 1 }), slot("SPACE", { send: 1 }), slot("OFF")], peq: [0, 0, 64, 64] };

test("the page's TYPE list is the engine's, in id order", () => {
  const h = readFileSync(join(HERE, "..", "..", "..", "core", "fx", "insert.cpp"), "utf8");
  // (the table's #if: the build with the send FX's own labels; the #else is the Teensy set's test build, the same names)
  const names = [...h.slice(h.indexOf("kLabels[kAllTypes]"), h.indexOf("#undef FXL")).replace(/#else[\s\S]*?#endif/g, "").matchAll(/^\s*\{"([A-Z]+)", (?:\{|FXL)/gm)].map((x) => x[1]);
  assert.deepEqual(MFX_TYPES, names);
});
test("the wiring: the section is in the tab, /status feeds it", () => {
  assert.match(page, /id="mfx"/);
  assert.match(page, /showMfx\(st\.ins && st\.ins\.mfx\)/);
  assert.match(page, /id="mfx-gr"/);
});
test("a slot's controls follow its TYPE", () => {
  const h = mfxHtml(m);
  assert.match(h, /data-mfx="byp:1"[^>]*>On/);
  assert.match(h, /SRC RET/);
  assert.match(h, /min="-24" max="0" value="-6" data-mfx-v="trim:"/);
  assert.match(h, /data-mfx-v="1:p1:"/);
  assert.match(h, /data-mfx="2:pair:0"[^>]*>L\/R pair/);
  assert.match(h, /data-mfx-v="3:mix:"/);
  assert.doesNotMatch(h, /data-mfx-v="1:mix:"/);
  assert.doesNotMatch(h, /data-mfx-v="4:p1:"/);
});
test("it says what the chain does not hold", () => {
  assert.match(MFX_NOTE.ret, /Rytm's own mix is not in it/);
  assert.match(MFX_NOTE.mix, /ROUTE TO MAIN/);
  assert.match(MFX_NOTE.mix, /outside the chain/);
  assert.match(MFX_NOTE.old, /mono/);
});
test("0.5.0: the send TYPEs' room, shared with the buses: full, a slot without one cannot pick one", () => {
  const full = mfxHtml({ ...m, fxcap: 4, fxused: 4 });
  assert.match(full, /4 play at once, the master and the buses together/);
  assert.match(full, /data-mfx-t="1">[\s\S]*?<option disabled>RINGS</);   // COMP's slot
  assert.doesNotMatch(full.slice(full.indexOf('data-mfx-t="3"'), full.indexOf('data-mfx-t="4"')), /disabled/);   // SPACE's slot holds one
  const free = mfxHtml({ ...m, fxcap: 4, fxused: 3 });
  assert.doesNotMatch(free, /disabled|play at once/);
  assert.doesNotMatch(mfxHtml(m), /disabled|play at once/);   // an older build: no room reported
});
