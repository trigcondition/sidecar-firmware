#!/usr/bin/env python3
"""The Sidecar sequencer end to end in rytm-emu: the emulated Rytm (rytm-mods 0026 "seq" build, rui1032seq) over the
Overbridge link with the real engine core as the host (sim/poly_live.py), headless (dtboot).

    make -C core lib && uv run python sim/seq_e2e.py --emu ~/PycharmProjects/worktrees/rytm-emu-seqp2 [--out DIR] [RUN ..]

Runs (each a fresh emulator from the base snapshot, A01 at 127 BPM, BD selected; times in M instructions after it,
200 M = 1 s):
  play    the host loads the demo pattern on BD (seq=demo:1: broken chords, loose notes; BD's voice primed as 6-OP FM
          E.PIANO 1); PLAY; GRID REC (RECORD) on BD: seq mode, the host opens its SEQ page, owns the trig and PAGE LEDs
  page    as play, then PAGE held: the page map on the trig LEDs
  held    as play, then T1 held: the step's keyboard, timing lane and values on the SEQ page
  rec     BD sequenced but empty; CHRO; RECORD held + PLAY (live record), the pads played loosely (their MIDI on the auto
          channel 14, bound to voice 0), STOP, GRID REC: the take on the SEQ page and the LEDs
  p2      seq-p2 (rytm-mods rui1033seq2), checked: CY a seq track, EMPTY on the Rytm, one host note on its step 1
          (voice 1 primed as 6-OP FM); BD a stock trig on step 1 (GRID REC + T1) as the reference; the latency calibrated
          automatically (clicks on CY's voice, back in the Rytm's CY stem); FUNC + REC (pattern copy), A02 selected, FUNC
          + STOP (paste): the host's A02 file = A01's; PLAY A01, A02 queued at step 8. Checks: CY's stem carries the host's
          note with no Rytm trig (the pin), CY's onsets within 1.5 ms of BD's from the second bar on, A02's first one too
          (the switch predicted: no late first step), the pattern dumps fetched (the Rytm view's source)
  nopin   as p2 up to the first bar with the pin off (seq=pinseq:off): the host plays, CY's stem stays silent
  makeheld / fresh  dbg: the hardware reports' regressions (dbg_runs)
  preview rui1037: CY a seq track with a chord on step 1, stopped, GRID REC on CY; T1 held + YES: the chord in the
          host's render (and nothing without YES), every note ended
  top     seqtop: CY a seq track of 8 steps at speed 2, REV, a fixed seed, a random step, a ratchet, an FX OCT, a lock
          on 1:2; two PLAYs: the order played (seq=played) the same both times and as the features say; the file v4
Each writes <run>.png (the Rytm's screen at the end, x4, with the 16 trig LEDs and the 4 PAGE LEDs as the emulator
lit them), <run>.wav (the host's voices, mono) and <run>.log (the host's log, the emulator's tail); p2 / nopin also
<run>_r.npy (the Rytm's R frames: MAIN, the 8 voice stems, AUDIO IN) and exit non-zero when a check fails.
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402

TAG = os.environ.get("SEQ_TAG", "rui1037b")   # rui1037: TRK + TRIG opens the SEQ page (rui1036: FUNC + LFO)


def png(path, rgb_rows, w, h):
    raw = b"".join(b"\x00" + bytes(r) for r in rgb_rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_png(path):
    """The emulator's END_PNG (8-bit RGB or grey, no interlace) -> (w, h, rows of RGB bytes)."""
    d = open(path, "rb").read()
    o, idat = 8, b""
    w = h = ct = 0
    while o < len(d):
        n = struct.unpack(">I", d[o:o + 4])[0]
        t = d[o + 4:o + 8]
        c = d[o + 8:o + 8 + n]
        if t == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", c[:10])
        elif t == b"IDAT":
            idat += c
        o += 12 + n
    raw = zlib.decompress(idat)
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    i = 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ct == 0:
            rows.append(bytes(v for g in line for v in (g, g, g)))
        else:
            rows.append(bytes(line) if bpp == 3 else bytes(line[x * bpp + k] for x in range(w) for k in range(3)))
    return w, h, rows


def compose(screen_png, leds, out):
    """The screen (as the emulator wrote it) over a row of the 16 trig LEDs and the 4 PAGE LEDs (page 1..4)."""
    w, h, rows = read_png(screen_png)
    W = max(w, 560) + 32
    bg = (18, 18, 22)
    img = [bytearray(bytes(bg) * W) for _ in range(h + 110)]
    for y in range(h):
        img[16 + y][3 * 16:3 * (16 + w)] = rows[y]
    y0 = h + 56
    for k in range(16):
        c = leds.get(k, (8, 8, 8))
        x = 16 + k * 32 + (k // 4) * 6
        for yy in range(y0, y0 + 20):
            img[yy][3 * x:3 * (x + 24)] = bytes(c) * 24
    for i, led in enumerate((43, 42, 41, 40)):   # page 1..4 (rytm-emu PAGE_LEDS)
        c = leds.get(led, (8, 8, 8))
        x = W - 16 - (4 - i) * 22
        for yy in range(y0 - 26, y0 - 16):
            img[yy][3 * x:3 * (x + 16)] = bytes(c) * 16
    png(out, img, W, h + 110)
    print("wrote", out, flush=True)


def prime_file(path, voice=0, mach=0x48, p=None):
    # the voice (BD's 0) as 6-OP FM, PAT 10 (ROM E.PIANO 1), its own envelopes (DEC 127), LEV 100, LEN 127 (seqx: or
    # another machine and its P1..P16)
    p = p or [10, 64, 64, 127, 64, 64, 100, 127] + [0] * 8
    with open(path, "w") as f:
        f.write(P.params_report(mach | voice, [x << 7 for x in p]).hex() + "\n")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run(name, emu, out, length, events, seq_cmds, card_dir, voice=0, extra=(), prime_mach=0x48, prime_p=None):
    port = free_port()
    prime = os.path.join(out, f"prime{voice}_{prime_mach:02x}.hex")
    prime_file(prime, voice, prime_mach, prime_p)
    log = open(os.path.join(out, f"{name}.log"), "w")
    host = subprocess.Popen([sys.executable, os.path.join(HERE, "poly_live.py"), "--link", f"127.0.0.1:{port}", "--card",
                             card_dir, "--wav", os.path.join(out, f"{name}.wav"), "--prime", prime, "--seq",
                             ",".join(seq_cmds)] + list(extra), stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.5)   # the core's boot
    at = lambda t: f"{7400 + t:g}M"   # noqa: E731
    cmd = [os.path.join(emu, "target/release/dtboot"), f"fw/AR173_us_{TAG}.syx", "--load", f"sections/us_{TAG}.snap",
           "--card", f"sections/us_{TAG}.img", "--instr", f"{length}M", "--every", "100000M", "--usb-host",
           "--usb-midi-real", "--ob-link", f"127.0.0.1:{port}"]
    for e in events:
        if e[1] == "turn":   # (t, "turn", "ENC:DELTA", count, spacing M)
            cmd += ["--turn", f"{e[2]}@{at(e[0])}x{e[3]}/{e[4]}M"]
        elif e[1] == "midi":   # (t, "midi", "hex"): DIN MIDI into the Rytm (fix1049: CC 93 Track Solo)
            cmd += ["--midi", f"{e[2]}@{at(e[0])}"]
        else:
            cmd += ["--press", f"{e[1]}@{at(e[0])}" + (f":{e[2]}M" if len(e) > 2 else "")]
    endpng = os.path.join(out, f"{name}_screen.png")
    env = dict(os.environ, DTBOOT_END_PNG=endpng, DTBOOT_LEDS="rgb")
    r = subprocess.run(cmd, cwd=emu, env=env, capture_output=True, text=True, timeout=1800)
    time.sleep(1.0)
    host.terminate()
    try:
        host.wait(10)
    except subprocess.TimeoutExpired:
        host.kill()
    log.write("\n---- dtboot\n" + r.stdout[-6000:] + r.stderr[-3000:])
    log.close()
    leds = {}
    m = re.search(r"led rgb: (.*)", r.stdout)
    if m:
        for kv in m.group(1).split():
            k, v = kv.split(":")
            leds[int(k)] = tuple(int(v[i:i + 2], 16) for i in (0, 2, 4))
    if os.path.exists(endpng):
        compose(endpng, leds, os.path.join(out, f"{name}.png"))
    host_log = open(os.path.join(out, f"{name}.log")).read()
    stats = [ln for ln in host_log.splitlines() if "seq:" in ln and "ticks" in ln]
    print(f"{name}: dtboot rc {r.returncode}; " + (stats[-1].strip() if stats else "no seq stats"), flush=True)
    return host_log, leds


FAILS = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail != "" else ""), flush=True)
    if not ok:
        FAILS.append(name)


def onsets(x, frames_per_step, thr_frac=0.12, min_gap=0.5):
    """onset frames of a stem: |x| smoothed over 16 frames crossing thr_frac of the stem's peak, then quiet again
    (min_gap steps apart at least)"""
    import numpy as np
    a = np.abs(x.astype(float))
    if a.max() <= 0:
        return []
    k = np.ones(16) / 16
    e = np.convolve(a, k, mode="same")
    thr = thr_frac * e.max()
    out, last = [], -10 ** 9
    above = e > thr
    idx = np.flatnonzero(above[1:] & ~above[:-1]) + 1
    for i in idx:
        if i - last >= min_gap * frames_per_step:
            j = i
            while j > 0 and a[j - 1] > 0.5 * thr:   # back to where it left the floor
                j -= 1
            out.append(int(j))
            last = i
    return out


BLK = 1500 * 60 / 127.0 / 4 * 32   # frames of a 1x step at 127 BPM (16ths)


def p2_runs(a):
    import numpy as np
    import tempfile
    # CY (track 11, voice 6 - CY/CB's, CY its owner; no trig and no envelope start in the base pattern [measured:
    # DTEMU_AMP_TRACE]) on the host: C3 on step 1, its own length 16
    seq = ["seq=track:11:on", "seq=step:11:1:48:127", "seq=len:11:16"]
    f = lambda t, k: [(t, "FUNC", 30), (t + 10, k)]   # noqa: E731
    ev = ([(700, "RECORD"), (750, "T1"), (800, "RECORD")] + f(850, "RECORD") + [(950, "BANK_A", 40), (960, "T2")] +
          f(1050, "STOP") + [(1150, "BANK_A", 40), (1160, "T1"), (1250, "PLAY"), (1250 + 8 * 23.6, "BANK_A", 40),
                             (1260 + 8 * 23.6, "T2"), (2800, "STOP")])
    for name in a.runs:
        if name not in ("p2", "nopin"):
            continue
        card = tempfile.mkdtemp(prefix=f"seq-e2e-{name}-")
        os.makedirs(os.path.join(card, "Samples"))
        rnpy = os.path.join(a.out, f"{name}_r.npy")
        extra = ["--rwav", rnpy, "--rytmpat"]
        if name == "nopin":
            run(name, a.emu, a.out, 1700, [(1250, "PLAY")], seq + ["seq=pinseq:off"], card, voice=6, extra=extra)
        else:
            run(name, a.emu, a.out, 2900, ev, seq, card, voice=6, extra=extra)
        log = open(os.path.join(a.out, f"{name}.log")).read()
        if not os.path.exists(rnpy):
            check(f"{name}: the Rytm's frames recorded", False)
            continue
        r = np.load(rnpy)
        bd, bt = r[:, 2], r[:, 9]   # the stems: BD 2, CY/CB 9 (MAIN 0/1, then BD SD RS/CP BT LT MT/HT CH/OH CY/CB [measured])
        print(f"{name}: {len(r)} frames, BD peak {np.abs(bd).max()}, CY peak {np.abs(bt).max()}")
        if name == "nopin":
            # CY plays from the host into an empty, unpinned Rytm track: its amp never opens
            import wave
            with wave.open(os.path.join(a.out, f"{name}.wav")) as w:
                host = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
            check("nopin: the host played its notes (its own output)", int(np.abs(host).max()) > 1000, int(np.abs(host).max()))
            check("nopin: CY's stem silent without the pin (an empty Rytm track)", np.abs(bt).max() < 0.002 * 2 ** 31,
                  np.abs(bt).max())
            continue
        m = [ln for ln in log.splitlines() if "latency calibrated" in ln]
        print("  " + (m[-1].strip() if m else "no calibration line"))
        check("p2: the latency calibrated automatically (clicks on CY, back in its stem)", bool(m))
        files = {}
        for root, _, fs in os.walk(os.path.join(card, ".rytm", "seq")):
            for fn in fs:
                if fn.endswith(".sqp"):
                    files[fn] = open(os.path.join(root, fn), "rb").read()
        print(f"  files {sorted(files)}")
        check("p2: FUNC + REC on A01, FUNC + STOP on A02: the host's A02 file is A01's", "p000.sqp" in files and
              files.get("p001.sqp") == files.get("p000.sqp"), sorted(files))
        p0 = int(np.flatnonzero(np.abs(bd) > 0.12 * np.abs(bd).max())[0]) - 2400   # PLAY: BD's first hit, step 1
        on_bd = [p0 + o for o in onsets(bd[p0:], BLK)]
        on_bt = [p0 + o for o in onsets(bt[p0:], BLK)]   # (the calibration clicks before PLAY left out)
        print(f"  BD onsets {on_bd[:12]}\n  CY onsets {on_bt[:12]}")
        errs = []
        for o in on_bd:
            near = [b - o for b in on_bt if abs(b - o) < BLK / 2]
            errs.append(near[0] if near else None)
        print(f"  CY - BD per bar (frames): {errs}")
        check("p2: CY (an empty Rytm track) sounds through the pinned amp", len(on_bt) >= 3, len(on_bt))
        good = [e for e in errs[1:] if e is not None]
        # (the residual: two onset shapes - an analog BD against a 6-OP FM E.PIANO through the cymbal voice's chain -
        # and the 32-frame Rytm / 12-frame host blocks; the first bar is late by design: PLAY is not predictable)
        med = sorted(good)[len(good) // 2] if good else None
        print(f"  from the second bar: median {med} frames, worst {max((abs(e) for e in good), default=None)}")
        check("p2: from the second bar on CY lands within 1.5 ms of BD's own trig (latency calibrated, ahead-rendered; "
              "the first of them the predicted switch)", len(good) >= 3 and all(abs(e) <= 72 for e in good), errs)
        sw = [ln for ln in log.splitlines() if "predicted" in ln]
        st = sw[-1] if sw else ""
        mm = re.search(r"predicted (\d+) \(ok (\d+) miss (\d+)\)", st)
        check("p2: the switch to A02 predicted (its first step planned ahead, not late)", bool(mm) and int(mm.group(2)) >= 1,
              mm.groups() if mm else st[-120:])
        d = [ln for ln in log.splitlines() if "Rytm pattern dump" in ln]
        check("p2: the Rytm's pattern dumps fetched (start, the trig placed, the pattern changes)", len(d) >= 3,
              [x.split("]")[-1] for x in d])


def e0_runs(a):
    """seqx (E0): CY (track 11, voice 6) a seq track on PLAITS (not a poly machine before E0) with VOICES 2 ROTATE; a
    3-note chord on step 1 and a note on step 9: the chord plays on 2 instances (one steal), the pattern's file is v4
    (its poly block) and tracks.txt has the track's default; the host's render carries it."""
    import tempfile
    import wave
    import numpy as np
    if "e0" not in a.runs:
        return
    card = tempfile.mkdtemp(prefix="seq-e2e-e0-")
    os.makedirs(os.path.join(card, "Samples"))
    # (--seq is comma-separated: one poly key a command, each applied over the last)
    seq = ["seq=track:11:on", "seq=poly:11:voices=2", "seq=poly:11:alloc=rotate", "seq=step:11:1:48:120",
           "seq=step:11:1:55:120", "seq=step:11:1:60:120", "seq=step:11:9:62:110", "seq=len:11:16"]
    at = []
    for t in (7.0, 7.5, 8.0, 8.4):
        at += ["--at", f"{t}:inst"]
    at += ["--at", "7.0:seq=steps:11", "--at", "7.0:seq=poly:11"]
    plaits = [0, 64, 64, 64, 90, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100]   # VA, DEC 90, LEV 100, VOL 100
    log, _ = run("e0", a.emu, a.out, 1700, [(1250, "PLAY")], seq, card, voice=6, extra=at, prime_mach=0x00,
                 prime_p=plaits)
    m = [ln for ln in log.splitlines() if "at: inst ->" in ln]
    print("  " + (m[-1].strip()[-240:] if m else "no inst line"))
    v7 = [re.search(r"v7 PLAITS inst (\d+)/(\d+) \(x1\).* (\d+) notes (\d+) steals", ln) for ln in m]
    v7 = [x for x in v7 if x]
    check("e0: CY on PLAITS plays the sequencer's notes on its instances (VOICES 2)", bool(v7) and v7[-1].group(2) == "2",
          v7[-1].groups() if v7 else m[-1:])
    check("e0: the 3-note chord on 2 instances stole one (ROTATE)", bool(v7) and int(v7[-1].group(4)) >= 1,
          v7[-1].groups() if v7 else "")
    with wave.open(os.path.join(a.out, "e0.wav")) as w:
        host = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    check("e0: the host's render carries the instances", int(np.abs(host).max()) > 1000, int(np.abs(host).max()))
    sqp, tracks = None, ""
    for root, _, fs in os.walk(os.path.join(card, ".rytm", "seq")):
        for fn in fs:
            if fn == "p000.sqp":
                sqp = open(os.path.join(root, fn), "rb").read()
            if fn == "tracks.txt":
                tracks = open(os.path.join(root, fn)).read()
    check("e0: the pattern's file is .sqp v4 (its poly block)", bool(sqp) and sqp[4] == 4, sqp[:6] if sqp else None)
    ex = {}
    t11 = [t for t in (P.sqp_unpack(sqp, ex) or []) if t["track"] == 10] if sqp else []
    check("e0: the poly config read back (sim/protocol.py's mirror)", bool(t11) and t11[0].get("poly", ())[:2] == (2, 1),
          t11[0].get("poly") if t11 else None)
    check("e0: tracks.txt keeps the track's default", "poly=11:2,1," in tracks, tracks.strip().replace("\n", " "))


def top_runs(a):
    """seqtop: CY (track 11, voice 6, 6-OP FM) a seq track of 8 steps (C3 .. G3) at twice the Rytm's speed, played in
    REV, with a fixed seed: step 3 random notes (C minor, QNT EDIT), step 5 a 3x ratchet, step 7 FX OCT +1, step 2 a P1
    lock on 1:2. Two PLAYs (STOP between): the order the notes started in (seq=played) the same both times and as the
    features say - REV over the 8 steps, two Sidecar steps a Rytm step, the ratchet's 3 notes, the octave, the random
    note in the scale; the pattern's file is .sqp v4 with every block (sim/protocol.py's mirror reads it back)."""
    import tempfile
    if "top" not in a.runs:
        return
    card = tempfile.mkdtemp(prefix="seq-e2e-top-")
    os.makedirs(os.path.join(card, "Samples"))
    seq = ["seq=track:11:on"] + [f"seq=step:11:{k}:{47 + k}:110" for k in range(1, 9)] + [
        "seq=len:11:8", "seq=spd:11:2", "seq=dir:11:rev", "seq=seed:41", "seq=key:C:min:edit", "seq=rndstep:11:3:1",
        "seq=rtrg:11:5:cnt=3", "seq=fx:11:7:1:OCT:1", "seq=lock:11:2:1:100", "seq=lcond:11:2:1:1:2"]
    at = ["--at", "8.55:seq=played:11", "--at", "11.5:seq=played:11", "--at", "11.5:seq=x:11"]
    log, _ = run("top", a.emu, a.out, 2400, [(1250, "PLAY"), (1650, "STOP"), (1850, "PLAY"), (2250, "STOP")], seq, card,
                 voice=6, extra=at)
    lines = [ln for ln in log.splitlines() if "at: seq=played:11 ->" in ln]
    xs = [ln for ln in log.splitlines() if "at: seq=x:11 ->" in ln]
    print("  " + (xs[-1].strip()[-200:] if xs else "no seq=x line"))
    passes = []
    for ln in lines:
        notes = []
        for m in re.finditer(r" (\d+)/(\d+):(\d+):(\d+)", ln.split("->", 1)[1]):
            notes.append(tuple(int(x) for x in m.groups()))   # (v, a, step 1.., pitch)
        passes.append(notes)
    for i, ps in enumerate(passes):
        print(f"  pass {i + 1}: {len(ps)} notes, first {ps[:12]}")
    check("top: two PLAYs, each with notes", len(passes) == 2 and all(len(x) >= 20 for x in passes),
          [len(x) for x in passes])
    if len(passes) != 2 or not passes[0]:
        return
    a1 = [(v, st, p) for v, _, st, p in passes[0]]
    a2 = [(v, st, p) for v, _, st, p in passes[1]]
    n = min(len(a1), len(a2))
    check("top: the second PLAY plays the same notes in the same order (a fixed seed: the random note too)",
          n >= 20 and a1[:n] == a2[:n], (a1[:6], a2[:6]))
    want = {8: [55], 7: [54 + 12], 6: [53], 5: [52, 52, 52], 4: [51], 2: [49], 1: [48]}
    ok_order, ok_rnd, rnd = True, True, set()
    for v, st, p in a1:
        if st != 8 - v % 8:
            ok_order = False
        if st == 3:
            rnd.add(p)
            ok_rnd &= 48 <= p <= 72 and (p % 12) in (0, 2, 3, 5, 7, 8, 10)
        elif p not in want.get(st, []):
            ok_order = False
    check("top: REV over 8 steps, step 7 an octave up, the others as written", ok_order, a1[:12])
    rat = [v for v, st, p in a1 if st == 5]
    check("top: step 5's ratchet - 3 notes at each of its steps", all(rat.count(v) == 3 for v in set(rat)) and len(rat) >= 6,
          rat[:9])
    check("top: step 3's random notes in C minor (48..72)", ok_rnd and bool(rnd), sorted(rnd))
    sp = [(v, aa) for v, aa, _, _ in passes[0] if v >= 8]
    check("top: two Sidecar steps a Rytm step (the Rytm's count at each start: v / 2, a tick late at most)",
          bool(sp) and all(v // 2 - 1 <= aa <= v // 2 for v, aa in sp), sp[:8])
    sqp = None
    for root, _, fs in os.walk(os.path.join(card, ".rytm", "seq")):
        for fn in fs:
            if fn == "p000.sqp":
                sqp = open(os.path.join(root, fn), "rb").read()
    check("top: the pattern's file is .sqp v4", bool(sqp) and sqp[4] == 4, sqp[:6] if sqp else None)
    ex = {}
    t11 = [t for t in (P.sqp_unpack(sqp, ex) or []) if t["track"] == 10] if sqp else []
    t = t11[0] if t11 else {}
    st_ = t.get("steps", {})
    check("top: the blocks read back (speed 2, REV, C minor EDIT, step 3 random, step 5's ratchet, step 7's FX, step 2's "
          "lock condition)", t.get("speed") == P.SEQ_SPEEDS.index("2") and t.get("dir") == P.SEQ_DIRS.index("REV") and
          ex.get("scale") == (0, P.SEQ_SCALES.index("MIN"), 1) and st_.get(2, {}).get("rnd") == 1 and
          st_.get(4, {}).get("rt", (0,))[0] == 3 and st_.get(6, {}).get("fx", ((0, 0),))[0] == (7, 1) and
          len(st_.get(1, {}).get("locks", [(0, 0)])[0]) == 3,
          {k: t.get(k) for k in ("speed", "dir", "rnd")} | {"scale": ex.get("scale")})


def ux_runs(a):
    """seq-ux (rytm-mods rui1036): screenshots of the remote pages on the Rytm's screen (the make prompt, the lock page
    with locked knobs, the SEQ page with locks) and a p-locked pattern played: the locks on their steps in the render."""
    import numpy as np
    import shutil
    import tempfile
    import wave
    shots = os.path.join(a.out, "sequx")
    os.makedirs(shots, exist_ok=True)
    # BD to 6-OP FM on its SRC list (machine 40: 20 DOWNs from BD PLASTIC); TRK + TRIG: the make prompt; YES; GRID REC
    # (seq mode, the SEQ page); T1 held + SRC + B and G turned: the lock page; T1 let go: the SEQ page with its locks
    ev = [(100, "SRC"), (200, "SRC")] + [(300 + 50 * i, "DOWN") for i in range(20)] + [(1320, "YES"),
          (1450, "TRK", 40), (1460, "TRIG")]
    after = [(1700, "YES"), (1800, "RECORD"), (1900, "T1", 400), (1940, "SRC"), (1980, "turn", "1:+3", 4, 10),
             (2050, "turn", "6:-2", 3, 10)]
    seq = ["seq=lat:192"]
    at = []   # BD's steps and locks once the project is named (the Rytm's 10 loads its own: earlier ones would go)
    for c in ("seq=step:1:1:60:110", "seq=step:1:5:64:100", "seq=step:1:9:67:100", "seq=step:1:13:72:90",
              "seq=step:1:13:76:90", "seq=lock:1:9:2:100", "seq=lock:1:9:4:20", "seq=lock:1:12:1:40"):
        at += ["--at", f"6.0:{c}"]
    for name, length, e in (("ux_make", 1650, ev), ("ux_lock", 2150, ev + after), ("ux_locks", 2500, ev + after)):
        if "ux" not in a.runs and name not in a.runs:
            continue
        card = tempfile.mkdtemp(prefix=f"seq-e2e-{name}-")
        os.makedirs(os.path.join(card, "Samples"))
        log, _ = run(name, a.emu, a.out, length, e, seq, card, extra=at)
        if os.path.exists(os.path.join(a.out, f"{name}.png")):
            shutil.copy(os.path.join(a.out, f"{name}.png"), os.path.join(shots, f"{name}.png"))
        if name == "ux_make":
            check("ux: TRK + TRIG on BD (6-OP FM, no seq track): the SEQ page asks to make it one",
                  bool(re.search(r"mask 0x000.*views 1 \(make prompt\)", log)), "")
        if name == "ux_lock":
            check("ux: YES made BD a seq track (mask 0x001)", bool(re.search(r"mask 0x001.*views 1", log)), "")
        if name == "ux_locks":
            m = re.findall(r"locks applied (\d+) set (\d+)", log)
            check("ux: hold T1 + SRC + B / G turned: locks written (seq stats)", bool(m) and int(m[-1][1]) >= 2, m[-1:])
    if "preview" in a.runs:
        preview_runs(a)
    if "lock" not in a.runs:
        return
    # the played locks: CY (track 11, voice 6) a seq track, EMPTY on the Rytm, C3 on steps 1 5 9 13; steps 5 and 13
    # lock LEV (P7) to 12: every second note quieter in the host's own render, the others at full level
    card = tempfile.mkdtemp(prefix="seq-e2e-lock-")
    os.makedirs(os.path.join(card, "Samples"))
    seq = ["seq=track:11:on", "seq=len:11:16"] + [f"seq=step:11:{s}:48:127" for s in (1, 5, 9, 13)] + \
          ["seq=lock:11:5:7:12", "seq=lock:11:13:7:12"]
    rnpy = os.path.join(a.out, "lock_r.npy")
    run("lock", a.emu, a.out, 2400, [(1250, "PLAY"), (2300, "STOP")], seq, card, voice=6, extra=["--rwav", rnpy])
    log = open(os.path.join(a.out, "lock.log")).read()
    with wave.open(os.path.join(a.out, "lock.wav")) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
    # the notes, 4 steps apart from the first one after the connect's calibration clicks (the first 3 s): each note's
    # peak in its window (a quarter step before its step to one step after)
    on = [o for o in onsets(x, BLK, min_gap=2) if o > 3 * 48000]
    seqp = []
    if on:
        for k in range(8):
            c = on[0] + int(round(k * 4 * BLK))
            if c + int(BLK) <= len(x):
                seqp.append(float(np.abs(x[max(0, c - int(BLK / 4)):c + int(BLK)]).max()))
    print(f"  lock: first note at {on[:1]}, peaks every 4 steps {[int(p) for p in seqp]}")
    a_, b_ = seqp[0::2], seqp[1::2]   # step 1 / 9 and step 5 / 13 (the first found note is a loud one or a quiet one)
    loud, quiet = (a_, b_) if seqp and np.mean(a_) >= np.mean(b_ or [0]) else (b_, a_)
    print(f"  lock: unlocked steps {[int(p) for p in loud]}, LEV-locked steps {[int(p) for p in quiet]}")
    check("lock: the host played its 4 steps a bar (8 notes measured)", len(seqp) == 8, len(seqp))
    check("lock: the LEV-locked steps (5, 13) under half the unlocked ones (1, 9): the lock applied on its step and "
          "given back at the next", len(seqp) == 8 and max(quiet) < 0.5 * min(loud), (loud, quiet))
    m = re.findall(r"locks applied (\d+) set (\d+)", log)
    check("lock: lock sets applied at their steps (seq stats)", bool(m) and int(m[-1][0]) >= 4, m[-1:])


def preview_runs(a):
    """rui1037, the step preview: CY (track 11, voice 6 primed as 6-OP FM) a seq track, EMPTY on the Rytm, a C3 chord
    (C3 E3 G3) on its step 1; stopped, CY selected (TRK + CY), GRID REC (seq mode); T1 held + YES: the host plays the
    step's notes in its own render, no PLAY. The same without YES: nothing. The seq stats: every note on has its off."""
    import numpy as np
    import tempfile
    import wave
    seq = ["seq=lat:192", "seq=track:11:on", "seq=len:11:16", "seq=step:11:1:48:120", "seq=step:11:1:52:120",
           "seq=step:11:1:55:120"]
    sel = [(800, "TRK", 40), (810, "CY"), (900, "RECORD"), (1100, "T1", 120)]
    res = {}
    for name, ev in (("preview", sel + [(1140, "YES", 20)]), ("preview_none", sel)):
        card = tempfile.mkdtemp(prefix=f"seq-e2e-{name}-")
        os.makedirs(os.path.join(card, "Samples"))
        log, _ = run(name, a.emu, a.out, 1500, ev, seq, card, voice=6)
        with wave.open(os.path.join(a.out, f"{name}.wav")) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
        on = [o for o in onsets(x, BLK, min_gap=2) if o > 3 * 48000]   # after the connect's calibration clicks
        st = re.findall(r"on (\d+) off (\d+)", log)
        pv = re.findall(r"previews? (\d+)", log)
        res[name] = (on, st[-1] if st else None, float(np.abs(x[3 * 48000:]).max()) if len(x) > 3 * 48000 else 0.0)
        print(f"  {name}: onsets after 3 s {on[:4]}, peak {res[name][2]:.0f}, notes on / off {res[name][1]}")
    p_on, p_st, p_pk = res["preview"]
    n_on, n_st, n_pk = res["preview_none"]
    check("preview: T1 held + YES, stopped: the step's chord in the host's render (an onset, CY's voice)",
          len(p_on) >= 1 and p_pk > 4 * max(n_pk, 1.0), (p_on[:2], p_pk, n_pk))
    check("preview: without YES nothing sounds (T1 held alone)", not n_on, n_on[:2])
    check("preview: every note on ended (seq stats on = off, 3 notes)", p_st is not None and int(p_st[0]) >= 3 and
          p_st[0] == p_st[1], p_st)


def dbg_runs(a):
    """dbg (hardware reports on rui1037 / Pi fe1e176):
    makeheld  BD to 6-OP FM, TRK + TRIG (the make prompt) and YES while TRK is still held (rytm-mods rui1037b: that YES
              is the page's, rui1037 took it as TRK + YES and left the page unanswered), GRID REC, trigs T1 T2 T3 T4 T9
              T11 T13, then out: TRK + TRIG (the page closed, seq mode on), RECORD (GRID REC off), T2 and T4 pressed:
              BD made a seq track (mask 0x001), the notes the host's, no TRK left held on the Rytm (its LED dark), the trig
              LEDs stock's after leaving (all dark: no stock trigs placed on BD), T2 / T4 then stock's (CLAP selected,
              the screen stock's)
    fresh     BD a seq track, GRID REC, playing; an empty trig held (T3), A +4, B +3, E (CHD) +1: the step's notes start
              from the track's default note (60): E major 64 68 71 (Pi fe1e176 made it from the last note any channel
              played, C0 on hardware)"""
    import tempfile
    if "makeheld" in a.runs:
        card = tempfile.mkdtemp(prefix="seq-e2e-makeheld-")
        os.makedirs(os.path.join(card, "Samples"))
        ev = [(100, "SRC"), (200, "SRC")] + [(300 + 50 * i, "DOWN") for i in range(20)] + [(1320, "YES"),
              (1450, "TRK", 300), (1460, "TRIG"), (1600, "YES"), (1800, "RECORD")]
        ev += [(1900 + 40 * i, f"T{k}", 10) for i, k in enumerate((1, 2, 3, 4, 9, 11, 13))]
        ev += [(2250, "TRK", 40), (2260, "TRIG"), (2350, "RECORD"), (2600, "T2", 10), (2700, "T4", 10)]
        log, leds = run("makeheld", a.emu, a.out, 2900, ev, ["seq=lat:192"], card, extra=["--at", "12.5:seq=steps:1"])
        m = re.findall(r"seq: steps(.*)", log)
        print(f"  makeheld: BD's host steps{m[-1:] or ' -'}; trig LEDs {[leds.get(k) for k in range(16)]}; TRK {leds.get(55)}")
        check("makeheld: YES with TRK held answered the make prompt (mask 0x001)", bool(re.search(r"mask 0x001", log)), "")
        check("makeheld: the 7 trigs the host's steps (1 2 3 4 9 11 13)",
              bool(m) and [int(x.split(":")[0]) for x in m[-1].split()] == [1, 2, 3, 4, 9, 11, 13], m[-1:])
        check("makeheld: after leaving every trig LED dark (no stock trigs on BD, no LED left the host's), TRK's dark",
              all(leds.get(k) == (8, 8, 8) for k in range(16)) and leds.get(55) == (8, 8, 8),
              {k: leds.get(k) for k in list(range(16)) + [55] if leds.get(k) != (8, 8, 8)})
    if "fresh" in a.runs:
        card = tempfile.mkdtemp(prefix="seq-e2e-fresh-")
        os.makedirs(os.path.join(card, "Samples"))
        ev = [(300, "PLAY"), (500, "RECORD"), (900, "T3", 300), (960, "turn", "0:2", 2, 10), (1000, "turn", "1:3", 3, 10),
              (1060, "turn", "4:1", 1, 10), (1400, "STOP")]
        log, _ = run("fresh", a.emu, a.out, 1900, ev, ["seq=lat:192", "seq=track:1:on", "seq=len:1:16"], card,
                     extra=["--at", "9.0:seq=steps:1"])
        m = re.findall(r"seq: steps(.*)", log)
        print(f"  fresh: BD's steps{m[-1:] or ' -'}")
        check("fresh: the held empty step made E major from C3: 3:64,68,71", bool(m) and m[-1].strip() == "3:64,68,71", m[-1:])


def rui1040_runs(a):
    """rui1040 (the hardware report on rui1039 + the user's TRIG / condition requests):
    stepedit  BD a seq track (C4 on step 1, E4 on step 3), GRID REC (seq mode: the host's SEQ page open): T1 held + FUNC +
              REC (copy), T5 held + FUNC + STOP (paste), T3 held + FUNC + PLAY (clear): the host's steps 1:60 5:60; the
              Rytm sent trig ops (2D 0A 0D / 0E / 0F, src 1), never stock's track ops 05 / 06 / 07 (rui1039: the whole
              track copied / pasted / cleared)
    trigview  seq mode: TRIG -> the SEQ page's TRIG view (the screen differs from the notes view), TRIG again -> the notes
              (the screen as without either press); TRIG, T1 held + E (COND) +9 -> step 1 gets 1:2; T1 held + SRC: LOCK SRC
    cond      BD 4 steps (C4 on 1, E4 on 2 with 1:2 by seq=cond), playing ~2.3 s; the same without the condition: the
              notes on with it ~3/4 of those without (step 2 every second loop)"""
    import tempfile
    seq = ["seq=lat:192", "seq=track:1:on", "seq=len:1:16", "seq=step:1:1:60:100", "seq=step:1:3:64:100"]

    def newcard(n):
        c = tempfile.mkdtemp(prefix=f"seq-e2e-{n}-")
        os.makedirs(os.path.join(c, "Samples"))
        return c
    if "stepedit" in a.runs:
        ev = [(300, "RECORD"), (700, "T1", 120), (740, "FUNC", 40), (750, "RECORD"), (900, "T5", 120), (940, "FUNC", 40),
              (950, "STOP"), (1100, "T3", 120), (1140, "FUNC", 40), (1150, "PLAY")]
        log, leds = run("stepedit", a.emu, a.out, 1400, ev, seq, newcard("stepedit"), extra=["--at", "6.5:seq=steps:1"])
        m = re.findall(r"seq: steps(.*)", log)
        ed = re.findall(r"Rytm -> host  f0 7d 52 54 2d 01 0a (\w\w) 00 00 01 (\w\w)", log)
        print(f"  stepedit: steps {m[-1:]}; edits {ed}; trig LEDs {[leds.get(k) for k in range(8)]}")
        check("stepedit: copy T1, paste on T5, clear T3: the host's steps 1:60 5:60", bool(m) and m[-1].strip() == "1:60 5:60", m[-1:])
        check("stepedit: the Rytm reported trig copy / paste / clear (0D 0E 0F, src 1), no stock track op",
              [x for x, _ in ed] == ["0d", "0e", "0f"] and all(y == "01" for _, y in ed), ed)
    if "trigview" in a.runs:
        base = [(300, "RECORD")]
        run("tv_base", a.emu, a.out, 900, base, seq, newcard("tv"))
        run("tv_trig", a.emu, a.out, 900, base + [(600, "TRIG", 20)], seq, newcard("tv"))
        run("tv_notes", a.emu, a.out, 900, base + [(600, "TRIG", 20), (640, "TRIG", 20), (680, "TRIG", 20), (720, "TRIG", 20)], seq,
            newcard("tv"))   # rui1050: TRIG cycles DEFAULTS SCALE RANDOM, the notes again
        log, _ = run("tv_cond", a.emu, a.out, 1300, base + [(600, "TRIG", 20), (700, "T1", 250), (760, "turn", "4:9", 1, 10)],
                     seq, newcard("tv"), extra=["--at", "5.6:seq=steps:1"])
        run("tv_lock", a.emu, a.out, 900, base + [(700, "T1", 250), (760, "SRC", 20)], seq, newcard("tv"))
        rd = lambda n: read_png(os.path.join(a.out, f"{n}_screen.png"))[2]   # noqa: E731
        m = re.findall(r"seq: steps(.*)", log)
        print(f"  trigview: steps {m[-1:]}")
        check("trigview: TRIG -> the TRIG view (the screen differs from the notes view)", rd("tv_trig") != rd("tv_base"))
        check("trigview: TRIG x4 -> the notes view again (the screen as without TRIG)", rd("tv_notes") == rd("tv_base"))
        check("trigview: TRIG, T1 held + COND +9 -> step 1 on 1:2", bool(m) and m[-1].strip().startswith("1:60[1:2]"), m[-1:])
        check("trigview: T1 held + SRC: the lock page (the screen differs from the TRIG view and the notes)",
              rd("tv_lock") != rd("tv_base") and rd("tv_lock") != rd("tv_trig"))
    if "padsel" in a.runs:
        # fix1040b: BD's own pad in the normal mode plays its 6-OP FM voice (poly's seq pads; bug 5's gate had silenced it).
        # bug 5 (hardware, rui1039 + 5cc980e): BD 6-OP FM a seq track (the SRC list, TRK + TRIG, YES), GRID REC, PLAY, BD
        # T1 tapped (a step), pad played (normal mode: ch 14 note 0, its 0x04: rui1039's host could bind ch 14 to BD's voice by these votes);
        # the SD pad selects SD (stock: a pad in GRID REC selects its track): seq mode ends, the SEQ page closes, the pads
        # play no Sidecar voice, T2 places SD's own (red) trig, BD's steps unchanged and still playing; TRK + BD pad:
        # seq mode again (the SEQ page opens); CHRO there: the pads play BD's poly voice (ch 14 bound); still in CHRO with
        # the page open, TRK + SD selects SD (rytm-mods rui1040: stock gets the caught TRK's press; rui1039 played SD's
        # chromatic note and stayed on BD); CHRO pads: the binding gone (SD's chromatic, no Sidecar voice).
        # Times: host seconds ~ M / 200 - 0.06.
        ev = [(100, "SRC"), (200, "SRC")] + [(300 + 50 * i, "DOWN") for i in range(20)] + [(1320, "YES"),
              (1450, "TRK", 300), (1460, "TRIG"), (1600, "YES"), (1800, "RECORD"), (1850, "T1", 8), (1900, "PLAY")]
        ev += [(1950, "BD", 15), (2000, "BD", 15), (2050, "BD", 15)]          # BD's pad (normal mode) x 3
        ev += [(2150, "SD", 15), (2250, "T2", 8), (2300, "SD", 15), (2350, "BT", 15), (2400, "CP", 15)]
        ev += [(2550, "TRK", 40), (2560, "BD"), (2650, "CHRO"), (2700, "BD", 15), (2730, "SD", 15), (2760, "BD", 15),
               (2790, "SD", 15), (2820, "RS", 15)]
        ev += [(2950, "TRK", 40), (2960, "SD"), (3080, "BD", 15), (3110, "SD", 15), (3140, "RS", 15), (3170, "CP", 15)]
        log, leds = run("padsel", a.emu, a.out, 3400, ev, ["seq=lat:192"], newcard("padsel"),
                        extra=["--at", "10.6:poly", "--at", "10.61:seq", "--at", "12.4:poly", "--at", "12.41:seq",
                               "--at", "12.45:seq=steps:1", "--at", "14.4:poly", "--at", "14.41:seq", "--at", "15.2:poly",
                               "--at", "15.21:seq", "--at", "16.4:poly", "--at", "16.41:seq"])
        ats = re.findall(r"at: (poly|seq=steps:1|seq) -> (.*)", log)
        pl = [x for k, x in ats if k == "poly"]
        sq = [x for k, x in ats if k == "seq"][1:2]
        sons = [int(re.search(r" on (\d+)", x).group(1)) for k, x in ats if k == "seq"]
        stp = [x for k, x in ats if k == "seq=steps:1"]
        modes = re.findall(r"2d 01 08 (\w\w) (\w\w)", log)
        live = [int(re.search(r"(\d+) live", x).group(1)) for x in pl]
        pads = [a_ - b_ for a_, b_ in zip(live, sons)] if len(sons) == len(live) else []   # live notes not the sequencer's
        bound = [re.search(r"bound (.*)", x).group(1) if "bound" in x else "?" for x in pl]
        spads = [int(m.group(1)) if (m := re.search(r"(\d+) seq pads", x)) else -1 for x in pl]
        print(f"  padsel: 08 {modes}; poly live {live} seq on {sons} -> the pads' {pads}; bound {bound}; steps {stp}; "
              f"seq pads {spads}")
        print(f"  padsel: trig LEDs {[leds.get(k) for k in range(4)]}")
        ok5 = len(pads) == 5
        check("padsel: seq mode on BD, off when the SD pad selected SD, on again with TRK + BD, off with TRK + SD (CHRO, "
              "the page open)",
              [m for m in modes if m != ("00", "7f")][:1] == [("01", "00")] and modes[-4:] == [("01", "00"), ("00", "7f"),
              ("01", "00"), ("00", "7f")], modes)
        check("padsel: the SEQ page closed when SD was selected", "the SEQ page closed" in log)
        check("padsel: fix1040b: BD's own pad (normal mode, seq track) plays its 6-OP FM voice: 3 presses, 3 seq pads",
              len(spads) == 5 and spads[0] == 3, spads)
        check("padsel: fix1040b: SD's pads (SD selected) play no Sidecar voice (seq pads unchanged)",
              len(spads) == 5 and spads[1] == spads[0], spads)
        check("padsel: SD selected, normal pads: no Sidecar voice played, nothing bound (rui1039 could bind ch 14 to BD's "
              "voice)", ok5 and pads[1] == pads[0] and bound[1] == "none", (pads, bound))
        check("padsel: BD's steps: T1's tap only (step 1; not T2, no pad note), BD still playing (notes on)",
              bool(stp) and re.fullmatch(r"seq: steps 1:\d+", stp[0].strip()) is not None and bool(sq) and int(re.search(r"on (\d+)", sq[0]).group(1)) >= 2,
              (stp, [re.search(r"on \d+", x).group(0) for x in sq]))
        check("padsel: back on BD, CHRO: the pads play BD's poly voice (ch 14 bound, live notes)",
              ok5 and pads[2] > pads[1] and bound[2].startswith("ch14->v0"), (pads, bound))
        check("padsel: SD selected, CHRO: the binding gone, SD's chromatic plays no Sidecar voice",
              ok5 and bound[3] == "none" and pads[4] == pads[3] and bound[4] == "none" and "unbound" in log, (pads, bound))
        # 1042 (the CHRO leftover): TRK + SD's own pad press (CHRO: stock sends its chromatic note too) played one note on
        # BD's bound voice before the selection report (49 02) came a UI tick later [measured, rui1041: pads 4 -> 5]
        check("padsel: 1042: TRK + SD in CHRO sounds no note on BD's voice (the selection reported before the pad)",
              ok5 and pads[3] == pads[2], pads)
    if "cond" in a.runs:
        res = {}
        for n, extra in (("cond", ["seq=cond:1:2:9"]), ("cond_none", [])):
            sq = ["seq=lat:192", "seq=track:1:on", "seq=len:1:4", "seq=step:1:1:60:100", "seq=step:1:2:64:100"] + extra
            log, _ = run(n, a.emu, a.out, 1300, [(300, "PLAY"), (760, "STOP")], sq, newcard(n),
                         extra=["--at", "6.0:seq=steps:1"])
            st = re.findall(r"on (\d+) off (\d+)", log)
            res[n] = int(st[-1][0]) if st else 0
        print(f"  cond: notes on with 1:2 {res['cond']}, without {res['cond_none']}")
        r_ = res["cond"] / max(res["cond_none"], 1)
        check("cond: 1:2 on step 2 plays it every second loop (notes on ~3/4 of the run without)",
              res["cond_none"] >= 8 and 0.62 <= r_ <= 0.88, (res, round(r_, 2)))
    if "solo" in a.runs:
        # fix1049 (the hardware pass: soloing another Rytm track left the Sidecar voices playing): BD a seq track playing
        # every step; SD soloed on the Rytm (CC 93 Track Solo on SD's channel 2, before PLAY) -> the Rytm's 2D 0D 02 00
        # (rui1049), the host plays none of BD's notes; BD soloed instead: all of them; none soloed: all of them
        res = {}
        for n, ev in (("solo_sd", [(150, "midi", "b15d01")]), ("solo_bd", [(150, "midi", "b05d01")]), ("solo_none", [])):
            sq = ["seq=lat:192", "seq=track:1:on", "seq=len:1:4"] + [f"seq=step:1:{k}:60:100" for k in range(1, 5)]
            log, _ = run(n, a.emu, a.out, 1300, ev + [(300, "PLAY"), (760, "STOP")], sq, newcard(n),
                         extra=["--at", "6.0:seq=steps:1"])
            st = re.findall(r"on (\d+) off (\d+)", log)
            res[n] = int(st[-1][0]) if st else -1
        print(f"  solo: BD's notes on with SD soloed {res['solo_sd']}, BD soloed {res['solo_bd']}, none {res['solo_none']}")
        check("solo: SD soloed on the Rytm: none of BD's seq notes play; BD soloed or none soloed: they play",
              res["solo_sd"] == 0 and res["solo_bd"] >= 8 and res["solo_none"] >= 8, res)


def rui1050_runs(a):
    """rui1050 (the Rytm side of seqtop, rytm-mods rui1050): BD a seq track of 4 steps (C4 D4 E4 F4), GRID REC (seq mode:
    the host's SEQ page open), every edit made with the Rytm's own keys and knobs, then played; the order / pitches the
    real core played (seq=played), its settings (seq=x) and the pattern's file. Each run's end screen is the page it used.
      r50dir    TRIG (DEFAULTS), E +2: PEND - the order played 1 2 3 4 3 2 ..
      r50spd    TRIG, F +1: SPD 3/2 - three Sidecar steps every two of the Rytm's
      r50key    TRIG TRIG (SCALE), B +2 (MIN), C +2 (QNT PLAY): E4 plays as Eb4 (C minor), the rest as written
      r50seed   TRIG x3 (RANDOM), A +42 in 6 turns (FREE -> a fixed seed), D +2 (TRACK): two PLAYs, the same random notes
      r50rtrg   T1 held, TRIG TRIG (RTRG), B +4: CNT 4 - step 1 plays 4 notes a pass
      r50fx     T2 held, TRIG x3 (FX), A +9 (JMP), B +3 (4): after step 2 step 4, step 3 never
      r50cond   step 1 a P1 lock (seq=lock); T1 held, SRC, A pushed 0.5 s (its COND), A +5 (1:2): the file's lock on 1:2
      r50slide  T1 held, SRC, A +5, FUNC + A pushed: the file's lock slides
      r50menu   step 2 random (seq=rndstep); FUNC + DOWN (the menu, updown1029), DOWN x4 (PLAY MODE), RIGHT: REV; DOWN, YES (PRINT RND):
                step 2's random note written, RND off"""
    import tempfile
    runs = [n for n in a.runs if n.startswith("r50")]
    if not runs:
        return
    seq = ["seq=lat:192", "seq=track:1:on", "seq=len:1:4", "seq=step:1:1:60:100", "seq=step:1:2:62:100",
           "seq=step:1:3:64:100", "seq=step:1:4:65:100"]

    def newcard(n):
        c = tempfile.mkdtemp(prefix=f"seq-e2e-{n}-")
        os.makedirs(os.path.join(c, "Samples"))
        return c

    def played(log):
        out = []
        for ln in log.splitlines():
            if "at: seq=played:1 ->" in ln:
                out.append([tuple(int(x) for x in m.groups()) for m in re.finditer(r" (\d+)/(\d+):(\d+):(\d+)", ln.split("->", 1)[1])])
        return out

    def xline(log):
        xs = [ln for ln in log.splitlines() if "at: seq=x:1 ->" in ln]
        return xs[-1] if xs else ""

    def sqp(card):   # the newest p000.sqp (the project the Rytm names comes a few seconds in: a folder of its own)
        fs = [os.path.join(r_, fn) for r_, _, f_ in os.walk(os.path.join(card, ".rytm", "seq")) for fn in f_ if fn == "p000.sqp"]
        if not fs:
            return {}, {}
        ex = {}
        t1 = [t for t in (P.sqp_unpack(open(max(fs, key=os.path.getmtime), "rb").read(), ex) or []) if t["track"] == 0]
        return (t1[0] if t1 else {}), ex
    at = ["--at", "6.9:seq=played:1", "--at", "6.95:seq=x:1"]
    rec = [(300, "RECORD")]
    play = [(800, "PLAY"), (1400, "STOP")]
    if "r50dir" in runs:
        log, _ = run("r50dir", a.emu, a.out, 1500, rec + [(500, "TRIG", 20), (600, "turn", "4:2", 1, 10)] + play, seq,
                     newcard("r50dir"), extra=at)
        ps = played(log)
        st = [x[2] for x in ps[-1]] if ps else []
        cyc = [1, 2, 3, 4, 3, 2]
        ok = len(st) >= 8 and any(all(st[i] == cyc[(j + i) % 6] for i in range(len(st))) for j in range(6))
        print(f"  r50dir: steps played {st[:16]}; {xline(log)[-120:]}")
        check("r50dir: TRIG, E +2 -> PEND: the order 1 2 3 4 3 2 ..", ok and "DIR PEND" in xline(log), st[:16])
    if "r50spd" in runs:
        log, _ = run("r50spd", a.emu, a.out, 1500, rec + [(500, "TRIG", 20), (600, "turn", "5:1", 1, 10)] + play, seq,
                     newcard("r50spd"), extra=at)
        ps = played(log)
        va = [(v, aa) for v, aa, _, _ in (ps[-1] if ps else [])]
        print(f"  r50spd: (v, a) {va[:10]}; {xline(log)[-120:]}")
        check("r50spd: TRIG, F +1 -> SPD 3/2: Sidecar step v starts at the Rytm's 2v/3 (a tick late at most)",
              "SPD 3/2" in xline(log) and len(va) >= 6 and all(v * 2 // 3 - 1 <= aa <= v * 2 // 3 + 1 for v, aa in va if v >= 3), va[:10])
    if "r50key" in runs:
        log, _ = run("r50key", a.emu, a.out, 1500, rec + [(500, "TRIG", 20), (540, "TRIG", 20), (600, "turn", "1:2", 1, 10),
                     (650, "turn", "2:2", 1, 10)] + play, seq, newcard("r50key"), extra=at)
        ps = played(log)
        pit = sorted({p_ for _, _, _, p_ in (ps[-1] if ps else [])})
        print(f"  r50key: pitches {pit}; {xline(log)[-120:]}")
        check("r50key: SCALE, B +2 (MIN), C +2 (QNT PLAY): E4 plays as Eb4, C D F as written", pit == [60, 62, 63, 65] and
              "KEY C MIN QNT PLAY" in xline(log), pit)
    if "r50seed" in runs:
        at2 = ["--at", "6.9:seq=played:1", "--at", "9.9:seq=played:1", "--at", "9.95:seq=x:1"]
        log, _ = run("r50seed", a.emu, a.out, 2100, rec + [(500, "TRIG", 20), (540, "TRIG", 20), (580, "TRIG", 20),
                     (650, "turn", "0:7", 6, 10), (760, "turn", "3:2", 1, 10)] + play + [(1500, "PLAY"), (2000, "STOP")],
                     seq, newcard("r50seed"), extra=at2)
        ps = played(log)
        a1 = [(st_, p_) for _, _, st_, p_ in ps[0]] if len(ps) > 1 else []
        a2 = [(st_, p_) for _, _, st_, p_ in ps[1]] if len(ps) > 1 else []
        n = min(len(a1), len(a2))
        print(f"  r50seed: pass 1 {a1[:8]}, pass 2 {a2[:8]}; {xline(log)[-140:]}")
        check("r50seed: RANDOM, A turned up from FREE -> a fixed seed, D +2 -> TRACK: two PLAYs, the same random notes, not as written",
              n >= 6 and a1[:n] == a2[:n] and re.search(r"seed \d+\+", xline(log)) is not None and "RND TRACK" in xline(log) and
              any(p_ not in (60, 62, 64, 65) for _, p_ in a1), (a1[:8], a2[:8]))
    if "r50rtrg" in runs:
        log, _ = run("r50rtrg", a.emu, a.out, 1500, rec + [(450, "T1", 250), (500, "TRIG", 20), (540, "TRIG", 20),
                     (600, "turn", "1:4", 1, 10)] + play, seq, newcard("r50rtrg"), extra=at)
        ps = played(log)
        st = [x[2] for x in (ps[-1] if ps else [])]
        import itertools
        runs1 = [len(list(g)) for k_, g in itertools.groupby(st) if k_ == 1][1:-1]   # (whole passes: the first / last may be cut)
        print(f"  r50rtrg: steps {st[:20]}, step 1's runs {runs1}")
        check("r50rtrg: T1 held, RTRG, B +4: step 1 plays 4 notes each pass", bool(runs1) and all(r_ == 4 for r_ in runs1),
              runs1)
    if "r50fx" in runs:
        log, _ = run("r50fx", a.emu, a.out, 1500, rec + [(450, "T2", 300), (500, "TRIG", 20), (540, "TRIG", 20), (580, "TRIG", 20),
                     (640, "turn", "0:9", 1, 10), (690, "turn", "1:3", 1, 10)] + play, seq, newcard("r50fx"), extra=at)
        ps = played(log)
        st = [x[2] for x in (ps[-1] if ps else [])]
        print(f"  r50fx: steps {st[:16]}")
        check("r50fx: T2 held, FX, A +9 (JMP), B +3 (4): step 4 after step 2, step 3 never", len(st) >= 6 and 3 not in st and
              all(st[i + 1] == 4 for i in range(len(st) - 1) if st[i] == 2), st[:16])
    if "r50cond" in runs:
        card = newcard("r50cond")
        # (P1's lock made by the console: dtboot keeps one --turn an encoder, so the knob's one turn is the COND's)
        run("r50cond", a.emu, a.out, 1900, rec + [(450, "T1", 500), (500, "SRC", 20), (560, "PUSH_A", 100),
            (820, "turn", "0:5", 1, 10)] + play, seq + ["seq=lock:1:1:1:100"], card)
        t, _ = sqp(card)
        lk = t.get("steps", {}).get(0, {}).get("locks", [])
        print(f"  r50cond: step 1's locks {lk}")
        check("r50cond: T1 + SRC, A pushed 0.5 s (COND), A +5: the file's P1 lock on 1:2 (code 9)",
              any(x[0] == 0 and len(x) > 2 and x[2] == 9 for x in lk), lk)
    if "r50slide" in runs:
        card = newcard("r50slide")
        run("r50slide", a.emu, a.out, 1500, rec + [(450, "T1", 400), (500, "SRC", 20), (560, "turn", "0:5", 1, 10),
            (620, "FUNC", 60), (640, "PUSH_A", 10)] + play, seq, card)
        t, _ = sqp(card)
        s1 = t.get("steps", {}).get(0, {})
        print(f"  r50slide: step 1's locks {s1.get('locks')}, slide {s1.get('slide')}")
        check("r50slide: T1 + SRC, A +5, FUNC + A pushed: the file's P1 lock slides", 0 in (s1.get("slide") or ()), s1)
    if "r50menu" in runs:
        card = newcard("r50menu")
        ev = rec + [(350, "FUNC", 130), (450, "DOWN", 20)] + [(500 + 30 * i, "DOWN", 10) for i in range(4)] + [(650, "RIGHT", 10), (700, "DOWN", 10),
              (750, "YES", 20)]
        log, _ = run("r50menu", a.emu, a.out, 1500, ev + play, seq + ["seq=rndstep:1:2:1"], card,
                     extra=["--at", "3.6:seq=steps:1", "--at", "6.95:seq=x:1"])
        stp = re.findall(r"at: seq=steps:1 -> seq: steps(.*)", log)
        print(f"  r50menu: steps {stp}; {xline(log)[-140:]}")
        check("r50menu: PLAY MODE RIGHT -> REV; PRINT RND: RND off, step 2 a plain note", "DIR REV" in xline(log) and
              "RND OFF" in xline(log), xline(log)[-140:])
        t, _ = sqp(card)
        s2 = t.get("steps", {}).get(1, {})
        check("r50menu: the file's step 2 has no random flag", bool(s2.get("notes")) and not s2.get("rnd"), s2)


def rui1051_runs(a):
    """rui1051 (the hardware report on rui1050: red trig LEDs left after GRID REC on a poly track):
    recleds   BD a seq track (6-OP FM, poly), GRID REC (seq mode: the host's SEQ page open, its colours on the trig
              keys), T1 and T3 placed (the host's steps), RECORD again (GRID REC off: the page closes), not playing: the
              16 trig LEDs stock's for that state (all dark: no stock trigs on BD), none left the host's or GRID REC's"""
    import tempfile
    if "recleds" in a.runs:
        card = tempfile.mkdtemp(prefix="seq-e2e-recleds-")
        os.makedirs(os.path.join(card, "Samples"))
        ev = [(500, "RECORD"), (800, "T1", 10), (850, "T3", 10), (1100, "RECORD")]
        log, leds = run("recleds", a.emu, a.out, 1500, ev, ["seq=lat:192", "seq=track:1:on"], card,
                        extra=["--at", "7.0:seq=steps:1"])
        m = re.findall(r"seq: steps(.*)", log)
        print(f"  recleds: BD's host steps{m[-1:] or ' -'}; trig LEDs {[leds.get(k) for k in range(16)]}")
        check("recleds: T1 / T3 the host's steps", bool(m) and [int(x.split(":")[0]) for x in m[-1].split()] == [1, 3],
              m[-1:])
        check("recleds: after GRID REC off every trig LED stock's (dark)",
              all(leds.get(k) == (8, 8, 8) for k in range(16)),
              {k: leds.get(k) for k in range(16) if leds.get(k) != (8, 8, 8)})


def rui1051b_runs(a):
    """rui1051 (the hardware report: a poly track of 2 pages, GRID REC, stopped - PAGE moved the PAGE LEDs, not the trig
    LEDs nor the step keys):
    pagestop  BD a seq track of 32 steps, GRID REC, stopped: T1 (step 1), PAGE pressed (page 2), T3 (step 19): the
              host's steps 1 and 19, the end's trig LEDs page 2's (T3 lit, T1 dark), page 2's PAGE LED the lit one"""
    import tempfile
    if "pagestop" in a.runs:
        card = tempfile.mkdtemp(prefix="seq-e2e-pagestop-")
        os.makedirs(os.path.join(card, "Samples"))
        ev = [(500, "RECORD"), (800, "T1", 10), (900, "PAGE", 10), (1000, "T3", 10)]
        log, leds = run("pagestop", a.emu, a.out, 1300, ev, ["seq=lat:192", "seq=track:1:on", "seq=len:1:32"], card,
                        extra=["--at", "6.0:seq=steps:1"])
        m = re.findall(r"seq: steps(.*)", log)
        print(f"  pagestop: BD's host steps{m[-1:] or ' -'}; trig LEDs {[leds.get(k) for k in range(4)]}; "
              f"PAGE LEDs {[leds.get(k) for k in (43, 42)]}")
        check("pagestop: T1 then PAGE then T3: steps 1 and 19",
              bool(m) and [int(x.split(":")[0]) for x in m[-1].split()] == [1, 19], m[-1:])
        check("pagestop: the trig LEDs page 2's (T3 lit, T1 dark)",
              leds.get(2) not in (None, (8, 8, 8)) and leds.get(0) == (8, 8, 8), [leds.get(k) for k in range(4)])
        # the same with the SEQ page closed (TRK + TRIG: seq mode, the LEDs the sequencer's own 34)
        card = tempfile.mkdtemp(prefix="seq-e2e-pagestop2-")
        os.makedirs(os.path.join(card, "Samples"))
        ev = [(500, "RECORD"), (700, "TRK", 40), (710, "TRIG"), (850, "T1", 10), (950, "PAGE", 10), (1050, "T3", 10)]
        log, leds = run("pagestopc", a.emu, a.out, 1300, ev, ["seq=lat:192", "seq=track:1:on", "seq=len:1:32"], card,
                        extra=["--at", "6.0:seq=steps:1"])
        m = re.findall(r"seq: steps(.*)", log)
        print(f"  pagestopc: BD's host steps{m[-1:] or ' -'}; trig LEDs {[leds.get(k) for k in range(4)]}; "
              f"PAGE LEDs {[leds.get(k) for k in (43, 42)]}")
        check("pagestopc (page closed): steps 1 and 19",
              bool(m) and [int(x.split(":")[0]) for x in m[-1].split()] == [1, 19], m[-1:])
        check("pagestopc (page closed): the trig LEDs page 2's (T3 lit, T1 dark)",
              leds.get(2) not in (None, (8, 8, 8)) and leds.get(0) == (8, 8, 8), [leds.get(k) for k in range(4)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu", default=os.path.expanduser("~/PycharmProjects/worktrees/rytm-emu-seqp2"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "core", "build", "seq_e2e"))
    ap.add_argument("runs", nargs="*", default=["play", "page", "held", "rec", "p2", "nopin", "ux", "lock", "preview", "makeheld", "fresh",
                                                "stepedit", "trigview", "cond", "padsel", "e0", "top", "solo",
                                                "r50dir", "r50spd", "r50key", "r50seed", "r50rtrg", "r50fx", "r50cond", "r50slide",
                                                "r50menu", "recleds", "pagestop"])
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    import tempfile
    demo = ["seq=lat:192", "seq=demo:1"]
    base = [(600, "PLAY"), (1300, "RECORD")]
    for name in a.runs:
        card = tempfile.mkdtemp(prefix=f"seq-e2e-{name}-")
        os.makedirs(os.path.join(card, "Samples"))
        if name == "play":
            run(name, a.emu, a.out, 1900, base, demo, card)
        elif name == "page":
            run(name, a.emu, a.out, 1900, base + [(1700, "PAGE", 300)], demo, card)
        elif name == "held":
            run(name, a.emu, a.out, 1900, base + [(1700, "T1", 300)], demo, card)
        elif name == "rec":
            # CHRO: the pads play notes on the auto channel (14 in this base), bound to BD's voice 0
            ev = [(150, "CHRO"), (300, "RECORD", 150), (350, "PLAY")]
            for t, pad in ((520, "BD"), (610, "RS"), (655, "BT"), (790, "MT"), (880, "SD"), (1010, "CP"), (1060, "LT"),
                           (1150, "HT")):   # a loose line on the pads
                ev.append((t, pad, 25))
            ev += [(1500, "STOP"), (1650, "RECORD")]
            run(name, a.emu, a.out, 2000, ev, ["seq=lat:192", "seq=track:1:on", "poly=bind:14:0"], card)
    p2_runs(a)
    e0_runs(a)   # seqx
    top_runs(a)   # seqtop
    ux_runs(a)
    dbg_runs(a)
    rui1040_runs(a)
    rui1050_runs(a)
    rui1051_runs(a)
    rui1051b_runs(a)
    if FAILS:
        print(f"FAILED: {FAILS}")
        sys.exit(1)
    print("ALL PASS" if any(n in ("p2", "nopin", "ux", "lock", "preview", "makeheld", "fresh", "stepedit", "trigview", "cond", "padsel", "e0", "top", "solo")
                            or n.startswith("r50") or n in ("recleds", "pagestop") for n in a.runs) else "done")


if __name__ == "__main__":
    main()
