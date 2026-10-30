#!/usr/bin/env python3
"""The Sidecar sequencer (core/seq/seq.h) in the real engine core, driven by a simulated Rytm: its 2C step ticks and
2D state (sim/protocol.py, the reference), stamped with its audio blocks and delivered with USB-like jitter.

    make -C core lib && uv run python sim/seq_sim.py [--out DIR] [--bars 4] [--bpm 120] [--lat 192]

Writes into DIR (default core/build/seq_sim):
  seq_strum.wav        stereo, 48 kHz: left the Sidecar's voice (6-OP FM, E.PIANO 1) playing the demo pattern of broken
                       chords and loose-timed notes, right a click where the Rytm plays each step (as the Sidecar's
                       render timeline sees it: a note with micro 0 lands on its click)
  seq_timeline.png     one bar of the left channel's envelope over the 16 steps (the clicks red): strums as staircases
  seq_page_play.png    the remote SEQ page (the Rytm's 128x64 screen, the text in the host's font) and, under it, the
                       16 trig LEDs and 4 PAGE LEDs in the Rytm's palette, while playing
  seq_page_held.png    a trig held: the step's keyboard, timing lane, the encoder values
  seq_page_strum.png   the same after turning STRUM (G)
  seq_page_map.png     PAGE held: the trig LEDs as the page map
  seq_page_record.png  live recording (REC) of a few loose notes, the page after
and prints the sequencer's stats.
"""
import argparse
import ctypes
import math
import os
import random
import struct
import sys
import tempfile
import wave
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402
from coreengine import CoreEngine  # noqa: E402

SR = 48000
STEP = 12   # frames a render call (one core block: arrivals land between blocks, as on the Pi's control core)

# the Rytm's palette (rytm-mods 0026 rui1026): ramps of 16 - red, green, blue, amber, cyan, magenta, white
HUES = [(255, 40, 30), (40, 255, 70), (50, 110, 255), (255, 170, 20), (30, 230, 255), (255, 50, 220), (255, 255, 255)]


def palette(c):
    if c >= 112 or c % 16 == 0:
        return (28, 28, 34)
    h, i = HUES[c // 16], (c % 16) / 15
    return tuple(int(28 + (x - 28) * (0.12 + 0.88 * i * i)) for x in h)


def png(path, rgb):
    """rgb: an (h, w, 3) uint8 array."""
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    print("wrote", path)


class Sim:
    def __init__(self, bpm, lat, jitter_ms, seed=7):
        self.card = tempfile.mkdtemp(prefix="seq-card-")
        os.makedirs(os.path.join(self.card, "Samples"))
        self.core = CoreEngine(self.card)
        L = self.core.lib
        self.lib = L
        L.core_c_fake_time.argtypes = [ctypes.c_int, ctypes.c_uint64]
        L.core_c_poly_midi.argtypes = [ctypes.c_int] * 3
        L.core_c_seq_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        L.core_c_seq_leds.argtypes = [ctypes.c_char_p]
        L.core_c_seq_frames.restype = ctypes.c_uint64
        L.core_c_seq_view_set.argtypes = [ctypes.c_int] * 4
        L.core_c_rui_tick.argtypes = [ctypes.c_uint32]
        L.core_c_rui_status.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.core_c_rui_frame_preview.argtypes = [ctypes.c_char_p]
        L.core_c_rui_status(1, 0, -1, -1, -1)
        self.rnd = random.Random(seed)
        self.bpm, self.period = bpm, SR * 60 / bpm / 4
        self.jitter = jitter_ms * SR / 1000
        self.delay = int(0.0015 * SR)   # 1.5 ms: the Rytm -> host path at its fastest
        self.frames = 0
        self.wire = []      # (host frame it arrives, message)
        self.left, self.right = [], []
        self.clicks = []    # host frames of the Rytm's steps as the render timeline sees them
        self.lat = lat
        self.t0 = None
        self.k = 0          # the next tick
        self.playing = False
        self.time()
        self.send(bytes([0xF0, 0x7D, 0x52, 0x54, 0x10]) + b"SEQSIM" + b"\xf7")
        # BD (track 0, voice 0) on 6-OP FM: PAT 10 (ROM E.PIANO 1), TIM MOR 64, DEC 127 (its own envelopes), LEV 100
        p = [10, 64, 64, 127, 64, 64, 100, 127] + [0] * 8
        self.send(P.params_report(0x48 | 0, [x << 7 for x in p]))   # 14-bit: value << 7
        self.send(P.rui_machines(0, [[40, 0x48 | 0]] + [[0, 0x7F]] * 11))
        self.cmd(f"seq=lat:{lat}")
        self.cmd("seq=demo:1")
        self.send(P.seq_tempo(bpm))

    def time(self):
        self.lib.core_c_fake_time(1, 1_000_000 + self.frames * 1_000_000 // SR)

    def ms(self):
        return 1000 + self.frames * 1000 // SR

    def send(self, m):
        self.core.report(bytes(m))

    def cmd(self, c):
        b = ctypes.create_string_buffer(700)
        self.lib.core_c_seq_command(c.encode(), b, 700)
        return b.value.decode()

    def stamp_send(self, rytm_frame, m):
        self.wire.append((rytm_frame + self.delay + int(self.rnd.random() * self.jitter), m))

    def play(self):
        self.t0 = self.frames + 600
        self.k = 0
        self.playing = True
        self.stamp_send(self.t0, P.seq_transport(P.SEQ_PLAY, self.t0 // 32))

    def stop(self):
        self.playing = False
        self.stamp_send(self.frames, P.seq_transport(P.SEQ_STOP, self.frames // 32))

    def run(self, n):
        end = self.frames + n
        while self.frames < end:
            # the Rytm: its steps fire at the start of the audio block they fall in
            while self.playing:
                tf = self.t0 + int(round(self.k * self.period))
                if tf > self.frames:
                    break
                blk = tf // 32
                self.stamp_send(blk * 32, P.seq_tick(blk, [(0, self.k % 16, 16, 2)]))
                self.clicks.append(blk * 32 + self.delay - self.lat)
                self.k += 1
            for item in [w for w in self.wire if w[0] <= self.frames]:
                self.wire.remove(item)
                self.send(item[1])
            self.time()
            a = self.core.frames(STEP)
            self.left.append(a[:, 0].astype(np.int16))
            self.frames += STEP
            if self.frames % 480 == 0:   # 10 ms: the control loop's ticks
                self.lib.core_c_poly_tick()
                self.lib.core_c_rui_tick(ctypes.c_uint32(self.ms()))
            for m in self.core.take_sysex():
                pass

    def page(self):
        for _ in range(3):
            self.run(30 * SR // 1000)
        fb = ctypes.create_string_buffer(1024)
        self.lib.core_c_rui_frame_preview(fb)
        leds = ctypes.create_string_buffer(20)
        self.lib.core_c_seq_leds(leds)
        return bytes(fb.raw), list(leds.raw)

    def panel(self, path, fb, leds):
        """The screen (x4) over the trig LED row and the 4 PAGE LEDs, as on the Rytm."""
        s, W = 4, 128 * 4
        img = np.zeros((64 * s + 96, W + 32, 3), np.uint8)
        img[:] = (18, 18, 22)
        for y in range(64):
            for x in range(128):
                on = fb[(7 - y // 8) + 8 * x] >> (y % 8) & 1
                c = (232, 238, 255) if on else (12, 14, 22)
                img[16 + y * s:16 + y * s + s - 1, 16 + x * s:16 + x * s + s - 1] = c
        y0 = 64 * s + 40
        for k in range(16):   # T1..T16 under the screen, 4 groups of 4
            x = 20 + k * 30 + (k // 4) * 6
            img[y0:y0 + 18, x:x + 22] = palette(leds[k])
        for i in range(4):    # the PAGE LEDs: page 1..4 left to right, small, above the trigs' right end
            x = W + 32 - 4 * 20 - 8 + i * 20
            img[y0 - 22:y0 - 14, x:x + 14] = palette(leds[16 + i])
        png(path, img)


def timeline(path, sim, bar_start, bar_len, steps=16):
    """bar_len frames of the left channel's envelope (white) from bar_start over `steps` steps (dotted), the Rytm's
    step clicks (red)."""
    a = np.concatenate(sim.left).astype(np.float32) / 32768
    W, H = 1024, 220
    img = np.zeros((H, W, 3), np.uint8)
    img[:] = (14, 14, 20)
    seg = a[bar_start:bar_start + bar_len]
    per = bar_len / W
    for x in range(W):
        c = seg[int(x * per):int((x + 1) * per)]
        if not len(c):
            continue
        h = int(min(1.0, float(np.abs(c).max()) * 1.6) * (H - 30))
        img[H - 10 - h:H - 10, x] = (220, 225, 245)
    for k in range(steps + 1):
        x = min(W - 1, int(k * W / steps))
        img[0:H:4, x] = (90, 90, 110) if k % 4 else (150, 150, 180)
    for f in sim.clicks:
        if bar_start <= f < bar_start + bar_len:
            x = int((f - bar_start) / per)
            img[0:14, max(0, x - 1):x + 2] = (255, 60, 50)
    png(path, img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "..", "core", "build", "seq_sim"))
    ap.add_argument("--bars", type=int, default=4)
    ap.add_argument("--bpm", type=float, default=110.0)
    ap.add_argument("--lat", type=int, default=192)
    ap.add_argument("--jitter", type=float, default=2.0, help="ms of arrival jitter on top of the fastest path")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    sim = Sim(a.bpm, a.lat, a.jitter)
    # the remote page open (as after the Rytm's 30 1), on SEQ
    sim.send(bytes([0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 1, 0, 0xF7]))
    sim.cmd("seq=show")
    sim.run(SR // 10)
    sim.play()
    bar = int(16 * sim.period)
    sim.run(int(a.bars * bar * 0.55))
    fb, leds = sim.page()
    sim.panel(os.path.join(a.out, "seq_page_play.png"), fb, leds)
    sim.run(int(a.bars * bar * 0.45) + bar // 4)
    # the WAV: left the voice, right the clicks; and one bar's timeline (bar 2: the clock settled)
    left = np.concatenate(sim.left)
    right = np.zeros_like(left)
    for f in sim.clicks:
        if 0 <= f < len(right) - 48:
            right[f:f + 48] = (np.sin(np.arange(48) * 2 * math.pi * 2000 / SR) * 9000 *
                               np.exp(-np.arange(48) / 12)).astype(np.int16)
    st = np.stack([left, right], axis=1).astype("<i2")
    wp = os.path.join(a.out, "seq_strum.wav")
    with wave.open(wp, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(st.tobytes())
    print(f"wrote {wp}: {len(left) / SR:.1f} s, peak {int(np.abs(left).max())}")
    t1 = sim.clicks[16] if len(sim.clicks) > 16 else 0
    timeline(os.path.join(a.out, "seq_timeline.png"), sim, t1, bar)
    # zoomed: the first step's slow upward strum and step 6's downward one (two steps each)
    timeline(os.path.join(a.out, "seq_strum_zoom.png"), sim, t1, int(2 * sim.period), steps=2)
    timeline(os.path.join(a.out, "seq_strum_down_zoom.png"), sim, t1 + int(6 * sim.period), int(2 * sim.period), steps=2)
    # a trig held on the first step: its notes; STRUM turned
    sim.lib.core_c_seq_view_set(0, 0, -1, 0)
    sim.send(P._msg(P.RUI_KEY, [0, 0, 1]))
    fb, leds = sim.page()
    sim.panel(os.path.join(a.out, "seq_page_held.png"), fb, leds)
    sim.send(P._msg(P.RUI_ENC, [6, 64 + 4, 0]))
    fb, leds = sim.page()
    sim.panel(os.path.join(a.out, "seq_page_strum.png"), fb, leds)
    sim.send(P._msg(P.RUI_KEY, [0, 0, 0]))
    sim.run(SR // 2)
    # PAGE held: the page map on the trig LEDs (seq mode on BD)
    sim.send(P.seq_seqmode(True, 0))
    sim.send(P._msg(P.RUI_KEY, [1, 4, 1]))
    fb, leds = sim.page()
    sim.panel(os.path.join(a.out, "seq_page_map.png"), fb, leds)
    sim.send(P._msg(P.RUI_KEY, [1, 4, 0]))
    sim.lib.core_c_seq_view_set(0, 1, -1, 0)
    # live record: a host keyboard on channel 1 bound to voice 0, REC on, loose notes on page 2 of the pattern
    b = ctypes.create_string_buffer(200)
    sim.lib.core_c_poly_command(b"poly=bind:1:0", b, 200)
    sim.send(P.seq_rec(True, False))
    k0 = (sim.k // 64 + 1) * 64 + 16   # page 2 of the next pass
    for i, (st_, n, ln) in enumerate([(0.1, 76, 0.8), (2.4, 79, 0.5), (4.0, 81, 1.6), (6.7, 84, 0.4), (9.2, 79, 2.2)]):
        at = sim.t0 + int((k0 + st_) * sim.period) + sim.delay
        sim.run(max(0, at - sim.frames))
        sim.lib.core_c_poly_midi(0x90, n, 100)
        sim.run(int(ln * sim.period))
        sim.lib.core_c_poly_midi(0x80, n, 0)
    sim.send(P.seq_rec(False, False))
    sim.run(int(4 * sim.period))
    sim.lib.core_c_seq_view_set(0, 1, -1, 0)
    fb, leds = sim.page()
    sim.panel(os.path.join(a.out, "seq_page_record.png"), fb, leds)
    sim.stop()
    sim.run(SR // 5)
    print(sim.cmd("seq"))


if __name__ == "__main__":
    main()
