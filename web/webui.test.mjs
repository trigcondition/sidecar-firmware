// node web/webui.test.mjs - the web console protocol end to end against the mock device (mockdev.js): frames and
// CRCs, the memory map, the plugin list, install / refuse / delete / assign, the card's listing, upload (with a
// damaged frame, a resume and an abort), download, rename, mkdir, delete, and the transport's write lock.
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { crc32, frame, parseReply, F, bytesName, treeOf, junkName } from "./proto.js";
import { Link, Cancelled } from "./link.js";
import { MockDevice, makeRmp } from "./mockdev.js";
import { parseRmp, fitProblems } from "./rmp.js";
import { StreamParser } from "./stream.js";
import { slotSrc, slotListExport, slotListParse, slotListCmds, slotListMissing } from "./slotlist.js";

assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
{
  const f = frame(Uint8Array.of(1, 2, 3));
  assert.deepEqual([...f.subarray(0, 6)], [0x59, 3, 0, 1, 2, 3]);
  assert.equal(new DataView(f.buffer).getUint32(6, true), crc32(Uint8Array.of(1, 2, 3)));
  assert.equal(bytesName("fÃ¼r"), "für");
  const d = parseReply("@d 7 0 " + crc32(Uint8Array.of(104, 105)).toString(16).padStart(8, "0") + " aGk=");
  assert.equal(d.ok, true); assert.deepEqual([...d.data], [104, 105]);
  assert.equal(parseReply("cpu 21 %"), null);
}

function rig(opts = {}) {
  let up = true, link;
  const lines = [];
  const sp = new StreamParser();   // the bytes as the page reads them (non-ASCII bytes dropped there)
  const dev = new MockDevice((l) => {
    for (const x of sp.push(new TextEncoder().encode(l + "\r\n"))) { lines.push(x); const o = parseReply(x); if (o) link.reply(o); }
  }, { rate: 0, ...opts });
  let tamper = null;
  link = new Link({
    line: async (t) => dev.feed(new TextEncoder().encode(":" + t + "\n")),
    bytes: async (b) => { if (tamper) b = tamper(b) || b; dev.feed(b); },
    connected: () => up,
  });
  return { dev, link, lines, setTamper: (f) => (tamper = f), setUp: (v) => (up = v) };
}

// memory map and plugins
{
  const { link } = rig();
  const mem = await link.mem();
  const k = Object.fromEntries(mem.map((r) => [r.k, r]));
  assert.equal(k.itcm.blocks, 4); assert.ok(k.arena.size === 32768); assert.ok(k.sd.free_kb > 0);
  const pl = await link.plugins();
  assert.equal(pl.pkgs.length, 3); assert.equal(pl.slots.length, 5);
  assert.equal(pl.slots[0].state, "READY"); assert.equal(pl.slots[2].state, "MISSING");
  const big = pl.pkgs.find((p) => p.id === "rytm.big_test");
  assert.equal(big.fit, false); assert.match(big.why, /arena/);
  assert.deepEqual(pl.pkgs.find((p) => p.id === "rytm.sine_test").slots, [1]);
}

// install: refused in the page, refused by the device, accepted; assign; delete
{
  const { link, dev } = rig();
  const real = "../../rytm-teensy/plugins/sine_test/sine_test.rmp";
  const pkg = existsSync(new URL(real, import.meta.url)) ? new Uint8Array(readFileSync(new URL(real, import.meta.url))) : makeRmp({ id: "rytm.sine_test", name: "SINE TEST" });
  const info = parseRmp(pkg);
  assert.equal(info.tableOk, true); assert.deepEqual(fitProblems(info, { arena: [32768, 0], state_limit: 16384, store: [4194304, 3e6], api: 2 }), []);
  const fat = makeRmp({ id: "x.fat", name: "FAT", image: 20000, state: 40000 });
  assert.match(fitProblems(parseRmp(fat), { arena: [32768, 0], state_limit: 16384, store: [4194304, 3e6] })[0], /state/);
  await assert.rejects(link.upload("p", "", fat), /state/);           // the device refuses it at the header too
  const nu = makeRmp({ id: "me.new_one", name: "NEW ONE", image: 9000 });
  const r = await link.upload("p", "", nu);
  assert.equal(r.st, "done"); assert.ok(dev.store.has("me.new_one"));
  await link.plugin("set:2:me.new_one");
  assert.equal(dev.slots[1].id, "me.new_one");
  await assert.rejects(link.plugin("set:3:not.there"), /not installed/);
  await link.plugin("clr:2");
  assert.equal(dev.slots[1].id, "");
  await link.plugin("del:me.new_one");
  assert.ok(!dev.store.has("me.new_one")); assert.ok(dev.store.size === 3);   // one package, never the store
  dev.transport = true;
  await assert.rejects(link.plugin("del:rytm.sine_test"), /STOP TO INSTALL/);
  dev.transport = false;
}

// the card: list, upload (a damaged frame on the way, resent), download, rename, mkdir, delete
{
  const { link, dev, setTamper } = rig();
  const root = await link.list("/");
  assert.ok(root.items.some((i) => i.n === "Samples" && i.d === 1));
  assert.ok(root.items.some((i) => i.n === "Notes für Sidecar.txt"));
  const data = new Uint8Array(5000).map((_, i) => (i * 31) & 255);
  let hit = 0;
  setTamper((b) => { if (b[3] === 0x77 && ++hit === 2) { const c = b.slice(); c[20] ^= 1; return c; } });   // 2nd chunk damaged
  const prog = [];
  const r = await link.upload("f", "/Samples/new.bin", data, (p) => prog.push(p.sent));
  setTamper(null);
  assert.equal(r.st, "done"); assert.deepEqual(dev.fs.get("/Samples/new.bin").data, data);
  assert.equal(prog.at(-1), 5000);
  const back = await link.download("/Samples/new.bin", () => {}, {}, 2000);
  assert.deepEqual(back, data);
  await link.mkdir("/Samples/Folder");
  await link.rename("/Samples/new.bin", "/Samples/Folder/renamed.bin");
  assert.ok(dev.fs.has("/Samples/Folder/renamed.bin"));
  await assert.rejects(link.del("/Samples/Folder"), /not empty/);
  await link.del("/Samples/Folder/renamed.bin");
  await link.del("/Samples/Folder");
  await assert.rejects(link.del("/.sidecar/env"), /protected/);
  await assert.rejects(link.upload("f", "/.rytm/x", data), /protected/);
  dev.transport = true;
  await assert.rejects(link.mkdir("/X"), /STOP TO WRITE/);
  dev.transport = false;
  // abort mid-way: nothing is kept
  const sig = {};
  await assert.rejects(link.upload("f", "/big.bin", new Uint8Array(50000), (p) => { if (p.sent > 10000) sig.cancelled = true; }, sig), Cancelled);
  assert.ok(!dev.fs.has("/big.bin")); assert.equal(dev.xfer, null);
}

// resume: the device goes away mid-upload, comes back, the same transfer continues where it was
{
  const { link, dev, setUp } = rig();
  const data = new Uint8Array(8000).map((_, i) => i & 255);
  let n = 0;
  const r = await link.upload("f", "/resume.bin", data, (p) => { if (++n === 3) { setUp(false); setTimeout(() => setUp(true), 600); } });
  assert.equal(r.st, "done"); assert.deepEqual(dev.fs.get("/resume.bin").data, data);
}

// a dropped folder: its folders made (an existing one is fine), its files in order, macOS junk left out
{
  const { link, dev } = rig();
  const b = (n, v) => new Uint8Array(n).fill(v);
  const tree = treeOf([{ rel: "Kit/sub/deep/one.wav", data: b(3000, 1) }, { rel: "Kit/two.wav", data: b(10, 2) },
    { rel: "Kit/.DS_Store", data: b(5, 9) }, { rel: "Kit/._two.wav", data: b(5, 9) }, { rel: "Kit/sub/three.wav", data: b(700, 3) }]);
  assert.deepEqual(tree.dirs, ["Kit", "Kit/sub", "Kit/sub/deep"]);
  assert.deepEqual(tree.files.map((x) => x.rel), ["Kit/sub/deep/one.wav", "Kit/sub/three.wav", "Kit/two.wav"]);
  assert.ok(junkName(".DS_Store") && junkName("._x") && !junkName("x.wav"));
  await link.mkdir("/Samples/Kit");   // there already: still fine
  const prog = [];
  const r = await link.putTree(tree, "/Samples", (p) => prog.push(p));
  assert.deepEqual(r.failed, []); assert.equal(r.files, 3); assert.equal(r.bytes, 3710);
  for (const d of ["/Samples/Kit", "/Samples/Kit/sub", "/Samples/Kit/sub/deep"]) assert.ok(dev.fs.get(d).dir);
  assert.deepEqual(dev.fs.get("/Samples/Kit/sub/deep/one.wav").data, b(3000, 1));
  assert.deepEqual(dev.fs.get("/Samples/Kit/two.wav").data, b(10, 2));
  assert.ok(!dev.fs.has("/Samples/Kit/.DS_Store") && !dev.fs.has("/Samples/Kit/._two.wav"));
  assert.equal(prog.at(-1).sent, 3710); assert.match(prog.find((p) => p.note && p.note.includes("%")).note, /^1\/3 Kit\/sub\/deep\/one.wav \d+ %/);
  // a protected file fails alone, the rest goes on; a cancel stops the lot
  const r2 = await link.putTree(treeOf([{ rel: "x/a.bin", data: b(5, 1) }]), "/.rytm");
  assert.equal(r2.failed.length, 1);
  const sig = {};
  await assert.rejects(link.putTree(treeOf([{ rel: "C/a.bin", data: b(40000, 1) }, { rel: "C/b.bin", data: b(10, 1) }]), "/",
    (p) => { if (p.sent > 5000) sig.cancelled = true; }, sig), Cancelled);
  assert.ok(!dev.fs.has("/C/a.bin") && !dev.fs.has("/C/b.bin"));
}

// framed links carry short download lines
{
  const { link } = rig({ framed: true });
  const d = await link.download("/Samples/Kicks/BD Tight 02.wav");
  assert.equal(d.length, 41000);
}
// the slot list (slotlist.js): where a slot's machine came from, export / import over plugin=put, start empty
{
  const { link, dev } = rig();
  let pl = await link.plugins();
  assert.equal(pl.summary.prints, "rytm");
  assert.equal(pl.slots[0].src, "saved");
  assert.equal(slotSrc(pl.slots[2]).text, "missing: install befaco.np. 1.0.0");
  assert.equal(slotSrc({ src: "none", unknown: true, want_fp: 4711, want_name: "WOBBLE" }).text, "needs a machine with fingerprint 4711 (WOBBLE)");
  assert.equal(slotSrc({ src: "saved", updated: true }).text, "updated from the project's sounds");
  assert.deepEqual(slotSrc({ src: "saved", id: "befaco.np.c", state: "NO_ROOM", why: "no room: needs 23288 B, 584 B free" }),
    { text: "no room: needs 23288 B, 584 B free", cls: "bad" });
  assert.equal(slotSrc({ src: "saved", id: "rytm.sine_test", state: "READY", why: "stale" }).text, "saved with the project");
  assert.equal(slotSrc({ src: "inherited" }).text, "inherited from the previous project");
  const exp = slotListExport(pl.summary.project, pl.slots);
  assert.equal(exp.project, "MYPROJECT");
  assert.deepEqual(exp.slots.map((x) => x.slot), [1, 3]);
  // the Pi's page writes the same file: it imports here unchanged
  const l = slotListParse(JSON.stringify({ format: "sidecar-slots", version: 1, project: "MYPROJECT",
    slots: [{ slot: 2, id: "ext.wobble", ver: "0.3.0", name: "WOBBLE", fp: 4711 }, { slot: 4, empty: true },
      { slot: 5, id: "a.very.long.package.id.of.31chx", ver: "1.2.3", name: "LONGNAME" }] }));
  const cmds = slotListCmds(l);
  assert.deepEqual(cmds.slice(0, 2), ["put:2:ext.wobble:0.3.0:WOBBLE", "put:4:-:0.0.0"]);
  assert.ok(cmds.every((c) => (":plugin=" + c).length <= 63));
  for (const c of cmds) await link.plugin(c);
  pl = await link.plugins();
  assert.equal(pl.slots[1].state, "MISSING"); assert.equal(pl.slots[1].src, "imported");
  assert.equal(pl.slots[3].src, "empty");
  assert.deepEqual(slotListMissing(l, pl.pkgs), ["U2 WOBBLE 0.3.0 (ext.wobble)", "U5 LONGNAME 1.2.3 (a.very.long.package.id.of.31chx)"]);
  assert.deepEqual(slotListParse("U1 rytm.sine_test 1.0.0\n").slots, [{ slot: 1, id: "rytm.sine_test", ver: "1.0.0" }]);
  assert.throws(() => slotListParse("nonsense"));
  await link.plugin("empty");
  assert.ok(dev.slots.every((s) => !s.id && s.src === "empty"));
}
console.log("webui.test: ok");
