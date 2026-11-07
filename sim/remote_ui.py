#!/usr/bin/env python3
"""Remote UI M1 host (rytm-mods 0026-remote-ui; docs/superpowers/specs/2026-10-16-remote-ui-research.md section 6).

Plays the Sidecar against rytm-emu's Overbridge link (`dtboot --ob-link`, src/oblink.rs) with the hello for the MIDI
pair and voice frames, so the emulator's frame requests (N) are this script's clock: emulated time = frames / 48000,
whatever speed the emulator runs at. It waits for the Rytm's 30 (remote page on: TRK + YES on the Rytm, or --open
sends 3A), then sends a checkerboard with a 1-pixel border as one full frame (38 + 39), then moves an 8x8 tile one
tile a step at 20 Hz (only the two tiles that change), prints each 31 / 32 it receives and draws the last encoder's
value (0..127) as a bar along the bottom tile row. Idle, it sends 3C every 250 ms.

    remote_ui.py [--link HOST:PORT] [--secs S] [--open] [--stop-at S] [--full] [--log FILE] [--frame FILE]
                 [--mask SPEC] [--mask-at S=SPEC ...]

--stop-at S: stop answering and close at emulated S (a killed Sidecar); --full: send the whole frame on every step
(the UART-bound case); --log: one JSON line per event, with t = emulated seconds; --frame: the last committed frame
(1024 B, framebuffer layout) written at exit.
1023: --splash TICKS: answer the Rytm's hello (35, logged as {"ev": "hello", "up": .., "caps": ..}) with a splash frame
(the checkerboard inverted, full) and 36 TICKS; --splash-end-at S: 36 0 at emulated S.
1022: --mask SPEC: the catch mask (37) sent after every 30 on, SPEC "e=0,3;k=LEFT,NO,0x84" (encoders, key names of
protocol.RUI_KEYS or codes); --mask-at S=SPEC: a new mask at emulated S. MIDI transport from the Rytm (FA start, FB
continue, FC stop) and note-ons are logged as {"ev": "rt", "b": byte} / {"ev": "note", "status": .., "note": ..}.
0.5.0: the Rytm's 2D 01 reports as {"ev": "state", "kind": k, "b": [bytes after the kind]} (QPERF 0E, SCENE 0F, the
host command's ack 10; the sequencer's kinds too).
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import protocol as P  # noqa: E402

HELLO_MIDI, HELLO_NO_R, HELLO_VOICES = 1, 2, 4
RATE = 48000


def pset(fb, x, y, on=True):
    """Glass pixel (x, y from the top) in the framebuffer layout: byte (7 - y // 8) + 8 x, bit y % 8."""
    i, m = (7 - y // 8) + 8 * x, 1 << (y % 8)
    fb[i] = (fb[i] | m) if on else (fb[i] & ~m)


def checkerboard():
    fb = bytearray(1024)
    for x in range(128):
        for y in range(64):
            border = x in (0, 127) or y in (0, 63)
            pset(fb, x, y, border or ((x // 4 + y // 4) & 1 == 1))
    return fb


def tile_xy(n):
    """The moving tile's n-th position: along the tile grid's second row band, 16 columns x pages 1..6."""
    k = n % (16 * 6)
    return k % 16, 6 - k // 16   # (tile column, framebuffer page; page 7 is the top band)


def draw_tile(fb, col, page, on):
    for x in range(col * 8, col * 8 + 8):
        fb[page + 8 * x] = 0xFF if on else 0x00


def mask_msg(spec):
    encs, keys = [], []
    for part in spec.split(";"):
        if not part.strip():
            continue
        k, v = part.split("=", 1)
        for x in v.split(","):
            x = x.strip()
            if not x:
                continue
            if k.strip() == "e":
                encs.append(int(x, 0))
            else:
                keys.append(P.RUI_KEYS[x] if x in P.RUI_KEYS else int(x, 0))
    return P.rui_mask(encs, keys)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--secs", type=float, default=60.0, help="real-time limit")
    ap.add_argument("--open", action="store_true", help="open the page from the host (3A 0 1) at 0.5 s emulated")
    ap.add_argument("--stop-at", type=float, default=None)
    ap.add_argument("--close-at", type=float, default=None, help="send 3A 0 0 at emulated S")
    ap.add_argument("--still-at", type=float, default=None, help="stop moving the tile at emulated S (keepalives go on)")
    ap.add_argument("--full", action="store_true", help="every step a whole frame, the screen inverted (all 128 tiles)")
    ap.add_argument("--rate", type=float, default=20.0)
    ap.add_argument("--log", default=None)
    ap.add_argument("--frame", default=None)
    ap.add_argument("--mask", default=None, help="1022: the catch mask sent after each 30 on")
    ap.add_argument("--mask-at", action="append", default=[], help="1022: S=SPEC, a new mask at emulated S")
    ap.add_argument("--splash", type=int, default=None, help="1023: answer a hello with a splash of TICKS x 50 ms")
    ap.add_argument("--splash-end-at", type=float, default=None, help="1023: 36 0 at emulated S")
    a = ap.parse_args()
    mask_at = sorted((float(x.split("=", 1)[0]), x.split("=", 1)[1]) for x in a.mask_at)
    log = open(a.log, "w") if a.log else None
    h, p = a.link.rsplit(":", 1)
    t0 = time.time()
    while True:
        try:
            s = socket.create_connection((h, int(p)), timeout=1)
            break
        except OSError:
            if time.time() - t0 > 60:
                sys.exit(f"no link at {a.link}")
            time.sleep(0.2)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.05)
    s.sendall(bytes([ord("H"), 1, HELLO_MIDI | HELLO_NO_R | HELLO_VOICES]))

    frames = 0
    emu = lambda: frames / RATE   # noqa: E731
    seq, base, fb, step = 0, checkerboard(), None, 0
    on, next_step, last_tx, opened = False, 0.0, 0.0, False
    enc = {}
    bar = 0
    stats = dict(rects=0, commits=0, bytes=0, keys=0, encs=0)

    def ev(what, **kw):
        kw.update(t=round(emu(), 6), ev=what)
        if log:
            log.write(json.dumps(kw) + "\n")
            log.flush()

    def midi(m):
        nonlocal last_tx
        for i in range(0, len(m), 60000):
            c = m[i:i + 60000]
            s.sendall(b"M" + struct.pack("<H", len(c)) + c)
        stats["bytes"] += len(m)
        last_tx = emu()

    def commit(rects):
        nonlocal seq
        seq = (seq + 1) & 0x7F
        out = b"".join(P.rui_rect(seq, c0, p0, nc, np_, fb) for c0, p0, nc, np_ in rects) + P.rui_commit(seq)
        midi(out)
        stats["rects"] += len(rects)
        stats["commits"] += 1
        ev("commit", seq=seq, rects=rects)

    def draw_bar():
        for x in range(128):
            fb[0 + 8 * x] = base[0 + 8 * x]
            if x < bar:
                fb[0 + 8 * x] |= 0x3C   # glass rows 58..61 (page 0 = the bottom band)

    buf, rmidi = b"", b""
    while time.time() - t0 < a.secs:
        if a.stop_at is not None and emu() >= a.stop_at:
            ev("stop")
            break
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        except socket.timeout:
            pass
        while buf:
            k = buf[0:1]
            if k == b"N" and len(buf) >= 5:
                n = struct.unpack("<I", buf[1:5])[0]
                buf = buf[5:]
                s.sendall(b"a" + struct.pack("<I", n) + bytes(16 * n))
                frames += n
            elif k == b"H" and len(buf) >= 3:
                print(f"hello answered: flags {buf[2]:#x}", flush=True)
                buf = buf[3:]
            elif k == b"M" and len(buf) >= 3 and len(buf) >= 3 + struct.unpack("<H", buf[1:3])[0]:
                n = struct.unpack("<H", buf[1:3])[0]
                chunk = buf[3:3 + n]
                rmidi += chunk
                for i, c in enumerate(chunk):   # status bytes never occur inside SysEx data: transport and note-ons
                    if c in (0xFA, 0xFB, 0xFC):
                        ev("rt", b=c)
                    elif 0x90 <= c <= 0x9F and i + 2 < len(chunk) and chunk[i + 2]:
                        ev("note", status=c, note=chunk[i + 1])
                buf = buf[3 + n:]
            elif k == b"T" and len(buf) >= 4:
                buf = buf[4:]
            elif k == b"P" and len(buf) >= 10:
                buf = buf[10:]
            elif k == b"R" and len(buf) >= 5 and len(buf) >= 5 + 48 * struct.unpack("<I", buf[1:5])[0]:
                buf = buf[5 + 48 * struct.unpack("<I", buf[1:5])[0]:]
            elif k in (b"N", b"H", b"M", b"T", b"P", b"R"):
                break
            else:
                sys.exit(f"bad message {buf[:8]!r}")
        while b"\xf0" in rmidi and b"\xf7" in rmidi[rmidi.index(b"\xf0"):]:
            i = rmidi.index(b"\xf0")
            j = rmidi.index(b"\xf7", i)
            msg, rmidi = rmidi[i:j + 1], rmidi[j + 1:]
            r = P.parse(msg)
            if r and r[0] == P.SEQ_STATE and len(r[1]) >= 2 and r[1][0] == 1:   # 0.5.0: 2D 01 <kind> ... (0E, 0F, 10)
                ev("state", kind=r[1][1], b=list(r[1][2:]))
                continue
            if not r or not P.RUI_FIRST <= r[0] <= P.RUI_LAST:
                continue
            m = P.decode_rui(r[0], r[1])
            if m is None:
                print("rytm: bad remote-UI message", msg.hex(" "), flush=True)
                continue
            if r[0] == P.RUI_HELLO:
                print(f"[{emu():8.3f}] rytm: hello, up {m['up'] / 20:.2f} s, caps {m['caps']}", flush=True)
                ev("hello", **m)
                if a.splash is not None:   # whatever the page: the Rytm decides (it ignores 36 while open)
                    fb = bytearray(b ^ 0xFF for b in checkerboard())
                    commit([(0, 0, 16, 8)])
                    midi(P.rui_splash(a.splash))
                    ev("splash", ticks=a.splash)
                else:
                    midi(P.rui_keepalive())
            elif r[0] == P.RUI_MODE:
                print(f"[{emu():8.3f}] rytm: page {m['page']} {'on' if m['on'] else 'off'} ({m['reason']})", flush=True)
                ev("mode", **m)
                on = bool(m["on"])
                if on:
                    fb = bytearray(base)
                    step = 0
                    draw_bar()
                    if a.mask:
                        midi(mask_msg(a.mask))
                        ev("mask", spec=a.mask)
                    commit([(0, 0, 16, 8)])
                    next_step = emu() + 1.0 / a.rate
            elif r[0] == P.RUI_KEY:
                stats["keys"] += 1
                print(f"[{emu():8.3f}] rytm: key {m['key']:#04x} {'down' if m['down'] else 'up'}", flush=True)
                ev("key", **m)
            elif r[0] == P.RUI_ENC:
                stats["encs"] += 1
                v = max(0, min(127, enc.get(m["enc"], 0) + m["delta"]))
                enc[m["enc"]] = v
                print(f"[{emu():8.3f}] rytm: encoder {m['enc']} {m['delta']:+d} func {m['func']} -> {v}", flush=True)
                ev("enc", value=v, **m)
                if on:
                    bar = v
                    draw_bar()
                    commit([(0, 0, 16, 8)] if a.full else [(0, 0, 16, 1)])
        if a.open and not opened and emu() >= 0.5:
            midi(P.rui_open(0, 1))
            opened = True
            ev("open")
        while mask_at and emu() >= mask_at[0][0]:
            midi(mask_msg(mask_at[0][1]))
            ev("mask", spec=mask_at[0][1])
            mask_at.pop(0)
        if a.splash_end_at is not None and emu() >= a.splash_end_at:
            midi(P.rui_splash(0))
            a.splash_end_at = None
            ev("splash_end")
        if a.close_at is not None and on and emu() >= a.close_at:
            midi(P.rui_open(0, 0))
            a.close_at = None
            ev("close")
        if on and emu() >= next_step and (a.still_at is None or emu() < a.still_at):
            next_step += 1.0 / a.rate
            c0, p0 = tile_xy(step)
            for x in range(c0 * 8, c0 * 8 + 8):   # the old tile back to the checkerboard
                fb[p0 + 8 * x] = base[p0 + 8 * x]
            step += 1
            c1, p1 = tile_xy(step)
            draw_tile(fb, c1, p1, True)
            if a.full:
                fb = bytearray(b ^ 0xFF for b in fb)
                base = bytearray(b ^ 0xFF for b in base)
            commit([(0, 0, 16, 8)] if a.full else [(c0, p0, 1, 1), (c1, p1, 1, 1)])
        elif emu() - last_tx >= 0.25 and frames:
            midi(P.rui_keepalive())
    s.close()
    if a.frame and fb is not None:
        open(a.frame, "wb").write(bytes(fb))
    print(f"done at {emu():.3f} s emulated: {stats}", flush=True)
    ev("done", **stats)


if __name__ == "__main__":
    main()
