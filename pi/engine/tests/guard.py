#!/usr/bin/env python3
"""The Pi engine's voice guard on the real Pi 4, over the serial cable, without a Rytm (Task 5; test-on-pi.sh).

The self-test transport (':selftest=on') paces the blocks the guard watches, in place of the Rytm's IN stream.

    uv run --with pyserial python pi/engine/tests/guard.py [--port P] mute|fatal|watchdog|reboot

mute      ':drone=all'; ':overtest=6' -> voice 6 muted "over budget on an overloaded core" (its longest render >= 300 us); ':faulttest=7'
          -> "core 2 faulted at PC 0x... (fault 1 on this core), resumed at task level (EL1t)", voice 7 muted
          "crashed", "engine: muted !X(6) !X(7)"; meanwhile the stats keep coming (no reboot), "under" stays flat through the fault, cpu c2 > 0 (voices 4-5 still render on core 2), peak > 0;
          ':unmute', ':faulttest=5', ':faulttest=7' -> faults 2 and 3 on core 2 recover at task level too, voices 4
          and 6 play; ':unmute' -> "guard: every voice unmuted" and no "engine: muted" line from the second stats
          line on.
fatal     ':hangtest=1' (voice 1 renders 30 ms once) -> "reboot: a render core did not finish its block in 20 ms -
          stopping the selftest stream first", then the serial goes quiet (the Pi in its bootloader; test-on-pi.sh
          then flashes again to show it came back).
watchdog  ':wdtest' (core 0's loop hangs) -> "watchdog test: core 0 hangs", then no more stats lines (the watchdog
          resets the Pi in 4 s; test-on-pi.sh then flashes again: without the watchdog the hung kernel would ignore
          the reboot magic and the flash would fail).
reboot    the flasher's reboot magic (RYTM-REBOOT) sent here while the self-test streams -> "reboot: the flasher's
          magic - stopping the selftest stream first", then quiet (core 0's loop stops the stream before it reboots,
          not the UART interrupt; with the Rytm the stop is "stream stopped cleanly: alt 0", DEFERRED (Rytm)).

Exit status 0 when every check passed. Nothing else may hold the port (flash.sh's monitor must have ended).
"""
import argparse
import glob
import re
import sys
import time

fails = []


def check(what, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {what}{(' - ' + detail) if detail else ''}", flush=True)
    if not ok:
        fails.append(what)


class Pi:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port, 921600, timeout=0.1)
        self.lines = []   # (time, line)

    def cmd(self, text):
        self.s.write(b":" + text.encode() + b"\n")

    def pump(self, secs, show=("guard", "muted", "reboot", "watchdog", "selftest:", "except")):
        t = time.time()
        while time.time() - t < secs:
            line = self.s.readline().decode(errors="replace").strip()
            if line:
                self.lines.append((time.time(), line))
                if any(k in line for k in show):
                    print("  pi:", line, flush=True)

    def since(self, i):
        return [l for _, l in self.lines[i:]]


def grep(lines, rx):
    return [m for m in (re.search(rx, l) for l in lines) if m]


ENGINE = r"engine: fifo min \d+ max \d+ target \d+ under (\d+) .*cpu c1 ([\d.]+) c2 ([\d.]+) c3 [\d.]+ %.*peak (\d+)/1000"
FAULT = r"(?i)\b(exception|abort|panic|assertion failed)\b"


def selftest_on(pi):
    pi.cmd("selftest=on")
    pi.pump(3)
    check("self-test on", bool(grep(pi.since(0), r"selftest: OUT \d+/s")), "a 'selftest: OUT N/s' line")


def mute(pi):
    selftest_on(pi)
    pi.cmd("drone=all")
    pi.pump(4)
    start = len(pi.lines)
    check("no voice muted by the drone alone", not grep(pi.since(0), r"engine: muted"))
    pi.cmd("overtest=6")
    pi.pump(4)
    over = pi.since(start)
    check("overtest armed", bool(grep(over, r"guard: voice 6 runs 300 us long for \d+ blocks")))
    m = grep(over, r"voice 6 !(\w+) muted: over budget on an overloaded core \(longest render (\d+) us, budget (\d+) us\)")
    check("voice 6 muted over budget", bool(m) and int(m[0].group(2)) >= 300,
          m[0].group(0) if m else "no mute line")
    check("engine: muted !X(6)", bool(grep(over, r"engine: muted !\w+\(6\)")))
    mid = len(pi.lines)
    pi.cmd("faulttest=7")
    pi.pump(4)
    fault = pi.since(mid)
    check("faulttest armed", bool(grep(fault, r"guard: voice 7 faults on its next render")))
    check("core 2 faulted, resumed at task level",
          bool(grep(fault, r"core 2 faulted at PC 0x[0-9a-f]+ \(fault 1 on this core\), resumed at task level \(EL1t\)")))
    check("voice 7 muted: crashed", bool(grep(fault, r"voice 7 !\w+ muted: crashed")))
    check("engine: muted !X(6) !X(7)", bool(grep(fault, r"engine: muted !\w+\(6\) !\w+\(7\)")))
    eng = grep(pi.since(start), ENGINE)
    check("stats every second throughout (no reboot)", len(eng) >= 7, f"{len(eng)} engine lines in 8 s")
    # the overtest overloads core 2 for 220 blocks (that is what it takes to mute now) - the FIFO underruns by design;
    # the fault does not: "under" stays flat from the fault test on
    unders = [int(e.group(1)) for e in grep(fault, ENGINE)]
    check("under flat through the fault", len(set(unders)) <= 1, f"under {unders}")
    c2 = [float(e.group(3)) for e in grep(fault, ENGINE)][1:]   # after the fault's own second
    check("core 2 still renders after the fault", bool(c2) and min(c2) > 0, f"cpu c2 {c2}")
    peaks = [int(e.group(4)) for e in eng]
    check("peak above 0", bool(peaks) and min(peaks) > 0, f"peak {peaks}")
    # two more faults on the same core (2), after an unmute: each must recover too (the first one's return left
    # the core in EL1t with its exception stack free, or these would not)
    for n, v in ((2, 5), (3, 7)):
        if n == 2:
            pi.cmd("unmute")
            pi.pump(2)
        i = len(pi.lines)
        pi.cmd(f"faulttest={v}")
        pi.pump(3)
        out = pi.since(i)
        check(f"fault {n} on core 2 (voice {v}) recovered at task level",
              bool(grep(out, rf"core 2 faulted at PC 0x[0-9a-f]+ \(fault {n} on this core\), resumed at task level \(EL1t\)"))
              and bool(grep(out, rf"voice {v} !\w+ muted: crashed")))
        c2 = [float(e.group(3)) for e in grep(out, ENGINE)][1:]
        check(f"core 2 still renders after fault {n}", bool(c2) and min(c2) > 0, f"cpu c2 {c2}")
    check("engine: muted !X(5) !X(7) (voice 4 and 6 play)",
          bool(grep(pi.since(i), r"engine: muted !\w+\(5\) !\w+\(7\)\s*$")))
    um = len(pi.lines)
    pi.cmd("unmute")
    pi.pump(4)
    after = pi.since(um)
    check("unmuted", bool(grep(after, r"guard: every voice unmuted")))
    stats = [i for i, l in enumerate(after) if re.search(r"engine: fifo min", l)]
    late = after[stats[1]:] if len(stats) >= 2 else []
    check("no 'engine: muted' after the unmute", bool(late) and not grep(late, r"engine: muted"))
    check("no fault", not grep(pi.since(0), FAULT))
    pi.cmd("drone=off")
    pi.pump(1)


def fatal(pi):
    selftest_on(pi)
    start = len(pi.lines)
    pi.cmd("hangtest=1")
    pi.pump(5)
    out = pi.since(start)
    check("hangtest armed", bool(grep(out, r"guard: voice 1 runs 30 ms long once")))
    check("clean stop before the reboot",
          bool(grep(out, r"reboot: a render core did not finish its block in 20 ms - stopping the selftest stream first")))
    r = [i for i, l in enumerate(out) if "reboot:" in l]
    check("quiet after the reboot (the bootloader waits)",
          bool(r) and not grep(out[r[0] + 1:], r"engine: fifo min"), "no stats line after the reboot line")
    check("no fault", not grep(out, FAULT))
    bootloader(pi, start, "reboot:")


def bootloader(pi, start, what, limit=30.0):
    """After a reboot: wait (up to limit s) for the serial bootloader's "IHEX-F" prompt, so the next flash does not
    start while it is still booting (measured: a flash ~10 s after the clean stop on the magic stalled its upload;
    one 30 s later went through). Its time from what (a line's text) is printed."""
    t0 = time.time()
    while time.time() - t0 < limit and not any("IHEX" in l for _, l in pi.lines[start:]):
        pi.pump(0.5)
    at = [t for t, l in pi.lines[start:] if what in l]
    up = [t for t, l in pi.lines[start:] if "IHEX" in l]
    check("the serial bootloader is up (its IHEX-F prompt)", bool(up),
          f"{up[0] - at[0]:.1f} s after '{what}'" if up and at else f"none within {limit:.0f} s")


def watchdog(pi):
    selftest_on(pi)
    start = len(pi.lines)
    pi.cmd("wdtest")
    pi.pump(8)
    out = pi.since(start)
    w = [i for i, l in enumerate(out) if "watchdog test: core 0 hangs" in l]
    check("watchdog test started", bool(w))
    check("core 0 hung: no stats line after it", bool(w) and not grep(out[w[0] + 1:], r"engine: fifo min"))
    bootloader(pi, start, "watchdog test: core 0 hangs")


def reboot(pi):
    selftest_on(pi)
    start = len(pi.lines)
    pi.s.write(b"RYTM-REBOOT")
    pi.pump(3)
    out = pi.since(start)
    r = [i for i, l in enumerate(out) if "reboot:" in l]
    check("the magic stops the stream first",
          bool(grep(out, r"reboot: the flasher's magic - stopping the selftest stream first")))
    check("quiet after the reboot (the bootloader waits)", bool(r) and not grep(out[r[0] + 1:], r"engine: fifo min"))
    check("no fault", not grep(out, FAULT))
    bootloader(pi, start, "reboot:")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial port (default: the first /dev/cu.usbserial-*)")
    ap.add_argument("test", choices=["mute", "fatal", "watchdog", "reboot"])
    a = ap.parse_args()
    try:
        import serial  # noqa: F401
    except ImportError:
        sys.exit("needs pyserial: uv run --with pyserial python pi/engine/tests/guard.py ...")
    port = a.port or next(iter(sorted(glob.glob("/dev/cu.usbserial-*"))), None)
    if not port:
        sys.exit("no serial adapter (/dev/cu.usbserial-*)")
    pi = Pi(port)
    pi.pump(1)
    {"mute": mute, "fatal": fatal, "watchdog": watchdog, "reboot": reboot}[a.test](pi)
    pi.s.close()
    print(f"guard.py {a.test}: {len(fails)} failure(s)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
