// pi/engine/tests/page_fxbw.test.mjs - the Sidecar page's Send FX tab: each send FX's return filter as BASE / WIDTH
// (sfx-basewidth, 0.5.0; /status "ins" c[8], c[9] "bw"), its sliders' commands. node --test pi/engine/tests/page_fxbw.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("function fxbwHtml"), b = page.indexOf("let fxbwLast");
assert.ok(a > 0 && b > a, "the section's code in index.html");
const { fxbwHtml } = new Function(page.slice(a, b) + "; return { fxbwHtml };")();
const c = [];
c[8] = { bw: [40, 30], peq: [40, 57, 64, 64] };
c[9] = { bw: [0, 127], peq: [0, 0, 64, 64] };

test("the wiring: the box in the Send FX tab, /status feeds it", () => {
  assert.match(page, /id="fxbw"/);
  assert.match(page, /showFxBw\(st\.ins\)/);
});
test("each FX: BASE and WIDTH sliders on ins=Bn:eq:base / width, the filter it makes", () => {
  const h = fxbwHtml(c);
  assert.match(h, /value="40" data-bw="B1:eq:base:"/);
  assert.match(h, /value="30" data-bw="B1:eq:width:"/);
  assert.match(h, /value="0" data-bw="B2:eq:base:"/);
  assert.match(h, /value="127" data-bw="B2:eq:width:"/);
  assert.match(h, /FX 1 filter<\/b> <span class="mut">· HPF 40 · LPF 57/);
  assert.match(h, /FX 2 filter<\/b> <span class="mut">· off/);
});
