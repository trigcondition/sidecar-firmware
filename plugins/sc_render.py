#!/usr/bin/env python3
# plugins/sc_render.py - a Mac render check of the sc_* starter plugins.
# SPDX-License-Identifier: MIT
# Copyright 2026 trigcondition and contributors
#
# Loads each plugins/sc_*/libsc_*.dylib (the RMP_NATIVE build, symbol rmp_entry), gives it an API-2
# rmp_host (48000 Hz, max_block 12, log, xorshift32 random), runs init, voice_init on a zeroed state,
# params with the manifest defaults (a P without a knob = 64), note_on(note, 0.8), renders 1.5 s in
# blocks of 12 with note_off at 0.5 s, checks the result and writes plugins/build-wav/<name>.wav.
#   python plugins/sc_render.py [sc_acid ... | nusaw | vox | hush1 | raffo]   (build first: make -C plugins/sc_xxx; the six by default)
import ctypes as C
import json
import pathlib
import sys
import wave

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
FS, BLOCK = 48000, 12
SECONDS, OFF_AT = 1.5, 0.5

LOGF = C.CFUNCTYPE(None, C.c_char_p)
RANDF = C.CFUNCTYPE(C.c_uint32, C.POINTER(C.c_uint32))


class Host(C.Structure):
    _fields_ = [("api", C.c_uint32), ("sample_rate", C.c_float), ("max_block", C.c_int32),
                ("log", LOGF), ("random", RANDF), ("reserved", C.c_void_p * 11)]


INITF = C.CFUNCTYPE(C.c_int32, C.POINTER(Host))
VOIDF = C.CFUNCTYPE(None, C.c_void_p)
NOTEF = C.CFUNCTYPE(None, C.c_void_p, C.c_float, C.c_float)
PARAMF = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(C.c_float))
RENDERF = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(C.c_float), C.c_int32)


class Plugin(C.Structure):
    _fields_ = [("magic", C.c_uint32), ("api", C.c_uint32), ("state_bytes", C.c_uint32),
                ("cpu_permille", C.c_uint32), ("init", INITF), ("voice_init", VOIDF), ("note_on", NOTEF),
                ("note_off", VOIDF), ("params", PARAMF), ("render", RENDERF), ("reserved", C.c_void_p * 8)]


@LOGF
def _log(msg):
    print("  log:", msg.decode(errors="replace"))


@RANDF
def _random(p):
    x = p[0] or 2463534242
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    p[0] = x
    return x


HOST = Host(2, float(FS), BLOCK, _log, _random)

# per plugin: the note, what it should do after note_off, and its spectral check
SPEC = {
    "sc_acid":    dict(note=48, kind="gated", pitch=True),
    "sc_wavepad": dict(note=60, kind="gated", pitch=True),
    "sc_metal":   dict(note=60, kind="decay", inharmonic=True),
    "sc_snare":   dict(note=60, kind="decay", noisy=True),
    "sc_pluck":   dict(note=60, kind="decay", pitch=True),
    "sc_grain":   dict(note=60, kind="gated", pitch=True),
    "nusaw":      dict(note=60, kind="gated", pitch=True),   # plugins/nusaw (its own package, not a starter)
    "vox":        dict(note=57, kind="gated", pitch=True),   # plugins/vox (its own package, not a starter)
    "hush1":      dict(note=48, kind="gated", pitch=True),   # plugins/hush1 (its own package, not a starter)
    "raffo":      dict(note=48, kind="gated", pitch=True),   # plugins/raffo (its own package, not a starter)
}


def defaults(manifest):
    p = [64.0] * 16
    for k in manifest.get("knobs", []):
        p[k["p"] - 1] = float(k.get("default", 64))
    return p


def render(name, note, vel=0.8):
    lib = C.CDLL(str(HERE / name / f"lib{name}.dylib"))
    pl = Plugin.in_dll(lib, "rmp_entry")
    assert pl.magic == 0x504D5252 and pl.api == 2, f"{name}: bad entry table"
    assert pl.init(C.byref(HOST)) == 0, f"{name}: init refused"
    state = C.create_string_buffer(pl.state_bytes + 16)
    sp = C.cast(state, C.c_void_p)
    pl.voice_init(sp)
    man = json.loads((HERE / name / "manifest.json").read_text())
    P = (C.c_float * 16)(*defaults(man))
    pl.params(sp, P)
    pl.note_on(sp, float(note), float(vel))
    total = int(SECONDS * FS) // BLOCK * BLOCK
    out = np.zeros(total, dtype=np.float32)
    buf = (C.c_float * BLOCK)()
    off = int(OFF_AT * FS) // BLOCK * BLOCK
    for i in range(0, total, BLOCK):
        if i == off:
            pl.note_off(sp)
        pl.render(sp, buf, BLOCK)
        out[i:i + BLOCK] = np.frombuffer(buf, dtype=np.float32)
    return pl, out


def write_wav(path, x):
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(pcm.tobytes())


def rms(x):
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64)))) if len(x) else 0.0


def spectrum(x):
    x = x - np.mean(x)
    n = 1 << 16
    X = np.abs(np.fft.rfft(x[:n] * np.hanning(min(n, len(x))), n))
    f = np.fft.rfftfreq(n, 1 / FS)
    return f, X


def peaks(f, X, count=8, lo=40.0):
    """the strongest local maxima above lo Hz, at least 3 bins apart"""
    m = (X[1:-1] > X[:-2]) & (X[1:-1] >= X[2:]) & (f[1:-1] > lo)
    idx = np.nonzero(m)[0] + 1
    idx = idx[np.argsort(X[idx])[::-1]]
    keep = []
    for i in idx:
        if all(abs(i - j) > 3 for j in keep):
            keep.append(i)
        if len(keep) == count:
            break
    return [(float(f[i]), float(X[i])) for i in keep]


def f0_harmonic(x, ref):
    """the f0 near ref (+-1 octave) whose first 8 harmonics carry the most (log) magnitude"""
    f, X = spectrum(x)
    L = np.log(X + 1e-9 * np.max(X))
    best, bestf = -1e30, ref
    for c in np.arange(-1250, 1251, 2.0):
        f0 = ref * 2 ** (c / 1200)
        k = np.arange(1, 9) * f0
        k = k[k < FS / 2 - 100]
        sc = float(np.mean(np.interp(k, f, L)))
        if sc > best:
            best, bestf = sc, f0
    return bestf


def f0_autocorr(x, fmin=40.0, fmax=2000.0):
    x = x - np.mean(x)
    n = len(x)
    r = np.fft.irfft(np.abs(np.fft.rfft(x, 2 * n)) ** 2)[:n]
    lo, hi = int(FS / fmax), int(FS / fmin)
    lag = lo + int(np.argmax(r[lo:hi]))
    if 1 <= lag < n - 1:   # parabolic refinement
        a, b, c = r[lag - 1], r[lag], r[lag + 1]
        d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
        return FS / (lag + d)
    return FS / lag


def cents(f, ref):
    return 1200.0 * np.log2(f / ref)


def pitch_class_cents(f, ref):
    """distance in cents from f to the nearest octave of ref"""
    c = cents(f, ref) % 1200.0
    return min(c, 1200.0 - c)


def check(name, x, spec):
    notes = []
    ok = True
    if not np.all(np.isfinite(x)):
        return False, ["NOT FINITE"]
    pk = float(np.max(np.abs(x)))
    if not 0.05 <= pk <= 1.0:
        ok = False
        notes.append(f"peak {pk:.3f} outside 0.05..1")
    r = rms(x)
    dcfrac = float(np.mean(x, dtype=np.float64) ** 2 / max(r * r, 1e-30))   # the DC share of the energy
    if r == 0 or dcfrac > 0.02:
        ok = False
        notes.append(f"DC: {100 * dcfrac:.1f}% of the energy")
    s = lambda a, b: x[int(a * FS):int(b * FS)]
    early, mid, tail = rms(s(0.0, 0.3)), rms(s(0.3, 0.5)), rms(s(1.25, 1.5))
    if spec["kind"] == "gated":
        if mid < 0.2 * early:
            ok = False
            notes.append(f"does not sustain while gated ({mid:.3f} vs {early:.3f})")
        if tail > 0.5 * mid:
            ok = False
            notes.append(f"does not release after note_off ({tail:.3f} vs {mid:.3f})")
    else:
        if tail > 0.5 * early:
            ok = False
            notes.append(f"does not decay ({tail:.3f} vs {early:.3f})")
    ref = 440.0 * 2 ** ((spec["note"] - 69) / 12)
    seg = s(0.05, 0.45)
    if spec.get("pitch"):
        f0 = f0_harmonic(s(0.1, 0.45), ref)
        d = pitch_class_cents(f0, ref)
        notes.append(f"f0 {f0:.1f} Hz (note {ref:.1f}, {d:.0f} c)")
        if d > 30:
            ok = False
            notes.append("PITCH OFF")
    f, X = spectrum(seg if len(seg) > 4096 else x)
    if spec.get("inharmonic"):
        pks = peaks(f, X, 6, lo=100.0)
        base = min(p[0] for p in pks)
        dev = [abs(p[0] / base - round(p[0] / base)) for p in pks if p[0] > base * 1.2]
        inh = float(np.median(dev)) if dev else 0.0
        notes.append(f"inharmonicity {inh:.2f} (peaks " + ",".join(f"{p[0]:.0f}" for p in sorted(pks)) + ")")
        if inh < 0.08:
            ok = False
            notes.append("TOO HARMONIC")
    if spec.get("noisy"):
        band = (f > 200) & (f < 16000)
        P = X[band] ** 2 + 1e-20
        flat = float(np.exp(np.mean(np.log(P))) / np.mean(P))
        cen = float(np.sum(f[band] * P) / np.sum(P))
        notes.append(f"flatness {flat:.3f}, centroid {cen:.0f} Hz")
        if flat < 0.01:
            ok = False
            notes.append("NO NOISE")
    return ok, notes


def main(names):
    outdir = HERE / "build-wav"
    outdir.mkdir(exist_ok=True)
    rows, all_ok = [], True
    for name in names:
        spec = SPEC[name]
        pl, x = render(name, spec["note"])
        write_wav(outdir / f"{name}.wav", x)
        ok, notes = check(name, x, spec)
        all_ok &= ok
        rows.append((name, pl.state_bytes, pl.cpu_permille, float(np.max(np.abs(x))), rms(x), ok, "; ".join(notes)))
        if pl.state_bytes > 16384:
            all_ok = False
            rows[-1] = rows[-1][:5] + (False, rows[-1][6] + "; STATE OVER 16384")
    print(f"{'plugin':<11} {'state B':>7} {'cpu':>4} {'peak':>6} {'rms':>6}  ok  notes")
    for n, sb, cpu, pk, r, ok, notes in rows:
        print(f"{n:<11} {sb:>7} {cpu:>4} {pk:>6.3f} {r:>6.3f}  {'ok' if ok else 'NO'}  {notes}")
    print(f"WAVs in {outdir}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or [n for n in SPEC if n.startswith("sc_")]))   # nusaw, vox, hush1, raffo by name
