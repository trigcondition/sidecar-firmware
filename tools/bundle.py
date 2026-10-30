#!/usr/bin/env python3
"""tools/bundle.py - a Sidecar project bundle (<NAME>.sidecar.zip, docs/BUNDLES.md) on a Mac or PC: Python's own zipfile
and hashlib only, written independently of the Sidecar's reader (core/bundle).

  python3 tools/bundle.py inspect BUNDLE          the project, its machines, its files by kind, sizes
  python3 tools/bundle.py verify BUNDLE           every rule of the format and every file's sha256 (exit 1 on a fault)
  python3 tools/bundle.py extract BUNDLE DIR      the files into DIR as the bundle names them (never outside DIR)
  python3 tools/bundle.py list BUNDLE             one line a file: kind, size, name, its card path(s)
"""
import hashlib
import json
import os
import struct
import sys
import zipfile

ALIGN = 4096
PREFIX = {"pool": "project/", "project": "project/", "seq": "project/", "song": "project/", "sample": "files/",
          "take": "files/", "sfz": "files/", "package": "plugins/", "rytm": "rytm/"}
READER = 1   # the bundle reader version this tool knows


def name_ok(n):
    if not n or len(n.encode()) > 200 or n.startswith("/") or n.endswith("/"):
        return False
    if any(c in n for c in "\\:") or any(ord(c) < 0x20 or ord(c) == 0x7F for c in n):
        return False
    return all(p not in ("", ".", "..") for p in n.split("/"))


def manifest(z):
    infos = z.infolist()
    if not infos or infos[0].filename != "manifest.json":
        raise ValueError("manifest.json is not the first entry")
    return json.loads(z.read("manifest.json").decode("utf-8"))


def verify(path):
    """The list of faults (empty: a good bundle)."""
    faults = []
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) > 0x100000000 - 65536:
        faults.append("over 4 GiB - 64 KiB")
    with zipfile.ZipFile(path) as z:
        bad = z.testzip()
        if bad:
            faults.append(f"CRC wrong: {bad}")
        for i in z.infolist():
            if i.compress_type != zipfile.ZIP_STORED:
                faults.append(f"not stored: {i.filename}")
            if i.flag_bits & 0x0008:
                faults.append(f"a data descriptor: {i.filename}")
            if not i.flag_bits & 0x0800:
                faults.append(f"no UTF-8 flag: {i.filename}")
            if not name_ok(i.filename):
                faults.append(f"an unsafe name: {i.filename!r}")
            nl, xl = struct.unpack("<HH", raw[i.header_offset + 26:i.header_offset + 30])
            data = i.header_offset + 30 + nl + xl
            if data % ALIGN:
                faults.append(f"data not on a {ALIGN}-byte boundary: {i.filename}")
            if xl < 6 or struct.unpack("<H", raw[i.header_offset + 30 + nl:i.header_offset + 32 + nl])[0] != 0xD935:
                faults.append(f"no 0xD935 alignment field: {i.filename}")
        try:
            m = manifest(z)
        except Exception as e:  # noqa: BLE001 - a fault to report, not to raise
            return faults + [f"manifest: {e}"]
        if m.get("format") != "sidecar-bundle":
            faults.append("not a Sidecar bundle")
        if m.get("min_reader", 99) > READER:
            faults.append(f"needs bundle reader {m.get('min_reader')} (this tool reads {READER})")
        names = set(z.namelist())
        for e in m.get("entries", []):
            n = e["name"]
            if n not in names:
                faults.append(f"in the manifest, not in the bundle: {n}")
                continue
            if not n.startswith(PREFIX.get(e["kind"], "?")):
                faults.append(f"a {e['kind']} outside {PREFIX.get(e['kind'])}: {n}")
            try:
                b = z.read(n)
            except zipfile.BadZipFile as x:
                faults.append(f"damaged: {n} ({x})")
                continue
            if len(b) != e["size"]:
                faults.append(f"size differs: {n}")
            if hashlib.sha256(b).hexdigest() != e["sha256"]:
                faults.append(f"sha256 differs: {n}")
            for p in e.get("paths", []):
                if not p.startswith("/") or not name_ok(p[1:]):
                    faults.append(f"an unsafe card path: {p!r}")
        if m.get("totals", {}).get("entries") != len(m.get("entries", [])):
            faults.append("totals.entries differs from the entries")
    return faults


def human(n):
    return f"{n / 1048576:.1f} MB" if n >= 1048576 else f"{n / 1024:.1f} KB" if n >= 1024 else f"{n} B"


def inspect(path):
    with zipfile.ZipFile(path) as z:
        m = manifest(z)
    p = m["project"]
    print(f"{os.path.basename(path)}: project {p['rytm_name']!r} (key {p['key']!r}), made {m.get('created') or '?'} "
          f"({m.get('clock')}) by Sidecar {m['sidecar']['version']} on {m['sidecar']['board']}, format {m['version']}")
    by = {}
    for e in m["entries"]:
        k = by.setdefault(e["kind"], [0, 0])
        k[0] += 1
        k[1] += e["size"]
    for k, (n, b) in sorted(by.items()):
        print(f"  {k:8} {n:5} files {human(b):>10}")
    for s in m.get("slots", []):
        pk = ", ".join(f"{t}: {v}" for t, v in s.get("packages", {}).items()) or "no package in the bundle"
        print(f"  U{s['slot']} {s['name'] or s['id']} {s['id']} {s['ver']} ({pk})")
    if m.get("rytm_file"):
        print(f"  Rytm project file: {m['rytm_file']['name']} ({human(m['rytm_file']['size'])})")
    else:
        print("  no Rytm project file")
    if m.get("formats"):
        print("  formats: " + ", ".join(f"{k} v{v}" for k, v in m["formats"].items() if v))
    if m.get("missing"):
        print(f"  missing on the source card: {', '.join(m['missing'])}")


def listing(path):
    with zipfile.ZipFile(path) as z:
        m = manifest(z)
    for e in m["entries"]:
        print(f"{e['kind']:8} {e['size']:>10} {e['name']}" + (f"  <- {', '.join(e['paths'])}" if e.get("paths") else ""))


def extract(path, out):
    out = os.path.realpath(out)
    with zipfile.ZipFile(path) as z:
        for i in z.infolist():
            if not name_ok(i.filename):
                raise SystemExit(f"refused: an unsafe name {i.filename!r}")
            dest = os.path.realpath(os.path.join(out, i.filename))
            if not dest.startswith(out + os.sep):
                raise SystemExit(f"refused: {i.filename!r} would land outside {out}")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(z.read(i.filename))
    print(f"extracted into {out}")


def main(argv):
    if len(argv) < 3 or argv[1] not in ("inspect", "verify", "extract", "list"):
        print(__doc__)
        return 2
    if argv[1] == "inspect":
        inspect(argv[2])
    elif argv[1] == "list":
        listing(argv[2])
    elif argv[1] == "extract":
        if len(argv) < 4:
            print(__doc__)
            return 2
        extract(argv[2], argv[3])
    else:
        f = verify(argv[2])
        for x in f:
            print("FAULT:", x)
        print(f"{argv[2]}: {'OK' if not f else str(len(f)) + ' faults'}")
        return 1 if f else 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
