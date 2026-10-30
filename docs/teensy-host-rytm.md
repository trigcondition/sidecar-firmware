# Teensy 4.1 as USB host of the stock Rytm: first hardware run (2026-10-15)

Setup: Teensy micro-USB to the Mac (power, serial); the PJRC-style host cable on 5 male pins soldered to the
Teensy's USB Host pads (joints not reflowed yet); the Rytm's A-to-B cable in it, no hub. Analog Rytm MKII, stock
OS, USB CONFIG = OVERBRIDGE, its sequencer playing (a track sending note 36 on MIDI channel 1). Same Rytm setup as
the Pi engine run of the same day (`docs/pi-engine.md`). Nothing was sent to the Rytm but the Overbridge stream,
USB MIDI reads, and (voiceloop) its periodic `F0 7D 52 54 1D ...` plugin-list message, which carries the
non-commercial ID 7D that the stock OS ignores. All [measured] unless tagged.

## 1. Enumeration (obhost_bringup)

- `Elektron device 1935:0B48 enumerated at 480 Mbit (high speed)`, at once, at every one of four Teensy reboots.
  No full-speed fallback, no re-enumeration while streaming. The unreflowed joints carry high speed.
- `d` (descriptors): the same as rytm-emu's OS 1.73 table in `docs/HOST-BRINGUP.md`: 6 interfaces, 0x83/0x03
  interrupt 32/88/368 B at alt 1/2/3 with bInterval 1, interface 3 bulk 0x02 and interrupt 0x82, MIDI on interface
  5 (bulk 0x01/0x81 512 B). Parsed as `in 368 B / out 368 B, 12 / 12 channels, MIDI interface 5`.
- `MIDI attached: 1935:0B48 'Elektron Music Machines' 'Elektron Analog Rytm MKII'`.
- The Rytm's MIDI: `90 ch 1 36 100` / `80 ch 1 36 0` with the pattern, and the clock at 120 BPM (48 clocks a
  second). In the first session after plugging in, no MIDI at all (no notes, 0 clocks) for about 50 s while
  the audio already streamed. Every later session had the notes and clock [check: whether the Rytm's USB MIDI
  out starts late after it is plugged in, or the sequencer was stopped then].

## 2. Overbridge stream

obhost_bringup (`g`, FIFO target 128), 30 s, and again 27 s:

- IN 47992-48006 Hz (6856-6858 blocks a second), OUT equal to IN. Over 205702 blocks: bad 0, counter gaps 0,
  unanswered 0, rearms 0/0, setup timeouts 0, queue fails 0, xruns 0. The FIFO stayed between 121 and 142.
- SET_INTERFACE 1/2 alt 3 and CLEAR_FEATURE 0x83/0x03 were answered. Streaming started within the first second.
- The Rytm's audio arrives: the BD track peaks at 0.253 and MAIN at 0.18 with the pattern.
- Teensy -> Rytm, as a loopback (`t`: a 440 Hz sine at -12 dBFS on voice 0's input, Overbridge channel 4): the
  BD track's peak in IN rose from 0.253 to 0.289 and MAIN from 0.18 to 0.197. Both fell back to the baseline
  when the tone stopped. With `m` (MAIN mirror) off, MAIN stayed at 0.197: the tone reaches MAIN through BD's
  channel, not through the Rytm's MAIN input [inferred].

voiceloop, host build (`RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1"`, plugins on, FIFO target 256):

- The boot was normal. The card, index, pool and plugin store mounted, and U1 `rytm.sine_test` / U2
  `rytm.fault_test` were loaded. Then `ob: streaming` and `MIDI attached`.
- Run of 200 s, 99 reports: in/out 47999-48006 Hz, FIFO min 249, max 256. xruns 0, bad 0, gaps 0, unanswered 0,
  rearms 0/0, queue fails 0, stream underruns 0. Clock 120 BPM. The first report (the start) has min 0 and in
  43435 Hz, a partial second.
- A Teensy voice into the Rytm: 20 0x04 trigs over the serial link (`sim/protocol.py` `trig_params`, voice byte
  U1|0 = voice 0 on rytm.sine_test, note 69). Each one logged `trig voice 0 U1 rytm.sine_test note 69`. The cpu
  line went from 0.5 % to 2.1 % (peak block 2 %), with xruns and underruns still 0. Voice 0 goes to Overbridge
  channel 4, the path the loopback above measured. Its level in the Rytm was not measured in this build:
  voiceloop's log has no IN peaks.
- The stock Rytm's MIDI notes did not play a Teensy voice in this first build: it played only the mod firmware's
  `F0 7D 52 54 04/03` trig reports, which the stock OS does not send. Section 3 adds the note adapter.

## 3. The stock Rytm's notes play Teensy voices (note adapter, same day)

The Pi engine's note adapter moved into the core (`core/notes.*`, docs/core.md "MIDI notes"). voiceloop feeds it
the Rytm's USB MIDI note-on/off and CC (host build) or `'M'` frames (serial build), switched at runtime with
serial console lines `:notes=1` / `:notes=0` / `:notemach=V:NAME` / `:notes`. Off at boot (`-DRYTM_NOTES=1` turns
it on), so the mod firmware's trig reports stay the only trigs by default. Every 2 s the host log now also has
the Rytm's IN peaks (`usb host: peaks (x1000) main L R | BD .. CY | in L R`).

Host build, the stock Rytm's pattern (note 36 on MIDI channel 1), notes counted at 14 per 2 s either way:

| run | trigs | BD IN peak (x1000) min / mean / max | MAIN mean | xruns, gaps, underruns | cpu |
|---|---|---|---|---|---|
| notes off, 20 s (baseline) | 0 | 253 / 253.9 / 254 | 180 | 0, 0, 0 | 0.5 % |
| `:notes=1`, voice 0 PLAITS VA, 200 s (100 reports) | 1402 `trig voice 0 note 36 engine 8`, none on voices 1..7 | 279 / 289.5 / 350 | 179 | 0, 0, 0 | 6.6 % |
| `:notemach=0:dd` DIGIDRUM EFM-BD, 30 s | 211 | 467 / 469.5 / 473 | 176 | 0, 0, 0 | 1.9 % |
| `:notemach=0:fm` 6-OP FM entry 0, 30 s | 212 | 253 / 271.8 / 273 | 181 | 0, 0, 0 | 7.1 % |

Voice 0's level reaches the Rytm on BD's channel, as the loopback in section 2 did (MAIN stays at its baseline:
the voice enters through BD's channel, BD's own VCA shaping it [inferred]). FIFO 249..256, in/out 47992..48006
Hz throughout. Serial build (`'M'` frames): boots off, no trig while off, `:notes=1` trigs voice 0 on channel 1
note 36, `:notemach=1:juno` plays channel 2 note 60 as `trig voice 1 JUNO`, an unknown line is answered.

The gate's `rmp_e2e.py --host` with this Rytm attached and streaming: 131 of 153 pass; the 22 failures are every
audio check (`peak 0`): while Overbridge streams, the serial link's `'N'` requests get silence by design (the
voices go to the Rytm, `send_silence`). Its test tones (sine_test at 440 Hz with pitch steps, the Wide preset's
+100 cents, TUN +12) went to the Rytm instead, heard there as a short sine sweep once [inferred: the e2e run's
timing]. The suite needs the Rytm off or unplugged from the Teensy's host port.

## 4. Output to MAIN for a stock Rytm (`:out=main`, same day)

Section 3's notes played voice 0, but only on BD's Overbridge channel: the stock OS plays only the Overbridge
MAIN L/R inputs, so nothing was audible (BD IN peak 254 -> ~289, MAIN unchanged). The host build now has the Pi
engine's output choice, from the shared `core/main_out.h` (docs/core.md "MAIN output"):

- `:out=tracks` (the default; the mod build): each voice on its track's channel, as before (no limit, no fade).
- `:out=main` (a stock Rytm): the 8 voices summed at x0.5 onto MAIN L/R, faded in over 200 ms (on the switch and
  on each new stream), hard-limited at -12 dBFS. The other channels are silent.
- `:out` prints the state; `:notes=1` while on tracks reminds you that a stock Rytm needs `:out=main`.
- `-DRYTM_OUT_MAIN=1` boots with main. The boot log says `output: main ...` or `output: tracks ...`; every 2 s
  with main the host log adds `usb host: out main, sent peak (x1000) N` (the level the Teensy sends; MAIN IN's
  peak in the `peaks` line is what the Rytm sends back).

A stock Rytm, plugged in, with the Teensy on the host build:

```
:notes=1          the Rytm's MIDI notes play the voices (channel N = voice N-1)
:out=main         the voices on MAIN L/R (audible on a stock Rytm)
:notemach=0:fm    optional: voice 0's machine (plaits juno daisy dsps grain fm; dd, DIGIDRUM, removed 2026-10-29; the Teensy has no built-in MD)
:out=tracks       back to the per-track channels (the mod firmware)
:notes=0          notes off
```

Typed into any serial monitor on the Teensy's micro-USB port (`teensy/serial_read.py`, or the Arduino IDE's),
each line ending in Enter. A build that boots straight into this:
`RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1 -DRYTM_NOTES=1 -DRYTM_OUT_MAIN=1" teensy/run_sketch.sh voiceloop`. (A stock Rytm only: with the mod
build the defaults - tracks, notes off - are the normal setup; `teensy/host_uac2.sh`, the Sidecar build, boots
with them and keeps `:out` / `:notes` as last set.)

Placement: the mix (`main_mix`) and the switch run from program flash; the ITCM change is only `host_feed`'s
branch and the library's map pointer, and the host build's `'N'` path (`send_frames`, tests only there) moved to
flash. Sizes (gate, before -> after):

| build | ITCM code | ITCM headroom | flash code |
|---|---|---|---|
| serial | 118280 -> 118280 | unchanged | 281692 -> 281852 |
| host | 128904 -> 128648 (-256) | 2168 -> 2424 B | 301044 -> 301788 |

Gate (`teensy/gate.sh full`, the Rytm unplugged from the Teensy): PASS - golden renders, `rmp_e2e.py` serial 154/154 and `--host`
163/163 (the 153 earlier checks, which failed 22 audio checks only because the Rytm was streaming in section 3,
plus step 14's new ones), sizes, plaits_bench, the plugin-less build. `rmp_e2e.py` step 14 checks
the console lines and, with `:out=main` and no stream, the `'N'` frames: slots 1..7 silent, slot 0 the 440 Hz
sine at most 8192 (-12 dBFS), faded in from 0. Not yet heard on a stock Rytm.

## 4. Latency and the FIFO target (2026-10-15, stock Rytm MKII)

Setup: the Teensy as the Rytm's USB host, `RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1 -DRYTM_NOTES=1 -DRYTM_OUT_MAIN=1"`;
the Rytm (OVERBRIDGE) playing a pattern with notes on channels 1..12 (70 note-ons per 2 s, 120 BPM), its own
voices muted, the metronome on. Teensy voices 0..7: dsps (pool slot 1, streamed from the card: 900 refills per
150 s), fm (P1 10), dd EFM-SD, EFM-BD, EFM-HH, EFM-CP, EFM-CY, juno. cpu 21.9 %, peak block 26 %.

New console lines (host build, flash only; ITCM 128648 -> 128776 B, headroom 2296 B):

- `:fifo=N` sets the FIFO target (24..RYTM_OB_FIFO - 32 frames), `:fifo` shows it.
- `:cap=MS` captures MS ms of the Overbridge IN stream. Each IN frame carries IN MAIN L/R (top 16 bits), OUT slot 0
  as handed to the Rytm at that IN frame, and marks: MIDI note-on arrival per channel 1..8, and the OUT frame
  where that voice's trigger takes effect. It is sent as `W` log lines, and nothing is allocated (11 KB of RAM2
  heap) or hooked until a capture runs. `tools/teensy_latency.py --port P --ms 20000 --channel N` reads the
  capture into spike3's 4-channel WAV and runs `pi/circle/spike3/latency.py` on it.

### Latency

Measured: the MIDI note-on's arrival at the Teensy to the voice in IN MAIN = arrival -> trigger frame sent
(`:cap` marks) + the OUT -> IN loop (GCC-PHAT of OUT sent against MAIN: 241 samples = 5.02 ms, gain -13.0 dB,
correlation 0.80, the same at both targets; the Pi's was 289 samples).

| FIFO target | notes | arrival -> trigger sent | arrival -> voice in MAIN | voice after the Rytm's drum [inferred] |
|---|---|---|---|---|
| 256 (old default) | 40 (ch 4, EFM-BD) | median 5.44 ms (5.17..5.54) | median 10.46 ms (10.19..10.56) | median 7.20 ms (6.93..7.30) |
| 96 (new default) | 320 (ch 1..4) | median 2.00 ms (1.12..2.21) | median 7.02 ms (6.15..7.23) | median 3.76 ms (2.89..3.97) |

The last column is not the Pi's direct measurement. The Rytm's voices were muted, and its metronome is not in
Overbridge IN MAIN: the residual after the Teensy's voices are subtracted has no onset before them.
So the column takes the Pi's own measurement of this stock Rytm, that its
drum's onset in MAIN comes 3.26 ms after its note-on reaches the host, and subtracts it. That assumes the Rytm's
USB MIDI timing is the same with the Teensy as host. The same composition gives the Pi 1.87 + 6.02 - 3.26 = 4.63 ms
against its measured 4.71 ms. To do: measure directly with one Rytm track unmuted, using the same capture
(the drum then shows in the residual, as on the Pi).

### FIFO sweep

`:fifo=N` live, 6 s settle, then 120-150 s of the 2-second reports. "FIFO min" is the lowest level the OUT side saw:
about 49 frames (1 ms) below the target every time, set by the longest loop pass. No step had bad blocks,
counter gaps, unanswered blocks or queue fails. In was 47992..48013 Hz throughout.

| target | time | xruns (blocks / frames) | FIFO min | stream underruns | max loop | max refill | max request |
|---|---|---|---|---|---|---|---|
| 256 | 120 s | 0 / 0 | 207 | 0 | 840 us | 570 us | 234 us |
| 192 | 120 s | 0 / 0 | 143 | 0 | 858 us | 571 us | 234 us |
| 128 | 150 s | 0 / 0 | 79 | 0 | 861 us | 573 us | 235 us |
| 96 | 150 s + 150 s | 0 / 0 | 47 | 0 | 853-863 us | 572 us | 235 us |
| 64 | 150 s + 150 s | 0 / 0 | 15 | 0 | 840-862 us | 576 us | 234 us |
| 48 | 150 s | **58 / 232** | 0 | 0 | 859 us | 571 us | 234 us |

The second runs at 64 and 96 had notes counted in every report (70 per 2 s). The first sweep included a short
stretch with the Rytm's sequencer stopped. 64 is the smallest clean target, so the host default
(`RYTM_HOST_TARGET`) is now 96 (2 ms), one step up, the Pi's rule (its USB default is 96 too). Not covered: a
Rytm project change or a long card operation. PLAN 3c measured a project change at 3.8 ms in the loop, which would
underrun a 96-frame FIFO [inferred, not re-measured]. Use `:fifo=256` where that matters.

Checks: `make -C core test` ALL PASS; `sim/rmp_e2e.py` serial 154/154 ALL PASS. `teensy/gate.sh full` (with
`rmp_e2e.py --host`) was not run, because the Rytm stays plugged in while the user listens. Rerun it with the
Rytm unplugged.

## Compared

| | Teensy 4.1 host (this run) | Pi 4 engine (`docs/pi-engine.md`) |
|---|---|---|
| link | 480 Mbit, no hub, 0 errors | xHCI, 0 gaps over 10 min |
| blocks a second | 6857 IN = OUT | 6857 |
| host-path CPU | not separable on hardware. obhost_bench estimated 3.5 % [inferred]. voiceloop's own cpu line 0.5 % idle, 2.1 % with the sine voice | core 0 |
| voices from the Rytm's notes | every machine, `:notes=1`; FIFO 96: note arrival -> voice in MAIN 7.02 ms, 3.76 ms after the drum [inferred] (section 4) | every machine, 4.71 ms after the drum (measured) |

## 5. On the modded Rytm (2026-10-16) [measured, by ear]

The Rytm on rytm-mods `fsr` (mod 0021-ext, flashed 2026-10-15), the Teensy host build
with `:notes=0` and `:out=tracks`: EXT machines chosen on tracks play their Teensy voices
on those tracks, through each track's analog chain, as with the Pi engine the day
before. `fsr` has no SysEx router, so names the Teensy itself sends (user slots, the
TEENSY folder) need the `hwus1nl` build.

## 6. Send FX on the Teensy (2026-10-17)

The core's SendFx (core/fx/send_fx.h) runs in the host build with the small set: CHORUS (1), COMB (2), CRUSH (16),
RINGMOD (17), HALO (18, 2026-10-18); RINGS / CLOUDSEED / STUTTER (6, 8, 15) stay MISSING (RYTM_SENDFX_LARGE 0). Glue:
teensy/voiceloop/sendfx_host.h. The stems (Overbridge IN 2..9) x each track's SEND FX 1/2 level go through both
effects in the USB host interrupt after each IN transfer; the return is added to the next OUT blocks, per slot's
P1 = ROUTE: 0..63 PRE on OUT 3-4 (MAIN FX), 64..127 POST on OUT 1-2 (MAIN). The stems are batched by three transfers (42 frames a Process call); the return ring is primed at 58 frames (1.2 ms).

- Memory: 16-bit delay lines (RYTM_SENDFX_LINE16): CHORUS 2048 samples a slot, COMB 1024 a side. SendFx 17292 B
  in RAM1 (bss, placed in setup), the return ring 1 KB, the stem batch 1.5 KB; nothing allocated, no PSRAM. Build on
  4f485f4 vs this: RAM1 variables 304736 -> 324704 B, free for locals 88480 -> 68512 B, ITCM 4 blocks (headroom
  2024 -> 1832 B, unwind entries only: no new ITCM code), RAM2 unchanged (18112 B free). Nothing removed: no
  plugin, user machine or plugin buffer was dropped or shrunk.
- CPU (measured on the Teensy 4.1, ':sendfxbench=42', 8 full-scale noise stems, all sends 127, FX1 PRE + FX2 POST,
  cycles per 12 frames of 150000): first build 18456 (12.3 %, max 33 %); now chorus + comb 6932 (4.6 %), chorus x2
  4.2 %, comb x2 5.0 %, crush + ringmod 4.4 %, the largest call 5.5 %. What did it: the stems batched to 42-frame
  calls (the per-call cost ~3600 cycles), the send mix and every smoother as per-block linear ramps (the 8 gains in
  registers, both slots in one pass), the effects' powf / exp2f / cosf only on a knob change, the chorus LFO once a
  block, COMB's fixed-delay read, branch-free limits and line clamps (vminnm / vmaxnm, vrintm), one NaN check a
  block, each pair's limit only when a slot reached it. Code stays in flash: ITCM measured no faster (the loops are
  FP-latency bound, the M7 in order).
- Knob maps: as the Pi's at 48 kHz. CHORUS keeps 8..25 ms delay + 6 ms depth (the 2048 line is 42 ms); COMB's
  lowest TUNE (55 Hz, R 3 % lower with SPRD) is 899 samples, inside 1024. Deviation: 16-bit lines (full scale
  +-2.0, ~-84 dB noise re 1.0; a feedback tail truncates to exact 0); at 96 kHz COMB's lowest notes would clamp.
- CRUSH: P2 RATE hold 1..64 samples, P3 BITS 16..1, P4 DRV 0..+24 dB, P5 TONE low-pass 18 kHz..400 Hz.
  RINGMOD: P2 FREQ 10..5120 Hz, P3 SPRD R carrier x1..1.5, P4 SHPE sine..square, P5 LFO 0 off / 0.1..10 Hz +-6 st.
  Both have no memory and play in every build (the Pi too).
- HALO (2026-10-18, docs/core.md "HALO"): a small lush modulated plate, P2 DCY T60 0.3..30 s (127 FRZ = FREEZE), P3 MOD,
  P4 TONE, P5 SHIM (octave-up shimmer). 8.3 KB a slot in the block the small types now share (one plays at a time):
  the build on mini-verb vs sidecar-1023: RAM1 variables 357632 -> 357568 B, locals 35584 -> 35648 B, ITCM unchanged
  (125544 B, 4 blocks, headroom 5528 B: HALO's code in flash), RAM2 unchanged (6400 B free), flash code +6.6 KB. CPU
  estimated ~5.5 % a slot (an ARM emulator's instruction count x the chorus + comb calibration; ':sendfxbench' now
  also runs halo + chorus, halo x2 and halo alone - to be measured on the device).
- Console: ':sendfx' (the core's line, then frames, cycles a 12-frame block mean/max, interrupt load, underruns,
  drops, peak, faults), ':sendfx=K:TYPE', ':sendfxout=', ':sendlvl=', ':sendknob=', ':sendfxbench' (both slots
  on synthetic stems, only without a stream).

## Open

- The Teensy's audio at the Rytm's outputs, by ear or captured: the sine trig on voice 0 should sound on BD while
  BD's VCA is open (the pattern's BD hits).
- The latency against the Rytm's own drum measured directly (one track unmuted, `:cap`), and the FIFO 96 default
  through a project change. `teensy/gate.sh full` with the Rytm unplugged (host e2e).
- `:out=main` on the stock Rytm: heard at its outputs, the MAIN IN peak with notes on, xruns at the switch.
- Reflowing the joints is still advised for the long term. Nothing in this run pointed at them.
