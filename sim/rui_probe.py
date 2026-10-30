#!/usr/bin/env python3
"""Measure the Rytm's own text drawing through 0026's 3D in rytm-emu (rytm-mods 0026-remote-ui, 1021): a tiny host on
the emulator's Overbridge link that, when the remote page opens, sends one frame (pixels + text slots) and keeps the
page alive; the emulator's last panel screen comes back as a 1024 B framebuffer (and a PNG). Firmware-derived output
(screens of the Rytm's fonts) stays outside git: --out defaults to sim/build/rui_probe (git-ignored).

    rui_probe.py ITEMS.json [--fill RECTS.json] [--out DIR] [--name NAME]

ITEMS: [[slot, x, y, font, flags, "text"], ...]; RECTS: [[x, y, w, h], ...] set pixels in the frame under the text.
Also importable: probe(items, rects) -> framebuffer bytes (the OLED), used by rui_fonts.py.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402
from rui_e2e import oled  # noqa: E402

EMU = os.environ.get("RYTM_EMU", "../rytm-emu")
TAG = os.environ.get("RUI_TAG", "rui1021")


def pset(fb, x, y):
    fb[(7 - y // 8) + 8 * x] |= 1 << (y % 8)


def host(port, items, rects, done, echo=()):
    t0 = time.time()
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            if time.time() - t0 > 60:
                return
            time.sleep(0.05)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.05)
    s.sendall(bytes([ord("H"), 1, 1 | 2 | 4]))
    fb = bytearray(1024)
    for x0, y0, w, h in rects:
        for x in range(x0, x0 + w):
            for y in range(y0, y0 + h):
                pset(fb, x, y)
    frames, last, buf, rm, on = 0, 0.0, b"", b"", False

    def midi(m):
        nonlocal last
        s.sendall(b"M" + struct.pack("<H", len(m)) + m)
        last = frames / 48000
    while not done.is_set():
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        except socket.timeout:
            pass
        except OSError:
            break
        while buf:
            k = buf[0:1]
            if k == b"N" and len(buf) >= 5:
                n = struct.unpack("<I", buf[1:5])[0]
                buf = buf[5:]
                s.sendall(b"a" + struct.pack("<I", n) + bytes(16 * n))
                frames += n
            elif k == b"H" and len(buf) >= 3:
                buf = buf[3:]
            elif k == b"M" and len(buf) >= 3 and len(buf) >= 3 + struct.unpack("<H", buf[1:3])[0]:
                n = struct.unpack("<H", buf[1:3])[0]
                rm += buf[3:3 + n]
                buf = buf[3 + n:]
            elif k == b"T" and len(buf) >= 4:
                buf = buf[4:]
            elif k == b"P" and len(buf) >= 10:
                buf = buf[10:]
            elif k == b"R" and len(buf) >= 5 and len(buf) >= 5 + 48 * struct.unpack("<I", buf[1:5])[0]:
                buf = buf[5 + 48 * struct.unpack("<I", buf[1:5])[0]:]
            else:
                break
        while b"\xf0" in rm and b"\xf7" in rm[rm.index(b"\xf0"):]:
            i = rm.index(b"\xf0")
            j = rm.index(b"\xf7", i)
            msg, rm = rm[i:j + 1], rm[j + 1:]
            r = P.parse(msg)
            if r and r[0] == P.RUI_MODE and not on and r[1][2]:
                on = True
                seq = 5
                out = P.rui_text_clear(seq) + P.rui_rect(seq, 0, 0, 16, 8, fb)
                out += b"".join(P.rui_text(seq, *it) for it in items)
                out += b"".join(P.rui_echo(*e) for e in echo)
                midi(out + P.rui_commit(seq))
        if frames and frames / 48000 - last >= 0.25:
            midi(P.rui_keepalive())
    s.close()


def probe(items, rects=(), name="probe", out=None, ms=1400, events=(), echo=()):
    out = out or os.path.join(HERE, "build", "rui_probe")
    os.makedirs(out, exist_ok=True)
    clock = json.load(open(os.path.join(EMU, f"sections/us_{TAG}.json")))["clock"]
    at = lambda m: f"{clock + m / 5:g}M"   # noqa: E731
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    cmd = [os.path.join(EMU, "target/release/dtboot"), f"fw/AR173_us_{TAG}.syx", "--load", f"sections/us_{TAG}.snap",
           "--card", f"sections/us_{TAG}.img", "--instr", f"{ms // 5}M", "--every", "100M", "--usb-host",
           "--usb-midi-real", "--ob-link", f"127.0.0.1:{port}", "--press", f"TRK@{at(300)}:60M", "--press",
           f"YES@{at(400)}"]
    for e in events:
        cmd += list(e(at))
    done = threading.Event()
    th = threading.Thread(target=host, args=(port, items, rects, done, echo), daemon=True)
    th.start()
    r = subprocess.run(cmd, cwd=EMU, env=dict(os.environ, DTBOOT_OLED="1"), capture_output=True, text=True)
    done.set()
    th.join(5)
    txt = r.stdout + r.stderr
    open(os.path.join(out, f"{name}.dt.txt"), "w").write(txt)
    fb = oled(txt)
    if fb:
        from rui_render import frame_png
        frame_png(os.path.join(out, f"{name}.png"), fb)
    return fb, txt


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("items")
    ap.add_argument("--fill", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--name", default="probe")
    a = ap.parse_args()
    fb, _ = probe(json.load(open(a.items)), json.load(open(a.fill)) if a.fill else (), a.name, a.out)
    print("screen:", os.path.join(a.out or os.path.join(HERE, "build", "rui_probe"), a.name + ".png") if fb else "none")
