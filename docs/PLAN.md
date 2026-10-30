# Plan (2026-10-07; updated 2026-10-09)

## Goal

A Teensy 4.1 on the Rytm MKII's USB port as an external voice engine: the patched
Rytm sends trigs, notes and the machine's parameters; the Teensy renders (Plaits
and other engines) and returns audio, which the Rytm plays through the voice's
digital-layer buffer A (and so through that voice's analog filter, overdrive and
amp, exactly as FM4 does). The machine on the Rytm side looks like any other: a
hidden machine slot, renamed, with its knobs on SRC and SMPL.

## Scope

Analog Rytm MKII only. The MKI is USB 2.0 Full Speed and runs Overbridge v1 over
isochronous endpoints (Overbridge manual; Overwitch README), so neither the
Overbridge-host route (interrupt endpoints, high speed) nor the bandwidth numbers
below apply to it; at Full Speed, SysEx would carry one or two voices at most.

## What the research found (docs/research-*.md, 2026-10-07)

- The Rytm MKII's USB is high speed [both Elektron manuals]. Class-compliant audio
  mode is stereo only (USB IN/OUT L/R); the 12 channels are Overbridge's.
- Overbridge 2 (Overwitch's reverse-engineered docs, GPL-3): interrupt endpoints
  (EP 0x83 Rytm->host, EP 0x03 host->Rytm, alt setting 3), 368-byte blocks of 7
  frames of int32 big-endian samples, about 2.5 MB/s each way; the Rytm is the
  clock master. Host->Rytm already carries 12 channels: MAIN L/R, MAIN FX L/R and
  the 8 voices, each voice's stream passing while that voice's amp is triggered.
- The Teensy's USB host library (USBHost_t36) does high-speed interrupt pipes and
  MIDI (MIDIDevice_BigBuffer for 512-byte packets) but no isochronous, so an
  Overbridge host fits it and a class-compliant audio host would not.
- No Teensy USB emulator is usable: the Teensy logic runs natively on the Mac
  behind a transport interface (this repo's sim/).

## Transport options, in order (revised)

1. Overbridge host on the Teensy (target). The per-voice host->Rytm streams are
   already the "external voice" path: the Teensy renders voice v into Overbridge
   input v, and the Rytm plays it through that voice's VCA while the voice is
   triggered. The Rytm side then mainly needs a silent machine (so only the
   Teensy's audio sounds) and to tell the Teensy about trigs and parameters (MIDI
   on the same USB connection, interface 5, or a side channel in the unused 28
   bytes of each Overbridge block [inferred]). To verify on the emulator and on
   hardware: where those streams enter the voice (before or after the analog
   filter and overdrive), and the VCA gating.
2. SysEx over USB MIDI (prototype, works today in the emulator): trigs and params
   out, 14-bit audio back at 96 KB/s a voice; 8 voices fit high-speed USB easily.
   The Rytm's MIDI parser cost is measured (about 1 instruction a byte for unknown
   SysEx, docs/measure-emulator.md); a mod must claim 0x7D and fill the voices.
3. A custom USB audio interface: dropped unless 1 and 2 fail.

Emulator work for option 1: model the MCF5441x USB device controller far enough to
run the Rytm's own Overbridge endpoints, then a host-side Overbridge client (from
Overwitch's documented format) in the sim, before the real Teensy.

## Done (2026-10-07)

- `sim/`: the Teensy side on the Mac: a draft protocol (`protocol.py`) and
  `teensy_sim.py` (stream audio blocks, listen to the Rytm).
- `docs/measure-emulator.md`: the Rytm OS takes up to 1.6 MB/s of unknown SysEx at
  about 1 instruction a byte; real-time streaming from the sim into the emulator's
  USB MIDI port at 108 KB/s (1 voice) and 432 KB/s (4 voices): all 22 500 blocks
  arrived, emulator at full speed.

## Working in the emulator (2026-10-07, night)

The whole loop runs: pad hit on the Rytm -> Rytm MIDI out -> simulated Teensy
(`sim/obhost.py`) -> Plaits -> Overbridge per-voice input -> the voice's analog
filter, overdrive and amp -> output.

- rytm-emu `--ob-link` (be6eb2c): the emulator serves the Overbridge host->Rytm
  stream over TCP (127.0.0.1:7301), asking for frames as the Rytm clocks them, and
  writes them where `voice_output_mix` reads the per-voice inputs. `dtboot --ob-in
  FILE.wav[:CH]` does the same from a WAV.
- rytm-mods 0021-ext (efm-173, fb899ca): EXT on machine 44, every track; DISABLE's
  engine keeps the voice's own source silent, P1..P8 carry CC 16..23.
- sim: `plaits/build.sh` builds Plaits (MIT) natively; `obhost.py` plays eight
  Plaits voices into the eight voice channels, triggered by the Rytm's MIDI out
  (pad notes on the auto channel, or track-channel notes), CC 16..23 = P1..P8
  (engine, harmonics, timbre, morph, decay, LPG colour, pitch, level).
- Checked headless: BD, SD and the CH track set to EXT; pad hits at 5, 7 and 9 s
  came back as Plaits at about 264 Hz (note 60), each shaped by its voice's AMP
  envelope (SD's shorter); emulator at full speed, 565 344 frames served.

Run it (window):

    cd ../rytm-emu
    ./target/release/rytm fw/AR173_ext.syx --card sections/ext.img \
        --snapshot sections/ext.snap --ob-link
    cd ../rytm-teensy/sim && uv run python obhost.py --ext 0,1,8

then play BD, SD or CH. The snapshot has the MIDI config the host needs (GLOBAL >
MIDI CONFIG > PORT CONFIG: OUTPUT CH = TRK CH, PARAM OUTPUT = CC, ENCODER DEST =
INT+EXT): a trig goes out as a note on the track's channel and P1..P8 as CC 16..23
[checked: BD hit -> `90 24 7F`, P1 turned -> `B0 10 40`]. Set the same on the real
Rytm.

Open: output level calibration (Overbridge full scale vs a real voice); latency
(the link keeps about 11 ms buffered; real Overbridge adds its own); SysEx from the
Rytm for parameters without MIDI config; EXT's knob widgets and a second page; the
Rytm->host channels (for a looper); the real USB controller.

## Done (2026-10-08)

- Rytm side, rytm-mods 0021 `ext_render`: the EXT machine reports each trig with
  its note, velocity and P1..P8 (14-bit, p-locks and LFO included) and P1..P8
  changes, as SysEx over USB MIDI (sim/protocol.py). The sequencer sends no MIDI
  notes, so this is how the host hears the pattern.
- Teensy, `teensy/voiceloop`: 8 Plaits voices driven by those reports, rendering
  per-voice audio on request; `sim/teensy_bridge.py` runs it against rytm-emu over
  the micro-USB cable. Checked: the emulator's sequencer plays the Teensy's voices.
- Plaits CPU on the Teensy (docs/measure-teensy-plaits.md): 5.8 % per voice on
  average, eight voices fit.

## Next

1. Host cable (ordered; or wire one: 5V, D-, D+, GND to the 5-pin header): the
   probe sketch (teensy/usbhost_probe) against the stock Rytm, then the
   Overbridge host on the Teensy (interrupt endpoints, alt 3, 12 x int32 each
   way; Overwitch is the reference), with the voice loop behind it.
   Written 2026-10-09, before the cable (docs/HOST-BRINGUP.md: the steps for
   the day, what each log line should say, fallbacks, assumptions):
   teensy/libraries/RytmHost (the Overbridge driver on USBHost_t36; USB MIDI is
   its stock MIDIDevice_BigBuffer), teensy/obhost_bringup (one step at a
   time), voiceloop with RYTM_USB_HOST 1 (the Rytm's reports from USB MIDI, the
   voices rendered on demand into the stream, the Rytm the clock). Descriptors
   and block format checked against the emulated Rytm; nothing run on hardware.
2. Calibration: output level against a real voice; latency and jitter end to end.
3. EXT polish: knob widgets, a second page (SMPL), names per Plaits parameter.
3a. Machines: DIGIDRUM (soejrd/digidrum's TRX and EFM machines, 2026-10-09;
   8 voices 13 % of the Teensy against Plaits' 50 %; replaced 2026-10-29 by
   the built-in MD on the same flag 0x40: core/md/md.h, docs/MD-BUILTIN.md). Later: split PLAITS into
   a 6-OP FM machine with its own parameter set (the three DX7 banks) and
   PLAITS with the rest of the models.
3a'. JUNO (2026-10-09): a Juno-106 voice of our own in float (sim/juno), on
   AMY's 106 model (factory patches as 18 sysex bytes, slider curves; MIT) -
   not AMY's runtime (176 KB of code, ~120 KB heap, 64-sample blocks). 8
   voices 23-29 % of the Teensy. Next: Juno-106 patch dumps (.syx, the same 18
   bytes) in a /JUNO folder, browsed and added per project as the DX7 voices
   below; the Teensy sending a patch's values back as CCs so the knobs show it.
3a''. Note-offs (2026-10-09): rytm-mods 0021-ext sends F0 7D 52 54 11 <voice> F7
   when a voice's gate closes (pad released, the trig's LEN elapsed, the
   sequencer stopped; sim/protocol.py). The hosts (sim/obhost.py, the Teensy's
   voiceloop) pass it to the voice: JUNO releases (LEN 127 now follows the
   Rytm's note length; LEN < 127 releases at its timed gate or the note-off,
   whichever comes first); the other machines play out and ignore it.
3a'''. Sample slots (2026-10-09): DSP SAMPLE picks from the project's pool as
   a stock-style slot list - SMP N is pool slot N (line N-1 of the pool file),
   0 OFF; FLD unused, the folder walk (09/0A/0B/0C) retired (still accepted,
   ignored). New: 12 ASSIGN <slot> <folder> <file> (a TEENSY file into a slot),
   13 ASK, and the host's 14 table (slot, folder/file, 13-char name; 6 a
   message) after a new project / the first 10, 0F, 12 and 13 (sim/protocol.py;
   sim/obhost.py and the voiceloop alike). Waits on rytm-mods 0021/0022/0023.
3a''''. JUNO knobs and names (2026-10-09): CUT RES ENV ATK DEC REL are now the
   patch's sliders as absolute values (bytes 5 6 7 11 12 14), not offsets. When
   the sound's PAT (0x03, or the first JUNO report) or the list entry it points
   at picks another patch, the host sends 15 <voice> <six values> so the Rytm's
   knobs show the patch, and plays the patch's own six until a report carries
   them (or 300 ms); a p-locked PAT on a trig (0x04) sends nothing. 16 sends
   the project's JUNO list's names (8 a message, empty slots too; total 0 = the
   factory names) with 14 after a new project / the first 10, on 13, and
   after LOAD TO PROJ of a JUNO patch. The list holds 127 slots now (7-bit
   total). sim/obhost.py reads a JUNO list from sim/pools/juno/ (no /JUNO
   browsing in the sim). Waits on rytm-mods 0021.
3b. 6-OP FM (2026-10-09; rytm-mods 0021 machine 40, voice flag 0x48): Plaits'
   six-op engine on DX7 voices (sim/plaits/fm_machine.h, the same code in the
   sim and on the Teensy). P1..P8 PAT TIM MOR DEC LPG TUN LEV LEN: PAT the
   effective FM list's entry min(PAT, total - 1), taken at a trig (a 0x03's
   PAT waits for the next trig: loading a voice cuts the sounding note); TIM
   and MOR Plaits' TIMBRE and MORPH (64 = the voice as stored, no knob sync);
   DEC/LPG Plaits' low-pass gate after the voice, pinged per trig, DEC 127 =
   none; TUN semitones; LEV level; LEN the gate as JUNO's. The machine is now
   decoded as voice byte & 0x78, exactly (0x48 is no longer read as DIGIDRUM
   or JUNO). The effective FM list is the project's /.rytm/fm/<PROJECT>.txt
   (bank MD5, voice, name per line) or else Plaits' 96 ROM voices; 17 sends
   its names (as 16), 18 asks for them. /DX7's banks (4104-byte .syx or bare
   4096 bytes) are indexed at boot (MD5 and names per bank, /.rytm/dx7.bin
   behind a hash of /DX7's listing; teensy/voiceloop/dx7_lib.h) and shown in
   TEENSY as a folder DX7 of /DX7's folders, each bank a folder of its 32
   voices; LOAD TO PROJ adds a voice to the FM list. JUNO banks (a .syx of
   several patch dumps) are folders of their patches the same way. The sim
   plays FM (ROM banks only, no /DX7 browsing; its list in sim/pools/fm/).
   Each reload of a voice unpacks 32 copies through Plaits' user-data hook
   (a stand-in user_data.h, sim/plaits/shim): one silent block, as Plaits'.
   Teensy CPU: not measured on hardware yet (the CPU log line in voiceloop
   reports it); Plaits' six-op measured 5.7 % a voice on average, 18 % peak at
   a patch load (docs/measure-teensy-plaits.md), so 8 FM voices about 46 %
   plus the LPG - to check with 8 FM voices and the other machines.
   Memory (2026-10-09, before 3c): RAM1 17 KB left for the stack, RAM2 3.7 KB
   for malloc, ITCM 4 KB below its next 32 KB step. Waits on rytm-mods 0021
   machine 40.
3c. Stress test, real time and headroom (2026-10-09). `sim/stress.py` drives
   the real Teensy without rytm-emu: the bridge's serial framing, rytm-emu's
   link pacing (the Rytm takes 32 frames every 0.667 ms and keeps 512 frames
   buffered or asked for; one request answered before the next, as the
   bridge), the reports as rytm-mods 0021 sends them (0x03 floods every 16
   blocks a voice). A Rytm block finding < 32 frames is a dropout; one within
   a buffer's length of the Mac's own stall counts as the host's. It runs
   single-threaded and spinning (a reader thread and time.sleep gave 5-20 ms
   host stalls on macOS). Project STRESS (pool: 8 long samples, 3.9-34 MB;
   switched back to the user's project after a run, `--restore`). Each run
   12 s; columns from the Teensy's log lines (cpu = render cycles against
   real time, worst 2 s window; peak = the worst 12-sample block; "w/ card"
   adds card reads), measured:

   | scenario | before: cpu / peak / dropouts | after: cpu / peak / dropouts |
   |---|---|---|
   | a 8 x 6-OP FM, a voice load per trig (PAT p-locked) | 38.6 / 171 % / 0 | 38.5 / 59 % / 0 |
   | e 8 x 6-OP FM loads on one block + 0x03 floods | 38.6 / 170 % / 0 | 38.4 / 49 % / 0 |
   | b 8 x JUNO chords, note-offs, PAT changes (15 back) | 27.0 / 35 % / 0 | 27.3 / 35 % / 0 |
   | c 8 x DSP SAMPLE streaming, STRING resonator | 92.9 / 94 % / 1891 | 39.1 / 42 % / 0 |
   | c2 8 x DSP SAMPLE, another sample every trig | (762, streams fixed) | 39.1 / 41 % / 0 |
   | cm 8 x DSP SAMPLE, MODAL resonator | 86.2 / 87 % / 1111 | 67.5 / 70 % / 0 |
   | d mixed 2 FM 2 JUNO 2 DSPS PLAITS DIGIDRUM, 150 BPM 16ths, knobs | 52.4 / 88 % / 20 | 39.1 / 46 % / 0 |
   | dl d + browsing TEENSY (0D) + project changes | (27) | 38.6 / 46 % / 0 |
   | e2 4 FM loads + 2 JUNO + 2 DSPS on one block, knobs | 49.6 / 119 % / 0 | 36.1 / 38 % / 0 |
   | f 8 x DAISYSP MODAL | 66.3 / 70 % / 0 | 58.1 / 61 % / 0 |
   | g 8 x PLAITS MODAL (the costliest model) | 86.3 / 88 % / 0 | 86.4 / 87 % / 0 |
   | h 8 x DIGIDRUM | 11.3 / 15 % / 0 | 12.3 / 13 % / 0 |
   | pw / pc / pt / ps 8 x PLAITS WAVETABLE / CHORDS / WAVE TERRAIN / SPEECH | 48.7 / 50.6 / 46.8 / 16.9 % | 49.0 / 50.7 / 46.9 / 17.1 % |

   Stream underruns: 0 in every run. What changed:
   - 6-OP FM: a voice load unpacked the 32 copies of the voice Plaits' six-op
     engine reloads (18 % of a block each); the Teensy's copy of the engine
     now unpacks only patch 0, the one played (RYTM_FM_SINGLE_PATCH,
     teensy/setup_plaits_lib.sh; the sim keeps 32). The FM list's card
     voices are read into RAM with the list (16 KB), never at a trig
     [inferred: a card voice read ~1-2 ms like a sample's head; no /DX7 on
     this card, so a2 was not run].
   - DSP SAMPLE: card reads moved out of the render loop: the streams are
     topped up while the sketch waits for requests, the stream with the
     least audio ahead first (in the render loop only for one that would run
     dry within the request); reads end on sectors; the render runs per
     sample format and keeps the position as frame + fraction (a float lost
     the fraction past 2^24 frames, ~6 min). A sample change is prepared
     between requests (a waiting trig starts when its head is in) from a
     per-slot file kept open: 1.2-4 ms -> 0.6 ms a change; 8 changes on one
     step start the last trig 8.5 ms late (the cpu line's "max trig wait").
   - DaisySP's String/StringVoice as block-rate copies (its parameter math -
     two powf, atanf, tanf - once per block; bit-identical on the Mac): a
     STRING voice 9.7 % -> 3.4 %. The modal resonator's modes four at a time.
   - Browsing while playing: the card index is sorted by name when built
     (version 3; no runtime sort), /JUNO folders' patch files are kept from
     the boot walk (listing 128 patches 10.6 ms -> 0.4 ms), the project's name
     is written from loop(); a project change still costs 3.8 ms (the pool's
     paths looked up), inside the buffer. Reports over 2 ms are logged.

   Limits (measured, 2026-10-09): every 8-voice combination is real time
   with the 10.7 ms buffer rytm-emu keeps - the costliest voice is PLAITS
   MODAL (10.8 %; 8 of them 86 %, peak block 87 %, no dropouts), 6-OP FM
   4.8 %, DSP SAMPLE 4.9 % (STRING) to 8.4 % (MODAL) - card reads for all
   eight streams ~1 % more (1.5 MB/s, 8 KB reads of <= 0.8 ms) -, DAISYSP MODAL 7.3 %, JUNO 3.5 %, DIGIDRUM 1.5 %.
   Not enforced: no load governor; 8 x PLAITS MODAL leaves 13 % for the
   Overbridge host still to come - if that needs more, cap PLAITS MODAL /
   HARMONIC polyphony there. A card stall longer than the buffer still
   drops out (not seen: refill reads <= 0.8 ms). Latency: a trig waiting
   for a sample change, up to 8.5 ms (8 changes on one step).

   Headroom (compile lines; before = be2b55f):

   | | before | after |
   |---|---|---|
   | RAM1 free for locals (stack) | 17.0 KB | 121.8 KB |
   | RAM2 free for malloc | 3.7 KB | 41.2 KB |
   | ITCM code (its 32 KB blocks) | 192.5 KB (6 blocks, 4 KB spare) | 147.3 KB (5 blocks, 16.6 KB spare) |
   | flash code | 206.8 KB | 174.2 KB |

   How: Plaits' wavetables (50 KB), speech banks (11 KB) and the JUNO
   factory patches in flash (PROGMEM; on the Teensy const data is copied
   into RAM1 otherwise; the models reading them measured unchanged); the
   card, pool and listing code from flash (FLASHMEM); integer-only printf
   (sniprintf) so newlib's float printf and dtoa are not linked; the output
   ring 1024 frames (a larger request rendered and sent in pieces); one
   string per DAISYSP voice; the engine curation below. The render path
   (voices, render_block, the streams' render) stays in ITCM/RAM1; card
   reads and SdFat run from ITCM still (a library).
3d. Engine curation (rytm-mods 0021, 2026-10-09; both sides decode alike):
   ENG = min((v * N) >> 7, N - 1), v the P value's integer part 0..127 -
   PLAITS N 17: VA VCF, PHASE DIST, WAVE TERRAIN, STRING MACH, CHIPTUNE, VA,
   WAVESHAPE, 2-OP FM, GRANULAR, HARMONIC, WAVETABLE, CHORDS, SPEECH, SWARM,
   PARTICLE, STRING, MODAL (Plaits models 0 1 5..16 18 19 20); DIGIDRUM N 11:
   EFM BD SD XT CP RS CB HH CY, TRX RS CB CL; DAISYSP and DSP SAMPLE N 3:
   MODAL STRING RESON (sim/protocol.py eng_zone, PLAITS_MODELS). Compiled
   out of the Teensy: Plaits' NOISE and drum engines (a silent engine keeps
   the numbers; the 6-OP FM engines stay for the 6-OP FM machine), TRX-B2
   (not linked), DaisySP's DRIP; TRX SD/CH/OH/CY share trx_family.c with RS/CB
   /CL and stay. Saved: ITCM code 17.5 KB, RAM1 7.3 KB, flash 17.5 KB.
3e. Bridge and boot (2026-10-09): teensy_bridge.py starts in any order (waits
   for the serial port, the Teensy's boot - one request answered - and
   rytm-emu's MIDI ports and link, retrying each second). The card listing
   hash on exFAT now reads the raw directory entry sets a sector at a time:
   237 ms -> 81 ms (75 folders, 1976 entries), identical on 10 warm reboots
   ('R' over serial; `stress.py boot N`). The once-seen 2.5 s did not come
   back: warm reboots stay at 81 ms, mount 8 ms, 0 hidden entries; it was
   not reproducible without a power cycle [inferred: a cold card's first
   reads, or the card busy after writes]. The boot log now gives the mount
   time and the entries walked, so a slow boot shows where; the bridge
   waits for the boot either way.
4. Later: record to the SD card and stream back (a looper / resampler). The
   Rytm -> host channels carry the voices after their analog chain; SD writes
   through a RAM buffer (~0.5 s per stream) to ride out card stalls, playback
   reads ahead the same way; about 1.5 MB/s for 8 stereo voices. Stock Teensy
   4.1; PSRAM (8 MB) optional for longer RAM buffers.

5. Ideas from Modded Cycles (2026-10-09; https://18nelli18.github.io/Modded-Cycles/
   - a patched Model:Cycles OS, same method as rytm-mods: the user's own OS file
   patched in the browser, checked against a reference build, an emulator that
   matches the hardware's samples). Their notes on the hardware: CPU marked
   MCF54415CMJ250 (MCF5441x, ColdFire V4e + EMAC, 250 MHz, ~385 Dhrystone MIPS),
   "the same CPU as Digitakt, Digitone, AR, A4 and M:S", all DSP in ColdFire
   assembly, no DSP chip; big-endian 32-bit samples in RAM. Their engines run on
   that CPU (MACRO ~2 voices' worth, a load governor fades the quietest voice);
   ours run on the Teensy, so the Rytm's CPU budget is not the limit. Not
   started; after the optimization work and the hardware tests, in this order:
   a. Sample playback modes for DSP SAMPLE (one shot, loop, slice, granular,
      stretch, pluck, wave; reverse when start > end): a MODE knob on the SD
      SAMPLE page. Granular/stretch/slice/wave need random access - the pool's
      samples loaded into the 16 MB PSRAM rather than streamed.
      Groundwork (2026-10-09, docs/psram.md; not run on PSRAM yet - the chips
      go on Monday, its checklist): the seven modes in one shared header
      (sim/sampler/sample_modes.h; sim and Teensy) on int16 mono samples in
      RAM, MOD on the SD SAMPLE page's FLD word (7 zones; MOD 0 = today's
      voice knob for knob), STA/END/LOP per mode (the table in psram.md);
      the sample RAM (sample_ram.h: contiguous blocks, LRU window eviction,
      pins, no compaction) and the Teensy's loader (teensy/voiceloop/
      psram_pool.*: one open or 8 KB read a loop() pass after urgent
      refills; a voice's sample first, the project's slots into free space
      at a project change; 4 MB kept for later). Behind RYTM_PSRAM: no PSRAM
      at boot -> nothing made, DSP SAMPLE streams as before (RAM2 free
      unchanged, RAM1 -96 B, ITCM +10 KB in the same blocks; -DRYTM_PSRAM=0
      the same code as before). sim: obhost.py --psram, 28 tests in
      sim/tests, samplebench.py (estimated 8 voices: ONE 7-15 %, GRAIN with
      the 32-grain budget 19-53 %, STRCH 18-51 %, WAVE <= 1024-frame cycles
      9-26 %, WAVE 2048 56-71 % - its cycles do not fit the D-cache).
      Waits on rytm-mods for MOD's label and value text.
   b. Sidechain ducking: the Rytm reports trigs of chosen tracks (e.g. the kick)
      and the Teensy ducks its own voices (depth, fixed release ~170 ms as
      theirs). Ducking the Rytm's analog voices too would need the firmware to
      modulate a track's level from the audio interrupt (the knob-write path
      found 2026-10-09) - research and review first.
   c. Arpeggiator for the Teensy machines (JUNO, 6-OP FM, PLAITS), clocked by
      the Rytm's MIDI clock; a Rytm track plays one note, so the chord comes
      from knobs (chord, range, mode UP/DOWN/UPDN/RAND, octaves 1-4).
   d. MACRO: Braids' 47 models (pichenettes/eurorack, MIT) as another Teensy
      machine on a free hidden slot - curated to what Plaits lacks.

6. Possible future endeavor (2026-10-09, not planned): a Rytm-only option, the
   digital machines on the Rytm's own ColdFire without a Teensy. Effectively a
   rewrite: no FPU (fixed-point engines: FM4-style, a fixed-point DX7 such as
   msfa, Braids instead of Plaits, small resonators), ~45 % of the CPU free
   (~40-60k instructions a 32-sample block), and code space beyond the caves
   (an engine area in sample RAM, as Modded Cycles reserves 1 MB). Estimated a
   few light voices (4-8) or 2-3 heavy ones. The Teensy stays the main path: SD
   storage, JUNO/DX7 presets by name, sample storage and streaming, stereo
   recording, a master looper, and room to grow.

7. Mixer page (2026-10-10, idea, not started): a mixer view on the Rytm for the
   voices - level (and pan/mute where it makes sense) per track, including the
   Teensy machines' output level, maybe the Overbridge return and the master
   looper; to be specified (a 0021/0022-style page on the Rytm, values to the
   Teensy as SysEx).
8. Noise Plethora (2026-10-10, idea, not started): Befaco's Noise Plethora
   (https://github.com/Befaco/Noise_plethora) - a Teensy 4.x eurorack noise
   module, ~40 algorithm plugins in banks on the Teensy Audio Library (X/Y
   controls). A natural fit for a Teensy machine (curated, as PLAITS). Licence:
   the firmware is CC BY-NC-SA 3.0 (non-commercial), its plugins CC BY-SA 4.0 -
   keep it as a separate, attributed component, never relicensed; fine for
   personal use, check before any distribution. Its plugins use the Teensy Audio
   Library's objects: port them to the voiceloop's block render (memory/CPU per
   plugin to measure).
9. User machines (2026-10-10; sub-project 1 built 2026-10-10: loader, store, slots, guard, Noise Plethora
   packages A/B/C, runs without PSRAM (docs/plugins.md); device-info report and Rytm side (sub-project 2)
   next): Machinedrum X.OS (Justin
   Valer's third-party OS, allowed by Elektron) lets users install custom USR
   machines as .syx over MIDI SysEx (FUNC+GLOBAL > FILE > MACHINE MGR > RECV;
   managed in banks; see github.com/janne808/machinedrum-kit for the MD machine
   development kit). Our version: machines that run on the Teensy, installed and
   managed as files (e.g. a /MACHINES folder or SysEx upload), shown on the Rytm
   by name with their own knob names, mapped to a generic Teensy machine slot -
   a plugin API on the Teensy (render, params, names) so the range of machines is
   not limited by the Rytm's CPU or caves. Needs: a stable plugin ABI (code
   loading on the Teensy: compiled-in registry first, loadable code later?),
   name/knob metadata to the Rytm (as 16/17), versioning.

Stock-first: everything in 1-3 runs on a stock Teensy 4.1 plus the host cable,
so others can build it like M8 headless.
