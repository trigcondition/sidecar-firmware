#!/usr/bin/env python3
"""A test card with one Sidecar project of every kind the bundles carry (docs/BUNDLES.md), synthesised here (no
third-party samples, nothing from a Rytm), for core/tests/demo/bundle_tool.cpp and testers:

    python3 tools/make_bundle_fixture.py CARD_DIR [PROJECT]       (default project "DEMO PERC")

Writes the project's pool lists (core/lib/pools.h: samples.pool.txt with bank A, a bank B row, an input and a
built-in; wavetables.pool.txt; instruments.pool.txt with content/MULTISAMPLE/SidecarTest's first .sfz, its members read
from the .sfz when the list is loaded; pool-used.txt), the sampler's takes (one unused), a slot map naming a package
if plugins/sine_test/sine_test.rmp is built (or SIDECAR_PKG names a package) (an RMP1: carried, never installed on a Pi), JUNO / FM lists, LFO and
INSERT settings (per kit), sequencer pattern placeholders (v2, v3 headers), a song take, and a stand-in Rytm project
file (text: a real one comes from Elektron Transfer). Standard library only.
"""
import hashlib
import math
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")


def wav(path, seconds, f0, kind="sine", rate=48000):
    n = int(seconds * rate)
    out = bytearray()
    for i in range(n):
        t = i / rate
        env = math.exp(-t * (8 if kind == "drum" else 1.5))
        if kind == "drum":
            v = math.sin(2 * math.pi * f0 * t * (1 + 2 * math.exp(-t * 40))) * env
        elif kind == "saw":
            v = 2 * ((f0 * t) % 1.0) - 1
        else:
            v = math.sin(2 * math.pi * f0 * t) * env
        out += struct.pack("<h", int(max(-1, min(1, 0.6 * v)) * 32767))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(out)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(out)) + out)


def text(path, s):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(s)


def ref(card, p):   # a list entry's reference: "<sha256 hex | -> <size> <path hint>" (lib::format_ref)
    f = os.path.join(card, p.lstrip("/"))
    if not os.path.exists(f):
        return f"- 100 {p}"
    b = open(f, "rb").read()
    return f"{hashlib.sha256(b).hexdigest()} {len(b)} {p}"


def main(card, project="DEMO PERC"):
    key = project   # (a name the key rule keeps as it is: letters, digits, spaces)
    smp = "".join(c if c.isalnum() or c in "_-" else "_" for c in project)
    j = lambda *p: os.path.join(card, *p)
    for name, f0, kind in [("kick", 55, "drum"), ("snare", 190, "drum"), ("tom", 110, "drum"), ("bell", 880, "sine")]:
        wav(j("Samples", "Drums", name + ".wav"), 0.6, f0, kind)
    shutil.copyfile(j("Samples", "Drums", "kick.wav"), j("Samples", "Drums", "kick (copy).wav"))   # same bytes, two paths
    wav(j("Samples", "Banks", "texture.wav"), 1.5, 220)
    wav(j("GRANULAR", "pad.wav"), 2.0, 330)
    wav(j("GRANULAR", "voices", "aah.wav"), 1.5, 440)
    wav(j("WAVETABLES", "saw.wav"), 0.043, 23.4375, "saw")
    wav(j("Samples", smp, "SMP_001.wav"), 0.5, 300)   # a sampler take the pool uses
    wav(j("Samples", smp, "SMP_002.wav"), 0.5, 600)   # and one it does not (in by default)
    src = os.path.join(ROOT, "content", "MULTISAMPLE", "SidecarTest")
    if os.path.isdir(src):
        shutil.copytree(src, j("MULTISAMPLE", "SidecarTest"), dirs_exist_ok=True)
    sfz = [f for f in os.listdir(j("MULTISAMPLE", "SidecarTest"))] if os.path.isdir(j("MULTISAMPLE", "SidecarTest")) else []
    sfz = [f for f in sfz if f.endswith(".sfz")]
    r = j(".rytm")
    pl = os.path.join(r, "projects", key)
    rows = [("A001", "/Samples/Drums/kick.wav"), ("A002", "/Samples/Drums/snare.wav"), ("A003", "/Samples/Drums/tom.wav"),
            ("A005", f"/Samples/{smp}/SMP_001.wav"), ("A006", "/Samples/Drums/missing.wav"), ("A010", "/GRANULAR/pad.wav"),
            ("A011", "/GRANULAR/voices/aah.wav"), ("B001", "/Samples/Banks/texture.wav")]
    text(os.path.join(pl, "samples.pool.txt"), "#sidecar samplepool 1\n" + "".join(f"{c} sample {ref(card, p)}\n" for c, p in rows) +
         "H001 input IN\nH011 builtin PAD\n")
    text(os.path.join(pl, "wavetables.pool.txt"), f"#sidecar wtpool 1\nA001 builtin BASIC\nA010 sample {ref(card, '/WAVETABLES/saw.wav')}\n")
    if sfz:
        text(os.path.join(pl, "instruments.pool.txt"), f"#sidecar instpool 1\nA001 instrument {ref(card, '/MULTISAMPLE/SidecarTest/' + sfz[0])}\n")
    text(os.path.join(pl, "pool-used.txt"), "#sidecar poolused 1\nsmp A001 1\n")
    pkg = os.environ.get("SIDECAR_PKG") or os.path.join(ROOT, "plugins", "sine_test", "sine_test.rmp")   # (any .rmp)
    if os.path.exists(pkg):
        hdr = open(pkg, "rb").read(96)
        pid = hdr[8:40].split(b"\0")[0].decode()
        ver = struct.unpack("<I", hdr[40:44])[0]
        os.makedirs(j("Plugins"), exist_ok=True)
        shutil.copyfile(pkg, j("Plugins", f"{pid}.rmp"))
        v = f"{ver >> 16}.{(ver >> 8) & 255}.{ver & 255}"
        text(os.path.join(r, "slots", key + ".txt"), f"U1 {pid} {v}\n")
        text(os.path.join(r, "slots", key + ".inf"), f"U1 saved 0 1829 {pid} {v} SINE\n")
    text(os.path.join(r, "fxslots", key + ".txt"), "\n")
    text(os.path.join(r, "juno", key + ".txt"), "00112233445566778899aabbccddeeff0011\tSTRINGS 1\n")
    text(os.path.join(r, "fm", key + ".txt"), "00112233445566778899aabbccddeeff\tE PIANO\n")
    text(os.path.join(r, "lfo", key + ".txt"), "# the project's default\nL1 V1 TIM SIN 40 20\n")
    text(os.path.join(r, "lfo", key, "k001.txt"), "L1 V2 TIM TRI 30 10\n")
    text(os.path.join(r, "ins", key + ".txt"), "V1 A FILTER 80 20 0 0\n")
    text(os.path.join(r, "ins", key, "k001.txt"), "V2 A DRIVE 40 0 0 0\n")
    for n, v in (("p001.sqp", 3), ("p002.sqp", 2)):
        os.makedirs(os.path.join(r, "seq", key), exist_ok=True)
        with open(os.path.join(r, "seq", key, n), "wb") as f:   # a placeholder pattern file: the head, test bytes
            f.write(b"SQP1" + bytes([v]) + bytes(59))
    text(os.path.join(r, "seq", key, "tracks.txt"), "T1 V1\n")
    text(os.path.join(r, "songs", key, "take-001.json"), '{"format":"test song placeholder","take":"take-001"}\n')
    text(os.path.join(r, "sendfx.txt"), "out=fx\n")
    text(j("Bundles", "Rytm", key + ".txt"), "A stand-in for the Rytm project file (Elektron Transfer exports the real one).\n")
    print(f"{card}: project {project!r}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    main(*sys.argv[1:3])
