// web-next/scripts/card.test.ts - the card script's names and files
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { gunzipSync } from "node:zlib";
import { describe, expect, it } from "vitest";
// @ts-expect-error: a plain .mjs script
import { cardFiles, safeName } from "./card.mjs";

describe("card", () => {
  it("takes only names the Pi serves", () => {
    for (const n of ["index.html", "assets/index-Ab_9.js", "licenses/OFL-Doto.txt"]) expect(safeName(n)).toBe(true);
    for (const n of ["", ".DS_Store", "a/.x", "a b", "a//b", "a/", "x".repeat(121), "é.js"]) expect(safeName(n)).toBe(false);
  });
  it("gzips the text files, keeps fonts as they are", () => {
    const d = mkdtempSync(join(tmpdir(), "card-"));
    mkdirSync(join(d, "assets"));
    writeFileSync(join(d, "index.html"), "<!doctype html>");
    writeFileSync(join(d, "assets", "a.woff2"), "font");
    const f = cardFiles(d) as [string, Buffer][];
    expect(f.map(([n]) => n)).toEqual(["assets/a.woff2", "index.html.gz"]);
    expect(gunzipSync(f[1][1]).toString()).toBe("<!doctype html>");
    writeFileSync(join(d, ".hidden"), "x");
    expect(() => cardFiles(d)).toThrow(/will not serve/);
  });
});
