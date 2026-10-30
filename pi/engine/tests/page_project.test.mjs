// pi/engine/tests/page_project.test.mjs - the Project tab (project bundles, docs/BUNDLES.md): its progress line, the
// projects' table, the import summary (machines to untick, the Rytm step), and the download's service worker (ranges
// in order, a paused answer waited out, a failed range retried). node --test pi/engine/tests/page_project.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("// ---- project bundles"), b = page.indexOf("// ---- end project bundles");
assert.ok(a > 0 && b > a, "the Project tab's script in index.html");
assert.match(page, /data-tab="project">Project</, "the tab");
assert.match(page, /fetch\("\/time", \{ method: "POST"/, "the page sends its clock");

const esc = (t) => String(t).replace(/[&<>"']/g, (c) => "&#" + c.charCodeAt(0) + ";");
const hb = (n) => n + " B";
// only the pure functions: everything from the first "$(" that wires the page is left out
const src = page.slice(a, b);
const pure = src.slice(0, src.indexOf("async function pbJson"));
const fns = new Function("esc", "hb", "E", pure + "; return { pbPhase, pbRows, pbSkip, pbPlanHtml, pbVolHtml, pbBundleRows, pbTrashRows, pbSlotStep, pbSameName };")(esc, hb, encodeURIComponent);

test("the progress line", () => {
  assert.equal(fns.pbPhase({ phase: "IDLE" }), "");
  assert.equal(fns.pbPhase({ phase: "HASH", done: 50, total: 200, waiting: "" }), "HASH 25 %");
  assert.equal(fns.pbPhase({ phase: "STAGE", done: 5000, total: 10000, waiting: "PLAYING" }), "STAGE 50 % · 5000 B of 10000 B · WAITING (PLAYING)");
});

test("the projects: current first, then the last used; the current one has no Delete", () => {
  const rows = fns.pbRows([
    { key: "OLD", rytm: "OLD", used: 1, files: 3, bytes: 10, date: "", current: false, imported: false },
    { key: "A_B", rytm: "A/B", used: 2, files: 1, bytes: 10, date: "2026-10-22T14:03:00Z", current: false, imported: true },
    { key: "LIVE", rytm: "LIVE", used: 0, files: 9, bytes: 10, date: "", current: true, imported: false },
  ]);
  const order = [...rows.matchAll(/data-key="([^"]+)"/g)].map((m) => m[1]);
  assert.deepEqual(order, ["LIVE", "A_B", "OLD"]);
  assert.match(rows, /A\/B <span class="mut">imp\.<\/span> <span class="mut">\(A_B\)<\/span>/);
  assert.match(rows, /2026-10-22 14:03/);
  const live = rows.slice(0, rows.indexOf("</tr>"));
  assert.doesNotMatch(live, /data-act="del"/);
  assert.match(rows.slice(rows.indexOf("</tr>")), /data-act="del"/);
});

test("phase 5: the USB stick - To USB only with a stick, the picker, a stick's bundles by their path", () => {
  const list = [{ key: "LIVE", rytm: "LIVE", used: 0, files: 9, bytes: 10, date: "", current: false, imported: false }];
  assert.doesNotMatch(fns.pbRows(list, false), /data-act="usb"/);
  assert.doesNotMatch(fns.pbRows(list, true), /data-act="usb"/);   // 0.3.0: no export to a stick, with one or without
  // fix1049 (the hardware pass: "only USB offered"): To card is offered with a stick and without
  assert.match(fns.pbRows(list, false), /data-act="card">To card</);
  assert.match(fns.pbRows(list, true), /data-act="card">To card</);
  assert.equal(fns.pbVolHtml({ present: false, vols: [] }), '<span class="mut">no USB stick (FAT32, in a blue port)</span>');
  const v = fns.pbVolHtml({ present: true, vols: [{ drive: "USB:", name: "USB", free: 3000, total: 4000, free_known: true },
                                                  { drive: "USB2:", name: "USB2", free: 0, total: 9, free_known: false }] }, "USB2:");
  assert.match(v, /<select id="pb-vol"><option value="USB:">USB · 3000 B free<\/option><option value="USB2:" selected>USB2 · free not counted<\/option><\/select>/);
  assert.match(v, /<button class="sm" id="pb-eject">Eject<\/button>/);
  const rows = fns.pbBundleRows([{ name: "A.sidecar.zip", path: "USB:/SIDECAR/A.sidecar.zip", size: 5, date: "d", incomplete: false },
                                 { name: "B.sidecar.zip.part", path: "USB:/SIDECAR/B.sidecar.zip.part", size: 1, date: "d", incomplete: true }], "usb");
  assert.match(rows, /data-path="USB:\/SIDECAR\/A\.sidecar\.zip"/);
  assert.match(rows, /data-act="imp">Import</);
  assert.equal((rows.match(/data-act="imp"/g) || []).length, 1, "an incomplete one cannot be imported");
  assert.match(fns.pbBundleRows([], "usb"), /none on the stick/);
  assert.match(fns.pbBundleRows([], "card"), /none yet/);
});

test("the summary: machines with their action, the slots left out, the Rytm step", () => {
  const plan = {
    phase: "READY", name: "AR PERC 2", clash: true, from: { key: "AR PERC", rytm_name: "AR PERC" }, new_bytes: 100, reused_bytes: 900,
    files: { write: 1, reuse: 9, relocated: 0 }, totals: { samples: 10, songs: 1, patterns: 2 },
    machines: [{ slot: 1, id: "sc.grain", ver: "1.0.0", name: "GRAIN", action: "install" },
               { slot: 2, id: "sc.acid", ver: "0.3.1", name: "ACID", action: "conflict" }],
    missing: ["/Samples/gone.wav"], warnings: [], rytm: { file: "rytm/AR PERC.arprj", to: "/Bundles/Rytm/AR PERC 2.arprj", size: 5 },
    step: "Send AR PERC.arprj to the Rytm with Elektron Transfer, then save it on the Rytm as AR PERC 2.",
  };
  const h = fns.pbPlanHtml(plan);
  assert.match(h, /<input id="pb-name" maxlength="23" value="AR PERC 2">/);
  assert.match(h, /data-slot="1" checked> U1 GRAIN 1\.0\.0 <span class="mut">install/);
  assert.match(h, /data-slot="2" checked disabled> U2 ACID/, "a conflict cannot be ticked away (the installed one plays)");
  assert.match(h, /save it on the Rytm as AR PERC 2/);
  assert.match(h, /id="pb-replace"/, "a name clash offers replace");
  assert.doesNotMatch(h, /pb-rytm/, "the Rytm file is offered once imported");
  assert.match(fns.pbPlanHtml({ ...plan, phase: "DONE" }), /id="pb-rytm"/);
  assert.equal(fns.pbSkip(plan.machines, [1]), 1);
  assert.equal(fns.pbSkip(plan.machines, [2, 5]), 2);
  assert.equal(fns.pbSkip(plan.machines, []), 0);
});

test("slot mode (2026-10-29): the slot picker, the Rytm's name for a named slot, replace only for a slot with files, the step", () => {
  const slots = Array.from({ length: 128 }, () => ["", 0, 0]);
  slots[0] = ["LIVE", 4, 1]; slots[46] = ["SDCR01R 03", 12, 0]; slots[4] = ["SDCR01R 03", 0, 0];
  const plan = {
    phase: "READY", name: "SDCR01R 03", key: "@047", clash: true, mode: "new", slot: 47, rytm_list: true, slots,
    from: { key: "@047", rytm_name: "SDCR01R 03" }, new_bytes: 29000, reused_bytes: 105000000,
    files: { write: 0, reuse: 86, relocated: 0 }, totals: { samples: 79, songs: 0, patterns: 16 },
    machines: [], missing: [], warnings: [], rytm: null,
    step: "This bundle holds only the Sidecar's side, no Rytm project file. Slot 047 on the Rytm holds SDCR01R 03: load it on the Rytm.",
  };
  const h = fns.pbPlanHtml(plan);
  assert.match(h, /<span>to slot<\/span><span><select id="pb-slot">/);
  assert.equal((h.match(/<option /g) || []).length, 128);
  assert.match(h, /<option value="47" selected>047 SDCR01R 03 · 12 Sidecar files<\/option>/);
  assert.match(h, /<option value="1" disabled>001 LIVE · 4 Sidecar files · the Rytm's current project<\/option>/, "the current slot cannot be picked");
  assert.match(h, /<option value="2">002 \(empty\)<\/option>/);
  assert.match(h, /id="pb-name" maxlength="23" value="SDCR01R 03" disabled>/, "a slot the Rytm names keeps that name");
  assert.match(h, /<label id="pb-replace-l"><input type="checkbox" id="pb-replace"> replace its Sidecar files \(they go to the project trash, restorable\)/);
  assert.match(h, /only the Sidecar&#39;s side/);
  assert.doesNotMatch(h, /is on the card\)/, "slot mode: no name clash line");
  // an empty slot: no replace offered, the name editable
  const e = fns.pbPlanHtml({ ...plan, slot: 2, clash: false });
  assert.match(e, /<label id="pb-replace-l" hidden>/);
  assert.match(e, /id="pb-name" maxlength="23" value="SDCR01R 03">/);
  // the step as the Sidecar words it, for any slot the picker moves to
  assert.equal(fns.pbSlotStep(plan, 47), plan.step);
  assert.equal(fns.pbSlotStep(plan, 5), "This bundle holds only the Sidecar's side, no Rytm project file. Slot 005 on the Rytm holds SDCR01R 03: load it on the Rytm.");
  assert.match(fns.pbSlotStep(plan, 2), /put the Rytm project into slot 002 yourself \(Elektron Transfer, or SAVE TO 002\) before you import/);
  assert.equal(fns.pbSlotStep({ ...plan, rytm: { file: "rytm/AR PERC.arprj" } }, 9),
               "Send AR PERC.arprj to the Rytm's slot 009 with Elektron Transfer, then load slot 009 on the Rytm.");
  assert.ok(fns.pbSameName("ar perc", "AR PERC") && fns.pbSameName("A VERY LONG NAM", "A VERY LONG NAME 2") && !fns.pbSameName("", "X"));
  // done: the slot as text
  assert.match(fns.pbPlanHtml({ ...plan, phase: "DONE" }), /<span>to slot<\/span><span>047 /);
  // every slot taken (slot 0): no default - "pick a slot" first, nothing else selected
  const none = fns.pbPlanHtml({ ...plan, slot: 0, clash: false });
  assert.match(none, /<select id="pb-slot"><option value="" selected disabled>pick a slot<\/option><option value="1" disabled>/);
  assert.doesNotMatch(none, /<option value="\d+" selected/);
  assert.match(page, /the import goes <b>to a slot<\/b>/, "the help says how slot mode imports");
  // name mode: no picker
  assert.doesNotMatch(fns.pbPlanHtml({ ...plan, slots: undefined }), /pb-slot/);
});

test("the service worker: ranges in order, a pause waited out, a failure retried, the whole file", async () => {
  const sw = readFileSync(join(HERE, "..", "www", "bundle-sw.js"), "utf8");
  const file = new Uint8Array(200000).map((_, i) => (i * 7) & 255);
  let calls = 0, paused = 1, failOnce = true, refuse = false, auth = [];
  // as a real Response: the worker reads X-Sidecar-Pair to tell a pairing refusal from zip bytes (paircode.h)
  const noPair = { get: () => null };
  const fetch = async (url, opt) => {
    calls++;
    auth.push((opt && opt.headers && opt.headers["X-Sidecar-Auth"]) || "");
    const u = new URL(url, "http://sidecar.local");
    const off = +u.searchParams.get("off"), len = +u.searchParams.get("len");
    if (refuse) return { ok: true, status: 200, headers: { get: (k) => (k === "X-Sidecar-Pair" ? "required" : null) },
                         arrayBuffer: async () => new TextEncoder().encode("401 not paired\n").buffer };
    if (off === 65536 && paused-- > 0) return { ok: true, status: 200, headers: noPair, arrayBuffer: async () => new ArrayBuffer(0) };
    if (off === 131072 && failOnce) { failOnce = false; throw new Error("network"); }
    return { ok: true, status: 200, headers: noPair, arrayBuffer: async () => file.slice(off, off + len).buffer };
  };
  const on = {};
  const self = { addEventListener: (k, f) => { on[k] = f; } };
  const module = { exports: {} };
  const setTimeout = (f) => globalThis.setTimeout(f, 0);
  new Function("self", "fetch", "module", "setTimeout", "ReadableStream", "AbortController", "URL", "Response", sw)(
    self, fetch, module, setTimeout, ReadableStream, AbortController, URL, Response);
  const r = module.exports.stream(file.length).getReader();
  const got = [];
  for (;;) {
    const { done, value } = await r.read();
    if (done) break;
    got.push(...value);
  }
  assert.equal(got.length, file.length);
  assert.deepEqual(Uint8Array.from(got), file);
  assert.ok(calls >= 6, "4 ranges, a pause and a retry");
  assert.ok(auth.every((a) => a === ""), "no token was posted to it, so none goes out");

  // the pairing code (paircode.h): /bundle/zip is gated, so the page posts this run's token and the worker puts it
  // on its own requests - it has none of the page's headers of its own
  on.message({ data: { sidecarAuth: "a".repeat(32) } });
  calls = 0; paused = 0; auth = [];
  const r2 = module.exports.stream(PIECE_ONE).getReader();
  await r2.read();
  assert.ok(auth.length > 0 && auth.every((a) => a === "a".repeat(32)), "the token rides on every range");

  // and a refusal is not zip bytes: it stops with a message rather than writing "401 not paired" into the download
  refuse = true;
  calls = 0;
  const r3 = module.exports.stream(PIECE_ONE).getReader();
  await assert.rejects(r3.read(), /pairing code again/);
  assert.ok(calls <= 2, `a refusal is not retried five times (${calls} calls)`);
});
const PIECE_ONE = 1000;

test("the trash (2026-10-27): its entries with Restore and Remove, the newest first as the Sidecar sends them; empty", () => {
  assert.match(fns.pbTrashRows([]), /empty/);
  const rows = fns.pbTrashRows([
    { id: 7, key: "@005", name: "PRESETS", why: "clear", date: "2026-10-27T10:03:00Z", items: 5 },
    { id: 3, key: "@002", name: "", why: "web", date: "", items: 1 },
  ]);
  assert.deepEqual([...rows.matchAll(/data-id="(\d+)"/g)].map((m) => m[1]), ["7", "3"]);
  assert.match(rows, /<td>PRESETS<\/td><td>@005<\/td><td>clear<\/td><td>5<\/td><td>2026-10-27 10:03<\/td>/);
  assert.match(rows, /<td>@002<\/td><td>@002<\/td><td>web<\/td>/);
  assert.equal([...rows.matchAll(/data-act="restore"/g)].length, 2);
  assert.match(page, /id="pb-trash-empty"/);
  assert.match(page, /"\/bundle\/trash\/empty", \{ confirm: "EMPTY" \}/);
});

test("a bundle's Delete (2026-10-29): the card's and the stick's through /bundle/remove, an incomplete .part too", () => {
  const rows = fns.pbBundleRows([{ name: "@047 001.sidecar.zip.part", path: "USB:/SIDECAR/@047 001.sidecar.zip.part", size: 9, date: "", incomplete: true }], "usb");
  assert.match(rows, /data-path="USB:\/SIDECAR\/@047 001.sidecar.zip.part"/);
  assert.match(rows, /data-act="rm"/);
  assert.doesNotMatch(rows, /data-act="imp"/);
  assert.match(page, /pbJson\("\/bundle\/remove", \{ path \}\)/);
  assert.doesNotMatch(page, /delete files on the stick with a computer/);
  assert.match(page, /if \(b\.dataset\.act === "rm"\) pbRemove\(tr\.dataset\.path, tr\.dataset\.name, PB\.vol \|\| "the stick"\)/);
});

test("the last run's breadcrumb (recover() at boot, IDLE) is shown, other IDLE messages are not", () => {
  assert.match(page, /const crumb = st\.phase === "IDLE" && \/\^the last run stopped in\/\.test\(st\.msg \|\| ""\);/);
  assert.match(page, /say\(\$\("#pb-msg"\), st\.phase === "IDLE" && !crumb \? "" : st\.msg, st\.phase === "FAILED" \|\| crumb\);/);
});
