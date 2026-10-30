#!/usr/bin/env python3
"""Stress the real Teensy's voice loop (teensy/voiceloop) without rytm-emu.

    stress.py [--port /dev/cu.usbmodemX] run [SCENARIOS] [--secs S]
    stress.py setup        the STRESS project's pool (8 long samples) and FM list
    stress.py ls [FOLDER]  a TEENSY folder's listing (0D/0E)
    stress.py boot [N]     reboot the Teensy N times ('R'), print its boot log
                           (the card listing's timing)

Plays sim/teensy_bridge.py's role over the same serial framing ('S' + len +
SysEx for the Rytm's reports, 'N' + n for audio, 'A'/'L'/'X' back) and
rytm-emu's Overbridge link's: the Rytm takes 32 frames every 0.667 ms and asks
for more so that what it holds plus what is in flight stays at 512 frames
(rytm-emu src/oblink.rs, TARGET); like the bridge, one request is answered
before the next is sent (at most 1024 frames a request). A Rytm block that
finds fewer than 32 frames buffered is a dropout (rytm-emu plays zeros there).

The project is switched to STRESS for the run (10 STRESS; the pool and FM
list live in /.rytm/pools/STRESS.txt and /.rytm/fm/STRESS.txt) and back to
the project the Teensy had before at the end (from its device-info report,
'D'; --restore NAME when that does not come), also after an error or ^C.

Scenarios (140-150 BPM 16ths; reports as rytm-mods 0021 sends them,
sim/protocol.py; 0x03 floods at the Rytm's rate, every 16 blocks a voice):
  a   8 x 6-OP FM, ROM voices, every voice retriggered on each 16th with
      another PAT (a p-locked PAT: a voice load per trig)
  a2  as a, PAT from the STRESS FM list (card banks)
  b   8 x JUNO chords, held, note-offs; PAT changes by 0x03 (15 back)
  c   8 x DSP SAMPLE streaming pool slots 1..8, retriggered every 16th
  d   mixed: 2 FM, 2 JUNO, 2 DSP SAMPLE, 2 PLAITS (MODAL; DIGIDRUM's voice
      until its removal, 2026-10-29); 16ths at
      150 BPM, knob sweeps on every voice
  e   worst case: 8 x FM with a voice load on every voice on the same block,
      plus knob floods
  e2  worst case mixed: 4 FM loads + 2 JUNO + 2 DSP SAMPLE retriggered on the
      same block, knob floods
  u   machine plugins on user slots (needs rytm.sine_test and rytm.fault_test
      installed: tools/rmp_install.py install ...; 20 s unless --secs): 8 voices
      on U1 (sine_test), P1/P3 sweeping, note-offs; voice 7 moves to U2
      (fault_test) at 10 s: mode 1 (null write) -> "CRASHED - muted"; U2 is
      assigned afresh at 14.92 s, mode 2 at 15 s -> "timed out - muted". U1
      must keep playing.
  np  a Noise Plethora package on U1 (--np a|b|c: befaco.np.<pkg>, installed;
      plugins/noiseplethora/<pkg>.json gives its algorithms): all 8 voices
      trigged once (NP drones; voices without state room stay silent with a
      log line), then P1 (ALG) steps through every algorithm, 5 s each unless
      --secs (the whole run), X and Y sweeping by 0x03 at the Rytm's rate. A
      table per algorithm: "load user" (all voices) and the longest render call
      (cycles) of the 2 s log periods that lie inside the algorithm's step.
"""
import argparse
import glob
import collections
import json
import re
import struct
import sys
import time

import os

import numpy as np
import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from rmp_install import assign_frame   # noqa: E402  (the 'U' frame; one copy of the framing)

HDR = [0xF0, 0x7D, 0x52, 0x54]
RATE, BLOCK, TARGET, CHUNK = 48000, 32, 512, 1024
FM, JUNO, DSPS, PLAITS, DAISY = 0x48, 0x08, 0x20, 0x00, 0x10


def pbytes(ps):
    ps = list(ps) + [0] * (16 - len(ps))
    out = []
    for p in ps[:16]:
        p = max(0.0, min(127.99, float(p)))
        v = int(p)
        out += [v, int((p - v) * 128) & 0x7F]
    return out


def trig(vb, note, vel, ps):
    return bytes(HDR + [0x04, vb, note & 0x7F, vel & 0x7F] + pbytes(ps) + [0xF7])


def params(vb, ps):
    return bytes(HDR + [0x03, vb] + pbytes(ps) + [0xF7])


def note_off(v):
    return bytes(HDR + [0x11, v & 7, 0xF7])


def sysex(cmd, *data):
    return bytes(HDR + [cmd] + list(data) + [0xF7])


def project_msg(name):
    return bytes(HDR + [0x10] + list(name.encode()) + [0xF7])


class Raw(bytes):
    """An event written to the port as it is (not wrapped in an 'S' report)."""


class Link:
    """The serial connection, read without blocking from the one thread that
    paces everything (a reader thread would wait for the GIL; time.sleep
    oversleeps by several ms on macOS): poll() sorts what came back."""

    def __init__(self, port, echo=False):
        self.port = port
        self.echo = echo
        self.open()

    def open(self):
        self.s = serial.Serial(self.port, 115200, timeout=0)
        self.audio = collections.deque()   # (t, n, raw)
        self.logs = []       # (t, text)
        self.xs = []         # (t, SysEx bytes)
        self.junk = 0
        self.buf = bytearray()
        self.alive = True

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def poll(self):
        try:
            n = self.s.in_waiting
            if n:
                self.buf += self.s.read(n)
        except (serial.SerialException, OSError):
            self.alive = False
            return
        b = self.buf
        i = 0
        t = time.perf_counter()
        while i < len(b):
            k = b[i]
            if k in (0x4C, 0x58):          # 'L', 'X'
                if i + 2 > len(b) or i + 2 + b[i + 1] > len(b):
                    break
                body = bytes(b[i + 2:i + 2 + b[i + 1]])
                i += 2 + b[i + 1]
                if k == 0x4C:
                    txt = body.decode(errors="replace")
                    self.logs.append((t, txt))
                    if self.echo:
                        print("teensy:", txt, flush=True)
                else:
                    self.xs.append((t, body))
            elif k == 0x41:                # 'A'
                if i + 3 > len(b):
                    break
                m = b[i + 1] | b[i + 2] << 8
                if i + 3 + m * 16 > len(b):
                    break
                self.audio.append((t, m, bytes(b[i + 3:i + 3 + m * 16])))
                i += 3 + m * 16
            else:
                self.junk += 1
                i += 1
        del b[:i]

    def send(self, msg):
        self.s.write(b"S" + bytes([len(msg)]) + msg)

    def request(self, n):
        self.s.write(b"N" + struct.pack("<H", n))

    def wait_audio(self, timeout=2.0):
        t = time.perf_counter()
        while not self.audio:
            self.poll()
            if not self.alive or time.perf_counter() - t > timeout:
                raise RuntimeError("Teensy did not answer")
        return self.audio.popleft()

    def frames(self, n, timeout=2.0):
        self.request(n)
        return self.wait_audio(timeout)[2]

    def idle(self, secs):
        t = time.perf_counter()
        while time.perf_counter() - t < secs:
            self.poll()
            time.sleep(0.002)


def find_port():
    found = sorted(glob.glob("/dev/cu.usbmodem*"))
    return found[0] if found else None


# ---------------------------------------------------------------------------
# The run: real-time pacing as rytm-emu's link and the bridge.

def runs(ts, join=0.05):
    """Dropout times -> [(from, to, blocks)] runs (joined when < 50 ms apart)."""
    out = []
    for t in ts:
        if out and t - out[-1][1] < join:
            out[-1][1] = t
            out[-1][2] += 1
        else:
            out.append([t, t, 1])
    return [(round(a, 3), round(b, 3), n) for a, b, n in out]


def run_events(link, events, secs, label):
    """events: [(t, msg)], t in seconds from the start. Returns the stats."""
    events = sorted(events, key=lambda e: e[0])
    # prefill as the emulator's first request does
    link.request(TARGET)
    link.wait_audio(3)
    t0 = time.perf_counter()
    log0 = len(link.logs)
    requested = popped = TARGET
    q = TARGET
    popped = 0
    nb = 0                       # next Rytm block boundary
    miss_blocks = miss_frames = 0
    min_q = TARGET
    rtts = []
    pend = None
    ei = 0
    sent = 0

    def boundaries(t):
        nonlocal nb, q, popped, miss_blocks, miss_frames, min_q
        while nb * BLOCK / RATE <= t:
            min_q = min(min_q, q)
            take = min(BLOCK, q)
            q -= take
            popped += take
            if take < BLOCK:
                miss_t.append(nb * BLOCK / RATE)
                if len(miss_at) < 8:
                    miss_at.append(round(nb * BLOCK / RATE, 3))
                miss_blocks += 1
                miss_frames += BLOCK - take
            nb += 1

    max_gap, last = 0.0, 0.0
    miss_at = []
    miss_t = []                  # every dropout's time
    gaps = []                    # (from, to): the host loop stalled > 2 ms
    sq, nsq = np.zeros(8), 0
    while True:
        now = time.perf_counter() - t0
        if now >= secs:
            break
        if now - last > 0.002:
            gaps.append((last, now))
        max_gap = max(max_gap, now - last)
        last = now
        link.poll()
        if pend is not None:
            if not link.audio:
                if not link.alive:
                    raise RuntimeError("serial link lost")
                if time.perf_counter() - pend > 2:
                    raise RuntimeError("Teensy did not answer")
                continue
            ta, m, raw = link.audio.popleft()
            a = np.frombuffer(raw, "<i2").reshape(m, 8).astype(np.float64)
            sq += (a * a).sum(axis=0)
            nsq += m
            boundaries(ta - t0)
            q += m
            rtts.append(ta - pend)
            pend = None
            continue
        boundaries(now)
        while ei < len(events) and events[ei][0] <= now:
            if isinstance(events[ei][1], Raw):
                link.s.write(bytes(events[ei][1]))
            else:
                link.send(events[ei][1])
            ei += 1
            sent += 1
        have = requested - popped
        if have < TARGET:
            n = min(CHUNK, TARGET - have)
            link.request(n)
            pend = time.perf_counter()
            requested += n
    t_end = time.perf_counter()
    # drain the last answer
    if pend is not None:
        try:
            link.wait_audio(2)
        except RuntimeError:
            pass
    link.idle(0.3)
    cpu, load, user, user_lines = [], [], [], []
    for t, txt in link.logs[log0:]:
        m = re.match(r"load user ([\d.]+) %", txt)
        if m and t - t0 > 2.2 and t <= t_end + 0.1:
            user.append(float(m.group(1)))
        m = re.match(r"load user ([\d.]+) % \(machine plugins\), longest render (\d+) cycles \(limit (\d+)\)", txt)
        if m:
            user_lines.append((t - t0, float(m.group(1)), int(m.group(2)), int(m.group(3))))
        m = re.match(r"load fm ([\d.]+) juno ([\d.]+) dsps ([\d.]+) daisy ([\d.]+)(?: dd ([\d.]+))? plaits ([\d.]+) %; "
                     r"max request (\d+) us, max report (\d+) us, refills (\d+) \((\d+) KB, max (\d+) us\)"
                     r"(?:, max loop (\d+) us)?(?:, max trig wait (\d+) us)?", txt)
        if m and t - t0 > 2.2 and t <= t_end + 0.1:
            load.append([float(x) if x is not None else 0.0 for x in m.groups()])
        m = re.match(r"cpu ([\d.]+) %.*peak block (\d+) %, with card reads (\d+) %, stream underruns (\d+)", txt)
        if m and t - t0 > 2.2 and t <= t_end + 0.1:
            cpu.append(tuple(float(x) for x in m.groups()))
    under0 = None
    for t, txt in link.logs[:log0]:
        m = re.search(r"stream underruns (\d+)", txt)
        if m:
            under0 = int(m.group(1))
    other = [f"[{t - t0:6.3f} s] {txt}" for t, txt in link.logs[log0:]
             if not txt.startswith(("cpu ", "trig voice", "load user ", "load fm "))]
    rtts.sort()
    # a dropout within a buffer's length (512 frames) after a host stall is the
    # host's (the Mac did not ask in time), not the Teensy's
    buf_s = TARGET / RATE + 0.002
    host_miss = sum(1 for t in miss_t if any(a - 0.001 <= t <= b + buf_s for a, b in gaps))
    st = {
        "label": label,
        "reports": sent,
        "cpu_avg": max((c[0] for c in cpu), default=float("nan")),
        "cpu_mean": sum(c[0] for c in cpu) / len(cpu) if cpu else float("nan"),
        "peak": max((c[1] for c in cpu), default=float("nan")),
        "card": max((c[2] for c in cpu), default=float("nan")),
        "underruns": (cpu[-1][3] - (under0 if under0 is not None else cpu[0][3])) if cpu else float("nan"),
        "miss_blocks": miss_blocks,
        "miss_host": host_miss,
        "miss_frames": miss_frames,
        "min_q_ms": min_q / RATE * 1000,
        "rtt_p99": rtts[int(len(rtts) * 0.99)] * 1000 if rtts else 0,
        "rtt_max": rtts[-1] * 1000 if rtts else 0,
        "blocks": nb,
        "host_gap": max_gap * 1000,
        "mach": [max((l[k] for l in load), default=0.0) for k in range(6)],
        "max_req_us": max((l[6] for l in load), default=0),
        "max_rep_us": max((l[7] for l in load), default=0),
        "refill_kbs": max((l[9] / 2 for l in load), default=0),
        "max_refill_us": max((l[10] for l in load), default=0),
        "max_loop_us": max((l[11] for l in load), default=0),
        "max_trig_wait_us": max((l[12] for l in load), default=0),
        "user": max(user, default=0.0),
        "user_n": len(user),
        "user_lines": user_lines,
        "other": other,
        "miss_at": miss_at,
        "miss_runs": runs(miss_t),
        "gap_runs": [(round(a, 3), round(b, 3)) for a, b in gaps if b - a > 0.02],
        "rms_db": [20 * np.log10(max(1e-9, np.sqrt(x / max(1, nsq)) / 32768)) for x in sq],
    }
    return st


# ---------------------------------------------------------------------------
# Scenarios

def step_len(bpm):
    return 60.0 / bpm / 4


def fm_ps(pat, tim=64.0):
    # PAT TIM MOR DEC LPG TUN LEV LEN
    return [pat, tim, 64, 90, 64, 64, 100, 60]


def juno_ps(pat, cut=80.0):
    # PAT CUT RES ENV ATK DEC REL LEN
    return [pat, cut, 30, 60, 5, 60, 40, 127]


def dsps_ps(slot, bri=64.0, eng=64, mix=64):
    # ENG STR BRI DMP MIX FB TUN LEV | 17 TUN 18 FIN 19 SMP 20 FLD 21 STA 22 END 23 LOP 24 LEV
    return [eng, 64, bri, 64, mix, 30, 64, 100, 64, 64, slot, 0, 0, 120, 0, 100]


PLAITS_ZONES = ["VA VCF", "PHASE DIST", "WAVE TERRAIN", "STRING MACH", "CHIPTUNE", "VA", "WAVESHAPE", "2-OP FM",
                "GRANULAR", "HARMONIC", "WAVETABLE", "CHORDS", "SPEECH", "SWARM", "PARTICLE", "STRING", "MODAL"]


def plaits_eng(name):
    """The ENG value of a PLAITS model (its zone's first value, N = 17)."""
    z = PLAITS_ZONES.index(name)
    return (z * 128 + 16) // 17


def plaits_ps(tim=64.0, eng=124):
    # ENG (124: MODAL) HRM TIM MOR DEC LPG TUN LEV | FM FIN AUX TMD MMD VEL GLD VOL
    return [eng, 64, tim, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100]


def knob_flood(voices, secs, start=0.0):
    """0x03 every 16 Rytm blocks a voice (staggered), the given parameter
    sweeping: voices [(vb, fn(x) -> ps)]."""
    ev = []
    period = 16 * BLOCK / RATE
    for k, (vb, fn) in enumerate(voices):
        t = start + k * period / len(voices)
        i = 0
        while t < secs:
            x = 20 + 100 * abs(((i * 0.02) % 2) - 1)
            ev.append((t, params(vb, fn(x))))
            t += period
            i += 1
    return ev


def sc_fm(secs, list_voices):
    ev = []
    st = step_len(140)
    k, t = 0, 0.2
    while t < secs - 0.3:
        for v in range(8):
            pat = (k * 8 + v * 5) % (20 if list_voices else 96)
            ev.append((t, trig(FM | v, 48 + v * 3, 100, fm_ps(pat))))
        k += 1
        t += st
    return ev


def sc_juno(secs):
    ev = []
    chord = [48, 52, 55, 59, 62, 64, 67, 71]
    bar = step_len(140) * 8      # a chord every half bar
    k, t = 0, 0.2
    while t < secs - 0.5:
        for v in range(8):
            ev.append((t, trig(JUNO | v, chord[v] + (k % 3) * 2, 100, juno_ps(k % 16))))
        for v in range(8):
            ev.append((t + bar * 0.7, note_off(v)))
        # a PAT move on the sound (0x03): the Teensy answers 15 per voice
        for v in range(8):
            ev.append((t + bar * 0.8, params(JUNO | v, juno_ps((k + 1) % 16))))
        k += 1
        t += bar
    ev += knob_flood([(JUNO | v, lambda x, v=v: juno_ps(0, x)) for v in range(8)], secs, 0.3)
    return ev


def sc_dsps(secs, **kw):
    ev = []
    st = step_len(140)
    for v in range(8):   # select the slots first (prepare: the head reads)
        ev.append((0.05 + v * 0.01, params(DSPS | v, dsps_ps(v + 1, **kw))))
    k, t = 0, 0.3
    while t < secs - 0.3:
        for v in range(8):
            ev.append((t, trig(DSPS | v, 60, 100, dsps_ps(v + 1, **kw))))
        k += 1
        t += st * 4 if k % 2 else st * 2
    return ev


def sc_dsps_plock(secs):
    """8 DSP SAMPLE voices, each trig p-locking another pool slot (a sample per
    step): every trig opens a file and reads its head."""
    ev = []
    st = step_len(140)
    k, t = 0, 0.3
    while t < secs - 0.3:
        for v in range(8):
            ev.append((t, trig(DSPS | v, 60, 100, dsps_ps(1 + (k + v) % 8))))
        k += 1
        t += st
    return ev


def sc_mixed(secs):
    ev = []
    st = step_len(150)
    kinds = [FM, FM, JUNO, JUNO, DSPS, DSPS, PLAITS, PLAITS]
    ps = {FM: fm_ps, JUNO: juno_ps, DSPS: None, PLAITS: plaits_ps}
    for v in range(8):
        if kinds[v] == DSPS:
            ev.append((0.05, params(DSPS | v, dsps_ps(v - 3))))
    k, t = 0, 0.2
    while t < secs - 0.3:
        for v in range(8):
            m = kinds[v]
            if m == FM:
                p = fm_ps((k * 3 + v) % 96)
            elif m == JUNO:
                p = juno_ps(k % 8)
                if k % 2:
                    ev.append((t + st * 0.5, note_off(v)))
            elif m == DSPS:
                p = dsps_ps(v - 3)
            else:
                p = ps[m]()
            ev.append((t + 0.0003 * v, trig(m | v, 48 + (k % 12), 100, p)))
        k += 1
        t += st
    fl = []
    for v in range(8):
        m = kinds[v]
        if m == FM:
            fl.append((m | v, lambda x: fm_ps(0, x)))
        elif m == JUNO:
            fl.append((m | v, lambda x: juno_ps(0, x)))
        elif m == DSPS:
            fl.append((m | v, lambda x, v=v: dsps_ps(v - 3, x)))
        else:
            fl.append((m | v, plaits_ps))
    ev += knob_flood(fl, secs, 0.25)
    return ev


def sc_worst(secs, mixed):
    ev = []
    st = step_len(150)
    kinds = [FM, FM, FM, FM, JUNO, JUNO, DSPS, DSPS] if mixed else [FM] * 8
    for v in range(8):
        if kinds[v] == DSPS:
            ev.append((0.05, params(DSPS | v, dsps_ps(v - 5))))
    k, t = 0, 0.2
    while t < secs - 0.3:
        for v in range(8):   # one burst: the Teensy takes them before its next request
            m = kinds[v]
            if m == FM:
                p = fm_ps((k * 11 + v * 7) % 96)
            elif m == JUNO:
                p = juno_ps((k + v) % 16)
            else:
                p = dsps_ps(v - 5)
            ev.append((t, trig(m | v, 40 + (k * 5 + v * 7) % 40, 110, p)))
        k += 1
        t += st
    fl = []
    for v in range(8):
        m = kinds[v]
        if m == FM:
            fl.append((m | v, lambda x: fm_ps(0, x)))
        elif m == JUNO:
            fl.append((m | v, lambda x: juno_ps(0, x)))
        else:
            fl.append((m | v, lambda x, v=v: dsps_ps(v - 5, x)))
    ev += knob_flood(fl, secs, 0.25)
    return ev


def sc_same(secs, m, fn):
    """8 voices of one machine, all retriggered on each 16th at 140 BPM."""
    ev = []
    st = step_len(140)
    k, t = 0, 0.2
    while t < secs - 0.3:
        for v in range(8):
            ev.append((t, trig(m | v, 48 + (k * 5 + v * 7) % 24, 100, fn())))
        k += 1
        t += st
    return ev


def daisy_ps(eng=0):
    # ENG STR BRI DMP INP EXC TUN LEV
    return [eng, 64, 64, 64, 0, 40, 64, 100]


def sc_browse(secs):
    """d, while the Rytm browses TEENSY (0D on the big folders, every 0.4 s)
    and switches projects (10) twice."""
    ev = sc_mixed(secs)
    t, k = 0.5, 0
    for_folders = [0, 4, 6, 75, 76, 1, 2, 3]
    while t < secs - 0.5:
        f = for_folders[k % len(for_folders)]
        ev.append((t, sysex(0x0D, (f >> 7) & 0x7F, f & 0x7F)))
        k += 1
        t += 0.4
    ev.append((secs * 0.4, project_msg("STRESS2")))
    ev.append((secs * 0.7, project_msg("STRESS")))
    return ev


def sc_user(secs):
    """u: see the module doc. U1 = 0x50 + v, U2 = 0x58 + v; P1 = TUNE, P2 =
    LEVEL, P3 = DECAY on sine_test; P1 * 4 / 128 picks fault_test's mode."""
    ev = []
    st = step_len(150)
    k, t = 0, 0.2
    while t < secs - 0.3:
        for v in range(8):
            fault = v == 7 and t >= 10
            p = [64] * 16
            p[1], p[2] = 100, 30
            if fault:
                p[0] = 33 if t < 14.95 else 65       # mode 1 (null write), then mode 2 (endless loop)
            ev.append((t + 0.0003 * v, trig((0x58 if fault else 0x50) | v, 48 + (v * 7) % 24, 100, p)))
            ev.append((t + 0.06, note_off(v)))
        k += 1
        t += st
    # P1 (TUNE) and P3 (DECAY) sweeping on the sine voices, 0x03 at the Rytm's rate
    ev += knob_flood([(0x50 | v, lambda x: [x, 100, 140 - x]) for v in range(7)], secs, 0.25)
    # the fault machine is gone after its crash: a fresh load before the endless loop
    ev.append((14.92, Raw(assign_frame(2, "rytm.fault_test", "1.0.0"))))
    ev.append((5.0, Raw(b"D")))   # the device-info report while the voices play: built in loop passes, no dropout
    return ev


def setup_user(link):
    """Assign U1 = rytm.sine_test, U2 = rytm.fault_test (current project) and
    wait for "ready"; a clear failure otherwise."""
    for slot, mid in ((1, "rytm.sine_test"), (2, "rytm.fault_test")):
        assign_wait(link, slot, mid, "u")


def assign_wait(link, slot, mid, scen):
    if True:
        n0 = len(link.logs)
        link.s.write(assign_frame(slot, mid, "1.0.0"))
        ok = bad = None
        t = time.perf_counter()
        while time.perf_counter() - t < 10 and not (ok or bad):
            link.idle(0.02)
            for _, txt in link.logs[n0:]:
                if re.match(rf"plugins: U{slot} {re.escape(mid)} ready", txt):
                    ok = txt
                elif re.match(r"plugins: (assign U\d refused|cannot |no card|safe mode)", txt) or \
                        re.match(rf"plugins: U{slot} {re.escape(mid)} (state|init|declares|needs)", txt):
                    bad = txt
        if not ok:
            sys.exit(f"scenario {scen}: assign U{slot} {mid} failed: {bad or 'no answer within 10 s'} "
                     "(installed? tools/rmp_install.py install plugins/<name>/<name>.rmp)")
        print(f"   teensy: {ok}", flush=True)


NP_PKG = "a"          # run --np
NP_STEP = 5.0         # seconds an algorithm plays (np)


def np_algs():
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "plugins", "noiseplethora",
                           f"{NP_PKG}.json")) as f:
        return json.load(f)["knobs"][0]["names"]


def np_zone(alg, n):
    return float(int((alg + 0.5) * 128 / n))


def sc_np(secs):
    """np: see the module doc. U1 = 0x50 + v; P1 ALG, P2 X, P3 Y, P8 LEV."""
    n = len(np_algs())
    step = secs / n
    ev = []
    for v in range(8):
        p = [np_zone(0, n), 64, 64, 64, 64, 64, 64, 100]
        ev.append((0.1 + 0.0003 * v, trig(0x50 | v, 48, 100, p)))

    def fn(v):
        return lambda x, v=v: [0, x, 140 - x, 64, 64, 64, 64, 100]   # P1 set below
    flood = knob_flood([(0x50 | v, fn(v)) for v in range(8)], secs, 0.2)
    for t, msg in flood:   # P1 follows the step the report falls in
        alg = min(n - 1, int(t / step))
        ev.append((t, msg[:6] + bytes(pbytes([np_zone(alg, n)])[:2]) + msg[8:]))
    return ev


def setup_np(link):
    assign_wait(link, 1, f"befaco.np.{NP_PKG}", "np")


def np_table(st, secs):
    names = np_algs()
    n = len(names)
    step = secs / n
    print(f"\n| ALG | name | load user (all voices) | longest call (cycles) | of the guard limit |")
    print("|---|---|---|---|---|")
    for a in range(n):
        lo, hi = a * step, (a + 1) * step
        inside = [u for u in st["user_lines"] if u[0] - 2.0 >= lo + 0.05 and u[0] <= hi + 0.02]
        if not inside:
            print(f"| {a} | {names[a]} | (no full 2 s log period inside the step) | | |")
            continue
        load = max(u[1] for u in inside)
        peak = max(u[2] for u in inside)
        lim = inside[0][3]
        print(f"| {a} | {names[a]} | {load:.1f} % | {peak} | {100 * peak / lim:.0f} % of {lim} |")


SETUP = {"u": setup_user, "np": setup_np}
DEFAULT_SECS = {"u": 20.0}


SCENARIOS = {
    "idle": ("no reports (the host's own jitter)", lambda s: []),
    "a": ("8 x 6-OP FM, ROM, PAT per trig", lambda s: sc_fm(s, False)),
    "a2": ("8 x 6-OP FM, card list, PAT per trig", lambda s: sc_fm(s, True)),
    "b": ("8 x JUNO chords, note-offs, PAT", sc_juno),
    "c": ("8 x DSP SAMPLE streams (STRING, MIX 64)", sc_dsps),
    "c0": ("8 x DSP SAMPLE streams, MIX 0 (no resonator)", lambda s: sc_dsps(s, mix=0)),
    "c2": ("8 x DSP SAMPLE, another pool slot every trig", sc_dsps_plock),
    "cm": ("8 x DSP SAMPLE streams, MODAL, MIX 64", lambda s: sc_dsps(s, eng=0)),
    "f": ("8 x DAISYSP MODAL, 16ths", lambda s: sc_same(s, DAISY, daisy_ps)),
    "g": ("8 x PLAITS MODAL, 16ths", lambda s: sc_same(s, PLAITS, plaits_ps)),
    "pw": ("8 x PLAITS WAVETABLE, 16ths", lambda s: sc_same(s, PLAITS, lambda: plaits_ps(eng=plaits_eng("WAVETABLE")))),
    "pc": ("8 x PLAITS CHORDS, 16ths", lambda s: sc_same(s, PLAITS, lambda: plaits_ps(eng=plaits_eng("CHORDS")))),
    "pt": ("8 x PLAITS WAVE TERRAIN, 16ths", lambda s: sc_same(s, PLAITS, lambda: plaits_ps(eng=plaits_eng("WAVE TERRAIN")))),
    "ps": ("8 x PLAITS SPEECH, 16ths", lambda s: sc_same(s, PLAITS, lambda: plaits_ps(eng=plaits_eng("SPEECH")))),
    "d": ("mixed 2 FM 2 JUNO 2 DSPS 2 PL, 150 BPM, knobs", sc_mixed),
    "dl": ("d while browsing TEENSY (0D) and switching projects", sc_browse),
    "e": ("worst: 8 FM loads on one block, knobs", lambda s: sc_worst(s, False)),
    "e2": ("worst mixed: 4 FM loads 2 JUNO 2 DSPS, knobs", lambda s: sc_worst(s, True)),
    "u": ("8 x user-slot sine_test, sweeps, note-offs; fault_test on U2 from 10 s", sc_user),
    "np": ("Noise Plethora package on U1 (--np), ALG stepping, X/Y sweeping", sc_np),
}


def silence(link):
    for v in range(8):
        link.send(note_off(v))
        # a PLAITS voice at level 0 stops nothing; the next scenario sets its machines


def info_project(link, timeout=10.0):
    """The project the Teensy holds, from its device-info report ('D': "info: i/n chunk" log lines of a JSON
    document, as tools/rmp_install.py's Link.info); None when it does not come."""
    n0 = len(link.logs)
    link.s.write(b"D")
    chunks, total = {}, None
    t = time.perf_counter()
    while time.perf_counter() - t < timeout and (total is None or len(chunks) < total):
        link.idle(0.05)
        for _, txt in link.logs[n0:]:
            m = re.match(r"^info: (\d+)/(\d+) (.*)$", txt)
            if m:
                chunks[int(m.group(1))] = m.group(3)
                total = int(m.group(2))
    try:
        return json.loads("".join(chunks[i] for i in range(1, total + 1))).get("project") or None
    except (TypeError, KeyError, ValueError, AttributeError):
        return None


def project_to_restore(link, a):
    """The project to switch back to at the end: the Teensy's own (its info report), else --restore."""
    name = info_project(link) or getattr(a, "restore", None)
    print(f"project now: {name or '(unknown: not restored at the end; --restore NAME)'}", flush=True)
    return name


def restore_project(link, name):
    if name and link.alive:
        link.send(project_msg(name))
        link.idle(0.5)
        print(f"project back to {name}")


def table(rows):
    hdr = ("scen", "what", "cpu avg", "peak blk", "w/ card", "underr", "dropouts (teensy, +host)", "min buf",
           "rtt max",
           "host gap", "max req", "max rep", "max loop", "trig wait", "card KB/s", "user cpu")
    print("\n| " + " | ".join(hdr) + " |")
    print("|" + "---|" * len(hdr))
    for r in rows:
        print(f"| {r['label']} | {r['what']} | {r['cpu_mean']:.1f} % | {r['peak']:.0f} % | "
              f"{r['card']:.0f} % | {r['underruns']:.0f} | {r['miss_blocks'] - r['miss_host']} (+{r['miss_host']} host) | "
              f"{r['min_q_ms']:.1f} ms | {r['rtt_max']:.1f} ms | {r['host_gap']:.1f} ms | "
              f"{r['max_req_us'] / 1000:.1f} ms | {r['max_rep_us'] / 1000:.1f} ms | {r['max_loop_us'] / 1000:.1f} ms | "
              f"{r['max_trig_wait_us'] / 1000:.1f} ms | "
              f"{r['refill_kbs']:.0f} | {r['user']:.1f} % |")
    print("\nper machine (render %, worst 2 s window): fm juno dsps daisy dd plaits (dd: older builds)")
    for r in rows:
        print(f"  {r['label']:3s} " + " ".join(f"{x:5.1f}" for x in r["mach"]))


def cmd_run(link, a):
    restore = project_to_restore(link, a)
    try:
        run_scenarios(link, a)
    finally:   # an error or ^C too: the user's project back
        restore_project(link, restore)


def run_scenarios(link, a):
    names = a.scenarios or list(SCENARIOS)
    link.send(project_msg("STRESS"))
    link.idle(0.5)
    rows = []
    for name in names:
        what, fn = SCENARIOS[name]
        secs = a.secs if a.secs is not None else (NP_STEP * len(np_algs()) if name == "np"
                                                  else DEFAULT_SECS.get(name, 12.0))
        if name in SETUP:
            SETUP[name](link)
        ev = fn(secs)
        print(f"scenario {name}: {what} ({len(ev)} reports, {secs:.0f} s)", flush=True)
        st = run_events(link, ev, secs, name)
        st["what"] = what
        rows.append(st)
        silence(link)
        print("   level per voice (dBFS):", " ".join(f"{x:.0f}" for x in st["rms_db"]))
        if st["miss_at"]:
            print("   dropouts at", st["miss_at"], "s")
        if st["miss_runs"]:
            print("   dropout runs (from s, to s, blocks):", st["miss_runs"][:12])
        if st["gap_runs"]:
            print("   host stalls > 20 ms (from s, to s):", st["gap_runs"][:12])
        for o in st["other"][:40]:
            print("   teensy:", o)
        if name == "np":
            np_table(st, secs)
        link.idle(0.3)
    table(rows)
    if link.junk:
        print(f"(junk bytes from the Teensy: {link.junk})")


# ---------------------------------------------------------------------------
# Card helpers: TEENSY listings, the STRESS pool and FM list.

def listing(link, folder):
    """0D folder -> (parent, [(kind, id, size, name)])."""
    n0 = len(link.xs)
    link.send(sysex(0x0D, (folder >> 7) & 0x7F, folder & 0x7F))
    ents, total, parent = {}, None, 0
    t = time.time()
    while time.time() - t < 3:
        link.frames(32)    # keep it busy as the bridge would; X arrive in between
        for _, m in link.xs[n0:]:
            if len(m) < 14 or m[4] != 0x0E or (m[5] << 7 | m[6]) != folder:
                continue
            parent = m[7] << 7 | m[8]
            first = m[9] << 7 | m[10]
            total = m[11] << 7 | m[12]
            i, k = 13, first
            while i < len(m) - 1:
                kind = m[i]
                eid = m[i + 1] << 7 | m[i + 2]
                size = m[i + 3] << 21 | m[i + 4] << 14 | m[i + 5] << 7 | m[i + 6]
                ln = m[i + 7]
                name = m[i + 8:i + 8 + ln].decode(errors="replace")
                ents[k] = (kind, eid, size, name)
                k += 1
                i += 8 + ln
        if total is not None and len(ents) >= total:
            break
    return parent, [ents[k] for k in sorted(ents)]


def cmd_ls(link, a):
    parent, ents = listing(link, a.folder)
    print(f"folder {a.folder} (parent {parent}): {len(ents)} entries")
    for kind, eid, size, name in ents:
        print(f"  {'DIR ' if kind else '    '} {eid:5d} {size:10d}  {name}")


def cmd_setup(link, a):
    """STRESS: pool slots 1..8 = the 8 largest .wav files found in a few
    folders below /Samples; FM list = 20 voices of the first /DX7 bank(s).
    The Teensy's project is switched back afterwards."""
    restore = project_to_restore(link, a)
    try:
        setup_stress(link)
    finally:
        restore_project(link, restore)


def setup_stress(link):
    link.send(project_msg("STRESS"))
    link.idle(0.5)
    # walk /Samples breadth first (TEENSY folder ids), skipping JUNO/DX7
    _, root = listing(link, 0)
    special = {}
    files = []
    todo = []
    for kind, eid, size, name in root:
        if kind and name in ("JUNO", "DX7"):
            special[name] = eid
        elif kind:
            todo.append(eid)
        else:
            files.append((size, 0, eid, name))
    seen = 0
    while todo and seen < 60:
        f = todo.pop(0)
        seen += 1
        _, ents = listing(link, f)
        for kind, eid, size, name in ents:
            if kind:
                todo.append(eid)
            else:
                files.append((size, f, eid, name))
    files.sort(reverse=True)
    pick = [x for x in files if x[0] < 64 * 1024 * 1024][:8]
    print("pool STRESS:")
    for slot, (size, f, eid, name) in enumerate(pick, 1):
        link.send(sysex(0x12, slot, (f >> 7) & 0x7F, f & 0x7F, (eid >> 7) & 0x7F, eid & 0x7F))
        link.frames(32)
        print(f"  slot {slot}: folder {f} file {eid} {name} ({size / 1e6:.1f} MB)")
        link.idle(0.2)
    if "DX7" in special:
        banks = []
        todo = [special["DX7"]]
        while todo and len(banks) < 2:
            f = todo.pop(0)
            _, ents = listing(link, f)
            if ents and not ents[-1][0]:      # a bank: its voices
                banks.append(f)
            todo += [e[1] for e in ents if e[0]]
        n = 0
        for b in banks:
            for i in range(32):
                if n >= 20:
                    break
                link.send(sysex(0x0F, (b >> 7) & 0x7F, b & 0x7F, 0, i))
                link.frames(32)
                link.idle(0.05)
                n += 1
        print(f"FM list STRESS: {n} voices from DX7 banks {banks}")
    else:
        print("no DX7 folder on the card: a2 plays empty slots")
    link.idle(0.5)
    for t, txt in link.logs:
        if txt.startswith(("pool", "fm list")):
            print("  teensy:", txt)


def cmd_boot(link, a):
    for i in range(a.n):
        link.s.write(b"R")
        link.close()
        t = time.time()
        time.sleep(0.3)
        port = None
        while time.time() - t < 10:
            port = find_port()
            if port:
                try:
                    link.port = port
                    link.open()
                    break
                except serial.SerialException:
                    pass
            time.sleep(0.05)
        tb = time.time()
        while time.time() - tb < 6:
            link.idle(0.1)
            if any("fm list" in x for _, x in link.logs) and time.time() - tb > 1:
                break
        print(f"--- boot {i + 1}")
        for _, txt in link.logs:
            print("  ", txt)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None)
    ap.add_argument("--echo", action="store_true", help="print every Teensy log line")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("scenarios", nargs="*", choices=list(SCENARIOS) + [[]], default=[])
    r.add_argument("--secs", type=float, default=None, help="seconds a scenario runs (12; u: 20)")
    r.add_argument("--restore", default=None, help="the project to switch back to at the end when the Teensy's info report does not name it")
    r.add_argument("--np", default="a", choices=["a", "b", "c"], help="the NP package scenario np plays (befaco.np.<pkg>)")
    s = sub.add_parser("ls")
    s.add_argument("folder", type=int, nargs="?", default=0)
    sub.add_parser("setup")
    b = sub.add_parser("boot")
    b.add_argument("n", type=int, nargs="?", default=1)
    a = ap.parse_args()
    port = a.port or find_port()
    if not port:
        sys.exit("no Teensy serial port")
    global NP_PKG
    NP_PKG = getattr(a, "np", None) or NP_PKG
    link = Link(port, a.echo)
    link.idle(0.2)
    {"run": cmd_run, "ls": cmd_ls, "setup": cmd_setup, "boot": cmd_boot}[a.cmd](link, a)
    link.close()


if __name__ == "__main__":
    main()
