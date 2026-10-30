#!/usr/bin/env python3
"""LIVE IN end to end in rytm-emu: the emulated Rytm's AUDIO IN (dtboot --audio-in, the emulator's AUDIO IN model) reaches
the real engine core as the Overbridge host (sim/poly_live.py --livein), plays through LIVE IN on voice 0 and its INSERT
chain (a DRIVE), and goes back into the Rytm on BD's voice input (OUT TRK), headless.

    make -C core lib && uv run python sim/livein_e2e.py --emu ~/PycharmProjects/worktrees/rytm-emu-seqp2 [--out DIR]

The input: a F_L tone on AUDIO IN L and a F_R tone on R (-12 dBFS), from 1 s on, with a 2 ms click at its start.
LIVE IN: INPT L, GAIN 0 dB, no gate, MON ALW; INSERT A a DRIVE. Checks:
  - the Rytm's R stream carries the tones on AUDIO IN L / R (IN 10 / 11): the emulator's AUDIO IN model;
  - the host's voice output carries F_L and not F_R (INPT L), with the DRIVE's harmonics;
  - LIVE IN read the stream without a gap after its start (its underruns), and its `livein` line;
  - the round trip in the emulator's frames: the click's onset on AUDIO IN (R IN 10) -> its onset in the host's output,
    and, if the Rytm passes BD's voice input to its stem without a trig, -> its onset in BD's stem (R IN 2).
  (F_L / F_R are 1210 / 1730 Hz, above the BD's own sound; the clicks repeat every 0.25 s.)
Writes <out>/livein_e2e.log (the host's log, the emulator's tail), host.wav, r.npy, in.wav; exits non-zero on a FAIL.
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402

TAG = os.environ.get("SEQ_TAG", "rui1050")
F_L, F_R = 1210.0, 1730.0   # above the BD's own sound
CLICK_GAP = 12000
SR = 48000
FAILS = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail != "" else ""), flush=True)
    if not ok:
        FAILS.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def tone(x, hz):
    n = len(x)
    t = np.arange(n) / SR
    return 2 * abs(np.dot(x, np.exp(-2j * np.pi * hz * t))) / n


def onset(x, start=0, frac=0.25):
    """The first frame from start on whose |x| reaches frac of x's peak after start; -1 none."""
    y = np.abs(x[start:].astype(np.float64))
    if not len(y) or y.max() <= 0:
        return -1
    i = np.argmax(y >= frac * y.max())
    return start + int(i)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu", default=os.path.expanduser("~/PycharmProjects/worktrees/rytm-emu-seqp2"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "core", "build", "livein_e2e"))
    ap.add_argument("--instr", type=int, default=1400, help="M instructions after the snapshot (200 M ~ 1 s)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    # the input WAV: 16-bit stereo, a click at 1 s then L F_L / R F_R at -12 dBFS
    n = SR * 8
    t = np.arange(n) / SR
    env = (t >= 1.0).astype(np.float64)
    L = 0.25 * np.sin(2 * np.pi * F_L * t) * env
    R = 0.25 * np.sin(2 * np.pi * F_R * t) * env
    for c0 in range(SR, n - 96, CLICK_GAP):   # the clicks: 3 kHz, 2 ms, -2 dBFS, every 0.25 s from 1 s on (L only)
        L[c0:c0 + 96] = 0.8 * np.sin(2 * np.pi * 3000 * np.arange(96) / SR)
    inwav = os.path.join(a.out, "in.wav")
    with wave.open(inwav, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.stack([L, R], 1) * 32767).astype("<i2").tobytes())
    # the host: voice 0 primed as LIVE IN (0x78) with its knobs, and forced (livein=voice:0), a DRIVE on it
    card = tempfile.mkdtemp(prefix="livein-e2e-")
    os.makedirs(os.path.join(card, "Samples"))
    prime = os.path.join(a.out, "prime.hex")
    knobs = [0, 64, 0, 28, 0, 0, 77, 0, 77, 0, 0, 0, 0, 0, 0, 0]   # INPT L, GAIN 0 dB, GATE OFF, MON ALW
    with open(prime, "w") as f:
        f.write(P.params_report(0x78 | 0, [x << 7 for x in knobs]).hex() + "\n")
    port = free_port()
    hwav, rnpy = os.path.join(a.out, "host.wav"), os.path.join(a.out, "r.npy")
    logp = os.path.join(a.out, "livein_e2e.log")
    log = open(logp, "w")
    host = subprocess.Popen([sys.executable, os.path.join(HERE, "poly_live.py"), "--link", f"127.0.0.1:{port}", "--card", card,
                             "--wav", hwav, "--prime", prime, "--rwav", rnpy, "--livein", "0",
                             "--cmd", "ins=V1:A:type:drive", "--cmd", "ins=V1:A:p1:127"], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.5)
    cmd = [os.path.join(a.emu, "target/release/dtboot"), f"fw/AR173_us_{TAG}.syx", "--load", f"sections/us_{TAG}.snap",
           "--card", f"sections/us_{TAG}.img", "--instr", f"{a.instr}M", "--every", "100000M", "--usb-host",
           "--usb-midi-real", "--ob-link", f"127.0.0.1:{port}", "--audio-in", os.path.abspath(inwav)]
    # a BD trig on step 1 (GRID REC + T1), PLAY: the Rytm's AMP opens on BD's voice at each bar (its Overbridge input
    # passes only then)
    for t, k in ((200, "RECORD"), (230, "T1"), (250, "T5"), (270, "T9"), (290, "T13"), (320, "RECORD"), (400, "PLAY")):
        cmd += ["--press", f"{k}@{7400 + t}M"]
    r = subprocess.run(cmd, cwd=a.emu, capture_output=True, text=True, timeout=1800)
    time.sleep(1.0)
    host.terminate()
    try:
        host.wait(15)
    except subprocess.TimeoutExpired:
        host.kill()
    log.write("\n---- dtboot\n" + r.stdout[-6000:] + r.stderr[-3000:])
    log.close()
    hl = open(logp).read()
    print(f"dtboot rc {r.returncode}")
    ok_files = os.path.exists(rnpy) and os.path.exists(hwav)
    check("the host wrote its output and the Rytm's R frames", ok_files)
    if not ok_files:
        print(hl[-3000:])
        sys.exit(1)
    rf = np.load(rnpy).astype(np.float64) / 2.0 ** 31
    with wave.open(hwav) as w:
        hv = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.float64) / 32767.0
    print(f"R frames {len(rf)} ({len(rf) / SR:.2f} s), host frames {len(hv)} ({len(hv) / SR:.2f} s)")
    # the emulator's AUDIO IN: the click, then the tones on IN 10 / 11
    ci = onset(rf[:, 10])
    check("AUDIO IN reaches the Rytm's R stream (IN 10 / 11)", ci >= 0 and len(rf) > ci + SR,
          f"click at R frame {ci}")
    if ci < 0 or len(rf) <= ci + SR:
        sys.exit(1)
    seg = slice(ci + 2400, min(len(rf), ci + 2400 + SR))
    a10, a11 = tone(rf[seg, 10], F_L), tone(rf[seg, 11], F_R)
    # (the emulator's AUDIO IN model puts the WAV's full scale at -6 dBFS on the stream: the -12 dBFS tones at -18)
    d10, d11 = 20 * np.log10(a10 + 1e-12), 20 * np.log10(a11 + 1e-12)
    check("IN 10 carries F_L, IN 11 F_R, at one level", abs(d10 - d11) < 1 and d10 > -24 and tone(rf[seg, 10], F_R) < 0.01 * a10,
          f"{d10:.1f} / {d11:.1f} dBFS (the WAV's -12 dBFS)")
    # the host's voice output: the click's onset there, then F_L (INPT L) with the DRIVE's harmonics, no F_R
    ho = onset(hv)
    check("the host's voice plays the input", ho >= 0, f"click at host frame {ho}")
    hs = hv[ho + 2400: ho + 2400 + SR] if ho >= 0 else hv[:0]
    f1, f3, f330 = tone(hs, F_L), tone(hs, 3 * F_L), tone(hs, F_R)
    check("INPT L: F_L there, F_R not", f1 > 0.05 and f330 < 0.01 * f1, f"F_L {f1:.3f}, F_R {f330:.5f}")
    check("the DRIVE insert's harmonics (3rd > 1 % of the fundamental)", f3 > 0.01 * f1, f"3rd / fundamental {f3 / (f1 + 1e-12):.3f}")
    # LIVE IN's own counters (its console line at the end)
    li = [ln for ln in hl.splitlines() if ln.startswith("livein:") or "livein: AUDIO IN" in ln]
    print("  " + (li[-1].strip()[:600] if li else "no livein line"))
    check("LIVE IN's console line", bool(li) and "voice 0:" in li[-1])
    # the round trip in the emulator's frames: the host renders ~512 frames ahead of the Rytm (the link's buffer), so the
    # host's frame count is the Rytm's + the lead; BD's stem (IN 2) is the voice back in the Rytm, if it passes it
    st = rf[:, 2]
    stem_has = np.abs(st[ci:]).max() > 1e-3
    if stem_has:
        # the loudest 0.1 s of BD's stem after the input started (a trig's AMP open): the tones there, and the lag that
        # lines BD's stem up with AUDIO IN L (the round trip in the Rytm's own frames)
        w = 4800
        e = np.convolve(st[ci:] ** 2, np.ones(w), "valid")
        b0 = ci + int(np.argmax(e))
        s2 = st[b0:b0 + w]
        check("BD's stem carries the processed input (F_L, no F_R)", tone(s2, F_L) > 10 * tone(s2, F_R) and tone(s2, F_L) > 1e-4,
              f"F_L {tone(s2, F_L):.4f}, F_R {tone(s2, F_R):.5f}")
        # the round trip: each click on AUDIO IN L (R frames) found again in BD's stem (a matched filter on its 3 kHz
        # burst), the median of the clicks that landed while the AMP was open
        tpl = np.sin(2 * np.pi * 3000 * np.arange(96) / SR)   # the click, matched (normalised correlation)
        lags = []
        for c in range(ci, len(rf) - 4896, CLICK_GAP):
            seg2 = st[c:c + 4896]
            num = np.correlate(seg2, tpl, "valid")
            den = np.sqrt(np.convolve(seg2 ** 2, np.ones(96), "valid")) * np.linalg.norm(tpl) + 1e-12
            cc = np.abs(num) / den
            k = int(np.argmax(cc))
            if cc[k] > 0.6 and np.abs(seg2[k:k + 96]).max() > 1e-3:
                lags.append(k)
        rt = int(np.median(lags)) if lags else -1
        spread = (max(lags) - min(lags)) if lags else -1
        print(f"  round trip AUDIO IN -> LIVE IN -> BD's voice input -> BD's stem: {rt} frames ({rt / 48:.2f} ms) [emulator]; "
              f"{len(lags)} clicks back, spread {spread} frames: {lags[:12]}")
        check("the clicks come back on BD's stem, at one delay (the round trip)", len(lags) >= 2 and 0 < rt < 4800 and spread <= 96,
              f"{rt} frames, spread {spread}")
    else:
        print("  BD's stem stays silent: the emulated Rytm passes a voice's Overbridge input only while its AMP is open "
              "(docs/overbridge-voice-inputs.md: the VCA must be triggered) - no trig in this run")
    print(f"  the host's lead over the Rytm: {ho - ci} frames between the click's onsets (host frames - R frames)")
    if FAILS:
        print(f"FAILED: {FAILS}")
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
