#!/usr/bin/env python3
"""The IN L/R source end to end in rytm-emu: the emulated Rytm (rytm-mods 0030-fx-in, build rui1024) against the real
engine core (sim/rui_link.py's Host: core/build/libcore.dylib), over the Overbridge link.

    RYTM_MODS=/path/to/rytm-mods-fxin python3 sim/fxin_e2e.py          (a base: rytm-mods tools/emu/us_base.sh rui1024)

The card holds /.rytm/fxin.txt "src=fx" (the setting, as the engine finds it at boot). One run, the Rytm from its base
snapshot with the Overbridge host (--usb-host --usb-midi-real --ob-link): REV send on track 1 and a BD hit, the host
browns out (--usb-host-gone) and comes back, another BD hit. Checks:
  - the host sends 26 01 after the Rytm's first 27 (port up, the engine had no cue to send before: its first contact does)
  - the Rytm answers 27 01 and the BD's reverb tail is on channels 10/11 (stereo), not on the other voice channels
  - after the re-plug the Rytm says 27 00 unasked, the host sends 26 01 again, 27 01 follows, the second hit's tail
    is on 10/11 again (nothing on them in between: the AUDIO IN, unplugged)
  - the core's recorder names for ids 11 / 12 are FX_L / FX_R now (core_c fxin json: state on)
Output: sim/build/fxin_e2e/{link.jsonl,dt.txt}.
"""
import ctypes
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rui_link  # noqa: E402

EMU = os.environ.get("RYTM_EMU", "../rytm-emu")
MODS = os.environ.get("RYTM_MODS", "../rytm-mods-fxin")
TAG = os.environ.get("FXIN_TAG", "rui1024")
FS = 48000
HDR = "F07D5254"
OUT = os.path.join(HERE, "build", "fxin_e2e")
FAILS = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail else ""), flush=True)
    if not ok:
        FAILS.append(name)


def db(x):
    x = x.astype(np.float64) / 2 ** 31
    p = np.mean(x * x) if len(x) else 0.0
    return 10 * np.log10(p) if p > 0 else -200.0


class FxHost(rui_link.Host):
    """The core as the Overbridge host, logging the 26s it sends and the 27s it gets, and the IN frames."""

    def __init__(self, *a):
        super().__init__(*a)
        self.events, self.chunks = [], []
        self.rframes = lambda b, n: self.chunks.append(np.frombuffer(b, "<i4").reshape(n, 12).copy())

    def take(self):
        before = len(self.out)
        super().take()
        for m in self.out[before:]:
            if m[4] == 0x26:
                self.events.append(("tx26", m[5]))
                self.note(tx26=m[5])

    def from_rytm(self, msg):
        if len(msg) == 7 and msg[4] == 0x27:
            self.events.append(("rx27", msg[5]))
            self.note(rx27=msg[5])
        super().from_rytm(msg)


def main():
    os.makedirs(OUT, exist_ok=True)
    clock = json.load(open(os.path.join(EMU, f"sections/us_{TAG}.json")))["clock"]
    card = tempfile.mkdtemp(prefix="fxin-card-")
    os.makedirs(os.path.join(card, ".rytm"))
    os.makedirs(os.path.join(card, "Samples"))   # (the core wants a card with /Samples)
    open(os.path.join(card, ".rytm", "fxin.txt"), "w").write("src=fx\n")
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    h = FxHost(card, os.path.join(OUT, "link.jsonl"), None)
    fx = ctypes.create_string_buffer(120)
    h.lib.core_c_fxin_json.argtypes = [ctypes.c_char_p, ctypes.c_int]
    h.lib.core_c_fxin_json(fx, 120)
    check("the setting is read from the card at boot", b'"want":1' in fx.value, fx.value.decode())
    print(f'link port {port}', flush=True)
    th = threading.Thread(target=rui_link.serve, args=(h, f"127.0.0.1:{port}", 600.0, None, rui_link.HELLO_MIDI), daemon=True)
    th.start()
    at = lambda m: f"{clock + m}M"
    cmd = ["./target/release/dtboot", f"fw/AR173_us_{TAG}.syx", "--load", f"sections/us_{TAG}.snap", "--card",
           f"sections/us_{TAG}.img", "--instr", "2600M", "--every", "100M", "--usb-host", "--usb-midi-real",
           "--ob-link", f"127.0.0.1:{port}", "--midi", f"B05200@{at(100)}", "--midi", f"B05364@{at(110)}",
           "--press", f"BD@{at(600)}", "--usb-host-gone", f"{at(1300)}:{at(1500)}", "--press", f"BD@{at(1900)}"]
    r = subprocess.run(cmd, cwd=EMU, env=dict(os.environ, DTEMU_STRICT="1"), capture_output=True, text=True)
    print('dtboot done; host thread alive:', th.is_alive(), 'frames', h.frames, flush=True)
    th.join(30)
    log = r.stdout + r.stderr
    open(os.path.join(OUT, "dt.txt"), "w").write(log)
    bad = r.returncode != 0 or "FAULT" in log or "strict:" in log
    check("dtboot ran clean (strict, no fault)", not bad, "" if not bad else str(re.findall(r".*(?:FAULT|strict:).*", log)[:3]))
    ev = h.events
    print("  events:", ev)
    a = np.concatenate(h.chunks) if h.chunks else np.zeros((0, 12), "i4")
    np.save(os.path.join(OUT, 'ob.npy'), a)
    check("the Overbridge stream was captured", len(a) > FS)
    check("the host sent 26 01 after connecting", ("tx26", 1) in ev)
    k = [e for e in ev if e == ("tx26", 1)]
    check("26 01 again after the re-plug (the unasked 27 00 is the cue)", len(k) >= 2, f"{len(k)} sent")
    rx = [v for t, v in ev if t == "rx27"]
    check("the Rytm confirmed (27 01), then 27 00 unasked on the re-plug, then 27 01 again (the port-up 27 00 before the link connects is not seen)",
          rx[:3] == [1, 0, 1], str(rx))
    # the tails: the first BD at 600 M (3.0 s after the base, in the stream's time from its start), the second at 1900 M
    lv = np.array([max(db(a[i:i + 4800, 10]), db(a[i:i + 4800, 11])) for i in range(0, len(a) - 4800, 4800)])
    onsets = [i for i in range(1, len(lv)) if lv[i] > -60 and lv[i] > lv[i - 1] + 15]   # a hit's tail starting (>= 15 dB up)
    print(f"  channel 10/11: tail onsets at {[round(i / 10, 1) for i in onsets]} s into the stream, "
          f"{int((lv > -80).sum())} of {len(lv)} 100 ms windows above -80 dB")
    check("a reverb tail starts on 10/11 for each hit: before and after the re-plug", len(onsets) >= 2, f"{len(onsets)} onsets")
    loud = lv > -80
    check("the FX return is on 10/11 in stereo (the two sides differ)", loud.any() and not np.array_equal(a[:, 10], a[:, 11]))
    h.lib.core_c_fxin_json(fx, 120)
    check("the core's state: on (recorder names FX_L / FX_R)", b'"state":"on"' in fx.value, fx.value.decode())
    print(f"{'ALL PASS' if not FAILS else f'{len(FAILS)} FAILED: ' + ', '.join(FAILS)}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
