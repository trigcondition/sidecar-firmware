#!/usr/bin/env python3
"""The remote UI's pages (core/ui, the code the Pi and the Teensy run: core/build/libcore.dylib) rendered on the Mac
to PNG, as the Rytm's 128x64 panel shows them: SIDECAR and SYSTEM (status), the PLAITS page of a voice, the widget gallery
(two pages: every widget kind), and the per-page draw cost; 1023: the connect splash's frames (splash_*ms.png); the LFO
page's views (lfo_*.png).

    python3 sim/rui_render.py [--out DIR] [--scale N]      (default sim/build/rui, git-ignored; scale 4)

Also reads a 1024 B frame in the Rytm's framebuffer layout (remote_ui.py --frame, rui_link.py --frame): --frame FILE.
"""
import argparse
import ctypes
import os
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
LIB = os.path.join(HERE, "..", "core", "build", "libcore.dylib")
ON, OFF = (232, 238, 255), (12, 14, 22)   # the panel's white on black, a little blue


def pixels(fb):
    """1024 B, the Rytm's layout -> rows of booleans (y from the top)."""
    return [[bool(fb[(7 - y // 8) + 8 * x] >> (y % 8) & 1) for x in range(128)] for y in range(64)]


def png(path, rows, scale=4, grid=False):
    h, w = len(rows) * scale, len(rows[0]) * scale
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            c = ON if rows[y // scale][x // scale] else OFF
            if grid and scale >= 3 and (x % scale == scale - 1 or y % scale == scale - 1):
                c = tuple(int(v * 0.8) for v in c)
            raw += bytes(c)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def frame_png(path, fb, scale=4):
    png(path, pixels(fb), scale, grid=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "build", "rui"))
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--frame", help="a 1024 B frame file to render instead (next to it, .png)")
    a = ap.parse_args()
    if a.frame:
        fb = open(a.frame, "rb").read()
        frame_png(a.frame + ".png", fb, a.scale)
        print(a.frame + ".png")
        return
    os.makedirs(a.out, exist_ok=True)
    lib = ctypes.CDLL(LIB)
    lib.core_c_begin.argtypes = [ctypes.c_char_p]
    lib.core_c_sysex.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.core_c_rui_render.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
    lib.core_c_rui_status.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.core_c_rui_bench.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_float)]
    lib.core_c_sendfx_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.core_c_begin(tempfile.mkdtemp(prefix="rui_card_").encode())
    # a scene: voices 1-3, 5 and 6 PLAITS, 4 JUNO, 7 FM, 8 DAISYSP; FX1 CHORUS PRE, FX2 COMB POST
    import protocol as P
    for vi, m in enumerate((0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x48, 0x10)):
        knobs = [100, 90, 40, 70, 60, 30, 71, 110, 64, 64, 0, 0, 0, 120, 0, 100] if vi == 2 else [45] + [64] * 15
        msg = bytes([0xF0, 0x7D, 0x52, 0x54, 0x03, m | vi] + [b for k in knobs for b in (k, 0)] + [0xF7])
        lib.core_c_sysex(msg, len(msg))
    r = ctypes.create_string_buffer(400)
    for c in (b"sendknob=1:0:1", b"sendknob=1:1:20", b"sendknob=2:0:2", b"sendknob=2:1:100"):
        lib.core_c_sendfx_command(c, r, 400)
    lib.core_c_rui_status(1, 0, 23, 18, 4)
    fb = ctypes.create_string_buffer(1024)
    gallery = [bytes([20, 100, 0, 10, 50, 40, 70, 0, 2, 40, 36, 112, 14, 110, 0, 0]),
               bytes([5, 100, 30, 90, 0, 70, 20, 40, 60, 80, 70, 96, 0, 0, 0, 0])]
    shots = [("sidecar", 0, 0, None), ("system", 4, 0, None), ("plaits_v3", 1, 2, None),
             ("gallery_filter_env", 2, 0, gallery[0]),
             ("gallery_widgets", 3, 0, gallery[1])]
    rows_all = []
    for name, which, voice, vals in shots:
        if not lib.core_c_rui_render(which, voice, vals, fb):
            sys.exit(f"no page {name}")
        path = os.path.join(a.out, f"{name}.png")
        frame_png(path, fb.raw, a.scale)
        rows_all += pixels(fb.raw) + [[False] * 128] * 4
        out = (ctypes.c_float * 2)()
        lib.core_c_rui_bench(which, voice, 2000, out)
        print(f"{path}: whole page {out[0]:.1f} us, one change {out[1]:.1f} us (Mac; Pi x 13.2: {out[0] * 13.2:.0f} / "
              f"{out[1] * 13.2:.0f} us)")
    png(os.path.join(a.out, "all_pages.png"), rows_all[:-4], 3)
    print(os.path.join(a.out, "all_pages.png"))
    # 1023: the connect splash (which 5) at t = vals[0] x 25 ms: the icon sliding in, then the line running down
    for t in (0, 50, 100, 150, 1500, 2900):
        lib.core_c_rui_render(5, 0, bytes([t // 25] + [0] * 15), fb)
        path = os.path.join(a.out, f"splash_{t:04d}ms.png")
        frame_png(path, fb.raw, a.scale)
        print(path)
    # the LFO page (core/lfo.h, page_lfo.cpp; which 6: the first page drawn by code): L1 on voice 3's TIM (a sine,
    # synced 1/8), L2 on BD's filter frequency (DESIGN, the RAMP preset then two steps edited), L3 on FX1's P3; each
    # view at a moment of its cycle (the dot / the playhead), and the design view
    lib.core_c_fake_time.argtypes = [ctypes.c_int, ctypes.c_uint64]
    lib.core_c_lfo_tick.argtypes = [ctypes.c_uint32]
    lib.core_c_lfo_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.core_c_lfo_page.argtypes = [ctypes.c_int] * 3
    for c in ("lfo=1:tgt:v3", "lfo=1:par:2", "lfo=1:wav:0", "lfo=1:sync:1", "lfo=1:rate:5", "lfo=1:dep:24", "lfo=1:trig:1",
              "lfo=2:tgt:bd", "lfo=2:par:0", "lfo=2:pre:steps", "lfo=2:st:6:40:2", "lfo=2:st:10:-50:1", "lfo=2:dep:-40",
              "lfo=2:base:72", "lfo=2:rate:40", "lfo=3:tgt:fx1", "lfo=3:par:1", "lfo=3:wav:6", "lfo=3:dep:16"):
        lib.core_c_lfo_command(c.encode(), r, 400)
    us = 1_000_000
    lib.core_c_fake_time(1, us)
    for sel, design, ms, name in ((0, 0, 300, "lfo_main_l1"), (1, 0, 900, "lfo_main_l2_design_wave"), (2, 0, 400, "lfo_main_l3_fx"),
                                  (1, 1, 700, "lfo_design_l2")):
        for _ in range(ms):
            us += 1000
            lib.core_c_fake_time(1, us)
            lib.core_c_lfo_tick(us & 0xFFFFFFFF)
        lib.core_c_lfo_page(sel, design, 6)
        lib.core_c_rui_render(6, 0, None, fb)
        path = os.path.join(a.out, f"{name}.png")
        frame_png(path, fb.raw, a.scale)
        print(path)
    lib.core_c_fake_time(0, 0)


if __name__ == "__main__":
    main()
