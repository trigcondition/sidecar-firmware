// pi/engine/tests/page_rec.test.mjs - the Record tab's destination (pi/engine/www/index.html, recVolChoices /
// recTakeRoots): the SD and each mounted drive to record to, the chosen one kept while it is not mounted, the takes listed
// from the SD's /Recordings and each mounted drive's /USB/Recordings (fsweb.h). node --test pi/engine/tests/page_rec.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("function recVolChoices"), b = page.indexOf("$(\"#rec-ch\").innerHTML");
assert.ok(a > 0 && b > a, "the destination helpers in index.html");
const F = new Function(page.slice(a, b) + "; return { recVolChoices, recTakeRoots };")();
const vols = (o) => ["SD", "USB", "USB2", "USB3", "NVME"].map((name) => ({ name, state: o[name] || "absent" }));

test("the SD always; a mounted drive; the chosen one marked when it is not mounted", () => {
  assert.deepEqual(F.recVolChoices(vols({ SD: "mounted" }), "SD"), [{ n: "SD", ok: true }]);
  assert.deepEqual(F.recVolChoices(vols({ USB: "mounted", NVME: "unavailable" }), "SD"), [{ n: "SD", ok: true }, { n: "USB", ok: true }]);
  assert.deepEqual(F.recVolChoices(vols({ USB: "ejected" }), "USB"), [{ n: "SD", ok: true }, { n: "USB", ok: false }]);
  assert.deepEqual(F.recVolChoices([], "SD"), [{ n: "SD", ok: true }]);   // no /status "stor" yet
});

test("the takes: the SD's and each mounted drive's, by its /fs path and id prefix", () => {
  assert.deepEqual(F.recTakeRoots(vols({ USB: "mounted", USB2: "present" })), [["SD", "/Recordings", ""], ["USB", "/USB/Recordings", "USB:"]]);
  assert.deepEqual(F.recTakeRoots(undefined), [["SD", "/Recordings", ""]]);
});

// 5d: the warning before a take, the take names' hint, the lists' order (by name or date, newest first), the clock
const wa = page.indexOf("function recLoad"), wb = page.indexOf("function recSave");   // (natCmp, byKey between)
assert.ok(wa > 0 && wb > wa, "the 5d helpers in index.html");
const G = new Function("store", page.slice(wa, wb) + "; return { recLoad, recWarn, recWarnText, recNameHint, byKey, sortNext, sortLoad, SORT0 };")(() => null);
test("the warning: more than 70 % of the destination's speed; none without a speed", () => {
  assert.equal(G.recWarn(4218, 5120), 82);      // 25 channels, 24-bit, on the SD that dropped blocks
  assert.equal(G.recWarn(1688, 5120), 0);       // the 12 IN channels
  assert.equal(G.recWarn(3584, 5120), 0);       // 70 %: fits
  assert.equal(G.recWarn(3585, 5120), 70);      // just over (rounded)
  assert.equal(G.recWarn(4218, 0), 0);
  assert.equal(G.recWarn(4218, undefined), 0);
  assert.equal(G.recLoad(4218, 5120), 82);
  const t = G.recWarnText(82, 4218, { vol: "SD", speedKBps: 5120, speedMeasured: 1 });
  assert.match(t, /4\.1 MB\/s: 82 % of the SD card's 5\.0 MB\/s \(measured/);
  assert.match(G.recWarnText(90, 5530, { vol: "USB", speedKBps: 6144, speedMeasured: 0 }), /the USB drive's 6\.0 MB\/s \(not measured/);
});
test("the take name's hint: the project's name unless one is typed, the drive's folder", () => {
  assert.match(G.recNameHint("", "SD"), /^<project>_2026-10-27_1432 .* in \/Recordings$/);
  assert.match(G.recNameHint("live", "USB"), /^live_.* in USB:\/Recordings$/);
});
test("the lists' order: date newest first by default, ties by name; none-dated last; by name either way", () => {
  const L = [{ n: "b", t: "2026-10-27 10:00" }, { n: "a", t: "2026-10-27 10:00" }, { n: "C", t: "" }, { n: "d", t: "2026-10-28 09:00" }];
  const names = (k, d) => [...L].sort(G.byKey(k, d)).map((x) => x.n).join("");
  assert.equal(names("t", -1), "dbaC");
  assert.equal(names("t", 1), "Cabd");
  assert.equal(names("n", 1), "abCd");   // any case
  assert.equal(names("n", -1), "dCba");
  const T = [{ name: "x_take-002", date: "" }, { name: "x_take-010", date: "" }];   // no clock: the names
  assert.deepEqual([...T].sort(G.byKey("t", -1, (x) => x.name, (x) => x.date)).map((x) => x.name), ["x_take-010", "x_take-002"]);
  const M = [{ n: "j_2026-10-27_1432_2", t: "2026-10-27 14:32" }, { n: "j_2026-10-27_1432_10", t: "2026-10-27 14:32" },
             { n: "j_2026-10-27_1432", t: "2026-10-27 14:32" }];   // the same minute: _10 after _2, newest first
  assert.deepEqual([...M].sort(G.byKey("t", -1)).map((x) => x.n), ["j_2026-10-27_1432_10", "j_2026-10-27_1432_2", "j_2026-10-27_1432"]);
  assert.deepEqual([...M].sort(G.byKey("n", 1)).map((x) => x.n), ["j_2026-10-27_1432", "j_2026-10-27_1432_2", "j_2026-10-27_1432_10"]);
  assert.deepEqual(G.SORT0, { k: "t", d: -1 });
  assert.deepEqual(G.sortNext({ k: "t", d: -1 }, "t"), { k: "t", d: 1 });
  assert.deepEqual(G.sortNext({ k: "t", d: -1 }, "n"), { k: "n", d: 1 });
  assert.deepEqual(G.sortNext({ k: "n", d: 1 }, "t"), { k: "t", d: -1 });
  assert.deepEqual(G.sortLoad("x"), { k: "t", d: -1 });   // nothing stored: newest first
});
test("the page's clock to the Pi: the epoch and the zone (minutes east of UTC)", () => {
  const ta = page.indexOf("const timeForm"), tb = page.indexOf("function sendTime");
  assert.ok(ta > 0 && tb > ta);
  const timeForm = new Function(page.slice(ta, tb) + "; return timeForm;")();
  assert.deepEqual(timeForm(1793111579123, -60), { epoch: 1793111579, tz: 60 });     // CET
  assert.deepEqual(timeForm(1793111579999, 300), { epoch: 1793111579, tz: -300 });   // EST
});

// recPush: what the page sends to the Pi. On opening (the remembered channels / bits / name: recPush()) never the
// destination - the Pi's destination is adopted from /status; rec=vol: only when the page's own picker is clicked.
const pa = page.indexOf("async function recPush"), pb = page.indexOf("function recView");
assert.ok(pa > 0 && pb > pa, "recPush in index.html");
function pushWith(name = "take") {
  const sent = [];
  const env = { cmd: async (c) => { sent.push(c); return true; }, recBusy: () => false, recIds: () => "0", recBits: 24,
    recVol: "USB", $: () => ({ value: name }), pend: { fail() {} } };
  const f = new Function(...Object.keys(env), page.slice(pa, pb) + "; return recPush;")(...Object.values(env));
  return { f, sent };
}
test("opening the page never re-points the destination; a click does", async () => {
  const a = pushWith();
  await a.f();
  assert.ok(a.sent.length === 3 && !a.sent.some((c) => c.startsWith("rec=vol:")), a.sent.join(" "));
  const b = pushWith();
  await b.f("vol");
  assert.deepEqual(b.sent, ["rec=vol:USB"]);
});
test("an empty take name: the project's name (rec=name: alone)", async () => {
  const a = pushWith("");
  await a.f("name");
  assert.deepEqual(a.sent, ["rec=name:"]);
  const b = pushWith("bad name!");
  await b.f("name");
  assert.deepEqual(b.sent, []);
});
test("the destination is not kept in the browser", () => {
  const save = page.slice(page.indexOf("function recSave"), page.indexOf("function recBusy"));
  assert.ok(save.length > 0 && !/vol/.test(save));
});
