// pi/engine/tests/page_um.test.mjs - the Sidecar page's user machines list (pi/engine/www/index.html, User machines):
// the API column (Plugin API 2 or 3, /um's "api") next to Ver, effects marked, effects never offered for a U slot,
// against mock_web.py's /um data. node --test pi/engine/tests/page_um.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const e0 = page.indexOf("const esc = "), e1 = page.indexOf("\n", e0);
const a = page.indexOf("function umApi("), b = page.indexOf("function renderUm(");
assert.ok(e0 > 0 && a > 0 && b > a, "esc and umApi in index.html");
const F = new Function(page.slice(e0, e1) + "\n" + page.slice(a, b) + "; return { umApi };")();

test("the API cell: 2, 3, an effect marked, none", () => {
  assert.equal(F.umApi({ api: 2 }), "2");
  assert.equal(F.umApi({ api: 3 }), "3");
  const fx = F.umApi({ api: 3, effect: true });
  assert.match(fx, /^3 <span class="badge"[^>]*>effect<\/span>$/);
  assert.equal(F.umApi({}), "-");
});

test("the table: API next to Ver, every row as wide as the head, effects not offered for a slot", () => {
  const head = page.slice(page.indexOf("<th>Name</th>"), page.indexOf('<tbody id="um-list">'));
  const ths = head.match(/<th[^>]*>[^<]*<\/th>/g).map((t) => t.replace(/<[^>]+>/g, ""));
  assert.deepEqual(ths.slice(0, 5), ["Name", "ID", "Ver", "API", "File"]);
  const r = page.slice(page.indexOf('$("#um-list").innerHTML = P.length'), page.indexOf("let slots = "));
  const tds = r.slice(r.indexOf("return `<tr>"), r.indexOf("</tr>`;")).match(/<td[ >]/g).length;
  assert.equal(tds, ths.length, "a row's cells");
  assert.ok(r.includes("<td>${umApi(p)}</td>") && r.indexOf("${esc(p.ver)}") < r.indexOf("umApi(p)"));
  for (const m of page.matchAll(/id="um-list"[\s\S]*?colspan="(\d+)"/g)) assert.equal(Number(m[1]), ths.length);
  assert.ok(r.includes(`colspan="${ths.length}"`));
  assert.ok(page.includes("if (p.fit && !p.effect) { const sel"), "the slot selects skip effects");
});

test("mock_web.py's /um: an API 3 machine and an effect next to the API 2 ones", () => {
  const out = execFileSync("python3", ["-c",
    "import json, sys; sys.path.insert(0, '.'); import mock_web; print(json.dumps(mock_web.um()))"], { cwd: HERE }).toString();
  const P = JSON.parse(out).filter((x) => x.t === "plugin");
  assert.ok(P.some((p) => p.api === 2));
  assert.ok(P.some((p) => p.id === "rytm.api3_test" && p.api === 3 && !p.effect));
  assert.ok(P.some((p) => p.id === "fx.pingpong" && p.api === 3 && p.effect === true));
  assert.equal(F.umApi(P.find((p) => p.id === "fx.pingpong")).startsWith("3 <span"), true);
});
