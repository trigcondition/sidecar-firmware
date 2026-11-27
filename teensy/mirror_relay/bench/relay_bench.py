#!/usr/bin/env python3
"""Bench check of the Teensy mirror relay (teensy/mirror_relay, pi/engine/relay_proto.h) with the Teensy alone on a PC.

The PC plays both ends: the Pi on the link (USB1, 1209:0002) and the computer on the mirrored device (USB2, the 5-pin
header with the check-1 cable: D+ D- GND, red wire cut). No Pi, no box, no unit claim. The mirrored device is made up
here (pid.codes 1209:0003, a vendor interface with the A4's endpoint shape: audio 0x83/0x03 at 256 B on alt 3, a pipe
pair 0x82/0x02 and a second pair 0x81/0x01, all bulk 512), plus a BOS / MS OS 2.0 set so Windows binds WinUSB.

    pip install libusb1
    python relay_bench.py --info              # link descriptors, stats, log
    python relay_bench.py --seconds 15 --json run.json   # staged: ctl, each stream alone, then all at once
    python relay_bench.py --paths audio_pc --seconds 60
    python relay_bench.py --bootloader        # reboot the Teensy into its bootloader for the next flash
    python relay_bench.py --computer-only --paths engine --late 2 --seconds 30   # the Overbridge Engine's pattern (TR2)

What runs at once:
  audio to the computer   "Pi" sends seq-numbered 256 B blocks on link 0x02, paced at 48000/7 a second; the
                          "computer" reads 0x83 and checks the seq (lost, duplicated, reordered)
  audio to the Pi         the "computer" sends blocks on 0x03; the "Pi" reads link 0x82 and checks them
  pipe to the box         the "computer" writes 0x02; the "Pi" reads link channel 0 IN, checks, grants a credit back
  pipe to the computer    the "Pi" writes link channel 0 OUT; the "computer" reads 0x82; kEvInTaken events counted
  control                 the "computer" sends vendor IN and OUT requests; the "Pi" answers from the events;
                          the round trip is timed and the answer checked
  engine (--computer-only) as the Overbridge Engine did in TR2 (T327): alt 3 taken, then --late s before the first
                          0x83 read; every IN block read is answered by one OUT block on 0x03 (no OUT clock of its own)
The Teensy's own counters (residency histograms, drops) are read at the end.
"""
import argparse
import json
import queue
import struct
import sys
import threading
import time

import usb1

LINK = (0x1209, 0x0002)
MIRR = (0x1209, 0x0003)
EV_EP, A_TO_PI, A_FROM_PI = 0x81, 0x82, 0x02
CH_IN = lambda c: 0x80 | (3 + c)
CH_OUT = lambda c: 3 + c
REQ = dict(SetDevice=1, SetConfig=2, SetString=3, SetExtra=4, Connect=5, CtlAnswer=6, CtlData=7, Credit=8,
           Stats=9, StatsReset=10, Log=11, Halt=12, Bootloader=13)
EV = {1: "hello", 2: "bus", 3: "config", 4: "alt", 5: "setup", 6: "abort", 7: "intaken", 8: "drop"}
BLOCK = 256                     # 8 channels: 32 + 7 * 8 * 4
BLOCK_HZ = 48000 / 7
PIPE_HZ = 300

STATS_FIELDS = ("magic version uptime_ms link_hs link_configured link_resets pc_hs pc_configured pc_resets pc_suspends "
                "shown setups ctl_forwarded ctl_answered ctl_stalled ctl_aborted ctl_late audio_to_pc audio_to_pc_drop "
                "audio_to_pc_empty audio_to_pi audio_to_pi_drop").split()
STATS_FIELDS += ["chan_to_pc%d" % i for i in range(4)] + ["chan_to_pi%d" % i for i in range(4)]
STATS_FIELDS += ["chan_nocredit%d" % i for i in range(4)] + "events events_drop q_to_pc_max q_to_pi_max".split()
STATS_FIELDS += ["hist_to_pc%d" % i for i in range(16)] + ["hist_to_pi%d" % i for i in range(16)]
STATS_FIELDS += "worst_to_pc_us worst_to_pi_us isr1 isr2".split()
STATS_FIELDS += "isr1_max_us isr2_max_us link_gap_max_us".split() + ["link_gap_hist%d" % i for i in range(16)]
STATS_FIELDS += "link_dry link_nak_polls pc_audio_err".split()
STATS_FMT = "<%dI" % len(STATS_FIELDS)


def made_up_device():
    dev = bytes([18, 1, 0x10, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x03, 0x00, 0x00, 0x01, 1, 2, 0, 1])
    c = [9, 2, 0, 0, 1, 1, 0, 0xC0, 0]
    sizes = {0: 0, 1: 32, 2: 88, 3: 256}
    for alt in range(4):
        neps = 4 + (2 if alt else 0)
        c += [9, 4, 0, alt, neps, 0xFF, 0, 0, 0]
        for a in (0x02, 0x82, 0x01, 0x81):
            c += [7, 5, a, 2, 0x00, 0x02, 0]
        if alt:
            m = sizes[alt]
            c += [7, 5, 0x83, 3, m & 0xFF, m >> 8, 1, 7, 5, 0x03, 3, m & 0xFF, m >> 8, 1]
    c[2], c[3] = len(c) & 0xFF, len(c) >> 8

    def s(t):
        b = t.encode("utf-16-le")
        return bytes([len(b) + 2, 3]) + b
    strings = {0: bytes([4, 3, 0x09, 0x04]), 1: s("Sidecar bench"), 2: s("Relay bench box")}
    # MS OS 2.0: WinUSB for the whole device, its own interface GUID
    guid = "{9E3D51A7-0B2C-4F86-A1D4-7C5E2B90F163}".encode("utf-16-le") + b"\0\0\0\0"
    name = "DeviceInterfaceGUIDs".encode("utf-16-le") + b"\0\0"
    prop = struct.pack("<HHH", 0, 4, 7) + struct.pack("<H", len(name)) + name + struct.pack("<H", len(guid)) + guid
    prop = struct.pack("<H", len(prop)) + prop[2:]
    compat = struct.pack("<HH", 20, 3) + b"WINUSB\0\0" + b"\0" * 8
    msos = struct.pack("<HHIH", 10, 0, 0x06030000, 0) + compat + prop
    msos = msos[:8] + struct.pack("<H", len(msos)) + msos[10:]
    code = 0x22
    bos = bytes([5, 0x0F, 33, 0, 1, 28, 0x10, 5, 0, 0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65,
                 0x9D, 0x9E, 0x64, 0x8A, 0x9F, 0, 0, 3, 6, len(msos) & 0xFF, len(msos) >> 8, code, 0])
    return dev, bytes(c), strings, bos, msos, code


def read_stats(link):
    raw = link.controlRead(0xC0, REQ["Stats"], 0, 0, struct.calcsize(STATS_FMT), timeout=1000)
    raw = raw.ljust(struct.calcsize(STATS_FMT), b"\0")
    return dict(zip(STATS_FIELDS, struct.unpack(STATS_FMT, raw)))


def read_log(link):
    out = b""
    while True:
        b = link.controlRead(0xC0, REQ["Log"], 0, 0, 512, timeout=1000)
        out += bytes(b)
        if len(b) < 512:
            return out.decode("latin-1")


PATTERN = bytes(i & 0xFF for i in range(2048))


def block(seq, n=BLOCK):
    o = (seq + 4) & 0xFF
    return struct.pack("<I", seq & 0xFFFFFFFF) + PATTERN[o:o + n - 4]


def check_block(b, n=BLOCK):
    if len(b) != n:
        return None
    seq = struct.unpack_from("<I", b)[0]
    return seq if b[4] == (seq + 4) & 0xFF and b[-1] == (seq + n - 1) & 0xFF else None


def classify_bad(b, want, n=BLOCK):
    """What a corrupt block is made of: for each byte, which block's pattern it fits (byte i of block s is
    (s + i) & 0xFF for i >= 4), as runs of (first, last, seq offset from WANT or None). Shows a block half-written,
    mixed from two blocks, shifted, or all zero."""
    if want is None:
        want = struct.unpack_from("<I", b)[0]
    runs = []
    for i in range(4, len(b)):
        off = (b[i] - i - want) & 0xFF		# this byte fits block want + off (mod 256)
        off = off - 256 if off >= 128 else off
        if runs and runs[-1][2] == off and runs[-1][1] == i - 1:
            runs[-1][1] = i
        else:
            runs.append([i, i, off])
    head = struct.unpack_from("<I", b)[0]
    return dict(head_seq=head, head_off=(head - want) if want is not None else None, zero=not any(b),
                runs=[r for r in runs if r[1] - r[0] >= 3][:12], nruns=len(runs))


class Seq:
    def __init__(self):
        self.n = self.lost = self.dup = self.bad = 0
        self.next = None

    def see(self, seq):
        if seq is None:
            self.bad += 1
            return
        self.n += 1
        if self.next is not None:
            d = (seq - self.next) & 0xFFFFFFFF
            if d >= 0x80000000:
                self.dup += 1
                return
            self.lost += d
        self.next = (seq + 1) & 0xFFFFFFFF

    def d(self):
        return dict(n=self.n, lost=self.lost, dup=self.dup, bad=self.bad)


class Bench:
    def __init__(self, ctx, link, mirr, seconds):
        self.ctx, self.link, self.mirr, self.seconds = ctx, link, mirr, seconds
        self.stop = False
        self.a_pc, self.a_pi, self.p_pi, self.p_pc = Seq(), Seq(), Seq(), Seq()
        self.events = {}
        self.ctl_q = queue.Queue()
        self.ctl_rtt = []
        self.ctl_bad = 0
        self.ctl_n = 0
        self.taken = 0
        self.lock = threading.Lock()
        self.xfers = []
        self.cred_q = queue.Queue()
        self.streams = []
        self.t_start = time.time()
        self.bad_lens = {}
        self.reset_due = False

    # ---- async helpers
    def _in(self, h, ep, size, depth, cb, kind=None):
        def done(t):
            if self.stop or t.getStatus() != usb1.TRANSFER_COMPLETED:
                if t.getStatus() not in (usb1.TRANSFER_COMPLETED, usb1.TRANSFER_CANCELLED) and not self.stop:
                    print("IN 0x%02x status %d" % (ep, t.getStatus()))
                if self.stop:
                    return
            else:
                cb(bytes(t.getBuffer()[: t.getActualLength()]))
            t.submit()
        for _ in range(depth):
            t = h.getTransfer()
            if kind == "int":
                t.setInterrupt(ep, size, callback=done, timeout=0)
            else:
                t.setBulk(ep, size, callback=done, timeout=0)
            t.submit()
            self.xfers.append(t)

    def _out_stream(self, h, ep, depth, gen, rate, kind="int"):
        """A paced OUT stream: the main loop submits (pump) while fewer than rate * elapsed went out; a finished
        transfer goes back to the idle list from its callback, which never blocks."""
        s = dict(ep=ep, who="pi" if h is self.link else "computer", gen=gen, rate=rate, idle=[], sent=0, t0=None, err=0)

        def done(t):
            if t.getStatus() != usb1.TRANSFER_COMPLETED and not self.stop:
                s["err"] += 1
                print("OUT 0x%02x status %d" % (ep, t.getStatus()))
            s["idle"].append(t)
        for _ in range(depth):
            t = h.getTransfer()
            if kind == "int":
                t.setInterrupt(ep, b"", callback=done, timeout=0)
            else:
                t.setBulk(ep, b"", callback=done, timeout=0)
            s["idle"].append(t)
            self.xfers.append(t)
        self.streams.append(s)
        return s

    def pump(self):
        now = time.perf_counter()
        ls = getattr(self, "lockstep", None)
        if ls is not None:			# engine: one OUT block per IN block read, as soon as a transfer is idle
            while ls["idle"] and ls["sent"] < ls["due"] and not self.stop:
                t = ls["idle"].pop()
                t.setBuffer(block(ls["sent"]))
                ls["sent"] += 1
                t.submit()
            ls["backlog_max"] = max(ls["backlog_max"], ls["due"] - ls["sent"])
        for s in self.streams:
            if s["t0"] is None:
                s["t0"] = now
            due = int((now - s["t0"]) * s["rate"]) + 1
            while s["idle"] and s["sent"] < due and not self.stop:
                t = s["idle"].pop()
                t.setBuffer(s["gen"](s["sent"]))
                s["sent"] += 1
                t.submit()

    def on_audio_pc(self, b):
        if getattr(self, "lockstep", None) is not None:	# the engine answers every block it reads, warm-up or not
            self.lockstep["due"] += len(b) // BLOCK
        if getattr(self, "warmup", 0) and time.time() < self.t_start + self.warmup:
            return
        if getattr(self, "warmup", 0) and not getattr(self, "warm_done", False):
            self.warm_done = True
            self.reset_due = True
        for i in range(0, len(b), BLOCK):
            c = b[i:i + BLOCK]
            if len(c) != BLOCK:
                self.bad_lens[len(c)] = self.bad_lens.get(len(c), 0) + 1
            s = check_block(c)
            if s is None and getattr(self, "dump", None) is not None and len(self.dump) < 200:
                self.dump.append(dict(after=self.a_pc.next, xfer_off=i, xfer_len=len(b), t=round(time.time() - self.t_start, 4),
                                      kind=classify_bad(c, self.a_pc.next), hex=c.hex()))
            self.a_pc.see(s)

    def on_pipe_pc(self, b):
        """An echo the computer has not sent yet in this stage is the previous stage's, left in the relay when the bench
        closed the device (TR1i: one such packet made every later echo count as a duplicate): counted as stale."""
        seq = check_block(b, len(b))
        if seq is not None and seq >= self.s_ppc["sent"]:
            self.p_stale = getattr(self, "p_stale", 0) + 1
            return
        self.p_pc.see(seq)

    # ---- the "Pi"
    def on_event(self, b):
        typ, a, bb, c, t_us = struct.unpack_from("<BBBBI", b)
        name = EV.get(typ, str(typ))
        with self.lock:
            self.events[name] = self.events.get(name, 0) + 1
        if name == "setup":
            self.ctl_q.put((a, bytes(b[8:16])))
        elif name == "intaken":
            self.taken += bb

    def pi_ctl_worker(self):
        while not self.stop:
            try:
                seq, setup = self.ctl_q.get(timeout=0.2)
            except queue.Empty:
                continue
            bm, req, wv, wi, wl = struct.unpack("<BBHHH", setup)
            try:
                if bm & 0x80:
                    ans = bytes(((wv + i) & 0xFF) for i in range(wl))
                    self.link.controlWrite(0x40, REQ["CtlAnswer"], seq, 0, ans, timeout=1000)
                else:
                    data = bytes(self.link.controlRead(0xC0, REQ["CtlData"], seq, 0, max(wl, 1), timeout=1000))
                    ok = data == bytes(((wv * 3 + i) & 0xFF) for i in range(wl))
                    self.link.controlWrite(0x40, REQ["CtlAnswer"], seq, 0 if ok else 1, b"", timeout=1000)
            except usb1.USBError as e:
                print("pi ctl answer:", e)

    def pi_credit_worker(self):
        while not self.stop:
            try:
                n = self.cred_q.get(timeout=0.2)
            except queue.Empty:
                continue
            while not self.cred_q.empty():
                n += self.cred_q.get_nowait()
            try:
                self.link.controlWrite(0x40, REQ["Credit"], 0x02, min(n, 4), b"", timeout=1000)
            except usb1.USBError as e:
                print("credit:", e)

    def on_pipe_to_pi(self, b):
        self.p_pi.see(check_block(b, len(b)) if len(b) >= 8 else None)
        self.cred_q.put(1)

    # ---- the "computer"
    def comp_ctl_worker(self):
        k = 0
        while not self.stop:
            k += 1
            t0 = time.perf_counter()
            try:
                if k & 1:
                    wl = 32 + (k % 64)
                    got = bytes(self.mirr.controlRead(0xC0, 0x41, k & 0xFFFF, 0, wl, timeout=1000))
                    ok = got == bytes((((k & 0xFFFF) + i) & 0xFF) for i in range(wl))
                else:
                    wl = 16 + (k % 100)
                    self.mirr.controlWrite(0x40, 0x42, k & 0xFFFF, 0,
                                           bytes((((k & 0xFFFF) * 3 + i) & 0xFF) for i in range(wl)), timeout=1000)
                    ok = True
            except usb1.USBError as e:
                ok = False
                print("computer ctl:", e)
            self.ctl_rtt.append((time.perf_counter() - t0) * 1e6)
            self.ctl_n += 1
            if not ok:
                self.ctl_bad += 1
            time.sleep(0.005)

    def run(self, paths):
        link, mirr = self.link, self.mirr
        pi = link is not None			# False: the Pi itself is at the link's end (relaytest=1)
        if pi:
            self._in(link, EV_EP, 64, 8, self.on_event, "int")
        if "audio_pc" in paths:		# the "Pi" sends blocks at the box's rate, the "computer" reads 0x83
            if pi:
                self.s_apc = self._out_stream(link, A_FROM_PI, 16, block, BLOCK_HZ)
            self._in(mirr, 0x83, BLOCK * getattr(self, "rd_blocks", 8), getattr(self, "rd_depth", 8), self.on_audio_pc, "int")
        if "engine" in paths:		# the Overbridge Engine's pattern (TR2): reads start late, OUT follows IN block for block
            ls = self.lockstep = dict(idle=[], sent=0, due=0, backlog_max=0, err=0)

            def done(t):
                if t.getStatus() != usb1.TRANSFER_COMPLETED and not self.stop:
                    ls["err"] += 1
                ls["idle"].append(t)
            for _ in range(16):
                t = mirr.getTransfer()
                t.setInterrupt(0x03, b"", callback=done, timeout=0)
                ls["idle"].append(t)
                self.xfers.append(t)
            late = getattr(self, "late", 0)
            if late:
                print("engine: alt 3 taken, first read in %.1f s" % late)
                time.sleep(late)
                self.t_start = time.time()		# the warm-up counts from the first read
            self._in(mirr, 0x83, BLOCK * getattr(self, "rd_blocks", 8), getattr(self, "rd_depth", 8), self.on_audio_pc, "int")
        if "audio_pi" in paths:		# the "computer" sends blocks on 0x03 at the same rate, the "Pi" reads link 0x82
            self.s_api = self._out_stream(mirr, 0x03, 16, block, BLOCK_HZ)
            if pi:
                self._in(link, A_TO_PI, 512, 32, lambda b: self.a_pi.see(check_block(b)), "int")
        if "pipes" in paths:
            # to the box: the "computer" writes 0x02, the "Pi" reads channel 0 IN and grants a credit per packet
            self.s_ppc = self._out_stream(mirr, 0x02, 4, lambda k: block(k, 64 + (k % 400)), PIPE_HZ, "bulk")
            if pi:
                self.link.controlWrite(0x40, REQ["Credit"], 0x02, 1, b"", timeout=1000)
                self._in(link, CH_IN(0), 512, 4, self.on_pipe_to_pi, "bulk")
                # to the computer: the "Pi" writes channel 0 OUT, the "computer" reads 0x82
                self.s_ppi = self._out_stream(link, CH_OUT(0), 4, lambda k: block(k, 64 + (k % 400)), PIPE_HZ, "bulk")
            # (relaytest: the Pi echoes 0x02 on 0x82, so the computer reads its own packets back)
            self._in(mirr, 0x82, 512, 4, self.on_pipe_pc, "bulk")
        workers = [self.pi_ctl_worker, self.pi_credit_worker] if pi else []
        if "ctl" in paths:
            workers.append(self.comp_ctl_worker)
        for f in workers:
            threading.Thread(target=f, daemon=True).start()
        t_end = time.time() + self.seconds
        last = time.time()
        while time.time() < t_end:
            self.ctx.handleEventsTimeout(0.0005)
            self.pump()
            if self.reset_due and self.link is None:
                self.reset_due = False
                try:
                    self.mirr.controlWrite(0x40, 0x7D, 0, 0, b"", timeout=1000)
                except usb1.USBError as e:
                    print("teensy counters reset:", e)
            if time.time() - last >= 5:
                last = time.time()
                print("t+%2.0f a_pc %s a_pi %s pipe>pi %s pipe>pc %s ctl %d bad %d ev %s" % (
                    self.seconds - (t_end - time.time()), self.a_pc.n, self.a_pi.n, self.p_pi.n, self.p_pc.n,
                    self.ctl_n, self.ctl_bad, self.events))
        self.stop = True
        for t in self.xfers:
            try:
                t.cancel()
            except usb1.USBError:
                pass
        end = time.time() + 1
        while time.time() < end:
            self.ctx.handleEventsTimeout(0.05)


def open_dev(ctx, vp, wait=5.0):
    end = time.time() + wait
    while time.time() < end:
        h = ctx.openByVendorIDAndProductID(*vp)
        if h:
            return h
        time.sleep(0.2)
    return None


STAGES = [("ctl",), ("audio_pc",), ("audio_pi",), ("pipes",), ("ctl", "audio_pc", "audio_pi", "pipes")]


def one_stage(ctx, link, paths, seconds):
    if link is None:
        return computer_stage(ctx, paths, seconds)
    dev, cfg, strings, bos, msos, code = made_up_device()
    link.controlWrite(0x40, REQ["Connect"], 0, 0, b"", timeout=1000)
    time.sleep(0.3)
    link.controlWrite(0x40, REQ["SetDevice"], 0, 0, dev, timeout=1000)
    link.controlWrite(0x40, REQ["SetConfig"], 0, 0, cfg, timeout=1000)
    for i, s in strings.items():
        link.controlWrite(0x40, REQ["SetString"], i, 0, s, timeout=1000)
    link.controlWrite(0x40, REQ["SetExtra"], 15, 0, bos, timeout=1000)
    link.controlWrite(0x40, REQ["SetExtra"], 0xEE, code, msos, timeout=1000)
    link.controlWrite(0x40, REQ["StatsReset"], 0, 0, b"", timeout=1000)
    link.controlWrite(0x40, REQ["Connect"], 1, 0, b"", timeout=1000)
    t0 = time.time()
    mirr = open_dev(ctx, MIRR, 15)
    if not mirr:
        print(read_log(link), end="")
        return dict(paths=list(paths), verdict="FAIL: the mirrored device 1209:0003 did not come up on USB2",
                    teensy=read_stats(link))
    print("== %s: mirrored device open after %.2f s" % ("+".join(paths), time.time() - t0))
    mirr.claimInterface(0)
    mirr.setInterfaceAltSetting(0, 3)
    b = Bench(ctx, link, mirr, seconds)
    b.run(paths)
    st = read_stats(link)
    rtt = sorted(b.ctl_rtt)
    sent = {"%s 0x%02x" % (s["who"], s["ep"]): s["sent"] for s in b.streams}
    res = dict(paths=list(paths),
               audio_to_pc=b.a_pc.d(), audio_to_pi=b.a_pi.d(), pipe_to_pi=b.p_pi.d(), pipe_to_pc=b.p_pc.d(),
               sent=sent, intaken=b.taken,
               ctl=dict(n=b.ctl_n, bad=b.ctl_bad, rtt_us_median=rtt[len(rtt) // 2] if rtt else None,
                        rtt_us_max=rtt[-1] if rtt else None),
               events=b.events, teensy=st)
    want = {"audio_pc": ["audio_to_pc"], "audio_pi": ["audio_to_pi"], "pipes": ["pipe_to_pi", "pipe_to_pc"]}
    errs = []
    for path in paths:
        for k in want.get(path, []):
            if res[k]["dup"] or res[k]["bad"]:
                errs.append(k + " dup/bad")
            if res[k]["n"] == 0:
                errs.append(k + " nothing")
            if k.startswith("pipe") and res[k]["lost"]:
                errs.append(k + " lost (a pipe must not lose)")
    if "ctl" in paths and (b.ctl_bad or b.ctl_n == 0):
        errs.append("control answers wrong or none")
    res["verdict"] = "PASS" if not errs else "FAIL: " + ", ".join(errs)
    print(read_log(link), end="")
    print(json.dumps({k: v for k, v in res.items() if k != "teensy"}, indent=1))
    print("teensy: drops pc %d pi %d, qmax pc %d pi %d, worst us pc %d pi %d" % (
        st["audio_to_pc_drop"], st["audio_to_pi_drop"], st["q_to_pc_max"], st["q_to_pi_max"],
        st["worst_to_pc_us"], st["worst_to_pi_us"]))
    print("hist to pc (125 us bins):", [st["hist_to_pc%d" % i] for i in range(16)])
    print("hist to pi (125 us bins):", [st["hist_to_pi%d" % i] for i in range(16)])
    print(res["verdict"])
    mirr.close()
    return res


def computer_stage(ctx, paths, seconds, opts=None):
    """--computer-only: the link is on a Pi running relaytest=1 (teensy_target.cpp), which plays the box: a synthetic IN
    stream at the box's rate, pipe packets echoed (0x02 -> 0x82), the control pattern answered. Only the computer's side
    is checked here; the Pi's own counters are in its log ("relaytest:" line, every 10 s)."""
    t0 = time.time()
    mirr = open_dev(ctx, MIRR, 30)
    if not mirr:
        return dict(paths=list(paths), verdict="FAIL: the made-up box 1209:0003 did not come up (Pi on relaytest=1?)")
    print("== %s (computer only): device open after %.2f s" % ("+".join(paths), time.time() - t0))
    mirr.claimInterface(0)
    mirr.setInterfaceAltSetting(0, 3)
    b = Bench(ctx, None, mirr, seconds)
    b.warmup = 1.0 if ("audio_pc" in paths or "engine" in paths) else 0	# relaytest: blocks queued before this stage are skipped, then the Teensy's
    o = opts or {}					# counters start from zero (0x40 0x7D)
    b.rd_blocks, b.rd_depth = o.get("read_blocks", 8), o.get("read_depth", 8)
    b.dump = [] if o.get("dump") else None
    b.late = o.get("late", 0)
    t_run = time.time()
    b.run(paths)
    t_run = time.time() - t_run - b.late
    rtt = sorted(b.ctl_rtt)
    res = dict(paths=list(paths), reader=dict(blocks=b.rd_blocks, depth=b.rd_depth), pipe_stale=getattr(b, "p_stale", 0),
               audio_to_pc=b.a_pc.d(), pipe_echo=b.p_pc.d(), bad_lengths=b.bad_lens,
               sent={"%s 0x%02x" % (s["who"], s["ep"]): s["sent"] for s in b.streams},
               ctl=dict(n=b.ctl_n, bad=b.ctl_bad, rtt_us_median=rtt[len(rtt) // 2] if rtt else None,
                        rtt_us_max=rtt[-1] if rtt else None))
    errs = []
    ls = getattr(b, "lockstep", None)
    if ls is not None:
        rate = b.a_pc.n / max(t_run - b.warmup, 0.1)
        res["engine"] = dict(late_s=b.late, in_blocks=b.a_pc.n, in_rate=round(rate), out_sent=ls["sent"],
                             out_backlog_max=ls["backlog_max"], out_err=ls["err"])
        print("engine: IN %d blocks (%.0f/s of %.0f), OUT %d sent, backlog max %d" % (
            b.a_pc.n, rate, BLOCK_HZ, ls["sent"], ls["backlog_max"]))
        if rate < 0.98 * BLOCK_HZ:
            errs.append("IN ran at %.0f blocks/s, not the box's %.0f" % (rate, BLOCK_HZ))
        if b.a_pc.lost or b.a_pc.dup or b.a_pc.bad:
            errs.append("audio to the computer lost/dup/bad %d/%d/%d" % (b.a_pc.lost, b.a_pc.dup, b.a_pc.bad))
    if "audio_pc" in paths and (b.a_pc.n == 0 or b.a_pc.dup or b.a_pc.bad):
        errs.append("audio to the computer none/dup/bad")
    if "pipes" in paths and (b.p_pc.n == 0 or b.p_pc.dup or b.p_pc.bad or b.p_pc.lost):
        errs.append("pipe echo none/dup/bad/lost")
    if "ctl" in paths and (b.ctl_bad or b.ctl_n == 0):
        errs.append("control answers wrong or none")
    # relaytest answers 0xC0 0x7E with the Teensy's counters (relay::Stats): how long the box's blocks waited there
    try:
        raw = bytes(mirr.controlRead(0xC0, 0x7E, 0, 0, struct.calcsize(STATS_FMT), timeout=2000))
        st = dict(zip(STATS_FIELDS, struct.unpack(STATS_FMT, raw.ljust(struct.calcsize(STATS_FMT), b"\0"))))
        res["teensy"] = st
        print("teensy: to-pc %d drop %d qmax %d worst %d us | to-pi %d drop %d worst %d us" % (
            st["audio_to_pc"], st["audio_to_pc_drop"], st["q_to_pc_max"], st["worst_to_pc_us"], st["audio_to_pi"],
            st["audio_to_pi_drop"], st["worst_to_pi_us"]))
        print("hist to pc (125 us bins):", [st["hist_to_pc%d" % i] for i in range(16)])
        if ("audio_pc" in paths or "engine" in paths) and st["worst_to_pc_us"] >= 1200:
            errs.append("a block waited %d us on the Teensy (margin ~1200 us)" % st["worst_to_pc_us"])
    except usb1.USBError as e:
        errs.append("the Teensy's counters not read (0x7E): %s" % e)
    res["verdict"] = "PASS" if not errs else "FAIL: " + ", ".join(errs)
    print(json.dumps({k: v for k, v in res.items() if k != "teensy"}, indent=1))
    if b.dump is not None:
        res["bad_dump"] = b.dump			# the first 200 corrupt blocks (classify_bad + bytes)
        kinds = {}
        for d in b.dump:
            k = d["kind"]
            key = "zero" if k["zero"] else "runs=%d head_off=%s offs=%s" % (
                k["nruns"], k["head_off"], ",".join(str(r[2]) for r in k["runs"][:4]))
            kinds[key] = kinds.get(key, 0) + 1
        print("bad block shapes (first %d):" % len(b.dump))
        for k, v in sorted(kinds.items(), key=lambda x: -x[1])[:15]:
            print("  %4d  %s" % (v, k))
    mirr.close()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=15, help="per stage")
    ap.add_argument("--paths", help="comma list of ctl,audio_pc,audio_pi,pipes (default: each alone, then all)")
    ap.add_argument("--json")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--bootloader", action="store_true", help="reboot the Teensy into its bootloader (to reflash)")
    ap.add_argument("--computer-only", action="store_true", help="the link is on a Pi with relaytest=1: check USB2 only")
    ap.add_argument("--read-blocks", type=int, default=8, help="--computer-only: blocks per 0x83 read")
    ap.add_argument("--read-depth", type=int, default=8, help="--computer-only: 0x83 reads kept queued")
    ap.add_argument("--dump", action="store_true", help="--computer-only: keep the first 200 corrupt blocks in the JSON")
    ap.add_argument("--late", type=float, default=0, help="--computer-only engine: seconds between alt 3 and the first read")
    a = ap.parse_args()
    ctx = usb1.USBContext()
    if a.computer_only:
        stages = [tuple(a.paths.split(","))] if a.paths else STAGES
        opts = dict(read_blocks=a.read_blocks, read_depth=a.read_depth, dump=a.dump, late=a.late)
        results = [computer_stage(ctx, s, a.seconds, opts) for s in stages]
        print(chr(10).join("%-32s %s" % ("+".join(r["paths"]), r["verdict"]) for r in results))
        if a.json:
            with open(a.json, "w") as f:
                json.dump(results, f, indent=1)
        sys.exit(0 if all(r["verdict"] == "PASS" for r in results) else 1)
    link = open_dev(ctx, LINK)
    if not link:
        sys.exit("no link device 1209:0002 (USB1 on the PC, relay firmware running?)")
    link.claimInterface(0)
    if a.bootloader:
        try:
            link.controlWrite(0x40, REQ["Bootloader"], 0x4C42, 0, b"", timeout=1000)
        except usb1.USBError:
            pass
        print("bootloader requested")
        return
    st = read_stats(link)
    print("link: hs %d, stats magic %08x version %04x, uptime %d ms" % (st["link_hs"], st["magic"], st["version"], st["uptime_ms"]))
    print(read_log(link), end="")
    if a.info:
        print(json.dumps(st, indent=1))
        return
    stages = [tuple(a.paths.split(","))] if a.paths else STAGES
    results = [one_stage(ctx, link, s, a.seconds) for s in stages]
    link.controlWrite(0x40, REQ["Connect"], 0, 0, b"", timeout=1000)
    print(chr(10).join("%-32s %s" % ("+".join(r["paths"]), r["verdict"]) for r in results))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    sys.exit(0 if all(r["verdict"] == "PASS" for r in results) else 1)


if __name__ == "__main__":
    main()
