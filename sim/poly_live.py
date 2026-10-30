#!/usr/bin/env python3
"""The real engine core (core/build/libcore.dylib, poly FM included) as a live Overbridge host for rytm-emu on the Mac.

    make -C core lib && uv run python sim/poly_live.py [--link 127.0.0.1:7301] [--card DIR] [--wav OUT.wav] [--secs S]

The emulator is started with --usb-host --usb-midi-real --ob-link (window: `rytm FW.syx --card C.img --snapshot S
--usb-host --usb-midi-real --ob-link`). This host:
- answers every N (frames wanted) with the core's eight voices rendered (poly FM chords on 6-OP FM voices);
- feeds the Rytm's USB MIDI into the core: SysEx (0x04 trigs, 0x03 reports, 28 02 answers) to the engine,
  channel messages (its pads' notes) to poly::midi (live pad chords: monitoring, detection, the 28 01 write-back);
- sends the core's SysEx (28 01 ...) back to the Rytm over the link;
- runs the core's clock on the emulated one (frames / 48000), so poly's chord window and holds follow the emulation;
- logs the notes, the 28 01 / 28 02 messages and (every 5 s) the poly stats; --wav writes the voices' sum (mono);
- the Sidecar sequencer (core/seq/seq.h): the Rytm's 2C / 2D reach it with the rest, its 2E / 2F / 34 / 46 go back; --seq
  runs console commands at the start (e.g. "seq=track:1:on,seq=demo:1,seq=lat:192"); its stats every 5 s;
- LIVE IN (core/livein/livein.h): --livein V plays it on voice V (livein=voice:V) and feeds the R frames' AUDIO IN to it
  (implies --rytm-audio); --cmd TEXT runs a console command at the start (ins=..., out=..., livein=...; repeatable);
- seq-p2: --rytm-audio asks the emulator for the Rytm's own output (R: MAIN, the 8 voice stems, AUDIO IN) and feeds it
  to the sequencer's latency calibration (core_c_seq_rytm_audio); --rwav FILE keeps it (12 channels, 24-bit values as
  int32 .npy) for the checks; every MIDI byte also reaches the pattern dump reader (core_c_rytm_byte); --rytmpat turns
  the Rytm pattern fetch on (rytmpat=on) and --rytmpat-out FILE writes the last record (13101 B) at the end.
One engine per process (the core keeps module state).
"""
import argparse
import ctypes
import os
import shutil
import socket
import struct
import sys
import queue
import tempfile
import threading
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from coreengine import CoreEngine  # noqa: E402

HELLO_MIDI, HELLO_NO_R, HELLO_VOICES = 1, 2, 4
SR = 48000
CHUNK = 96


class PolyHost:
    def __init__(self, card, wav=None):
        self.core = CoreEngine(card)
        self.lib = self.core.lib
        self.lib.core_c_fake_time.argtypes = [ctypes.c_int, ctypes.c_uint64]
        self.lib.core_c_poly_midi.argtypes = [ctypes.c_int] * 3
        self.lib.core_c_poly_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        self.lib.core_c_msamp_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        # the remote UI (core/ui/remote.h): its tick draws the SRC / chord views and queues their SysEx like the engine's
        self.lib.core_c_rui_tick.argtypes = [ctypes.c_uint32]
        self.lib.core_c_rui_status.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        self.lib.core_c_rui_status(1, 0, -1, -1, -1)
        self.lib.core_c_rytm_byte.argtypes = [ctypes.c_int]
        self.lib.core_c_seq_rytm_audio.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.core_c_rytm_record.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32)]
        self.lib.core_c_livein_in.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.core_c_livein_fifo.argtypes = [ctypes.c_int]
        self.livein = False
        self.rframes = []   # --rwav: the Rytm's R frames (n x 12 int32)
        self.rwav = None
        self.rytm_gen = 0
        self.frames = 0
        self.msamp_on = False
        self.out = []
        self.wav = wav
        self.chunks = []
        self.ch = []        # a channel message being read
        self.sx = None      # a SysEx being read
        self.t0 = time.time()
        self.set_time()

    def t(self):
        return self.frames / SR

    def log(self, *a):
        print(f"[{self.t():8.3f}s]", *a, flush=True)

    def set_time(self):
        self.lib.core_c_fake_time(1, 1_000_000 + self.frames * 1_000_000 // SR)

    def msamp(self, c):
        """A console msamp / sdtest command (core/msamp/msamp.h): its answer."""
        b = ctypes.create_string_buffer(600)
        self.lib.core_c_msamp_command(c.encode(), b, 600)
        return b.value.decode()

    def seq(self, c):
        """A console seq command (core/seq/seq.h): its answer."""
        b = ctypes.create_string_buffer(700)
        self.lib.core_c_poly_command(c.encode(), b, 700)
        return b.value.decode()

    def poly_stats(self):
        b = ctypes.create_string_buffer(400)
        self.lib.core_c_poly_command(b"poly", b, 400)
        return b.value.decode()

    def render(self, n):
        """n frames x 8 voices, in small chunks so the core's clock and poly's tick follow."""
        while getattr(self, "timed", None) and self.timed[0][0] <= self.t():
            c = self.timed.pop(0)[1]
            self.log(f"at: {c} -> {self.seq(c)}")
        parts = []
        while n > 0:
            k = min(n, CHUNK)
            self.set_time()
            self.lib.core_c_poly_tick()
            self.core.idle()
            parts.append(self.core.frames(k))
            self.frames += k
            n -= k
            self.take()
        self.lib.core_c_rui_tick(ctypes.c_uint32(self.frames * 1000 // SR))
        self.take()
        a = np.concatenate(parts)
        if self.wav:
            self.chunks.append(a.astype(np.int32).sum(axis=1).clip(-32768, 32767).astype(np.int16))
        return a

    def take(self):
        for m in self.core.take_sysex():
            self.out.append(m)
            if len(m) > 5 and (m[4] in (0x28, 0x2E, 0x46) or m[:4] == b"\xf0\x00\x20\x3c"):
                self.log("host -> Rytm  " + m.hex(" "))

    def rytm_audio(self, data, n):
        """R: n frames x 12 int32 (LE) the Rytm sent: the calibration's stream, kept for --rwav."""
        a = np.frombuffer(data, dtype="<i4").reshape(n, 12)
        a = np.ascontiguousarray(a)
        self.lib.core_c_seq_rytm_audio(a.ctypes.data, n)
        if self.livein:
            self.lib.core_c_livein_in(a.ctypes.data, n)
        if self.rwav:
            self.rframes.append(a.copy())

    def rytm_record(self):
        """the pattern dump's last record (rytm_dump.h) and its pattern, (None, -1) when none"""
        out = ctypes.create_string_buffer(13101)
        gen = ctypes.c_uint32(0)
        p = self.lib.core_c_rytm_record(out, ctypes.byref(gen))
        if p >= 0 and gen.value != self.rytm_gen:
            self.rytm_gen = gen.value
            self.log(f"Rytm pattern dump: pattern {p}, record {len(out.raw)} B (gen {gen.value})")
        return (out.raw, p) if p >= 0 else (None, -1)

    def midi_byte(self, c):
        """The Rytm's USB MIDI as a byte stream."""
        self.lib.core_c_rytm_byte(c)
        if c == 0xF7:
            self.rytm_record()
        if self.sx is not None:
            if c == 0xF7:
                msg = bytes(self.sx) + b"\xf7"
                self.sx = None
                if len(msg) > 5 and (msg[4] == 0x28 or (msg[4] == 0x2D and len(msg) > 6 and msg[6] != 9)):
                    self.log("Rytm -> host  " + msg.hex(" "))
                self.core.report(msg)
                self.take()
                return
            if c & 0x80:
                self.sx = None   # (a status inside a SysEx: dropped)
            else:
                self.sx.append(c)
                return
        if c == 0xF0:
            self.sx = bytearray([c])
        elif c >= 0xF8:
            return
        elif c & 0x80:
            self.ch = [c]
        elif self.ch:
            self.ch.append(c)
            st = self.ch[0]
            need = 2 if st & 0xE0 == 0xC0 else 3
            if len(self.ch) == need:
                d2 = self.ch[2] if need == 3 else 0
                if st & 0xF0 in (0x80, 0x90):
                    self.log(f"Rytm pad  {'on ' if st & 0xF0 == 0x90 and d2 else 'off'} ch {(st & 15) + 1} note {self.ch[1]} vel {d2}")
                self.set_time()
                self.lib.core_c_poly_midi(st, self.ch[1], d2)
                self.ch = [st]   # running status
                self.take()

    # ---- a MIDI keyboard on the Mac, straight into the core's live poly path (no Rytm in between)
    def keys_setup(self, entries):
        """entries: [(keyboard channel 1..16, voice 0..7, chord keys?)]; core channel 16, 15, ... plays entry 0, 1, ..."""
        self.entries = entries
        self.sel = 0
        self.kq = queue.Queue()
        self.kheld = set()   # (entry, key): notes down
        self.bind_all()
        self.keys_on = 0

    def core_ch(self, i):
        return 15 - i

    def bind_all(self):
        """(Re)bind: the core forgets direct bindings on a link reset, and a pad binding can take a voice over."""
        for i, (_, vi, chord) in enumerate(self.entries):
            b = ctypes.create_string_buffer(200)
            self.lib.core_c_poly_command(f"poly=bind:{self.core_ch(i) + 1}:{vi}{':chord' if chord else ''}".encode(), b, 200)
        self.last_bind = time.time()

    def keys_select(self, i):
        if 0 <= i < len(self.entries) and i != self.sel:
            self.keys_off(self.sel)
            self.sel = i
            ch, vi, chord = self.entries[i]
            self.log(f"keyboard -> entry {i + 1}: channel {ch}, voice {vi}{' (chord keys)' if chord else ''}")

    def keys_off(self, i):
        for e, k in list(self.kheld):
            if e == i:
                self.kheld.discard((e, k))
                self.set_time()
                self.lib.core_c_poly_midi(0x80 | self.core_ch(e), k, 0)

    def entries_on(self):
        """Drain one queued keyboard message / control line (True: there was one)."""
        if not getattr(self, "kq", None):
            return False
        try:
            k, x = self.kq.get_nowait()
        except queue.Empty:
            if time.time() - self.last_bind > 1.0:
                self.bind_all()
            return False
        if k == "key":
            self.keys_msg(x)
        else:
            x[1].append(self.ctl(x[0]))
            x[2].set()
        return True

    def keys_msg(self, m):
        """A mido message from the keyboard: note on/off to the selected entry (the keyboard's channel picks one when it
        is in the map), program change N = entry N+1, CC 123 / 120 all notes off."""
        t = m.type
        if t in ("note_on", "note_off"):
            i = next((j for j, e in enumerate(self.entries) if e[0] == m.channel + 1), self.sel)
            self.log(f"keys {t} ch {m.channel + 1} note {m.note} vel {m.velocity} -> entry {i + 1}")
            on = t == "note_on" and m.velocity > 0
            self.set_time()
            self.lib.core_c_poly_midi((0x90 if on else 0x80) | self.core_ch(i), m.note, m.velocity if on else 0)
            (self.kheld.add if on else self.kheld.discard)((i, m.note))
            self.keys_on += on
        elif t == "program_change":
            self.keys_select(m.program)
        elif t == "control_change" and m.control in (120, 123):
            self.keys_off(self.sel)
        self.take()

    def ctl(self, line):
        """A line from the UDP control port: sel N | cmd TEXT (a core console command) | stats."""
        w = line.strip().split(None, 1)
        if not w:
            return "?"
        if w[0] == "sel" and len(w) > 1:
            self.keys_select(int(w[1]) - 1)
            return f"entry {self.sel + 1}"
        if w[0] == "cmd" and len(w) > 1:
            b = ctypes.create_string_buffer(600)
            if not self.lib.core_c_poly_command(w[1].encode(), b, 600):
                self.lib.core_c_msamp_command(w[1].encode(), b, 600)
            return b.value.decode()
        if w[0] == "stats":
            return self.poly_stats() + f"; keys down {len(self.kheld)}, note-ons {self.keys_on}, entry {self.sel + 1}"
        return "sel N | cmd TEXT | stats"

    def prime(self):
        """Replay the Rytm's params reports (a file of hex 0x03 messages) so the voices' machines/params are known before
        the Rytm sends its own (it sends one when a track's knob is turned or a trig plays)."""
        for m in getattr(self, "prime_msgs", []):
            self.core.report(m)
        for c in getattr(self, "seq_cmds", []):   # (a link reset forgets poly=bind: the --seq commands again)
            self.log(self.seq(c))
        self.take()
        if getattr(self, "prime_msgs", None):
            self.log(f"primed with {len(self.prime_msgs)} params reports")

    def save_wav(self):
        if not self.wav or not self.chunks:
            return
        a = np.concatenate(self.chunks)
        with wave.open(self.wav, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(a.tobytes())
        print(f"wrote {self.wav}: {len(a) / SR:.1f} s, peak {int(np.abs(a).max())}", flush=True)


def serve(h, link, secs, hello=HELLO_MIDI | HELLO_NO_R | HELLO_VOICES):
    host, port = link.rsplit(":", 1)
    t0 = time.time()
    while True:
        try:
            s = socket.create_connection((host, int(port)), timeout=1)
            break
        except OSError:
            if time.time() - t0 > 120:
                sys.exit(f"no link at {link}")
            time.sleep(0.2)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.05)
    s.sendall(bytes([ord("H"), 1, hello]))
    print(f"poly_live: connected to {link}", flush=True)
    buf, last = b"", time.time()
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            d = s.recv(65536)
            if not d:
                print("poly_live: the emulator closed the link", flush=True)
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
                a = h.render(n)
                s.sendall(b"a" + struct.pack("<I", n) + a.astype("<i2").tobytes())
            elif k == b"H" and len(buf) >= 3:
                buf = buf[3:]
                h.prime()
            elif k == b"M" and len(buf) >= 3 and len(buf) >= 3 + struct.unpack("<H", buf[1:3])[0]:
                n = struct.unpack("<H", buf[1:3])[0]
                data, buf = buf[3:3 + n], buf[3 + n:]
                for c in data:
                    h.midi_byte(c)
            elif k == b"T" and len(buf) >= 4:
                buf = buf[4:]
            elif k == b"P" and len(buf) >= 10:
                buf = buf[10:]
            elif k == b"R" and len(buf) >= 5 and len(buf) >= 5 + 48 * struct.unpack("<I", buf[1:5])[0]:
                n = struct.unpack("<I", buf[1:5])[0]
                h.rytm_audio(buf[5:5 + 48 * n], n)
                buf = buf[5 + 48 * n:]
            elif k in (b"N", b"H", b"M", b"T", b"P", b"R"):
                break
            else:
                sys.exit(f"bad message {buf[:8]!r}")
        while h.entries_on():
            pass
        for m in h.out:
            for i in range(0, len(m), 60000):
                c = m[i:i + 60000]
                s.sendall(b"M" + struct.pack("<H", len(c)) + c)
        h.out = []
        if time.time() - last > 5:
            last = time.time()
            h.log("poly:", h.poly_stats())
            if h.seq_on:
                h.log(h.seq("seq"))
                h.log(h.seq("rytmpat"))
            if h.msamp_on:
                h.log(h.msamp("msamp"))
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--card", default=None, help="the core's card directory (default: an empty temp one)")
    ap.add_argument("--wav", default=None)
    ap.add_argument("--secs", type=float, default=36000.0)
    ap.add_argument("--msamp", default="", help="voices (e.g. 0,1) that play MULTISAMPLE whatever the Rytm reports "
                    "(msamp=voice:V); the card gets content/MULTISAMPLE when it is a temp one")
    ap.add_argument("--keys", default=None, help="a MIDI input port (name substring; waits for it up to --keys-wait s) whose "
                    "notes play the core's live poly path directly")
    ap.add_argument("--keys-map", default="1:0,2:1:chord", help="keyboard channel:voice[:chord],... (the first is selected "
                    "at the start; the keyboard's channel, or its program change N, picks one; chord = a key plays the "
                    "voice's CHD chord on it)")
    ap.add_argument("--keys-wait", type=float, default=600.0)
    ap.add_argument("--prime", default=None, help="a file of hex 0x03 params reports replayed at the start and at every "
                    "link hello (the voices' machines before the Rytm reports them)")
    ap.add_argument("--seq", default="", help="sequencer console commands at the start, comma-separated (seq=track:1:on, "
                    "seq=demo:1, seq=lat:N, seq=quant:1, seq=show)")
    ap.add_argument("--ctl", type=int, default=0, help="a UDP control port on 127.0.0.1: sel N | cmd TEXT | stats")
    ap.add_argument("--rytm-audio", action="store_true", help="the Rytm's own output (R) to the calibration")
    ap.add_argument("--rwav", default=None, help="the Rytm's R frames at the end (implies --rytm-audio): a .npy of n x 12 int32")
    ap.add_argument("--rytmpat", action="store_true", help="the Rytm pattern fetch on (its SysEx dump, rytmpat=on)")
    ap.add_argument("--rytmpat-out", default=None, help="the last fetched record (13101 B) to this file at the end")
    ap.add_argument("--livein", type=int, default=-1, help="voice V plays LIVE IN (livein=voice:V), fed the R frames' AUDIO IN")
    ap.add_argument("--cmd", action="append", default=[], help="a console command at the start (repeatable)")
    ap.add_argument("--at", action="append", default=[], help="SECS:COMMAND (repeatable): a console command at that "
                    "emulated time (frames / 48000)")
    a = ap.parse_args()
    card = a.card or tempfile.mkdtemp(prefix="poly-card-")
    if not a.card:
        os.makedirs(os.path.join(card, "Samples"))
        src = os.path.join(HERE, "..", "content", "MULTISAMPLE")
        if a.msamp and os.path.isdir(src):
            shutil.copytree(src, os.path.join(card, "MULTISAMPLE"))
    h = PolyHost(card, a.wav)
    h.msamp_on = bool(a.msamp)
    h.seq_on = bool(a.seq)
    h.seq_cmds = [c for c in a.seq.split(",") if c]
    for v in filter(None, a.msamp.split(",")):
        print(h.msamp(f"msamp=voice:{int(v)}"), flush=True)
    h.entries = []
    h.kq = None
    if a.keys:
        import mido
        ents = []
        for e in a.keys_map.split(","):
            w = e.split(":")
            ents.append((int(w[0]), int(w[1]), "chord" in w[2:]))
        h.keys_setup(ents)
        t0 = time.time()
        port = None
        while port is None and time.time() - t0 < a.keys_wait:
            port = next((n for n in mido.get_input_names() if a.keys.lower() in n.lower()), None)
            if port is None:
                time.sleep(1)
        if port is None:
            sys.exit(f"poly_live: no MIDI input matching {a.keys!r} in {a.keys_wait:.0f} s")
        h.midi_in = mido.open_input(port, callback=lambda m: h.kq.put(("key", m)))
        print(f"poly_live: keyboard {port!r}: " + "; ".join(
            f"channel {c} -> voice {v}{' chord keys' if k else ''}" for c, v, k in ents), flush=True)
    if a.ctl:
        if h.kq is None:
            h.keys_setup([])
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        us.bind(("127.0.0.1", a.ctl))

        def ctl_loop():
            while True:
                d, ad = us.recvfrom(1000)
                r, ev = [], threading.Event()
                h.kq.put(("ctl", (d.decode(errors="replace"), r, ev)))
                ev.wait(2)
                us.sendto((r[0] if r else "timeout").encode(), ad)
        threading.Thread(target=ctl_loop, daemon=True).start()
    h.prime_msgs = [bytes.fromhex(x) for x in open(a.prime).read().split()] if a.prime else []
    h.prime()
    if a.livein >= 0:
        h.livein = True
        h.lib.core_c_livein_fifo(512)   # the link holds ~512 frames (oblink.rs TARGET): the latency estimate's FIFO
        print(h.ctl(f"cmd livein=voice:{a.livein}"), flush=True)
    for c in a.cmd:
        print(h.ctl(f"cmd {c}"), flush=True)
    h.rwav = a.rwav
    h.timed = sorted((float(x.split(":", 1)[0]), x.split(":", 1)[1]) for x in a.at)
    if a.rytmpat:
        print(h.seq("rytmpat=on"), flush=True)
    print("poly_live: ready", flush=True)
    hello = HELLO_MIDI | HELLO_VOICES | (0 if (a.rytm_audio or a.rwav or a.livein >= 0) else HELLO_NO_R)
    try:
        serve(h, a.link, a.secs, hello)
    except KeyboardInterrupt:
        pass
    finally:
        h.log("poly:", h.poly_stats())
        if h.seq_on:
            h.log(h.seq("seq"))
        if h.msamp_on:
            h.log(h.msamp("msamp"))
        if h.livein:
            h.log(h.ctl("cmd livein"))
        h.save_wav()
        if a.rwav and h.rframes:
            np.save(a.rwav, np.concatenate(h.rframes))
            print(f"wrote {a.rwav}: {sum(len(x) for x in h.rframes)} frames", flush=True)
        if a.rytmpat_out:
            rec, p = h.rytm_record()
            if rec:
                open(a.rytmpat_out, "wb").write(rec)
                print(f"wrote {a.rytmpat_out}: pattern {p}", flush=True)


if __name__ == "__main__":
    main()
