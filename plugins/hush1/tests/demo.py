#!/usr/bin/env python3
# plugins/hush1/tests/demo.py - a listening demo of HUSH1: libhush1.dylib (the native build) played as a mono track
# plays it (one state, its notes in turn), 48 kHz, 12-frame blocks. A 16th bass line on the defaults, a lead with
# glide (saw + pulse, overlapping notes), then the bass line again under a slow CUT sweep with RES up.
#   python tests/demo.py [OUT.wav]      (make -C plugins/hush1 demo DEMO=OUT.wav; default build/hush1-demo.wav)
# MIT, Copyright 2026 trigcondition and contributors.
import ctypes as C
import json
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent))
import sc_render as R   # noqa: E402  (its Host / Plugin ctypes tables and write_wav)

FS, BLOCK = R.FS, R.BLOCK
MAN = json.loads((HERE.parent / "manifest.json").read_text())
BY = {k["short"]: k["p"] for k in MAN["knobs"]}
BASS = [36, 36, 48, 36, 39, 36, 43, 36, 36, 46, 36, 48, 41, 36, 43, 34]


def knobs(**over):
    p = R.defaults(MAN)
    for k, v in over.items():
        p[BY[k] - 1] = float(v)
    return p


def play(pl, events, seconds, prm, sweep=None):
    """events: (start s, length s, note) on one state; sweep(t) -> {short: value} each block"""
    total = int(seconds * FS) // BLOCK * BLOCK
    out = np.zeros(total, dtype=np.float64)
    buf = (C.c_float * BLOCK)()
    st = C.create_string_buffer(pl.state_bytes + 16)
    sp = C.cast(st, C.c_void_p)
    pl.voice_init(sp)
    pl.params(sp, (C.c_float * 16)(*prm))
    ons = {int(t * FS) // BLOCK * BLOCK: n for t, _, n in events}
    offs = {}
    for t, d, n in events:
        offs.setdefault(int((t + d) * FS) // BLOCK * BLOCK, []).append(n)
    held = None
    for i in range(0, total, BLOCK):
        if sweep:
            p = list(prm)
            for k, v in sweep(i / FS).items():
                p[BY[k] - 1] = float(v)
            pl.params(sp, (C.c_float * 16)(*p))
        if i in offs and held in offs[i] and i not in ons:   # the Rytm's gate: off unless a new note takes over
            pl.note_off(sp)
            held = None
        if i in ons:
            pl.note_on(sp, float(ons[i]), 0.8)
            held = ons[i]
        pl.render(sp, buf, BLOCK)
        out[i:i + BLOCK] = np.frombuffer(buf, dtype=np.float32)
    return out


def main(out):
    lib = C.CDLL(str(HERE.parent / "libhush1.dylib"))
    pl = R.Plugin.in_dll(lib, "rmp_entry")
    assert pl.init(C.byref(R.HOST)) == 0
    six = 60 / 124 / 4
    bass_ev = [((b * 16 + k) * six, six * 0.5, n) for b in range(2) for k, n in enumerate(BASS)]
    bass = play(pl, bass_ev, 32 * six + 0.4, knobs())
    lead_notes = [(0, 2, 60), (2, 2, 67), (4, 1, 65), (5, 1, 63), (6, 2, 62), (8, 3, 70), (11, 1, 67), (12, 4, 72)]
    q = 60 / 124 / 2
    lead_ev = [(a * q, d * q + 0.03, n) for a, d, n in lead_notes]   # overlapping: legato, the glide sings
    lead = play(pl, lead_ev, 16 * q + 1.0, knobs(SAW=96, PULS=70, PW=60, SUB=30, CUT=58, RES=50, ENV=40, ATK=30,
                                                 DEC=70, SUS=90, REL=60, GLID=34, TRK=90))
    dur = 32 * six + 0.4
    sweep = play(pl, bass_ev, dur, knobs(RES=96, ENV=50, SUB=40, PULS=40),
                 sweep=lambda t: {"CUT": 10 + 90 * (0.5 - 0.5 * np.cos(2 * np.pi * t / dur))})
    gap = np.zeros(int(0.3 * FS))
    x = np.concatenate([bass, gap, lead, gap, sweep])
    assert np.all(np.isfinite(x))
    x *= 0.9 / max(1e-9, float(np.max(np.abs(x))))   # the host's headroom, roughly
    out.parent.mkdir(parents=True, exist_ok=True)
    R.write_wav(out, x.astype(np.float32))
    print(f"{out}: {len(x) / FS:.1f} s, rms {float(np.sqrt(np.mean(x * x))):.3f}")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "hush1-demo.wav")
