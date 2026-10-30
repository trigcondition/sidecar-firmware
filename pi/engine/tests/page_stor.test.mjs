// pi/engine/tests/page_stor.test.mjs - the Status tab's Storage section (pi/engine/www/index.html, storVolText): each
// volume's state from /status "stor" (pi/engine/stor_vol.cpp json_vols) as a line. node --test pi/engine/tests/page_stor.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const h = page.indexOf("function hb("), he = page.indexOf("const join =");
const a = page.indexOf("const STOR_CAP"), b = page.indexOf("function storRender");
assert.ok(h > 0 && he > h && a > 0 && b > a, "the storage helpers in index.html");
const F = new Function(page.slice(h, he) + page.slice(a, b) + "; return { STOR_CAP, storVolText };")();
const vol = (o) => Object.assign({ name: "USB", state: "absent", fs: "", totalMB: 0, freeMB: 0, link: "", model: "", why: "", pulls: 0 }, o);

test("a mounted drive: format, free of total, link, model", () => {
  assert.equal(F.storVolText(vol({ state: "mounted", fs: "FAT32", totalMB: 61056, freeMB: 58982, link: "USB 3 SuperSpeed", model: "0781:5581" })),
    "FAT32 · 58 GB free of 60 GB · USB 3 SuperSpeed · 0781:5581");
  assert.equal(F.storVolText(vol({ state: "mounted", fs: "FAT32", totalMB: 61056, freeMB: null })),
    "FAT32 · 60 GB, free space not counted (stor=free:USB)");
  assert.equal(F.storVolText(vol({ state: "mounted", fs: "exFAT", totalMB: 900, freeMB: 8, pulls: 2 })),
    "exFAT · 8.0 MB free of 900 MB · pulled without eject 2×");
});

test("the other states say what to do", () => {
  assert.equal(F.storVolText(vol({ state: "present", why: "GPT partition table: use the MBR scheme (FAT32)" })),
    "not mounted: GPT partition table: use the MBR scheme (FAT32)");
  assert.equal(F.storVolText(vol({ state: "ejected" })), "ejected: safe to remove");
  assert.equal(F.storVolText(vol({ name: "NVME", state: "unavailable" })), "not on this board");
  assert.equal(F.storVolText(vol({ name: "NVME", why: "no SSD answered at boot" })), "none (no SSD answered at boot)");
  assert.equal(F.storVolText(vol({})), "none");
  assert.equal(F.STOR_CAP.NVME, "NVMe SSD");
});
