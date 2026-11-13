#!/usr/bin/env python3
"""End to end, emulator only: the Sidecar send FX between the emulated Rytm (rytm-mods sendfx-ui, build tag sfxui:
0022 sfx=1 + 0025 + 0029) and the real engine core on the Mac (sim/sendfx_link.py: libcore over rytm-emu's
Overbridge link). Nothing is flashed; no hardware.

    uv run python sim/sendfx_e2e.py [TEST ...|all]        TESTS: pages names locks kits audio halo

Needs rytm-emu beside this checkout (RYTM_EMU=...) built, with fw/AR173_us_sfxui.syx and the base
sections/us_sfxui.snap + .img + .json (rytm-mods tools/emu/us_base.sh sfxui). The base and its card are only read
(dtboot --card without --card-save). Screenshots, logs and WAVs (firmware screens) go to the git-ignored
sim/build/sendfx-e2e/.

  pages   AMPLITUDE 2 (SFX1 / SFX2 on BD) and SEND FX 1 / 2 (TYPE P1..P5) turned in the emulated Rytm: the core's
          values and its :sendfx answer follow the 0x20 reports; a TYPE change answered by 0x22 STATE
  names   the Rytm's 0x23 ASK answered by the core's 0x21 NAMES + 0x24 LABELS + 0x22 STATE; the TYPE knob shows the core's names
          (screenshots), a user slot whose kit tag does not match is MISSING: the Rytm shows !NAME
  locks   a step-locked SFX1 (BD) and SEND FX 1 P2 (FX 38): the core's values change per step while it plays
  kits    two patterns on two kits with different TYPEs: the core's FX TYPE follows the pattern change
  explore (not in all) knob turns with the values the core receives printed (the encoder's steps)
  audio   the core's SendFx on a synthetic BD impulse stem with the values the Rytm sent: comb peaks at the tuned
          frequency, the chorus delay at P3's time, a TYPE change faded (no click)
  halo    FX 1 = HALO (TYPE 18): READY, the Rytm's P2..P4 at the core, a wide, dense, decaying impulse response
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402

EMU = os.environ.get("RYTM_EMU", os.path.abspath(os.path.join(REPO, "..", "rytm-emu")))
TAG = os.environ.get("SFX_TAG", "sfx1020")
FW = os.path.join(EMU, "fw", f"AR173_us_{TAG}.syx")
SNAP = os.path.join(EMU, "sections", f"us_{TAG}.snap")
CARD = os.path.join(EMU, "sections", f"us_{TAG}.img")
OUT = os.path.join(HERE, "build", "sendfx-e2e")   # git-ignored (build/): firmware screens stay out of git
SR = 48000
fails = []


def check(what, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {what}{(' - ' + detail) if detail else ''}", flush=True)
    if not ok:
        fails.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Run:
    """One scenario: the core host (its own process) and dtboot on the base, with key events."""

    def __init__(self, name, length, events=(), slots=None, extra=(), turn_png=False, snap=None, start=None,
                 save=False, slots_project=False):
        self.name = name
        os.makedirs(OUT, exist_ok=True)
        tmp = tempfile.mkdtemp(prefix="sfxe2e-")
        card = os.path.join(tmp, "card")
        os.makedirs(os.path.join(card, ".rytm", "fxslots"))
        os.makedirs(os.path.join(card, "Samples"))   # the core takes a card without /Samples as no card
        # the user-FX slot map of the default project, and with slots_project of the base's own: the Rytm's 0x10 names it
        # about 0.4 s in (project @001, slot mode) and the core re-sends the list for it
        for key in ("_default", "@001") if slots_project else ("_default",) if slots else ():
            open(os.path.join(card, ".rytm", "fxslots", f"{key}.txt"), "w").write("\n".join(slots) + "\n")
        port = free_port()
        self.log_path = os.path.join(OUT, f"{name}.jsonl")
        self.wav = os.path.join(OUT, f"{name}.wav")
        host = subprocess.Popen([sys.executable, os.path.join(HERE, "sendfx_link.py"), "--card", card, "--link",
                                 f"127.0.0.1:{port}", "--log", self.log_path, "--wav", self.wav, "--secs", "900"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        clock = json.load(open(os.path.join(EMU, "sections", f"us_{TAG}.json")))["clock"] if start is None else start
        self.clock = clock
        cmd = [os.path.join(EMU, "target", "release", "dtboot"), FW, "--load", snap or SNAP, "--card", CARD,
               "--instr", f"{length}M", "--every", "5M", "--usb-host", "--usb-midi-real", "--ob-link",
               f"127.0.0.1:{port}"]
        for at, kind, val in events:
            t = clock + at
            if kind == "press":
                k, _, hold = val.partition(":")
                cmd += ["--press", f"{k}@{t}M" + (f":{hold}" if hold else "")]
            elif kind == "turn":   # ("ENC:DELTA", count, spacing M)
                spec, n, gap = val
                cmd += ["--turn", f"{spec}@{t}Mx{n}/{gap}M"]
        self.snap_out = os.path.join(tmp, f"{name}.snap") if save else None
        if save:
            cmd += ["--save", self.snap_out]
        cmd += list(extra)
        env = dict(os.environ, DTBOOT_END_PNG=os.path.join(OUT, f"{name}-end.png"), DTEMU_STRICT="1")
        if turn_png:
            d = os.path.join(OUT, f"{name}-turns")
            shutil.rmtree(d, True)
            os.makedirs(d)
            env["DTBOOT_TURN_PNG"] = d
            env["DTBOOT_TURN_SETTLE"] = "5000000"   # the knob's value text is up a few UI ticks after the turn
        r = subprocess.run(cmd, cwd=EMU, env=env, capture_output=True, text=True)
        self.emu = r.stdout + r.stderr
        open(os.path.join(OUT, f"{name}.emu.log"), "w").write(self.emu)
        try:
            host.wait(timeout=30)
        except subprocess.TimeoutExpired:
            host.kill()
        hout = host.stdout.read()
        check(f"{name}: emulator and core host ran (no fault, no strict access)",
              r.returncode == 0 and "FAULT" not in self.emu and not any(x.startswith("strict:") for x in
                                                                         self.emu.splitlines())
              and "Traceback" not in hout, (self.emu[-300:] if r.returncode else "") + hout[-300:])
        with open(self.log_path) as f:
            self.log = [json.loads(x) for x in f]
        shutil.rmtree(card, True)

    def vals(self):
        """[(frame, values)] each change of the core's values."""
        return [(e["f"], e["vals"]) for e in self.log if "vals" in e]

    def last(self):
        v = self.vals()
        return v[-1][1] if v else None

    def shows(self):
        return [(e["f"], e["show"]) for e in self.log if "show" in e]

    def tx(self, cmd):
        out = [(e["f"], bytes.fromhex(e["tx"][1]) if isinstance(e["tx"][1], str) else e["tx"][1])
               for e in self.log if "tx" in e and e["tx"][0] == cmd]
        if cmd == P.SENDFX_STATE:   # (sfx-basewidth: a 22 with ver 2 is BASE / WIDTH, not a STATE)
            out = [(f, m) for f, m in out if not m or m[0] != P.SENDFX_BW_VER]
        return out

    def rx(self, cmd):
        return [(e["f"], e["rx"][1]) for e in self.log if "rx" in e and e["rx"][0] == cmd]

    def at(self, frame):
        """The core's values as they stood at `frame`."""
        cur = None
        for f, v in self.vals():
            if f > frame:
                break
            cur = v
        return cur


def frame_of(run, clock_m):
    """The link's frame count at emulator clock clock_m (M instructions, absolute): 48000 frames an emulated second,
    200M instructions an emulated second, counted from the run's start."""
    return int((clock_m - run.clock) * 1e6 / 200e6 * SR)


# Rytm OS 1.73 addresses of the sfxui build's state, as rytm-mods tools/emu/sfu_emu.py and sfx_emu.py use them
# [measured there, 2026-10-16]: live kit k at KIT0 + 2531 k; in a kit, track t's sound index 0 at + 0x3e + 152 t,
# FX word i at + 1866 + 2 i; bank pattern n's kit number at ST_PAT + 13101 (n + 1) + 0x3325.
KIT0 = 0x412558E3
MODS = os.environ.get("RYTM_MODS", os.path.abspath(os.path.join(REPO, "..", "rytm-mods-ui")))   # the sendfx-ui tree
ST_PAT = 0x405C8F38


def peek(snap, addr, n):
    r = subprocess.run([os.path.join(EMU, "target", "release", "dtpeek"), snap, "mem", f"{addr:x}", str(n)], cwd=EMU,
                       env=dict(os.environ, DTEMU_FW=FW), capture_output=True, text=True, check=True)
    return bytes(int(x, 16) for line in r.stdout.splitlines() for x in line.split(":", 1)[1].split())


def w16(snap, addr):
    return int.from_bytes(peek(snap, addr, 2), "big")


PROJ_KIT = 0x407658E5   # the project's stored copy of the active kit; FX word 29 at + 0x7f2, word 0 at + 0x814


def pokes(edits, snap=None):
    """{addr: bytes} -> dtboot --poke args (32-bit words, the other bytes as the base snapshot holds them)."""
    mem = {}
    for a, d in edits.items():
        for i, b in enumerate(d):
            mem[a + i] = b
    out = []
    for w in sorted({a & ~3 for a in mem}):
        cur = bytearray(peek(snap or SNAP, w, 4))
        for i in range(4):
            cur[i] = mem.get(w + i, cur[i])
        out += ["--poke", f"{w:x}={int.from_bytes(cur, 'big'):x}"]
    return out


def kit_values(snap, kit=0):
    """What the core should hold for kit `kit`, read from the Rytm: BD's sends and both FX's TYPE + P1..P5."""
    k = KIT0 + 2531 * kit
    bd = w16(snap, k + 0x3E)
    fx = {i: w16(snap, k + 1866 + 2 * i) for i in (0, 29, 38, 39, 40, 41)}
    hl = lambda w: [(w >> 8) & 0x7F, (w >> 1) & 0x7F]   # noqa: E731
    return dict(send0=hl(bd), knobs=[hl(fx[29]) + hl(fx[38]) + hl(fx[39]), hl(fx[0]) + hl(fx[40]) + hl(fx[41])])


# ---- the tests

def steps(k, n):
    """Knob k (0..7 = A..H) up exactly n values from rest: turns of +4 counts 40M apart, of which the Rytm takes the
    first three after a pause as showing the value only, then one value a turn [measured with t_explore, OS 1.73 +
    rytm-emu's encoder messages]."""
    return "turn", (f"{k}:+4", n + 3, 40)


def tstep(k):
    """One step of an enum knob (rytm-mods 0029 sfx-enum-knobs: TYPE, P1 and the stepped zones step as the Rytm's
    FILTER TYPE): three turns of +8 counts 40M apart - two wake an idle knob (or one is the hold-off after a step on
    an awake one), one steps [measured with rytm-mods tools/emu/sfu_emu.py enum]. TYPE steps through the core's 0x21
    list (OFF and the ids it plays), so it never lands on an id the list lacks."""
    return "turn", (f"{k}:+8", 3, 40)


def type_poke(fx, i):
    """--poke args putting TYPE id i in FX fx (0: word 29 hi, 1: word 0 hi) of the base's live kit 0: an id the core's
    list may lack (a reserved built-in, an empty UFX slot), which turning no longer reaches."""
    a = KIT0 + 1866 + (58 if fx == 0 else 0)
    t = 0x8000E5B0 + 84 * 12 + (58 if fx == 0 else 0)   # and the FX track's trig array: what plays and is reported
    return pokes({a: bytes([i, peek(SNAP, a + 1, 1)[0]]), t: bytes([i, peek(SNAP, t + 1, 1)[0]])})


def big(k, n):
    """n fast turns of +24 counts (3 detents: a whole number, so a later steps() is not off by a remainder), 20M
    apart: several values each after the first (the Rytm's acceleration)."""
    return "turn", (f"{k}:+24", n, 20)


def t_explore(a):
    """(not in all) turns on SEND FX 1 with the values the core receives printed: how the Rytm's encoder steps."""
    ev = [(700, "press", "FX"), (800, "press", "SRC"), (900, "turn", ("0:+24", 4, 20)),
          (1000, "turn", ("0:-24", 12, 20)),
          (1240, "turn", ("0:+4", 6, 40)), (1600, "turn", ("0:+24", 14, 20)), (1880, "turn", ("0:-4", 6, 40))]
    r = Run("explore", 2800, ev, turn_png=True)
    for f, v in r.vals():
        print(f"    f {f} t {f / SR:.2f} s knobs {v['knobs']}")


def t_pages(c):
    """AMPLITUDE 2 and SEND FX 1 / 2 turned in the Rytm: the core's values, :sendfx and 0x22 follow."""
    ev = [(700, "press", "AMP"), (800, "press", "AMP"), (900, *big(0, 9)), (1150, *big(1, 5)),
          (1400, "press", "FX"), (1500, "press", "SRC"), (1600, *tstep(0)), (2000, *tstep(0)),
          (2400, *big(1, 6)), (2550, *big(2, 4)),
          (2700, *big(3, 3)), (2850, *big(4, 3)), (3000, *big(5, 2)),
          (3200, "press", "SMPL"), (3300, "press", "SMPL"), (3400, *tstep(0)), (3700, *big(1, 3)),
          (3850, *big(2, 5)),
          (4000, *big(3, 4)), (4150, *big(4, 2)), (4300, *big(5, 3))]
    r = Run("pages", 4900, ev, save=True)
    want = kit_values(r.snap_out)
    got = r.last()
    print(f"    Rytm kit 0: BD sends {want['send0']} FX1 {want['knobs'][0]} FX2 {want['knobs'][1]}")
    print(f"    core:       BD sends {got['send'][0]} FX1 {got['knobs'][0]} FX2 {got['knobs'][1]}")
    check("pages: the core holds the Rytm's BD SFX1 / SFX2 (sound index 0 hi / lo), both turned",
          got["send"][0] == want["send0"] and want["send0"][0] > 0 and want["send0"][1] > 0, str(got["send"][0]))
    check("pages: the core holds SEND FX 1 TYPE P1..P5 = FX words 29 / 38 / 39, all turned", got["knobs"][0] ==
          want["knobs"][0] and all(want["knobs"][0]), str(got["knobs"][0]))
    check("pages: the core holds SEND FX 2 TYPE P1..P5 = FX words 0 / 40 / 41, all turned", got["knobs"][1] ==
          want["knobs"][1] and all(want["knobs"][1]), str(got["knobs"][1]))
    check("pages: the other tracks' sends untouched (0)", all(v == [0, 0] for v in got["send"][1:]),
          str(got["send"][1:]))
    sh = r.shows()[-1][1]
    k1, k2 = got["knobs"]
    name = lambda i: P.SENDFX_TYPES.get(i, f"FX{i}")   # noqa: E731
    miss = lambda i: "" if i in (0,) + P.SENDFX_BUILT else " MISSING"   # noqa: E731  (the ids this build plays)
    want_sh = (f"fx1 {name(k1[0])} ({k1[0]}){miss(k1[0])} p {' '.join(map(str, k1[1:]))} send {got['send'][0][0]} 0 0",
               f"fx2 {name(k2[0])} ({k2[0]}){miss(k2[0])} p {' '.join(map(str, k2[1:]))} send {got['send'][0][1]} 0 0")
    print(f"    :sendfx -> {sh}")
    check("pages: :sendfx shows both FX's TYPE by name, P1..P5 and BD's send", all(w in sh for w in want_sh),
          " | ".join(want_sh))
    types = [v["knobs"][0][0] for _, v in r.vals()]
    types = [x for i, x in enumerate(types) if i == 0 or types[i - 1] != x]
    st = [(f, P.decode_sendfx_state(m)) for f, m in r.tx(P.SENDFX_STATE)]
    fx1 = [(s["id"], s["state"], s["name"]) for _, s in st if s["fx"] == 0]
    listed = [i for _, m in r.tx(P.SENDFX_NAMES) for i, _ in P.decode_sendfx_names(m)[2]]
    exp = [(i, P.ST_READY if i in P.SENDFX_BUILT else P.ST_EMPTY if i == 0 else P.ST_MISSING, P.SENDFX_TYPES[i])
           for i in types]
    check("pages: SEND FX 1's TYPE stepped through the core's list in its order (OFF and the built effects only: an "
          "enum knob, sfx-enum-knobs), each id answered by 0x22 STATE in order", len(types) >= 2 and types[0] == 0 and
          all(i in listed for i in types) and types == sorted(types, key=listed.index) and
          [x for i, x in enumerate(fx1) if i == 0 or fx1[i - 1] != x] == exp, f"types {types} list {listed} 22s {fx1}")
    fx2 = [(s["id"], s["state"], s["name"]) for _, s in st if s["fx"] == 1]
    check("pages: SEND FX 2's TYPE answered: 0x22 FX 2 = its TYPE (a built effect) READY",
          fx2 and fx2[-1] == (k2[0], P.ST_READY, P.SENDFX_TYPES[k2[0]]) and k2[0] in P.SENDFX_BUILT, str(fx2))
    c["pages"] = r


def type_text(r, fx=0):
    """What 0029 should show on FX fx's TYPE knob, from what the core sent: the 0x21 list's name (else FX<n> /
    UFX<n>), with ! when the last 0x22 for that FX reported the id MISSING (a MISSING id the list lacks: !<22's
    name>, 7 characters as 0029 keeps them)."""
    names = {}
    for _, m in r.tx(P.SENDFX_NAMES):
        rep, _, ent = P.decode_sendfx_names(m)
        names = {} if rep else names
        names.update({i: n[:7] for i, n in ent})
    st = [P.decode_sendfx_state(m) for _, m in r.tx(P.SENDFX_STATE)]
    st = [x for x in st if x["fx"] == fx]
    i = r.last()["knobs"][fx][0]
    miss = st and st[-1]["state"] == P.ST_MISSING and st[-1]["id"] == i
    n = names.get(i) or (st[-1]["name"][:7] if miss else "")
    if not n:
        return f"FX{i}" if i < 64 else f"UFX{i - 63}"
    return ("!" if miss else "") + n


def shown(snap, i, fx=0):
    """0029's TYPE value text for id i, from its state in the snapshot (sfu_st: name[128][8], then miss_id[2]; the
    build's symbol from rytm-mods' build/emu/<TAG>/manifest.json, written by tools/emu/us_base.sh), as type_text
    builds it: the name with ! when miss_id[fx] == i, FX<n> / UFX<n> for no name."""
    m = json.load(open(os.path.join(MODS, "build", "emu", TAG, "manifest.json")))
    a = m["elek"]["symbols"]["rytm-0029-sendfx-ui:sfu_st"]
    n = peek(snap, a + 8 * i, 8).split(b"\0")[0].decode("ascii", "replace")
    miss = peek(snap, a + 1024 + fx, 1)[0]
    if not n:
        return f"FX{i}" if i < 64 else f"UFX{i - 63}"
    return ("!" if miss == i else "") + n


def t_names(c):
    """0x23 ASK -> the core's 0x21 NAMES + 0x22 STATE; the TYPE knob shows them (screenshots), !NAME for MISSING."""
    # the core's card: UFX1 holds MYVERB (tag 0x1234); the Rytm sends no kit tag (M4: tags open), so it is MISSING.
    # Each phase ends with a screenshot 100M after its last turn: the value text stays up, the 0x22 is in by then.
    # TYPE is an enum over the core's list (sfx-enum-knobs): OFF, the built effects, 64 MYVERB; an id the list lacks
    # (7, a reserved built-in; 65, an empty UFX slot) is reached only from the kit - poked here - and stays shown
    # until moved away. Phases: the kit on 2, 7, 65; then 7 and one step up, which lands on the next listed id up
    # (64: the list is OFF 1 2 64).
    ev = [(1000, "press", "FX"), (1100, "press", "SRC")]
    shots, runs = [], []
    for k, (i, evk, at) in enumerate([(2, [], 1460), (7, [], 1460), (65, [], 1460), (7, [(1700, *tstep(0))], 2000)]):
        r = Run(f"names{k + 1}", at, ev + evk, slots=["1234 MYVERB"], save=True, extra=type_poke(0, i))
        runs.append(r)
        i = r.last()["knobs"][0][0]
        shots.append((os.path.join(OUT, f"names{k + 1}-end.png"), i, type_text(r), shown(r.snap_out, i)))
    asks = r.rx(P.SENDFX_ASK)
    # the list may span messages (8 names each): the first has replace, the rest append; the core sends it unasked on
    # the first 0x20 after boot (sendfx-hello), so the Rytm's 0x23 ASK is optional
    burst = [P.decode_sendfx_names(m) for _, m in r.tx(P.SENDFX_NAMES)]
    names = []
    for d in burst:
        if d[0] or not names:
            names.append((d[0], d[1], list(d[2])))
        else:
            names[-1][2].extend(d[2])
    built = [(i, P.SENDFX_TYPES[i]) for i in sorted((0,) + P.SENDFX_BUILT)]   # the core lists in id order
    check("names: the core sent 0x21 NAMES (unasked after boot, or answering a 0x23 ASK), over one or more messages "
          "(replace on the first): OFF, the built effects and UFX1's MYVERB at 64",
          names and names[0][0] and names[0][1] == len(built) + 1 and names[0][2] == built + [(64, "MYVERB")],
          f"{len(asks)} asks, {len(burst)} messages, {names[:1]}")
    # MYVERB in the base's project too, so the list it sends stays two messages: 0029 holds them - the first message
    # replaces, the second appends
    r0 = Run("names0", 800, ev, slots=["1234 MYVERB"], save=True, extra=type_poke(0, 2), slots_project=True)
    my = shown(r0.snap_out, 64)
    print(f"    0029's name for id 64 at 800M (before the project message): {my}")
    check("names: 0029 took the list's second message (appended to the first's): UFX1 is named MYVERB in its TYPE list",
          my == "MYVERB", my)
    labs = [P.decode_sendfx_labels(m) for _, m in r.tx(P.SENDFX_LABELS)]
    want_l = [(i, list(lab), [tuple(z) for z in zs]) for i, (lab, zs) in sorted(P.SENDFX_LABEL_SETS.items())]
    # (a whole run of them: a hello or a project starts the list again, a run cut short by that is no fault)
    check("names: and 0x24 LABELS for each built effect (protocol.py's SENDFX_LABEL_SETS)",
          any(labs[i:i + len(want_l)] == want_l for i in range(len(labs))), f"{[x[0] for x in labs]}")
    st = [P.decode_sendfx_state(m) for _, m in r.tx(P.SENDFX_STATE)]
    fx1 = [(s["id"], s["state"], s["name"]) for s in st if s["fx"] == 0]
    fx1 = [(s["id"], s["state"], s["name"]) for x in runs for s in
           (P.decode_sendfx_state(m) for _, m in x.tx(P.SENDFX_STATE)) if s["fx"] == 0]
    want = [(2, P.ST_READY, "COMB"), (7, P.ST_MISSING, "S4 RESO"), (65, P.ST_MISSING, "UFX2"),
            (64, P.ST_MISSING, "MYVERB")]
    up = min(i for i in [b[0] for b in built] + [64] if i > 7)   # the next listed id above the unlisted 7
    want[3] = (up, P.ST_READY if up in P.SENDFX_BUILT else P.ST_MISSING, P.SENDFX_TYPES.get(up, "MYVERB"))
    check("names: 0029's TYPE text (from its state, as type_text draws it) is the core's: COMB, !S4 RESO, !UFX2, then "
          "the id one step above 7", [x[3] for x in shots] == [x[2] for x in shots] and [x[2] for x in shots][:3] ==
          ["COMB", "!S4 RESO", "!UFX2"], str([(x[2], x[3]) for x in shots]))
    check(f"names: TYPE on 2, 7, 65 (the kit's), 7 + one step up = {up} (the unlisted 7 sits in id order in the "
          "core's list), each answered with 0x22", [x[1] for x in shots] == [2, 7, 65, up]
          and all(w in fx1 for w in want), f"{[x[1] for x in shots]} {fx1}")
    for sh, i, w, _ in shots:
        print(f"    screenshot {os.path.relpath(sh, REPO)}: TYPE {i}, the knob should read {w}")
    c["names_shots"] = shots


def toggles(seq):
    """[(frame, value)] -> the changes only."""
    out = []
    for f, v in seq:
        if not out or out[-1][1] != v:
            out.append((f, v))
    return out


def t_locks(c):
    """P-locks: BD step 1 locks SFX1/SFX2, the FX track's step 1 locks SEND FX 1 P2; playing, the core's values
    follow the steps."""
    # as rytm-mods sfu_emu.py t_locks: base values, RECORD, trigs on steps 1 and 5 (BD, then FX), step 1 held + knobs
    tr = lambda k, n: ("turn", (f"{k}:+20", n, 20))   # noqa: E731
    ev = [(50, "press", "AMP"), (150, "press", "AMP"), (200, *tr(1, 2)),
          (250, "press", "RECORD"), (350, "press", "T1"),
          (450, "press", "T5"), (600, "press", "T1:500M"), (700, *tr(0, 3)), (850, *tr(1, 2)),
          (1200, "press", "FX"), (1300, "press", "SRC"), (1350, *tr(1, 2)), (1380, *tr(4, 2)),
          (1450, "press", "T1"), (1550, "press", "T5"),
          (1650, "press", "T1:500M"), (1750, *tr(2, 3)), (1900, *tr(0, 2)),
          (2300, "press", "RECORD"), (2500, "press", "PLAY")]
    r = Run("locks", 4500, ev)
    play = frame_of(r, r.clock + 2600)
    bd = toggles([(f, tuple(v["send"][0])) for f, v in r.vals() if f > play])
    p2 = toggles([(f, v["knobs"][0][2]) for f, v in r.vals() if f > play])
    ty = {v["knobs"][0][0] for f, v in r.vals() if f > play}
    print(f"    BD sends while playing: {[(round(f / SR, 2), v) for f, v in bd[:6]]}")
    print(f"    FX1 P2 while playing:   {[(round(f / SR, 2), v) for f, v in p2[:6]]}")
    bar = 60 / 127 * 4   # 16 sixteenths at the base's 127 BPM
    def per(ch):   # from the second change on (the first is the value before PLAY catching up)
        ch = ch[1:]
        return np.diff([f for f, v in ch if v == ch[0][1]]) / SR
    check("locks: playing, the core's BD SFX1/SFX2 alternate between two values (the step 1 lock, the base on "
          "step 5), at least 3 changes", len(bd) >= 4 and len({v for _, v in bd}) == 2, str(bd[:4]))
    check("locks: playing, the core's SEND FX 1 P2 alternates (step 1 lock / base), the TYPE not locked (one value)",
          len(p2) >= 4 and len({v for _, v in p2}) == 2 and len(ty) == 1, f"{p2[:4]} types {ty}")
    d1, d2 = per(bd), per(p2)
    check(f"locks: each value comes back once a bar ({bar:.3f} s at 127 BPM, +-30 ms)",
          len(d1) >= 2 and len(d2) >= 2 and np.all(abs(d1 - bar) < 0.03) and np.all(abs(d2 - bar) < 0.03),
          f"BD {np.round(d1, 3)} P2 {np.round(d2, 3)}")
    gaps = np.diff([f for f, _ in p2]) / SR
    check(f"locks: P2's lock holds 4 steps ({bar / 4:.3f} s: step 1 to step 5), the base 12 (+-30 ms)",
          np.all(abs(gaps[0::2] - bar / 4) < 0.03) and np.all(abs(gaps[1::2] - 3 * bar / 4) < 0.03),
          str(np.round(gaps, 3)))
    c["locks"] = r


def pattern(at, t):
    """Select bank A pattern t (1..16): BANK_A held, the trig key pressed."""
    return [(at, "press", "BANK_A:200M"), (at + 50, "press", f"T{t}")]


def save_kit(at):
    """SAVE KIT (FUNC + PLAYMODE: the KIT menu, its third entry), into the kit's own slot, its name kept (as rytm-mods
    tools/emu/el_area.py): the last YES 1400M after `at`; leave ~500M more before the next key."""
    ev = [(at, "press", "FUNC:300M"), (at + 20, "press", "PLAYMODE")]
    t = at + 400
    for k in ["UP"] * 3 + ["DOWN"] * 2 + ["YES"]:
        ev.append((t, "press", k))
        t += 100
    return ev + [(t + 200, "press", "YES"), (t + 400, "press", "YES")]


def t_kits(c):
    """Kit travel: A01 (kit 0) SEND FX 1 = COMB, A02 (kit 1) = CHORUS, set on the pages; playing A01 then A02 and
    back switches the core's FX 1 TYPE at the pattern changes."""
    # A kit edited on the pages and left unsaved is dropped when a pattern on another kit loads [measured: kit 0's
    # TYPE was gone after A01 -> A02 -> A01], as on the Rytm: each kit is saved (SAVE KIT) after its TYPE is set.
    # All while playing (a pattern selected while stopped did not load its kit here [measured]; playing, a selected
    # pattern starts at the end of the one playing, ~1.9 s).
    ev = ([(700, "press", "FX"), (800, "press", "SRC"), (900, "press", "PLAY"), (1000, *tstep(0)), (1200, *tstep(0))] +
          save_kit(1400) + pattern(3300, 2) + [(3900, *tstep(0))] + save_kit(4300) + pattern(6300, 1) +
          pattern(7300, 2) + pattern(8300, 1))
    r = Run("kits", 9500, ev, save=True)
    kit_a = [peek(r.snap_out, ST_PAT + 13101 * n + 0x3325, 1)[0] for n in (1, 2)]
    k0, k1 = kit_values(r.snap_out, 0)["knobs"][0][0], kit_values(r.snap_out, 1)["knobs"][0][0]
    print(f"    A01 on kit {kit_a[0]}, A02 on kit {kit_a[1]}; the Rytm's FX1 TYPE: kit 0 = {k0}, kit 1 = {k1}")
    ty = toggles([(f, v["knobs"][0][0]) for f, v in r.vals()])
    print(f"    the core's FX1 TYPE: {[(round(f / SR, 2), v) for f, v in ty]}")
    play = frame_of(r, r.clock + 6000)   # from the third pattern select on: both kits saved
    played = [(f, v) for f, v in ty if f >= play]
    check("kits: A01 and A02 on different kits, kit 0's TYPE COMB (2) and kit 1's CHORUS (1) in the Rytm",
          kit_a[0] != kit_a[1] and (k0, k1) == (2, 1), f"kits {kit_a} types {k0} {k1}")
    check("kits: the queued pattern changes switch the core's TYPE: 2 (A01, kit 0), 1 (A02, kit 1), 2 (A01)",
          [v for _, v in played] == [2, 1, 2], str(ty))
    st = [P.decode_sendfx_state(m) for _, m in r.tx(P.SENDFX_STATE)]
    names = [x["name"] for x in st if x["fx"] == 0]
    check("kits: each switch answered by 0x22 (COMB / CHORUS READY)", names[-3:] == ["COMB", "CHORUS", "COMB"],
          str(names))
    if len(played) == 3:
        sel = [frame_of(r, r.clock + t + 50) for t in (6300, 7300, 8300)]
        lag = [(f - s0) / SR for (f, _), s0 in zip(played, sel)]
        check("kits: each TYPE switch follows its pattern select (queued to the playing pattern's end: 0..2 bars + "
              "a report)", all(0 < x < 2 * 60 / 127 * 4 + 0.1 for x in lag), f"{np.round(lag, 2)} s after the selects")
    c["kits"] = r


def read_wav(path):
    with wave.open(path) as w:
        a = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels())
    return a.astype(np.float64) / 32767


def replay(r, stem):
    """The run's 0x20s into a fresh core, its SendFx on `stem` in the live run's blocks -> (stem, L, R) array."""
    out = r.wav[:-4] + f".replay-{stem}.wav"
    card = tempfile.mkdtemp(prefix="sfxe2e-card-")
    os.makedirs(os.path.join(card, "Samples"))
    subprocess.run([sys.executable, os.path.join(HERE, "sendfx_link.py"), "--card", card, "--replay", r.log_path,
                    "--blocks", r.wav + ".blocks.npy", "--wav", out, "--stem", stem], check=True,
                   capture_output=True)
    shutil.rmtree(card, True)
    return read_wav(out)


def t_audio(c):
    """Audio (synthetic stem, real values): the core's SendFx with what the Rytm sent - CHORUS delay, COMB tuning,
    the TYPE change faded."""
    ev = [(700, "press", "AMP"), (800, "press", "AMP"), (900, *big(0, 9)),
          (1150, "press", "FX"), (1250, "press", "SRC"),
          (1650, *big(2, 3)), (1800, *big(3, 2)), (1950, *big(4, 3)),   # P2..P4 (P1 is ROUTE: left PRE)
          (2800, *tstep(0)), (3700, "turn", ("0:+4", 1, 40))]
    r = Run("audio", 4400, ev, extra=type_poke(0, 1))   # CHORUS in the kit (an enum TYPE: one step on is COMB)
    tl = [(f, v["knobs"][0], v["send"][0][0]) for f, v in r.vals()]
    ty = toggles([(f, k[0]) for f, k, _ in tl])
    print(f"    FX1 TYPE over the run: {[(round(f / SR, 2), v) for f, v in ty]}")
    live = read_wav(r.wav)
    imp = replay(r, "impulse")
    check("audio: the live render (in the loop, values as they came) equals an offline replay of the logged 0x20s "
          "(same blocks): the log holds everything the effect used", live.shape == imp.shape and
          np.max(abs(live - imp)) <= 1.5 / 32767, f"{live.shape} {imp.shape} max diff "
          f"{np.max(abs(live - imp)) * 32767 if live.shape == imp.shape else '-'} LSB")

    def seg(t):   # the stable stretch of TYPE t: from 0.3 s after its last value change to the next change
        f0 = [f for f, v in ty if v == t]
        if not f0:
            return None
        a = f0[0]
        nxt = [f for f, _ in ty if f > a]
        b = nxt[0] if nxt else len(live)
        last = max([f for f, k, _ in tl if a <= f < b] + [a])
        return last + int(0.3 * SR), b, [k for f, k, _ in tl if f <= last][-1], [s for f, _, s in tl if f <= last][-1]
    sc, sm = seg(1), seg(2)
    check("audio: the Rytm set CHORUS then COMB with BD's SFX1 up", sc and sm and sc[3] > 0 and sm[3] > 0,
          f"{sc} {sm}")
    if not (sc and sm):
        return
    # CHORUS (P1 is ROUTE since 2026-10-16: the effect's knobs are P2..P5): one echo per impulse at the delay P4
    # sets, swung by P3's depth (P2's rate): delay (8 + 17 P4/127) ms, depth (0.25 + 0.75 P3/127) x 6 ms either way
    a, b, k, _ = sc
    dl = (0.008 + 0.017 * k[4] / 127) * SR
    dp = (0.25 + 0.75 * k[3] / 127) * 0.006 * SR
    lags = []
    for i0 in range((a // 12000 + 1) * 12000, b - 3000, 12000):
        for ch in (1, 2):
            x = abs(imp[i0:i0 + 3000, ch])
            lags.append(int(np.argmax(x)))
    print(f"    CHORUS P1..P5 {k[1:]}: delay {dl:.0f} +- {dp:.0f} samples; echo lags {sorted(set(lags))[:8]}...")
    check("audio: CHORUS - every impulse's echo (L and R) at the delay P4 sets, within P3's depth (+-2 samples)",
          lags and all(dl - dp - 2 <= x <= dl + dp + 2 for x in lags), f"{len(lags)} echoes, {min(lags)}..{max(lags)}")
    # COMB: an impulse rings at the period sr / (55 Hz x 2^(P2/24)) (L; R up to 3 % longer by P5)
    a, b, k, _ = sm
    f0 = 55.0 * 2 ** (k[2] / 24)
    per = SR / f0
    pers = []
    for i0 in range((a // 12000 + 1) * 12000, b - 9000, 12000):
        x = imp[i0:i0 + 9000, 1]
        ac = np.correlate(x, x, "full")[len(x) - 1:]
        lo = int(per * 0.7)
        pers.append(lo + int(np.argmax(ac[lo:int(per * 1.3)])))
    spec = np.abs(np.fft.rfft(imp[(a // 12000 + 1) * 12000:(a // 12000 + 1) * 12000 + 9000, 1] * np.hanning(9000)))
    fr = np.fft.rfftfreq(9000, 1 / SR)
    pk = fr[np.argmax(spec[fr > 20]) + np.count_nonzero(fr <= 20)]
    print(f"    COMB P1..P5 {k[1:]}: f0 {f0:.1f} Hz, period {per:.1f} samples; measured {pers[:4]}, spectral peak "
          f"{pk:.1f} Hz")
    check("audio: COMB - the impulse response repeats at the period P2 tunes (+-1 sample), its strongest spectral "
          "peak a harmonic of f0 (+-3 %)", pers and all(abs(x - per) <= 1 for x in pers) and
          abs(pk / f0 - round(pk / f0)) < 0.03 * round(pk / f0) and round(pk / f0) >= 1,
          f"{pers[:4]} vs {per:.1f}, peak {pk:.1f} Hz = {pk / f0:.2f} f0")
    # the TYPE change 1 -> 2 on a continuous stem: faded out, then in, no step
    sine = replay(r, "sine")
    fs = [f for f, v in ty if v == 2][0]
    y = sine[:, 1]
    rms = lambda u: float(np.sqrt(np.mean(u * u)))   # noqa: E731
    before, dip = rms(y[fs - 4800:fs]), min(rms(y[i:i + 48]) for i in range(fs, fs + 1500, 24))
    step_max = np.max(abs(np.diff(y[fs - 2000:fs + 3000])))
    steady = np.max(abs(np.diff(y[fs - 24000:fs - 4000])))
    after = rms(y[fs + 4800:fs + 9600])
    print(f"    TYPE switch at {fs / SR:.3f} s: RMS before {before:.4f}, dip {dip:.5f}, after {after:.4f}; "
          f"largest step {step_max:.4f} (steady chorus {steady:.4f})")
    check("audio: the TYPE change fades (10 ms out, the new effect from silence, 10 ms in): the output dips to "
          "< 5 % of its level, no step larger than the steady signal's, the comb sounds after",
          dip < 0.05 * before and step_max <= 1.05 * steady and after > 0.1 * before,
          f"dip {dip / before:.3f}, step {step_max / steady:.2f}x")
    faults = [e for e in r.log if "end" in e]
    check("audio: no SendFx fault (NaN / runaway restart) in the live run", faults and faults[-1]["faults"] == 0,
          str(faults[-1:]))


def t_halo(c):
    """HALO (TYPE 18, every build): the kit's FX 1 set to HALO, P2..P4 turned in the emulated Rytm - READY, the knobs
    reach the core, its impulse response is a wide, dense, decaying tail."""
    ev = [(700, "press", "AMP"), (800, "press", "AMP"), (900, *big(0, 9)),
          (1150, "press", "FX"), (1250, "press", "SRC"),
          (1650, *big(2, 4)), (1800, *big(3, 3)), (1950, *big(4, 3))]   # P2 DECAY, P3 MOD, P4 TONE (P1 ROUTE: PRE)
    r = Run("halo", 4000, ev, extra=type_poke(0, 18))
    st = [P.decode_sendfx_state(m) for _, m in r.tx(P.SENDFX_STATE)]
    check("halo: FX 1 reported READY as HALO (0x22)", any(x and x["fx"] == 0 and x["id"] == 18 and
                                                            x["state"] == P.ST_READY and x["name"] == "HALO" for x in st),
          str(st[-2:]))
    v = r.last()
    k = v["knobs"][0] if v else None
    check("halo: the core plays HALO with the Rytm's P2..P4 and BD's SFX1", v and k[0] == 18 and all(k[2:5]) and
          v["send"][0][0] > 0, str(v and (k, v["send"][0])))
    live = read_wav(r.wav)
    imp = replay(r, "impulse")
    check("halo: the live render equals an offline replay of the logged 0x20s", live.shape == imp.shape and
          np.max(abs(live - imp)) <= 1.5 / 32767, f"{live.shape} {imp.shape}")
    # the last second (the knobs settled): impulses every 0.25 s on BD -> the tail between them
    n = len(imp)
    a = n - SR
    L, R = imp[a:, 1], imp[a:, 2]
    rms = lambda u: float(np.sqrt(np.mean(u * u)))   # noqa: E731
    corr = float(np.corrcoef(L, R)[0, 1]) if rms(L) > 0 and rms(R) > 0 else 1.0
    # density: the share of 1 ms windows in the last 0.2 s before an impulse that are not near-silent
    i0 = ((a // 12000) + 1) * 12000
    early, late = rms(imp[i0 + 480:i0 + 4800, 1]), rms(imp[i0 + 7200:i0 + 11500, 1])
    win = [rms(imp[i0 + j:i0 + j + 48, 1]) for j in range(2400, 11500, 48)]
    dense = sum(w > 0.05 * max(win) for w in win) / len(win)
    print(f"    HALO P1..P5 {k[1:] if k else '-'}: L/R correlation {corr:.3f}, rms 10..100 ms {early:.4f}, "
          f"150..240 ms {late:.4f}, dense {dense:.2f}")
    check("halo: wide (L/R correlation < 0.3), dense (> 90 % of 1 ms windows lit), decaying within an impulse's "
          "0.25 s (later quieter than earlier), not silent", abs(corr) < 0.3 and dense > 0.9 and 0 < late < early,
          f"corr {corr:.3f} dense {dense:.2f} {early:.4f} {late:.4f}")
    faults = [e for e in r.log if "end" in e]
    check("halo: no SendFx fault in the live run", faults and faults[-1]["faults"] == 0, str(faults[-1:]))


TESTS = {"explore": t_explore, "pages": t_pages, "names": t_names, "locks": t_locks, "kits": t_kits,
         "audio": t_audio, "halo": t_halo}


def main():
    names = [t for t in TESTS if t != "explore"] if sys.argv[1:] in ([], ["all"]) else sys.argv[1:]
    if any(t not in TESTS for t in names):
        sys.exit(__doc__)
    for f in (FW, SNAP, CARD):
        if not os.path.exists(f):
            sys.exit(f"missing {f}")
    ctx = {}
    for t in names:
        print(f"-- {t}: {TESTS[t].__doc__.splitlines()[0]}")
        TESTS[t](ctx)
    print(f"\n{'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
