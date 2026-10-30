#!/usr/bin/env python3
"""Golden renders for the engine core extraction (docs/superpowers/plans/2026-10-14-engine-core-extraction.md).

    core_golden.py capture [--port P]       the Teensy's renders of every scenario -> sim/golden/teensy/<name>.npz
    core_golden.py check-teensy [--port P]  the Teensy now against them: audio and SysEx identical (but 1A-1D)
    core_golden.py check-host               the core on the Mac (sim/coreengine.py) against them (Task 13)
    core_golden.py run-host NAME OUT        one scenario on the Mac core into OUT.npz (check-host runs each in a
                                            fresh process: the core is one engine per program)
    core_golden.py sizes LOG BUILD [--record]   a run_sketch.sh log's ITCM/RAM numbers against
                                            sim/golden/baseline.json (BUILD: serial or host)

A scenario is a list of steps: ("sx", bytes) a SysEx report as the Rytm sends it, ("wait", secs) time for the
Teensy's loop() (DSP SAMPLE prepares), ("n", frames) audio of the 8 voices. Each starts from a reboot ('R') on
the Teensy and a fresh process on the Mac, then 10 <project>. The golden files are this checkout's own renders
(not firmware data); git-ignored, captured once before the extraction (Task 1).
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import time

import numpy as np
import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from rmp_install import Link  # noqa: E402

GOLD = os.path.join(HERE, "golden", "teensy")
BASELINE = os.path.join(HERE, "golden", "baseline.json")
HDR = [0xF0, 0x7D, 0x52, 0x54]
PLAITS, JUNO, DAISY, DSPS, FM, U1 = 0x00, 0x08, 0x10, 0x20, 0x48, 0x50
TOLERANCE_DB = 60.0          # check-host: each voice's SNR against the Teensy; measured 2026-10-15: worst mixed
                             # 87.9 dB (plaits 103.0, plaits_page2 94.5, juno 106.8, daisy 91.1), exact: fm,
                             # digidrum, user_empty (DIGIDRUM's scenario and its mixed voice went with it, 2026-10-29:
                             # recapture "mixed")
USER_SLOT_CMDS = (0x1A, 0x1B, 0x1C, 0x1D)   # both checks leave these out: plugin generations (seeded from the
                                            # Teensy's TRNG at every boot) and the owed 1D's timer-driven timing


def keep(ms):
    """The SysEx output without the user-slot messages (USER_SLOT_CMDS)."""
    return [m for m in ms if len(m) < 5 or m[4] not in USER_SLOT_CMDS]


def pbytes(ps):
    """P1..P16 (floats 0..127.99) -> 32 bytes: the value, then its fraction in 1/128 (sim/protocol.py)."""
    ps = list(ps) + [0] * (16 - len(ps))
    out = []
    for p in ps[:16]:
        p = max(0.0, min(127.99, float(p)))
        v = int(p)
        out += [v, int((p - v) * 128) & 0x7F]
    return out


def trig(vb, note, vel, ps):
    return ("sx", bytes(HDR + [0x04, vb, note, vel] + pbytes(ps) + [0xF7]))       # 41 bytes, untagged


def params(vb, ps):
    return ("sx", bytes(HDR + [0x03, vb] + pbytes(ps) + [0xF7]))                   # 39 bytes


def note_off(v):
    return ("sx", bytes(HDR + [0x11, v, 0xF7]))


def project(name):
    return ("sx", bytes(HDR + [0x10] + list(name.encode()) + [0xF7]))


def n(frames):
    return ("n", frames)


def plaits_p(eng, tim=64, mor=64, gld=0):
    #        ENG  HRM  TIM  MOR  DEC LPG TUN LEV  FM FIN AUX TMD MMD VEL  GLD VOL
    return [eng, 64, tim, mor, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, gld, 100]


def juno_p(pat, cut=80, ln=127):
    #       PAT CUT RES ENV ATK DEC REL LEN
    return [pat, cut, 20, 64, 10, 64, 64, ln]


def fm_p(pat, ln=127):
    #       PAT TIM MOR DEC LPG TUN LEV  LEN
    return [pat, 64, 64, 64, 64, 64, 100, ln]


def daisy_p(eng):
    #       ENG STR BRI DMP INP EXC TUN LEV
    return [eng, 64, 64, 64, 0, 64, 64, 100]


def dsps_p(slot):
    #       ENG STR BRI DMP MIX FB TUN LEV   TUN FIN SMP MOD STA END LOP LEV
    return [0, 64, 64, 64, 64, 64, 64, 100, 64, 64, slot, 0, 0, 120, 0, 100]


def scenarios(dsps_file=None):
    """name -> steps. dsps_file: (folder, file) of the card sample DSP SAMPLE plays (Teensy only)."""
    s = {
        "plaits": [project("CORETEST"), trig(PLAITS | 0, 48, 100, plaits_p(0)), n(9600),
                   trig(PLAITS | 0, 55, 100, plaits_p(40)), n(9600), trig(PLAITS | 0, 60, 90, plaits_p(90)), n(9600),
                   trig(PLAITS | 0, 36, 127, plaits_p(124)), n(9600)],
        "plaits_page2": [project("CORETEST"), trig(PLAITS | 0, 48, 100, plaits_p(60, gld=40)), n(4800),
                         trig(PLAITS | 0, 60, 60, plaits_p(60, gld=40)), n(4800),
                         params(PLAITS | 0, plaits_p(60, tim=110, mor=20, gld=40)), n(9600)],
        "juno": [project("CORETEST"), trig(JUNO | 1, 60, 100, juno_p(0)), n(9600), params(JUNO | 1, juno_p(0, cut=30)),
                 n(4800), note_off(1), n(9600), trig(JUNO | 1, 64, 80, juno_p(37)), n(9600)],
        "fm": [project("CORETEST"), trig(FM | 2, 48, 100, fm_p(0)), n(9600), trig(FM | 2, 52, 100, fm_p(17)), n(9600),
               trig(FM | 2, 55, 100, fm_p(95)), n(4800), note_off(2), n(9600)],
        "daisy": [project("CORETEST")] + [x for e in (0, 64, 127) for x in (trig(DAISY | 3, 57, 100, daisy_p(e)), n(9600))],
        "mixed": [project("CORETEST"), trig(PLAITS | 0, 48, 100, plaits_p(124)), trig(JUNO | 1, 60, 100, juno_p(5)),
                  trig(FM | 2, 50, 100, fm_p(3)), trig(DAISY | 3, 57, 100, daisy_p(64)),
                  trig(PLAITS | 5, 62, 100, plaits_p(70)), trig(FM | 6, 45, 100, fm_p(40)),
                  trig(JUNO | 7, 67, 100, juno_p(60)), n(24000)],
        "user_empty": [project("CORETEST"), trig(U1 | 6, 60, 100, plaits_p(64)), n(4800)],
    }
    if dsps_file is not None:
        f, smp = dsps_file
        s["dsps"] = [project("CORESMP"),
                     ("sx", bytes(HDR + [0x0F, (f >> 7) & 0x7F, f & 0x7F, (smp >> 7) & 0x7F, smp & 0x7F, 0xF7])),
                     ("wait", 0.5), trig(DSPS | 5, 60, 100, dsps_p(1)), ("wait", 0.5), n(24000)]
    return s


# ---------------------------------------------------------------- the Teensy
def port_of(a):
    return a.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)


def reboot(port):
    """'R', then a fresh Link once the Teensy answers again (its USB serial port goes away and comes back)."""
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
        s.write(b"R")
        s.flush()
        s.close()
    except (serial.SerialException, OSError):
        pass
    time.sleep(2.0)
    t0 = time.time()
    while time.time() - t0 < 40:
        try:
            link = Link(port, quiet=True)
            link.sync(30)
            return link
        except (serial.SerialException, OSError, TimeoutError):
            time.sleep(0.5)
    raise TimeoutError("the Teensy did not come back after 'R'")


def render(link, frames):
    out = []
    while frames > 0:
        k = min(1024, frames)
        out.append(np.frombuffer(link.frames(k), "<i2").reshape(k, 8))
        frames -= k
    return np.concatenate(out)


def run_teensy(link, steps):
    audio, mark = [], len(link.sysex)
    for kind, arg in steps:
        if kind == "sx":
            link.report(arg)
        elif kind == "wait":
            t = time.time()
            while time.time() - t < arg:
                link._read_frame()
        else:
            audio.append(render(link, arg))
    t = time.time()
    while time.time() - t < 0.3:      # SysEx sent after the last request
        link._read_frame()
    return np.concatenate(audio), [bytes(m) for m in link.sysex[mark:]]


def find_dsps_file(link):
    from rmp_e2e import find_card_file  # its 0D/0E walk; imported late (it needs a Link)
    best = find_card_file(link)
    if best is None:
        sys.exit("no .wav in the first folders of the Teensy's card: the dsps scenario needs one")
    return best[0], best[1]


def home_project(port):
    """The Teensy's project before the scenarios move it (its device-info report; "" without one)."""
    link = reboot(port)
    return (link.info() or {}).get("project", "")


def restore_project(port, link, home, current):
    """Back on the project the Teensy had (rmp_e2e.py, run after check-teensy by gate.sh full, expects it).
    current: the project the scenarios left it on (None: unknown, e.g. after an error; link None: a fresh one).
    Never raises: a failed restore is a loud warning on stderr, not a failed check."""
    if not home or current == home:
        return
    try:
        if link is None:
            link = reboot(port)
        mark = len(link.logs)
        link.report(project(home)[1])
        if not link.wait_log(rf"^project: {re.escape(home)}$", None, 5, since=mark):
            print(f"WARNING: the Teensy did not confirm project {home!r} within 5 s: it may still be on "
                  f"{current or 'a scenario project'} (rmp_e2e.py expects {home!r})", file=sys.stderr, flush=True)
            return
        t = time.time()
        while time.time() - t < 1.5:   # loop() saves it to the card (/.rytm/project) between requests
            link._read_frame()
    except Exception as e:  # noqa: BLE001 - the restore must not hide the scenarios' own result or error
        print(f"WARNING: restoring the Teensy's project {home!r} failed: {e!r}", file=sys.stderr, flush=True)


def run_scenarios(port, home, items, each):
    """each(name, link, steps) per scenario, each on a freshly rebooted Teensy; the home project restored after
    them, also when one raises. The project lives on the card, so it carries over the reboots: the current one
    is the last project step run (home before any)."""
    link, current = None, home
    try:
        for name, steps in items:
            link = reboot(port)
            each(name, link, steps)
            current = next((bytes(a[5:-1]).decode() for k, a in reversed(steps)
                            if k == "sx" and len(a) > 5 and a[4] == 0x10), current)
    except BaseException:
        restore_project(port, None, home, None)   # a fresh link: the old one may be mid-frame
        raise
    restore_project(port, link, home, current)


def save(path, audio, sysex, meta):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    np.savez(path, audio=audio, sysex=np.array([m.hex() for m in sysex]), meta=json.dumps(meta))


def load(path):
    z = np.load(path)
    return z["audio"], [bytes.fromhex(h) for h in z["sysex"]], json.loads(str(z["meta"]))


def cmd_capture(a):
    port = port_of(a)
    home = home_project(port)
    link = reboot(port)
    dsps = find_dsps_file(link)

    def each(name, link, steps):
        audio, sx = run_teensy(link, steps)
        save(os.path.join(GOLD, name + ".npz"), audio, sx, {"dsps_file": list(dsps)})
        print(f"captured {name}: {audio.shape[0]} frames, peak {int(np.abs(audio).max())}, {len(sx)} SysEx", flush=True)
    run_scenarios(port, home, scenarios(dsps).items(), each)


def cmd_check_teensy(a):
    port = port_of(a)
    first = load(os.path.join(GOLD, "plaits.npz"))[2]
    home = home_project(port)
    bad = []

    def each(name, link, steps):
        gold_audio, gold_sx, _ = load(os.path.join(GOLD, name + ".npz"))
        audio, sx = run_teensy(link, steps)
        same_a = audio.shape == gold_audio.shape and np.array_equal(audio, gold_audio)
        same_s = keep(sx) == keep(gold_sx)
        if not same_a and audio.shape == gold_audio.shape:
            cols = [v for v in range(8) if not np.array_equal(audio[:, v], gold_audio[:, v])]
            detail = f"voices {cols} differ, max |diff| {int(np.abs(audio.astype(int) - gold_audio).max())}"
        else:
            detail = "" if same_a else f"shape {audio.shape} vs {gold_audio.shape}"
        print(f"{'PASS' if same_a and same_s else 'FAIL'}: {name} audio {'identical' if same_a else detail}, "
              f"SysEx {'identical' if same_s else f'{len(keep(sx))} vs {len(keep(gold_sx))} messages'} (0x1A-0x1D left out)", flush=True)
        if not (same_a and same_s):
            bad.append(name)
    run_scenarios(port, home, scenarios(tuple(first["dsps_file"])).items(), each)
    print("CHECK-TEENSY PASS" if not bad else "CHECK-TEENSY FAIL: " + ", ".join(bad))
    sys.exit(1 if bad else 0)


# ---------------------------------------------------------------- the Mac core (Task 13)
def run_host(name, out):
    from coreengine import CoreEngine  # noqa: E402  (needs core/build/libcore.dylib)
    card = tempfile.mkdtemp(prefix="coretest-card-")
    os.makedirs(os.path.join(card, "Samples"))
    eng = CoreEngine(card)
    eng.frames(32)   # as reboot()'s Link.sync: 3 blocks rendered, 4 frames left in the ring for the scenario
    audio = []
    for kind, arg in scenarios()[name]:
        if kind == "sx":
            eng.report(arg)
        elif kind == "wait":
            eng.idle(arg)
        else:
            audio.append(eng.frames(arg))
    eng.idle(0.0)
    save(out, np.concatenate(audio), eng.take_sysex(), {})


def snr_db(ref, got):
    ref, got = ref.astype(np.float64), got.astype(np.float64)
    e = np.sqrt(np.mean((ref - got) ** 2))
    r = np.sqrt(np.mean(ref ** 2))
    return float("inf") if e == 0 else (float("-inf") if r == 0 else 20 * np.log10(r / e))


def cmd_check_host(a):
    bad, table = [], []
    for name in scenarios():
        gold_audio, gold_sx, _ = load(os.path.join(GOLD, name + ".npz"))
        out = os.path.join(tempfile.mkdtemp(), name + ".npz")
        subprocess.run([sys.executable, os.path.abspath(__file__), "run-host", name, out], check=True)
        audio, sx, _ = load(out)
        ok = audio.shape == gold_audio.shape
        worst = float("inf")
        for v in range(8):
            if not ok:
                break
            if not gold_audio[:, v].any() and not audio[:, v].any():
                continue
            d = snr_db(gold_audio[:, v], audio[:, v])
            worst = min(worst, d)
            ok = ok and d >= TOLERANCE_DB
        ok_s = keep(sx) == keep(gold_sx)
        exact = audio.shape == gold_audio.shape and np.array_equal(audio, gold_audio)
        table.append((name, "exact" if exact else f"{worst:.1f} dB", ok_s))
        print(f"{'PASS' if ok and ok_s else 'FAIL'}: {name} audio {'exact' if exact else f'worst voice {worst:.1f} dB'}"
              f" (tolerance {TOLERANCE_DB} dB), SysEx {'same' if ok_s else 'DIFFERENT'}", flush=True)
        if not (ok and ok_s):
            bad.append(name)
    print("(dsps not checked here: its sample lives on the Teensy's card; check-teensy covers it)")
    print("CHECK-HOST PASS" if not bad else "CHECK-HOST FAIL: " + ", ".join(bad))
    sys.exit(1 if bad else 0)


# ---------------------------------------------------------------- sizes
SIZE_RES = {
    "itcm_code": r"ITCM (\d+) B in \d+ blocks",
    "itcm_blocks": r"ITCM \d+ B in (\d+) blocks",
    "locals": r"locals (\d+) B",
    "ram2_free": r"RAM2: variables:\d+\s+free for malloc/new:(\d+)",
    "flash_code": r"FLASH: code:(\d+)",
}


def cmd_sizes(a):
    text = open(a.log, errors="replace").read()
    got = {}
    for k, rx in SIZE_RES.items():
        m = re.findall(rx, text)
        if not m:
            sys.exit(f"{a.log}: no {k} ({rx})")
        got[k] = int(m[-1])
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    if a.record:
        base[a.build] = got
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        json.dump(base, open(BASELINE, "w"), indent=1)
        print(f"SIZES recorded for {a.build}: {got}")
        return
    b = base[a.build]
    fails = []
    if got["itcm_blocks"] != b["itcm_blocks"]:
        fails.append(f"ITCM blocks {got['itcm_blocks']} (baseline {b['itcm_blocks']})")
    if got["locals"] < b["locals"] - 2048:
        fails.append(f"RAM1 locals {got['locals']} (baseline {b['locals']})")
    if got["ram2_free"] < b["ram2_free"] - 2048:
        fails.append(f"RAM2 free {got['ram2_free']} (baseline {b['ram2_free']})")
    print("  ".join(f"{k} {got[k]} ({got[k] - b[k]:+d})" for k in got))
    print("SIZES PASS" if not fails else "SIZES FAIL: " + "; ".join(fails))
    sys.exit(1 if fails else 0)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("capture", "check-teensy"):
        sub.add_parser(c).add_argument("--port")
    sub.add_parser("check-host")
    r = sub.add_parser("run-host")
    r.add_argument("name")
    r.add_argument("out")
    z = sub.add_parser("sizes")
    z.add_argument("log")
    z.add_argument("build", choices=("serial", "host"))
    z.add_argument("--record", action="store_true")
    a = ap.parse_args()
    if a.cmd == "capture":
        cmd_capture(a)
    elif a.cmd == "check-teensy":
        cmd_check_teensy(a)
    elif a.cmd == "check-host":
        cmd_check_host(a)
    elif a.cmd == "run-host":
        run_host(a.name, a.out)
    else:
        cmd_sizes(a)


if __name__ == "__main__":
    main()
