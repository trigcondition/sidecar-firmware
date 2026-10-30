#!/usr/bin/env python3
"""pi_run.py's analysis on the Mac, without a Pi: synthetic captures (4-channel int32 WAVs as capture.py writes,
OUT MAIN L on channel 2) through the level and pitch checks and `analyse`, and synthetic log lines through the stats,
gaps and fault checks.

    uv run python pi/engine/tests/test_pi_run.py
"""
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pi_run  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
t = np.arange(48000 * 10) / 48000.0
LIM = 10 ** (-12 / 20)   # the -12 dBFS limiter's ceiling


def write_capture(path, main_l):
    x = np.zeros((len(main_l), 4), dtype="<i4")
    x[:, 2] = np.clip(main_l * 2147483648.0, -2147483648, 2147483647).astype("<i4")
    x[:, 0] = (0.5 * np.sin(2 * np.pi * 1000 * t[:len(main_l)]) * 2147483647).astype("<i4")   # IN: ignored
    with wave.open(path, "wb") as w:
        w.setnchannels(4)
        w.setsampwidth(4)
        w.setframerate(48000)
        w.writeframes(x.tobytes())


def run_checks(fn):
    pi_run.fails.clear()
    fn()
    return list(pi_run.fails)


def audio(x, want):
    pi_run.fails.clear()
    pi_run.check_audio("x", x, want)
    return list(pi_run.fails)


def main():
    note48 = pi_run.NOTE48
    saw = 2 * ((t * note48) % 1.0) - 1
    gated = np.where((t % 0.5) < 0.3, 1.0, 0.0)   # the drone's / self-test's notes: on 0.3 s of every 0.5 s
    cases = [
        ("sine 130.81 at -12 dBFS", LIM * np.sin(2 * np.pi * note48 * t), note48, []),
        ("saw 130.81 gated at -14 dBFS", 0.2 * saw * gated, note48, []),
        ("sine 261.6 (an octave up)", 0.2 * np.sin(2 * np.pi * 2 * note48 * t), note48, ["x: pitch"]),
        ("sine 65.4 (an octave down)", 0.2 * np.sin(2 * np.pi * note48 / 2 * t), note48, ["x: pitch"]),
        ("sine 65.4 for JUNO/FM (16')", 0.2 * np.sin(2 * np.pi * pi_run.NOTE48_16FT * t), pi_run.EXPECT["juno"], []),
        ("sine 220 for DSP SAMPLE", 0.2 * np.sin(2 * np.pi * 220 * t), 220.0, []),
        ("sine 146.8 (a tone up)", 0.2 * np.sin(2 * np.pi * 146.83 * t), note48, ["x: pitch"]),
        ("sine 523 (two octaves up)", 0.2 * np.sin(2 * np.pi * 4 * note48 * t), note48, ["x: pitch"]),
        ("silence", 0 * t, note48, ["x: level", "x: pitch"]),
        ("over the limiter (-6 dBFS)", 0.5 * np.sin(2 * np.pi * note48 * t), note48, ["x: level"]),
        ("sine 130.81 on a DC offset", 0.1 + 0.1 * np.sin(2 * np.pi * note48 * t), note48, []),
        ("noise, level only (MD)", np.clip(0.05 * np.random.default_rng(1).standard_normal(len(t)), -0.2, 0.2), None, []),
    ]
    for name, x, want, fails in cases:
        got = audio(x, want)
        assert got == fails, f"{name}: fails {got}, wanted {fails}"
        print(f"ok  {name}: peak {pi_run.peak_db(x):.1f} dBFS, pitch {pi_run.pitch(x):.1f} Hz -> fails {got}")

    # A capture on disk: read_wav takes channel 2, `analyse` passes and fails as the checks do.
    with tempfile.TemporaryDirectory() as d:
        good, bad = os.path.join(d, "good.wav"), os.path.join(d, "bad.wav")
        write_capture(good, 0.2 * saw * gated)
        write_capture(bad, 0.2 * np.sin(2 * np.pi * 146.83 * t))
        x = pi_run.read_wav(good)
        assert abs(pi_run.peak_db(x) - 20 * np.log10(0.2)) < 0.1 and pi_run.pitch_ok(pi_run.pitch(x), note48)
        run = lambda p: subprocess.run([sys.executable, os.path.join(HERE, "pi_run.py"), "analyse", p, "--want",
                                        str(note48)], capture_output=True, text=True)
        r = run(good)
        assert r.returncode == 0 and "ALL PASS" in r.stdout, r.stdout
        r = run(bad)
        assert r.returncode == 1 and "FAIL: bad.wav: pitch" in r.stdout, r.stdout
        print("ok  analyse: a good capture passes, a wrong pitch fails")

    # The stats, from the kernel's real line formats (kernel.cpp, selftest.cpp, ob_usb.cpp).
    def eng(under, blocks=4000):
        return (f"engine: fifo min 80 max 96 target 96 under {under} | blocks {blocks} max 40 us | cpu c1 0.4 c2 0.3 "
                f"c3 1.2 % | plaits 0.2 | midi 6 B sysex 2 notes 2 | to-host dropped 0 | peak 120/1000 | 51 C")
    st = lambda out=6857, notes=2: f"selftest: OUT {out}/s notes {notes} slips 0"
    ob = lambda gap=0, lost=0: (f"rytmob: 1000ms IN 6857/s 6857 gap {gap} lost {lost} max 0 odd 0 sh 0 bad 0 err 0 "
                                f"dt 200us pk 0 | OUT 6857/s 6857 err 0 age 0ms cr 2 drop 0 | pend 8/2")
    assert run_checks(lambda: pi_run.check_stats([eng(3), st(), eng(3), st()], "m", True)) == []
    assert run_checks(lambda: pi_run.check_stats([eng(3), ob(), eng(3), ob()], "m", False)) == []
    assert run_checks(lambda: pi_run.check_stats([eng(3), st(), eng(4), st(5000)], "m", True)) == [
        "m: OUT 6857 blocks/s", "m: no output underflow"]
    assert run_checks(lambda: pi_run.check_stats([eng(0)], "m", True)) == ["m: stats lines"]
    c0, c1 = pi_run.counters([eng(5), ob(3, 7)], False), pi_run.counters([eng(5), ob(3, 7)], False)
    assert c0 == {"under": 5, "cpu": ("0.4", "0.3", "1.2"), "gap": 3, "lost": 7, "out": 6857}, c0
    assert run_checks(lambda: pi_run.check_gaps(c0, c1, [], [])) == []
    c2 = pi_run.counters([eng(6), ob(4, 8)], False)
    assert run_checks(lambda: pi_run.check_gaps(c0, c2, [], ["engine: muted !PLAITS(6)"])) == [
        "no Overbridge gaps", "no output underflows", "no muted voice"]
    s0 = pi_run.counters([eng(5), st()], True)
    assert "gap" not in s0 and s0["out"] == 6857
    assert run_checks(lambda: pi_run.check_gaps(s0, s0, [6857, 6858], [])) == []
    assert run_checks(lambda: pi_run.check_gaps(s0, s0, [6857, 3000], [])) == [
        "the self-test pulls 6857 blocks/s throughout"]
    assert pi_run.progress(s0, 10) == "    10 s: OUT 6857/s slips 0 under 5 cpu 0.4/0.3/1.2 %"
    assert pi_run.faults(["ok", "core 2 faulted at PC 0x80000 (fault 1 on this core)", "\x1b[1mpanic"]) == ["\x1b[1mpanic"]
    assert pi_run.faults(["Synchronous exception at 0x80000"]) and not pi_run.faults([eng(0), st()])
    print("ok  stats, gaps and fault checks on the kernel's line formats")

    # The boot bench (kernel.cpp LogBench's two formats, with Circle's log prefix).
    def bench_lines(pct_bp, block_permille, skip=()):
        out = []
        for mach, bp in pct_bp.items():
            for c in range(4):
                if (mach, c) in skip:
                    continue
                b = bp + c   # the cores differ a little
                out.append(f"00:00:01.20 engine: bench c{c} {mach}: {b // 100}.{b % 100:02d} % of a core per voice, "
                           f"worst block {block_permille // 10}.{block_permille % 10} % -> {8000 // b} voices a core")
            w = bp + 3
            out.append(f"00:00:01.30 engine: bench {mach}: {w // 100}.{w % 100:02d} % of a core per voice at 1500 MHz "
                       f"-> {8000 // w} voices a core (worst of cores 0-3, worst block {block_permille // 10}."
                       f"{block_permille % 10} %, budget 25 %)")
        return out
    good = {"plaits": 95, "juno": 310, "fm": 420, "daisy": 150, "dsps": 40, "wt": 120, "gr": 90, "livein": 5, "chop": 70, "md": 60}
    lines = bench_lines(good, 52)
    cores, summary = pi_run.parse_bench(lines)
    assert cores["plaits"][2] == (0.97, 5.2, 8000 // 97), cores["plaits"]
    assert summary["fm"] == {"pct": 4.23, "mhz": 1500, "voices": 8000 // 423, "block": 5.2, "budget": 25}, summary["fm"]
    assert run_checks(lambda: pi_run.check_bench(lines)) == []
    table = pi_run.bench_table(cores, summary)
    assert "| plaits | 0.95 % | 0.96 % | 0.97 % | 0.98 % | 5.2 % (budget 25 %) | 81 |" in table, table
    assert run_checks(lambda: pi_run.check_bench(bench_lines(dict(good, juno=2500), 52))) == [
        "bench juno: four voices fit a core"]
    assert run_checks(lambda: pi_run.check_bench(bench_lines(good, 1003))) == [
        f"bench {m}: worst block within one block" for m in pi_run.BENCH_MACHINES]
    assert run_checks(lambda: pi_run.check_bench(bench_lines(good, 52, skip={("md", 3)}))) == [
        "bench md: cores 0-3 and a summary"]
    assert run_checks(lambda: pi_run.check_bench(lines[:-1])) == ["bench md: cores 0-3 and a summary"]
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "engine-x.log")
        open(log, "w").write("\n".join(["==== rytm-pi engine ==== CPU 1500 MHz"] + lines) + "\n")
        r = subprocess.run([sys.executable, os.path.join(HERE, "pi_run.py"), "bench", log], capture_output=True, text=True)
        assert r.returncode == 0 and "ALL PASS" in r.stdout and "| md |" in r.stdout, r.stdout + r.stderr
        open(log, "w").write("\n".join(bench_lines(dict(good, md=2100), 52)) + "\n")
        r = subprocess.run([sys.executable, os.path.join(HERE, "pi_run.py"), "bench", log], capture_output=True, text=True)
        assert r.returncode == 1 and "FAIL: bench md: four voices fit a core" in r.stdout, r.stdout + r.stderr
    print("ok  bench: per-core and summary lines parsed, the table, a machine over 20 % or a missing core fails")
    print("test_pi_run: PASS")


if __name__ == "__main__":
    main()
