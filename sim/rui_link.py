#!/usr/bin/env python3
"""The engine core (core/build/libcore.dylib: the code the Pi and the Teensy run) as rytm-emu's Overbridge host for the
remote UI (core/ui/remote.h; rytm-mods 0026-remote-ui): the Rytm's USB MIDI into the core, the core's SysEx (the
remote UI's 38 / 39 / 3C, and the rest) back into the Rytm, the core's remote_tick on the emulator's clock (the frame
requests: emulated ms = frames / 48), and a log of what went each way.

    rui_link.py --card DIR [--link HOST:PORT] [--log OUT.jsonl] [--frames DIR] [--stop-at S] [--secs S]

--log: one JSON object a line, "t" = emulated seconds since the link started:
  {"rx": "mode", "on": 1, "reason": 0}        the Rytm's 30 (31 "key", 32 "enc" alike)
  {"tx": "commit", "seq": n, "rects": [[c0, p0, nc, np], ...], "tiles": k, "frame": i}   a frame the core sent
  {"tx": "alive"}                              a 3C
  {"tx": "mask", "encs": [..], "keys": [..]}   1022: a 37 catch mask the core sent
  {"rx": "0x35", "up": n, "caps": c}           1023: the Rytm's hello; {"tx": "0x36", "body": [1, ticks]} the splash
  {"rx": "rt", "b": 250}                       1022: MIDI transport from the Rytm (FA start, FB continue, FC stop)
  {"rx": "note", "status": 0x9n, "note": k}    1022: a note-on from the Rytm (a pad)
  {"param": [voice, k, before, after]}         a voice parameter the core holds now differs from before an encoder report
  {"stats": [...]}                             core_c_rui_stats at the end (opens closes frames tiles rects bytes
                                               keepalives keys encs edits widgets bad draw_us_max draw_us_sum page voice
                                               texts echoes split masks hellos splashes machines)
--frames DIR: each committed frame (the core's copy of what the Rytm's shadow holds) as DIR/frame_<i>.bin, 1024 B.
--stop-at S: stop answering and close at emulated S (the Sidecar gone). One process per run (the core keeps state).
RUI_TAP=synth: the remote UI's input tap (core/ui/extra.h: SCOPE, the meters) gets sim/rui_live.py's synthetic pattern
on the emulated clock (the emulator's analog voices are stubbed: its own IN stream is silent); otherwise the
emulator's IN frames ("R") go to the tap while a page reads it.
RUI_VOICE1="M:P1,..,P16": after every report from the Rytm, voice 1 is set to machine M (0x20 DSP SAMPLE) with these
P1..P16 (the emulated Rytm's kit has no Sidecar machine on it); with it the card gets sim/rui_live.py's samples
(/Samples/Drums/break.wav, pad.wav) as pool slots 1 and 2. CORE_SAMPLE_RAM_MB: the sample RAM (GRAIN).
RUI_SAMPLES=1: the card with those samples, no voice forced (the real path: the Rytm's reports decide).
RUI_FRESH=1: no voice reported at the start (the core shows a voice's page only once the Rytm reported its machine;
otherwise voice 1 is reported on PLAITS first, the emulated kit's tracks being analog ones that never report).
RUI_RENDER=1: the voices rendered for the frames the emulator asks for ("N"; sent when the hello takes voices, else
discarded): a machine's time runs with the emulated audio clock (CHOP's LAZY playhead, rui_e2e choplazy).
RUI_NM="pool=smp:fill:B:/Banks;browse=gr:cd:/E2E": console commands of the pools / the browser at the start and after
the Rytm names its project, each one's steps done before the next. RUI_NO_SRCVIEW=1: the Rytm's 29 (SRC view on / off)
never reaches the core, so the Rytm's own stock SRC page (its knob texts, cu_text) stays on the panel.
RUI_EMU_CLOCK=1: the core's clock is the emulated one (core_c_fake_time, as RUI_LFO does): its timers (a note's 2 s)
in emulated time, not the Mac's (the emulator runs faster than real time).
RUI_SEQ_CMDS="S:cmd;S:cmd..": the sequencer's console commands (core_c_seq_command, seq=...) at emulated second S each,
logged as {"seqcmd": cmd, "out": reply}; with RUI_SEQ_TICK every 2E the core sends is logged as {"tx": "2e", "body": [..]}
(the seq tracks the Rytm captures) and every 2D from the Rytm as {"rx": "2d", "body": [..]}; at the end {"seq": status}.
RUI_SEQ_IDLE=1 (with RUI_SEQ_TICK): 4 idle passes (core_c_idle: the project's store fill, the sequencer's prefetch and
saves) every link tick too.
RUI_SEQ_TICK=1: the sequencer's tick (core_c_poly_tick: seq, rytm_dump, song) every link tick, as the Pi's control loop
(the Rytm's transport reaches rytm_dump::is_playing: the pools' "PLAYING" guard).
DSP SAMPLE's 14 table from the core is logged as {"tx": "0x14", "replace": r, "rows": [[slot, folder, file, name]..]}.
RUI_DBG_SX=1: every SysEx from the Rytm logged ({"sx": first 12 bytes as hex}); a voice report whose voice byte
changed is always logged as {"rx": "voice", "cmd": 3|4, "voice": v, "machine": flags}.
RUI_REC=1: a model of the Pi's recorder behind the REC page (core/ui/extra.h RecInfo): the page's requests taken every
tick and obeyed (logged as {"rec": "start"|"stop"|"arm"|"disarm"}), its state handed over every 200 ms (emulated); and of
its playback (PlayInfo: two synthetic takes, newest first, take-0007 with MASTER BD SD, 90 s): B pushed {"play":
"start"|"stop"}, B turned {"play_ch": name}, C {"play_take": name}, D {"play_level": n}; a recording stops it.
RUI_PLAY_TAKES=N: the playback model with N takes (session_NNN, newest first, 1..13 channels each).
RUI_LFO="lfo=1:tgt:fx1;lfo=1:par:0;...": the LFOs (core/lfo.h) set by these console commands at the start; the core's
clock is then the emulated one (core_c_fake_time) and its LFO tick runs every link tick. Always: the Rytm's MIDI clock /
transport / song position go to the core (fx::clock_in -> the LFOs' sync), its notes and CCs to lfo::midi_in, and every
CC the core sends the Rytm is logged as {"tx": "cc", "ch": c, "cc": n, "v": value}; at the end {"lfo": <the LFO JSON>}.
"""
import argparse
import ctypes
import json
import os
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import protocol as P  # noqa: E402
from coreengine import CoreEngine  # noqa: E402

HELLO_MIDI, HELLO_NO_R, HELLO_VOICES = 1, 2, 4
SR = 48000


class Host:
    def __init__(self, card, log, frames_dir):
        self.voice1 = None
        v1 = os.environ.get("RUI_VOICE1")
        if v1:
            m, ps = v1.split(":")
            self.voice1 = bytes([0xF0, 0x7D, 0x52, 0x54, 0x03, int(m, 0)] + [b for k in ps.split(",") for b in (int(k), 0)] +
                                [0xF7])
        if v1 or os.environ.get("RUI_SAMPLES") == "1":   # the samples as pool slots 1, 2 (RUI_SAMPLES: no voice forced)
            from rui_live import card_with_samples
            import shutil
            src = card_with_samples()
            shutil.copytree(os.path.join(src, "Samples"), os.path.join(card, "Samples"), dirs_exist_ok=True)
        self.bundle = os.environ.get("RUI_BUNDLE") == "1"
        if self.bundle:   # bundles phase 3 (the Rytm's PROJECT page): two projects with samples on the core's card
            import struct as _st
            def put(path, data):
                full = os.path.join(card, path.lstrip("/"))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "wb" if isinstance(data, bytes) else "w") as f:
                    f.write(data)
            def wav(n, k):
                pcm = bytes((i * k) & 0xFF for i in range(2 * n))
                return (b"RIFF" + _st.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + _st.pack("<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16) +
                        b"data" + _st.pack("<I", len(pcm)) + pcm)
            put("/Samples/Live/loop.wav", wav(60000, 7))
            put("/Samples/Live/hit.wav", wav(9000, 3))
            put("/.rytm/pools/LIVE SET.txt", "Live/loop.wav\nLive/hit.wav\n")
            put("/.rytm/seq/LIVE SET/p001.sqp", b"SQP1\x03" + b"\x11" * 59)
            put("/.rytm/pools/JAM 1021.txt", "Live/hit.wav\n")
        self.core = CoreEngine(card)
        lib = self.lib = self.core.lib
        if self.bundle:
            lib.core_c_bundle_setup()
            lib.core_c_bundle_tick.argtypes = [ctypes.c_int]
            lib.core_c_bundle_msg.restype = ctypes.c_char_p
            self.bundle_phase = -1
        lib.core_c_rui_tick.argtypes = [ctypes.c_uint32]
        lib.core_c_rui_frame.argtypes = [ctypes.c_char_p]
        lib.core_c_rui_stats.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
        lib.core_c_rui_status.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.core_c_rui_frame_texts.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.core_c_rui_frame_preview.argtypes = [ctypes.c_char_p]
        lib.core_c_rui_status2.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_uint32,
                                           ctypes.c_char_p]
        lib.core_c_rui_legacy.argtypes = [ctypes.c_int]
        lib.core_c_tap_in.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        lib.core_c_rui_rec.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                       ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
        self.rec = None
        if os.environ.get("RUI_REC") == "1":
            lib.core_c_rui_play.argtypes = [ctypes.c_int] * 6 + [ctypes.c_uint32, ctypes.c_uint32] + [ctypes.c_char_p] * 3
            lib.core_c_rui_play_take.argtypes = [ctypes.POINTER(ctypes.c_int)]
            self.rec = dict(state=1, t0=0.0, secs=0, take=7, last=-1.0)
            # the playback model: a synthetic take (MASTER, BD, SD; 90 s) and an older one, newest first
            self.play = dict(state=1, level=100, ch=0, take=0, pos=0.0, t=0.0,
                             takes=[("take-0007", ["MASTER", "BD", "SD"]), ("take-0006", ["BD", "CH-OH"])])
            nt = int(os.environ.get("RUI_PLAY_TAKES", "0"))   # many takes, long names, 1..13 channels each
            if nt:
                chs = ["MASTER", "MAIN_L", "MAIN_R", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB", "IN_L", "VOICE1"]
                self.play["takes"] = [(f"session_{nt - i:03d}", chs[i % 3:i % 3 + 1 + i % 13]) for i in range(nt)]
            self.rec_push()
        self.synth = None
        if os.environ.get("RUI_TAP") == "synth":
            from rui_live import pattern
            self.synth, self.synth_at = pattern(8.0), 0
        lib.core_c_voice_param.restype = ctypes.c_float
        lib.core_c_voice_param.argtypes = [ctypes.c_int, ctypes.c_int]
        self.track0 = None if os.environ.get("RUI_FRESH") == "1" else (self.voice1[5] & 0x78 if self.voice1 else 0x00)
        if os.environ.get("RUI_FRESH") != "1":   # voice 1 reported on PLAITS, as its trig would (the emulated kit's
            ps = self.params()[0]                 # tracks are analog ones that never report): one voice page
            self.core.report(bytes([0xF0, 0x7D, 0x52, 0x54, 0x03, 0] + [b for x in ps for b in (x & 0x7F, 0)] + [0xF7]))
        lib.core_c_rui_status(1, 0, -1, -1, -1)
        if os.environ.get("RUI_LEGACY") == "1":   # the 1020 host (50 ms frames, no echo, one commit): before / after
            lib.core_c_rui_legacy(1)
        st = os.environ.get("RUI_STATUS")          # "rec,secs,eth,wifi,a.b.c.d,U1|U2|.." for the SYSTEM page
        if st:
            rec, secs, eth, wifi, ip, users = st.split(",", 5)
            a = [int(x) for x in ip.split(".")]
            lib.core_c_rui_status(1, 0, 12, 34, 5)
            lib.core_c_rui_status2(int(rec), int(secs), int(eth), int(wifi), a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3],
                                   users.replace("|", ",").encode())
        # "sendknob=1:0:8;..." core sendfx commands for the SIDECAR page's FX (tests: the FX the Rytm's kit does not have)
        self.samples = bool(self.voice1) or os.environ.get("RUI_SAMPLES") == "1"
        if self.samples:
            for slot, f in ((1, 0), (2, 1)):
                self.core.report(P.pool_assign(slot, 1, f))
        if self.voice1:
            self.core.report(self.voice1)
        self.sendfx = os.environ.get("RUI_SENDFX")
        lib.core_c_fake_time.argtypes = [ctypes.c_int, ctypes.c_uint64]
        lib.core_c_lfo_tick.argtypes = [ctypes.c_uint32]
        lib.core_c_lfo_midi.argtypes = [ctypes.c_int] * 3
        lib.core_c_lfo_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        lib.core_c_lfo_json.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.core_c_sendfx_clock.argtypes = [ctypes.c_int, ctypes.c_int]
        self.lfo = os.environ.get("RUI_LFO")
        # rui1038: RUI_NM="pool=smp:fill:B:/Banks;..." console commands of the pools / browser (core_c_nm_command: pool=, browse=) at the
        # start and after the Rytm names its project (the pools are the project's)
        lib.core_c_nm_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        self.nm_cmds = os.environ.get("RUI_NM")
        self.render_on = os.environ.get("RUI_RENDER") == "1"   # rui1051: CHOP's LAZY playhead needs the voices rendered
        self.no_view = os.environ.get("RUI_NO_SRCVIEW") == "1"
        self.seq_tick = os.environ.get("RUI_SEQ_TICK") == "1"
        self.seq_idle = os.environ.get("RUI_SEQ_IDLE") == "1"
        lib.core_c_seq_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        self.seq_cmds = sorted((float(x.split(":", 1)[0]), x.split(":", 1)[1])
                               for x in os.environ.get("RUI_SEQ_CMDS", "").split(";") if ":" in x)
        self.emu_clock = os.environ.get("RUI_EMU_CLOCK") == "1"
        if self.emu_clock:
            lib.core_c_fake_time(1, 1_000_000)
        if self.nm_cmds:
            self.nm_setup()
        if self.lfo:
            lib.core_c_fake_time(1, 1_000_000)
            rb = ctypes.create_string_buffer(200)
            for c in self.lfo.split(";"):
                lib.core_c_lfo_command(c.encode(), rb, 200)
        lib.core_c_sendfx_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        self.log = open(log, "w") if log else None
        self.frames_dir = frames_dir
        if frames_dir:
            os.makedirs(frames_dir, exist_ok=True)
        self.frames, self.nframe = 0, 0
        self.out = []
        self.vb_seen = {}

    def t(self):
        return round(self.frames / SR, 6)

    def note(self, **kw):
        if self.log:
            self.log.write(json.dumps(dict(t=self.t(), **kw)) + "\n")
            self.log.flush()

    def params(self):
        return [[int(self.lib.core_c_voice_param(v, k)) for k in range(16)] for v in range(8)]

    def nm_setup(self):
        rb = ctypes.create_string_buffer(300)
        for _ in range(200):   # the pools read first (srclib's steps)
            self.lib.core_c_idle(10)
        for c in self.nm_cmds.split(";"):
            self.lib.core_c_nm_command(c.encode(), rb, 300)
            if hasattr(self, "frames"):   # (at __init__ the log is not open yet)
                self.note(nm=c, out=rb.value.decode(errors="replace"))
            else:
                print(f"rui_link: {c}: {rb.value.decode(errors='replace')}", flush=True)
            for _ in range(200):   # each command's steps done before the next (a fill's listing, then browse=..:cd)
                self.lib.core_c_idle(10)

    def take(self):
        """The core's SysEx since the last call: logged (remote UI ones decoded), queued for the Rytm."""
        buf = (ctypes.c_uint8 * 4096)()
        rects, texts = [], 0
        while True:
            k = self.lib.core_c_take_sysex(buf, 4096)
            if not k:
                break
            m = bytes(buf[:k])
            self.out.append(m)
            if len(m) == 3 and m[0] & 0xF0 == 0xB0:   # an LFO's CC (core/lfo.h)
                self.note(tx="cc", ch=m[0] & 15, cc=m[1], v=m[2])
                continue
            r = P.parse(m)
            if not r:
                continue
            cmd, body = r
            if cmd == 0x2E and self.seq_tick:
                self.note(tx="2e", body=list(body))
            if cmd == P.RUI_RECT:
                rects.append(list(body[2:6]))
            elif cmd == P.RUI_TEXT:
                texts += 1
            elif cmd == P.RUI_COMMIT:
                fb = ctypes.create_string_buffer(1024)
                self.lib.core_c_rui_frame(fb)
                if self.frames_dir:
                    open(os.path.join(self.frames_dir, f"frame_{self.nframe}.bin"), "wb").write(fb.raw)
                    pv = ctypes.create_string_buffer(1024)
                    self.lib.core_c_rui_frame_preview(pv)
                    open(os.path.join(self.frames_dir, f"preview_{self.nframe}.bin"), "wb").write(pv.raw)
                    tb = ctypes.create_string_buffer(36 * 64)
                    n = self.lib.core_c_rui_frame_texts(tb, 64)
                    items = [[tb.raw[36 * i], tb.raw[36 * i + 1], tb.raw[36 * i + 2], tb.raw[36 * i + 3],
                              tb.raw[36 * i + 4:36 * i + 36].rstrip(b"\0").decode()] for i in range(n)]
                    json.dump(items, open(os.path.join(self.frames_dir, f"texts_{self.nframe}.json"), "w"))
                self.note(tx="commit", seq=body[1], rects=rects, tiles=sum(a[2] * a[3] for a in rects),
                          texts=texts, frame=self.nframe)
                self.nframe += 1
                rects, texts = [], 0
            elif cmd == P.RUI_KEEPALIVE:
                self.note(tx="alive")
            elif cmd == P.RUI_MASK:
                mk = P.decode_rui_mask(body)
                self.note(tx="mask", encs=sorted(mk[0]) if mk else None, keys=sorted(mk[1]) if mk else None)
            elif P.RUI_FIRST <= cmd <= P.RUI_LAST or cmd in (P.RUI_NAMES, P.RUI_WRITE, P.RUI_BANK_LIST):   # (rui1038: 47 / 48; rui1040: 4A)
                self.note(tx=f"{cmd:#x}", body=list(body))
            elif cmd == P.POOL_TABLE:   # DSP SAMPLE's 14 table (the pools: bank A of SAMPLES)
                rep, rows = P.decode_pool_table(body)
                self.note(tx="0x14", replace=rep, rows=[list(x) for x in rows])

    def from_rytm(self, msg):
        r = P.parse(msg)
        before = None
        if r and r[0] == P.RUI_BANK_LIST:   # rui1038: the bank list opened / closed on the Rytm
            self.note(rx="banklist", **(P.decode_bank_list(r[1]) or {"raw": msg.hex()}))
        if r and (P.RUI_FIRST <= r[0] <= P.RUI_LAST or r[0] in (P.RUI_SRC_VIEW, P.RUI_SRC_VALS, P.RUI_SRC_DIAG)):
            m = P.decode_rui(r[0], r[1])
            kind = {P.RUI_MODE: "mode", P.RUI_KEY: "key", P.RUI_ENC: "enc", P.RUI_MACHINES: "machines",
                    P.RUI_SRC_VIEW: "srcview", P.RUI_SRC_VALS: "srcvals", P.RUI_SRC_DIAG: "srcdiag"}.get(r[0], f"{r[0]:#x}")
            self.note(rx=kind, **(m or {"raw": msg.hex()}))
            if r[0] == P.RUI_ENC:
                before = self.params()
        if r and r[0] == 0x2D and self.seq_tick:
            self.note(rx="2d", body=list(r[1]))
        if os.environ.get("RUI_DBG_SX"):
            self.note(sx=msg[:12].hex())
        if r and r[0] in (0x03, 0x04) and r[1]:   # a voice report: its voice byte (the machine the Rytm's track plays)
            vb = r[1][0]
            if self.vb_seen.get(vb & 7) != vb:
                self.vb_seen[vb & 7] = vb
                self.note(rx="voice", cmd=r[0], voice=vb & 7, machine=vb & 0x78)
        if not (self.no_view and r and r[0] == P.RUI_SRC_VIEW):   # RUI_NO_SRCVIEW: the core never hears 29 (no view)
            self.core.report(msg)
        # 1025+ (the Rytm's 33 decides the voice pages): the emulated kit's analog BD stands in for the track the link
        # puts on voice 1 (PLAITS, or RUI_VOICE1's machine) - a 33 naming track 1 analog (7F) is followed by one naming
        # it on that machine; a Sidecar machine the Rytm names (the SRC list) is left as it is
        if self.track0 is not None and r and r[0] == P.RUI_MACHINES and m and m["t0"] == 0 and m["tracks"][0][1] == 0x7F:
            self.core.report(P.rui_machines(0, [(m["tracks"][0][0], self.track0)]))
        if self.lfo and r and r[0] == 0x3B and not getattr(self, "lfo_kit", False):   # 1026: the Rytm's kit loads its
            self.lfo_kit = True                                                       # LFOs (none on this card): ours again
            rb = ctypes.create_string_buffer(200)
            for c in self.lfo.split(";"):
                self.lib.core_c_lfo_command(c.encode(), rb, 200)
        if self.lfo and r and r[0] == 0x10 and not getattr(self, "lfo_project", False):   # the Rytm's project loads its
            self.lfo_project = True                                                       # LFOs (none on this card): ours again
            rb = ctypes.create_string_buffer(200)
            for c in self.lfo.split(";"):
                self.lib.core_c_lfo_command(c.encode(), rb, 200)
        if self.nm_cmds and r and r[0] == 0x10 and bytes(r[1]) != getattr(self, "nm_project", None):
            self.nm_project = bytes(r[1])   # rui1038: the Rytm's project: its pools read again - our set-up again (only
            self.nm_setup()                 # when it names another: it sends 0x10 again after kit writes, and a set-up
                                            # mid-run would move the browser the test is in)
        if self.samples and r and r[0] == 0x10:   # the Rytm's project: its pool replaces ours - the samples again
            for slot, f in ((1, 0), (2, 1)):
                self.core.report(P.pool_assign(slot, 1, f))
        if self.voice1 and r and r[0] in (0x03, 0x04, 0x10):   # the Rytm's report of a voice: voice 1 back on its machine
            self.core.report(self.voice1)
        if self.sendfx:   # after every report: the Rytm's own FX track reports set the knobs too
            rb = ctypes.create_string_buffer(400)
            for c in self.sendfx.split(";"):
                self.lib.core_c_sendfx_command(c.encode(), rb, 400)
        if before is not None:
            after = self.params()
            for v in range(8):
                for k in range(16):
                    if after[v][k] != before[v][k]:
                        self.note(param=[v, k, before[v][k], after[v][k]])
        self.take()

    def tap(self, n):
        """n frames of the synthetic stream into the tap, in 7-frame blocks (the Overbridge block)."""
        if self.synth is None or not self.lib.core_c_tap_on():
            return
        import numpy as np
        for i in range(0, n, 7):
            k = min(7, n - i)
            at = self.synth_at % (len(self.synth) - 8)
            b = np.ascontiguousarray(self.synth[at:at + k])
            self.lib.core_c_tap_in(b.ctypes.data, k, 12)
            self.synth_at += k

    def rec_push(self):
        r = self.rec
        if r["state"] == 3:
            r["secs"] = int(self.frames / SR - r["t0"])
        self.lib.core_c_rui_rec(r["state"], r["secs"], 23 * 1024, 30 * 1024, 1152, 0x3FF,
                                f"take-{r['take']:04d}".encode(), b"")
        p = self.play
        name, chs = p["takes"][p["take"]]
        self.lib.core_c_rui_play(p["state"], p["level"], p["ch"], len(chs), p["take"], len(p["takes"]),
                                 int(p["pos"] * 1000), 90000, name.encode(), chs[p["ch"]].encode(), b"")

    def rec_tick(self):
        """The recorder model: the page's request obeyed, the state handed over every 200 ms or after a request."""
        r = self.rec
        q = self.lib.core_c_rui_rec_take()
        names = {1: "start", 2: "stop", 3: "arm", 4: "disarm"}
        if q:
            self.note(rec=names.get(q, q))
            if q == 1 and r["state"] in (1, 2):
                r.update(state=3, t0=self.frames / SR, secs=0, take=r["take"] + 1)
            elif q == 2 and r["state"] == 3:
                r["state"] = 1
            elif q == 3 and r["state"] == 1:
                r["state"] = 2
            elif q == 4 and r["state"] == 2:
                r["state"] = 1
        t = self.frames / SR
        p, a = self.play, (ctypes.c_int * 4)()
        got = self.lib.core_c_rui_play_take(a)
        if p["state"] == 2:
            p["pos"] += t - p["t"]
        p["t"] = t
        if r["state"] == 3 and p["state"] == 2:   # a take records: playback stops
            p.update(state=1, pos=0.0)
            self.note(play="stop", why="recording")
        if got:
            tog, dch, dtake, dlev = list(a)
            if dlev:
                p["level"] = max(0, min(127, p["level"] + dlev))
                self.note(play_level=p["level"])
            if dtake:
                p.update(take=max(0, min(len(p["takes"]) - 1, p["take"] + dtake)), ch=0, state=1, pos=0.0)
                self.note(play_take=p["takes"][p["take"]][0])
            if dch:
                p["ch"] = max(0, min(len(p["takes"][p["take"]][1]) - 1, p["ch"] + dch))
                self.note(play_ch=p["takes"][p["take"]][1][p["ch"]])
            if tog and r["state"] != 3:
                p.update(state=1 if p["state"] == 2 else 2, pos=0.0)
                self.note(play="start" if p["state"] == 2 else "stop")
        if q or got or t - r["last"] >= 0.2:
            r["last"] = t
            self.rec_push()

    def tick(self):
        if self.bundle:   # core 0's loop: the feed and a bundle step (a few a block: the emulator runs fast)
            ph = self.lib.core_c_bundle_tick(4)
            if ph != self.bundle_phase:
                self.bundle_phase = ph
                self.note(bundle=ph, msg=self.lib.core_c_bundle_msg().decode(errors="replace"))
        if self.rec:
            self.rec_tick()
        if self.lfo or self.emu_clock:
            us = 1_000_000 + self.frames * 1_000_000 // SR
            self.lib.core_c_fake_time(1, us)
            if self.lfo:
                self.lib.core_c_lfo_tick(ctypes.c_uint32(us & 0xFFFFFFFF))
        while self.seq_cmds and self.seq_cmds[0][0] <= self.t():
            c = self.seq_cmds.pop(0)[1]
            self.note(seqcmd=c, out=self.seq_command(c))
        if self.seq_tick:   # the Pi's control loop runs the sequencer's tick too (rytm_dump's transport: the pools' P3)
            self.lib.core_c_poly_tick()
            if self.seq_idle:   # and its idle passes (the project's store fills, the prefetch, the saves)
                self.lib.core_c_idle(4)
        self.lib.core_c_rui_tick(ctypes.c_uint32(self.frames * 1000 // SR))
        self.take()

    def seq_command(self, c):
        b = ctypes.create_string_buffer(1000)
        self.lib.core_c_seq_command(c.encode(), b, 1000)
        return b.value.decode(errors="replace")

    def lfo_json(self):
        b = ctypes.create_string_buffer(4096)
        n = self.lib.core_c_lfo_json(b, 4096)
        try:
            return json.loads("{" + b.raw[:n].decode() + "}")["lfo"]
        except ValueError:
            return None

    def stats(self):
        w = (ctypes.c_uint32 * 23)()
        self.lib.core_c_rui_stats(w, 23)
        return list(w)


def serve(h, link, secs, stop_at, hello=HELLO_MIDI | HELLO_NO_R | HELLO_VOICES):
    host, port = link.rsplit(":", 1)
    t0 = time.time()
    while True:
        try:
            s = socket.create_connection((host, int(port)), timeout=1)
            break
        except OSError:
            if time.time() - t0 > 60:
                sys.exit(f"no link at {link}")
            time.sleep(0.005)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.05)
    s.sendall(bytes([ord("H"), 1, hello]))
    buf, midi, insx = b"", bytearray(), False
    ch_msg = []   # a channel message being read (status, data...)
    t0 = time.time()
    while time.time() - t0 < secs:
        if stop_at is not None and h.frames / SR >= stop_at:
            h.note(ev="stop")
            break
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        except socket.timeout:
            pass
        except OSError:
            break
        while buf:
            k = buf[0:1]
            if k == b"N" and len(buf) >= 5:
                n = struct.unpack("<I", buf[1:5])[0]
                buf = buf[5:]
                ob = None
                if h.render_on:   # RUI_RENDER: the voices rendered for the frames the emulator asks for
                    ob = (ctypes.c_int16 * (8 * n))()
                    h.lib.core_c_render(ob, n)
                if hello & HELLO_VOICES:   # (the voices' frames; without the flag the emulator takes none)
                    s.sendall(b"a" + struct.pack("<I", n) + (bytes(ob) if ob is not None else bytes(16 * n)))
                h.tap(n)
                h.frames += n
                h.tick()
            elif k == b"H" and len(buf) >= 3:
                h.note(hello=buf[2])
                buf = buf[3:]
            elif k == b"M" and len(buf) >= 3 and len(buf) >= 3 + struct.unpack("<H", buf[1:3])[0]:
                n = struct.unpack("<H", buf[1:3])[0]
                data, buf = buf[3:3 + n], buf[3 + n:]
                for i, c in enumerate(data):   # a byte stream: whole SysEx out of it (real-time bytes inside one skipped)
                    if c in (0xF8, 0xFA, 0xFB, 0xFC):   # the clock and transport: the core's (STUTTER, the LFOs)
                        h.lib.core_c_sendfx_clock(c, 0)
                    if c in (0xFA, 0xFB, 0xFC):
                        h.note(rx="rt", b=c)
                    if not insx and c < 0xF0 and (c & 0x80 or ch_msg):   # channel messages: the LFOs' notes and CCs
                        if c & 0x80:
                            ch_msg = [c]
                        else:
                            ch_msg.append(c)
                            if len(ch_msg) == 3 or (len(ch_msg) == 2 and ch_msg[0] & 0xE0 == 0xC0):
                                if len(ch_msg) == 3:
                                    h.lib.core_c_lfo_midi(*ch_msg)
                                ch_msg = [ch_msg[0]]   # running status
                    elif c >= 0xF0 and c < 0xF8:
                        ch_msg = []
                    if 0x90 <= c <= 0x9F and not insx and i + 2 < len(data) and data[i + 2]:
                        h.note(rx="note", status=c, note=data[i + 1])
                    if c == 0xF0:
                        midi, insx = bytearray([c]), True
                    elif c >= 0xF8:
                        continue
                    elif insx:
                        midi.append(c)
                        if c == 0xF7:
                            insx = False
                            h.from_rytm(bytes(midi))
                        elif c & 0x80:
                            insx = False
            elif k == b"T" and len(buf) >= 4:
                buf = buf[4:]
            elif k == b"P" and len(buf) >= 10:
                buf = buf[10:]
            elif k == b"R" and len(buf) >= 5 and len(buf) >= 5 + 48 * struct.unpack("<I", buf[1:5])[0]:
                n = struct.unpack("<I", buf[1:5])[0]
                if getattr(h, "rframes", None):   # fxin_e2e.py: the IN frames, whatever the tap
                    h.rframes(buf[5:5 + 48 * n], n)
                if h.synth is None and h.lib.core_c_tap_on():   # the emulator's IN frames: 12 x int32
                    rb = ctypes.create_string_buffer(buf[5:5 + 48 * n], 48 * n)
                    h.lib.core_c_tap_in(rb, n, 12)
                buf = buf[5 + 48 * n:]
            elif k in (b"N", b"H", b"M", b"T", b"P", b"R"):
                break
            else:
                sys.exit(f"bad message {buf[:8]!r}")
        for m in h.out:
            for i in range(0, len(m), 60000):
                c = m[i:i + 60000]
                s.sendall(b"M" + struct.pack("<H", len(c)) + c)
        h.out = []
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--card", required=True)
    ap.add_argument("--log")
    ap.add_argument("--frames")
    ap.add_argument("--stop-at", type=float, default=None)
    ap.add_argument("--secs", type=float, default=900.0)
    a = ap.parse_args()
    h = Host(a.card, a.log, a.frames)
    print("rui_link: ready", flush=True)   # (rui_e2e.py starts the emulator on this line: the core is up first)
    try:
        serve(h, a.link, a.secs, a.stop_at)
    finally:
        h.note(lfo=h.lfo_json())
        if h.seq_tick:
            h.note(seq=h.seq_command("seq"))
        h.note(stats=h.stats())
        print("rui_link:", h.stats(), flush=True)


if __name__ == "__main__":
    main()
