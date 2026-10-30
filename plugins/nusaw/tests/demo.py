#!/usr/bin/env python3
# plugins/nusaw/tests/demo.py - a listening demo of NUSAW: libnusaw.dylib (the native build) played as a poly track's
# instances play it (one state a note), 48 kHz, 12-frame blocks. A pad (four bars of slow chords, ATK / REL up), then
# stabs on the defaults (eighths at 128 BPM over the same chords), then the stabs with DETN and ENV up.
#   python tests/demo.py [OUT.wav]      (make -C plugins/nusaw demo DEMO=OUT.wav; default build/nusaw-demo.wav)
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
CHORDS = [(57, 60, 64, 69), (53, 57, 60, 65), (48, 55, 60, 64), (55, 59, 62, 67)]   # Am F C G


def knobs(**over):
    man = json.loads((HERE.parent / "manifest.json").read_text())
    p = R.defaults(man)
    by = {k["short"]: k["p"] for k in man["knobs"]}
    for k, v in over.items():
        p[by[k] - 1] = float(v)
    return (C.c_float * 16)(*p)


def play(pl, events, seconds, prm):
    """events: (start s, length s, note, velocity); one state a note, summed"""
    total = int(seconds * FS) // BLOCK * BLOCK
    mix = np.zeros(total, dtype=np.float64)
    buf = (C.c_float * BLOCK)()
    for t0, dur, note, vel in events:
        st = C.create_string_buffer(pl.state_bytes + 16)
        sp = C.cast(st, C.c_void_p)
        pl.voice_init(sp)
        pl.params(sp, prm)
        a = int(t0 * FS) // BLOCK * BLOCK
        off = a + int(dur * FS) // BLOCK * BLOCK
        pl.note_on(sp, float(note), float(vel))
        for i in range(a, total, BLOCK):
            if i == off:
                pl.note_off(sp)
            pl.render(sp, buf, BLOCK)
            mix[i:i + BLOCK] += np.frombuffer(buf, dtype=np.float32)
    return mix


def main(out):
    lib = C.CDLL(str(HERE.parent / "libnusaw.dylib"))
    pl = R.Plugin.in_dll(lib, "rmp_entry")
    assert pl.init(C.byref(R.HOST)) == 0
    bar = 4 * 60 / 128
    pad_ev = [(i * 2 * bar, 2 * bar - 0.05, n, 0.7) for i, ch in enumerate(CHORDS) for n in ch]
    pad = play(pl, pad_ev, len(CHORDS) * 2 * bar + 1.5, knobs(ATK=72, REL=100, CUT=64, ENV=36, DETN=46, FDEC=96))
    eighth = bar / 8
    stab_ev = [((i * 8 + k) * eighth, eighth * 0.5, n, 0.95 if k % 4 == 0 else 0.75)
               for i, ch in enumerate(CHORDS) for k in (0, 2, 3, 5, 6) for n in ch]
    stabs = play(pl, stab_ev, len(CHORDS) * bar + 0.6, knobs(REL=56))
    wide = play(pl, stab_ev, len(CHORDS) * bar + 0.8, knobs(REL=60, DETN=70, ENV=80, RES=40, CUT=54))
    gap = np.zeros(int(0.25 * FS))
    x = np.concatenate([pad, gap, stabs, gap, wide])
    assert np.all(np.isfinite(x))
    x *= 0.9 / max(1e-9, float(np.max(np.abs(x))))   # the host's headroom, roughly
    out.parent.mkdir(parents=True, exist_ok=True)
    R.write_wav(out, x.astype(np.float32))
    print(f"{out}: {len(x) / FS:.1f} s, rms {float(np.sqrt(np.mean(x * x))):.3f}")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "nusaw-demo.wav")
