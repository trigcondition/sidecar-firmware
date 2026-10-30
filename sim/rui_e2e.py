#!/usr/bin/env python3
"""The remote UI end to end in rytm-emu: the emulated Rytm (rytm-mods 0026-remote-ui, build rui1022) against the real
engine core (sim/rui_link.py: core/build/libcore.dylib's remote UI host, core/ui), over the Overbridge link.

    python3 sim/rui_e2e.py [TEST ...]       TESTS: connect sidecar plaits status leave gone keys splash latehello scope wave rec play gfx extmach machines sdvoice srcgfx srcwave noexit lfo srcview latency (default all but latency and srcview)
                                            the pools (rui1043): srclist srcbrowse poolset (poolset-gr / -wt / -ms) poolrefuse poolp3 poolnames pooldsp, or "pools" for all of them
                                            rui1048 (RUI_TAG=rui1048): polycfg polymake (POLY CONFIG from the track menu; E0's make prompt)
                                            rui1049 (RUI_TAG_1043=rui1049): dspsrcyes (FUNC + YES on DSP SAMPLE's SRC view: the browser at once)
                                            rui1051 (RUI_TAG=rui1051): polypat (poly tracks per pattern with the real core's sequencer:
                                            a new pattern plain, TURN OFF holding through GRID REC, a poly <-> plain switch's 2E before its 2D 03)
                                            U1 (RUI_TAG=rui1051): noteedit (NOTE EDIT: DATA walks the notes, the push selects, FUNC + NO deletes)
                                            rui1051 (RUI_TAG=rui1051): chopmach chopchro choplazy (CHOP and LIVE IN in the machine list,
                                            CHRO pads play slices, the CHOP editor from the SRC view and LAZY from the pads)

Every run: rytm-emu's dtboot with the rui1023 firmware from its base snapshot (rytm-mods tools/emu/us_base.sh rui1023),
DTEMU_STRICT=1, --usb-host --usb-midi-real --ob-link, keys and turns by --press / --turn; times are ms after the
snapshot (200 M instructions = 1 emulated second). The checks:
  connect  2026-10-19 (a freeze on hardware, PLAY while the Sidecar connected): BD on JUNO, the sequencer playing and
           pads hammered while the USB host re-attaches RUI_CONNECTS (8) times (rytm-emu --usb-host-gone), PLAY / STOP
           at points through each enumeration: the UI's 20 renders and the audio interrupt's 1500 blocks a second
           throughout, no fault, no USB MIDI wait from interrupt level, a hello each attach, the core silenced at it.
  sidecar  TRK + YES opens the page (30 on); the core sends the whole SIDECAR frame; the panel's own pixels (the OLED
           the panel MCU holds) equal it byte for byte.
  plaits   RIGHT twice (SIDECAR, SYSTEM) goes to the first PLAITS voice page; encoder B (HRM) turned +1 x 6 changes that voice's P2 in the
           engine by +6; the knob's frames carry only changed tiles (a few), the panel sends exactly those; the OLED
           equals the core's last frame.
  leave    TRK + YES again leaves (30 off, reason escape): the stock screen returns, equal to a run that never opened.
  gone     the core stops mid-page: the Rytm leaves by its 1 s timeout, the UI keeps rendering 20 times a second and the
           stock screen returns (no hang).
  status   the two status pages: SIDECAR (the tracks' machines, FX1 / FX2 with PRE / POST) first, RIGHT to SYSTEM (U1..U5,
           the recorder, Overbridge, each core, the network: Ethernet up and its address), drawn as text by the Rytm
           (0026 1021: 3D), never the Wi-Fi's name; again with long names, recording and Wi-Fi (the worst case).
  keys     1022, the catch mask (37): on the PLAITS page PLAY / STOP start and stop the sequencer (the Rytm's MIDI FA / FC)
           while the page stays and shows the core's frame, a pad plays (the Rytm's note-on) and never reaches the core,
           HRM (caught) still edits the voice, LEVEL (passed) never reaches the core; LEFT pressed on PLAITS changes the
           page and so the mask, its release still reaches the core; UP held across leaving: its release eaten; 0026's
           key books empty and the stock screen as a Rytm that never opened the page.
  splash   1023, the connect splash: the base Rytm has been up ~30 s, the run's USB host enumerates it, 0026 says hello
           (35) and the core answers with its splash (the frame, 36 64, the icon sliding in, the line running down, 36 0
           at 3 s): the panel shows the core's frames while PLAY runs the sequencer and a pad plays (nothing reaches the
           core, the page stays closed); after it the stock screen equals the same presses without a splash; NO ends it
           early (eaten); TRK + YES during it opens the SIDECAR page; the core gone mid-splash: stock ~1 s later.
           RUI_SHOTS: the splash's frames on the panel (splash_<ms>.png).
  latehello  TRK + YES inside the half second before the Rytm's first hello (35): the page stays open (no close, no 36), is served
           on (the whole frame and the mask again after the hello) and the panel shows SIDECAR, not the splash.
  scope    LEFT on SIDECAR opens SCOPE (the last page) with sim/rui_live.py's synthetic IN stream in the tap (RUI_TAP):
           its mask (encoders A..C), the frame rate (<= 15 / s), tiles and bytes a second within the budget, the panel
           equals the frame, a TIME turn reaches the core and changes the bottom bar within a frame, the Rytm's UI
           keeps its 20 renders a second while the scope streams.
  wave     voice 1 on DSP SAMPLE (RUI_VOICE1; the core's card with a drum break as pool slot 1, the sample RAM): RIGHT
           twice opens its page, the overview loads (LOADING, then the mode), STA turned moves the region's marker in
           frames of few tiles, MOD turned into GRAIN's zone turns the page into GRAIN (POS SIZE DENS), the panel equals
           the frame.
  rec      the REC page with a model of the Pi's recorder (RUI_REC): LEFT x5 from SIDECAR (POOL, AUTO, SAMPLE, SCOPE, then REC), its mask
           (encoders A..D), YES records (the take's time in large digits, RECORDING), YES again stops, A turned right arms;
           the page stays (YES is the page's), the panel equals the frame.
  play     playback on the REC page against the model's synthetic takes (RUI_REC): B turned picks the channel, B
           pushed plays (PLAYING, the take, BD 2/3, the progress track, the level, the length), D the level; C the
           older take (stops), YES records: playback stops and B / B turned do nothing while the take records.
  gfx      voice 1 on 6-OP FM (RUI_VOICE1): its page with the algorithm diagram; PAT turned picks other ROM voices, the
           diagram's tiles change; the panel equals the frame.
  extmach  the machine chosen on the Rytm (SRC SRC, the machine list to JUNO, then to 6-OP FM, YES: 0021's EXT
           machines on track BD), no voice reported at the start (RUI_FRESH): no report until the track sounds, no
           voice page and no PLAITS guessed; the BD pad (passed while the page shows) trigs it: SIDECAR names it, RIGHT
           twice opens its page; the panel equals the frame.
  machines 1025 (33, rui1025's base, RUI_TAG_1025): JUNO picked in BD's SRC list without a trig: the Rytm's 33s (the
           snapshots, BD (41, 08)), no voice report, TRK + YES and RIGHT twice open voice 1's JUNO page; the list again
           up to an analog machine: SIDECAR names none for track 1, no voice page left.
  lfo      the LFO page (core/lfo.h; RUI_LFO sets L1 on BD's FREQ, L2 a 3 Hz sine on SD's RES, L3 on voice 1's HRM):
           LEFT twice from SIDECAR, its mask (all eight encoders), D turned sets the depth, YES the design view (A..D
           and T1..T16 caught), T5 and B edit step 5, NO back; the CCs to the Rytm (CC 74 on channel 1 only on a change,
           at most 100 a second in all); the panel equals the frame; then the stock FLTR page with and without the
           LFOs: the emulated Rytm took the CC (the panels differ).
  latency  a turn on the PLAITS page, the 1020 host (RUI_LEGACY) against this one: the Rytm's own clocks (DTCN0) for
           turn -> report, turn -> the local echo on the panel, turn -> the host's frame on the panel.
  pools    the pools redesign (docs/superpowers/specs/2026-10-22-pools-tonverk-design.md 3.5, 3.6, 7 "Rytm"; rytm-mods
           0026 1043), rui1043's base (RUI_TAG_1043): srclist / srcbrowse the bank list and FUNC + YES's browser on the
           SAMPLES pool (pool=smp:fill:B:/Banks); poolset FUNC + YES into A005 (WAVETABLE A007: A001..A006 hold the
           built-in tables) and C001 of each machine's own list, set (the pool list file) and committed (48, P1);
           poolrefuse row 000 and input / built-in rows refused; poolp3 the P3 ask (a row in use), IN POOL (one entry per
           file), PLAYING (refused while the Rytm plays), and poolused (a 48 commit is "used"); poolnames bank A's names
           on the Rytm's own SRC page (the 47 with bank byte 0; no host: the firmware's own A010 / OFF); pooldsp DSP
           SAMPLE's 14 table from bank A (an input an empty slot) and the Rytm's SMP list.
  polycfg  rui1048: BD to PLAITS, FUNC + CHRO, DOWN x 9 (POLY CONFIG), YES: 30 page 0x20, the core's POLY CONFIG page
           (not a poly track), YES makes it one (4 NOTES 4 INST), knob A x 3: VOICES 3; NO: the stock screen again.
  polymake rui1048 / E0: UP + TRIG on BD on each Sidecar machine kind: MAKE BD A POLY TRACK? with the machine; on the
           analog BD: why not (POLYMAKE=PLAITS,JUNO limits the kinds).
1021: the text is the Rytm's (3D, drawn by its own text routine in its own font); the core's frame carries the pixels
and the text items. "The panel equals the frame": every pixel outside the text items' boxes equal, and every item's
box inked by the Rytm (rows y .. y + 6, the nominal width + a margin: the Rytm's glyphs are its own).
Output (git-ignored): sim/build/rui_e2e/<test>.{png,dt.txt,jsonl,link.txt} and the frames the core sent (frames_<test>/).
RYTM_EMU: the rytm-emu checkout (default ../rytm-emu); RUI_TAG (default rui1023);
RUI_TAG_1025 / _1027 (the runs that need those builds' messages; default rui1025 / rui1027), RUI_TAG_1043 (the pools
runs; default rui1043);
RUI_CARD_CLONE: a directory the base card image is cloned into (cp -c) and run from, so the base is never written;
RYTM_MODS: the rytm-mods checkout whose build/emu/TAG/manifest.json names 0026's symbols (latency); RUI_SHOTS: copy
the screens there as PNG.
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rui_render import frame_png  # noqa: E402

EMU = os.environ.get("RYTM_EMU", "../rytm-emu")
TAG = os.environ.get("RUI_TAG", "rui1023")
MODS = os.environ.get("RYTM_MODS", "../rytm-mods-1023")   # its build/emu/TAG/manifest.json
SHOTS = os.environ.get("RUI_SHOTS")   # a directory for the screens as PNG (outside git: firmware-derived pixels)
OUT = os.path.join(HERE, "build", "rui_e2e")
TILE_FN, RENDER = 0x40095924, 0x400a2aaa   # 1.73: the panel tile writer, the UI render (20 Hz) [rytm-mods 0026 design.md]
FAILS = []
# TRK + YES opens the page - after the Rytm's hello (35), which comes ~0.5 s after its USB port came up (0026 HELLO_TICKS; the
# link sees it at ~480 ms): the base Rytm has been up 30 s, so a hello with the page closed is answered with the connect splash,
# and the scripted checks are about the page that follows. (A hello with the page already open is its own run, "latehello".)
ENTER = [(560, "press", "TRK", 150), (620, "press", "YES")]


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail else ""), flush=True)
    if not ok:
        FAILS.append(name)


def oled(out):
    """dtboot's last panel screen (DTBOOT_OLED: the panel MCU's own pixels), half blocks -> the 1024 B framebuffer."""
    i = out.rfind("panel MCU OLED")
    if i < 0:
        return None
    fb = bytearray(1024)
    for ty, line in enumerate(out[i:].splitlines()[1:33]):
        for x, ch in enumerate(line[:128]):
            for dy, on in enumerate({"█": (1, 1), "▀": (1, 0), "▄": (0, 1)}.get(ch, (0, 0))):
                if on:
                    y = 2 * ty + dy
                    fb[(7 - y // 8) + 8 * x] |= 1 << (y % 8)
    return bytes(fb)


def status(out):
    """The status lines: [(clock M, {counter: n})]."""
    r = []
    for m in re.finditer(r"^\s*(\d+)M pc=\S+ .*?$", out, re.M):
        cs = dict((int(a, 16), int(n)) for a, n in re.findall(r"([0-9a-f]{8}):(\d+)", m.group(0)))
        r.append((int(m.group(1)), cs))
    return r


class Emu:
    def __init__(self, tag=None):
        tag = tag or TAG
        self.fw = f"fw/AR173_us_{tag}.syx"
        self.card = f"sections/us_{tag}.img"
        clone = os.environ.get("RUI_CARD_CLONE")   # a directory: the base card image cloned there (cp -c), never the base
        if clone:
            os.makedirs(clone, exist_ok=True)
            dst = os.path.join(os.path.abspath(clone), f"us_{tag}.img")
            if not os.path.exists(dst):
                subprocess.run(["cp", "-c", os.path.join(EMU, self.card), dst], check=True)
            self.card = dst
        self.snap = f"sections/us_{tag}.snap"
        self.clock = json.load(open(os.path.join(EMU, f"sections/us_{tag}.json")))["clock"]
        os.makedirs(OUT, exist_ok=True)
        self.core_card = tempfile.mkdtemp(prefix="rui_e2e_card_")

    def at(self, ms):
        return f"{self.clock + ms / 5:g}M"

    def run(self, name, ms, events=(), link=True, stop_at=None, save=False, extra=()):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        cmd = [os.path.join(EMU, "target/release/dtboot"), self.fw, "--load", self.snap, "--card", self.card,
               "--instr", f"{ms // 5}M", "--every", "20M", "--usb-host", "--usb-midi-real",
               "--ob-link", f"127.0.0.1:{port}", "--count", f"{TILE_FN:x}", "--count", f"{RENDER:x}"]
        cmd += list(extra)
        snap = os.path.join(OUT, f"{name}.snap")
        if save:
            cmd += ["--save", snap]
        for e in events:
            if e[1] == "press":
                cmd += ["--press", f"{e[2]}@{self.at(e[0])}" + (f":{e[3] // 5}M" if len(e) > 3 else "")]
            elif e[1] == "turn":
                cmd += ["--turn", f"{e[2]}@{self.at(e[0])}x{e[3]}/{e[4] // 5}M"]
        log = os.path.join(OUT, f"{name}.jsonl")
        frames = os.path.join(OUT, f"frames_{name}")
        sp = None
        if link:
            for p in (log,):
                if os.path.exists(p):
                    os.remove(p)
            args = [sys.executable, os.path.join(HERE, "rui_link.py"), "--link", f"127.0.0.1:{port}", "--card",
                    self.core_card, "--log", log, "--frames", frames]
            if stop_at is not None:
                args += ["--stop-at", str(stop_at)]
            lt = os.path.join(OUT, f"{name}.link.txt")
            sp = subprocess.Popen(args, stdout=open(lt, "w"), stderr=subprocess.STDOUT)
            t0 = time.time()
            while "ready" not in open(lt).read():   # the core up before the emulator runs (it runs faster than real time)
                if time.time() - t0 > 30 or sp.poll() is not None:
                    sys.exit(f"rui_link did not start: {open(lt).read()}")
                time.sleep(0.01)
        r = subprocess.run(cmd, cwd=EMU, env=dict(os.environ, DTEMU_STRICT="1", DTBOOT_OLED="1"), capture_output=True,
                           text=True, timeout=1800)
        if sp:
            try:
                sp.wait(timeout=30)
            except subprocess.TimeoutExpired:
                sp.kill()
        out = r.stdout + r.stderr
        open(os.path.join(OUT, f"{name}.dt.txt"), "w").write(out)
        res = dict(out=out, oled=oled(out), status=status(out), events=[], rc=r.returncode, snap=snap)
        if link and os.path.exists(log):
            res["events"] = [json.loads(x) for x in open(log)]
        if res["oled"]:
            frame_png(os.path.join(OUT, f"{name}.png"), res["oled"])
        fault = "FAULT" in out or "strict:" in out or r.returncode != 0
        check(f"{name}: dtboot ran clean (strict, no fault)", not fault,
              "" if not fault else (re.findall(r".*(?:FAULT|strict:).*", out)[:3] or [f"rc {r.returncode}"]))
        return res


def ev(res, **kw):
    return [e for e in res["events"] if all(e.get(k) == v for k, v in kw.items())]


def last_frame(name, res):
    c = ev(res, tx="commit")
    if not c:
        return None
    return open(os.path.join(OUT, f"frames_{name}", f"frame_{c[-1]['frame']}.bin"), "rb").read()


def diff_bytes(a, b):
    return sum(x != y for x, y in zip(a or b"", b or b"")) if a and b else 1024


def last_texts(name, res):
    c = ev(res, tx="commit")
    if not c:
        return []
    p = os.path.join(OUT, f"frames_{name}", f"texts_{c[-1]['frame']}.json")
    return json.load(open(p)) if os.path.exists(p) else []


def px(fb, x, y):
    return fb[(7 - y // 8) + 8 * x] >> (y % 8) & 1


def text_boxes(items):
    """Each item's box with a margin: the widest advance (5 px, core/ui/ui.h kRSmall) for the width, rows y - 1 .. y + 6."""
    out = []
    for x, y, font, flags, t in items:
        w = (7 if font == 7 else 5) * len(t)   # 1023: the bold font (7) 7 px a character, 10 rows
        a = flags & 3
        x0 = x - w // 2 if a == 1 else x - w if a == 2 else x
        out.append((x0 - 2, y - 1, x0 + w + 2, y + (12 if font == 7 else 7), t))
    return out


def panel_matches(oled, frame, items):
    """(pixels outside the text boxes that differ, the items whose box the Rytm left as the frame has it)."""
    if not oled or not frame:
        return 8192, items
    boxes = text_boxes(items)
    inside = lambda x, y: any(b[0] <= x < b[2] and b[1] <= y < b[3] for b in boxes)   # noqa: E731
    out = sum(px(oled, x, y) != px(frame, x, y) for y in range(64) for x in range(128) if not inside(x, y))
    blank = [b[4] for b in boxes if b[4].strip() and not any(
        px(oled, x, y) != px(frame, x, y) for y in range(max(0, b[1]), min(64, b[3])) for x in range(max(0, b[0]), min(128, b[2])))]
    return out, blank


def shot(name, src):
    if SHOTS and os.path.exists(src):
        os.makedirs(SHOTS, exist_ok=True)
        import shutil
        shutil.copy(src, os.path.join(SHOTS, name + ".png"))


# the SIDECAR page names the eight voices by their tracks (core/ui/remote.cpp kTrackName, UX review T3; was "1".."8")
SIDECAR_TRACKS = ["BD", "SD", "RS", "BT", "LT", "MT", "CH", "CY"]


def t_sidecar(e):
    r = e.run("sidecar", 1200, ENTER)
    m = ev(r, rx="mode")
    check("sidecar: TRK + YES opens the page (30 on, reason user)", bool(m) and m[0]["on"] == 1 and m[0]["reason"] == "user", m[:1])
    c = ev(r, tx="commit")
    check("sidecar: the core sent the whole SIDECAR frame first (128 tiles)", bool(c) and c[0]["tiles"] == 128,
          c[0]["tiles"] if c else None)
    fr, tx = last_frame("sidecar", r), last_texts("sidecar", r)
    check("sidecar: the frame carries the page's text as Rytm text items (3D): SIDECAR, track 1's name (BD), FX1, FX2",
          all(any(t[4] == w for t in tx) for w in ("SIDECAR", "BD", "FX1", "FX2")),
          [t[4] for t in tx][:6])
    out, blank = panel_matches(r["oled"], fr, tx)
    check("sidecar: the panel equals the core's frame outside the text, and the Rytm drew every text item",
          out == 0 and not blank, f"{out} pixels differ outside, not drawn: {blank}")
    print(f"    screenshot {os.path.join(OUT, 'sidecar.png')}")
    shot("sidecar-status-default", os.path.join(OUT, "sidecar.png"))


def t_plaits(e):
    turn_at = 1400
    r = e.run("plaits", 2600, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (turn_at, "turn", "1:+1", 6, 20)])
    keys = [x for x in ev(r, rx="key") if x["key"] == 0x41]
    check("plaits: RIGHT x 2 reached the core (31 0x41 down + up, twice)", len(keys) == 4, keys)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("plaits: the core is on a voice page (PLAITS)", page is not None and page[0] == 1, page)
    voice = page[1] if page else 0
    turns = ev(r, rx="enc")
    total = sum(x["delta"] for x in turns if x["enc"] == 1)
    check("plaits: encoder B's turns arrived summed (+6 on encoder 1)", total == 6, [(x["enc"], x["delta"]) for x in turns])
    ps = [x["param"] for x in r["events"] if "param" in x]
    hrm = [p for p in ps if p[0] == voice and p[1] == 1]
    check(f"plaits: voice {voice + 1}'s HRM (P2) in the engine moved +6 (the core's 0x03 path), nothing else",
          bool(hrm) and hrm[-1][3] - hrm[0][2] == 6 and all(p[1] == 1 for p in ps), ps)
    c = ev(r, tx="commit")
    t_turn = turn_at / 1000.0
    after = [x for x in c if x["t"] >= t_turn - 0.02]
    tiles = [x["tiles"] for x in after]
    check("plaits: each knob frame carried only changed tiles (the HRM cell's and the bottom bar's: 1..12 a frame)",
          bool(tiles) and all(0 <= k <= 12 for k in tiles) and any(tiles), tiles)
    # the panel's own tile writes over the turn window = the tiles the core sent then
    stl = r["status"]
    lo, hi = e.clock + (turn_at - 100) / 5, e.clock + 2400 / 5
    w = [x for x in stl if lo <= x[0] <= hi]
    panel = (w[-1][1].get(TILE_FN, 0) - w[0][1].get(TILE_FN, 0)) if len(w) >= 2 else None
    sent = sum(x["tiles"] for x in c if (w[0][0] - e.clock) * 5 / 1000 < x["t"] <= (w[-1][0] - e.clock) * 5 / 1000) if w else None
    # 1021: the Rytm draws the text, so the tiles under a text item that changed are written too (the core never sends
    # those pixels): sent tiles <= panel tiles <= sent + the changed text items' tiles
    txt_tiles = 0
    if w:
        prev = None
        for x in c:
            p_ = os.path.join(OUT, "frames_plaits", f"texts_{x['frame']}.json")
            cur = {(t[0], t[1], t[3]): t for t in json.load(open(p_))} if os.path.exists(p_) else {}
            if prev is not None and (w[0][0] - e.clock) * 5 / 1000 < x["t"] <= (w[-1][0] - e.clock) * 5 / 1000:
                for k in set(cur) | set(prev):
                    if cur.get(k) != prev.get(k):
                        b = text_boxes([cur.get(k) or prev.get(k)])[0]
                        txt_tiles += len({(xx // 8, yy // 8) for xx in range(max(0, b[0] + 2), min(128, b[2] - 2))
                                          for yy in range(max(0, b[1] + 1), min(64, b[3] - 1))})
            prev = cur
    check("plaits: the panel wrote the tiles the core sent and the changed text's, nothing more (no stock redraw)",
          panel is not None and sent <= panel <= sent + txt_tiles, f"panel {panel}, sent {sent}, text tiles {txt_tiles}")
    fr, tx = last_frame("plaits", r), last_texts("plaits", r)
    hv = hrm[-1][3] if hrm else None
    lab = VOICE_TRACKS[voice]   # the page names the voice by its track (core/ui/remote.cpp voice_label)
    check(f"plaits: the bottom bar names the turned knob and its value ({'/'.join(lab)} HRM:{hv}) as Rytm text",
          any(t[4] in [f"{n} HRM:{hv}" for n in lab] for t in tx), [t[4] for t in tx if "HRM" in t[4]])
    out, blank = panel_matches(r["oled"], fr, tx)
    check("plaits: the panel equals the core's last frame outside the text (the knob moved), every text item drawn",
          out == 0 and not blank, f"{out} pixels differ outside, not drawn: {blank}")
    print(f"    tiles per frame after the turn {tiles}; screenshot {os.path.join(OUT, 'plaits.png')}")
    shot("plaits-page", os.path.join(OUT, "plaits.png"))


def t_leave(e):
    ref = e.run("stock", 2600, [], link=False)
    r = e.run("leave", 2600, ENTER + [(1300, "press", "TRK", 300), (1400, "press", "YES")])
    m = ev(r, rx="mode")
    check("leave: TRK + YES again leaves (30 off, reason escape)", len(m) == 2 and m[1]["on"] == 0 and m[1]["reason"] == "escape",
          m)
    late = [x for x in ev(r, tx="commit") if m and len(m) == 2 and x["t"] > m[1]["t"]]
    check("leave: nothing drawn after leaving", not late, len(late))
    check("leave: the stock screen is back, equal to a Rytm that never opened the page",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    print(f"    screenshots {os.path.join(OUT, 'leave.png')} (after leaving), {os.path.join(OUT, 'stock.png')} (never opened)")


def t_gone(e):
    r = e.run("gone", 3600, ENTER, stop_at=1.4)
    check("gone: the core sent frames before it stopped", bool(ev(r, tx="commit")))
    stl = r["status"]
    tail = [x for x in stl if x[0] >= e.clock + 1600 / 5]
    renders = (tail[-1][1].get(RENDER, 0) - tail[0][1].get(RENDER, 0)) if len(tail) >= 2 else 0
    secs = (tail[-1][0] - tail[0][0]) / 200 if len(tail) >= 2 else 1
    check("gone: the UI kept rendering 20 times a second after the core went (no hang)", abs(renders / secs - 20) <= 1,
          f"{renders} renders in {secs:.1f} s")
    ref = e.run("stock-gone", 3600, [], link=False)
    check("gone: the stock screen is back (the Rytm's 1 s timeout), as a Rytm that never opened the page",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    print(f"    screenshot {os.path.join(OUT, 'gone.png')}")


def texts_of(name, res, before=None):
    """The text of the last commit (before emulated second `before`)."""
    c = [x for x in ev(res, tx="commit") if before is None or x["t"] < before]
    if not c:
        return []
    p = os.path.join(OUT, f"frames_{name}", f"texts_{c[-1]['frame']}.json")
    return [t[4] for t in json.load(open(p))] if os.path.exists(p) else []


def status_run(e, name, st, fx=None, events=()):
    os.environ["RUI_STATUS"] = st
    if fx:
        os.environ["RUI_SENDFX"] = fx
    try:
        return e.run(name, 1400, ENTER + list(events))
    finally:
        os.environ.pop("RUI_STATUS", None)
        os.environ.pop("RUI_SENDFX", None)


def panel_check(name, r, live=False):
    """live: a page that keeps changing (the meters): the panel may still show one of the last frames (a commit waits for
    the Rytm's next 20 Hz render), so any of the last three counts."""
    out, blank = panel_matches(r["oled"], last_frame(name, r), last_texts(name, r))
    c = ev(r, tx="commit")
    for x in (c[-3:-1] if live else []):
        if out == 0 and not blank:
            break
        d = os.path.join(OUT, f"frames_{name}")
        fr = open(os.path.join(d, f"frame_{x['frame']}.bin"), "rb").read()
        tp = os.path.join(d, f"texts_{x['frame']}.json")
        out, blank = panel_matches(r["oled"], fr, json.load(open(tp)) if os.path.exists(tp) else [])
    check(f"{name}: the panel equals the frame outside the text, every text item drawn by the Rytm", out == 0 and not blank,
          f"{out} pixels differ outside, not drawn: {blank}")


def t_status(e):
    normal = "3,75,2,1,192.168.1.20,CLUST1|---|DRONE|---|---"
    worst = "3,7425,1,2,192.168.100.200,SUPERSAW|GRANULAR|WAVETBL8|DRONE 02|BASSLINE"
    long_fx = "sendknob=1:0:8;sendknob=1:1:127;sendknob=2:0:4;sendknob=2:1:0"   # CLOUDSEED POST, PINGPONG PRE
    right = [(800, "press", "RIGHT")]
    for tag, st, fx in (("", normal, None), ("-worst", worst, long_fx)):
        n1, n2 = f"status-p1{tag}", f"status-p2{tag}"
        r = status_run(e, n1, st, fx)
        names = texts_of(n1, r)
        check(f"{n1}: TRK + YES opens SIDECAR: the tracks (BD SD RS BT LT MT CH CY), FX1 / FX2, nothing of SYSTEM",
              all(w in names for w in ["SIDECAR", "FX1", "FX2"] + SIDECAR_TRACKS) and
              not any(n.startswith(("OB ", "U1", "ETH", "WIFI", "NET")) for n in names), names)
        if fx:
            check(f"{n1}: the long FX names whole, with PRE / POST", all(w in names for w in ("CLOUDSEED", "PINGPONG", "POST", "PRE")),
                  names)
        panel_check(n1, r)
        shot(n1, os.path.join(OUT, f"{n1}.png"))
        r = status_run(e, n2, st, fx, right)
        st_ = [x for x in r["events"] if "stats" in x]
        page = st_[-1]["stats"][14] if st_ else None
        c0 = ev(r, tx="commit")   # the first frame (SIDECAR): the link's clock is not the emulator's to a few 100 ms
        before, names = texts_of(n2, r, c0[0]["t"] + 1e-6) if c0 else [], texts_of(n2, r)
        check(f"{n2}: RIGHT goes from SIDECAR to SYSTEM (the core on page 2)",
              "SIDECAR" in before and any(t.startswith("SYSTEM") for t in names) and "SIDECAR" not in names and page == 2, (before[:1], names[:1], page))
        want = (["ETH UP 192.168.1.20", "REC 1:15", "U1 CLUST1", "U3 DRONE", "12%", "34%", "5%", "OB OK", "UND 0"] if not fx else
                ["WIFI UP 192.168.100.200", "REC 123:45", "U1 SUPERSAW", "U2 GRANULAR", "U3 WAVETBL8", "U5 BASSLINE"])
        check(f"{n2}: SYSTEM shows U1..U5, the recorder, Overbridge, each core, the network", all(w in names for w in want),
              [w for w in want if w not in names])
        check(f"{n2}: nothing of the Wi-Fi but up / down (no SSID, no key)", not any("SSID" in n or "PSK" in n for n in names))
        panel_check(n2, r)
        print(f"    screenshots {os.path.join(OUT, n1 + '.png')}, {os.path.join(OUT, n2 + '.png')}")
        shot(n2, os.path.join(OUT, f"{n2}.png"))


def peek_st(e, snap, offs, n=512, raw=False):
    """0026's struct rui words at the given offsets (its symbol from rytm-mods' manifest), from a snapshot."""
    man = os.path.join(MODS, "build", "emu", TAG, "manifest.json")
    if not os.path.exists(man):
        return None
    a = json.load(open(man))["elek"]["symbols"]["rytm-0026-remote-ui:st"]
    r = subprocess.run([os.path.join(EMU, "target/release/dtpeek"), snap, "mem", f"{a:x}", str(n)], cwd=EMU,
                       env=dict(os.environ, DTEMU_FW=e.fw), capture_output=True, text=True)
    b = bytearray()
    for line in r.stdout.splitlines():
        m = re.match(r"^[0-9a-f]+: ((?:[0-9a-f]{2} ?)+)", line)
        if m:
            b += bytes.fromhex(m.group(1).replace(" ", ""))
    if raw:
        return bytes(b)
    return {k: int.from_bytes(b[o:o + 4], "big") for k, o in offs.items()}


# struct rui (0026 rui.c): cb_* 304.., er_* 316.., 1021: comps 348, echoes 364, ee_* 372.., enc_sent_now 384, e2e 396..
ST_OFFS = dict(cb_sum=304, cb_n=308, cb_max=312, er_sum=316, er_n=320, er_max=324, comps=348, texts_drawn=352,
               comp_sum=356, comp_max=360, echoes=364, ee_sum=372, ee_n=376, ee_max=380, enc_now=384, e2e_sum=396,
               e2e_n=400, e2e_max=404)
TIMER_HZ = 132e6   # DTCN0 [measured, 0026 design.md]


def t_latency(e):
    res = {}
    for mode in ("legacy", "now"):
        name = f"latency-{mode}"
        if mode == "legacy":
            os.environ["RUI_LEGACY"] = "1"
        try:   # turns of +1 every 120 ms on HRM: each a round trip on its own
            r = e.run(name, 3400, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1400, "turn", "1:+1", 12, 120)],
                      save=True)
        finally:
            os.environ.pop("RUI_LEGACY", None)
        st = peek_st(e, r["snap"], ST_OFFS)
        if not st:
            check("latency: rytm-mods' manifest for the symbols", False, MODS)
            return
        ms = lambda s_, n_: 1000 * st[s_] / max(1, st[n_]) / TIMER_HZ   # noqa: E731
        mx = lambda k: 1000 * st[k] / TIMER_HZ   # noqa: E731
        res[mode] = dict(report=(ms("er_sum", "er_n"), mx("er_max")), echo=(ms("ee_sum", "ee_n"), mx("ee_max"), st["ee_n"]),
                         e2e=(ms("e2e_sum", "e2e_n"), mx("e2e_max"), st["e2e_n"]), blit=(ms("cb_sum", "cb_n"), mx("cb_max")),
                         compose=(1e6 * st["comp_sum"] / max(1, st["comps"]) / TIMER_HZ, 1e6 * st["comp_max"] / TIMER_HZ,
                                  st["comps"], st["texts_drawn"]))
        c = ev(r, tx="commit")
        rx = ev(r, rx="enc")
        lags = []
        for x in rx:   # the host: the 32's arrival -> its next commit (emulated ms)
            n = [y["t"] for y in c if y["t"] >= x["t"]]
            if n:
                lags.append(1000 * (n[0] - x["t"]))
        res[mode]["host"] = (sum(lags) / len(lags) if lags else -1, max(lags) if lags else -1)
        print(f"    {mode}: turn -> report {res[mode]['report'][0]:.1f} ms (max {res[mode]['report'][1]:.1f}); host 32 -> "
              f"frame {res[mode]['host'][0]:.1f} ms (max {res[mode]['host'][1]:.1f}); commit -> blit "
              f"{res[mode]['blit'][0]:.1f} ms (max {res[mode]['blit'][1]:.1f}); turn -> host frame on the panel "
              f"{res[mode]['e2e'][0]:.1f} ms (max {res[mode]['e2e'][1]:.1f}, n {res[mode]['e2e'][2]}); turn -> local echo "
              f"{res[mode]['echo'][0]:.1f} ms (max {res[mode]['echo'][1]:.1f}, n {res[mode]['echo'][2]}); compose "
              f"{res[mode]['compose'][0]:.0f} us mean, {res[mode]['compose'][1]:.0f} us max, {res[mode]['compose'][2]} composes, "
              f"{res[mode]['compose'][3]} texts", flush=True)
    check("latency: the local echo shows a turn on the panel (n > 0) sooner than the host's frame does",
          res["now"]["echo"][2] > 0 and res["now"]["echo"][0] < res["now"]["e2e"][0], res["now"]["echo"])
    check("latency: turn -> the host's frame on the panel is shorter than with the 1020 host",
          res["now"]["e2e"][0] < res["legacy"]["e2e"][0], (res["now"]["e2e"], res["legacy"]["e2e"]))
    json.dump(res, open(os.path.join(OUT, "latency.json"), "w"), indent=1)


K_LEFT, K_UP, K_PLAY, K_STOP, K_BD = 0x40, 0x42, 0x51, 0x52, 0x90
BOOKS = (200, 232, 264, 904)   # struct rui: stock_down, our_down, eat, 1022 both_down (32 B each)


def t_keys(e):
    # the page open: transport, a pad, a caught and a passed knob
    r = e.run("keys", 2300, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1300, "press", "PLAY"),
                                     (1400, "press", "BD"), (1500, "turn", "1:+1", 3, 20), (1600, "turn", "8:+1", 3, 20),
                                     (1900, "press", "STOP")])
    rt = [x["b"] for x in ev(r, rx="rt")]
    check("keys: PLAY started and STOP stopped the sequencer with the page open (the Rytm's MIDI FA, then FC)",
          0xFA in rt and 0xFC in rt and rt.index(0xFA) < max(i for i, b in enumerate(rt) if b == 0xFC), [hex(b) for b in rt])
    keys = [(x["key"], x["down"]) for x in ev(r, rx="key")]
    check("keys: PLAY, STOP and the pad never reached the core", all(k not in (K_PLAY, K_STOP, K_BD) for k, _ in keys), keys)
    fa = [x["t"] for x in ev(r, rx="rt") if x["b"] == 0xFA][:1]
    notes = ev(r, rx="note")
    check("keys: the pad played (the Rytm's note-on 0.1 s after PLAY's FA)", bool(fa) and any(0.05 < n["t"] - fa[0] < 0.3
                                                                                              for n in notes),
          [(n["t"], n["note"]) for n in notes])
    masks = ev(r, tx="mask")
    check("keys: the core sent the catch mask on opening (SIDECAR: all nine encoders) and on the PLAITS page (its encoders)",
          len(masks) >= 2 and masks[0]["encs"] == list(range(9)) and 0x78 in masks[0]["keys"] and 1 in masks[-1]["encs"] and 8 not in masks[-1]["encs"] and
          K_PLAY not in masks[-1]["keys"] and K_BD not in masks[-1]["keys"], [(m["encs"], len(m["keys"])) for m in masks])
    enc = {}
    for x in ev(r, rx="enc"):
        enc[x["enc"]] = enc.get(x["enc"], 0) + x["delta"]
    check("keys: HRM (encoder B, caught) reached the core (+3), LEVEL (passed) did not", enc == {1: 3}, enc)
    st = [x for x in r["events"] if "stats" in x]
    voice = st[-1]["stats"][15] if st else 0
    hrm = [p for p in (x["param"] for x in r["events"] if "param" in x) if p[0] == voice and p[1] == 1]
    check("keys: the caught knob still edits the voice (HRM +3 in the engine)", bool(hrm) and hrm[-1][3] - hrm[0][2] == 3, hrm)
    m = ev(r, rx="mode")
    check("keys: the page stayed open (one 30 on, no 30 off)", [x["on"] for x in m] == [1], m)
    out, blank = panel_matches(r["oled"], last_frame("keys", r), last_texts("keys", r))
    check("keys: the panel shows the core's last frame (no stock draw over it)", out == 0 and not blank,
          f"{out} pixels differ outside, not drawn: {blank}")
    shot("keys-plaits-open", os.path.join(OUT, "keys.png"))
    # ownership: LEFT pressed on PLAITS (caught) changes the page and the mask, released after; B turned on SYSTEM
    # (every knob is caught there) edits nothing; UP held across leaving
    r = e.run("owner", 3600, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1400, "press", "LEFT", 400),
                                      (2000, "turn", "1:+1", 2, 20), (2400, "press", "UP", 600),
                                      (2600, "press", "TRK", 300), (2700, "press", "YES")], save=True)
    keys = [(x["key"], x["down"], x["t"]) for x in ev(r, rx="key")]
    lk = [(d, t) for k, d, t in keys if k == K_LEFT]
    mk = [x["t"] for x in ev(r, tx="mask")]
    check("owner: LEFT went to the core down and up; the up after the mask the new page sent",
          [d for d, _ in lk] == [1, 0] and any(lk[0][1] <= t <= lk[1][1] for t in mk), (lk, mk))
    enc = [x for x in ev(r, rx="enc")]
    check("owner: encoder B on SYSTEM (caught there) reached the core or was ignored there, nothing else",
          all(x["enc"] == 1 for x in enc), enc)
    uk = [d for k, d, t in keys if k == K_UP]
    m = ev(r, rx="mode")
    check("owner: UP held across leaving: down reached the core, its release (after leaving) did not",
          uk == [1] and len(m) == 2 and m[1]["reason"] == "escape", (uk, m))
    books = peek_st(e, r["snap"], {}, 1024, raw=True)
    if books is None and not os.path.exists(os.path.join(MODS, "build", "emu", TAG, "manifest.json")):
        # (the books are read at 0026's symbol, from rytm-mods' manifest of this very build: none here is not a failure)
        print(f"  SKIP  owner: 0026's key books empty - no {MODS}/build/emu/{TAG}/manifest.json (RYTM_MODS)")
    else:
        check("owner: 0026's key books empty (stock / ours / eaten / both)", books is not None and
              not any(any(books[o:o + 32]) for o in BOOKS), MODS)
    ref = e.run("owner-stock", 3600, [], link=False)
    check("owner: after leaving the stock screen is the one of a Rytm that never opened the page and had no turn "
          "(the turn on SYSTEM was caught)",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    # status pages catch every knob: A and LEVEL turned on SIDECAR change nothing in the stock state
    r = e.run("status-knobs", 3600, ENTER + [(1400, "turn", "0:+1", 3, 20), (1800, "turn", "8:+1", 3, 20),
                                             (2600, "press", "TRK", 300), (2700, "press", "YES")], save=True)
    m = ev(r, rx="mode")
    check("status-knobs: left again (30 on, 30 off)", [x["on"] for x in m] == [1, 0], m)
    check("status-knobs: A and LEVEL on SIDECAR never edited the voice",
          not [x for x in r["events"] if "param" in x], [x for x in r["events"] if "param" in x][:3])
    check("status-knobs: after leaving the stock screen is the one of a Rytm that had no turn",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")


def t_splash(e):
    play = [(1200, "press", "PLAY"), (1400, "press", "BD")]
    os.environ["RUI_STATUS"] = "1,0,2,0,192.168.1.20,-|-|-|-|-"   # the Pi on Ethernet: the address on the splash
    try:
        r = e.run("splash", 2000, play)
    finally:
        os.environ.pop("RUI_STATUS", None)
    h = ev(r, rx="0x35")
    sp = ev(r, tx="0x36")
    check("splash: the Rytm's hello (35, up > 20 s, caps 1) answered with the splash (frame, 36 64)",
          len(h) == 1 and h[0]["up"] > 400 and h[0]["caps"] & 1 and sp[:1] and sp[0]["body"] == [1, 64], (h, sp))
    c = ev(r, tx="commit")
    check("splash: after the first (whole) frame, the icon slides in (<= 18 tiles a frame), then the line runs "
          "down (<= 3)",
          len(c) >= 10 and c[0]["tiles"] == 128 and all(x["tiles"] <= 18 for x in c[1:4]) and
          all(x["tiles"] <= 3 for x in c[4:]), [x["tiles"] for x in c][:12])
    items = last_texts("splash", r)
    out, blank = panel_matches(r["oled"], last_frame("splash", r), items)
    check("splash: the panel shows the core's last splash frame (CONNECTED, the address, drawn by the Rytm)",
          out == 0 and not blank and any(t[4] == "CONNECTED" for t in items) and any(t[4] == "192.168.1.20" for t in items),
          f"{out} pixels differ outside, not drawn: {blank}")
    shot("splash_on_2000ms", os.path.join(OUT, "splash.png"))
    rt = [x["b"] for x in ev(r, rx="rt")]
    check("splash: PLAY ran the sequencer and the pad played under it; no key reached the core, the page stayed closed",
          0xFA in rt and bool(ev(r, rx="note")) and not ev(r, rx="key") and not ev(r, rx="mode"), [hex(b) for b in rt])
    r = e.run("splash-end", 4500, play)
    ref = e.run("splash-end-stock", 4500, play, link=False)
    sp = ev(r, tx="0x36")
    check("splash: 36 0 at the end (3 s); then the stock screen equals the same presses without a splash",
          [x["body"] for x in sp] == [[1, 64], [1, 0]] and r["oled"] == ref["oled"],
          (sp, f"{diff_bytes(r['oled'], ref['oled'])} bytes differ"))
    r = e.run("splash-no", 2600, [(1500, "press", "NO")])
    ref = e.run("splash-no-stock", 2600, [], link=False)
    check("splash: NO ends it early and is eaten (the stock screen as without the press)", r["oled"] == ref["oled"],
          f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    r = e.run("splash-open", 2600, [(1300, "press", "TRK", 300), (1400, "press", "YES")])
    m = ev(r, rx="mode")
    out, blank = panel_matches(r["oled"], last_frame("splash-open", r), last_texts("splash-open", r))
    late = [x for x in ev(r, tx="0x36") if m and x["t"] > m[0]["t"]]
    check("splash: TRK + YES during it opens the SIDECAR page (30 on), the page's frame on the panel, no 36 after",
          m[:1] and m[0]["on"] == 1 and out == 0 and not blank and not late and
          any(t[4] == "SIDECAR" for t in last_texts("splash-open", r)), (m, out, blank, late))
    r = e.run("splash-gone", 3000, [], stop_at=1.0)
    ref = e.run("splash-gone-stock", 3000, [], link=False)
    check("splash: the core gone mid-splash: the stock screen back (the Rytm's 1 s without a message)",
          r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")
    if SHOTS:   # the panel along the splash: the hello comes ~0.5 s in, then the slide (50 ms frames) and the line
        t0 = h[0]["t"] * 1000 if h else 500
        for ms in (0, 50, 100, 150, 1500, 2800):
            at = int(t0 + 120 + ms)
            rs = e.run(f"splash-shot-{ms}", at, [])
            shot(f"splash_{ms:04d}ms", os.path.join(OUT, f"splash-shot-{ms}.png"))


def rect_bytes(rects):
    """The 38 messages' bytes of a commit (6 header + 5 + the 7-in-8 packed tiles + F7)."""
    return sum(12 + (nc * np_ * 8 * 8 + 6) // 7 for _, _, nc, np_ in rects)


def t_latehello(e):
    """The Rytm's first hello (35) comes ~0.5 s after its USB port came up. TRK + YES inside that half second opens the page
    before it (the other runs press after it, ENTER): the host keeps the page - no splash over it (the Rytm ignores a 36 while
    its page is open) and it is still served - and sends again what the hello invalidates (the whole frame, the mask)."""
    r = e.run("latehello", 2200, [(300, "press", "TRK", 300), (400, "press", "YES")])
    m, h = ev(r, rx="mode"), ev(r, rx="0x35")
    op = [x for x in m if x["on"] == 1]
    check("latehello: the page opened (30 on, user) and the Rytm's hello (35, up > 20 s) came after it",
          bool(op) and bool(h) and h[0]["t"] > op[0]["t"] and h[0]["up"] > 400 and h[0]["caps"] & 1, (op[:1], h[:1]))
    t_h = h[0]["t"] if h else 0
    check("latehello: the page was not closed (no 30 off) and no splash was sent (no 36)",
          not [x for x in m if x["on"] == 0] and not ev(r, tx="0x36"), (m, ev(r, tx="0x36")))
    c = ev(r, tx="commit")
    after = [x for x in c if x["t"] > t_h]
    check("latehello: the page was served again after the hello (the whole frame, the mask)",
          len(after) >= 1 and after[0]["tiles"] == 128 and any(x["t"] > t_h for x in ev(r, tx="mask")),
          ([x["tiles"] for x in after][:3], [x["t"] for x in ev(r, tx="mask")]))
    tx = texts_of("latehello", r)
    check("latehello: the SIDECAR page is on the panel's frame at the end, not the connect splash",
          "SIDECAR" in tx and "CONNECTED" not in tx, tx)
    panel_check("latehello", r)
    shot("latehello", os.path.join(OUT, "latehello.png"))


def t_scope(e):
    os.environ["RUI_TAP"] = "synth"
    try:
        r = e.run("scope", 4200, ENTER + [(800, "press", "LEFT"), (3000, "turn", "1:+1", 1, 20)])
    finally:
        os.environ.pop("RUI_TAP", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14] if st else None
    check("scope: LEFT on SIDECAR opens SCOPE (the core on an extra page)", page == 3, page)
    masks = ev(r, tx="mask")
    check("scope: its catch mask takes encoders A..C only, the stock keys as every page", bool(masks) and masks[-1]["encs"] == [0, 1, 2]
          and 0x51 not in masks[-1]["keys"] and 0x90 not in masks[-1]["keys"], masks[-1:] if masks else None)
    c = [x for x in ev(r, tx="commit") if 1.4 <= x["t"] < 2.9]
    secs = 1.5
    fps, tps = len(c) / secs, sum(x["tiles"] for x in c) / secs
    bps = sum(rect_bytes(x["rects"]) + 8 for x in c) / secs
    check("scope: at most 15 frames a second while the music plays, and it moves (>= 8)", 8 <= fps <= 15.5, f"{fps:.1f}")
    check("scope: tiles within the budget (<= 900 a second, the panel link ~1560)", tps <= 900, f"{tps:.0f}")
    print(f"    scope link: {fps:.1f} frames/s, {tps:.0f} tiles/s, {bps:.0f} B/s ({bps / max(fps, 0.1):.0f} B a frame)")
    stl = r["status"]
    w = [x for x in stl if e.clock + 1400 / 5 <= x[0] <= e.clock + 2900 / 5]
    renders = (w[-1][1].get(RENDER, 0) - w[0][1].get(RENDER, 0)) if len(w) >= 2 else 0
    rs = (w[-1][0] - w[0][0]) / 200 if len(w) >= 2 else 1
    check("scope: the Rytm's UI keeps rendering 20 times a second while the scope streams", abs(renders / rs - 20) <= 1,
          f"{renders} in {rs:.1f} s")
    turns = [x for x in ev(r, rx="enc") if x["enc"] == 1]
    lag = None
    if turns:
        nxt = [y for y in ev(r, tx="commit") if y["t"] >= turns[0]["t"]]
        lag = 1000 * (nxt[0]["t"] - turns[0]["t"]) if nxt else None
    check("scope: TIME (encoder B) reaches the core, a frame follows within 30 ms", lag is not None and lag <= 30, lag)
    tx = last_texts("scope", r)
    check("scope: the bottom bar shows the new timebase (85MS), the source and the meters' labels as Rytm text",
          any("85MS" in t[4] for t in tx) and any(t[4] == "MAIN" for t in tx) and any(t[4] == "IN" for t in tx),
          [t[4] for t in tx])
    panel_check("scope", r, live=True)
    print(f"    screenshot {os.path.join(OUT, 'scope.png')}")
    shot("scope", os.path.join(OUT, "scope.png"))


def t_wave(e):
    os.environ["RUI_VOICE1"] = "0x20:0,64,64,64,0,64,64,100,64,64,1,0,10,110,0,100"
    os.environ["CORE_SAMPLE_RAM_MB"] = "16"
    try:
        r = e.run("wave", 3400, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1600, "turn", "4:+1", 10, 20),
                                         (2400, "turn", "3:+2", 28, 20)])
    finally:
        os.environ.pop("RUI_VOICE1", None)
        os.environ.pop("CORE_SAMPLE_RAM_MB", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("wave: RIGHT twice opens voice 1's page", page == [1, 0], page)
    early = texts_of("wave", r, 1.1)
    mid = texts_of("wave", r, 1.55)
    check("wave: DSP SAMPLE's page, the overview loading then shown with its mode (ONE)",
          "DSP SAMPLE" in mid and "ONE" in mid and "LOADING" not in mid and "EMPTY SLOT" not in mid, (early, mid))
    fr = last_frame("wave", r)
    wavepx = sum(px(fr, x, y) for x in range(1, 127) for y in range(11, 25)) if fr else 0
    check("wave: the waveform is drawn", wavepx > 150, wavepx)
    ps = [x["param"] for x in r["events"] if "param" in x]
    sta = [p for p in ps if p[0] == 0 and p[1] == 12]
    check("wave: STA (encoder E) moved +10 in the engine", bool(sta) and sta[-1][3] - sta[0][2] == 10, sta[-3:])
    c = [x for x in ev(r, tx="commit") if 1.6 <= x["t"] < 2.3]
    tiles = [x["tiles"] for x in c]
    check("wave: each STA frame carries few tiles (the marker, the knob, the bottom bar: <= 24)", bool(tiles) and max(tiles) <= 24,
          tiles)
    tx = texts_of("wave", r)
    check("wave: MOD into GRAIN's zone: the GRAIN page (POS SIZE DENS)", all(w in tx for w in ("GRAIN", "POS", "SIZE", "DENS")), tx)
    panel_check("wave", r)
    print(f"    tiles per STA frame {tiles}; screenshot {os.path.join(OUT, 'wave.png')}")
    shot("wave", os.path.join(OUT, "wave.png"))


# SIDECAR -> REC by LEFT: with RUI_REC the ring ends ... LFO, REC, SCOPE, SAMPLE, AUTO, POOL (SAMPLE and AUTO show where REC does;
# the pools' POOL page) [measured: the catch masks after each LEFT: POOL 0F, AUTO FF, SAMPLE 0F, SCOPE 07, REC 01], so REC is the 5th
# page to the left. Presses are short (60 ms); the 5th lands before the first YES.
REC_NAV = [(800 + 90 * i, "press", "LEFT", 60) for i in range(5)]


def t_rec(e):
    os.environ["RUI_REC"] = "1"
    os.environ["RUI_TAP"] = "synth"
    try:
        r = e.run("rec", 4200, ENTER + REC_NAV + [(1300, "press", "YES"),
                                        (3200, "press", "YES"), (3600, "turn", "0:+1", 1, 20)])
    finally:
        os.environ.pop("RUI_REC", None)
        os.environ.pop("RUI_TAP", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("rec: LEFT x5 from SIDECAR opens REC (extra page 1: LFO, REC, SCOPE, SAMPLE, AUTO, POOL) and YES keeps it", page == [3, 1], page)
    masks = ev(r, tx="mask")
    check("rec: its catch mask takes encoders A..D (A arming, B C D playback)", bool(masks) and masks[-1]["encs"] == [0, 1, 2, 3],
          masks[-1:] if masks else None)
    reqs = [x["rec"] for x in r["events"] if "rec" in x]
    check("rec: YES started, YES stopped, A right armed (the platform's requests)", reqs == ["start", "stop", "arm"], reqs)
    mid = texts_of("rec", r, 3.1)
    check("rec: while recording: RECORDING, the take's name, YES: STOP", all(w in mid for w in ("RECORDING", "TAKE-0008", "YES: STOP")), mid)
    fr = None
    c = [x for x in ev(r, tx="commit") if x["t"] < 3.1]
    if c:
        fr = open(os.path.join(OUT, "frames_rec", f"frame_{c[-1]['frame']}.bin"), "rb").read()
    big = sum(px(fr, x, y) for x in range(19, 70) for y in range(12, 26)) if fr else 0
    check("rec: the elapsed time drawn as large digits (0:01)", big > 80, big)
    end = texts_of("rec", r)
    check("rec: armed at the end (ARMED, PLAY:REC)", "ARMED" in end and any("PLAY:REC" in t for t in end), end)
    panel_check("rec", r, live=True)
    print(f"    screenshot {os.path.join(OUT, 'rec.png')}")
    shot("rec", os.path.join(OUT, "rec.png"))


def t_gfx(e):
    os.environ["RUI_VOICE1"] = "0x48:0,64,64,100,64,64,100,127,64,64,64,64,64,64,64,64"
    try:
        r = e.run("gfx", 2600, ENTER + [(800, "press", "RIGHT"), (1000, "press", "RIGHT"), (1500, "turn", "0:+1", 5, 40)])
    finally:
        os.environ.pop("RUI_VOICE1", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    tx = texts_of("gfx", r)
    check("gfx: voice 1's 6-OP FM page (PAT TIM under the algorithm)", page == [1, 0] and all(w in tx for w in ("6-OP FM", "PAT", "TIM")),
          (page, tx))
    before = None
    c = [x for x in ev(r, tx="commit") if x["t"] < 1.5]
    if c:
        before = open(os.path.join(OUT, "frames_gfx", f"frame_{c[-1]['frame']}.bin"), "rb").read()
    after = last_frame("gfx", r)
    region = lambda fb: [px(fb, x, y) for x in range(0, 64) for y in range(10, 26)] if fb else None   # noqa: E731
    check("gfx: PAT turned: another voice, the algorithm diagram redrawn", before and after and region(before) != region(after))
    panel_check("gfx", r)
    print(f"    screenshot {os.path.join(OUT, 'gfx.png')}")
    shot("gfx", os.path.join(OUT, "gfx.png"))


def t_play(e):
    """Playback on the REC page against the recorder model's synthetic takes (RUI_REC)."""
    os.environ["RUI_REC"] = "1"
    os.environ["RUI_TAP"] = "synth"
    try:
        r = e.run("play", 3200, ENTER + REC_NAV + [(1300, "turn", "1:+1", 1, 20),
                                         (1600, "press", "PUSH_B"), (2200, "turn", "3:+1", 3, 20)])
        r2 = e.run("play-rec", 3600, ENTER + REC_NAV + [(1300, "press", "PUSH_B"),
                                             (1800, "turn", "2:+1", 1, 20), (2200, "press", "PUSH_B"),
                                             (2600, "press", "YES"), (3000, "press", "PUSH_B"), (3200, "turn", "1:+1", 1, 20)])
    finally:
        os.environ.pop("RUI_REC", None)
        os.environ.pop("RUI_TAP", None)
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    check("play: on REC (extra page 1) after B / D turns and B pushed (the page's own)", page == [3, 1], page)
    got = [(k, x[k]) for x in r["events"] for k in ("play", "play_ch", "play_level", "play_take") if k in x]
    check("play: B turned picked BD, B pushed started it, D +3 raised the level (the platform's input, summed per report)",
          [g for g in got if g[0] != "play_level"] == [("play_ch", "BD"), ("play", "start")] and got[-1] == ("play_level", 103), got)
    end = texts_of("play", r)
    check("play: while playing: PLAYING, the take, BD 2/3, B STOP and the level and the length",
          all(w in end for w in ("PLAYING", "TAKE-0007", "BD 2/3", "B: STOP  D: LEV 103  1:30")), end)
    fr = last_frame("play", r)
    track = sum(px(fr, x, 28) for x in range(0, 128, 2)) if fr else 0
    mark = sum(px(fr, x, y) for x in range(3, 13) for y in range(12, 25)) if fr else 0
    check("play: the progress track (row 28) and the filled play mark drawn", track >= 60 and mark > 30, (track, mark))
    panel_check("play", r, live=True)
    shot("play", os.path.join(OUT, "play.png"))
    got2 = [(k, x[k]) for x in r2["events"] for k in ("play", "play_take", "rec") if k in x]
    check("play-rec: B played, C took the older take (stopped), B played it, YES recorded and stopped it, B / B turned "
          "during the take did nothing", got2 == [("play", "start"), ("play_take", "take-0006"), ("play", "start"),
                                                  ("rec", "start"), ("play", "stop")], got2)
    end2 = texts_of("play-rec", r2)
    check("play-rec: the recorder's view while the take records (RECORDING, YES: STOP)",
          "RECORDING" in end2 and "YES: STOP" in end2 and "PLAYING" not in end2, end2)
    print(f"    screenshots {os.path.join(OUT, 'play.png')}, {os.path.join(OUT, 'play-rec.png')}")
    shot("play-rec", os.path.join(OUT, "play-rec.png"))
    # C turned quickly through many takes with long names (hardware: "glitchy" while picking a take): every frame from
    # the first turn to 2.5 s after the last is the playback view (none of the recorder's in between), few tiles a turn
    os.environ.update(RUI_REC="1", RUI_TAP="synth", RUI_PLAY_TAKES="40")
    turns = [(1400 + 150 * k, "turn", "2:+1", 1, 20) for k in range(12)]
    try:
        r3 = e.run("play-takes", 4400, ENTER + REC_NAV + turns)
    finally:
        for k in ("RUI_REC", "RUI_TAP", "RUI_PLAY_TAKES"):
            os.environ.pop(k, None)
    tk = [x for x in r3["events"] if "play_take" in x]
    heads, tiles = [], []
    if tk:
        t0, t1 = tk[0]["t"], tk[-1]["t"] + 2.5
        for x in ev(r3, tx="commit"):
            if t0 <= x["t"] <= t1:
                p_ = os.path.join(OUT, "frames_play-takes", f"texts_{x['frame']}.json")
                tx = [i[4] for i in json.load(open(p_))] if os.path.exists(p_) else []
                heads.append("PLAY" if "PLAY" in tx else "IDLE" if "IDLE" in tx else "?")
                tiles.append(x["tiles"])
    check("play-takes: C turned 12 times: each turn a take, the playback view in every frame (no recorder frame between)",
          len(tk) == 12 and heads and all(h == "PLAY" for h in heads) and max(tiles) <= 40,
          (len(tk), "".join(h[0] for h in heads), max(tiles) if tiles else None))
    end3 = texts_of("play-takes", r3)
    check("play-takes: the last take's long name and channel n/N", "SESSION_028" in end3 and any("/13" in t for t in end3), end3[:6])
    panel_check("play-takes", r3, live=True)
    shot("play-takes", os.path.join(OUT, "play-takes.png"))


# Sidecar voice -> its tracks, as the Rytm plays them (0021's 04s; core/ui/remote.cpp kTrackVoice)
VOICE_TRACKS = {0: ["BD"], 1: ["BT"], 2: ["SD"], 3: ["LT"], 4: ["CH", "OH"], 5: ["MT", "HT"], 6: ["CY", "CB"], 7: ["RS", "CP"]}
EXT_DOWNS = {"JUNO": 21, "6-OP FM": 20}   # the machine list from track BD on the base (rytm-mods tools/emu/us_ui.json)


def t_extmach(e):
    """The machine chosen on the Rytm (SRC SRC, the machine list, YES: 0021's EXT machine) and the voice page."""
    for title, downs in EXT_DOWNS.items():
        name = "extmach_" + ("juno" if title == "JUNO" else "fm")
        evs, t = [], 300
        for k in ["SRC", "SRC"] + ["DOWN"] * downs + ["YES"]:   # 500 ms a step: the list takes them all
            evs.append((t, "press", k))
            t += 500
        t_open = t + 300
        t_pad = t_open + 1600
        evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
                (t_open + 800, "press", "RIGHT"), (t_pad, "press", "BD"), (t_pad + 400, "press", "NO"),
                (t_pad + 1200, "press", "RIGHT"), (t_pad + 1400, "press", "RIGHT")]
        os.environ["RUI_FRESH"] = "1"
        try:
            r = e.run(name, t_pad + 2000, evs)
        finally:
            os.environ.pop("RUI_FRESH", None)
        vs = ev(r, rx="voice", voice=0)
        pad_s = (t_pad - 400) / 1000   # the link's clock runs within a few 100 ms of the emulator's
        check(f"{name}: no report from the Rytm until the track sounds (the list's YES sends none)",
              not [x for x in vs if x["t"] < pad_s], vs[:2])
        before = texts_of(name, r, pad_s)
        stl = [x for x in r["events"] if "stats" in x]
        if ev(r, rx="machines"):   # 1025+: the Rytm's 33 names the machine before any trig (sim/rui_e2e.py machines)
            check(f"{name}: before the trig: the Rytm's 33 already gave voice 1's {title} page, no PLAITS guessed",
                  "PLAITS" not in before and title in before, before[:6])
        else:
            check(f"{name}: before the trig: SIDECAR then SYSTEM, no voice page and no PLAITS guessed",
                  "PLAITS" not in before and title not in before, before[:6])
        m = 0x08 if title == "JUNO" else 0x48
        check(f"{name}: the BD pad (passed to the Rytm while the page shows) trigs the track: 0x04, machine {m:#04x}",
              any(x["cmd"] == 4 and x["machine"] == m for x in vs), vs)
        side = texts_of(name, r, (t_pad + 1000) / 1000)
        short = "JUNO" if title == "JUNO" else "FM"
        check(f"{name}: NO: SIDECAR names track 1's machine ({short})", short in side, side[:8])
        page = stl[-1]["stats"][14:16] if stl else None
        tx = texts_of(name, r)
        check(f"{name}: RIGHT twice: voice 1's {title} page", page == [1, 0] and title in tx, (page, tx[:6]))
        panel_check(name, r)
        print(f"    screenshot {os.path.join(OUT, name + '.png')}")
        shot(name, os.path.join(OUT, name + ".png"))


def rates(r):
    """Per status line (20M = 100 ms): the UI renders and the audio interrupt's (vector 174) count, as deltas."""
    ren = [c.get(RENDER, 0) for _, c in r["status"]]
    irq = [int(m) for m in re.findall(r"^\s*\d+M pc=.*?v174:(\d+)", r["out"], re.M)]
    return [b - a for a, b in zip(ren, ren[1:])], [b - a for a, b in zip(irq, irq[1:])]


def t_connect(e):
    """2026-10-19, a hardware freeze: PLAY pressed while the Sidecar was still connecting. The sequencer playing an
    EXT machine (0021's trig reports from the audio interrupt) and pads hammered while the USB host attaches and
    re-attaches N times (rytm-emu --usb-host-gone: VBUS off, a fresh host, a new enumeration, 0026's hello, the splash,
    0033's machine reports, 0030's 27), PLAY / STOP pressed at different points of each enumeration: the Rytm's UI
    keeps its 20 renders a second and the audio interrupt its 1500 blocks a second throughout, no fault, every attach
    says hello; the core silences its voices at each hello (core::link_reset: nothing hangs across a reconnect)."""
    if "--usb-host-gone" not in subprocess.run([os.path.join(EMU, "target/release/dtboot"), "--help"],
                                              capture_output=True, text=True).stdout + subprocess.run(
            [os.path.join(EMU, "target/release/dtboot"), "--help"], capture_output=True, text=True).stderr:
        check("connect: rytm-emu's dtboot has --usb-host-gone", False, "rebuild rytm-emu with the usb-host-gone work")
        return
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:   # BD on JUNO (extmach's list)
        evs.append((t, "press", k))
        t += 500
    os.environ["RUI_FRESH"] = "1"
    try:
        base = e.run("connect-base", t + 1500, evs + [(t + 300, "press", "NO"), (t + 800, "press", "BD")], save=True)
        snap, clock = e.snap, e.clock
        e.snap, e.clock = base["snap"], e.clock + (t + 1500) / 5
        n, gap, ex, ev2 = int(os.environ.get("RUI_CONNECTS", "8")), 1200, [], []
        for k in range(n):
            t0 = 800 + k * gap
            up = t0 + 300 + 37 * k
            ex += ["--usb-host-gone", f"{e.at(t0)}:{e.at(up)}"]
            ev2.append((up + [0, 10, 30, 60, 120, 250, 400, 600][k % 8], "press", "PLAY" if k % 2 == 0 else "STOP"))
            ev2 += [(x, "press", "BD", 20) for x in range(t0 + 100, t0 + 900, 40)]
        ms = 800 + n * gap + 1000
        r = e.run("connect", ms, [(100, "press", "PLAY")] + ev2, extra=ex)
    finally:
        e.snap, e.clock = snap, clock
        os.environ.pop("RUI_FRESH", None)
    check("connect: JUNO on BD reported (0021's trig report from the audio interrupt)",
          any(x["cmd"] == 4 and x["machine"] == 0x08 for x in ev(base, rx="voice")), ev(base, rx="voice")[:2])
    ren, irq = rates(r)
    check(f"connect: {n} attaches while playing: the UI renders 20 a second throughout (2 every 100 ms)",
          ren and min(ren) >= 2, (min(ren) if ren else None, sum(x == 0 for x in ren)))
    check("connect: the audio interrupt keeps its 1500 blocks a second (150 every 100 ms)",
          irq and min(irq) >= 149, (min(irq) if irq else None, max(irq) if irq else None))
    m = re.search(r"enumerations: (\d+)", r["out"])
    w = re.search(r"waits from interrupt level: (\d+)", r["out"])
    check(f"connect: {n + 1} enumerations, no USB MIDI wait from interrupt level",
          m and int(m.group(1)) == n + 1 and w and w.group(1) == "0", (m and m.group(0), w and w.group(0)))
    h = ev(r, rx="0x35")
    lt = open(os.path.join(OUT, "connect.link.txt")).read()
    check("connect: a hello (35) for every re-attach, the core's voices silenced at each (link_reset)",
          len(h) >= n and lt.count("link hello") >= 1, (len(h), lt.count("link hello")))


def t_machines(_e):
    """1025 (33): the Rytm names the tracks' machines itself: JUNO picked in BD's SRC list, no trig -> its page."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"machines: the {tag} base in rytm-emu (rytm-mods-fxin tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t_open = t + 300
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
            (t_open + 800, "press", "RIGHT")]
    t_leave = t_open + 1600
    evs += [(t_leave, "press", "TRK", 300), (t_leave + 100, "press", "YES")]
    t = t_leave + 600
    for k in ["SRC", "SRC"] + ["UP"] * 8 + ["YES"]:   # the list again, up to an analog machine
        evs.append((t, "press", k))
        t += 500
    t_back = t + 300
    evs += [(t_back, "press", "TRK", 300), (t_back + 100, "press", "YES"), (t_back + 1000, "press", "RIGHT"),
            (t_back + 1200, "press", "RIGHT")]
    os.environ["RUI_FRESH"] = "1"
    try:
        r = e.run("machines", t_back + 1800, evs)
    finally:
        os.environ.pop("RUI_FRESH", None)
    m = ev(r, rx="machines")
    snaps = [x for x in m if len(x["tracks"]) == 12]
    bd = [tuple(x["tracks"][0]) for x in m if x["t0"] == 0]
    check("machines: the Rytm's 33s: snapshots (hello, page open), BD JUNO (41, 0x08) on the way, BD analog (vb 0x7F) last",
          len(snaps) >= 2 and (41, 0x08) in bd and bd[-1][1] == 0x7F and bd[-1][0] < 35, (len(snaps), bd[-4:]))
    vs = ev(r, rx="voice")
    check("machines: no voice report at all (nothing sounded: the page comes from 33 alone)", not vs, vs[:2])
    on = texts_of("machines", r, (t_leave - 200) / 1000)
    check("machines: TRK + YES, RIGHT twice: voice 1's JUNO page without a trig", "JUNO" in on and "PAT" in on, on[:6])
    side = texts_of("machines", r, (t_back + 700) / 1000)
    check("machines: BD back on an analog machine: SIDECAR names no machine for track 1", "JUNO" not in side and
          "SIDECAR" in side, side[:8])
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    end = texts_of("machines", r)
    check("machines: RIGHT twice now passes no voice page (SCOPE), the JUNO page gone", page and page[0] != 1 and
          "JUNO" not in end, (page, end[:3]))
    panel_check("machines", r, live=True)
    shot("machines", os.path.join(OUT, "machines.png"))


def t_sdvoice(_e):
    """JUNO on SD (track 2), whose voice is not 0026's 33 table's: the Rytm plays SD on voice 2 (0021's 04 carries
    0x0A), 0026's 33 says 0x09 (voice 1). The Sidecar takes the voice from the 04s' table (core/ui/remote.cpp
    kTrackVoice): SIDECAR names track 2 JUNO, the voice page is V3's, and an edit there goes to voice 2."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"sdvoice: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [(300, "press", "SD")], 900   # SD selected by its pad (rytm-mods tools/emu/us_ui.json downs_sd: U1 18)
    for k in ["SRC", "SRC"] + ["DOWN"] * (18 + 6) + ["YES"]:   # U1..U5 18..22, 6-OP FM 23, JUNO 24
        evs.append((t, "press", k))
        t += 500
    t_pad = t + 300
    t_open = t_pad + 800
    evs += [(t_pad, "press", "SD"), (t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"),
            (t_open + 600, "press", "RIGHT"), (t_open + 800, "press", "RIGHT")]
    os.environ["RUI_FRESH"] = "1"
    try:
        r = e.run("sdvoice", t_open + 1600, evs)
    finally:
        os.environ.pop("RUI_FRESH", None)
    sd = [tuple(x["tracks"][0]) for x in ev(r, rx="machines") if x["t0"] == 1 and len(x["tracks"]) == 1]
    check("sdvoice: SD's 33 on the way: JUNO (41) with 0026's vb 0x09 (voice 1)", (41, 0x09) in sd, sd[-4:])
    vs = [x for x in ev(r, rx="voice") if x["cmd"] == 4 and x["machine"] == 0x08]
    check("sdvoice: SD's pad: the Rytm's 04 for JUNO is voice 2's (0x0A), not 33's voice 1",
          vs and all(x["voice"] == 2 for x in vs), [(x["voice"], x["machine"]) for x in vs][:3])
    st = [x for x in r["events"] if "stats" in x]
    page = st[-1]["stats"][14:16] if st else None
    tx = texts_of("sdvoice", r)
    check("sdvoice: TRK + YES, RIGHT twice: voice 2 (index; SD plays it) JUNO page, named SD", page == [1, 2] and "JUNO" in tx and "SD" in tx,
          (page, tx[:6]))
    panel_check("sdvoice", r, live=True)
    shot("sdvoice", os.path.join(OUT, "sdvoice.png"))


def t_noexit(e):
    """NO on SIDECAR leaves the page (3A 0 0), as TRK + YES: TRK + YES, RIGHT, NO -> SIDECAR, NO -> the stock screen."""
    ref = e.run("stock", 2600, [], link=False)
    r = e.run("noexit", 2600, ENTER + [(900, "press", "RIGHT"), (1100, "press", "NO"), (1400, "press", "NO")])
    side = texts_of("noexit", r, 1.35)
    check("noexit: RIGHT, NO: back on SIDECAR", "SIDECAR" in side, side[:4])
    m = ev(r, rx="mode")
    check("noexit: NO on SIDECAR: the Rytm leaves the page (30 off)", len(m) == 2 and m[1]["on"] == 0, m)
    late = [x for x in ev(r, tx="commit") if len(m) == 2 and x["t"] > m[1]["t"]]
    check("noexit: nothing drawn after leaving", not late, len(late))
    check("noexit: the stock screen is back, equal to a Rytm that never opened the page",
          r["oled"] is not None and r["oled"] == ref["oled"], f"{diff_bytes(r['oled'], ref['oled'])} bytes differ")


def t_srcgfx(_e):
    """The real path to the machine graphics (rui1025's 33): JUNO picked in BD's SRC list (no trig) -> 33 -> the page
    list -> RIGHT twice: the JUNO page with its envelope and patch; then 6-OP FM in the same list -> its algorithm."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcgfx: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t_open = t + 300
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
            (t_open + 800, "press", "RIGHT")]
    t_leave = t_open + 1600
    evs += [(t_leave, "press", "TRK", 300), (t_leave + 100, "press", "YES")]
    t = t_leave + 600
    for k in ["SRC", "SRC", "UP", "YES"]:   # the list's cursor on JUNO (41): one up is 6-OP FM (40)
        evs.append((t, "press", k))
        t += 500
    t_fm = t + 300
    evs += [(t_fm, "press", "TRK", 300), (t_fm + 100, "press", "YES"), (t_fm + 600, "press", "RIGHT"),
            (t_fm + 800, "press", "RIGHT")]
    r = e.run("srcgfx", t_fm + 1600, evs)   # voice 1 reported on PLAITS first (as on a Sidecar that heard a trig)
    bd = [tuple(x["tracks"][0]) for x in ev(r, rx="machines") if x["t0"] == 0]
    check("srcgfx: BD's 33s: JUNO (41, 0x08), then 6-OP FM (40, 0x48)", (41, 0x08) in bd and bd[-1] == (40, 0x48), bd[-4:])

    def frame_before(sec):
        c = [x for x in ev(r, tx="commit") if x["t"] < sec]
        return open(os.path.join(OUT, "frames_srcgfx", f"frame_{c[-1]['frame']}.bin"), "rb").read() if c else None

    ink = lambda fb, x0, x1, y0, y1: sum(px(fb, x, y) for x in range(x0, x1) for y in range(y0, y1)) if fb else 0  # noqa: E731
    on = texts_of("srcgfx", r, (t_leave - 200) / 1000)
    fj = frame_before((t_leave - 200) / 1000)
    check("srcgfx: JUNO: TRK + YES, RIGHT twice: voice 1's JUNO page (PAT CUT ... LEN)", "JUNO" in on and "LEN" in on, on[:6])
    check("srcgfx: JUNO: the envelope drawn (cell row 2, columns 1..3) and the patch track (cell 1)",
          ink(fj, 2, 94, 32, 48) > 60 and ink(fj, 2, 30, 20, 26) > 6, (ink(fj, 2, 94, 32, 48), ink(fj, 2, 30, 20, 26)))
    if fj:
        frame_png(os.path.join(OUT, "srcgfx_juno.png"), fj)
    end = texts_of("srcgfx", r)
    ff = last_frame("srcgfx", r)
    check("srcgfx: 6-OP FM: the list again, one up, YES: voice 1's 6-OP FM page", "6-OP FM" in end and "TIM" in end, end[:6])
    check("srcgfx: 6-OP FM: the algorithm diagram drawn (the two-column cell)", ink(ff, 1, 63, 11, 30) > 40,
          ink(ff, 1, 63, 11, 30))
    panel_check("srcgfx", r)
    shot("srcgfx-juno", os.path.join(OUT, "srcgfx_juno.png"))
    shot("srcgfx-fm", os.path.join(OUT, "srcgfx.png"))


def t_srcwave(_e):
    """The real path to DSP SAMPLE's waveform and GRAIN (rui1025's 33, no voice forced): DSP SAMPLE picked in BD's SRC
    list, the BD pad (its report: the voice on DSP SAMPLE), TRK + YES, RIGHT twice -> its page (SMP OFF, the kit's), SMP
    (C) to pool slot 1 -> the sample's waveform; MOD (D) turned on the page into GRAIN's zone -> the GRAIN view."""
    tag = os.environ.get("RUI_TAG_1025", "rui1025")
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * (EXT_DOWNS["JUNO"] + 3) + ["YES"]:   # JUNO 41, 42, 44, DSP SAMPLE 45
        evs.append((t, "press", k))
        t += 500
    evs += [(t + 400, "press", "BD")]
    t_open = t + 1000
    t_mod = t_open + 1600
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 600, "press", "RIGHT"),
            (t_open + 800, "press", "RIGHT"), (t_open + 1100, "turn", "2:+1", 1, 20), (t_mod, "turn", "3:+2", 28, 20)]
    os.environ.update(RUI_SAMPLES="1", CORE_SAMPLE_RAM_MB="16")
    try:
        r = e.run("srcwave", t_mod + 1800, evs)
    finally:
        os.environ.pop("RUI_SAMPLES", None)
        os.environ.pop("CORE_SAMPLE_RAM_MB", None)
    bd = [tuple(x["tracks"][0]) for x in ev(r, rx="machines") if x["t0"] == 0]
    check("srcwave: BD's 33: DSP SAMPLE (45, 0x20)", bool(bd) and bd[-1] == (45, 0x20), bd[-3:])
    vs = ev(r, rx="voice", voice=0)
    check("srcwave: the BD pad trigs the track: 0x04 machine 0x20", any(x["cmd"] == 4 and x["machine"] == 0x20 for x in vs), vs[:3])
    mid = texts_of("srcwave", r, (t_mod - 100) / 1000)
    c = [x for x in ev(r, tx="commit") if x["t"] < (t_mod - 100) / 1000]
    fr = open(os.path.join(OUT, "frames_srcwave", f"frame_{c[-1]['frame']}.bin"), "rb").read() if c else None
    wavepx = sum(px(fr, x, y) for x in range(1, 127) for y in range(11, 25)) if fr else 0
    check("srcwave: DSP SAMPLE's page with the pool sample's waveform", "DSP SAMPLE" in mid and "SMP OFF" not in mid and
          wavepx > 150, (mid[:8], wavepx))
    if fr:
        frame_png(os.path.join(OUT, "srcwave_dsps.png"), fr)
    end = texts_of("srcwave", r)
    check("srcwave: MOD into GRAIN's zone: the GRAIN view (POS SIZE DENS)", all(w in end for w in ("GRAIN", "POS", "SIZE", "DENS")),
          end[:8])
    panel_check("srcwave", r)
    shot("srcwave-dsps", os.path.join(OUT, "srcwave_dsps.png"))
    shot("srcwave-grain", os.path.join(OUT, "srcwave.png"))


def t_srcsmp(_e):
    """1027, the SRC view on DSP SAMPLE (rui1027): DSP SAMPLE picked in BD's SRC list, the stock SMPL page, SMP (D: TUN FIN BR SMP)
    turned to pool slot 1 there, then SRC: the view draws that sample's waveform (the Rytm's P11 from 2A, the overview
    read while the view shows), never SMP OFF; the voice never sounded (no trig)."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcsmp: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * (EXT_DOWNS["JUNO"] + 3) + ["YES"]:   # JUNO 41, 42, 44, DSP SAMPLE 45
        evs.append((t, "press", k))
        t += 500
    t_smp = t + 500
    # an enum steps by one with 6 counts of +8 (rytm-mods' test); SRC at once: the 29 t 1's 2A may carry the old SMP, the
    # changed span follows when the kit word lands (~0.5-1.3 s)
    evs += [(t_smp, "press", "SMPL"), (t_smp + 600, "turn", "3:+8", 6, 50), (t_smp + 1500, "press", "SRC")]
    t_end = t_smp + 4500
    os.environ.update(RUI_SAMPLES="1", CORE_SAMPLE_RAM_MB="16", RUI_FRESH="1")
    try:
        r = e.run("srcsmp", t_end, evs)
    finally:
        for k in ("RUI_SAMPLES", "CORE_SAMPLE_RAM_MB", "RUI_FRESH"):
            os.environ.pop(k, None)
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0]
    smp = [x["values"][10 - x["first"]] for x in vals if x["first"] <= 10 < x["first"] + len(x["values"])]
    check("srcsmp: the Rytm's 2A carries SMP (P11) = slot 1 by the end", smp and int(smp[-1]) == 1, smp[-4:])
    end = texts_of("srcsmp", r)
    fr = last_frame("srcsmp", r)
    wavepx = sum(px(fr, x, y) for x in range(1, 101) for y in range(11, 25)) if fr else 0
    check("srcsmp: SRC: the view's DSP SAMPLE page with the sample's waveform, not SMP OFF", "DSP SAMPLE" in end and
          "SMP OFF" not in end and "LOADING" not in end and wavepx > 150, (end[:8], wavepx))
    check("srcsmp: no voice report (nothing sounded: the view reads the Rytm's values)", not ev(r, rx="voice"), "")
    if fr:
        frame_png(os.path.join(OUT, "srcsmp.png"), fr)
    shot("srcsmp", os.path.join(OUT, "srcsmp.png"))


def t_srcreplug(_e):
    """1027, the SRC view across a replug (rytm-emu --usb-host-gone, rui1027): BD on JUNO, its SRC page shows the view;
    the USB host goes and comes back (a) while the SRC page shows, (b) while TRIG shows, then SRC: after each the
    Rytm's hello, its 29 0 1, the core's whole frame, and the panel showing the core's frame (not stock SRC)."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcreplug: the {tag} base in rytm-emu", False)
        return
    for case in ("onsrc", "ontrig"):
        e = Emu(tag)
        evs, t = [], 300
        for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
            evs.append((t, "press", k))
            t += 500
        t_gone = t + 1500
        if case == "ontrig":
            evs.append((t_gone - 600, "press", "TRIG"))
        t_up = t_gone + int(os.environ.get("SRCREPLUG_GONE_MS", "3000"))   # past the Rytm's 1 s timeout, as a hand replug
        if case == "ontrig":
            evs.append((t_up + 4500, "press", "SRC"))
        t_end = t_up + 7000
        name = f"srcreplug-{case}"
        r = e.run(name, t_end, evs, extra=["--usb-host-gone", f"{e.at(t_gone)}:{e.at(t_up)}"])
        hel = [x["t"] for x in ev(r, rx="0x35")]
        # the link's clock (Overbridge frames) stands while the host is gone: "after" = from the last hello on
        on = [x["t"] for x in ev(r, rx="srcview") if x["on"] == 1 and hel and x["t"] >= hel[-1]]
        com = [x["t"] for x in ev(r, tx="commit") if on and x["t"] > on[-1]]
        print(f"    {name}: hellos {[round(h, 2) for h in hel]}, 29 on after {[round(x, 2) for x in on[:4]]}, "
              f"commits after the last {len(com)}", flush=True)
        check(f"{name}: a hello after the replug, then the Rytm's 29 0 1", hel and on, (hel, on[:3]))
        check(f"{name}: the core answers with frames", bool(com), len(com))
        end = texts_of(name, r)
        check(f"{name}: the core's last frame is BD's JUNO view", "JUNO" in end, end[:4])
        panel_check(name, r)
        shot(name, os.path.join(OUT, f"{name}.png"))


def t_srcview(_e):
    """1027, the SRC view (rui1027's base, RUI_TAG_1027): JUNO picked in BD's SRC list, the stock SRC page of BD stays:
    the Rytm says 29 0 1 and sends BD's values (2A), the core answers with a whole frame of BD's JUNO page drawn from
    them (no 37, no 3F, no 29 back); CUT (encoder B) turned on the Rytm: its 2As, the bottom bar names CUT, the engine's voice the Rytm's (the
    page only reads); TRK + YES opens the remote page (it comes first), leaving it shows the view again; the list up to
    an analog machine: the Rytm's 29 0 0, the core draws nothing more."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcview: the {tag} base in rytm-emu (rytm-mods-1027 tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t_turn = t + 2500
    evs.append((t_turn, "turn", "1:+8", 6, 50))   # CUT up on the Rytm's own SRC page (stock: ~3 counts a step)
    t_open = t_turn + 1200
    evs += [(t_open, "press", "TRK", 300), (t_open + 100, "press", "YES"), (t_open + 500, "press", "RIGHT"),
            (t_open + 700, "press", "RIGHT")]   # SIDECAR, SYSTEM, then no voice page (the SRC view has them)
    t_leave = t_open + 1200
    evs += [(t_leave, "press", "TRK", 300), (t_leave + 100, "press", "YES")]
    t_back = t_leave + 1000
    t = t_back
    for k in ["SRC", "SRC"] + ["UP"] * 8 + ["YES"]:   # the list again (rytm-mods mrep list_open), up to an analog machine
        evs.append((t, "press", k))
        t += 500
    t_end = t + 1500
    os.environ["RUI_FRESH"] = "1"
    try:
        r = e.run("srcview", t_end, evs)
    finally:
        os.environ.pop("RUI_FRESH", None)
    views = ev(r, rx="srcview")
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0]
    check("srcview: the Rytm's 29 0 1 (BD's SRC page on JUNO) and BD's values (2A)",
          any(x["track"] == 0 and x["on"] == 1 for x in views) and vals, (views[:4], len(vals)))
    on1 = [x["t"] for x in views if x["track"] == 0 and x["on"] == 1]
    commits = ev(r, tx="commit")
    check("srcview: the core answers BD's 29 0 1 with a whole frame (a commit after it), never a 29 back",
          on1 and any(x["t"] > on1[0] for x in commits) and not ev(r, tx="0x29"), (on1[:2], len(commits)))
    masks = [x for x in ev(r, tx="mask") if x["t"] < t_open / 1000]
    check("srcview: no catch mask in the view (every key and knob the Rytm's)", not masks, masks[:2])
    view = texts_of("srcview", r, (t_open - 100) / 1000)
    check("srcview: BD's JUNO page drawn in the view, the bottom bar names CUT", "JUNO" in view and
          any(str(x).startswith("BD CUT:") for x in view), view[:10])
    one = [x for x in vals if len(x["values"]) == 1 and x["first"] == 1]
    check("srcview: the turn came as 2A n 1 for P2 (CUT), above its 64", one and one[-1]["values"][0] > 64,
          [x["values"] for x in one])
    pg = texts_of("srcview", r, (t_leave - 50) / 1000)
    check("srcview: TRK + YES, RIGHT twice: not BD's JUNO page (no voice pages once the Rytm has the SRC view)",
          pg and "PAT" not in pg and "CUT" not in pg, pg[:6])
    back = texts_of("srcview", r, (t_back - 100) / 1000)
    check("srcview: after TRK + YES left: the view again", "JUNO" in back, back[:6])
    off = [x["t"] for x in views if x["track"] == 0 and x["on"] == 0]
    late = [x for x in commits if off and x["t"] > off[-1] + 0.1]
    check("srcview: BD analog: the Rytm's 29 0 0, nothing drawn after it", off and not late, (off[-1:], len(late)))
    shot("srcview", os.path.join(OUT, "srcview.png"))


def t_srcturn(_e):
    """1027, the SRC view's responsiveness (rui1027): BD on JUNO, its stock SRC page, CUT (encoder B) turned for 2 s
    (+8 a count every 20 ms, then -8): per 2A the time from the turn that caused it (the Rytm's part), per 2A the time
    to the core's commit (the host's part), the tiles and bytes a frame. Prints the numbers; checks the host's part."""
    tag = os.environ.get("RUI_TAG_1027", "rui1027")
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcturn: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * EXT_DOWNS["JUNO"] + ["YES"]:
        evs.append((t, "press", k))
        t += 500
    t0 = t + 2500
    n = int(os.environ.get("SRCTURN_N", "50"))
    evs += [(t0, "turn", "1:+8", n, 20), (t0 + 20 * n + 500, "turn", "1:-8", n, 20)]
    r = e.run("srcturn", t0 + 40 * n + 2000, evs)
    turns = [(t0 + 20 * k) / 1000 for k in range(n)] + [(t0 + 20 * n + 500 + 20 * k) / 1000 for k in range(n)]
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0 and x["t"] >= t0 / 1000]
    commits = [x for x in ev(r, tx="commit") if x["t"] >= t0 / 1000]
    lat_r, lat_h, prev = [], [], None
    for x in vals:   # the Rytm: from the first turn since the previous 2A
        since = [tt for tt in turns if tt <= x["t"] and (prev is None or tt > prev)]
        if since:
            lat_r.append(1000 * (x["t"] - since[0]))
        nxt = [c for c in commits if c["t"] >= x["t"]]
        if nxt:
            lat_h.append(1000 * (nxt[0]["t"] - x["t"]))
        prev = x["t"]
    gaps = [1000 * (b["t"] - a["t"]) for a, b in zip(vals, vals[1:])]
    link = open(os.path.join(OUT, "srcturn.link.txt")).read() if os.path.exists(os.path.join(OUT, "srcturn.link.txt")) else ""
    tiles = [c["tiles"] for c in commits]
    med = lambda v: sorted(v)[len(v) // 2] if v else None  # noqa: E731
    print(f"  srcturn: {len(turns)} turns, {len(vals)} 2As (gap median {med(gaps)} ms, max {max(gaps) if gaps else None}),"
          f" {len(commits)} commits; turn->2A median {med(lat_r)} ms max {max(lat_r) if lat_r else None};"
          f" 2A->commit median {med(lat_h)} ms max {max(lat_h) if lat_h else None}; tiles/commit median {med(tiles)}"
          f" max {max(tiles) if tiles else None}", flush=True)
    nb = [sum(12 + a[2] * a[3] * 8 + (a[2] * a[3] * 8 + 6) // 7 for a in c["rects"]) + 9 for c in commits]   # 38s + 39
    print(f"  srcturn: bytes/commit (38s + 39, without 3D) median {med(nb)} max {max(nb) if nb else None}", flush=True)
    check("srcturn: a 2A every Rytm UI tick while the knob turns (gap median <= 60 ms)", gaps and med(gaps) <= 60,
          (len(vals), med(gaps)))
    check("srcturn: the core commits within 10 ms of each 2A (the input path)", lat_h and max(lat_h) <= 10, lat_h[:8])
    check("srcturn: a turn's frame is a few tiles, not the whole page", tiles and med(tiles) <= 24, tiles[:10])


def lfo_des(d):
    """The LFO JSON's design hex -> [(value, shape)] x 16."""
    return [(int(d[3 * k:3 * k + 2], 16) - 64, int(d[3 * k + 2], 16)) for k in range(16)]


def t_lfo(e):
    # L1 on FX1's P2 (a square at 0.02 Hz), L2 a fast sine on FX2's P3, L3 on voice 1's HRM (the engine's own); 1026: no
    # Rytm targets, so not a single CC goes to the Rytm
    os.environ["RUI_LFO"] = ";".join(["lfo=1:tgt:fx1", "lfo=1:par:0", "lfo=1:wav:4", "lfo=1:rate:0", "lfo=1:dep:40",
                                      "lfo=2:tgt:fx2", "lfo=2:par:1", "lfo=2:wav:0", "lfo=2:rate:92", "lfo=2:dep:50",
                                      "lfo=3:tgt:v1", "lfo=3:par:1", "lfo=3:dep:20", "lfo=4:tgt:bd"])
    try:
        # ux1027: the LFO page is off the ring - UP + LFO opens it (LEFT x 2 from SIDECAR did before)
        r = e.run("lfo", 3600, ENTER + [(800, "press", "UP", 150), (840, "press", "LFO"), (1400, "turn", "3:+1", 3, 20),
                                        (1800, "press", "YES"), (2100, "press", "T5"), (2400, "turn", "1:-1", 5, 20),
                                        (2800, "press", "NO")])
    finally:
        os.environ.pop("RUI_LFO", None)
    masks = ev(r, tx="mask")
    main = [m for m in masks if m["encs"] == list(range(8))]
    des = [m for m in masks if m["encs"] == [0, 1, 2, 3] and all(k in m["keys"] for k in range(16))]
    check("lfo: UP + LFO opens the LFO page: its mask takes all eight encoders, no trig key",
          bool(main) and not any(k in main[0]["keys"] for k in range(16)), main[:1])
    check("lfo: YES opens the design view: A..D and the trig keys T1..T16 caught; NO back to the main view's mask",
          bool(des) and masks[-1]["encs"] == list(range(8)) and not any(k in masks[-1]["keys"] for k in range(16)),
          [m["encs"] for m in masks])
    lj = [x["lfo"] for x in r["events"] if "lfo" in x and x["lfo"]]
    l = lj[-1]["l"] if lj else None
    check("lfo: encoder D on the page set L1's depth (40 + 3)", bool(l) and l[0]["dep"] == 43, l[0] if l else None)
    check("lfo: L4 on BD refused (no Rytm targets): OFF", bool(l) and l[3]["tgt"] == "OFF", l[3] if l else None)
    d = lfo_des(l[0]["des"]) if l else None
    check("lfo: T5 picked step 5, encoder B lowered its value by 5 (the triangle's 0 -> -5)", bool(d) and d[4][0] == -5,
          d[:6] if d else None)
    cc = ev(r, tx="cc")
    check("lfo: not a single CC to the Rytm", not cc, cc[:3])
    sx = [x for x in r["events"] if x.get("tx") == "commit"]
    check("lfo: the page keeps the link light (the dot every 50 ms: < 400 tiles a second)",
          sum(x["tiles"] for x in sx if 1.2 <= x["t"] < 1.8) / 0.6 < 400, sum(x["tiles"] for x in sx if 1.2 <= x["t"] < 1.8))
    panel_check("lfo", r, live=True)
    shot("lfo-page", os.path.join(OUT, "lfo.png"))


def t_upamp(e):
    # rui1047 (rytm-mods RUI_TAG = rui1047): UP + AMP -> 30 page 3; the real core opens the INSERT page on its OUT view
    r = e.run("upamp", 1400, [(560, "press", "UP", 150), (600, "press", "AMP")])
    m = ev(r, rx="mode")
    check("upamp: UP + AMP opens the page (30 page 3, on, reason user)",
          bool(m) and m[0]["on"] == 1 and m[0]["page"] == 3 and m[0]["reason"] == "user", m[:1])
    tx = last_texts("upamp", r)
    check("upamp: the core's frame is the INSERT page's OUT view (the title text INSERT OUT, A OUT, B LEV, C PAN)",
          all(any(t[4] == w for t in tx) for w in ("INSERT OUT", "OUT", "LEV", "PAN")), [t[4] for t in tx][:10])
    panel_check("upamp", r)


def t_upmapover(e):
    # rui1050 (rytm-mods RUI_TAG = rui1050): UP held alone with the page open (SIDECAR) - the Rytm's 49 1 5 1, the real
    # core shows its KEYS page as UP's map over it, and at the release (49 1 5 0) SIDECAR again; UP tapped: no map
    def frames_texts(name, res):
        out = []
        for c in ev(res, tx="commit"):
            p = os.path.join(OUT, f"frames_{name}", f"texts_{c['frame']}.json")
            out.append([t[4] for t in json.load(open(p))] if os.path.exists(p) else [])
        return out
    r = e.run("upmapover", 2200, ENTER + [(800, "press", "UP", 700)])   # (ms: the map at 0.4 s held)
    ft = frames_texts("upmapover", r)
    mapped = [i for i, tx in enumerate(ft) if "UP + KEY" in tx]
    check("upmapover: UP held over the open SIDECAR page: the core's map (UP + KEY) on the glass", bool(mapped),
          [tx[:3] for tx in ft][-6:])
    check("upmapover: after the release the SIDECAR page again (the page stays open)", bool(ft) and "SIDECAR" in ft[-1] and
          bool(mapped) and mapped[-1] < len(ft) - 1, ft[-1][:4] if ft else None)
    panel_check("upmapover", r)
    t = e.run("upmaptap", 2200, ENTER + [(800, "press", "UP", 100)])
    check("upmapover: UP tapped: no map", not any("UP + KEY" in tx for tx in frames_texts("upmaptap", t)))


# ---- rui1048 (rytm-mods 0026 1048; RUI_TAG = rui1048): POLY CONFIG, the Rytm's track menu item (FUNC + CHRO, the last of
# TRIGS SETUP, PAD SCALE, RELOAD TRACK, SAVE TRACK, U1..U5, POLY CONFIG), and E0's make prompt on every Sidecar machine
# MD: 0021's machine 42 (DIGIDRUM's place and name on the list until the MD flash; the prompt says MD either way)
POLY_DOWNS = {"PLAITS": 23, "JUNO": 21, "MD": 22, "DSP SAMPLE": 24, "DAISYSP": 29, "WAVETABLE": 25, "GRANULAR": 26,
              "LIVE IN": 27, "CHOP": 28, "MULTISAMPLE": 30, "6-OP FM": 20}   # the machine list from BD PLASTIC (rytm-mods
                                                                             # tools/emu/us_ui.json; rui1051: LIVE IN, CHOP)


def to_machine(name, t=1500):
    """BD to that machine (SRC SRC, DOWN x n, YES; after the hello's splash): the events and the time after them."""
    out = []
    for k in ["SRC", "SRC"] + ["DOWN"] * POLY_DOWNS[name] + ["YES"]:
        out.append((t, "press", k))
        t += 400
    return out, t + 600


def t_polycfg(e):
    """FUNC + CHRO, DOWN x 9, YES on BD (PLAITS, no poly track): the Rytm's 30 page 0x20, the core's POLY CONFIG page
    (PLAITS: NOT A POLY TRACK, YES: MAKE IT); YES makes it a poly track (4 NOTES 4 INST 2.92%: AUTO on PLAITS); knob A
    turned up three times: VOICES 3 (3 NOTES 3 INST); NO: the page closes (the Rytm's 30 off), back to the stock screen;
    the panel equals the core's frame while it shows."""
    evs, t = to_machine("PLAITS")
    evs += [(t, "press", "FUNC", 300), (t + 50, "press", "CHRO")] + [(t + 600 + 200 * i, "press", "DOWN") for i in range(9)]
    t_yes = t + 600 + 1800 + 200
    t_make = t_yes + 1000
    t_turn = t_make + 1000
    t_no = t_turn + 1200
    evs += [(t_yes, "press", "YES"), (t_make, "press", "YES"), (t_turn, "turn", "0:+1", 3, 150)]
    r = e.run("polycfg", t_no - 300, evs)
    m = [x for x in ev(r, rx="mode") if x["on"]]
    print(f"  30s: {[(x['page'], x['on'], x['reason']) for x in ev(r, rx='mode')]}")
    check("polycfg: the track menu's POLY CONFIG -> 30 page 0x20 (BD), on, user", bool(m) and m[0]["page"] == 0x20 and
          m[0]["reason"] == "user", m[:1])
    off = (m[0]["t"] - t_yes / 1000 - 0.03) if m else 0
    first = texts_of("polycfg", r, (t_yes + 600) / 1000 + off)
    check("polycfg: the core's POLY CONFIG page on BD: PLAITS, not a poly track yet, YES makes it one",
          "POLY CONFIG" in first and "PLAITS: NOT A POLY TRACK" in first and "YES: MAKE IT A POLY TRACK" in first, first)
    made = texts_of("polycfg", r, (t_make + 600) / 1000 + off)
    check("polycfg: YES: a poly track, AUTO on PLAITS (4 NOTES 4 INST 2.92%)", "4 NOTES 4 INST 2.92%" in made, made)
    end = texts_of("polycfg", r)
    check("polycfg: knob A x 3: VOICES 3 (3 NOTES 3 INST 2.19%), the footer BD VOICES:3",
          "3 NOTES 3 INST 2.19%" in end and "BD VOICES:3" in end, end)
    panel_check("polycfg", r)
    shot("polycfg", os.path.join(OUT, "polycfg.png"))
    r = e.run("polycfg-no", t_no + 1200, evs + [(t_no, "press", "NO")])
    mm = [(x["page"], x["on"]) for x in ev(r, rx="mode")]
    check("polycfg: NO: the page closes (the Rytm's 30 off after the core's 3A 0)", mm and mm[-1][1] == 0, mm)
    tx = [t[4] for t in last_texts("polycfg-no", r)]
    check("polycfg: then BD's SRC page again (the core's SRC view: PLAITS, BD a poly track now), not POLY CONFIG",
          "PLAITS" in tx and "POLY CONFIG" not in tx, tx[:6])


def t_polymake(e):
    """E0's make prompt (UP + TRIG: the SEQ page of a track that is no poly track) on each Sidecar machine kind on BD:
    MAKE BD A POLY TRACK?, the machine and its notes at once (AUTO), FUNC+CHRO: POLY CONFIG; BD on its analog machine
    (the base's BD PLASTIC): BD PLAYS ANALOG, THE RYTM'S VOICE IS MONO, no make. POLYMAKE=PLAITS,JUNO limits the kinds."""
    kinds = [k for k in POLY_DOWNS if not os.environ.get("POLYMAKE") or k in os.environ["POLYMAKE"].split(",")]
    for k in kinds:
        evs, t = to_machine(k)
        n = "polymake-" + k.replace(" ", "").replace("-", "").lower()
        r = e.run(n, t + 1500, evs + [(t, "press", "UP", 300), (t + 150, "press", "TRIG")])
        m = [(x["page"], x["on"]) for x in ev(r, rx="mode")]
        tx = texts_of(n, r)
        print(f"  {k}: 30s {m}; {tx[:8]}")
        check(f"polymake {k}: UP + TRIG -> the make prompt (MAKE BD A POLY TRACK?, {k}, FUNC+CHRO: POLY CONFIG)",
              (0x10, 1) in m and "MAKE BD A POLY TRACK?" in tx and any(x.startswith(k + ", ") for x in tx) and
              "FUNC+CHRO: POLY CONFIG" in tx, (m, tx))
        panel_check(n, r)
    os.environ["RUI_FRESH"] = "1"   # (rui_link otherwise reports BD's voice on PLAITS)
    try:
        r = e.run("polymake-analog", 4000, [(1500, "press", "NO"), (2500, "press", "UP", 300), (2650, "press", "TRIG")])
    finally:
        os.environ.pop("RUI_FRESH", None)
    tx = texts_of("polymake-analog", r)
    check("polymake analog: BD PLASTIC -> BD PLAYS ANALOG:, THE RYTM'S VOICE IS MONO, no make",
          "BD PLAYS ANALOG:" in tx and "THE RYTM'S VOICE IS MONO" in tx and not any(x.startswith("MAKE") for x in tx), tx)


# ---- the pools (docs/superpowers/specs/2026-10-22-pools-tonverk-design.md 3.5, 3.6, 5, 7 "Rytm"; rui1043's base,
# RUI_TAG_1043): the machines' lists are the project's SAMPLES / WAVETABLES / INSTRUMENTS pools, 8 banks x 128, row 0 OFF
POOL_DOWNS = dict(gr=26, wt=25, ms=30)   # the machine list from BD PLASTIC (rytm-mods tools/emu/us_ui.json; bk_emu.py)
POOL_P1 = dict(gr=10, wt=1, ms=0)        # the P1 defaults (0026 1043: GRANULAR SRC 10, WAVETABLE TBL 1, MULTISAMPLE 0)
POOL_LIST = dict(gr=0, wt=1, ms=2)       # 47's list byte = the pool list (pools.h kSamples kTables kInstr)
POOL_FILE = dict(gr="samples.pool.txt", wt="wavetables.pool.txt", ms="instruments.pool.txt")
POOL_KIND = dict(gr="gr", wt="wt", ms="ms")   # browse=<kind>
# the bank A row FUNC + YES sets: A005 (spec 7), but WAVETABLES' template holds the six built-in tables in A001..A006
# (A005 = FM: refused, BUILT-IN), so WAVETABLE uses its first empty row A007
POOL_ROW = dict(gr=5, wt=7, ms=5)


def pool_tag():
    return os.environ.get("RUI_TAG_1043", "rui1043")


def pool_wav(path, period, n=9600):
    import struct
    import wave
    os.makedirs(os.path.dirname(path), exist_ok=True)
    w = wave.open(path, "wb")
    w.setnchannels(1), w.setsampwidth(2), w.setframerate(48000)
    w.writeframes(b"".join(struct.pack("<h", int(8000 * ((k // period) % 2 * 2 - 1))) for k in range(n)))
    w.close()


def pool_card(card):
    """The core's card: /Banks alpha beta gamma (srclist's), /Samples/Drums/kick.wav, /E2E a.wav b.wav a.sfz b.sfz (each
    .sfz one region on its WAV)."""
    for i, nm in enumerate(["alpha", "beta", "gamma"]):
        pool_wav(os.path.join(card, "Banks", f"{nm}.wav"), 40 + 10 * i)
    pool_wav(os.path.join(card, "Samples", "Drums", "kick.wav"), 30)
    for i, nm in enumerate("ab"):
        pool_wav(os.path.join(card, "E2E", f"{nm}.wav"), 60 + 20 * i)
        open(os.path.join(card, "E2E", f"{nm}.sfz"), "w").write(f"<region> sample={nm}.wav lokey=0 hikey=127 "
                                                                 "pitch_keycenter=60\n")


def pool_files(card, key):
    """{row: line} of the core card's pool list file (the project's /.rytm/projects/<KEY>/<list>.pool.txt)."""
    import glob
    out = {}
    for p in glob.glob(os.path.join(card, ".rytm", "projects", "*", POOL_FILE[key])):
        for ln in open(p, errors="replace"):
            if ln[:1] in "ABCDEFGH" and len(ln) > 4:
                out[ln[:4]] = ln.rstrip("\n")
    return out


def machine_keys(key, t=300):
    """BD to that machine from the base (SRC SRC, DOWN x n, YES): the events and the time after them."""
    evs = []
    for k in ["SRC", "SRC"] + ["DOWN"] * POOL_DOWNS[key] + ["YES"]:
        evs.append((t, "press", k))
        t += 400
    return evs, t


def link_clock(res, emu_ms):
    """The link's clock (frames since its start) is the emulator's minus the Overbridge start-up: the offset from the
    first 4A open (the Rytm sends it a UI tick after the opening turn at emu_ms)."""
    bl = ev(res, rx="banklist")
    off = bl[0]["t"] - emu_ms / 1000 - 0.03 if bl else 0
    return lambda ms: ms / 1000 + off


def notes_of(name, res, t0, t1, margin=0.08):
    """Every text item of the commits in [t0 - margin, t1) of the link's clock (margin: link_clock's mapping is a UI tick
    or two off; 0.005 for windows between the Rytm's own events)."""
    out = []
    for c in ev(res, tx="commit"):
        if t0 - margin <= c["t"] < t1:
            p = os.path.join(OUT, f"frames_{name}", f"texts_{c['frame']}.json")
            if os.path.exists(p):
                out += [t[4] for t in json.load(open(p))]
    return out


def yes_downs(res):
    """The link times of the Rytm's YES key-downs (0x44) the core got: the browser's YES presses (a set, an ask, a
    folder entered); the list's YES that commits comes as 4A off (why yes) instead. Windows between the Rytm's own
    events: the link's clock runs a few % off the emulator's (link_clock drifts over seconds)."""
    return [x["t"] for x in ev(res, rx="key") if x["key"] == 0x44 and x["down"]]


def bl_times(res, on, why=None):
    return [x["t"] for x in ev(res, rx="banklist") if x["on"] == on and (why is None or x["why"] == why)]


def answered(res, t, within):
    """The core's 4A bodies (state bytes) in [t, t + within]."""
    return [x["body"][1:3] for x in ev(res, tx="0x4a") if t <= x["t"] <= t + within]


def at(lst, i, default=None):
    return lst[i] if 0 <= i < len(lst) else default


def pool_env(**kw):
    """Set the run's environment (always the emulated clock for the core: its notes' 2 s in emulated time); returns the
    keys to pop."""
    kw.setdefault("RUI_EMU_CLOCK", "1")
    os.environ.update(kw)
    return list(kw)


def t_poolset(_e, keys=("gr", "wt", "ms")):
    """Spec 7 "Rytm": FUNC + YES into A005 and C001 of each machine's own list, set and committed (WAVETABLE: A007,
    POOL_ROW). BD to GRANULAR / WAVETABLE / MULTISAMPLE; the browser parked on /E2E (browse=K:cd:/E2E: a.wav b.wav,
    MULTISAMPLE a.sfz b.sfz); knob A opens the bank list at the P1 default (A010 / A001 / A000), knob A to row 5 (7);
    FUNC + YES: the Rytm's 4A on 3, the core's 4A 0 1 (browser); YES on the first file: SET A005 (bank A like B..H: no
    BANK A refusal; MULTISAMPLE: READING A005, set when the .sfz is read), the core's 4A 0 0; YES commits: 48 0 0 5 0 1,
    the Rytm's P1 = 5. The list again (opens at A005), RIGHT x 2 (C005), knob A -4 (C001),
    FUNC + YES, DOWN, YES: SET C001 (MULTISAMPLE: READING C001, then set when the .sfz is read), YES: 48 0 0 1 0x20 1,
    P1 = 1 + 32 / 128. The pool list file has A005 = a, C001 = b (the right list: GRANULAR samples, WAVETABLE
    wavetables, MULTISAMPLE instruments)."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"poolset: the {tag} base in rytm-emu (us_base.sh {tag})", False)
        return
    for key in keys:
        name = f"poolset-{key}"
        e = Emu(tag)
        pool_card(e.core_card)
        evs, t = machine_keys(key)
        t0 = t + 2500
        row = POOL_ROW[key]
        ra = f"A{row:03d}"
        d5 = row - POOL_P1[key]
        evs += [(t0, "turn", "0:+1", 1, 50)]
        if d5:
            evs += [(t0 + 500, "turn", f"0:{d5:+d}", 1, 50)]
        evs += [(t0 + 1000, "press", "FUNC", 300), (t0 + 1100, "press", "YES"), (t0 + 2000, "press", "YES"),
                (t0 + 2800, "press", "YES"),
                (t0 + 3800, "turn", "0:+1", 1, 50), (t0 + 4300, "press", "RIGHT"), (t0 + 4600, "press", "RIGHT"),
                (t0 + 5000, "turn", f"0:{1 - row:+d}", 1, 50), (t0 + 5500, "press", "FUNC", 300), (t0 + 5600, "press", "YES"),
                (t0 + 6300, "press", "DOWN"), (t0 + 6800, "press", "YES"), (t0 + 7600, "press", "YES")]
        ks = pool_env(RUI_NM=f"browse={POOL_KIND[key]}:cd:/E2E", RUI_FRESH="1")
        try:
            r = e.run(name, t0 + 9000, evs)
        finally:
            for k in ks:
                os.environ.pop(k, None)
        bl = ev(r, rx="banklist")
        print(f"  {name}: 4A {[(round(x['t'], 2), x['on'], x['code'], x['why']) for x in bl]}")
        check(f"{name}: the list opens at the P1 default (A{POOL_P1[key]:03d}), FUNC + YES twice (4A on 3), two YES "
              "commits", bool(bl) and bl[0]["on"] == 1 and bl[0]["code"] == POOL_P1[key] and
              sum(x["on"] == 3 for x in bl) == 2 and sum(x["on"] == 0 and x["why"] == "yes" for x in bl) == 2, bl)
        a4 = [x["body"][1:3] for x in ev(r, tx="0x4a")]
        check(f"{name}: the core's browser opened and closed twice (4A 0 1, 4A 0 0) - bank A too", a4 == [[0, 1], [0, 0]] * 2, a4)
        ys, cl = yes_downs(r), bl_times(r, 0, "yes")
        print(f"  {name}: browser YES at {[round(y, 2) for y in ys]}, commits at {[round(c, 2) for c in cl]}")
        set_a = notes_of(name, r, at(ys, 0, 99), at(cl, 0, 99), 0.005)
        set_c = notes_of(name, r, at(ys, 1, 99), at(cl, 1, 99), 0.005)
        want_c = ("READING C001", "SET C001") if key == "ms" else ("SET C001",)
        want_a = (f"READING {ra}", f"SET {ra}") if key == "ms" else (f"SET {ra}",)
        print(f"  {name}: after the first set {sorted(set(set_a))[:16]}")
        print(f"  {name}: after the second set {sorted(set(set_c))[:16]}")
        check(f"{name}: {' / '.join(want_a)} on the Rytm's screen (the bank list's note)", any(w in set_a for w in want_a),
              sorted(set(set_a))[:16])
        check(f"{name}: {' / '.join(want_c)} on the Rytm's screen", any(w in set_c for w in want_c), sorted(set(set_c))[:16])
        w48 = [x["body"][1:6] for x in ev(r, tx="0x48")]
        check(f"{name}: YES commits {ra} then C001 (48 0 0 {row} 0 1, 48 0 0 1 0x20 1)",
              [0, 0, row, 0, 1] in w48 and [0, 0, 1, 0x20, 1] in w48 and
              w48.index([0, 0, row, 0, 1]) < w48.index([0, 0, 1, 0x20, 1]), w48)
        vals = [x["values"][0] for x in ev(r, rx="srcvals") if x["track"] == 0 and x["first"] == 0]
        check(f"{name}: the Rytm's P1 (2A) = {row} after the first commit, then 1 . C (1 + 32 / 128)",
              any(abs(v - row) < 1e-6 for v in vals) and bool(vals) and abs(vals[-1] - (1 + 32 / 128)) < 1e-6, vals[-6:])
        f = pool_files(e.core_card, key)
        print(f"  {name}: {POOL_FILE[key]} {f}")
        ext = "sfz" if key == "ms" else "wav"
        check(f"{name}: the {POOL_FILE[key]} holds {ra} = /E2E/a.{ext}, C001 = /E2E/b.{ext}",
              f"/E2E/a.{ext}" in f.get(ra, "") and f"/E2E/b.{ext}" in f.get("C001", ""), f)
        if key == "ms":
            check(f"{name}: the instrument's member (a.wav) under A005 (instpool's m line)",
                  any("/E2E/a.wav" in ln for ln in open(glob_one(e.core_card, POOL_FILE[key]))), "")
        others = [k for k in POOL_FILE if k != key and k != "gr"] + (["gr"] if key != "gr" else [])
        for o in others:
            fo = pool_files(e.core_card, o)
            check(f"{name}: nothing of it in {POOL_FILE[o]}", not any("/E2E/" in ln for ln in fo.values()), fo)
        shot(name, os.path.join(OUT, f"{name}.png"))


def glob_one(card, fname):
    import glob
    g = glob.glob(os.path.join(card, ".rytm", "projects", "*", fname))
    return g[0] if g else os.devnull


def t_poolrefuse(_e):
    """Spec 3.5 / 5: FUNC + YES refuses only row 000 (000 IS OFF) and an input / built-in row (BUILT-IN: UNLOAD FIRST);
    the core answers the Rytm's 4A on 3 with 4A 0 0 (no browser). GRANULAR: A000 (the list at A010, knob A -10), then H001
    (the template's input IN: LEFT x 0, RIGHT x 7, knob A to row 1); WAVETABLE: A001 (the template's BASIC, its default
    TBL 1) right where the list opens, then A000."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"poolrefuse: the {tag} base in rytm-emu", False)
        return
    # (the gap after each: GRANULAR's second refusal after the first's note (2 s) is gone; WAVETABLE's while it shows)
    plans = dict(gr=[(-10, [], "000 IS OFF", 2500), (1, ["RIGHT"] * 7, "BUILT-IN: UNLOAD FIRST", 2500)],
                 wt=[(0, [], "BUILT-IN: UNLOAD FIRST", 1200), (-1, [], "000 IS OFF", 1500)])
    for key, plan in plans.items():
        name = f"poolrefuse-{key}"
        e = Emu(tag)
        pool_card(e.core_card)
        evs, t = machine_keys(key)
        t0 = t + 2500
        evs.append((t0, "turn", "0:+1", 1, 50))
        tt, marks = t0 + 500, []
        for dr, presses, want, gap in plan:
            for k in presses:
                evs.append((tt, "press", k))
                tt += 300
            if dr:
                evs.append((tt, "turn", f"0:{dr:+d}", 1, 50))
                tt += 500
            evs += [(tt, "press", "FUNC", 300), (tt + 100, "press", "YES")]
            marks.append((tt + 100, want))
            tt += gap
        evs.append((tt, "press", "NO"))
        ks = pool_env(RUI_FRESH="1")
        try:
            r = e.run(name, tt + 1000, evs)
        finally:
            for k in ks:
                os.environ.pop(k, None)
        bl = ev(r, rx="banklist")
        a4 = [x["body"][1:3] for x in ev(r, tx="0x4a")]
        on3 = bl_times(r, 3)
        print(f"  {name}: 4A {[(round(x['t'], 2), x['on'], x['code'], x['why']) for x in bl]}; core 4A {a4}")
        check(f"{name}: each FUNC + YES reached the core (4A on 3) and was answered 4A 0 0 only (no browser opened)",
              sum(x["on"] == 3 for x in bl) == len(plan) and a4 == [[0, 0]] * len(plan), (bl, a4))
        for i, (tm, want) in enumerate(marks):
            got = notes_of(name, r, at(on3, i, 99), at(on3, i, 99) + 1.1, 0.005)
            check(f"{name}: {want} on the Rytm's screen" + (" (while the first refusal's note still shows)" if
                  (dr, presses, want, gap) == plan[-1] and plan[0][3] < 2000 else ""), want in got, sorted(set(got))[:14])
        check(f"{name}: no 48 (nothing committed)", not ev(r, tx="0x48"), ev(r, tx="0x48"))
        shot(name, os.path.join(OUT, f"{name}.png"))


def t_poolp3(_e):
    """The pools' P3 rules on the Rytm (GRANULAR, the browser on /E2E): A005 = a.wav committed, the BD pad (the voice
    on A005: in use); FUNC + YES on A005 again, DOWN, YES on b.wav: A005 USED 1: YES=SET (asked), a second YES: SET A005;
    the list again, FUNC + YES, DOWN, YES on b.wav (in A005 now): IN POOL A005 and the list on A005; PLAY, FUNC + YES,
    YES on a.wav: PLAYING: STOP FIRST (refused while the Rytm plays); STOP. Then poolused: the 48 commit alone (no pad)
    puts A005 in the used set (spec 4.3: "48 commits"), so the replace asks too."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"poolp3: the {tag} base in rytm-emu", False)
        return
    t_poolused(tag)
    name = "poolp3"
    e = Emu(tag)
    pool_card(e.core_card)
    evs, t = machine_keys("gr")
    t0 = t + 2500
    evs += [(t0, "turn", "0:+1", 1, 50), (t0 + 500, "turn", "0:-5", 1, 50),
            (t0 + 1000, "press", "FUNC", 300), (t0 + 1100, "press", "YES"), (t0 + 2000, "press", "YES"),   # A005 = a
            (t0 + 2800, "press", "YES"),                                                                 # commit
            (t0 + 3300, "press", "BD"),                                                                  # plays A005
            (t0 + 3800, "turn", "0:+1", 1, 50), (t0 + 4300, "press", "FUNC", 300), (t0 + 4400, "press", "YES"),
            (t0 + 5000, "press", "DOWN"), (t0 + 5500, "press", "YES"),                                   # ask
            (t0 + 6300, "press", "YES"),                                                                 # confirmed
            (t0 + 7100, "turn", "0:+1", 1, 50), (t0 + 7600, "press", "FUNC", 300), (t0 + 7700, "press", "YES"),
            (t0 + 8300, "press", "DOWN"), (t0 + 8800, "press", "YES"),                                   # IN POOL A005
            (t0 + 9800, "press", "PLAY"),
            (t0 + 10300, "press", "FUNC", 300), (t0 + 10400, "press", "YES"), (t0 + 11000, "press", "YES"),   # playing
            (t0 + 11600, "press", "YES"),                                                    # again: still refused
            (t0 + 12200, "press", "NO"), (t0 + 12600, "press", "NO"), (t0 + 13000, "press", "STOP")]
    ks = pool_env(RUI_NM="browse=gr:cd:/E2E", RUI_FRESH="1", RUI_SEQ_TICK="1")
    try:
        r = e.run(name, t0 + 14000, evs)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    bl = ev(r, rx="banklist")
    ys = yes_downs(r)   # a.wav set, b.wav asked, b.wav confirmed, b.wav IN POOL, a.wav playing, a.wav playing again
    print(f"  {name}: browser YES at {[round(y, 2) for y in ys]}")
    print(f"  {name}: 4A {[(round(x['t'], 2), x['on'], x['code'], x['why']) for x in bl]}; core 4A "
          f"{[x['body'][1:3] for x in ev(r, tx='0x4a')]}; rt {[x['b'] for x in ev(r, rx='rt')]}")
    y1, y2 = at(ys, 1), at(ys, 2)
    a1, a2 = (answered(r, y1, 0.3) if y1 else []), (answered(r, y2, 0.1) if y2 else [])
    print(f"  {name}: YES on b.wav at {y1}: 4A {a1}; the second at {y2}: 4A {a2}")
    check(f"{name}: a file over a row in use (the voice on it): the first YES only asks (the browser stays open, no 4A)",
          y1 is not None and a1 == [], (y1, a1))
    check(f"{name}: the second YES on the same file sets it (4A 0 0)", y2 is not None and a2 == [[0, 0]], (y2, a2))
    ask = notes_of(name, r, at(ys, 1, 99), at(ys, 2, 99), 0.005)
    check(f"{name}: the ask on the Rytm's screen: A005 USED 1: YES=SET / A005 IN USE: YES=SET",
          "A005 USED 1: YES=SET" in ask or "A005 IN USE: YES=SET" in ask, sorted(set(ask))[:14])
    conf = notes_of(name, r, at(ys, 2, 99), at(ys, 2, 99) + 0.6, 0.005)
    check(f"{name}: SET A005 on the Rytm's screen after the second YES", "SET A005" in conf, sorted(set(conf))[:14])
    dup = notes_of(name, r, at(ys, 3, 99), at(ys, 4, 99), 0.005)
    check(f"{name}: a file already in the list goes to its row: IN POOL A005 (the list on A005)",
          "IN POOL A005" in dup and any(str(x).startswith("005 B") for x in dup), sorted(set(dup))[:14])
    p1, p2 = at(ys, 4), at(ys, 5)
    b1, b2 = (answered(r, p1, 0.3) if p1 else []), (answered(r, p2, 0.3) if p2 else [])
    print(f"  {name}: playing: YES on a.wav at {p1}: 4A {b1}; again at {p2}: 4A {b2}; rt {[x['b'] for x in ev(r, rx='rt')]}")
    check(f"{name}: while the Rytm plays (FA) neither YES on a.wav sets A005 (no 4A: refused, not asked)",
          p1 is not None and p2 is not None and b1 == [] and b2 == [] and 250 in [x["b"] for x in ev(r, rx="rt")],
          (p1, b1, p2, b2))
    ply = notes_of(name, r, at(ys, 4, 99), at(ys, 5, 99) + 0.5, 0.005)
    check(f"{name}: the refusal on the Rytm's screen: PLAYING: STOP FIRST", "PLAYING: STOP FIRST" in ply,
          sorted(set(ply))[:14])
    f = pool_files(e.core_card, "gr")
    print(f"  {name}: {f}")
    check(f"{name}: samples.pool.txt: A005 = b.wav (the confirmed replace; a.wav refused while playing), no A006",
          "/E2E/b.wav" in f.get("A005", "") and "A006" not in f, f)
    shot(name, os.path.join(OUT, f"{name}.png"))


def t_poolused(tag):
    """poolp3's first part without the pad: A005 = a.wav committed by the list's YES (48), then FUNC + YES, DOWN, YES on
    b.wav: asked (A005 IN USE: YES=SET) - the 48 commit is in the used set (spec 4.3, pools.h "used")."""
    name = "poolused"
    e = Emu(tag)
    pool_card(e.core_card)
    evs, t = machine_keys("gr")
    t0 = t + 2500
    evs += [(t0, "turn", "0:+1", 1, 50), (t0 + 500, "turn", "0:-5", 1, 50),
            (t0 + 1000, "press", "FUNC", 300), (t0 + 1100, "press", "YES"), (t0 + 2000, "press", "YES"),
            (t0 + 2800, "press", "YES"),
            (t0 + 3800, "turn", "0:+1", 1, 50), (t0 + 4300, "press", "FUNC", 300), (t0 + 4400, "press", "YES"),
            (t0 + 5000, "press", "DOWN"), (t0 + 5500, "press", "YES"), (t0 + 6300, "press", "NO"), (t0 + 6700, "press", "NO")]
    ks = pool_env(RUI_NM="browse=gr:cd:/E2E", RUI_FRESH="1")
    try:
        r = e.run(name, t0 + 8000, evs)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    w48 = [x["body"][1:6] for x in ev(r, tx="0x48")]
    ys = yes_downs(r)   # a.wav set, b.wav over it
    y1 = at(ys, 1)
    a1 = answered(r, y1, 0.3) if y1 else []
    print(f"  {name}: 48 {w48}; the YES on b.wav at {y1}: 4A {a1}; pool {pool_files(e.core_card, 'gr').get('A005')}")
    check(f"{name}: A005 committed (48 0 0 5 0 1), then b.wav over it only asks (the 48 commit is in the used set: no "
          "4A, A005 still a.wav)", [0, 0, 5, 0, 1] in w48 and y1 is not None and a1 == [] and
          "/E2E/a.wav" in pool_files(e.core_card, "gr").get("A005", ""), (w48, y1, a1))


def t_poolnames(_e):
    """Spec 3.5 (bank A's names) and rytm-mods 0026 1043 (cu_text: bank A like B..H): BD on GRANULAR / MULTISAMPLE at
    the P1 default (A010 / A000), the Rytm's own stock SRC page (RUI_NO_SRCVIEW: the core never draws the view, so the
    knob text is the Rytm's). (a) no host: the firmware's own text (A010 / OFF); (b) the core (the template: SAMPLES
    bank A empty, INSTRUMENTS empty): its 47 for bank A (bank byte 0, total 127, all 128 rows: OFF, then A001.. - no IN /
    MAIN / BD names anywhere in bank A) and the screen equal to (a); (c) the core with A010 / A000.. named (GRANULAR:
    alpha set into A010): the screen differs from (a) (the host's name shown)."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"poolnames: the {tag} base in rytm-emu", False)
        return
    for key in ("gr", "ms"):
        e = Emu(tag)
        pool_card(e.core_card)
        evs, t = machine_keys(key)
        T = t + 3000
        ks = pool_env(RUI_FRESH="1", RUI_NO_SRCVIEW="1")
        try:
            a = e.run(f"poolnames-{key}-nohost", T, evs, link=False)
            b = e.run(f"poolnames-{key}-host", T, evs)
            if key == "gr":
                os.environ["RUI_NM"] = "pool=smp:set:A010:/Banks/alpha.wav"
                c = e.run(f"poolnames-{key}-named", T, evs)
        finally:
            for k in ks + ["RUI_NM"]:
                os.environ.pop(k, None)
        n47 = [x["body"] for x in ev(b, tx="0x47") if len(x["body"]) >= 5 and x["body"][1] == POOL_LIST[key] and x["body"][4] == 0]
        rows = {}
        for body in n47:
            for i in range(5, len(body) - 13, 14):
                rows[body[i]] = bytes(body[i + 1:i + 14]).rstrip(b"\0").decode(errors="replace")
        print(f"  poolnames-{key}: bank A 47s {len(n47)}, rows {len(rows)}: {[rows.get(i) for i in range(0, 13)]}")
        bad = [nm for nm in rows.values() if nm in ("IN", "MAIN", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB",
                                                     "PAD", "BELLS")]
        check(f"poolnames-{key}: the core names bank A in the 47 form with bank byte 0: all 128 rows, 000 OFF, empty rows "
              "A001 .. (no IN / MAIN texts)", len(rows) == 128 and rows.get(0) == "OFF" and rows.get(5) == "A005" and
              not bad, ([rows.get(i) for i in range(13)], bad))
        check(f"poolnames-{key}: before any 47 (no host) the Rytm's own text equals the host-named one "
              f"({'A010' if key == 'gr' else 'OFF'}: the screens equal)", a["oled"] is not None and a["oled"] == b["oled"],
              diff_bytes(a["oled"], b["oled"]))
        if key == "gr":
            check("poolnames-gr: alpha in A010: the Rytm shows the host's name (the screen differs from A010's)",
                  c["oled"] is not None and c["oled"] != a["oled"], diff_bytes(a["oled"], c["oled"]))
            shot("poolnames-gr-named", os.path.join(OUT, "poolnames-gr-named.png"))
        shot(f"poolnames-{key}", os.path.join(OUT, f"poolnames-{key}-host.png"))


def t_pooldsp(_e):
    """Spec 3.6: DSP SAMPLE's 14 table from SAMPLES bank A: A001 = /Samples/Drums/kick.wav (slot 1, its card folder /
    file), A002 = the input IN (an empty slot: not in the table), A003 = /Banks/alpha.wav (outside /Samples: folder
    0x3fff, the name only), A004.. empty; the template's inputs in bank H nothing. rui1045: the table stays for the Rytm
    builds before it (whose SMP opened the stock sample list drawn from it); on rui1045 SMP opens the bank list
    (dspbank), so the stock list's rows are checked only on an older base (the tag's 0026 without 1045)."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"pooldsp: the {tag} base in rytm-emu", False)
        return
    e = Emu(tag)
    pool_card(e.core_card)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * (EXT_DOWNS["JUNO"] + 3) + ["YES"]:   # JUNO 41, 42, 44, DSP SAMPLE 45
        evs.append((t, "press", k))
        t += 400
    old = tag < "rui1045"   # (rui1043, rui1044: SMP still stock's slot list)
    if old:
        evs += [(t + 500, "press", "SMPL"), (t + 1100, "turn", "3:+8", 1, 50)]   # the list opens, the cursor on 0:OFF
    ks = pool_env(RUI_NM="pool=smp:set:A001:/Samples/Drums/kick.wav;pool=smp:input:A002:IN;"
                         "pool=smp:set:A003:/Banks/alpha.wav", RUI_FRESH="1")
    try:
        r = e.run("pooldsp", t + 2600, evs)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    tb = ev(r, tx="0x14")
    last = []
    for x in tb:   # the last full table: from the last replacing message on
        last = list(x["rows"]) if x["replace"] else last + list(x["rows"])
    print(f"  pooldsp: 14s {len(tb)}, the last table {last}")
    slots = {x[0]: x for x in last}
    check("pooldsp: the 14 table: slot 1 = A001's kick (a card folder / file, name KICK)", 1 in slots and
          slots[1][1] != 0x3FFF and slots[1][3].upper() == "KICK", last)
    check("pooldsp: slot 2 (A002 the input IN) not in the table (an empty slot); nothing from bank H's inputs",
          2 not in slots and all(1 <= s <= 127 for s in slots) and len(slots) == 2, last)
    check("pooldsp: slot 3 = A003's alpha outside /Samples: folder 0x3fff, the name only", 3 in slots and
          slots[3][1] == 0x3FFF and slots[3][3].upper() == "ALPHA", last)
    if not old:
        shot("pooldsp", os.path.join(OUT, "pooldsp.png"))
        return
    fb = r["oled"]
    # the list's rows on the panel: row i's glyphs at y 12 + 7 i .. + 4; the text after "N:" from x 77 [measured]
    band = (lambda i: [tuple(px(fb, x, y) for x in range(77, 112)) for y in range(12 + 7 * i, 17 + 7 * i)]) if fb else None
    ok = bool(fb) and band(2) == band(4) == band(5) and any(any(rw) for rw in band(4))
    check("pooldsp: the Rytm's SMP list draws slot 2 (the input) as the empty slots 4 and 5 (---)", ok,
          "" if ok else "rows differ")
    check("pooldsp: ... and slots 1 (kick) and 3 (alpha) with their names (not ---)", bool(fb) and band(1) != band(4) and
          band(3) != band(4), "")
    shot("pooldsp", os.path.join(OUT, "pooldsp.png"))


def t_srclist(_e):
    """rui1038's source banks (rytm-mods 0026 1038) on the pools (spec 3.5; rui1043's base, RUI_TAG_1043): the core's
    card with /Banks (three WAVs) filled into the SAMPLES pool's bank B (pool=smp:fill:B:/Banks); BD set to GRANULAR on
    the Rytm (SRC 10: A010), its SRC view drawn by the core; knob A turned on the Rytm opens the bank list (4A on, P1
    A010: the opening turn moves nothing), the core draws the tabs and bank A's rows around A010 (an empty row: ---);
    RIGHT: bank B (its names sent to the Rytm: 47 with the bank byte 1), knob A -9: row 1 (B001 ALPHA); YES: 4A off
    (yes), the core's 48 0 0 1 0x10 1, the Rytm's 2A then P1 = 1 . B (1 + 16 / 128); the view again (no SOURCE list).
    Times on the link's clock from the 4A open (link_clock)."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srclist: the {tag} base in rytm-emu (rytm-mods tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    pool_card(e.core_card)
    evs, t = machine_keys("gr")
    t_open = t + 2500
    evs += [(t_open, "turn", "0:+1", 1, 50), (t_open + 600, "press", "RIGHT"), (t_open + 1000, "turn", "0:-9", 1, 50),
            (t_open + 1600, "press", "YES")]
    t_end = t_open + 3500
    ks = pool_env(RUI_NM="pool=smp:fill:B:/Banks", RUI_FRESH="1")
    try:
        r = e.run("srclist", t_end, evs)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    lc = link_clock(r, t_open)
    bl = ev(r, rx="banklist")
    print(f"  4A {bl}")
    check("srclist: the Rytm's 4A: opened on BD at A010, then closed by YES",
          len(bl) >= 2 and bl[0]["on"] == 1 and bl[0]["track"] == 0 and bl[0]["code"] == 10 and bl[-1]["on"] == 0 and
          bl[-1]["why"] == "yes", bl)
    lst = texts_of("srclist", r, lc(t_open + 500))
    print(f"  srclist: the list at A010: {lst}")
    check("srclist: the core draws the bank list over the view (tabs A .. H, a text item each; A010's row, empty: ---)",
          all(c in lst for c in "ABCDEFGH") and "010 ---" in lst, lst)
    lb = texts_of("srclist", r, at(bl_times(r, 0, "yes"), 0, 99) - 0.01)
    print(f"  srclist: bank B row 1: {lb}")
    check("srclist: RIGHT -> bank B, knob A -9 -> row 1: B001 ALPHA in the list", "B" in lb and
          any(str(x).upper().startswith("001 ALPHA") for x in lb), lb)
    n47 = [x for x in ev(r, tx="0x47") if len(x["body"]) >= 6 and x["body"][1] == 0 and x["body"][4] == 1]   # (body[0]: ver)
    check("srclist: bank B's names to the Rytm (47 list 0 with the bank byte 1)", bool(n47), ev(r, tx="0x47")[-2:])
    w48 = ev(r, tx="0x48")
    check("srclist: YES -> the core's 48 0 0 1 0x10 1 (B001, the list's commit)", any(x["body"][1:6] == [0, 0, 1, 0x10, 1]
          for x in w48), w48)
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0 and x["first"] == 0]
    last = vals[-1]["values"][0] if vals else None
    check("srclist: the Rytm's 2A then: P1 = 1 . B (1 + 16 / 128)", last is not None and abs(last - (1 + 16 / 128)) < 1e-6,
          last)
    end = texts_of("srclist", r)
    check("srclist: the view again after the commit (GRANULAR's page, no SOURCE list)", "GRANULAR" in end and
          "SOURCE" not in end and not all(c in end for c in "ABCDEFGH"), end[:10])
    f = pool_files(e.core_card, "gr")
    check("srclist: the fill put alpha beta gamma in B001..B003 of samples.pool.txt",
          all(f"/Banks/{n}.wav" in f.get(f"B00{i + 1}", "") for i, n in enumerate(["alpha", "beta", "gamma"])), f)
    shot("srclist", os.path.join(OUT, "srclist.png"))


def t_srcbrowse(_e):
    """rui1040's FUNC + YES on a bank row (rytm-mods 0026 1040) on the pools (rui1043's base, RUI_TAG_1043): /Banks
    (alpha beta gamma) filled into SAMPLES bank B, the browser put back at the root after the fill
    (browse=gr:root: a fill lists its folder in the machine's browser and leaves it there), BD on GRANULAR, the list
    opened by knob A, RIGHT x 2 (bank C), knob A -9 (row 1, empty), FUNC + YES: the Rytm's 4A on 3, the core's 4A 0 1
    and its browser at the root (C001 /, GRANULAR's @INPUTS/ @BUILT-IN/, then the card's folders BANKS/ E2E/ SAMPLES/);
    DOWN x 3, YES into /E2E, YES on a.wav (not in the pool yet): C001 = /E2E/a.wav (SET C001), the core's 4A 0 0, the
    list again on C001 with A; YES then commits C001 (48 0 0 1 0x20 1). The tabs' selected bank: its own item."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"srcbrowse: the {tag} base in rytm-emu (rytm-mods tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    pool_card(e.core_card)
    evs, t = machine_keys("gr")
    t0 = t + 2500
    evs += [(t0, "turn", "0:+1", 1, 50), (t0 + 500, "press", "RIGHT"), (t0 + 800, "press", "RIGHT"),
            (t0 + 1100, "turn", "0:-9", 1, 50), (t0 + 1500, "press", "FUNC", 300), (t0 + 1600, "press", "YES"),
            (t0 + 2400, "press", "DOWN"), (t0 + 2700, "press", "DOWN"), (t0 + 3000, "press", "DOWN"),
            (t0 + 3400, "press", "YES"), (t0 + 4000, "press", "YES"), (t0 + 5000, "press", "YES")]
    ks = pool_env(RUI_NM="pool=smp:fill:B:/Banks;browse=gr:root", RUI_FRESH="1")
    try:
        r = e.run("srcbrowse", t0 + 6500, evs)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    bl = ev(r, rx="banklist")
    print(f"  4A {bl}")
    on3 = at(bl_times(r, 3), 0, 99)
    downs = [x["t"] for x in ev(r, rx="key") if x["key"] == 0x43 and x["down"] and x["t"] > on3]
    tabs = texts_of("srcbrowse", r, on3 - 0.01)
    print(f"  list on C001: {tabs}")
    check("srcbrowse: the tabs a letter an item, C001 the cursor", all(c in tabs for c in "ABCDEFGH") and
          "001 ---" in tabs, tabs)
    br = texts_of("srcbrowse", r, at(downs, 0, 99) - 0.01)
    print(f"  browser: {br}")
    check("srcbrowse: FUNC + YES -> the Rytm's 4A on 3, the core's browser at the root (C001 /: @INPUTS/ @BUILT-IN/, "
          "the card's BANKS/ E2E/)", any(x.get("on") == 3 for x in bl) and any(str(x).startswith("C001") for x in br) and
          all(any(w == str(x).split()[0] for x in br) for w in ("@INPUTS/", "@BUILT-IN/", "BANKS/", "E2E/")), br)
    a4 = [x for x in ev(r, tx="0x4a")]
    print(f"  core 4A {[x['body'] for x in a4]}")
    check("srcbrowse: the core answered 4A 0 1 (browser open) then 4A 0 0 (closed)",
          [x["body"][1:3] for x in a4][:2] == [[0, 1], [0, 0]], [x["body"] for x in a4])
    after = notes_of("srcbrowse", r, at(yes_downs(r), 1, 99), at(bl_times(r, 0, "yes"), 0, 99), 0.005)
    print(f"  after the set: {sorted(set(after))}")
    check("srcbrowse: a.wav set into C001 (the list again: 001 A, SET C001)", "001 A" in after and "SET C001" in after,
          sorted(set(after)))
    w48 = ev(r, tx="0x48")
    check("srcbrowse: YES then commits C001 (48 0 0 1 0x20 1)", any(x["body"][1:6] == [0, 0, 1, 0x20, 1] for x in w48), w48)
    f = pool_files(e.core_card, "gr")
    check("srcbrowse: samples.pool.txt C001 = /E2E/a.wav", "/E2E/a.wav" in f.get("C001", ""), f)
    shot("srcbrowse", os.path.join(OUT, "srcbrowse.png"))


def texts_all(name, res):
    """Every text item of every commit of the run."""
    out = []
    for c in ev(res, tx="commit"):
        p = os.path.join(OUT, f"frames_{name}", f"texts_{c['frame']}.json")
        if os.path.exists(p):
            out += [t[4] for t in json.load(open(p))]
    return out


def t_dspbank(_e):
    """rui1045 (rytm-mods 0026 1045, the pools' DSP SAMPLE on every bank; RUI_TAG_1043 = a rui1045 base): BD to DSP SAMPLE,
    its SMPL page, SMP (knob D) turned: the Rytm's 4A on (P11 A000), its 29 1 (the view over the SMPL page for the list),
    the core draws the bank list (tabs A..H, SAMPLE); RIGHT x 2 (bank C), D +20 (row 20); FUNC + YES: the Rytm's 4A on 3,
    the core's 4A 0 1 with its browser on /Samples/E2E (a file the card index has: the page's overview reads it); YES on
    c.wav: SET C020 (samples.pool.txt C020 = /Samples/E2E/c.wav), 4A 0 0; YES
    commits: the core's 48 0 10 20 0x20 1, the Rytm's SMP word 0x1440 (C020); SRC: the view's DSP SAMPLE page draws
    C020's waveform (not SMP OFF / EMPTY SLOT: the core resolved bank C), its 2A P11 = 20 . C; the BD pad plays it (the
    core's DSP SAMPLE trig names slot 274 = 2 x 127 + 20). A second run: FUNC + YES on row 20 of every bank A..H opens the browser
    (the core's 4A 0 1 eight times, its header A020 .. H020, NO back: 4A 0 0)."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"dspbank: the {tag} base in rytm-emu (rytm-mods tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    pool_card(e.core_card)
    pool_wav(os.path.join(e.core_card, "Samples", "E2E", "c.wav"), 25, 24000)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * (EXT_DOWNS["JUNO"] + 3) + ["YES"]:   # DSP SAMPLE (45)
        evs.append((t, "press", k))
        t += 400
    t0 = t + 2500
    evs += [(t0, "press", "SMPL"), (t0 + 600, "turn", "3:+1", 1, 50), (t0 + 1100, "press", "RIGHT"),
            (t0 + 1400, "press", "RIGHT"), (t0 + 1800, "turn", "3:+20", 1, 50), (t0 + 2200, "press", "FUNC", 300),
            (t0 + 2300, "press", "YES"), (t0 + 3100, "press", "YES"), (t0 + 3900, "press", "YES"),
            (t0 + 4600, "press", "SRC"), (t0 + 6000, "press", "BD")]
    ks = pool_env(RUI_NM="browse=gr:cd:/Samples/E2E", RUI_FRESH="1", RUI_SAMPLES="1", CORE_SAMPLE_RAM_MB="16")
    try:
        r = e.run("dspbank", t0 + 7500, evs, save=True)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    bl = ev(r, rx="banklist")
    print(f"  4A {[(round(x['t'], 2), x['on'], x['code'], x['why']) for x in bl]}")
    check("dspbank: SMP opens the list (the Rytm's 4A on at A000), FUNC + YES (4A on 3), YES commits (4A off, yes)",
          bool(bl) and bl[0]["on"] == 1 and bl[0]["code"] == 0 and any(x["on"] == 3 for x in bl) and
          any(x["on"] == 0 and x["why"] == "yes" for x in bl), bl)
    lc = link_clock(r, t0 + 600)
    lst = texts_of("dspbank", r, lc(t0 + 1700))
    print(f"  the list on bank C: {lst}")
    check("dspbank: the core draws the bank list over the SMPL page (tabs A .. H, SAMPLE, C's row 000 OFF)",
          all(c in lst for c in "ABCDEFGH") and "SAMPLE" in lst, lst)
    a4 = [x["body"][1:3] for x in ev(r, tx="0x4a")]
    check("dspbank: the core's browser opened and closed (4A 0 1, 4A 0 0)", a4[:2] == [[0, 1], [0, 0]], a4)
    ys, cl = yes_downs(r), bl_times(r, 0, "yes")
    set_c = notes_of("dspbank", r, at(ys, 0, 99), at(cl, 0, 99), 0.005)
    check("dspbank: SET C020 on the Rytm's screen", "SET C020" in set_c, sorted(set(set_c))[:12])
    f = pool_files(e.core_card, "gr")
    check("dspbank: samples.pool.txt C020 = /Samples/E2E/c.wav", "/Samples/E2E/c.wav" in f.get("C020", ""), f)
    w48 = [x["body"][1:6] for x in ev(r, tx="0x48")]
    check("dspbank: YES commits SMP = C020 (the core's 48 0 10 20 0x20 1)", [0, 10, 20, 0x20, 1] in w48, w48)
    peek = subprocess.run([os.path.join(EMU, "target/release/dtpeek"), r["snap"], "mem", "800067b8", "4"], cwd=EMU,
                          env=dict(os.environ, DTEMU_FW=e.fw), capture_output=True, text=True).stdout
    kit = int(peek.split(":")[1].split()[0], 16) & 127 if ":" in peek else 0
    wl = subprocess.run([os.path.join(EMU, "target/release/dtpeek"), r["snap"], "mem",
                         f"{0x41255933 + 2531 * kit + 2 * 10:x}", "2"], cwd=EMU, env=dict(os.environ, DTEMU_FW=e.fw),
                        capture_output=True, text=True).stdout
    w = int("".join(wl.split(":")[1].split()[:2]), 16) if ":" in wl else -1
    check("dspbank: the Rytm's SMP word (19) = 0x1440 (C020)", w == 0x1440, hex(w))
    vals = [x for x in ev(r, rx="srcvals") if x["track"] == 0 and x["first"] <= 10 < x["first"] + len(x["values"])]
    smp = [x["values"][10 - x["first"]] for x in vals]
    check("dspbank: the view's 2A carries P11 = 20 . C (20 + 32 / 128) by the end", bool(smp) and abs(smp[-1] - (20 + 32 / 128)) < 1e-6,
          smp[-4:])
    end = texts_of("dspbank", r, lc(t0 + 5900))
    fr = last_frame("dspbank", r)
    wavepx = sum(px(fr, x, y) for x in range(1, 101) for y in range(11, 25)) if fr else 0
    print(f"  SRC view: {end}, wave px {wavepx}")
    check("dspbank: SRC: the DSP SAMPLE page draws C020's waveform (not SMP OFF / EMPTY SLOT)", "DSP SAMPLE" in end and
          "SMP OFF" not in end and "EMPTY SLOT" not in end and wavepx > 150, (end[:8], wavepx))
    lt = open(os.path.join(OUT, "dspbank.link.txt"), errors="replace").read()
    trig = re.findall(r"trig voice 0 note \d+ DSP SAMPLE .*", lt)
    print(f"  the core's trig: {trig[-1:]}")
    check("dspbank: the BD pad plays it: the core's DSP SAMPLE trig on SMP 20 slot 274 (C020: 2 x 127 + 20)",
          any("smp 20 slot 274" in x for x in trig), trig[-2:])
    if fr:
        frame_png(os.path.join(OUT, "dspbank.png"), fr)
    shot("dspbank", os.path.join(OUT, "dspbank.png"))
    # every bank: FUNC + YES on row 20 of A..H opens the browser (the core's 4A 0 1), NO back (4A 0 0)
    e2 = Emu(tag)
    pool_card(e2.core_card)
    evs2 = [x for x in evs if x[0] < t0 + 1100]   # to the list's opening
    tt = t0 + 1100
    evs2 += [(tt, "turn", "3:+20", 1, 50)]
    tt += 500
    for b in range(8):
        if b:
            evs2 += [(tt, "press", "RIGHT")]
            tt += 400
        evs2 += [(tt, "press", "FUNC", 300), (tt + 100, "press", "YES"), (tt + 900, "press", "NO")]
        tt += 1400
    ks = pool_env(RUI_FRESH="1")
    try:
        r2 = e2.run("dspbank-all", tt + 500, evs2)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    on3 = [x for x in ev(r2, rx="banklist") if x["on"] == 3]
    a42 = [x["body"][1:3] for x in ev(r2, tx="0x4a")]
    heads = sorted({str(x)[:4] for x in texts_all("dspbank-all", r2) if re.match(r"^[A-H]020( |$)", str(x))})
    print(f"  every bank: 4A on 3 x {len(on3)}; core 4A {a42}; browser headers {heads}")
    check("dspbank: FUNC + YES eight times (the Rytm's 4A on 3), the core's browser headed A020 .. H020",
          len(on3) == 8 and heads == [f"{c}020" for c in "ABCDEFGH"], (len(on3), heads))
    check("dspbank: the core's browser opened in every bank (4A 0 1 eight times, each closed by NO: 4A 0 0)",
          a42 == [[0, 1], [0, 0]] * 8, a42)


def t_dspsrcyes(_e):
    """fix1049 (rytm-mods 0026 1049; the hardware pass: FUNC + YES on DSP SAMPLE's SRC page opened nothing, SMP OFF, the
    pool empty): BD to DSP SAMPLE, its SRC page (the core's view: SMP OFF), FUNC + YES: the Rytm's 4A on (A000, why open)
    then 4A on 3 (why srcyes), the core's browser at once aimed at A003 (bank A's first free row: pool_card holds A001
    break, A002 kick); YES on c.wav: SET A003 (samples.pool.txt A003); YES commits SMP = A003 (the core's 48 0 10 3 0 1);
    the view draws its waveform; the BD pad plays it (the core's DSP SAMPLE trig, smp 3 slot 3)."""
    tag = pool_tag()
    if not os.path.exists(os.path.join(EMU, f"sections/us_{tag}.json")):
        check(f"dspsrcyes: the {tag} base in rytm-emu (rytm-mods tools/emu/us_base.sh {tag})", False)
        return
    e = Emu(tag)
    pool_card(e.core_card)
    pool_wav(os.path.join(e.core_card, "Samples", "E2E", "c.wav"), 25, 24000)
    evs, t = [], 300
    for k in ["SRC", "SRC"] + ["DOWN"] * (EXT_DOWNS["JUNO"] + 3) + ["YES"]:   # DSP SAMPLE (45)
        evs.append((t, "press", k))
        t += 400
    t0 = t + 2500
    evs += [(t0, "press", "FUNC", 300), (t0 + 100, "press", "YES"), (t0 + 900, "press", "YES"), (t0 + 1700, "press", "YES"),
            (t0 + 3200, "press", "BD")]
    ks = pool_env(RUI_NM="browse=gr:cd:/Samples/E2E", RUI_FRESH="1", RUI_SAMPLES="1", CORE_SAMPLE_RAM_MB="16")
    try:
        r = e.run("dspsrcyes", t0 + 4500, evs, save=True)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    lc = link_clock(r, t0)
    before = texts_of("dspsrcyes", r, lc(t0 - 100))
    print(f"  SRC view before: {before}")
    check("dspsrcyes: DSP SAMPLE's SRC view first (the core's page, SMP OFF)", "DSP SAMPLE" in before and "SMP OFF" in before,
          before[:8])
    bl = ev(r, rx="banklist")
    print(f"  4A {[(round(x['t'], 2), x['on'], x['code'], x['why']) for x in bl]}")
    check("dspsrcyes: FUNC + YES on the SRC view: the Rytm's 4A on (A000) then 4A on 3 (srcyes); YES commits (4A off, yes)",
          len(bl) >= 3 and bl[0]["on"] == 1 and bl[0]["code"] == 0 and bl[1]["on"] == 3 and bl[1]["why"] == "srcyes" and
          any(x["on"] == 0 and x["why"] == "yes" for x in bl), bl)
    a4 = [x["body"][1:3] for x in ev(r, tx="0x4a")]
    check("dspsrcyes: the core's browser opened at once and closed on the file (4A 0 1, 4A 0 0)", a4[:2] == [[0, 1], [0, 0]], a4)
    heads = sorted({str(x)[:4] for x in texts_all("dspsrcyes", r) if re.match(r"^A00[0-9]( |$)", str(x))})
    check("dspsrcyes: the browser headed A003 (bank A's first free row, not 000 IS OFF)", heads == ["A003"], heads)
    f = pool_files(e.core_card, "gr")
    check("dspsrcyes: samples.pool.txt A003 = /Samples/E2E/c.wav", "/Samples/E2E/c.wav" in f.get("A003", ""), f)
    w48 = [x["body"][1:6] for x in ev(r, tx="0x48")]
    check("dspsrcyes: YES commits SMP = A003 (the core's 48 0 10 3 0 1)", [0, 10, 3, 0, 1] in w48, w48)
    end = texts_of("dspsrcyes", r, lc(t0 + 3000))
    fr = last_frame("dspsrcyes", r)
    wavepx = sum(px(fr, x, y) for x in range(1, 101) for y in range(11, 25)) if fr else 0
    print(f"  SRC view after: {end}, wave px {wavepx}")
    check("dspsrcyes: SRC: the view draws A003's waveform (not SMP OFF / EMPTY SLOT)", "DSP SAMPLE" in end and
          "SMP OFF" not in end and "EMPTY SLOT" not in end and wavepx > 150, (end[:8], wavepx))
    lt = open(os.path.join(OUT, "dspsrcyes.link.txt"), errors="replace").read()
    trig = re.findall(r"trig voice 0 note \d+ DSP SAMPLE .*", lt)
    print(f"  the core's trig: {trig[-1:]}")
    check("dspsrcyes: the BD pad plays it: the core's DSP SAMPLE trig on SMP 3 slot 3", any("smp 3 slot 3 " in x for x in trig),
          trig[-2:])
    if fr:
        frame_png(os.path.join(OUT, "dspsrcyes.png"), fr)
    shot("dspsrcyes", os.path.join(OUT, "dspsrcyes.png"))


def t_project(e):
    """Bundles phase 3: the Rytm's PROJECT page (the last extra page: LEFT on SIDECAR) against the real core's bundle job
    (rui_link.py RUI_BUNDLE: two projects on the core's card, the job stepped as core 0 would): its mask (A, H, T1..T5),
    the list, T1 + YES exports JAM 1021 to the card (DONE), YES back, T2 lists the card's bundles, YES checks the
    bundle, the summary (IMPORT AS JAM 1021 2), YES imports, the done screen names the Rytm project to save. The Rytm's
    card image is a clone (cp -c) of the tag's."""
    card = os.path.join(OUT, f"project_{TAG}.img")
    if os.path.exists(card):
        os.remove(card)
    subprocess.run(["cp", "-c", os.path.join(EMU, e.card), card], check=True)
    keep = e.card
    e.card = card
    os.environ["RUI_BUNDLE"] = "1"
    try:
        # (after the connect splash: the tag's base Rytm has been up long enough for one, 3 s from the hello)
        T = 3500
        r = e.run("project", T + 4300, [(T + 300, "press", "TRK", 300), (T + 400, "press", "YES"), (T + 800, "press", "LEFT"),
                                        (T + 1100, "press", "T1"), (T + 1300, "press", "YES"), (T + 2100, "press", "YES"),
                                        (T + 2300, "press", "T2"), (T + 2600, "press", "YES"), (T + 3300, "press", "YES")])
    finally:
        os.environ.pop("RUI_BUNDLE", None)
        e.card = keep
        if os.path.exists(card):
            os.remove(card)
    masks = ev(r, tx="mask")
    pm = [m for m in masks if m["encs"] == [0, 7]]
    check("project: LEFT on SIDECAR opens PROJECT: its mask takes encoders A and H and the trig keys T1..T5",
          bool(pm) and all(k in pm[-1]["keys"] for k in range(5)), [m["encs"] for m in masks])
    lst = texts_of("project", r, T / 1000 + 1.05)
    check("project: the list: PROJECT, the card, both projects", "PROJECT" in lst and any("JAM 1021" in t for t in lst) and
          any("LIVE SET" in t for t in lst), lst)
    ask = texts_of("project", r, T / 1000 + 1.25)
    check("project: T1 asks EXPORT JAM 1021 TO CARD?", any("EXPORT JAM 1021" in t for t in ask) and any("TO CARD?" in t for t in ask), ask)
    ph = [x["bundle"] for x in r["events"] if "bundle" in x]
    check("project: YES exported (DONE), then the import: VERIFY, READY, STAGE, DONE",
          ph.count(14) >= 2 and 8 in ph and 6 in ph and 10 in ph, ph)
    summ = texts_of("project", r, T / 1000 + 3.25)
    check("project: the summary: IMPORT AS JAM 1021 2?", any("IMPORT AS JAM 1021 2?" in t for t in summ), summ)
    end = texts_of("project", r)
    check("project: done: SAVE THE RYTM PROJECT AS JAM 1021 2", any("SAVE THE RYTM PROJECT AS" in t for t in end) and
          any("JAM 1021 2" in t for t in end), end)
    r = os.path.join(e.core_card, ".rytm")   # (its pool lists' folder, or any other project file the bundle carried)
    got = [p for p in ("projects/JAM 1021 2", "slots/JAM 1021 2.txt", "seq/JAM 1021 2", "lfo/JAM 1021 2.txt", "songs/JAM 1021 2")
           if os.path.exists(os.path.join(r, p))]
    check("project: the core's card has the imported project (JAM 1021 2's files) and the bundle in /Bundles",
          got and any(f.endswith(".sidecar.zip") for f in os.listdir(os.path.join(e.core_card, "Bundles"))), got)
    panel_check("project", r)
    shot("project", os.path.join(OUT, "project.png"))


# ---- rui1051 (rytm-mods 0021 / 0026 1051; RUI_TAG = rui1051): LIVE IN (machine 48, voice flag 0x78) and CHOP (49, 0x18)
# in the Rytm's machine list; CHOP's slices from the pads in CHRO, its editor from the SRC view, LAZY from the pads
CHOP_DOWNS = 28   # BD PLASTIC -> CHOP (49): the list's 29th (rytm-mods tools/emu/us_ui.json: DAISYSP 29 since 1051)
CHOP_ENV = dict(RUI_FRESH="1", RUI_SAMPLES="1", RUI_RENDER="1",
                RUI_NM="pool=smp:set:A010:/Samples/Drums/break.wav")   # SRC's default 10; the voices rendered


def chop_run(name, ms, evs):
    if not os.path.exists(os.path.join(EMU, f"sections/us_{TAG}.json")):
        check(f"{name}: the {TAG} base in rytm-emu (rytm-mods tools/emu/us_base.sh {TAG})", False)
        return None, None
    e = Emu(TAG)
    ks = pool_env(**CHOP_ENV)
    try:
        r = e.run(name, ms, evs)
    finally:
        for k in ks:
            os.environ.pop(k, None)
    return r, open(os.path.join(OUT, f"{name}.link.txt"), errors="replace").read()


def chop_keys(t=300, downs=CHOP_DOWNS):
    evs = []
    for k in ["SRC", "SRC"] + ["DOWN"] * downs + ["YES"]:
        evs.append((t, "press", k))
        t += 400
    return evs, t


def t_chopmach(_e):
    """rui1051: the machine list's CHOP (49) on BD: its reports carry 0x18 and the core plays CHOP (a BD pad: a slice of
    the break in A010); one up, LIVE IN (48): 0x78, the core's LIVE IN."""
    evs, t = chop_keys()
    evs += [(t + 1500, "press", "BD")]
    t2 = t + 2300
    for k in ["SRC", "SRC", "UP", "YES"]:
        evs.append((t2, "press", k))
        t2 += 400
    evs += [(t2 + 600, "press", "BD")]
    r, lt = chop_run("chopmach", t2 + 1500, evs)
    if not r:
        return
    flags = [(x["voice"], x["machine"]) for x in ev(r, rx="voice")]
    print(f"  voice reports' machines: {flags}")
    check("chopmach: BD from the list's CHOP: voice 0 reported with 0x18", (0, 0x18) in flags, flags)
    trig = re.findall(r"trig voice 0 CHOP slice \d+ of \d+ \(note \d+\)", lt)
    print(f"  the core: {trig[:2]}")
    check("chopmach: the BD pad: the core's CHOP trig on voice 0 (a slice of the break)", bool(trig), trig)
    check("chopmach: one up the list: LIVE IN, voice 0 reported with 0x78", (0, 0x78) in flags, flags)


def t_chopchro(_e):
    """rui1051: BD on CHOP, CHRO: the pads BD SD RS (notes 60 61 62) play slices 1 2 3 (SLC 1 + the note - 60)."""
    evs, t = chop_keys()
    t0 = t + 2000
    evs += [(t0, "press", "CHRO"), (t0 + 500, "press", "BD"), (t0 + 900, "press", "SD"), (t0 + 1300, "press", "RS")]
    r, lt = chop_run("chopchro", t0 + 2200, evs)
    if not r:
        return
    sl = re.findall(r"(?:trig|live) voice 0 CHOP slice (\d+)[^\n]*?(?:note|key) (\d+)", lt)
    print(f"  the core's slices: {sl}")
    got = [(int(a), int(b)) for a, b in sl]
    check("chopchro: CHRO pads 60 61 62 -> slices 1 2 3", [x for x in got if x[1] >= 60][-3:] == [(1, 60), (2, 61), (3, 62)], got)


def t_choplazy(_e):
    """rui1051: BD on CHOP; PUSH B on its SRC view opens the CHOP editor (the core's page CHOP EDIT); PUSH G arms LAZY (the
    break plays from its start); four BD pad taps 300 ms apart drop four markers (each the playhead at the tap less the
    latency, on a zero crossing); YES keeps them: 4 slices, spaced as the taps (300 ms, within 20 ms)."""
    evs, t = chop_keys()
    t0 = t + 2500
    taps = [t0 + 1200 + 300 * i for i in range(4)]
    evs += [(t0, "press", "PUSH_B"), (t0 + 700, "press", "PUSH_G")] + [(x, "press", "BD") for x in taps]
    evs += [(taps[-1] + 600, "press", "YES")]
    r, lt = chop_run("choplazy", taps[-1] + 1800, evs)
    if not r:
        return
    titles = sorted({x for x in texts_all("choplazy", r) if str(x).startswith("CHOP ")})
    print(f"  the page's titles: {titles}")
    check("choplazy: PUSH B on the SRC view opened the core's CHOP editor", "CHOP EDIT" in titles, titles)
    check("choplazy: PUSH G armed LAZY (the core's log)", "chop: LAZY on voice 0" in lt, "")
    m = re.findall(r"chop: LAZY: (\d+) slices at ([\d ]+)", lt)
    print(f"  LAZY: {m}")
    ok = bool(m) and int(m[-1][0]) == 4
    starts = [int(x) for x in m[-1][1].split()] if m else []
    gaps = [b - a for a, b in zip(starts, starts[1:])]
    print(f"  marker gaps {gaps} frames (taps 14400 apart)")
    check("choplazy: YES kept four slices", ok, m)
    check("choplazy: the markers spaced as the taps (300 ms each, within 20 ms)", len(gaps) == 3 and
          all(abs(g - 14400) <= 960 for g in gaps), gaps)
    shot("choplazy", os.path.join(OUT, "choplazy.png"))


# ---- per pattern poly (docs/superpowers/specs/2026-10-26-per-pattern-poly-design.md; RUI_TAG=rui1051, the real core's
# sequencer ticked: RUI_SEQ_TICK). The emulated kit's tracks are analog (the cleared kit: every 33 says 7F) except BD,
# which rui_link reports on PLAITS; BD is made a poly track in A01 by the console (RUI_SEQ_CMDS) with steps on 1 and 5.
POLYPAT_CMDS = "1.6:seq=track:1:on;1.65:seq=step:1:1:60;1.7:seq=step:1:5:64"
T0 = 2200   # (ms) the keys from here: the project's store filled, BD poly


def seq_log(r):
    """(t, mask) of every 2E the core sent, the Rytm's 2D 02 / 03 / 08 as (t, kind, body), the final status line."""
    masks = [(x["t"], x["body"][1] | x["body"][2] << 7) for x in ev(r, tx="2e") if len(x["body"]) >= 4]
    d2 = [(x["t"], x["body"][1], x["body"][2:]) for x in ev(r, rx="2d") if len(x["body"]) >= 2 and x["body"][1] in (2, 3, 8, 11)]
    st = [x["seq"] for x in r["events"] if "seq" in x]
    cmds = [(x["seqcmd"], x["out"]) for x in r["events"] if "seqcmd" in x]
    return masks, d2, (st[-1] if st else ""), cmds


def polypat_run(e, name, ms, evs, cmds=POLYPAT_CMDS):
    # the core's card needs /Samples (card_ok: without it the sequencer has no files - a switch would drop the edits);
    # RUI_RENDER: the voices rendered, so the sequencer's frames run (its planner and the switch prediction)
    os.makedirs(os.path.join(e.core_card, "Samples"), exist_ok=True)
    os.environ.update(RUI_SEQ_TICK="1", RUI_SEQ_IDLE="1", RUI_EMU_CLOCK="1", RUI_SEQ_CMDS=cmds, RUI_RENDER="1")
    try:
        r = e.run(name, ms, evs)
    finally:
        for k in ("RUI_SEQ_TICK", "RUI_SEQ_IDLE", "RUI_EMU_CLOCK", "RUI_SEQ_CMDS", "RUI_RENDER"):
            os.environ.pop(k, None)
    masks, d2, st, c = seq_log(r)
    print(f"  {name}: cmds {c}")
    print(f"  {name}: 2E masks {[(round(t, 3), hex(m)) for t, m in masks]}")
    print(f"  {name}: 2D {[(round(t, 3), k, list(b)) for t, k, b in d2]}")
    print(f"  {name}: {st[:160]}")
    shot(name, os.path.join(OUT, f"{name}.png"))
    return r, masks, d2, st


def t_polypat(e):
    """per pattern poly on the emulated Rytm with the real core: (new) BD poly in A01, A02 chosen (a new, empty pattern on
    the cleared kit): a 2E with mask 0, GRID REC on BD is the Rytm's own (no seq mode, no SEQ page) and T1 makes a stock
    trig; (off) TURN OFF on BD's SEQ page (YES, DOWN, YES, YES): mask 0, then GRID REC left and entered again: still off;
    (switch) playing A01, A02 queued, then A01 queued: each 2E with the next pattern's mask before the Rytm's 2D 03."""
    pre = [(1000, "press", "NO")]   # the connect splash ended
    t = T0
    r, masks, d2, st = polypat_run(e, "polypat-a01", t + 1000, pre + [(t, "press", "RECORD"), (t + 500, "press", "T9")])
    check("polypat a01: BD poly in A01: a 2E with BD (0x001)", any(m == 1 for _, m in masks), masks)
    check("polypat a01: GRID REC on BD is the Sidecar's (2D 08 seq mode on BD)", any(k == 8 and b[:1] == [1] for _, k, b in d2), d2)
    evs = pre + [(t, "press", "BANK_A", 200), (t + 50, "press", "T2"), (t + 600, "press", "RECORD"), (t + 1100, "press", "T9")]
    r, masks, d2, st = polypat_run(e, "polypat-new", t + 1600, evs)
    sw = [x for x, k, b in d2 if k == 2 and b[:1] == [1]]
    check("polypat new: the Rytm went to A02 (2D 02 now 1)", bool(sw), d2)
    after = [m for x, m in masks if sw and x >= sw[0] - 0.05]
    check("polypat new: A02 (new, empty) on the cleared kit: a 2E with mask 0 at the change", after[:1] == [0], (sw, masks))
    check("polypat new: GRID REC on BD is the Rytm's own (no 2D 08 seq mode on)",
          not any(k == 8 and b[:1] == [1] and x >= (sw[0] if sw else 0) for x, k, b in d2), d2)
    check("polypat new: T9 in GRID REC made a stock trig (the Rytm's 2D 0B DIRTY after it)",
          any(k == 11 and x >= (t + 1100) / 1000 for x, k, b in d2), d2)
    check("polypat new: the core says pattern 1, mask 0x000", "pattern 1, mask 0x000" in st, st[:80])
    # (off) BD's SEQ page (TRK + TRIG), its track menu: FUNC + DOWN (updown1029; DOWN ux1027), DOWN (TURN OFF), YES, YES;
    # the page closed (NO); GRID REC on, off, on again; T13. (FUNC 100 ms before DOWN: the mask naming DOWN first)
    off = [(t, "press", "TRK", 300), (t + 10, "press", "TRIG"), (t + 400, "press", "FUNC", 200), (t + 500, "press", "DOWN"),
           (t + 700, "press", "DOWN"),
           (t + 900, "press", "YES"), (t + 1100, "press", "YES")]
    r, masks, d2, st = polypat_run(e, "polypat-turnoff", t + 1500, pre + off)
    check("polypat turnoff: mask 0 after TURN OFF + YES", masks[-1:] and masks[-1][1] == 0 and any(m == 1 for _, m in masks),
          masks)
    again = off + [(t + 1300, "press", "NO"), (t + 1700, "press", "RECORD"), (t + 2100, "press", "RECORD"),
                   (t + 2500, "press", "RECORD"), (t + 2900, "press", "T13")]
    r, masks, d2, st = polypat_run(e, "polypat-offagain", t + 3400, pre + again)
    late = [m for x, m in masks if x > (t + 1200) / 1000]
    check("polypat offagain: GRID REC left and entered again: no 2E with BD after TURN OFF", not any(m & 1 for m in late),
          masks)
    check("polypat offagain: GRID REC is the Rytm's own (no 2D 08 seq mode on after TURN OFF)",
          not any(k == 8 and b[:1] == [1] and x > (t + 1200) / 1000 for x, k, b in d2), d2)
    check("polypat offagain: T13 made a stock trig (2D 0B DIRTY)", any(k == 11 and x >= (t + 2900) / 1000 for x, k, b in d2), d2)
    check("polypat offagain: the core says mask 0x000 (BD's slot off, its steps kept)", "mask 0x000" in st, st[:80])
    # (switch) PLAY A01 (BD poly), A02 queued (plain), then A01 queued again: each switch's mask before its 2D 03
    sw_ev = [(t, "press", "PLAY"), (t + 600, "press", "BANK_A", 200), (t + 650, "press", "T2"),
             (t + 3200, "press", "BANK_A", 200), (t + 3250, "press", "T1")]
    r, masks, d2, st = polypat_run(e, "polypat-switch", t + 6500, pre + sw_ev)
    sws = [(x, b[1]) for x, k, b in d2 if k == 3 and len(b) > 1]
    print(f"  polypat switch: switches {[(round(x, 3), p) for x, p in sws]}")
    check("polypat switch: two switches, A01 -> A02 -> A01", [p for _, p in sws][:2] == [1, 0], sws)
    for x, p in sws[:2]:
        want = 0 if p == 1 else 1
        before = [(y, m) for y, m in masks if y < x]
        ok = bool(before) and before[-1][1] == want and x - before[-1][0] < 0.25
        check(f"polypat switch: to A0{p + 1}: the 2E with mask {want:#x} went before the Rytm's 2D 03 (within 250 ms)", ok,
              (round(x, 3), [(round(y, 3), hex(m)) for y, m in before[-3:]]))
    check("polypat switch: the core says pattern 0, mask 0x001", "pattern 0" in st and "mask 0x001" in st, st[:80])


# ---- U1 NOTE EDIT (docs/SEQ-IDEAS.md U1; RUI_TAG=rui1051, the real core's sequencer): BD poly with S1 C4 E4 G4, S5 D4,
# S9 F4; its SEQ page by TRK + TRIG; DATA (encoder 8, caught by the page's mask) walks the notes, D PTCH, the DATA push
# (PUSH_LEVEL, 0x78) selects, E VEL on the selection, FUNC + NO deletes it
NOTEEDIT_CMDS = ("1.6:seq=track:1:on;1.62:seq=step:1:1:60;1.64:seq=step:1:1:64;1.66:seq=step:1:1:67;1.68:seq=step:1:5:62;"
                 "1.7:seq=step:1:9:65")


def noteedit_evs(t, upto):
    """the gestures from t (ms), those before upto"""
    def turn(at, enc, delta, count=1, spacing=150):
        return (at, "turn", f"{enc}:{delta}", count, spacing)
    evs = [(t, "press", "TRK", 300), (t + 10, "press", "TRIG"),       # BD's SEQ page
           turn(t + 700, 8, 1), turn(t + 1000, 8, 1, 3),              # DATA: lands on S1 C4; +3: S1 E4 G4, S5 D4
           turn(t + 1700, 3, 1, 2),                                   # D PTCH +2: E4
           (t + 2100, "press", "PUSH_LEVEL"),                         # selected
           # (the Rytm drops an encoder's turns for ~0.3 s after its push is let go [measured, rytm-mods 1050]: DATA
           # turned 0.7 s after the push)
           turn(t + 3300, 8, -1), (t + 3700, "press", "PUSH_LEVEL"),  # DATA -1: S1 G4, selected
           turn(t + 4100, 4, -2, 5),                                  # E VEL down on both
           (t + 4900, "press", "FUNC", 900), (t + 5050, "press", "NO"),   # FUNC + NO: asks (two notes) ..
           (t + 5500, "press", "NO"),                                    # .. FUNC + NO again: both deleted
           (t + 6200, "press", "SD"),                                    # SD (a plain track) selected: the page closes
           turn(t + 6700, 8, 1, 3), (t + 7300, "press", "PUSH_LEVEL")]   # DATA and its push: the Rytm's own again
    return [x for x in evs if x[0] < upto]


def t_noteedit(e):
    """U1 NOTE EDIT on the emulated Rytm with the real core: the SEQ page's mask catches DATA (encoder 8) and its push
    (0x78); DATA lands on S1's C4 and walks C4 E4 G4 D4 (S5); D turned: S5's note E4; the push selects it, DATA back to
    S1's G4, the push selects it too (SEL 2); E turned lowers both; FUNC + NO deletes both: S1 C4 E4 and S9 F4 left."""
    pre = [(1000, "press", "NO")]
    t = T0
    shots = [("noteedit-1-walk", t + 1700), ("noteedit-2-sel", t + 4800), ("noteedit-3-deleted", t + 6000),
             ("noteedit-4-plain", t + 7800)]
    res = {}
    for name, end in shots:
        e.core_card = tempfile.mkdtemp(prefix="rui_e2e_card_")   # each run from a fresh core card (no pattern saved before)
        res[name] = polypat_run(e, name, end, pre + noteedit_evs(t, end),
                                cmds=NOTEEDIT_CMDS + f";{(end - 150) / 1000:.2f}:seq=steps:1")
    r, masks, d2, st = res["noteedit-4-plain"]
    texts = sorted({x for n, _ in shots for x in texts_all(n, res[n][0]) if isinstance(x, str)})
    print(f"  texts: {texts[:60]}")
    cmds = [(x["seqcmd"], x["out"]) for x in r["events"] if "seqcmd" in x]
    steps = [o for c, o in cmds if c.startswith("seq=steps")]
    walk = [o for c, o in [(x["seqcmd"], x["out"]) for x in res["noteedit-1-walk"][0]["events"] if "seqcmd" in x]
            if c.startswith("seq=steps")]
    sel = [o for c, o in [(x["seqcmd"], x["out"]) for x in res["noteedit-2-sel"][0]["events"] if "seqcmd" in x]
           if c.startswith("seq=steps")]
    print(f"  steps after the walk: {walk}; after the selection: {sel}; after FUNC + NO: {steps}")
    check("noteedit: the page says NOTE EDIT", "NOTE EDIT" in texts, texts[:20])
    check("noteedit: DATA walked to S5 (S5 1/1 on the page)", any(x.startswith("S5 1/1") for x in texts), texts[:30])
    check("noteedit: the walk changed nothing", bool(walk) and walk[-1].strip().endswith("steps 1:60,64,67 5:62 9:65"), walk)
    check("noteedit: D PTCH +2 on the highlighted note: S5 holds E4 (64)", bool(sel) and " 5:64" in sel[-1], sel)
    check("noteedit: two selected (SEL 2)", "SEL 2" in texts, texts[:30])
    check("noteedit: E turned on the selection: VELOCITY .. ALL", any(x.startswith("VELOCITY V") and x.endswith("ALL") for x in texts),
          [x for x in texts if "VEL" in x])
    check("noteedit: the first FUNC + NO asked (two notes)", any(x.startswith("F+NO AGAIN") for x in texts), texts[:30])
    t_sd = (T0 + 6200) / 1000
    late = [x for x in r["events"] if x.get("rx") in ("enc", "key") and x["t"] > t_sd + 0.2]
    print(f"  after SD: {late}")
    check("noteedit: on SD (plain) DATA and its push are the Rytm's (none reached the core)",
          not any((x.get("enc") == 8) or (x.get("key") == 120) for x in late), late)
    check("noteedit: FUNC + NO deleted the selection: S1 C4 E4, S9 F4 left (S5 gone)",
          bool(steps) and steps[-1].strip().endswith("steps 1:60,64 9:65"), steps)
    for name, _ in shots:
        shot(name, os.path.join(OUT, f"{name}.png"))


TESTS = dict(polypat=t_polypat, noteedit=t_noteedit, upmapover=t_upmapover, connect=t_connect, sidecar=t_sidecar, plaits=t_plaits, status=t_status, leave=t_leave, gone=t_gone, keys=t_keys,
             splash=t_splash, latehello=t_latehello, latency=t_latency, scope=t_scope, wave=t_wave, rec=t_rec, play=t_play, gfx=t_gfx, extmach=t_extmach,
             machines=t_machines, sdvoice=t_sdvoice, srcgfx=t_srcgfx, srcwave=t_srcwave, noexit=t_noexit, lfo=t_lfo, upamp=t_upamp, polycfg=t_polycfg, polymake=t_polymake,
             srcview=t_srcview, srcturn=t_srcturn, srcsmp=t_srcsmp, srcreplug=t_srcreplug, srclist=t_srclist, srcbrowse=t_srcbrowse, project=t_project,
             poolset=t_poolset, poolrefuse=t_poolrefuse, poolp3=t_poolp3, poolnames=t_poolnames, pooldsp=t_pooldsp, dspbank=t_dspbank, dspsrcyes=t_dspsrcyes,
             chopmach=t_chopmach, chopchro=t_chopchro, choplazy=t_choplazy,
             **{f"poolset-{k}": (lambda k: lambda e: t_poolset(e, (k,)))(k) for k in ("gr", "wt", "ms")})
POOLS = ["srclist", "srcbrowse", "poolset", "poolrefuse", "poolp3", "poolnames", "pooldsp", "dspbank"]   # "pools": these
DEFAULT = ["connect", "sidecar", "plaits", "status", "leave", "gone", "keys", "splash", "latehello", "scope", "wave", "rec", "play", "gfx", "extmach",
           "machines", "srcgfx", "srcwave", "noexit", "lfo"]


def main():
    names = sys.argv[1:] or DEFAULT
    names = [x for n in names for x in (POOLS if n == "pools" else [n])]
    e = Emu()
    for n in names:
        print(f"[{n}]", flush=True)
        TESTS[n](e)
    print("ALL PASS" if not FAILS else f"FAILED: {FAILS}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
