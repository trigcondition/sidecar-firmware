// pi/engine/tests/page_pool.test.mjs - the POOL on the page (pi/engine/www/index.html): the Files tab's +SMP / +WT on a
// WAV or AIFF and +INST on an .sfz (pool=K:add:B:PATH), Fill bank (pool=K:fill:B:DIR) and what their answers say; the
// Pool tab's helpers (the console's answers read from the log, the P3 question, the unload lines, a bank's rows with
// their states) and its edits with P3 (a row in use asks, then :yes; refused while the Rytm plays), against a fake
// console and against mock_web.py's /pool and pool= lines. node --test pi/engine/tests/page_pool.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const cut = (from, to) => {
  const a = page.indexOf(from), b = page.indexOf(to, a);
  assert.ok(a > 0 && b > a, "in index.html: " + from);
  return page.slice(a, b);
};
const F = new Function(cut("const esc =", "const join =") + cut("const POOLS", '$("#fs-list").addEventListener') +
  cut("// ---- the Pool tab (GET /pool", "// ---- the Pool tab's wiring") +
  "; return { POOLS, poolBtns, bankBtn, bankOf, addSay, fillSay, poolAns, p3Ask, plRam, unloadCmds, plRowsHtml, plRow, plTags, rowText, inUse, poolEdit };")();
const kinds = (h) => [...h.matchAll(/data-k="(\w+)"/g)].map((m) => m[1]);

test("one noun: the tabs, the buttons and the notes say POOL, never a machine's pool", () => {
  assert.match(page, /<a href="#pool" data-tab="pool">Pool<\/a><a href="#library" data-tab="library">Presets<\/a>/);
  assert.ok(!/Pool &amp; presets|Sample pool|Sidecar pool|GR pool|bank pool|(GRANULAR|WAVETABLE|MULTISAMPLE|machine)'s pool/i.test(page), "no old pool names");
  assert.ok(!/lib=(gr|wt|ms):(add|fill)|lib=\$\{/.test(page), "the removed lib=K:add / fill lines");
  assert.ok(!page.includes('id="lib-pool"'), "the old DSP pool section is gone");
  for (const id of ["pl-lists", "pl-banks", "pl-ram", "pl-rows", "pl-unused", "pl-unload", "pl-undo", "pl-pick", "fs-bank"])
    assert.ok(page.includes(`id="${id}"`), id);
});

test("+SMP / +WT on a WAV or AIFF, +INST on an .sfz", () => {
  assert.deepEqual(kinds(F.poolBtns({ n: "kick.wav", d: 0 }, 3)), ["smp", "wt"]);
  assert.deepEqual(kinds(F.poolBtns({ n: "Clap.AIFF", d: 0 }, 3)), ["smp", "wt"]);
  assert.deepEqual(kinds(F.poolBtns({ n: "pad.aif", d: 0 }, 3)), ["smp", "wt"]);
  assert.deepEqual(kinds(F.poolBtns({ n: "piano.sfz", d: 0 }, 3)), ["inst"]);
  assert.equal(F.poolBtns({ n: "Drums", d: 1 }, 3), "");
  assert.equal(F.poolBtns({ n: "._kick.wav", d: 0 }, 3), "");
  assert.equal(F.poolBtns({ n: "notes.txt", d: 0 }, 3), "");
  const h = F.poolBtns({ n: "kick.wav", d: 0 }, 7);
  assert.match(h, />\+SMP</);
  assert.match(h, />\+WT</);
  assert.match(h, /title="Add to the POOL's SAMPLES/);
  assert.match(h, /data-act="pool"[^>]*data-i="7"/);
  const s = F.poolBtns({ n: "x.sfz", d: 0 }, 1);
  assert.match(s, />\+INST</);
  assert.match(s, /title="Add to the POOL's INSTRUMENTS/);
});

test("Fill bank: a folder, a list and a bank A..H", () => {
  assert.match(F.bankBtn({ n: "Drums", d: 1 }, 4), /data-act="fill"[^>]*data-i="4"/);
  assert.equal(F.bankBtn({ n: "kick.wav", d: 0 }, 4), "");
  assert.equal(F.bankBtn({ n: ".git", d: 1 }, 4), "");
  assert.deepEqual(F.bankOf("smp c"), ["smp", "C"]);
  assert.deepEqual(F.bankOf(" WT H "), ["wt", "H"]);
  assert.deepEqual(F.bankOf("inst a"), ["inst", "A"]);   // bank A too: every bank takes files now
  assert.equal(F.bankOf("gr c"), null);
  assert.equal(F.bankOf("smp i"), null);
  assert.equal(F.bankOf(""), null);
});

test("the console's answers, read from the log (pools.cpp's lines as cores.cpp logs them)", () => {
  const A = (l, k = "smp") => F.poolAns("00:01:02.345 cores: " + l, k);
  assert.deepEqual(A("pool: smp C004 /Samples/kick.wav: SET"), { row: "C004", res: "SET", n: -1, text: "C004 /Samples/kick.wav: SET" });
  assert.equal(A("pool: smp C004 /Samples/a: b.wav: IN POOL").res, "IN POOL");      // a ": " in the path
  assert.equal(A("pool: smp A010 /x.wav: IN USE: REPLACE?").res, "IN USE: REPLACE?");
  assert.equal(A("pool: smp A010 /x.wav: PLAYING: STOP FIRST").res, "PLAYING: STOP FIRST");
  assert.deepEqual(A("pool: smp - /x.wav: BANK FULL"), { row: "", res: "BANK FULL", n: -1, text: "- /x.wav: BANK FULL" });
  assert.deepEqual(A("pool: smp: 2 unloaded"), { row: "", res: "", n: 2, text: "2 unloaded" });
  assert.equal(A("pool: smp: 0 unloaded, refused: IN USE: REPLACE?").res, "IN USE: REPLACE?");
  assert.equal(A("pool: wt: undone", "wt").text, "undone");
  assert.equal(A("pool: inst bank C: filling from /MULTISAMPLE", "inst").text, "bank C: filling from /MULTISAMPLE");
  assert.equal(A("pool: smp: undone", "wt"), null);                  // another list's
  assert.equal(F.poolAns("web: command :pool=smp:add:C:/x.wav", "smp"), null);   // the command's own echo
  assert.equal(A("pools: SAMPLES C004 = /x.wav"), null);             // the module's own log line
});

test("what +SMP / +WT / +INST and Fill bank say", () => {
  const a = (row, res, text = "") => ({ row, res, n: -1, text });
  assert.deepEqual(F.addSay(a("C004", "SET"), "kick.wav", "smp", "C"), ["kick.wav -> SAMPLES C004", false]);
  assert.deepEqual(F.addSay(a("A001", "IN POOL"), "kick.wav", "smp", "C"), ["kick.wav: already in A001", false]);
  assert.deepEqual(F.addSay(a("", "BANK FULL"), "kick.wav", "wt", "H"), ["WAVETABLES bank H is full: choose another POOL bank", true]);
  assert.match(F.addSay(a("", "READING"), "Piano.sfz", "inst", "B")[0], /reading the \.sfz, then into INSTRUMENTS bank B/);
  assert.deepEqual(F.addSay(a("", "NOT FOR THIS POOL"), "x.wav", "inst", "A"), ["x.wav: NOT FOR THIS POOL", true]);
  assert.equal(F.addSay(null, "x.wav", "smp", "A")[1], true);
  assert.deepEqual(F.fillSay(a("", "", "bank C: filling from /Kit"), "/Kit", "smp", "C"),
    ["SAMPLES bank C: filling from /Kit (empty rows only, in name order; the Pool tab shows them)", false]);
  assert.equal(F.fillSay(a("", "", "bank C: cannot fill from /x"), "/x", "smp", "C")[1], true);
});

test("P3's question, the RAM figure, rows as codes", () => {
  assert.equal(F.p3Ask("C012", { used: 40, users: 3 }, "UNLOAD"), "ROW C012 USED BY 3 SOUNDS - UNLOAD?");
  assert.equal(F.p3Ask("C012", { used: 40, users: 1 }, "REPLACE"), "ROW C012 USED BY 1 SOUND - REPLACE?");
  assert.equal(F.p3Ask("A001", { used: 7, users: 0 }, "UNLOAD"), "ROW A001 PLAYED IN THIS PROJECT (7x) - UNLOAD?");
  assert.equal(F.inUse({ used: 0, users: 0 }), false);
  assert.equal(F.inUse({ used: 0, users: 1 }), true);
  assert.equal(F.inUse(null), false);
  assert.equal(F.plRam({ budget: 0, used: 0, tables: 0, pct: 0 }), "-");   // until the shared cache fills it
  assert.equal(F.plRam({ budget: 1 << 30, used: 200 << 20, tables: 14 << 20, pct: 20 }), "214 MB of 1.0 GB (20 %)");
  assert.equal(F.rowText(0), "A000");
  assert.equal(F.rowText(2 * 128 + 12), "C012");
  assert.equal(F.rowText(7 * 128 + 127), "H127");
});

test("the unload lines: at most 190 characters, :yes only on the rows confirmed in use", () => {
  assert.deepEqual(F.unloadCmds("smp", ["A001", "A002"], new Set(["A002"])), ["pool=smp:unload:A001,A002:yes"]);
  const rows = Array.from({ length: 60 }, (_, i) => F.rowText(128 + i + 1)), yes = new Set(rows.filter((_, i) => i % 3 === 0));
  const L = F.unloadCmds("inst", rows, yes);
  assert.ok(L.length > 1 && L.every((c) => c.length <= 190 && c.startsWith("pool=inst:unload:")));
  const back = L.flatMap((c) => c.slice("pool=inst:unload:".length).split(","));
  assert.deepEqual(back.map((t) => t.slice(0, 4)), rows);
  assert.deepEqual(back.filter((t) => t.endsWith(":yes")).map((t) => t.slice(0, 4)), [...yes]);
});

test("a bank's rows: OFF first, the states, sizes, voices on it, the buttons a row takes", () => {
  const d = { list: "smp", bank: 2, locked: false, count: 6, rows: [
    [1, "sample", "kick", "EXACT", "/Samples/kick.wav", 37, 2, 4000, 0],
    [2, "sample", "gone", "MISSING", "/Samples/old/gone.wav", 0, 0, 52000, 0],
    [3, "sample", "hat <open>", "CHANGED", "/Samples/hat.wav", 0, 0, 30200, 0],
    [4, "instrument", "Piano", "PENDING", "/MULTISAMPLE/Piano.sfz", 0, 0, 2100, 24],
    [11, "builtin", "PAD", "BUILT-IN", "", 0, 0, 0, 0],
    [12, "input", "IN", "INPUT", "", 0, 0, 0, 0]] };
  const h = F.plRowsHtml(d, new Set(["C002"]), false);
  const tr = (row) => h.split("<tr").find((t) => t.includes(`data-row="${row}"`)) || "";
  assert.match(h, /^<tr class="mut"><td><\/td><td class="r">C000<\/td><td colspan="6">OFF<\/td><\/tr>/);
  assert.match(tr("C001"), /badge ok">EXACT/);
  assert.match(tr("C001"), /3.9 KB/);
  assert.match(tr("C001"), /<td class="r">37<\/td><td class="r">2<\/td>/);
  assert.match(tr("C002"), /badge err">MISSING/);
  assert.match(tr("C002"), /\/Samples\/old\/gone\.wav/);                 // a missing file shows where it was
  assert.match(tr("C002"), /data-sel="C002"[^>]*checked/);
  assert.match(tr("C003"), /badge wn">CHANGED/);
  assert.match(tr("C003"), /data-pa="accept"/);
  assert.match(tr("C003"), /hat &#60;open&#62;/);                        // names escaped
  assert.ok(!/data-pa="accept"/.test(tr("C001")));
  assert.match(tr("C004"), /24 samples/);
  assert.match(tr("C011"), /BUILT-IN/);
  assert.match(tr("C012"), /badge mut">INPUT/);
  assert.ok(!/data-pa="replace"/.test(tr("C011")) && /data-pa="unload"/.test(tr("C011")), "a built-in: unload only");
  assert.match(tr("C001"), /data-pa="replace"/);
  assert.ok(!/disabled/.test(h));
  assert.ok(F.plRowsHtml(d, new Set(), true).match(/disabled/g).length >= 6 * 2, "a locked list: no edits");
  assert.match(F.plRowsHtml({ bank: 5, rows: [] }, new Set(), false), /bank F is empty/);
  // docs/STEREO.md: the channels' tag (ST / M, once known) and "always in RAM"
  const st = F.plRowsHtml({ bank: 0, rows: [[1, "sample", "pad", "EXACT", "/S/pad.wav", 0, 0, 9000, 0, 2, 1],
                                          [2, "sample", "kick", "EXACT", "/S/kick.wav", 0, 0, 9000, 0, 1, 0],
                                          [3, "sample", "new", "EXACT", "/S/new.wav", 0, 0, 9000, 0]] }, new Set(), false);
  const r0 = (row) => st.split("<tr").find((t) => t.includes(`data-row="${row}"`)) || "";
  assert.match(r0("A001"), /· ST · RAM/);
  assert.match(r0("A002"), /· M</);
  assert.ok(!/· (ST|M|RAM)/.test(r0("A003")), "channels not known yet: no tag");
});

// a fake console: the lines sent, answers by rule, the questions asked
function fakeIo(answer, yes = true) {
  const io = { sent: [], asked: [], send: async (c) => { io.sent.push(c); return answer(c); }, ask: async (q) => { io.asked.push(q); return yes; } };
  return io;
}
const ans = (res, n = -1, row = "") => ({ row, res, n, text: res });

test("P3 with a fake console: ask first for a row in use, :yes after; nothing sent when not confirmed", async () => {
  let io = fakeIo(() => ans("SET"));
  assert.deepEqual(await F.poolEdit(io, "smp", "replace", "B003", { used: 0, users: 0 }, "/Samples/a.wav"), ["B003 = a.wav (Undo takes it back)", false]);
  assert.deepEqual(io.sent, ["pool=smp:replace:B003:/Samples/a.wav"]);
  assert.deepEqual(io.asked, []);
  io = fakeIo(() => ans("", 1));
  assert.deepEqual(await F.poolEdit(io, "wt", "unload", "C012", { used: 9, users: 3 }), ["C012 unloaded (Undo takes it back)", false]);
  assert.deepEqual(io.asked, ["ROW C012 USED BY 3 SOUNDS - UNLOAD?"]);
  assert.deepEqual(io.sent, ["pool=wt:unload:C012:yes"]);
  io = fakeIo(() => ans("SET"), false);
  assert.deepEqual(await F.poolEdit(io, "smp", "replace", "C012", { used: 2, users: 0 }, "/x.wav"), ["C012: not changed", false]);
  assert.deepEqual(io.sent, []);
  // in use only by the console's count (seen since the list was read): its IN USE asks, then :yes
  io = fakeIo((c) => (c.endsWith(":yes") ? ans("SET") : ans("IN USE: REPLACE?")));
  assert.deepEqual((await F.poolEdit(io, "smp", "replace", "A004", { used: 0, users: 0 }, "/b.wav"))[1], false);
  assert.deepEqual(io.sent, ["pool=smp:replace:A004:/b.wav", "pool=smp:replace:A004:/b.wav:yes"]);
  assert.equal(io.asked.length, 1);
  // while the Rytm plays: refused, never forced from the page
  io = fakeIo(() => ans("PLAYING: STOP FIRST"));
  assert.deepEqual(await F.poolEdit(io, "smp", "unload", "A001", { used: 4, users: 1 }), ["A001 is in use: not changed while the Rytm plays (stop it first)", true]);
  assert.ok(io.sent.every((c) => !c.includes(":force")));
  io = fakeIo(() => ans("IN POOL", -1, "A001"));
  assert.deepEqual(await F.poolEdit(io, "smp", "replace", "B001", null, "/Samples/kick.wav"), ["kick.wav is in A001 already: one row per file", true]);
  io = fakeIo(() => null);
  assert.equal((await F.poolEdit(io, "smp", "unload", "B001", null))[1], true);
});

test("the Pool tab's edits against mock_web.py: /pool, add, replace and unload with P3, playing, accept, unused, undo", async () => {
  // MOCK_PAIR=0 is cmdline.txt paircode=0: the pairing code is out of the way here, because this test is about
  // something else. The gate itself (every endpoint it covers, refusals, the lockout) is page_pair.test.mjs.
  const port = 18900 + Math.floor(Math.random() * 500), base = `http://127.0.0.1:${port}`;
  const srv = spawn("python3", [join(HERE, "mock_web.py"), String(port)], { stdio: "ignore", env: { ...process.env, MOCK_PAIR: "0" } });
  try {
    let up = false;
    for (let i = 0; i < 50 && !up; i++) { try { up = (await fetch(base + "/status")).ok; } catch (e) { await new Promise((r) => setTimeout(r, 100)); } }
    assert.ok(up, "mock_web answers");
    const pool = async (q) => (await fetch(base + "/pool?" + q)).json();
    let since = Number((await (await fetch(base + "/log?since=0")).text()).split("\n")[0].split(" ")[1]);
    // as the page's poolSend: /cmd, then the answer from the log
    const send = (k) => async (c) => {
      await fetch(base + "/cmd", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: "c=" + encodeURIComponent(c) });
      const t = await (await fetch(base + "/log?since=" + since)).text(), nl = t.indexOf("\n");
      since = Number(t.slice(0, nl).split(" ")[1]);
      for (const l of t.slice(nl + 1).split("\n")) { const a = F.poolAns(l, k); if (a) return a; }
      return null;
    };
    const io = (k, yes = true) => ({ asked: [], send: send(k), ask: async function (q) { this.asked.push(q); return yes; } });

    let d = await pool("l=smp&b=0");
    assert.equal(d.st.project, "AR PERC PATCH");
    assert.equal(F.plRam(d.ram), "-");
    assert.equal(d.bank.list, "smp");
    const A001 = F.plRow(d.bank.rows.find((r) => r[0] === 1));
    assert.equal(A001.name, "kick");
    assert.ok(F.inUse(A001));
    assert.match(F.plRowsHtml(d.bank, new Set(), false), /MISSING/);

    // +SMP: the first free row of bank C (C001 is taken), then the same file again; +INST, +WT, Fill bank
    let a = await send("smp")("pool=smp:add:C:/Samples/Kit/clap.aif");
    assert.deepEqual(F.addSay(a, "clap.aif", "smp", "C"), ["clap.aif -> SAMPLES C002", false]);
    a = await send("smp")("pool=smp:add:A:/Samples/Kit/clap.aif");
    assert.deepEqual(F.addSay(a, "clap.aif", "smp", "A"), ["clap.aif: already in C002", false]);
    a = await send("inst")("pool=inst:add:B:/MULTISAMPLE/Piano/Piano.sfz");
    assert.equal(F.addSay(a, "Piano.sfz", "inst", "B")[0], "Piano.sfz: already in A001");
    a = await send("wt")("pool=wt:add:A:/Samples/Kit/snare.wav");
    assert.equal(F.addSay(a, "snare.wav", "wt", "A")[0], "snare.wav -> WAVETABLES A007");
    assert.equal(F.fillSay(await send("smp")("pool=smp:fill:D:/Samples/Loops"), "/Samples/Loops", "smp", "D")[1], false);
    assert.deepEqual((await pool("l=smp&b=D")).bank.rows.map((r) => r[2]), ["loop 1", "loop 2"], "bank D: the folder's files in name order");

    // REPLACE a row in use: asked, then :yes
    let o = io("smp");
    assert.deepEqual(await F.poolEdit(o, "smp", "replace", "A001", A001, "/Samples/Kit/tom.wav"), ["A001 = tom.wav (Undo takes it back)", false]);
    assert.deepEqual(o.asked, ["ROW A001 USED BY 2 SOUNDS - REPLACE?"]);
    // while the Rytm plays: refused; not confirmed: nothing; confirmed: unloaded, and Undo brings it back
    await send("smp")("mock=playing:1");
    assert.deepEqual(await F.poolEdit(io("smp"), "smp", "unload", "A001", A001), ["A001 is in use: not changed while the Rytm plays (stop it first)", true]);
    await send("smp")("mock=playing:0");
    assert.deepEqual(await F.poolEdit(io("smp", false), "smp", "unload", "A001", A001), ["A001: not changed", false]);
    assert.deepEqual(await F.poolEdit(io("smp"), "smp", "unload", "A001", A001), ["A001 unloaded (Undo takes it back)", false]);
    assert.ok(!(await pool("l=smp&b=0")).bank.rows.some((r) => r[0] === 1));
    assert.equal((await send("smp")("pool=smp:undo")).text, "undone");
    assert.ok((await pool("l=smp&b=0")).bank.rows.some((r) => r[0] === 1 && r[2] === "tom"));

    // ACCEPT a CHANGED row
    assert.equal((await send("smp")("pool=smp:accept:A004")).res, "SET");
    assert.equal(F.plRow((await pool("l=smp&b=0")).bank.rows.find((r) => r[0] === 4)).state, "EXACT");

    // SELECT UNUSED: the rows not seen playing with the warning, then unloaded (none of them asks)
    d = await pool("l=smp&b=0&u=1");
    assert.equal(d.warn, "NOT SEEN PLAYING - A RYTM LOCK MAY STILL USE IT");
    const rows = d.unused.map(F.rowText);
    assert.ok(rows.includes("A004") && rows.includes("C001") && !rows.includes("A002"));
    let done = 0;
    for (const c of F.unloadCmds("smp", rows, new Set())) done += (await send("smp")(c)).n;
    assert.equal(done, rows.length);
    assert.equal((await pool("l=smp&b=0&u=1")).unused.length, 0);
  } finally { srv.kill(); }
});
