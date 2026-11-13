// web-next/scripts/card.mjs - puts the built app (dist/, `npm run build`) on the Sidecar's SD card, where the Pi serves
// it at /next (pi/engine/webnext_path.h): <card>/.rytm/www-next/. Text files go gzipped only (name.gz: the Pi sends
// them with Content-Encoding: gzip), fonts and images as they are. The folder is replaced as a whole, so no stale
// hashed files stay behind. Names the Pi would refuse (anything outside A-Z a-z 0-9 - _ . /, a part starting with a dot,
// longer than 120 characters) stop the copy.
//   node scripts/card.mjs /Volumes/SIDECAR          (the card's mount point)
//   node scripts/card.mjs /Volumes/SIDECAR --dry    (list what would be written)
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";

const GZ = /\.(html|js|css|svg|txt|json)$/;
export const safeName = (rel) => rel.length > 0 && rel.length <= 120 && /^[A-Za-z0-9_\-./]+$/.test(rel) && !rel.split("/").some((p) => p === "" || p.startsWith("."));

/** What goes onto the card: [relative name on the card, bytes]. */
export function cardFiles(dist) {
  const out = [];
  const walk = (d) => {
    for (const e of readdirSync(d).sort()) {
      const p = join(d, e);
      if (statSync(p).isDirectory()) walk(p);
      else {
        const rel = relative(dist, p).split(sep).join("/");
        if (!safeName(rel)) throw new Error(`${rel}: a name the Pi will not serve`);
        const b = readFileSync(p);
        out.push(GZ.test(rel) ? [`${rel}.gz`, gzipSync(b, { level: 9 })] : [rel, b]);
      }
    }
  };
  walk(dist);
  if (!out.some(([n]) => n === "index.html.gz")) throw new Error("no index.html: run npm run build first");
  return out;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const card = process.argv[2], dry = process.argv.includes("--dry");
  if (!card || card.startsWith("--")) {
    console.error("usage: node scripts/card.mjs <the card's mount point> [--dry]");
    process.exit(2);
  }
  if (!existsSync(card)) {
    console.error(`${card}: not found (is the card mounted?)`);
    process.exit(1);
  }
  const files = cardFiles(fileURLToPath(new URL("../dist", import.meta.url)));
  const dest = join(card, ".rytm", "www-next");
  let total = 0;
  for (const [n, b] of files) total += b.length;
  if (!dry) {
    rmSync(dest, { recursive: true, force: true });
    for (const [n, b] of files) {
      mkdirSync(dirname(join(dest, n)), { recursive: true });
      writeFileSync(join(dest, n), b);
    }
  }
  for (const [n, b] of files) console.log(`${String(b.length).padStart(8)}  ${n}`);
  console.log(`${dry ? "would write" : "wrote"} ${files.length} files, ${Math.round(total / 1024)} KB, to ${dest}`);
}
