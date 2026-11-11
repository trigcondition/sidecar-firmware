#!/usr/bin/env python3
# plugins/ufx_render.py - a Mac render of the API 3 user effects (ufx_phaser, ufx_bbd, ufx_pingpong) for listening and
# a quick check. SPDX-License-Identifier: MIT. Copyright 2026 trigcondition and contributors.
#
# Loads plugins/<name>/lib<name>.dylib (the RMP_NATIVE build: `make -C plugins/<name> lib<name>.dylib`), finds its
# rmp3_plugin (rmp_plugin.reserved[4]), gives each state an arena as the host would (want bytes, zeroed), runs init,
# voice_init, voice_attach (place SEND), then render in blocks of 12 with RMP3_B_WET_ONLY, the knobs in b->p and a
# transport at the given tempo, as the send FX does on the Pi (core 2). Renders test material made here (a chord, a
# drum loop, noise), checks it (finite, no denormals, bounded, tails that end) and writes stereo WAVs to
# plugins/build-wav/ (not committed): <name>-<material>.wav, the dry input first, then the return.
#   python plugins/ufx_render.py [ufx_phaser ufx_bbd ...] [--p1 N --p2 N --p3 N --p4 N] [--bpm 120]
import argparse
import ctypes as C
import json
import pathlib
import sys
import wave

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
FS, BLOCK = 48000, 12

LOGF = C.CFUNCTYPE(None, C.c_char_p)
RANDF = C.CFUNCTYPE(C.c_uint32, C.POINTER(C.c_uint32))


class Host(C.Structure):
    _fields_ = [("api", C.c_uint32), ("sample_rate", C.c_float), ("max_block", C.c_int32),
                ("log", LOGF), ("random", RANDF), ("reserved", C.c_void_p * 11)]


INITF = C.CFUNCTYPE(C.c_int32, C.POINTER(Host))
VOIDF = C.CFUNCTYPE(None, C.c_void_p)
PARAMF = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(C.c_float))


class Plugin(C.Structure):
    _fields_ = [("magic", C.c_uint32), ("api", C.c_uint32), ("state_bytes", C.c_uint32),
                ("cpu_permille", C.c_uint32), ("init", INITF), ("voice_init", VOIDF), ("note_on", C.c_void_p),
                ("note_off", C.c_void_p), ("params", PARAMF), ("render", C.c_void_p), ("reserved", C.c_void_p * 8)]


class Transport(C.Structure):   # rmp3_transport, 32 B
    _fields_ = [("flags", C.c_uint32), ("bpm", C.c_float), ("tick_us", C.c_float), ("free_tick", C.c_float),
                ("song_tick", C.c_int32), ("song_frac", C.c_float), ("step16", C.c_uint32), ("lead_frames", C.c_uint32)]


class Block(C.Structure):       # rmp3_block, 112 B
    _fields_ = [("size", C.c_uint32), ("n", C.c_int32), ("flags", C.c_uint32), ("gen", C.c_uint32),
                ("out", C.POINTER(C.c_float) * 2), ("inp", C.POINTER(C.c_float) * 2), ("aux", C.POINTER(C.c_float) * 2),
                ("p", C.POINTER(C.c_float)), ("p_changed", C.c_uint32), ("place", C.c_uint32),
                ("transport", C.POINTER(Transport)), ("sig_in", C.c_void_p), ("sig_audio", C.c_void_p),
                ("sig_value", C.c_void_p)]


class Voice(C.Structure):       # rmp3_voice, 40 B
    _fields_ = [("shared", C.c_void_p), ("arena", C.c_void_p), ("shared_bytes", C.c_uint32), ("arena_bytes", C.c_uint32),
                ("slot", C.c_int32), ("voice", C.c_int32), ("instance", C.c_int32), ("place", C.c_uint32)]


RENDER3 = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(Block))
ATTACH3 = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(Voice))


class Plugin3(C.Structure):     # rmp3_plugin, 136 B
    _fields_ = [("magic", C.c_uint32), ("size", C.c_uint32), ("minor", C.c_uint32), ("kind", C.c_uint32),
                ("needs", C.c_uint32), ("wants", C.c_uint32), ("shared_min", C.c_uint32), ("shared_want", C.c_uint32),
                ("voice_min", C.c_uint32), ("voice_want", C.c_uint32), ("states", C.c_uint16),
                ("latency_frames", C.c_uint16), ("out_channels", C.c_uint8), ("in_channels", C.c_uint8),
                ("aux_channels", C.c_uint8), ("places", C.c_uint8), ("sig_out_n", C.c_uint8), ("sig_in_n", C.c_uint8),
                ("pad", C.c_uint8 * 6), ("sig_out", C.c_void_p), ("sig_in", C.c_void_p), ("setup", C.c_void_p),
                ("voice_attach", ATTACH3), ("render", RENDER3), ("view", C.c_void_p), ("reserved", C.c_void_p * 4)]


assert C.sizeof(Block) == 112 and C.sizeof(Plugin3) == 136 and C.sizeof(Voice) == 40 and C.sizeof(Transport) == 32


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


HOST = Host(3, float(FS), 16, _log, _random)
RMP3_MAGIC, RMP3_AT_SEND, RMP3_B_WET_ONLY = 0x33504D52, 1 << 3, 1 << 1
RMP3_T_RUNNING, RMP3_T_LOCKED = 1 << 0, 1 << 2


class Effect:
    """one state of an effect on a send slot, as the host runs it"""

    def __init__(self, name):
        self.lib = C.CDLL(str(HERE / name / f"lib{name}.dylib"))
        self.pl = Plugin.in_dll(self.lib, "rmp_entry")
        assert self.pl.magic == 0x504D5252 and self.pl.api == 3, f"{name}: not an API 3 only table"
        self.p3 = C.cast(self.pl.reserved[4], C.POINTER(Plugin3)).contents
        assert self.p3.magic == RMP3_MAGIC and self.p3.kind == 2, f"{name}: not an effect"
        assert self.pl.init(C.byref(HOST)) == 0, f"{name}: init refused"
        self.state = C.create_string_buffer(self.pl.state_bytes + 64)
        self.sp = C.cast(self.state, C.c_void_p)
        self.pl.voice_init(self.sp)
        nbytes = self.p3.voice_want or self.p3.voice_min
        self.arena = C.create_string_buffer(nbytes) if nbytes else None   # zeroed, as the host zeroes it at the load
        v = Voice(None, C.cast(self.arena, C.c_void_p) if self.arena else None, 0, nbytes, 64, -1, 0, RMP3_AT_SEND)
        if self.p3.voice_attach:
            self.p3.voice_attach(self.sp, C.byref(v))
        self.man = json.loads((HERE / name / "manifest.json").read_text())

    def defaults(self):
        p = [64.0] * 4
        for k in self.man.get("knobs", []):
            if k["p"] <= 4:
                p[k["p"] - 1] = float(k.get("default", 64))
        return p

    def run(self, x, p, bpm=120.0, running=False):
        """x (mono float32) through the effect; p: P1..P4 (a list, or a function of the block's start frame)"""
        n = len(x) // BLOCK * BLOCK
        L, R = np.zeros(n, np.float32), np.zeros(n, np.float32)
        lb, rb, ib = (C.c_float * 16)(), (C.c_float * 16)(), (C.c_float * 16)()
        P = (C.c_float * 16)()
        tick_us = 60e6 / (24.0 * bpm)
        tr = Transport(0, bpm, tick_us, 0.0, -1, 0.0, 0, 385)
        b = Block(112, BLOCK, RMP3_B_WET_ONLY, 0)
        b.out[0] = C.cast(lb, C.POINTER(C.c_float))
        b.out[1] = C.cast(rb, C.POINTER(C.c_float))
        b.inp[0] = C.cast(ib, C.POINTER(C.c_float))
        b.p = C.cast(P, C.POINTER(C.c_float))
        b.place = RMP3_AT_SEND
        b.transport = C.pointer(tr)
        last = None
        for i in range(0, n, BLOCK):
            q = p(i) if callable(p) else p
            for k in range(4):
                P[k] = float(q[k])
            b.p_changed = 0 if last == list(q) else 0xF
            last = list(q)
            if running:
                t = i / FS * 1e6 / tick_us
                tr.flags = RMP3_T_RUNNING | RMP3_T_LOCKED
                tr.song_tick, tr.song_frac = int(t), t - int(t)
            ib[:BLOCK] = x[i:i + BLOCK]
            b.gen = i // BLOCK
            self.p3.render(self.sp, C.byref(b))
            L[i:i + BLOCK] = lb[:BLOCK]
            R[i:i + BLOCK] = rb[:BLOCK]
        return L, R


# ---- test material (made here: no samples needed)
def chord(seconds=6.0, amp=0.18):
    """Cmaj9 as detuned saw pads, a slow attack"""
    t = np.arange(int(seconds * FS)) / FS
    y = np.zeros_like(t)
    for f in (130.81, 164.81, 196.0, 246.94, 293.66):
        for d in (-0.004, 0.004):
            ph = (t * f * (1 + d)) % 1.0
            y += 2 * ph - 1
    env = np.minimum(1.0, t / 0.3) * np.minimum(1.0, (seconds - t) / 0.5)
    return (amp * y / 10 * env).astype(np.float32)


def drums(seconds=6.0, bpm=120.0):
    """kick, snare, closed hat on 16ths: a plain 4/4 loop"""
    n = int(seconds * FS)
    y = np.zeros(n, np.float32)
    rng = np.random.default_rng(3)
    step = int(FS * 60 / bpm / 4)
    for s in range(n // step):
        at = s * step
        if s % 4 == 0:
            t = np.arange(min(9600, n - at)) / FS
            y[at:at + len(t)] += 0.8 * np.sin(2 * np.pi * (50 + 120 * np.exp(-t * 30)) * t) * np.exp(-t * 9)
        if s % 8 == 4:
            t = np.arange(min(7200, n - at)) / FS
            y[at:at + len(t)] += 0.5 * (rng.standard_normal(len(t)) * 0.6 + np.sin(2 * np.pi * 190 * t)) * np.exp(-t * 18)
        t = np.arange(min(2400, n - at)) / FS
        hat = rng.standard_normal(len(t))
        hat = np.diff(hat, prepend=0.0)
        y[at:at + len(t)] += 0.12 * hat * np.exp(-t * 60)
    return np.clip(y, -1, 1).astype(np.float32)


def write_wav(path, dry, L, R):
    x = np.concatenate([np.stack([dry, dry], 1), np.zeros((FS // 2, 2), np.float32), np.stack([L, R], 1)])
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(pcm.tobytes())


def check(name, what, L, R, tail_from=None, limit=1.0):
    ok = True
    for c, y in (("L", L), ("R", R)):
        bad = ~np.isfinite(y)
        sub = (y != 0) & (np.abs(y) < np.finfo(np.float32).tiny)
        pk = float(np.max(np.abs(y[np.isfinite(y)]))) if len(y) else 0.0
        if bad.any() or sub.any() or pk > limit:
            print(f"  FAIL {name} {what} {c}: nonfinite {int(bad.sum())}, denormals {int(sub.sum())}, peak {pk:.3f}")
            ok = False
    if tail_from is not None:
        t = float(np.sqrt(np.mean(np.square(L[-FS:], dtype=np.float64))))
        if t > 1e-3:
            print(f"  FAIL {name} {what}: the tail still at {t:.2e} after the input stopped")
            ok = False
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*", default=["ufx_phaser", "ufx_bbd"])
    for k in range(1, 5):
        ap.add_argument(f"--p{k}", type=float)
    ap.add_argument("--bpm", type=float, default=120.0)
    a = ap.parse_args()
    out = HERE / "build-wav"
    out.mkdir(exist_ok=True)
    ok = True
    for name in a.names:
        fx = Effect(name)
        p = fx.defaults()
        for k in range(4):
            v = getattr(a, f"p{k + 1}")
            if v is not None:
                p[k] = v
        print(f"{name}: P1..P4 {p}, {a.bpm:g} BPM")
        for what, x in (("chord", chord()), ("drums", drums(bpm=a.bpm))):
            x = np.concatenate([x, np.zeros(FS * 8, np.float32)])   # 8 s for the tail to end
            fx = Effect(name)
            L, R = fx.run(x, p, a.bpm, running=True)
            ok &= check(name, what, L, R, tail_from=len(x) - FS * 8)
            write_wav(out / f"{name}-{what}.wav", x, L, R)
            rms = lambda y: 20 * np.log10(max(1e-9, float(np.sqrt(np.mean(np.square(y[:len(y) - FS * 8], dtype=np.float64))))))
            print(f"  {what}: in {rms(x):.1f} dBFS, out L {rms(L):.1f} R {rms(R):.1f} dBFS, peak {float(np.max(np.abs(L))):.3f}")
        # the knobs at their ends, swept every 2 s across noise: nothing but bounded, finite numbers
        x = (np.random.default_rng(5).standard_normal(FS * 16) * 0.5).astype(np.float32)
        corners = [[0, 0, 0, 0], [127, 127, 127, 127], [63, 127, 127, 0], [127, 0, 127, 127], [0, 127, 0, 127]]
        L, R = Effect(name).run(x, lambda i: corners[(i // (FS * 2)) % len(corners)], a.bpm)
        ok &= check(name, "corners", L, R)
    print("ufx_render: PASS" if ok else "ufx_render: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
