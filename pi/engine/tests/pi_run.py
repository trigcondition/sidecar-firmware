#!/usr/bin/env python3
"""The Pi engine on the real Pi 4 over the serial cable: with a stock Rytm MKII (OVERBRIDGE mode) as its clock, or
with no Rytm through the self-test transport (--selftest: the generic timer paces the output, synthetic MIDI notes
48 on channel 1 go through the real MIDI parser and note adapter; selftest.h).

    pi_run.py [--flash] [--port P] [--selftest] engines [--machines M,...]
        each machine on voice 0 (the Rytm: the engine's drone, note 48; --selftest: the synthetic notes): the voice
        set, its trig lines, the stats, then a 10 s 'w' capture's OUT MAIN L checked for level and (except MD)
        pitch. DSP SAMPLE plays the card's self-test sample (bridge::selftest_content: pool slot 1, 440 Hz at its
        root, so note 48 sounds 220 Hz).
    pi_run.py [--flash] [--port P] engines --rytm-notes V,... [--note N] [--machines M,...]
        the same on the stock Rytm's own MIDI notes (its sequencer playing; MIDI channel V+1 plays voice V): each
        machine on every listed voice, no drone; the pitches expected are note N's (the Rytm's track note, default
        36), the trig lines must show note N.
    pi_run.py [--flash] [--port P] [--selftest] gaps SECS [--fifo N]
        all 8 voices droning (every machine): after a 30 s warm-up the output underflows and the muted voices must
        stay at 0; with the Rytm also the Overbridge gaps/lost, with --selftest the pacer's OUT rate (no gap counter:
        nothing streams over USB). A progress line every 10 s, with core 3's longest pass. --machines M,... sets
        the voices' machines (repeated to 8). --fifo N first sets the FIFO target (:fifo=N; the
        Task 7 sweep).
    pi_run.py [--flash] bench [LOG]
        the boot bench (Task 7) from LOG (default: the newest pi/circle/out/engine-*.log): every machine on cores
        0-3 and the per-machine summaries; each machine's worst core must fit four voices a core (at most 20 % of a
        core per voice) and its worst block must take less than a whole block (a NOTE when over the guard's budget:
        it mutes only after three in a row). Prints a Markdown table for docs/pi-engine.md.
    pi_run.py analyse WAV [--want HZ]
        the level and pitch checks on a capture already on disk (no Pi)

This script sends nothing to the Rytm (the engine itself answers its host-link SysEx: docs/pi-engine.md "Safety"); the Pi gets ':' console commands (docs/pi-engine.md). Captures go to pi/circle/out/
(git-ignored). Nothing else may hold the port (stop flash.sh's monitor first). Each capture's base64 dump takes 30 s
to 2 minutes at 921600 baud. --flash first builds and uploads pi/engine
(CIRCLE_TREE=circle-engine pi/circle/flash.sh ../engine 25). Exit status 0 when every check passed.

    uv run python pi/engine/tests/pi_run.py --selftest engines
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time
import wave

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
CIRCLE = os.path.join(ROOT, "pi", "circle")
NOTE48 = 130.81
# Each machine's pitch on note 48, within 3 %: PLAITS VA plays the note; JUNO factory patch 0 sounds its 16' stop
# and 6-OP FM ROM voice 0 (SOLID BASS) has its carrier at a 0.5 ratio, so both sound an octave below it; DSP
# SAMPLE plays the pool's 440 Hz sine an octave down (note 48 against its root 60).
NOTE48_16FT = NOTE48 / 2   # 65.41 Hz
EXPECT = {"plaits": NOTE48, "juno": NOTE48_16FT, "fm": NOTE48_16FT, "daisy": None, "dsps": 220.0, "md": None,
          "grain": None}
# grain (core/notes.cpp: DSP SAMPLE in GRAIN from the sample RAM, its eight grains always busy): retriggered twice a
# second by the notes, so every trig stops sounding grains from the control core. It must still sound in the
# capture's last seconds (a grain count given back to the wrong core held the render core at the budget: silent).
GRAIN_TAIL_S = 3
# DAISYSP MODAL is inharmonic: no fundamental at the note. Note 48's modes on the core (the Mac's test_content and
# the Pi's capture alike): 78, 210, 392, 626 (the strongest), 906 Hz. Its check: the strongest mode within 3 %.
MODE = {"daisy": 626.0}
# core/voices.cpp's trig log line per machine on voice 0 (MD's has no note)
TRIG = {"plaits": r"trig voice V note N engine \d+", "juno": r"trig voice V JUNO .* note N\b",
        "fm": r"trig voice V 6-OP FM entry \d+ note N\b", "daisy": r"trig voice V note N DAISYSP model \d+",
        "dsps": r"trig voice V note N DSP SAMPLE model \d+", "md": r"trig voice V MD \S+",
        "grain": r"trig voice V note N DSP SAMPLE model \d+ smp 1 mix 0 sta 60 end 127"}


def trig_rx(m, voice=0, note=48):
    return TRIG[m].replace("voice V", f"voice {voice}").replace("note N", f"note {note}")


def at_note(hz, note):
    """A pitch measured at note 48 moved to note (every machine here tracks the keyboard)."""
    return hz * 2 ** ((note - 48) / 12) if hz else hz
GAPS_MACHINES = ["plaits", "juno", "fm", "daisy", "dsps", "md", "plaits", "juno"]
ENGINE = r"engine: fifo min (\d+) max (\d+) target (\d+) under (\d+)(?: \(restart \d+, streams \d+\))? \| blocks (\d+) max \d+ us \| cpu c1 (\S+) c2 (\S+) c3 (\S+) %"
PASS = r"engine: fifo min (\d+) .*\| pass max (\d+) us"
SELFTEST = r"selftest: OUT (\d+)/s notes (\d+) slips (\d+)"
RYTMOB = r"IN (\d+)/s \d+ gap (\d+) lost (\d+) .*\| OUT (\d+)/s"
BENCH_CORE = re.compile(r"bench c(\d) (\w+): (\d+\.\d+) % of a core per voice, worst block (\d+\.\d+) % -> (\d+) voices a core")
BENCH = re.compile(r"bench (\w+): (\d+\.\d+) % of a core per voice at (\d+) MHz -> (\d+) voices a core "
                   r"\(worst of cores 0-3, worst block (\d+\.\d+) %, budget (\d+) %\)")
BENCH_MACHINES = ["plaits", "juno", "fm", "daisy", "dsps", "wt", "gr", "livein", "chop", "md"]
FAULT = re.compile(r"\b(exception|abort|panic|assertion failed|out of memory)\b", re.I)
fails = []
seen = []   # every log line read, for the final fault check


def check(what, ok, detail="", why=""):
    """detail (a measurement) is printed either way; why (the failure's explanation) only on a FAIL."""
    text = detail if ok or not why else (f"{detail}; {why}" if detail else why)
    print(f"{'PASS' if ok else 'FAIL'}: {what}{(' - ' + text) if text else ''}", flush=True)
    if not ok:
        fails.append(what)


# ---- the analysis (no Pi: tests/test_pi_run.py)

def matches(lines, rx):
    return [m for m in (re.search(rx, line) for line in lines) if m]


def last(lines, rx):
    m = matches(lines, rx)
    return m[-1] if m else None


def read_wav(path, channel=2):
    """One channel of a capture.py WAV (int32) as floats in [-1, 1); channel 2 is OUT MAIN L."""
    import numpy as np
    with wave.open(path, "rb") as w:
        ch, width, raw = w.getnchannels(), w.getsampwidth(), w.readframes(w.getnframes())
    dt = {2: "<i2", 4: "<i4"}[width]
    x = np.frombuffer(raw, dt).reshape(-1, ch)[:, min(channel, ch - 1)]
    return x.astype(np.float64) / float(1 << (8 * width - 1))


def peak_db(x):
    import numpy as np
    return float(20 * np.log10(max(float(np.abs(x).max()) if len(x) else 0.0, 1e-9)))


def pitch(x):
    """The median fundamental over 8192-sample windows that sound (autocorrelation, 25..1000 Hz: the Rytm's
    note 36 puts JUNO and 6-OP FM at 32.7 Hz)."""
    import numpy as np
    est = []
    for i in range(0, len(x) - 8192, 4096):
        w = x[i:i + 8192] - np.mean(x[i:i + 8192])   # a DC offset would put its peak at the longest lag
        w = w * np.hanning(8192)
        if np.sqrt(np.mean(w ** 2)) < 1e-4:
            continue
        r = np.fft.irfft(np.abs(np.fft.rfft(w, 16384)) ** 2)[:8192]
        lo, hi = 48000 // 1000, 48000 // 25
        # past the zero lag's own lobe: a low note (32.7 Hz) keeps r high for over a millisecond, so the search
        # starts at r's first negative value (or its first minimum)
        neg = np.nonzero(r[lo:hi] < 0)[0]
        rise = np.nonzero(np.diff(r[lo:hi]) > 0)[0]
        start = lo + (int(neg[0]) if len(neg) else int(rise[0]) if len(rise) else 0)
        lag = start + int(np.argmax(r[start:hi]))
        est.append(48000.0 / lag)
    return float(np.median(est)) if est else 0.0


def strongest(x):
    """The strongest spectral peak in 40..3000 Hz (Hz), over the whole capture."""
    import numpy as np
    y = x - np.mean(x)
    s = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    f = np.fft.rfftfreq(len(y), 1 / 48000)
    band = (f >= 40) & (f <= 3000)
    return float(f[band][int(np.argmax(s[band]))]) if len(y) else 0.0


def pitch_ok(f, want):
    """Within 3 % of want (EXPECT pins each machine's own pitch: no octave tolerance)."""
    return f > 0 and abs(f - want) <= 0.03 * want


def check_audio(name, x, want, path="", mode=None):
    """The level (the -12 dBFS limiter's ceiling, above -45 dBFS) and, with want, the pitch; with mode (an
    inharmonic machine), the strongest spectral peak within 3 % of it."""
    peak = peak_db(x)
    check(f"{name}: level", -45.0 < peak <= -11.9, f"peak {peak:.1f} dBFS ({os.path.basename(path)})")
    if want:
        f = pitch(x)
        check(f"{name}: pitch", pitch_ok(f, want), f"{f:.1f} Hz, want {want:.1f} Hz")
    if mode:
        f = strongest(x)
        check(f"{name}: strongest mode", abs(f - mode) <= 0.03 * mode, f"{f:.1f} Hz, the core's {mode:.0f} Hz")


def check_stats(lines, m, selftest):
    """A few seconds' steady state: engine blocks/s, the transport's OUT blocks/s, underflow growth."""
    eng = matches(lines, ENGINE)
    out = matches(lines, SELFTEST if selftest else RYTMOB)
    if len(eng) < 2 or len(out) < 2:
        check(f"{m}: stats lines", False, f"{len(eng)} engine / {len(out)} transport lines")
        return
    blocks = int(eng[-1].group(5))
    check(f"{m}: engine renders 4000 blocks/s", 3900 <= blocks <= 4100, f"blocks {blocks}")
    rate = int(out[-1].group(1) if selftest else out[-1].group(4))
    check(f"{m}: OUT 6857 blocks/s", 6700 <= rate <= 7000, f"OUT {rate}/s")
    if selftest:
        notes = int(out[-1].group(2))
        check(f"{m}: 2 synthetic notes a second", 1 <= notes <= 3, f"notes {notes}")
    u0, u1 = int(eng[-2].group(4)), int(eng[-1].group(4))
    check(f"{m}: no output underflow", u1 == u0, f"under {u0}->{u1}, fifo min {eng[-1].group(1)}")


def counters(lines, selftest):
    """The last stats: under, cpu c1/c2/c3, and the transport's (gap, lost) or (OUT rate, slips); None: missing."""
    e = last(lines, ENGINE)
    t = last(lines, SELFTEST if selftest else RYTMOB)
    if not e or not t:
        return None
    c = {"under": int(e.group(4)), "cpu": (e.group(6), e.group(7), e.group(8))}
    if selftest:
        c.update(out=int(t.group(1)), slips=int(t.group(3)))
    else:
        c.update(gap=int(t.group(2)), lost=int(t.group(3)), out=int(t.group(4)))
    return c


def progress(c, secs):
    tail = f"OUT {c['out']}/s slips {c['slips']}" if "slips" in c else f"gap {c['gap']} lost {c['lost']}"
    return f"  {secs:4d} s: {tail} under {c['under']} cpu {'/'.join(c['cpu'])} %{c.get('pass', '')}"


def pass_stats(lines):
    """Core 3's longest pass (us) and the FIFO's lowest level (frames) over these stats lines; None: none."""
    m = matches(lines, PASS)
    return (max(int(x.group(2)) for x in m), min(int(x.group(1)) for x in m)) if m else None


def check_gaps(c0, c1, outs, lines):
    """c0 after the warm-up, c1 at the end, outs every progress line's OUT rate."""
    if not c0 or not c1:
        check("stats lines", False, "no engine/transport stats line")
        return
    if "gap" in c0:
        check("no Overbridge gaps", (c1["gap"], c1["lost"]) == (c0["gap"], c0["lost"]),
              f"gap {c0['gap']}->{c1['gap']} lost {c0['lost']}->{c1['lost']}")
    else:
        bad = [o for o in outs if not 6700 <= o <= 7000]
        check("the self-test pulls 6857 blocks/s throughout", not bad, f"OUT {min(outs)}..{max(outs)}/s" if outs else "")
    check("no output underflows", c1["under"] == c0["under"], f"under {c0['under']}->{c1['under']}")
    check("no muted voice", not any("engine: muted" in line for line in lines))


def parse_bench(lines):
    """The boot bench's lines: {machine: {core: (pct, worst_block_pct, voices)}}, {machine: summary dict}."""
    cores, summary = {}, {}
    for line in lines:
        m = BENCH_CORE.search(line)
        if m:
            cores.setdefault(m.group(2), {})[int(m.group(1))] = (float(m.group(3)), float(m.group(4)), int(m.group(5)))
            continue
        m = BENCH.search(line)
        if m:
            summary[m.group(1)] = {"pct": float(m.group(2)), "mhz": int(m.group(3)), "voices": int(m.group(4)),
                                   "block": float(m.group(5)), "budget": int(m.group(6))}
    return cores, summary


def bench_table(cores, summary):
    """Markdown for docs/pi-engine.md's Measurements: % of a core per voice on each core, the worst block, voices."""
    rows = ["| machine | c0 | c1 | c2 | c3 | worst block | voices a core (80 %) |", "|---|---|---|---|---|---|---|"]
    for mach in BENCH_MACHINES:
        c, s = cores.get(mach, {}), summary.get(mach)
        cells = [f"{c[k][0]:.2f} %" if k in c else "-" for k in range(4)]
        tail = [f"{s['block']:.1f} % (budget {s['budget']} %)", str(s["voices"])] if s else ["-", "-"]
        rows.append("| " + " | ".join([mach] + cells + tail) + " |")
    return "\n".join(rows)


def check_bench(lines):
    cores, summary = parse_bench(lines)
    for mach in BENCH_MACHINES:
        c, s = cores.get(mach, {}), summary.get(mach)
        check(f"bench {mach}: cores 0-3 and a summary", sorted(c) == [0, 1, 2, 3] and s is not None,
              f"cores {sorted(c)}, summary {'yes' if s else 'no'}")
        if not s:
            continue
        check(f"bench {mach}: four voices fit a core", s["pct"] <= 20.0, f"{s['pct']:.2f} % of a core per voice")
        check(f"bench {mach}: worst block within one block", s["block"] < 100.0, f"{s['block']:.1f} % of a block")
        if s["block"] >= s["budget"]:   # one block over is harmless: the guard mutes after three in a row
            print(f"NOTE: bench {mach}: a block took {s['block']:.1f} % (budget {s['budget']} %, three in a row mute)")
    return cores, summary


def faults(lines):
    return [line for line in lines if FAULT.search(line) or "\x1b[1m" in line or "\x1b[91m" in line]


# ---- the Pi

def port_of(a):
    return a.port or next(iter(sorted(glob.glob("/dev/cu.usbserial-*"))), None)


def flash():
    env = dict(os.environ, CIRCLE_TREE="circle-engine")
    try:
        subprocess.run([os.path.join(CIRCLE, "flash.sh"), "../engine", "25"], env=env, check=True,
                       stdout=subprocess.DEVNULL, timeout=900)
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
        sys.exit(f"FAIL: flash - {e}")


class Pi:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port, 921600, timeout=0.1)
        self.lines = []

    def cmd(self, text):
        self.s.write(b":" + text.encode() + b"\n")

    def pump(self, secs, show=()):
        t = time.time()
        while time.time() - t < secs:
            line = self.s.readline().decode(errors="replace").strip()
            if line:
                self.lines.append(line)
                seen.append(line)
                if any(k in line for k in show):
                    print("  pi:", line, flush=True)

    def close(self):
        self.s.close()


def session(port, *cmds, secs=1.5, show=()):
    pi = Pi(port)
    for c in cmds:
        pi.cmd(c)
    pi.pump(secs, show=show)
    pi.close()
    return pi.lines


def capture(port, name):
    out = os.path.join(CIRCLE, "out", f"engine-{name}-{time.strftime('%Y%m%d-%H%M%S')}.wav")
    try:
        r = subprocess.run(["uv", "run", "--with", "pyserial", "python", os.path.join(CIRCLE, "capture.py"), "-p",
                            port, "-o", out], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        print("  capture.py: no dump within 300 s")
        return None, out
    if r.returncode != 0 or not os.path.exists(out):
        print(r.stdout[-2000:])
        return None, out
    return read_wav(out), out


def selftest_on(port):
    lines = session(port, "selftest=on", secs=2, show=("selftest:",))
    check("self-test on", any("selftest: on" in line for line in lines) or bool(matches(lines, SELFTEST)),
          why="no 'selftest: on' / 'selftest: OUT' line (refused while a Rytm is connected)")


def selftest_off(port):
    lines = session(port, "selftest=off", secs=1.5, show=("selftest:",))
    check("self-test off", any("selftest: off" in line for line in lines))


def cmd_engines(a, port):
    machines = [m for m in a.machines.split(",") if m]
    bad = [m for m in machines if m not in EXPECT]
    if bad:
        sys.exit(f"unknown machine(s) {bad} (have {', '.join(EXPECT)})")
    voices = [int(v) for v in a.rytm_notes.split(",")] if a.rytm_notes else [0]
    note = a.note if a.rytm_notes else 48
    if a.selftest:
        selftest_on(port)
    for m in machines:
        cmds = [f"notemach={v}:{m}" for v in voices] + ([] if a.selftest or a.rytm_notes else ["drone=0"])
        lines = session(port, *cmds, secs=4, show=("notes:", "muted", "unknown command"))
        for v in voices:
            check(f"{m}: voice {v} set", any(f"notes: voice {v} plays {m}" in line for line in lines))
            rx = trig_rx(m, v, note)
            n = len(matches(lines, rx))
            check(f"{m}: voice {v} trigs", n > 0, f"{n} in 4 s", why=f"no line matching /{rx}/")
        check_stats(lines, m, a.selftest)
        muted = [line for line in lines if "muted" in line]
        check(f"{m}: not muted", not muted, muted[0] if muted else "")
        x, path = capture(port, m)
        if x is None:
            check(f"{m}: capture", False, f"capture.py failed ({path})")
            continue
        check_audio(m, x, at_note(EXPECT[m], note), path, at_note(MODE.get(m), note))
        if m == "grain":
            tail = x[-GRAIN_TAIL_S * 48000:]
            check(f"{m}: still sounding after the retriggers", peak_db(tail) > -45.0,
                  f"peak {peak_db(tail):.1f} dBFS in the last {GRAIN_TAIL_S} s")
    if a.selftest:
        selftest_off(port)
    elif not a.rytm_notes:
        session(port, "drone=off")


def cmd_gaps(a, port):
    if a.selftest:
        selftest_on(port)
    pi = Pi(port)
    if a.fifo:
        pi.cmd(f"fifo={a.fifo}")
        pi.pump(1.5)
        check(f"fifo target {a.fifo}", any(f"fifo target {a.fifo} frames" in line for line in pi.lines),
              why="no 'fifo target N frames' line (the kernel clamps to 24..1024)")
    machines = a.machines.split(",") if a.machines else GAPS_MACHINES
    for v in range(8):
        pi.cmd(f"notemach={v}:{machines[v % len(machines)]}")
    pi.cmd("drone=all")
    pi.pump(30)
    c0 = counters(pi.lines, a.selftest)
    outs = []
    t0 = time.time()
    while time.time() - t0 < a.secs:
        pi.pump(10)
        c = counters(pi.lines[-40:], a.selftest)
        ps = pass_stats(pi.lines[-40:])
        if c and ps:
            c["pass"] = f" pass max {ps[0]} us fifo min {ps[1]}"
        if c:
            outs.append(c["out"])
            print(progress(c, int(time.time() - t0)), flush=True)
    check_gaps(c0, counters(pi.lines, a.selftest), outs if a.selftest else [], pi.lines)
    ps = pass_stats(pi.lines)
    if ps:
        print(f"core 3: longest pass {ps[0]} us, FIFO min {ps[1]} frames (the whole run, warm-up included)")
    pi.cmd("drone=off")
    pi.close()
    if a.selftest:
        selftest_off(port)


def cmd_bench(a, port):
    log = a.log
    if not log:
        logs = sorted(glob.glob(os.path.join(CIRCLE, "out", "engine-*.log")), key=os.path.getmtime)
        if not logs:
            check("a flash log", False, "no pi/circle/out/engine-*.log")
            return
        log = logs[-1]
    lines = open(log, errors="replace").read().splitlines()
    for line in lines:
        if "bench" in line:
            print(line.strip())
    cores, summary = check_bench(lines)
    print(f"\n{log}:\n{bench_table(cores, summary)}")


def cmd_analyse(a, port):
    x = read_wav(a.wav, a.channel)
    check_audio(os.path.basename(a.wav), x, a.want, a.wav)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial port (default: the first /dev/cu.usbserial-*)")
    ap.add_argument("--flash", action="store_true", help="build and upload pi/engine first")
    ap.add_argument("--selftest", action="store_true", help="no Rytm: the self-test transport paces the output")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("engines")
    e.add_argument("--machines", default=",".join(EXPECT), help=f"comma-separated (default {','.join(EXPECT)})")
    e.add_argument("--rytm-notes", metavar="V,...", help="the Rytm's own notes play these voices (no drone)")
    e.add_argument("--note", type=int, default=36, help="with --rytm-notes: the note the Rytm sends (default 36)")
    g = sub.add_parser("gaps")
    g.add_argument("secs", type=int)
    g.add_argument("--fifo", type=int, help="set the FIFO target first (:fifo=N)")
    g.add_argument("--machines", help=f"the 8 voices' machines, repeated to 8 (default {','.join(GAPS_MACHINES)}; "
                                      "dsps: DSP SAMPLE on every voice, the card or sample RAM work on core 3)")
    b = sub.add_parser("bench")
    b.add_argument("log", nargs="?", help="a flash log (default: the newest pi/circle/out/engine-*.log)")
    w = sub.add_parser("analyse")
    w.add_argument("wav")
    w.add_argument("--want", type=float, help="the expected pitch in Hz (default: level only)")
    w.add_argument("--channel", type=int, default=2, help="the WAV channel (default 2, OUT MAIN L)")
    a = ap.parse_args()
    port = None
    if a.cmd != "analyse":
        if a.flash:
            flash()
        port = port_of(a)
        if not port and a.cmd != "bench":
            sys.exit("no serial adapter (/dev/cu.usbserial-*)")
    {"engines": cmd_engines, "gaps": cmd_gaps, "bench": cmd_bench, "analyse": cmd_analyse}[a.cmd](a, port)
    if a.cmd in ("engines", "gaps"):
        f = faults(seen)
        check("no fault in the log", not f, f[0] if f else "")
    print(f"\n{'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
