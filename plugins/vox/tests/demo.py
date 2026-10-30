#!/usr/bin/env python3
# plugins/vox/tests/demo.py - a listening demo of VOX: libvox.dylib (the native build) played as a poly track's
# instances play it (one state a note), 48 kHz, 12-frame blocks, the knobs automated per block as an LFO or the
# Rytm's p-locks would: a sung "aah" sweeping through the vowels (VOWL, TONG), a chord (Am, F) with a slow vowel
# morph, syllables from closures (CONS / CDIA, NASL for m / n, a fricative for s), then a legato glide (GLID).
#   python tests/demo.py [OUT.wav]      (make -C plugins/vox demo DEMO=OUT.wav; default build/vox-demo.wav)
# MIT, Copyright 2026 trigcondition and contributors.
import ctypes as C
import json
import math
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent))
import sc_render as R   # noqa: E402  (its Host / Plugin ctypes tables and write_wav)

FS, BLOCK = R.FS, R.BLOCK
MAN = json.loads((HERE.parent / "manifest.json").read_text())
BY = {k["short"]: k["p"] for k in MAN["knobs"]}


def knobs(over):
    p = R.defaults(MAN)
    for k, v in over.items():
        p[BY[k] - 1] = float(max(0.0, min(127.0, v)))
    return (C.c_float * 16)(*p)


def play(pl, voices, seconds, base, auto=None):
    """voices: one per state, each a list of (time s, 'on' note / 'off'); auto(t) -> knob overrides per block"""
    total = int(seconds * FS) // BLOCK * BLOCK
    mix = np.zeros(total, dtype=np.float64)
    buf = (C.c_float * BLOCK)()
    for ev in voices:
        st = C.create_string_buffer(pl.state_bytes + 16)
        sp = C.cast(st, C.c_void_p)
        pl.voice_init(sp)
        pl.params(sp, knobs(base))
        ev = sorted(ev)
        k = 0
        for i in range(0, total, BLOCK):
            t = i / FS
            while k < len(ev) and ev[k][0] <= t:
                if ev[k][1] == "off":
                    pl.note_off(sp)
                else:
                    pl.note_on(sp, float(ev[k][1]), 0.8)
                k += 1
            if auto:
                pl.params(sp, knobs({**base, **auto(t)}))
            pl.render(sp, buf, BLOCK)
            mix[i:i + BLOCK] += np.frombuffer(buf, dtype=np.float32)
    return mix


def tri(t, period):
    x = (t / period) % 1.0
    return 2 * x if x < 0.5 else 2 - 2 * x


def main(out):
    lib = C.CDLL(str(HERE.parent / "libvox.dylib"))
    pl = R.Plugin.in_dll(lib, "rmp_entry")
    assert pl.init(C.byref(R.HOST)) == 0
    parts = []
    # 1. "aah", then through the vowels: VOWL back .. front, TONG high .. low
    def vowels(t):
        u = t - 1.2
        if u < 0:
            return {}
        if u < 4.5:
            return {"VOWL": 127 * tri(u, 3.0), "TONG": 33 + 60 * math.sin(math.pi * u / 4.5) ** 2}
        return {"VOWL": 127 * tri(u, 3.0), "TONG": 33 - 33 * tri(u - 4.5, 2.0)}
    parts.append(play(pl, [[(0.0, 57), (7.2, "off")]], 8.0, {"VIBR": 30}, vowels))
    # 2. a chord: Am then F, a slow vowel morph
    am, f = (57, 60, 64, 69), (53, 57, 60, 65)
    chord = [[(0.0, a), (2.4, b), (4.8, "off")] for a, b in zip(am, f)]
    parts.append(play(pl, chord, 5.8, {"ATK": 80, "REL": 85}, lambda t: {"VOWL": 64 * tri(t, 4.8), "TONG": 33 + 40 * tri(t, 4.8)}))
    # 3. syllables: ma na da ga ba sa la, on a little tune
    syl = [("m", 127), ("n", 96), ("d", 96), ("g", 58), ("b", 127), ("s", 100), ("l", 92)]
    notes = [60, 62, 64, 60, 67, 64, 62]
    step = 0.42
    def cons(t):
        k = int(t / step)
        if k >= len(syl):
            return {}
        c, pos = syl[k]
        u = t - k * step
        o = {"CONS": pos}
        if c == "s":
            o["CDIA"] = 52 if u < 0.16 else 127
        elif c == "l":
            o["CDIA"] = 70 if u < 0.09 else 127
        else:
            o["CDIA"] = 0 if u < 0.09 else 127
        if c in "mn":
            o["NASL"] = 110 if u < 0.11 else 0
        return o
    ev = [(k * step, n) for k, n in enumerate(notes)] + [(len(notes) * step, "off")]
    parts.append(play(pl, [ev], len(notes) * step + 0.8, {"ATK": 40, "REL": 60}, cons))
    # 4. a legato glide (note_on on the sounding voice)
    gl = [(0.0, 50), (0.9, 62), (1.8, 55), (2.7, 67), (3.6, 57), (4.6, "off")]
    parts.append(play(pl, [gl], 5.4, {"GLID": 92, "VIBR": 36, "VOWL": 20, "TONG": 50}))
    gap = np.zeros(int(0.3 * FS))
    x = np.concatenate([np.concatenate([p, gap]) for p in parts])
    assert np.all(np.isfinite(x))
    x *= 0.9 / max(1e-9, float(np.max(np.abs(x))))   # the host's headroom, roughly
    out.parent.mkdir(parents=True, exist_ok=True)
    R.write_wav(out, x.astype(np.float32))
    print(f"{out}: {len(x) / FS:.1f} s, rms {float(np.sqrt(np.mean(x * x))):.3f}")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "vox-demo.wav")
