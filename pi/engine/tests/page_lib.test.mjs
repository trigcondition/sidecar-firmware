// pi/engine/tests/page_lib.test.mjs - the Status tab's Library line (pi/engine/www/index.html, libText): /status
// "library" (core/lib/library.cpp status_json) as one line. node --test pi/engine/tests/page_lib.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const h = page.indexOf("function hb("), he = page.indexOf("const join =");
const a = page.indexOf("function libText("), b = page.indexOf("// ---- the volumes and the storage bench");
assert.ok(h > 0 && he > h && a > 0 && b > a, "the library helpers in index.html");
assert.ok(page.includes('id="lib-st"') && page.includes("libText(status.library)"), "the line and its render");
const F = new Function(page.slice(h, he) + page.slice(a, b) + "; return { libText };")();
const lib = (o) => Object.assign({ state: "READY", files: 0, bytes: 0, hashed: 0, pending: 0, pct: 100, dups: 0, dupBytes: 0,
  missing: 0, unreadable: 0, recordings: 0, waiting: "", logLines: 0, readOnly: false }, o);

test("hashing, with duplicates and a reason to wait", () => {
  assert.equal(F.libText(lib({ state: "HASHING", files: 48213, bytes: 19756849152, pct: 61, pending: 18803, dups: 212,
    dupBytes: 404750336, waiting: "PLAYING" })),
    "48,213 files (18 GB), 61 % hashed, 18,803 pending, 212 duplicates (386 MB) · waiting: playing");
});

test("ready, missing and unreadable files, recordings indexed", () => {
  assert.equal(F.libText(lib({ files: 1203, bytes: 2147483648, missing: 7, unreadable: 1, recordings: 4 })),
    "1,203 files (2.0 GB), all hashed, 7 missing, 1 unreadable, 4 recordings indexed");
});

test("the early states and a newer build's index", () => {
  assert.equal(F.libText(lib({ state: "OFF" })), "off (no card)");
  assert.equal(F.libText(lib({ state: "LOADING" })), "loading the index");
  assert.equal(F.libText(lib({ state: "WALKING", files: 512 })), "512 files, looking at the card");
  assert.equal(F.libText(lib({ files: 3, bytes: 3000, readOnly: true })),
    "3 files (2.9 KB), all hashed · its index is a newer Sidecar's: read-only here");
});
