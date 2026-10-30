#!/usr/bin/env python3
"""The free test instrument for MULTISAMPLE (docs/MULTISAMPLE.md): synthesised here, so it is ours to redistribute.

    python3 tools/make_multisample_test.py [OUT_DIR]     (default content/MULTISAMPLE/SidecarTest)

Five keys an octave apart (C2 C3 C4 C5 C6 = 36 48 60 72 84), two velocity layers each (1..79: a sine with a soft
second harmonic; 80..127: a band-limited saw), mono 16-bit 48 kHz, 1.25 s: a 20 ms attack, a decay to a steady level
by 0.25 s, then exactly whole periods to the end, marked as a loop in the file's smpl chunk (the tone's frequency is
set so the loop holds whole periods; the .sfz's `tune` puts each zone back on pitch). A release sample (a short
filtered noise breath, 44.1 kHz stereo 24-bit: the converter's other path) on trigger=release. Standard library only.
"""
import math
import os
import random
import struct
import sys

SR = 48000
LEN = int(1.25 * SR)
LOOP_S = int(0.25 * SR)
KEYS = [36, 48, 60, 72, 84]
NAMES = {36: "C2", 48: "C3", 60: "C4", 72: "C5", 84: "C6"}


def wav_bytes(frames, rate, channels, bits, loop=None):
    """frames: list of tuples of floats -1..1. loop: (start, end) inclusive, written as a smpl chunk."""
    bps = bits // 8
    data = bytearray()
    for fr in frames:
        for x in fr:
            v = max(-1.0, min(1.0, x))
            if bits == 16:
                data += struct.pack("<h", int(round(v * 32767)))
            else:
                i = int(round(v * 8388607))
                data += struct.pack("<i", i)[:3]
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * bps, channels * bps, bits)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(data)) + bytes(data) + (b"\0" if len(data) & 1 else b"")
    if loop:
        smpl = struct.pack("<9I", 0, 0, int(1e9 / rate), 60, 0, 0, 0, 1, 0)
        smpl += struct.pack("<6I", 0, 0, loop[0], loop[1], 0, 0)
        chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


def tone(key, loud):
    """The zone's samples and its tune (cents) so it plays on pitch."""
    f = 440.0 * 2 ** ((key - 69) / 12)
    loop_len = LEN - LOOP_S
    periods = max(1, round(loop_len * f / SR))
    f2 = periods * SR / loop_len           # whole periods in the loop
    tune = 1200 * math.log2(f / f2)
    nh = max(1, int((SR / 2 * 0.9) // f2)) if loud else 2
    out = []
    for n in range(LEN):
        ph = 2 * math.pi * f2 * n / SR
        if loud:
            x = sum(math.sin(h * ph) / h for h in range(1, nh + 1)) * 0.45
        else:
            x = math.sin(ph) * 0.5 + 0.08 * math.sin(2 * ph)
        t = n / SR
        env = min(1.0, t / 0.02) * (0.7 + 0.3 * math.exp(-t / 0.05)) if n < LOOP_S else 0.7
        if n >= LOOP_S:   # steady: the loop's amplitude is constant
            env = 0.7 + 0.3 * math.exp(-0.25 / 0.05)
        out.append((x * env,))
    return out, tune


def breath():
    rnd = random.Random(7)
    rate, n = 44100, int(0.3 * 44100)
    out, lp = [], 0.0
    for i in range(n):
        lp += 0.15 * (rnd.uniform(-1, 1) - lp)
        env = min(1.0, i / 200) * math.exp(-i / (0.08 * rate))
        out.append((lp * env * 0.6, lp * env * 0.5))
    return wav_bytes(out, rate, 2, 24)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "content", "MULTISAMPLE", "SidecarTest")
    os.makedirs(os.path.join(out, "samples"), exist_ok=True)
    lines = [
        "// SidecarTest: the free test instrument for the Sidecar's MULTISAMPLE machine (docs/MULTISAMPLE.md).",
        "// Synthesised by tools/make_multisample_test.py. Licence: CC0 (public domain).",
        "<control>",
        "default_path=samples/",
        "<global>",
        "ampeg_attack=0.002 ampeg_release=0.4 amp_veltrack=100",
    ]
    for i, k in enumerate(KEYS):
        lo = 0 if i == 0 else (KEYS[i - 1] + k) // 2 + 1
        hi = 127 if i == len(KEYS) - 1 else (k + KEYS[i + 1]) // 2
        lines.append(f"<group> lokey={lo} hikey={hi} pitch_keycenter={k}")
        for loud, (lv, hv) in ((False, (1, 79)), (True, (80, 127))):
            data, tune = tone(k, loud)
            name = f"{NAMES[k]}_{'loud' if loud else 'soft'}.wav"
            with open(os.path.join(out, "samples", name), "wb") as f:
                f.write(wav_bytes(data, SR, 1, 16, loop=(LOOP_S, LEN - 1)))
            lines.append(f"<region> sample={name} lovel={lv} hivel={hv} tune={tune:.3f} loop_mode=loop_sustain")
    with open(os.path.join(out, "samples", "release breath.wav"), "wb") as f:
        f.write(breath())
    lines.append("<group> trigger=release ampeg_release=0.2 volume=-12")
    lines.append("<region> sample=release breath.wav lokey=0 hikey=127")
    with open(os.path.join(out, "SidecarTest.sfz"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
