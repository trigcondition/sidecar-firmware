#!/usr/bin/env python3
# plugins/raffo/tests/demo.py - a listening demo of RAFFO: libraffo.dylib (the native build) played as a mono track
# plays it (one state, note_on on every trig, note_off when the gate closes: an overlapping trig is legato and glides),
# 48 kHz, 12-frame blocks. A bassline on the defaults (16ths at 124 BPM, two slides), a lead with glide (WAV1 SAW,
# WAV2 SQR, octave-up phrases, GLID up, legato), then the bassline under a filter sweep (CUT 10 -> 120 -> 30, RES up).
#   python tests/demo.py [OUT.wav]      (make -C plugins/raffo demo DEMO=OUT.wav; default build/raffo-demo.wav)
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


def knobs(**over):
    p = R.defaults(MAN)
    for k, v in over.items():
        p[BY[k] - 1] = float(v)
    return p


def play(pl, events, seconds, prm_at):
    """events: (start s, length s, note, velocity) on one state; prm_at(t) -> the 16 knobs at time t"""
    total = int(seconds * FS) // BLOCK * BLOCK
    out = np.zeros(total, dtype=np.float64)
    st = C.create_string_buffer(pl.state_bytes + 16)
    sp = C.cast(st, C.c_void_p)
    pl.voice_init(sp)
    buf = (C.c_float * BLOCK)()
    ev = [(int(t0 * FS) // BLOCK * BLOCK, int((t0 + d) * FS) // BLOCK * BLOCK, n, v) for t0, d, n, v in events]
    gate = False
    for i in range(0, total, BLOCK):
        pl.params(sp, (C.c_float * 16)(*prm_at(i / FS)))
        for a, _, n, v in ev:
            if a == i:
                pl.note_on(sp, float(n), float(v))
        held = any(a <= i < b for a, b, _, _ in ev)
        if gate and not held:
            pl.note_off(sp)
        gate = held
        pl.render(sp, buf, BLOCK)
        out[i:i + BLOCK] = np.frombuffer(buf, dtype=np.float32)
    return out


def bassline(bars, step):
    # C2-based, 16ths; 's' marks a slide (the note lasts into the next one: legato)
    pat = [(36, 1, ""), (36, 1, ""), (48, 1, ""), (36, 1, ""), (39, 2, "s"), (41, 1, ""), (36, 1, ""),
           (36, 1, ""), (46, 1, ""), (36, 1, ""), (43, 1, "s"), (31, 1, ""), (34, 1, ""), (36, 1, "")]
    ev, t = [], 0.0
    for _ in range(bars):
        k = 0
        for n, ln, fl in pat:
            d = ln * step * (1.15 if fl == "s" else 0.55)
            ev.append((t, d, n, 0.95 if k % 4 == 0 else 0.7))
            t += ln * step
            k += ln
    return ev, t


def main(out):
    lib = C.CDLL(str(HERE.parent / "libraffo.dylib"))
    pl = R.Plugin.in_dll(lib, "rmp_entry")
    assert pl.init(C.byref(R.HOST)) == 0
    step = 60 / 124 / 4
    bass_ev, blen = bassline(4, step)
    bass = play(pl, bass_ev, blen + 0.6, lambda t: knobs())

    # the lead: a phrase of long legato notes (each overlaps the next: glide), an octave up, then a held top note
    lead_notes = [(60, 2), (63, 1), (67, 1), (70, 2), (72, 4), (70, 1), (67, 1), (63, 2), (65, 2), (67, 4), (72, 2),
                  (75, 2), (79, 6)]
    lead_ev, t = [], 0.0
    for i, (n, ln) in enumerate(lead_notes):
        lead_ev.append((t, ln * step * 2 + (0.05 if i + 1 < len(lead_notes) and (i + 1) % 4 != 0 else -0.08), n, 0.8))
        t += ln * step * 2
    lead_k = knobs(WAV1=48, WAV2=80, DTN2=70, MIX=70, SUB=30, CUT=58, RES=60, ENV=40, ATK=14, DEC=70, SUS=100,
                   REL=46, FDEC=60, FSUS=50, GLID=46)
    lead = play(pl, lead_ev, t + 1.0, lambda _t: lead_k)

    # the sweep: the bassline again, CUT 10 -> 120 -> 30 over its length, RES high, less contour
    sweep_len = blen

    def sweep(tt):
        x = min(1.0, tt / sweep_len)
        cut = 10 + 110 * (x / 0.6) if x < 0.6 else 120 - 90 * ((x - 0.6) / 0.4)
        return knobs(CUT=cut, RES=96, ENV=30, FDEC=40, WAV2=48, DTN2=76)
    swp = play(pl, bass_ev, blen + 0.8, sweep)

    gap = np.zeros(int(0.3 * FS))
    x = np.concatenate([bass, gap, lead, gap, swp])
    assert np.all(np.isfinite(x)) and float(np.max(np.abs(x))) <= 1.0
    x *= 0.9 / max(1e-9, float(np.max(np.abs(x))))   # the host's headroom, roughly
    out.parent.mkdir(parents=True, exist_ok=True)
    R.write_wav(out, x.astype(np.float32))
    print(f"{out}: {len(x) / FS:.1f} s, rms {float(np.sqrt(np.mean(x * x))):.3f}")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "build" / "raffo-demo.wav")
