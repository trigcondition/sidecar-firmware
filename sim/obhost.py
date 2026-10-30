#!/usr/bin/env python3
"""The simulated Teensy: an Overbridge host that plays Plaits into the Rytm's voices.

    obhost.py [--ext TRACKS] [--link HOST:PORT]

- Audio: connects to rytm-emu's Overbridge link (rytm-emu --ob-link, default
  127.0.0.1:7301) and answers each frame request with the eight per-voice
  channels (Overwitch's host->Rytm order: MAIN L/R, MAIN FX L/R, BD, SD, RS/CP, BT,
  LT, MT/HT, CH/OH, CY/CB). The Rytm plays each voice's channel through that
  voice's analog chain while the voice is triggered (rytm-mods 0021-ext).
- Control: listens to the Rytm's USB MIDI out ("rytm-emu usb out"). A note on the
  auto channel (14) whose number is a pad (0..11), or a note on a track channel
  (1..12), triggers that track's voice. CC 16..23 on a track channel (or the auto
  channel: the last track played) are the EXT machine's P1..P8:
      P1 ENG model (0..127 in 17 zones)   P2 HRM harmonics   P3 TIM timbre   P4 MOR morph
      P5 DEC decay         P6 LPG colour      P7 TUN tune (semitones from 64)
      P8 LEV level         (names and ranges: rytm-mods 0021's records)
  The Rytm's sequencer sends no MIDI notes, though: its trigs reach the host from
  the EXT machine itself, as SysEx (protocol.py cmd 0x04 / 0x03, rytm-mods 0021
  ext_render), with P1..P8 (p-locks and LFO included), and 0x11 when a voice's
  gate closes (its note-off: JUNO's and 6-OP FM's LEN 127 hold until it). The
  machine is the voice byte & 0x78 (protocol.machine): 0x48 is 6-OP FM, Plaits'
  six-op engine on the effective FM list's DX7 voices (the project's FM list
  from sim/pools/fm/, its banks found among Plaits' ROM banks by MD5 - no /DX7
  browsing here -, else the 96 ROM voices); 17 sends the list's names, 18 asks
  for them. An older EXT build without
  that is covered by rytm-emu's stand-in over the link (T/P messages); once SysEx
  reports arrive, the link's are ignored.
- --ext TRACKS (e.g. 0,1,8): set those tracks to EXT (machine 44) with CC 15 on
  their channels through "rytm-emu in", once at start.
"""
import argparse
import json
import os
import socket
import struct
import sys
import threading
import time

import mido
import numpy as np

from daisyvoice import INPUTS as DAISY_INPUTS, MODELS as DAISY_MODELS, DaisyVoice
from fmvoice import ROM_VOICES, FmVoice, rom_banks, rom_voice, voice_name
from junovoice import JunoVoice, factory as juno_factory
from plaitsvoice import Voice
from protocol import (DAISYSP, DIGIDRUM, DSP_SAMPLE, FM, JUNO, JUNO_KNOB_BYTES, PLAITS_MODELS, eng_zone,
                      fm_names, juno_knobs, juno_names, machine)
from samplevoice import Library, SampleVoice, read_wav

AUTO_CH = 13                                   # MIDI channel 14
TRACK_VOICE = [0, 2, 7, 7, 1, 3, 5, 5, 4, 4, 6, 6]   # rytm-mods machines.md
VOICE_CH = [4, 7, 5, 8, 10, 9, 11, 6]          # Overbridge channel of physical voice v
# The Rytm's Overbridge output carries the voices in track order (BD SD RS/CP BT
# LT MT/HT CH/OH CY/CB on channels 2..9): physical voice v's index there.
VOICE_OUT_IDX = [0, 3, 1, 4, 6, 5, 7, 2]
# physical voice -> its first track
VOICE_TRACK = [TRACK_VOICE.index(v) for v in range(8)]
MAIN_GAIN = 1.5          # DAISYSP OUT=MAIN: resonance into MAIN L/R (by ear)


def folder_label(path):
    """A card folder's name for the Rytm's knob, distinctive part first (the knob
    shows about 7 characters): its own name, then its parent's - initials when
    the parent has several words. "Drums/Hits/TR808/CH" -> "CH TR808",
    ".../DnB Rebellion One Shots/BASS" -> "BASS DROS", "Synths" -> "Synths"."""
    parts = [p for p in path.split(os.sep) if p not in ("", ".")]
    if not parts:
        return "/"
    last = parts[-1]
    if len(parts) < 2:
        return last
    words = parts[-2].split()
    parent = "".join(w[0] for w in words).upper() if len(words) > 1 else parts[-2]
    return f"{last} {parent}"


class LoopCanceller:
    """DAISYSP with OUT=VOICE and INP NAT/NOISE: the voice's own output, which
    excites the resonator, also carries the resonance sent back to it - a loop
    through the voice's analog filter and amp. FB keeps that much of it: the
    rest is cancelled by subtracting what was sent, at the round-trip delay and
    gain measured by cross-correlation (re-measured every second). The analog
    filter colours the loop, so cancellation is never complete."""

    N = 1 << 15

    def __init__(self):
        self.y = np.zeros(self.N, np.float32)    # sent, oldest first
        self.x = np.zeros(self.N, np.float32)    # received, aligned to blocks
        self.lag, self.gain = 0, 0.0
        self.since = 0

    def process(self, x, fb):
        """x: n received samples; returns x with (1 - fb) of the loop removed."""
        n = len(x)
        self.x = np.roll(self.x, -n)
        self.x[-n:] = x
        self.since += n
        if self.since >= 48000:
            self.since = 0
            self.measure()
        if self.gain == 0.0 or fb >= 1.0:
            return x
        L = self.lag
        seg = self.y[self.N - n - L:self.N - L] if L else self.y[-n:]
        return x - (1.0 - fb) * self.gain * seg

    def sent(self, y):
        n = len(y)
        self.y = np.roll(self.y, -n)
        self.y[-n:] = y

    def measure(self):
        y, x = self.y[-16384:], self.x[-16384:]
        if float(np.dot(y, y)) < 1e-6:
            return
        f = np.fft.rfft(np.concatenate([y, np.zeros_like(y)]))
        g = np.fft.rfft(np.concatenate([x, np.zeros_like(x)]))
        c = np.fft.irfft(g * np.conj(f))[:4096]        # x lagging y by 0..4095
        L = int(np.argmax(np.abs(c)))
        ys = self.y[self.N - 16384 - L:self.N - L] if L else y
        den = float(np.dot(ys, ys))
        if den > 1e-9:
            self.lag, self.gain = L, float(np.dot(x, ys)) / den

EXT_MACHINE = 44


def pool_name13(rel):
    """A pool entry's name for 14: the file name without extension, printable
    ASCII (other bytes of its UTF-8 '_', as the Teensy's), cut to 13 bytes and
    NUL padded."""
    stem = os.path.splitext(rel.split("/")[-1])[0].encode()
    b = [c if 0x20 <= c <= 0x7E else ord("_") for c in stem[:13]]
    return b + [0] * (13 - len(b))


def pool_messages(pool, ids):
    """The 14 messages (SysEx data without F0/F7) for a pool table: pool is a
    list of paths ("" empty) for slots 1.., ids(path) -> (folder, file)."""
    ents = [(n + 1, r) for n, r in enumerate(pool) if r]
    msgs = []
    for first in range(0, max(len(ents), 1), 6):
        chunk = ents[first:first + 6]
        data = [0x7D, 0x52, 0x54, 0x14, 1 if first == 0 else 0, len(chunk)]
        for slot, rel in chunk:
            f, e = ids(rel)
            data += [slot, (f >> 7) & 0x7F, f & 0x7F, (e >> 7) & 0x7F, e & 0x7F] + pool_name13(rel)
        msgs.append(data)
    return msgs


def find(names, want):
    for n in names:
        if want in n:
            return n
    sys.exit(f"no MIDI port containing {want!r}; have: {names}")


class Host:
    def __init__(self):
        self.voices = [Voice() for _ in range(8)]
        # DAISYSP (rytm-mods 0021, machine 51): its reports flag the voice 0x10
        self.daisy = [DaisyVoice() for _ in range(8)]
        self.is_daisy = [False] * 8
        # DSP SAMPLE (machine 45, flag 0x20): DAISYSP rung by a sample from the
        # Teensy's card. SRC: ENG STR BRI DMP MIX FB TUN LEV; its SMPL page (SD
        # SAMPLE) by sample word: 17 TUN 18 FIN 19 SMP (pool slot) 20 FLD (unused; MOD with --psram) 21 STA
        # 22 END 23 LOP 24 LEV, as the Rytm's own sample page.
        self.is_dsps = [False] * 8
        # 0x40: DIGIDRUM until 2026-10-29, the built-in MD since (core/md/md.h, on the C++ core only):
        # this Python host has no MD, a voice reported with it is silent
        self.is_off = [False] * 8
        # JUNO (rytm-mods 0021, flag 0x08): a Juno-106 voice (juno/juno_voice.h);
        # P1..P8 PAT CUT RES ENV ATK DEC REL LEN, the note the trig's
        self.juno = [JunoVoice() for _ in range(8)]
        self.is_juno = [False] * 8
        # Knob sync (15): the sound's PAT (from 0x03 or the first report), the
        # patch the Rytm's knobs were sent, and the six values held until the
        # Rytm's reports carry them (or JUNO_HOLD s passed)
        self.juno_pat = [None] * 8
        self.juno_key = [None] * 8
        self.juno_hold = [None] * 8      # (values, since)
        # 6-OP FM (rytm-mods 0021 machine 40, flag 0x48): Plaits' six-op engine
        # (plaits/fm_machine.h); P1..P8 PAT TIM MOR DEC LPG TUN LEV LEN, PAT
        # the effective FM list's entry min(PAT, total - 1)
        self.fm = [FmVoice() for _ in range(8)]
        self.is_fm = [False] * 8
        self.rom_md5 = {md5: bank for md5, bank in rom_banks()}
        self.lib = Library(os.path.join(os.path.dirname(os.path.abspath(__file__)), "samples"))
        self.smp = [SampleVoice() for _ in range(8)]
        # --psram (docs/psram.md): the Teensy with PSRAM - DSP SAMPLE plays the
        # pool's samples from RAM in MOD's mode (samplemodes.py, word 20, was
        # FLD); here every sample is resident (the whole file is read). Off:
        # sample_voice.h as the Teensy streams today, MOD ignored.
        self.psram = False
        self.ram = None
        self.from_ram = [False] * 8
        # The pool ("static" samples, as the Octatrack's), one per Rytm project
        # (10 <name>): slots 1..127, slot N the file's line N-1, a path under
        # the library, "" an empty slot - as the Teensy's
        # /.rytm/pools/<PROJECT>.txt, here sim/pools/; the last project in
        # sim/pools/.project. Filled with LOAD TO PROJ (0F) or ASSIGN (12) from
        # the Rytm; DSP SAMPLE's SMP picks slot SMP (0 = off).
        self.pool_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pools")
        try:
            with open(os.path.join(self.pool_dir, ".project")) as fh:
                self.project = fh.read().strip()
        except OSError:
            self.project = ""
        self.project_seen = False       # no 10 yet: the first one pushes the table
        self.pool_load()
        self.juno_load()
        self.fm_load()
        self.din = None                 # the Rytm's MIDI in, for --ext
        self.note = [60.0] * 8
        # The Rytm's per-voice Overbridge output (channels 2..9, track order:
        # BD SD RS/CP BT LT MT/HT CH/OH CY/CB) as it arrives, for DAISYSP's INP.
        self.vin = np.zeros((0, 8), dtype=np.float32)
        self.loops = [LoopCanceller() for _ in range(8)]
        # DAISYSP page 2 (EXCITER) per voice: OUT on MAIN, FB 0..1, WID 0..1
        self.d_main = [False] * 8
        self.d_fb = [0.0] * 8
        self.d_wid = [0.5] * 8
        self.wid_hist = [np.zeros(1024, np.float32) for _ in range(8)]
        # P1 engine 0..127 (17 zones, protocol.PLAITS_MODELS), P2..P6 0..127, P7 tune 32..96 (64 = none), P8 level
        # (rytm-mods 0021's records)
        # P9..P16 (page 2, the sample words): FM FIN AUX TMD MMD VEL GLD VOL
        self.params = [[45, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100]
                       for _ in range(8)]
        self.live = [False] * 8
        # --log: what happened since the last activity line
        self.act = {"trig": 0, "rep": 0, "midi": 0, "peak": 0.0}
        self.last_track = 0
        # Once the Rytm reports by SysEx (rytm-mods 0021 ext_render), the
        # emulator's stand-in reports over the link are ignored.
        self.sysex_seen = False
        self.lock = threading.Lock()
        self.looper = Looper()
        for v in range(8):
            self.apply(v)

    def voice(self, v):
        if self.is_fm[v]:
            return self.fm[v]
        if self.is_juno[v]:
            return self.juno[v]
        return self.daisy[v] if self.is_daisy[v] else self.voices[v]

    def name(self, v):
        if self.is_fm[v]:
            return f"6-OP FM {self.fm[v].name}"
        if self.is_off[v]:
            return "MD (not in this Python host: silent)"
        if self.is_juno[v]:
            return f"JUNO {self.juno[v].name}"
        if self.is_daisy[v]:
            d = self.daisy[v]
            if self.is_dsps[v]:
                return f"DSP SAMPLE {DAISY_MODELS[d.model]} <- {self.smp[v].name}"
            return f"DAISYSP {DAISY_MODELS[d.model]} <- {DAISY_INPUTS[d.input]}"
        return f"PLAITS engine {self.voices[v].engine}"

    def apply(self, v):
        p = self.params[v]
        if self.is_fm[v]:
            self.fm[v].knobs(p[:8])
            return
        if self.is_juno[v]:
            self.juno[v].knobs(p[:8])
            return
        if self.is_off[v]:
            return
        if self.is_daisy[v]:
            # ENG STR BRI DMP INP EXC TUN LEV; ENG in three zones of 0..127
            # (MODAL STRING RESON), INP in eleven
            d = self.daisy[v]
            d.model = eng_zone(p[0], len(DAISY_MODELS))
            d.structure, d.brightness, d.damping = p[1] / 127, p[2] / 127, p[3] / 127
            if self.is_dsps[v]:
                d.input = 1                 # rendered with an input (the sample)
                d.exciter = 0.25            # input gain 1; the sample's LEV drives it
                self.d_main[v] = False      # always back through the voice
                self.d_fb[v] = p[5] / 127
                if len(p) >= 16:            # SMP (word 19): pool slot, 0 off
                    self.smp[v].set(self.pool_entry(int(p[10])))
                return
            d.input = max(0, min(10, int(p[4] * 11 / 128)))
            d.exciter = p[5] / 127
            # page 2, EXCITER, by sample word: 17 XTN 18 XDC 19 FB 20 OUT 21 XP4
            # 22 XLV 23 XP5 24 WID (the X knobs are the Rytm's own business)
            if len(p) >= 16:
                self.d_fb[v] = p[10] / 127
                self.d_main[v] = p[11] >= 64
                self.d_wid[v] = p[15] / 127
            if d.input:
                self.live[v] = True     # it rings from the input, trig or not
            return
        vc = self.voices[v]
        # P1 ENG: the whole knob, 0..127, in 17 equal zones (rytm-mods 0021,
        # curated 2026-10-09): protocol.PLAITS_MODELS - no NOISE, 6-OP FM or drums
        vc.engine = PLAITS_MODELS[eng_zone(p[0], len(PLAITS_MODELS))]
        vc.harmonics, vc.timbre, vc.morph = p[1] / 127, p[2] / 127, p[3] / 127
        vc.decay, vc.lpg_colour = p[4] / 127, p[5] / 127
        if len(p) >= 16:
            vc.fm_amount = (p[8] - 64) / 64
            vc.aux_mix = p[10] / 127
            vc.timbre_amount = p[11] / 127
            vc.morph_amount = p[12] / 127
            vc.glide = p[14] / 127 * 0.5            # up to 0.5 s
            vc.gain = p[15] / 100

    def midi(self, m):
        if m.type in ("clock", "start", "stop", "continue"):
            self.looper.clock(m.type)
            return
        if m.type == "sysex":
            d = list(m.data)
            if len(d) >= 6 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x05:
                self.looper.mode(d[4], d[5])
            elif len(d) >= 4 and d[:3] == [0x7D, 0x52, 0x54] and d[3] in (0x09, 0x0B, 0x0C):
                pass                        # the retired folder walk (older 0021/0022)
            elif len(d) >= 4 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x10:
                self.set_project("".join(chr(c) for c in d[4:] if c))
            elif len(d) >= 8 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x0F:
                self.pool_add(d[4] << 7 | d[5], d[6] << 7 | d[7])
            elif len(d) >= 6 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x0D:
                self.send_listing(d[4] << 7 | d[5])
            elif len(d) >= 9 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x12:
                self.pool_assign(d[4], d[5] << 7 | d[6], d[7] << 7 | d[8])
            elif len(d) >= 4 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x13:
                self.send_pool()
                self.send_juno_names()
            elif len(d) >= 4 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x18:
                self.send_fm_names()
            elif len(d) >= 5 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x11:
                self.note_off(d[4] & 7)
            elif len(d) >= 6 and d[:3] == [0x7D, 0x52, 0x54] and d[3] == 0x06:
                if d[5]:
                    self.looper.pad(d[4])
            elif len(d) >= 4 and d[:3] == [0x7D, 0x52, 0x54]:
                self.report(d[3], d[4:])
            return
        with self.lock:
            if m.type == "note_on" and m.velocity > 0:
                if m.channel == AUTO_CH and m.note < 12:
                    track, note = m.note, None
                elif m.channel < 12:
                    track, note = m.channel, m.note
                else:
                    return
                self.last_track = track
                v = TRACK_VOICE[track]
                # P7: tune, semitones from 64. Pad notes (auto channel) carry no
                # pitch: C4 plus the tune.
                if note is None:
                    note = 60 + self.params[v][6] - 64
                else:
                    note = note + round(self.params[v][6] - 64)
                self.voice(v).trigger(note, m.velocity / 127 * self.params[v][7] / 127)
                self.live[v] = True
                self.act["midi"] += 1
                print(f"trig track {track} voice {v} note {note:.0f} {self.name(v)}")
            elif m.type == "control_change" and 16 <= m.control <= 23:
                # On a track channel: that track. On the auto channel (OUTPUT CH =
                # AUTO CH): the knobs edit the active track; taken as the last
                # track played.
                if m.channel < 12:
                    track = m.channel
                elif m.channel == AUTO_CH:
                    track = self.last_track
                else:
                    return
                v = TRACK_VOICE[track]
                self.params[v][m.control - 16] = m.value
                self.apply(v)

    def report(self, cmd, d):
        """The PLAITS/DAISYSP machines' report: 0x04 voice note velocity P1..P16
        (a trig with the parameters it plays with), 0x03 voice P1..P16 (they
        changed). The machine is the voice byte & 0x78 (protocol.machine:
        0x10 DAISYSP, 0x20 DSP SAMPLE, 0x08 JUNO, 0x48 6-OP FM, 0x40 MD (silent here),
        else PLAITS). Each P is two bytes, value then fraction (1/128 steps)."""
        # 8 parameters (older 0021) or 16 (with page 2)
        if cmd == 0x04 and len(d) >= 19:
            v, note, vel, raw = d[0] & 7, d[1], d[2], d[3:]
        elif cmd == 0x03 and len(d) >= 17:
            v, note, vel, raw = d[0] & 7, None, None, d[1:]
        else:
            return
        mach = machine(d[0])
        off, juno, fm = mach == DIGIDRUM, mach == JUNO, mach == FM
        daisy = mach in (DAISYSP, DSP_SAMPLE)
        dsps = mach == DSP_SAMPLE
        npar = 16 if len(raw) >= 32 else 8
        p = [raw[2 * k] + raw[2 * k + 1] / 128 for k in range(npar)]
        with self.lock:
            if not self.sysex_seen:
                self.sysex_seen = True
                print("the Rytm reports by SysEx: link reports ignored from now on")
            was_juno, was_fm = self.is_juno[v], self.is_fm[v]
            self.is_daisy[v] = daisy       # only the voice's current engine renders
            self.is_dsps[v] = dsps
            self.is_off[v] = off
            self.is_juno[v] = juno
            self.is_fm[v] = fm
            self.params[v][:len(p)] = list(p)
            if fm and (note is not None or not was_fm):
                # PAT at a trig (a p-locked one too) or when the voice becomes
                # FM; a 0x03's waits for the next trig (a voice change cuts the
                # sounding note)
                self.fm_select(v, int(p[0]))
            if juno:
                # the knobs follow the sound's PAT (0x03), not a p-locked one (0x04)
                self.juno_select(v, int(p[0]), cmd == 0x03 or not was_juno, not was_juno)
            self.apply(v)
            self.act["trig" if note is not None else "rep"] += 1
            if note is not None:
                q = self.params[v]
                if fm:
                    # the note the trig's (TUN in the voice), the velocity the DX7's
                    self.fm[v].trigger(note, max(vel, 1) / 127)
                    self.live[v] = True
                    print(f"trig (sysex) voice {v} note {note} vel {vel} {self.name(v)}")
                    return
                if juno:
                    self.juno[v].trigger(note, max(vel, 1) / 127)
                    self.live[v] = True
                    print(f"trig (sysex) voice {v} note {note} vel {vel} {self.name(v)}")
                    return
                if off:
                    self.live[v] = False
                    print(f"trig (sysex) voice {v}: {self.name(v)}")
                    return
                if daisy:
                    # TUN semitones; P9..P16 are the sound's own sample page
                    n = note + round(q[6] - 64)
                    amp = max(vel, 1) / 127 * q[7] / 127
                else:
                    # TUN semitones, FIN a semitone either way; VEL how much velocity counts
                    n = note + round(q[6] - 64) + (q[9] - 64) / 64
                    sens = q[13] / 127
                    amp = (1 - sens + sens * max(vel, 1) / 127) * q[7] / 127
                self.voice(v).trigger(n, amp)
                self.live[v] = True
                if dsps:
                    self.note[v] = note
                    self.smp[v].trigger()
                    self.ram_trigger(v, note, q)
                print(f"trig (sysex) voice {v} note {n} vel {vel} {self.name(v)}")

    JUNO_HOLD = 0.3             # s: the knobs' old values ignored after 15 at most this long

    def juno_pick(self, pat):
        """The patch PAT picks -> (18 bytes, name, key): the project's JUNO
        list's slot min(PAT, total - 1) when there is a list (an empty slot:
        no oscillator), else factory patch PAT. key tells patches apart."""
        if self.juno_list:
            slot = min(pat, len(self.juno_list) - 1)
            b, name = self.juno_list[slot] or (bytes(18), "(empty)")
            return b, name, ("L", slot, b)
        return juno_factory(pat), "", ("F", pat)

    def juno_select(self, v, pat, sync, force=False):
        """Voice v plays the patch PAT picks; with sync (a 0x03 report, the
        first JUNO one, a changed list) PAT is the sound's: when that is
        another patch (or force) the Rytm gets 15 with its six slider values,
        held over the incoming CUT..REL until a report carries them."""
        b, name, key = self.juno_pick(pat)
        self.juno[v].use(b if self.juno_list else None, name)
        if sync:
            self.juno_pat[v] = pat
            if force or key != self.juno_key[v]:
                self.juno_key[v] = key
                vals = [b[i] for i in JUNO_KNOB_BYTES]
                self.juno_hold[v] = (vals, time.monotonic())
                self.to_rytm(juno_knobs(v, b))
        h = self.juno_hold[v]
        if h:
            q = self.params[v]
            if all(int(q[1 + i]) == h[0][i] for i in range(6)) or time.monotonic() - h[1] >= self.JUNO_HOLD:
                self.juno_hold[v] = None
            else:
                q[1:7] = list(h[0])

    def fm_path(self):
        return os.path.join(self.pool_dir, "fm", os.path.basename(self.pool_path()))

    def fm_load(self):
        """The project's FM list, as the Teensy's /.rytm/fm/<PROJECT>.txt (here
        sim/pools/fm/): a line per slot, the bank's MD5 (32 hex digits), a tab,
        the voice 0..31, a tab, its name; an empty line an empty slot. Entries
        are (voice bytes or None when the bank is not one of Plaits' ROM banks,
        name), None for an empty slot. At most 127 slots."""
        self.fm_list = []
        try:
            with open(self.fm_path()) as fh:
                for ln in fh:
                    f = ln.rstrip("\r\n").split("\t")
                    try:
                        md5, i = f[0].lower(), int(f[1])
                    except (IndexError, ValueError):
                        self.fm_list.append(None)
                        continue
                    bank = self.rom_md5.get(md5)
                    vb = bank[128 * i:128 * (i + 1)] if bank and 0 <= i < 32 else None
                    name = f[2] if len(f) > 2 else (voice_name(vb) if vb else "")
                    if vb is None:
                        print(f"fm list: {md5} voice {i} ({name}): not a ROM bank here, silent")
                    self.fm_list.append((vb, name))
        except OSError:
            pass
        self.fm_list = self.fm_list[:127]
        while self.fm_list and self.fm_list[-1] is None:
            self.fm_list.pop()

    def fm_entries(self):
        """The effective FM list: the project's when it has a slot, else
        Plaits' 96 ROM voices: [(voice bytes or None, name) or None]."""
        if self.fm_list:
            return self.fm_list
        if not hasattr(self, "_rom"):
            self._rom = [(rom_voice(i), voice_name(rom_voice(i))) for i in range(ROM_VOICES)]
        return self._rom

    def fm_select(self, v, pat):
        """Voice v plays entry min(PAT, total - 1) of the effective FM list
        (an empty slot or a bank not found: silent)."""
        ents = self.fm_entries()
        e = ents[min(max(pat, 0), len(ents) - 1)]
        self.fm[v].use(e[0] if e else None, e[1] if e else "")

    def send_fm_names(self):
        """17: the effective FM list's names to the Rytm (sim/protocol.py)."""
        for m in fm_names([e[1] if e else None for e in self.fm_entries()]):
            self.to_rytm(m)

    def to_rytm(self, msg):
        """A SysEx message (F0..F7) to the Rytm."""
        out = getattr(self.looper, "out", None)
        if out is not None:
            out.send(mido.Message("sysex", data=list(msg[1:-1])))

    def juno_path(self):
        return os.path.join(self.pool_dir, "juno", os.path.basename(self.pool_path()))

    def juno_load(self):
        """The project's JUNO list, as the Teensy's /.rytm/juno/<PROJECT>.txt
        (here sim/pools/juno/): a line per slot, 36 hex digits, a tab, the
        name; an empty line an empty slot (None). At most 127 slots."""
        self.juno_list = []
        try:
            with open(self.juno_path()) as fh:
                for ln in fh:
                    ln = ln.rstrip("\r\n")
                    try:
                        e = (bytes.fromhex(ln[:36]), ln[37:] if ln[36:37] == "\t" else "")
                    except ValueError:
                        e = None
                    self.juno_list.append(e if e and len(e[0]) == 18 else None)
        except OSError:
            pass
        self.juno_list = self.juno_list[:127]
        while self.juno_list and self.juno_list[-1] is None:
            self.juno_list.pop()

    def juno_relist(self):
        """The JUNO list changed: voices whose PAT now points at another
        patch play it, and their knobs get it (15)."""
        for v in range(8):
            if self.is_juno[v] and self.juno_pat[v] is not None:
                self.juno_select(v, self.juno_pat[v], True)
                self.apply(v)

    def send_juno_names(self):
        """16: the project's JUNO list's names to the Rytm (sim/protocol.py)."""
        for m in juno_names([e[1] if e else None for e in self.juno_list]):
            self.to_rytm(m)

    def note_off(self, v):
        """0x11 NOTE_OFF (rytm-mods 0021-ext): the voice's gate closed. The
        voice byte has no machine flags: the voice's engine is the one its
        last trig set."""
        with self.lock:
            self.voice(v).note_off()
            if self.is_dsps[v] and self.from_ram[v]:
                self.ram[v].note_off()   # LOOP GRAIN STRCH WAVE fade out

    def use_psram(self):
        from samplemodes import ModeVoice
        self.psram = True
        self.ram = [ModeVoice(v + 1) for v in range(8)]
        self.mono = {}

    def ram_trigger(self, v, note, q):
        """A DSP SAMPLE trig with --psram: from RAM (samplemodes.ModeVoice),
        the sample its SMP's pool slot, the mode MOD's."""
        if self.from_ram[v]:
            self.ram[v].stop()
        self.from_ram[v] = False
        if not self.psram or len(q) < 16:
            return
        from samplemodes import MODES, PATH_RAM, choose_path, to_mono16
        entry = self.pool_entry(int(q[10]))
        path = choose_path(True, entry is not None, eng_zone(q[11], len(MODES)))
        if path != PATH_RAM:
            return
        pcm, rate, rel = entry
        if rel not in self.mono:
            self.mono[rel] = to_mono16(pcm)
        self.ram[v].set(self.mono[rel], rate)
        self.ram[v].trigger(note, q[8:16])
        self.from_ram[v] = True

    def link_trig(self, v, note, vel):
        """A trig reported by the Rytm side (the emulator's stand-in): the
        sequencer's trigs arrive only this way, it sends no MIDI notes."""
        if self.sysex_seen:
            return
        with self.lock:
            note = note + round(self.params[v][6] - 64)
            self.voice(v).trigger(note, max(vel, 1) / 127 * self.params[v][7] / 127)
            self.live[v] = True
            print(f"trig (link) voice {v} note {note} vel {vel} {self.name(v)}")

    def link_params(self, v, p):
        if self.sysex_seen:
            return
        with self.lock:
            self.params[v] = list(p)
            self.apply(v)

    def pool_path(self):
        safe = "".join(c if c.isalnum() or c in "-_ ." else "_" for c in self.project).rstrip(" .")
        return os.path.join(self.pool_dir, (safe or "_default") + ".txt")

    POOL_SLOTS = 127            # slots 1..127: lines 0..126 of the pool file

    def pool_load(self):
        try:
            with open(self.pool_path()) as fh:
                self.pool = [ln.rstrip("\r\n") for ln in fh][:self.POOL_SLOTS]
        except OSError:
            self.pool = []
        while self.pool and not self.pool[-1]:
            self.pool.pop()
        missing = sum(1 for r in self.pool if r and not os.path.isfile(os.path.join(self.lib.root, r)))
        print(f"pool {self.project or '_default'}: {len(self.pool)} slots, {missing} not in the library")

    def pool_save(self):
        try:
            os.makedirs(self.pool_dir, exist_ok=True)
            with open(self.pool_path(), "w") as fh:
                fh.write("".join(r + "\n" for r in self.pool))
        except OSError as e:
            print(f"pool: cannot save {self.pool_path()}: {e}")

    def set_project(self, name):
        """10 <name>: the Rytm's project; its pool from now on. The table goes
        to the Rytm on the first 10 and whenever the project changes (not on
        the repeats the Rytm sends every few seconds)."""
        first, self.project_seen = not self.project_seen, True
        if name != self.project:
            self.project = name
            print(f"project: {name}")
            self.pool_load()
            self.juno_load()
            self.fm_load()
            with self.lock:
                self.juno_relist()
                for v in range(8):
                    if self.is_fm[v]:
                        self.fm_select(v, int(self.params[v][0]))
            try:
                os.makedirs(self.pool_dir, exist_ok=True)
                with open(os.path.join(self.pool_dir, ".project"), "w") as fh:
                    fh.write(name)
            except OSError as e:
                print(f"pool: cannot save the project name: {e}")
        elif not first:
            return
        self.send_pool()
        self.send_juno_names()
        self.send_fm_names()

    def file_path(self, f, i):
        """TEENSY folder f's file i (as 0D/0E number them) -> its path under
        the library, or None."""
        num, _, _ = self.lib.numbered()
        if not 0 <= f < len(num):
            return None
        files = self.lib.wavs(num[f])
        if not 0 <= i < len(files):
            return None
        return "/".join(list(num[f]) + [files[i]])

    def file_ids(self, rel):
        """A path under the library -> (folder, file) as 0E numbers them, or
        (0x3fff, 0x3fff) when it is not there."""
        num, _, _ = self.lib.numbered()
        parts = rel.split("/")
        place = tuple(parts[:-1])
        if place in num:
            files = self.lib.wavs(place)
            if parts[-1] in files:
                return num.index(place), files.index(parts[-1])
        return 0x3FFF, 0x3FFF

    def pool_add(self, f, i):
        """0F <folder> <file>: LOAD TO PROJ on a TEENSY file - into the first
        empty slot of the project's pool (not again if it is there), then the
        table to the Rytm."""
        rel = self.file_path(f, i)
        if rel is not None and rel not in self.pool:
            slot = self.pool.index("") if "" in self.pool else len(self.pool)
            if slot < self.POOL_SLOTS:
                if slot == len(self.pool):
                    self.pool.append(rel)
                else:
                    self.pool[slot] = rel
                self.pool_save()
        if rel is not None and rel in self.pool:
            print(f"pool {self.project or '_default'}: slot {self.pool.index(rel) + 1} = {rel}")
        self.send_pool()

    def pool_assign(self, slot, f, i):
        """12 <slot> <folder> <file>: pool slot 1..127 = that TEENSY file
        (whatever it held; the same file may be in other slots too), then the
        table to the Rytm."""
        rel = self.file_path(f, i)
        if not 1 <= slot <= self.POOL_SLOTS or rel is None:
            print(f"pool: assign slot {slot} <- folder {f} file {i}: ignored (no such slot or file)")
        else:
            self.pool += [""] * (slot - len(self.pool))
            self.pool[slot - 1] = rel
            self.pool_save()
            print(f"pool {self.project or '_default'}: slot {slot} = {rel}")
        self.send_pool()

    def send_pool(self):
        """The project's pool table to the Rytm: 14 <replace> <count> and per
        entry <slot> <folder hi lo> <file hi lo> <13 name bytes>, 6 a message,
        non-empty slots in order; the first message replaces (sim/protocol.py)."""
        out = getattr(self.looper, "out", None)
        if out is None:
            return
        for data in pool_messages(self.pool, self.file_ids):
            out.send(mido.Message("sysex", data=data))
        print(f"pool {self.project or '_default'}: table sent, "
              f"{sum(1 for r in self.pool if r)} entries")

    def pool_entry(self, slot):
        """SMP's pool slot: 0 off, N slot N; an empty slot or one beyond the
        pool is silent (None)."""
        if not 1 <= slot <= len(self.pool) or not self.pool[slot - 1]:
            return None
        rel = self.pool[slot - 1]
        if rel not in self.lib.cache:
            try:
                pcm, rate = read_wav(os.path.join(self.lib.root, rel))
            except Exception as e:
                print(f"pool: {rel}: {e}")
                return None
            self.lib.cache[rel] = (pcm, rate, rel)
        return self.lib.cache[rel]

    def send_listing(self, f):
        """0D <folder>: the folder's entries for 0023's TEENSY folder, as
        0E <folder hi lo> <parent hi lo> <first hi lo> <total hi lo> and per
        entry <kind> <id hi lo> <size 4 x 7 bits> <name length> <name>, 6 a
        message (sim/protocol.py)."""
        out = getattr(self.looper, "out", None)
        if out is None:
            return
        parent, ents = self.lib.listing(f)
        ents = ents[:512]
        hl = lambda x: [(x >> 7) & 0x7F, x & 0x7F]
        total = len(ents)
        for first in range(0, max(total, 1), 6):
            data = [0x7D, 0x52, 0x54, 0x0E] + hl(f) + hl(parent) + hl(first) + hl(total)
            for kind, i, size, name in ents[first:first + 6]:
                nm = [ord(c) if 32 <= ord(c) < 127 else ord("?") for c in name[:23]]
                size = min(size, (1 << 28) - 1)
                data += [kind] + hl(i) + [(size >> 21) & 0x7F, (size >> 14) & 0x7F,
                                          (size >> 7) & 0x7F, size & 0x7F, len(nm)] + nm
            out.send(mido.Message("sysex", data=data))
        print(f"listing: folder {f}: {total} entries sent")

    def render_dsps(self, v, n, own):
        """DSP SAMPLE: the card sample rings the resonator directly (no round trip);
        MIX blends dry and wet; FB lets the voice's own output back in."""
        p, d = self.params[v], self.daisy[v]
        semis = (p[8] - 64) + (p[9] - 64) / 64 + (self.note[v] - 60)
        if self.from_ram[v]:
            x = self.ram[v].render(n, p[8:16])
        else:
            x = self.smp[v].render(n, semis, p[12] / 120, p[13] / 120, p[14] >= 1, p[15] / 100)
        # FB 0: the voice's output is not listened to at all - no loop, no leak
        # from imperfect cancellation; above 0 the loop comes in, cancelled down
        # to FB of it.
        if self.d_fb[v] > 0.0:
            x_in = x + self.loops[v].process(own, self.d_fb[v])
        else:
            x_in = x
        mix = p[4] / 127
        if mix <= 0:            # MIX 0: the sample alone, no resonator to run
            return np.tanh(x)
        wet = d.render(n, x_in)
        return np.tanh((1 - mix) * x + mix * wet)

    def feed(self, frames):
        """The Rytm's Overbridge output frames (n x 12 int32): keep the voices'."""
        x = frames[:, 2:10].astype(np.float32) / 2147483648.0
        with self.lock:
            self.vin = np.concatenate([self.vin, x])[-8192:]

    def render(self, n):
        out = np.zeros((n, 12), dtype=np.int32)
        with self.lock:
            # The voices' audio for this block: the oldest n frames received
            # (silence while too few have come).
            if len(self.vin) >= n:
                vin, self.vin = self.vin[:n], self.vin[n:]
            else:
                vin = np.zeros((n, 8), dtype=np.float32)
            main = np.zeros((n, 2), np.float32)
            for v in range(8):
                if not self.live[v]:
                    continue
                if self.is_dsps[v]:
                    o = self.render_dsps(v, n, vin[:, VOICE_OUT_IDX[v]])
                elif self.is_juno[v] or self.is_fm[v]:
                    o = self.voice(v).render(n)
                elif self.is_daisy[v]:
                    d = self.daisy[v]
                    own = d.input in (1, 2)        # NAT / NOISE: this voice's own output
                    inp = None
                    if own:
                        inp = vin[:, VOICE_OUT_IDX[v]]
                        if not self.d_main[v]:
                            inp = self.loops[v].process(inp, self.d_fb[v])
                    elif d.input >= 3:
                        inp = vin[:, d.input - 3]
                    o = d.render(n, inp)
                    if own and not self.d_main[v]:
                        o = np.tanh(o)             # the loop's limiter
                    if self.d_main[v]:
                        # resonance on MAIN L/R; WID: R up to 12 ms behind L
                        h = np.concatenate([self.wid_hist[v], o])
                        self.wid_hist[v] = h[-1024:]
                        dly = int(self.d_wid[v] * 576)
                        main[:, 0] += o
                        main[:, 1] += h[len(h) - n - dly:len(h) - dly]
                        self.act["peak"] = max(self.act["peak"], float(np.abs(o).max()))
                        continue
                else:
                    o = self.voice(v).render(n)
                self.act["peak"] = max(self.act["peak"], float(np.abs(o).max()))
                y = np.clip(o * 0.7, -1.0, 1.0)
                out[:, VOICE_CH[v]] = (y.astype(np.float64) * 2147483647.0).astype(np.int32)
                if self.is_daisy[v]:
                    self.loops[v].sent(y)
            if main.any():
                m = np.tanh(main.astype(np.float64) * 0.7 * MAIN_GAIN)
                out[:, 0:2] = (m * 2147483647.0).astype(np.int32)
        self.looper.play(out)
        return out


class Looper:
    """Master looper core (docs/looper.md): a rolling recording of the Rytm's
    MAIN L/R (Overbridge IN channels 0/1, with rytm-emu --usb-host in OVERBRIDGE
    mode) and a loop played back into MAIN L/R (OUT channels 0/1: joins after
    the Rytm's master FX). Clips will live on the SD card on the Teensy; here
    they are arrays."""

    BAR_TICKS = 96             # MIDI clock: 24 a beat, 4/4

    def __init__(self, secs=120, bars=4, gain=21.0):
        self.hist = np.zeros((48000 * secs, 2), dtype=np.int32)
        self.w = 0              # frames recorded so far
        self.clip = None
        self.pos = 0
        self.bars = bars
        # Playback gain: the Rytm scales Overbridge MAIN in by 0xeb2a7c30 (-0.163,
        # -15.8 dB) before its return [OS 1.73 disassembly], and in rytm-emu a loop
        # came back 26 dB below the live mix; x21 makes them match there.
        # Calibrate on a real Rytm (--loop-gain).
        self.gain = gain
        self.lock = threading.Lock()
        # Looper mode (rytm-mods 0022: TRK + REC, pads as slots)
        self.clips = {}         # slot -> frames (on the Teensy: files on SD)
        self.playing = None     # slot
        self.pending = None     # ("rec" | "play" | "stop", slot), at the next bar
        self.rec = None         # (slot, start frame, end tick)
        self.ticks = None       # clock ticks since START, None when stopped
        self.last_tick_w = None
        self.frames_per_tick = 48000 * 60 / 120 / 24   # 120 BPM until measured
        self.out = None         # a mido output to the Rytm (slot states for its pads)
        self.env = 0.0          # the playing clip's fade level, and where it heads
        self.target = 0.0
        self.prev = None        # (clip, pos, level): a clip fading out
        self.xfade = None       # (slot, end frame): a loop point still to smooth

    FADE = 480                  # 10 ms in / out (loops only; the Rytm's mutes are instant)
    XF = 240                    # 5 ms crossfade at the loop point

    EMPTY, QUEUED, RECORDING, PLAYING, STOPPED = range(5)

    def mutes(self, on):
        """Hand the Rytm's tracks over to the loop (mute all) or back (restore)."""
        if self.out is not None:
            self.out.send(mido.Message("sysex", data=[0x7D, 0x52, 0x54, 0x08, 1 if on else 0]))

    def _start(self, slot):
        """Play a slot's clip from its start, fading in; another clip fades out."""
        if self.clip is not None and self.target > 0:
            self.prev = (self.clip, self.pos, self.env)
            self.show(self.playing, self.STOPPED)
        else:
            self.mutes(True)
        self.playing, self.clip, self.pos = slot, self.clips[slot], 0
        self.env, self.target = 0.0, 1.0
        self.show(slot, self.PLAYING)

    def show(self, slot, state):
        """Tell the Rytm a slot's state (rytm-mods 0022 lights the pad)."""
        if self.out is not None:
            self.out.send(mido.Message("sysex", data=[0x7D, 0x52, 0x54, 0x07, slot, state]))

    def record(self, frames):
        n = len(frames)
        idx = (self.w + np.arange(n)) % len(self.hist)
        self.hist[idx] = frames[:, 0:2]
        self.w += n
        # Transport stopped: a recording ends after its length in frames.
        with self.lock:
            if self.xfade and self.w >= self.xfade[1] + self.XF:
                slot, end = self.xfade
                self.xfade = None
                c = self.clips.get(slot)
                if c is not None and len(c) > 2 * self.XF:
                    after = self.hist[(end + np.arange(self.XF)) % len(self.hist)].astype(float)
                    r = (np.arange(self.XF) / self.XF)[:, None]
                    c[:self.XF] = (c[:self.XF] * r + after * (1 - r)).astype(np.int32)
            if self.rec and self.ticks is None and self.w >= self.rec[2]:
                self._finish_rec(self.rec[2])

    # -- looper mode ------------------------------------------------------
    def mode(self, on, page):
        print(f"looper: mode {'on' if on else 'off'}, page {page + 1}")

    def pad(self, slot):
        with self.lock:
            if slot == self.playing:
                act = ("stop", slot)
            elif slot in self.clips:
                act = ("play", slot)
            elif self.rec is None:
                act = ("rec", slot)
            else:
                print(f"looper: slot {slot + 1}: busy recording slot {self.rec[0] + 1}")
                return
            if self.ticks is None:
                self._do(act)
            else:
                if self.pending and self.pending[1] != slot:
                    p = self.pending[1]
                    self.show(p, self.PLAYING if p == self.playing else
                              self.STOPPED if p in self.clips else self.EMPTY)
                self.pending = act
                self.show(slot, self.QUEUED)
                print(f"looper: slot {slot + 1}: {act[0]} at the next bar")

    def clock(self, msg):
        with self.lock:
            if msg == "start":
                self.ticks = 0
                self.last_tick_w = self.w
                if self.pending:
                    self._do(self.pending)
                    self.pending = None
                return
            if msg == "stop":
                self.ticks = None
                return
            if msg == "continue":
                self.ticks = self.ticks or 0
                return
            if self.ticks is None:
                return
            if self.last_tick_w is not None:
                d = self.w - self.last_tick_w
                if 0 < d < 48000:
                    self.frames_per_tick = 0.95 * self.frames_per_tick + 0.05 * d
            self.last_tick_w = self.w
            self.ticks += 1
            if self.ticks % self.BAR_TICKS == 0:
                if self.rec and self.ticks >= self.rec[2]:
                    self._finish_rec(self.w)
                if self.pending:
                    self._do(self.pending)
                    self.pending = None

    def _do(self, act):
        kind, slot = act
        if kind == "rec":
            if self.ticks is None:
                end = self.w + int(self.bars * self.BAR_TICKS * self.frames_per_tick)
            else:
                end = self.ticks + self.bars * self.BAR_TICKS
            self.rec = (slot, self.w, end)
            self.show(slot, self.RECORDING)
            print(f"looper: slot {slot + 1}: recording {self.bars} bars")
        elif kind == "play":
            self._start(slot)
            print(f"looper: slot {slot + 1}: playing")
        elif kind == "stop":
            self.target = 0.0
            self.playing = None
            self.mutes(False)
            self.show(slot, self.STOPPED)
            print(f"looper: slot {slot + 1}: stopped")

    def _finish_rec(self, end_w):
        slot, start, _ = self.rec
        n = min(end_w - start, len(self.hist))
        idx = (start + np.arange(n)) % len(self.hist)
        self.clips[slot] = self.hist[idx].copy()
        self.rec = None
        self.xfade = (slot, end_w)
        self._start(slot)
        rms = int(self.clips[slot].astype(float).std() / 65536)
        print(f"looper: slot {slot + 1}: recorded {n / 48000:.2f} s (rms {rms}), playing")

    def capture(self, frames):
        """The last `frames` of MAIN as a clip, looping from now."""
        frames = min(frames, self.w, len(self.hist))
        idx = (self.w - frames + np.arange(frames)) % len(self.hist)
        self.clip = self.hist[idx].copy()
        self.pos = 0
        self.env, self.target = 0.0, 1.0
        rms = int(self.clip.astype(float).std() / 65536)
        print(f"looper: captured {frames / 48000:.2f} s of MAIN (rms {rms}), looping")

    def stop(self):
        self.target = 0.0
        print("looper: stopped")

    def _ramp(self, level, target, n):
        """Per-frame gain from level toward target over FADE frames."""
        step = 1.0 / self.FADE
        g = level + np.sign(target - level) * step * np.arange(1, n + 1)
        return np.clip(g, min(level, target), max(level, target))

    def _mix(self, out, clip, pos, g):
        n = len(out)
        idx = (pos + np.arange(n)) % len(clip)
        y = out[:, 0:2].astype(np.int64) + (clip[idx] * (g * self.gain)[:, None]).astype(np.int64)
        out[:, 0:2] = np.clip(y, -2**31, 2**31 - 1)
        return (pos + n) % len(clip)

    def play(self, out):
        n = len(out)
        with self.lock:
            if self.prev is not None:
                clip, pos, level = self.prev
                g = self._ramp(level, 0.0, n)
                pos = self._mix(out, clip, pos, g)
                self.prev = None if g[-1] <= 0 else (clip, pos, g[-1])
            if self.clip is None:
                return
            g = self._ramp(self.env, self.target, n)
            self.pos = self._mix(out, self.clip, self.pos, g)
            self.env = float(g[-1])
            if self.target == 0.0 and self.env <= 0.0:
                self.clip = None


def activity(host, log, t0, frames, main_peak):
    """One --log line: wall time, frames served, trigs (SysEx / MIDI notes) and
    parameter reports since the last line, the Plaits voices' peak, the Rytm's
    MAIN peak (dBFS), the looper's state and the parameters of voice 0."""
    a = host.act
    lp = host.looper
    db = lambda x: f"{20 * np.log10(x):6.1f}" if x > 0 else "  -inf"
    p0 = " ".join(f"{x:.0f}" for x in host.params[0])
    log.write(f"{time.monotonic() - t0:8.2f} {frames:9d} trig {a['trig']:3d} midi {a['midi']:3d} "
              f"rep {a['rep']:3d} voices {db(a['peak'])} main {db(main_peak)} "
              f"playing {lp.playing} rec {lp.rec} | v0 {p0}\n")
    log.flush()
    host.act = {"trig": 0, "rep": 0, "midi": 0, "peak": 0.0}


def serve(host, addr, loop_test=None, log=None):
    h, p = addr.rsplit(":", 1)
    s = socket.create_connection((h, int(p)))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"overbridge link: connected to {addr}")
    buf = b""
    frames = 0
    sizes = {b"N": 5, b"T": 4, b"P": 10}
    received = 0
    t0, logged, main_peak = time.monotonic(), 0, 0.0

    def need():
        # R: b'R' + u32 n + n x 12 x i32 (the Rytm's Overbridge output)
        if buf[0:1] == b"R":
            return 5 if len(buf) < 5 else 5 + struct.unpack("<I", buf[1:5])[0] * 48
        return sizes.get(buf[0:1], 1)

    while True:
        while not buf or len(buf) < need():
            d = s.recv(4096)
            if not d:
                print(f"overbridge link closed after {frames} frames")
                return
            buf += d
        kind = buf[0:1]
        if kind == b"R":
            k = need()
            fr = np.frombuffer(buf[5:k], "<i4").reshape(-1, 12)
            buf = buf[k:]
            host.looper.record(fr)
            host.feed(fr)
            if log and len(fr):
                main_peak = max(main_peak, float(np.abs(fr[:, 0:2]).max()) / 2147483648)
            received += len(fr)
            if loop_test:
                at, secs, until = loop_test
                if host.looper.clip is None and received >= at * 48000 and received - len(fr) < at * 48000:
                    host.looper.capture(int(secs * 48000))
                if received >= until * 48000 and received - len(fr) < until * 48000:
                    host.looper.stop()
            continue
        if kind not in sizes:
            sys.exit(f"unexpected {buf[:5]!r}")
        msg, buf = buf[:sizes[kind]], buf[sizes[kind]:]
        if kind == b"T":
            host.link_trig(msg[1], msg[2], msg[3])
        elif kind == b"P":
            host.link_params(msg[1], msg[2:10])
        else:
            n = struct.unpack("<I", msg[1:5])[0]
            a = host.render(n)
            s.sendall(b"A" + struct.pack("<I", n) + a.astype("<i4").tobytes())
            frames += n
            if log and frames - logged >= 24000:
                activity(host, log, t0, frames, main_peak)
                logged, main_peak = frames, 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--link", default="127.0.0.1:7301")
    ap.add_argument("--ext", default="", help="tracks to set to EXT, e.g. 0,1,8")
    ap.add_argument("--midi-from", default="rytm-emu usb out")
    ap.add_argument("--midi-to", default="rytm-emu usb in")
    ap.add_argument("--midi-din", default="rytm-emu in", help="for --ext")
    ap.add_argument("--loop-gain", type=float, default=21.0,
                    help="loop playback gain (x21 matches the live mix in rytm-emu)")
    ap.add_argument("--loop-test", default=None,
                    help="AT,SECS,UNTIL: at AT s of Rytm output, loop its last SECS s of MAIN until UNTIL s")
    ap.add_argument("--samples", default=None,
                    help="DIR standing in for the Teensy's SD card (default sim/samples): "
                         "its folders and .wav files the TEENSY folder's")
    ap.add_argument("--psram", action="store_true",
                    help="DSP SAMPLE as on a Teensy with PSRAM: the pool's samples from RAM, "
                         "MOD (SD SAMPLE word 20) picking the playback mode (docs/psram.md)")
    ap.add_argument("--log", default=None,
                    help="FILE: a line of activity every 0.5 s (trigs, reports, levels, looper)")
    args = ap.parse_args()
    host = Host()
    if args.psram:
        host.use_psram()
    if args.samples:
        host.lib = Library(args.samples)
    print(f"samples: {host.lib.root}: " + (", ".join(f"FLD {i} {d} ({len(f)})" for i, (d, f) in
                                                     enumerate(host.lib.folders)) or "none"))
    host.looper.gain = args.loop_gain
    try:
        host.din = mido.open_output(find(mido.get_output_names(), args.midi_din))
    except (OSError, SystemExit, StopIteration, ValueError) as e:
        print(f"no MIDI in on the Rytm ({e}): --ext not applied")
    if args.ext and host.din is not None:
        for t in (int(x) for x in args.ext.split(",")):
            host.din.send(mido.Message("control_change", channel=t, control=15, value=EXT_MACHINE))
            print(f"track {t} -> EXT")
    inp = mido.open_input(find(mido.get_input_names(), args.midi_from), callback=host.midi)
    host.looper.out = mido.open_output(find(mido.get_output_names(), args.midi_to))
    try:
        lt = tuple(float(x) for x in args.loop_test.split(",")) if args.loop_test else None
        log = open(args.log, "w") if args.log else None
        serve(host, args.link, lt, log)
    finally:
        inp.close()


if __name__ == "__main__":
    main()
