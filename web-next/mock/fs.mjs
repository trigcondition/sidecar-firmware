// web-next/mock/fs.mjs - a small card in memory behind the mock's /fs/* (pi/engine/fsweb.h's answers: ls as JSON, the
// rest "<code> <msg>" as text): the content folders, recordings on the SD and a USB drive, plugins, presets.
const now = () => {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")} ${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
};

export function createFs() {
  const files = new Map(); // path -> { d: true } | { s: size, t, data?: Buffer }
  const dir = (p, t = now()) => files.set(p, { d: true, t });
  const file = (p, s, t = now()) => files.set(p, { s, t });
  for (const p of ["/Recordings", "/Samples", "/Samples/SHARED SET", "/Plugins", "/PRESETS", "/MULTISAMPLE", "/WAVETABLES", "/GRANULAR", "/Bundles", "/USB", "/USB/Recordings"]) dir(p);
  const take = (base, name, t, chans) => {
    dir(`${base}/${name}`, t);
    chans.forEach((c, i) => file(`${base}/${name}/${String(i).padStart(2, "0")}_${c}.wav`, 2_900_000 + i * 31_000, t));
  };
  take("/Recordings", "SHARED_SET_2026-11-05_2114", "2026-11-05 21:14", ["MASTER", "BD", "SD"]);
  take("/Recordings", "SHARED_SET_2026-11-06_1032", "2026-11-06 10:32", ["MASTER", "MAIN_L", "MAIN_R", "CH-OH"]);
  take("/Recordings", "jam_2026-11-06_1840", "2026-11-06 18:40", ["MASTER"]);
  take("/USB/Recordings", "SHARED_SET_2026-11-04_2201", "2026-11-04 22:01", ["MASTER", "FX1"]);
  for (const [n, s] of [["kick 909.wav", 88_000], ["snare tape.wav", 120_000], ["vox chop.wav", 410_000]]) file(`/Samples/${n}`, s);
  for (const id of ["sc_grain", "sc_acid", "sc_metal", "sc_wobble"]) file(`/Plugins/${id}.rmp`, 12_000);

  const parent = (p) => p.slice(0, p.lastIndexOf("/")) || "/";
  const name = (p) => p.slice(p.lastIndexOf("/") + 1);
  const kids = (p) => [...files.entries()].filter(([k]) => k !== p && parent(k) === p);

  return {
    files,
    addTake: (base, n, chans) => take(base, n, now(), chans),
    ls(p) {
      if (p !== "/" && !files.get(p)?.d) return "404 not found\n";
      return { path: p, free: 28 << 30, total: 30 << 30, more: false, entries: kids(p).map(([k, v]) => ({ n: name(k), d: v.d ? 1 : 0, s: v.s ?? 0, t: v.t, ro: k.startsWith("/Recordings") || k.startsWith("/USB") ? 0 : 0 })) };
    },
    get(p, off, len) {
      const f = files.get(p);
      if (!f || f.d) return null;
      const n = Math.max(0, Math.min(len, f.s - off));
      const b = Buffer.alloc(n);
      if (f.data) f.data.copy(b, 0, off, off + n);
      return b;
    },
    del(p) {
      const f = files.get(p);
      if (!f) return "404 not found\n";
      if (f.d && kids(p).length) return "409 the folder is not empty\n";
      files.delete(p);
      return "200 deleted\n";
    },
    mkdir(p, parents) {
      if (files.has(p)) return "200 exists\n";
      if (!parents && !files.get(parent(p))?.d && parent(p) !== "/") return "404 no parent\n";
      const parts = p.split("/").filter(Boolean);
      for (let i = 1; i <= parts.length; i++) {
        const q = "/" + parts.slice(0, i).join("/");
        if (!files.has(q)) dir(q);
      }
      return "200 made\n";
    },
    mv(p, to) {
      const f = files.get(p);
      if (!f) return "404 not found\n";
      if (files.has(to)) return "409 exists\n";
      for (const [k, v] of [...files.entries()]) if (k === p || k.startsWith(p + "/")) { files.delete(k); files.set(to + k.slice(p.length), v); }
      return "200 renamed\n";
    },
    // a chunk of an upload (fsweb's protocol: off must be what is there; done on the last)
    put(p, off, total, data) {
      const part = files.get(p + ".part") ?? { s: 0, data: Buffer.alloc(0) };
      if (off !== part.s) return `409 expected ${part.s}\n`;
      part.data = Buffer.concat([part.data, data]);
      part.s = part.data.length;
      part.t = now();
      if (part.s >= total) {
        files.delete(p + ".part");
        files.set(p, { s: part.s, t: part.t, data: part.data });
        return `200 done ${part.s}\n`;
      }
      files.set(p + ".part", part);
      return `200 ok ${part.s}\n`;
    },
  };
}
