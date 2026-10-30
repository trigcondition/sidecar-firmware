# SONG RECORD: record a Rytm performance on the Sidecar and replay it (research + design, 2026-10-20)

The Analog Rytm MKII's song mode is a list of pattern rows: it cannot hold knob moves, scenes, PERF, mutes over time
or fills, and it is edited on the 128x64 screen. The user wants it replaced. **SONG RECORD** is a Sidecar mode: arm it on the
Pi, press PLAY on the Rytm, perform; everything that changes is captured in the Pi's RAM against the Rytm's step
clock. On STOP the Sidecar writes a JSON song file per Rytm project. It also captures the **start state** (working kit,
working pattern, settings, mutes, tempo, Sidecar state), so a replay can first put the Rytm back where the take began
and then drive the Rytm and itself through the recorded events, in sync. The Pi touchscreen shows the take as a DAW-like
timeline (lanes per track / parameter, pattern blocks, scene / PERF curves, mute regions), first read-only and later
editable.

Tags as in SEQUENCER.md:
- **[measured]**: run in rytm-emu (worktree rytm-emu-polychord, `dtboot` headless), **stock OS 1.73**
  (`fw/AR173_rled_stock.syx`, sha1 6d19d9de..., identical to `Analog-Rytm_MKII_OS1.73.syx`), base
  `sections/rled_stock.snap` (4800 M), simulated USB host with the firmware's own USB MIDI sender
  (`--usb-host --usb-midi-real`), the Rytm's USB MIDI output captured raw (`DTBOOT_USB_OUT`) and DIN (`--midi-out`).
  Keys and encoders were driven through the panel MCU (`--press`, `--nudge`), so the stock UI code ran. Snapshots and
  dumps stay in the scratchpad (firmware-derived, never committed).
- **[inferred]**: follows from measured facts or code, not run.
- **[public]**: the Rytm MKII manual's MIDI chart (as transcribed in rytm-mods `re/subsystems/midi-cc.md`), the
  public Elektron SysEx framing, or earlier hardware measurements recorded there.

## Answer in short

| Area | Feasible? | Basis |
|---|---|---|
| 1. What the Rytm sends | **Yes, with the right MIDI CONFIG.** With OUTPUT CH = TRK CH and ENCODER DEST = INT+EXT every knob of every track, the FX track and track level arrives as CC (7 bit) or NRPN (14 bit) on that track's / the FX channel; mutes as CC 94, scenes as CC 92, pattern changes as program change at the moment the switch is committed, transport as FA/FC. **Not sent:** tempo (only the F8 rate), FILL, PERF pad pressure (not measurable here), trig / retrig / condition edits, sound loads, pattern edits, kit name / machine reloads. The stock defaults (AUTO CH, ENCODER DEST INT) are useless for recording: knobs send nothing, and with AUTO CH the CC does not say which track. | [measured] |
| 2. What the Rytm accepts | **Yes.** CC and NRPN set the parameter of the track whose channel they arrive on (not only the selected one) and are not echoed; CC 94 mutes at once; program change on the auto channel queues the pattern to the end of the playing one (sequential) or switches at once when stopped; a **kit SysEx dump to the working buffer is applied while playing** ("Received KIT", within 15 ms in the emulator). Dump requests for the working kit / sound / pattern / song / settings / global all answer. | [measured] |
| 3. Timing | **Yes.** Stamp every event with (bar, step, tick) from 2C step ticks / 2D SWITCH (and the PC-before-switch lead measured below), not wall clock. Tempo changes only scale replay speed. | [measured] + design |
| 4. Storage | **Small.** Start snapshot ~22 KB raw SysEx (~30 KB base64); events ~20-25 B each in JSON: a heavy 10-minute take (30 events/s average) is ~0.4 MB, typical 50-100 KB. RAM is a non-issue on the Pi 4B; autosave as an append-only journal every bar. | [measured sizes] + estimate |
| 5. Replay | **Yes for patterns, mutes, parameters, scenes; partly for tempo and PERF; not for FILL, edits, sound loads without a mod.** Start-state restore = 4 SysEx sends (kit X, pattern X, settings X optional, mutes) before PLAY. | [measured] + [inferred] |
| 6. UI | Timeline on the Pi (OLED theme mockups below); arm / replay from the Pi and from a remote SONG page on the Rytm (0026), start always with the Rytm's PLAY. | design |
| 7. Risks | Writing the working kit / pattern overwrites unsaved work: explicit confirmation + automatic backup dump of the current working state before any restore. | design |
| 8. Plan | **MVP ~2.5 weeks** (record CC/NRPN + PC + mutes + scene + transport + tempo report + start snapshot; JSON; replay pattern + mutes + params + scenes; read-only timeline). Then editing (~1.5 w), sections / loops (~1 w), export to MIDI file (~3 d), mod gap-fillers (~1 w). | section 8 |

## 1. What the Rytm sends over USB MIDI [measured]

### 1.1 The MIDI CONFIG the recorder needs

Stock defaults on this card (GLOBAL > MIDI CONFIG > PORT CONFIG, read on screen): OUTPUT TO = MIDI+USB, OUTPUT CH =
**AUTO CH**, PARAM OUTPUT = CC, ENCODER DEST = **INT**, PAD DEST = INT+EXT, PRESSURE DEST = INT+EXT, MUTE DEST =
INT+EXT, RECEIVE NOTES on, RECEIVE CC/NRPN on. SYNC: CLOCK RECEIVE off, CLOCK SEND off, TRANSPORT RECEIVE on,
TRANSPORT SEND off, PROG CH RECEIVE off, PROG CH SEND off. CHANNELS: tracks 1..12 on channels 1..12, FX 13, auto 14.
The settings live in RAM at `0x407c2d10..` (ENCODER DEST `0x407c2d3a` went 00 -> 01 when set; the same bytes open the
GLOBAL X dump, below).

| setting | value for SONG RECORD | why |
|---|---|---|
| ENCODER DEST | **INT+EXT** | INT: knob turns send nothing (measured: 3 runs, 0 messages) |
| OUTPUT CH | **TRK CH** | AUTO CH sends every knob of every track **on the auto channel (14)**: the message does not say which track, and FX knobs (CC 16 = delay time) collide with track knobs (CC 16 = synth 1) |
| PARAM OUTPUT | **NRPN** (CC works) | CC is 7 bit; parameters with finer steps (TUN, FUNC + knob) send repeated equal values in CC mode (`CC 17 56`, `CC 17 56`) but distinct LSBs in NRPN (`6 56 / 38 68`, `6 56 / 38 92`) |
| TRANSPORT SEND, PROG CH SEND | on | PLAY / STOP and pattern changes |
| PROG CH RECEIVE | on (replay) | program change switches patterns |
| CLOCK SEND | optional | the 2C step ticks are the better time base (SEQUENCER.md 1.3) |

The Sidecar can set these itself: the GLOBAL X dump (80 bytes unpacked, below) carries the MIDI config, and a GLOBAL
X send is stock. **But changing the user's global settings is a standing change**: the Sidecar asks once ("SONG
RECORD needs MIDI OUT = TRK CH, ENCODER INT+EXT, NRPN. Change? YES / NO") and offers to restore the old values.

### 1.2 Per action (TRK CH, NRPN and CC runs; channel = the track's unless stated)

| action | CC mode | NRPN mode (MSB/LSB, then CC 6 / 38) |
|---|---|---|
| knob, sound page (SRC / SMPL / FLTR / AMP / LFO) of track t | CC per the chart on channel t (e.g. BD TUN -> `CC17`, LEV `CC16`, AMP ATK `CC78`, REV send `CC83`, SMPL TUN `CC24`, FLTR FREQ `CC74`, LFO SPD `CC102`); the channel is the selected (edited) track's | `99 1 / 98 n`, MSB 1 = sound parameter, n = the parameter (TUN 1/1, LEV 1/0, AMP ATK 1/24, REV 1/29) |
| knob, TRIG page | `CC3` NOTE, `CC4` VEL ... on channel t | MSB **3** (`3/0` NOTE) |
| knob, FX track (any page) | channel **13**, CC per the FX chart (`CC16` delay time, `CC23`, `CC70` dist, `CC78` comp threshold) | MSB **2** (`2/0` delay time, `2/24` comp) |
| track LEVEL encoder | `CC95` on channel t | `1/100` |
| MUTE mode + pad | `CC94 1` / `CC94 0` on the muted track's channel (CH -> channel 9) | `1/101` value 1 / 0 |
| SCENE mode + pad | `CC92 <scene 1..12>` on, `CC92 0` off, on the selected track's channel (with AUTO CH: channel 14) | `1/104` |
| PERF mode + pad | nothing in the emulator: the pad's value comes from pressure, which rytm-emu does not model | [public] CC 35..47 (no 38) |
| pad (any mode but MUTE) | note on / off (track channel: note 36 here; auto channel: note = track - 1) | same |
| PLAY | `FA`, then **`PC <current pattern>`** on the auto channel | |
| STOP | `FC`; second STOP: `PC <pattern>`, `CC120` on channels 1..13, `FC`, `F2 00 00` | |
| pattern select while stopped (BANK + T) | `PC n` at once, then `F2 01 00` | |
| pattern select while playing | `PC n` **when the switch is committed: 4 MIDI clocks (2/3 of a step) before the new pattern's first step** (0x405c8d06 became 2 at 1875 ms, the pattern word 0x405c8d04 and step 0 at 1955 ms, 127 BPM, 16-step A01) | |
| TEMPO + encoder | **nothing** (only the F8 rate, when CLOCK SEND is on) | |
| FILL (held while playing) | **nothing** | |
| FUNC + knob (fine) | the same CC repeated where the step is below 7-bit resolution | distinct LSB values |
| received CC / NRPN | **not echoed** (no feedback loop on replay) | |
| received PC | **echoed** as `PC n` + `F2 01 00` (PROG CH SEND), and the switch happens | |

Not sent at all [measured where run, else inferred from the chart having no entry]: trig edits (placing / removing
trigs, retrig, trig conditions, micro timing, p-locks recorded in live REC), sound loads / sound pool, kit save /
reload, machine changes from the menu [inferred: CC15 exists in the chart, not tested], pattern copy / paste / clear,
pattern length / speed / swing, song / chain edits, sample changes from the menu other than the SMPL page knob.

### 1.3 What our host link adds (rui1032seq, sim/protocol.py)

The Sidecar is the Rytm's USB host; the same port carries stock MIDI and our 0x7D SysEx. Already there:
2C step ticks (block-stamped), 2D TRANSPORT / PATTERN (now + queued) / SWITCH (from, to, block) / MUTES (mask) / REC /
TEMPO (bpm x 120) / SEQMODE / POS, 0x3B kit, 0x10 project name, 0x04 / 0x03 Sidecar track trig + params. That
already covers pattern changes with the exact switch block, mutes and **tempo** (which stock MIDI does not send).

Gaps a small mod report could fill (all in the UI task, as 2D 08 EDIT was designed):

| gap | report | effort |
|---|---|---|
| FILL on / off | 2D 0A FILL <0/1> (poll the fill flag in the UI tick) | locate flag + 0.5 d |
| scene / PERF values independent of MIDI CONFIG | 2D 0B SCENE <n>, 2D 0C PERF <knob> <value> | locate + 1 d |
| kit reload / sound load / machine change | 2D 08 EDIT op (reload, sound load) + track | 1 d (hooks exist for 0026) |
| pattern edits during the take (trig placed in live REC) | 2D 08 EDIT "pattern dirty" + pattern id: the recorder then re-dumps that pattern at STOP | 0.5 d |
| tempo **in** (replay without clock slave) | host -> Rytm 2E-style "set tempo" that calls the stock tempo setter (tempo_word 0x40287c9c, writer at 0x4009e482 [measured, tempo.md]) | 1 d |
| fill **in** | host -> Rytm "fill on / off" | after the flag is located |

## 2. What the Rytm accepts for replay [measured]

| input (USB MIDI) | result |
|---|---|
| `B0 17 00` (CC 17 = 0, channel 1) | BD TUN to minimum on screen |
| `B0 16 00` | BD LEV to 0 |
| NRPN `99 1, 98 1, 6 0, 38 0` on channel 1 | BD TUN to minimum |
| `B1 17 00` (channel 2, track 1 selected) | **SD's** TUN to minimum (seen after selecting SD): a parameter of any track, not only the selected one |
| `B2 5E 01` (CC 94 = 1, channel 3) | mute mask (`0x407c23c3`, BE word) 0 -> 4 within 10 ms |
| `CD 03` (PC 3, auto channel) while stopped | pattern A04 at once |
| `CD 03` while playing | queued at once (0x405c8d06 = 3 at 610 ms), switched at the end of the playing pattern (1960 ms): **sequential, like BANK + trig** |
| kit X dump (`F0 00 20 3C 07 00 58 ...`, the one just requested, name byte changed, checksum fixed) stopped | "Received KIT", name `QYTMLINJE` on screen within 15 ms of delivery |
| the same while playing | applied, **the sequencer keeps playing** |
| CC 92 (scene) on channel 1 or 14 | no visible change in the emulator: **not verified** |

Notes:
- CC / NRPN go to the **working kit** (what the knobs change): replay sets the sound live and marks the kit edited, as a
  knob turn does [inferred: the parameter setter path, rytm-mods symbols `param_changed`]. They do not touch a stored kit.
- [public, midi-cc.md] loading a kit resets its parameters, undoing CCs sent before it: after a pattern switch that
  changes kit, parameter events must be sent after the switch (the replay engine orders them so).
- [public, midi-cc.md] MIDI Start with CLOCK RECEIVE on waits for clock; MIDI Stop only pauses (a second Stop does not
  fully stop): replay stops with STOP + CC 120 on channels 1..13, as the Rytm's own double STOP sends.
- Temp jump / direct start / direct jump are the pattern PLAY MODE (PLAYMODE key) [public]; a received PC follows the
  current play mode [inferred]. For replay the recorder stores the *observed* switch position, and the engine sends
  the PC early enough for the play mode in force (section 5).

### 2.1 SysEx dumps (stock format; framing public, sizes measured)

Request `F0 00 20 3C 07 00 <id> 01 01 <nr> 00 00 00 05 F7`; reply `F0 00 20 3C 07 00 <id - 0x10> 01 01 <nr> <7-bit
packed payload from byte 10> <checksum 2 x 7 bit = sum(bytes 9 .. end of payload) & 0x3FFF> <length 2 x 7 bit = bytes
5 .. end of payload> F7` (checksum and length verified on the kit dump).

| request id | object | reply size (SysEx bytes) | unpacked | emulator reply time |
|---|---|---|---|---|
| 0x68 | **kit, working buffer** | 2998 | 2610 (name at +4) | < 5 ms |
| 0x69 nr = track | sound, working buffer | 201 | 162 | |
| 0x6A | **pattern, working buffer** | 14988 | 13101 (= the stored stride 13101 in pc_emu.py; pattern BPM x 120 near the end: 0x3B88 = 127.0) | ~10-20 ms |
| 0x6B | song, working | 1506 | 1304 | |
| 0x6C | settings, working | 2748 | 2391 (0x3840 = 120.0 near the start) | |
| 0x6D | global, working | 107 | 80 (the MIDI config bytes of 0x407c2d10..) | |
| 0x62 / 0x64 nr | stored kit / pattern nr | 2998 / 14988 | | |

Real USB full-speed time for a pattern dump is bounded by the host's bulk polling; a Linux host on the Pi should move
15 KB in well under 0.3 s [inferred, not measured]; DIN would take ~4.8 s (31250 baud), so the Sidecar uses USB only.

## 3. Timing: musical, not wall clock

Every recorded event gets a **musical time** `t` in ticks: **96 ticks per step** (SEQ_SUB in protocol.py), steps
counted from the take's start across pattern switches, plus the position inside the playing pattern for display:

    t      = global tick since PLAY (96 per 1x step; pattern speed scales step length, not t's unit)
    bar    = derived for display: 16 steps a bar, independent of pattern length

How `t` is obtained (host side, core/seq/clock.cpp already does this for the sequencer):
1. **2C step ticks** (block-stamped) anchor each step; between them the StepClock interpolates with the measured step
   period. An incoming CC / NRPN / PC / 2D is stamped with the audio block it arrived in and converted to `t` with the
   same follower. USB MIDI and the 0x7D link share one stream, so their order is preserved [inferred: one sender, the
   ring in usb_midi_send].
2. **Pattern changes are stored twice**: `queue` at the time the PC (or 2D PATTERN queued) arrives and `switch` at 2D
   SWITCH's block (stock MIDI alone: switch = PC + 4 clocks, measured). Replay needs the switch point; the queue time
   only matters for display.
3. **Tempo changes** (2D TEMPO) are events like any other: `t` stays musical, so a replay at another tempo stays
   aligned. Wall time is kept per event as `ms` only for diagnostics.
4. Pause / continue: `t` freezes; the gap is not recorded.

Resolution: a knob event lands within one 32-frame block (0.67 ms) + the USB latency, far below one tick at 96 / step
(1.2 ms at 127 BPM 1/16). Replay quantises nothing; it plays events at their tick.

## 4. Storage format

Path: `/.rytm/songs/<project>/<name>.json` on the Pi's card (`<project>` from 0x10, sanitised; `<name>` defaults to
`take-YYYYMMDD-HHMM`). During recording a journal `<name>.jsonl` is appended; at STOP it is folded into the JSON and
deleted. On boot, an orphan `.jsonl` is recovered into a JSON ("recovered take").

```json
{
  "format": "sidecar-song", "version": 1,
  "project": "RYTMLINJE", "created": "2026-10-20T17:40:00Z", "fw": "1.73", "sidecar": "0.2.1",
  "ppq_step": 96, "bar_steps": 16,
  "midi_config": {"out_ch": "trk", "param": "nrpn", "track_ch": [1,2,3,4,5,6,7,8,9,10,11,12], "fx_ch": 13, "auto_ch": 14},
  "start": {
    "pattern": 0, "tempo": 127.0, "mutes": 0, "scene": 0, "play_mode": "seq",
    "rytm_sysex": {"kit_x": "<base64>", "pattern_x": "<base64>", "settings_x": "<base64>"},
    "sidecar": {"machines": [...], "params": {...}, "seq_tracks": [...], "send_fx": {...}}
  },
  "patterns_used": {"0": "<base64 pattern dump at first use>", "4": "..."},
  "sections": [{"t": 0, "name": "INTRO"}, {"t": 12288, "name": "DROP"}],
  "events": [
    [0,     "pc",    {"to": 1, "mode": "queue"}],
    [1536,  "sw",    {"from": 0, "to": 1}],
    [1600,  "p",     {"trk": 0, "nrpn": [1, 1], "v": 7236}],
    [1700,  "cc",    {"ch": 13, "n": 16, "v": 64}],
    [1800,  "mute",  {"mask": 4}],
    [1900,  "scene", {"n": 5}],
    [2000,  "tempo", {"bpm": 128.0}],
    [2100,  "sc",    {"trk": 13, "p": 3, "v": 9000}],
    [3072,  "stop",  {}]
  ]
}
```

Event types: `pc` / `sw` pattern queue and switch; `p` a Rytm parameter by NRPN (MSB/LSB, 14-bit value) or CC (7-bit);
`mute` the whole mask (from 2D MUTES; CC 94 only as a fallback); `scene`, `perf`; `tempo`; `fill`; `edit`
(informational: pattern / kit edited, cannot replay); `sc` a Sidecar parameter / `sn` a Sidecar note (seq tracks are
already pattern data, so only live notes and knob moves are events); `play` / `pause` / `stop`; `mark` a user marker.
Arrays not objects for the event list in v2 if size ever matters.

Sizes:

| part | bytes |
|---|---|
| start SysEx (kit 2998 + pattern 14988 + settings 2748) | 20.7 KB raw, 27.6 KB base64 [measured sizes] |
| each further pattern at first use | 15 KB raw, 20 KB base64 |
| Sidecar state | 2-10 KB (the existing per-project state) |
| one event | 20-35 B JSON |
| 10-minute take, typical (5 events/s) | 3000 events = ~90 KB |
| 10-minute take, heavy (30 events/s, constant knob work) | 18000 events = ~550 KB |
| worst case (an encoder spun flat out, ~60 messages/s, all 10 min) | ~36000 events = ~1.1 MB |

RAM: the in-memory event array is 16 B / event (u32 t, u8 type, u8 target, u16 param, u32 value, u32 aux): 36000 events
= 0.6 MB. Nothing on a Pi 4B. (On the Teensy Mini, 0.6 MB fits PSRAM only; Sidecar Mini would cap a take at ~8000
events in DTCM / OCRAM or stream to SD: **SONG is a Pi feature first**, the Mini later.)

Power loss: the `.jsonl` journal is flushed once a bar (or every 64 events), `fsync` every 4 bars; a crash loses at
most 4 bars. The start snapshot is written as the journal's first line before the first event.

Recording thinning: knob events closer than one tick for the same parameter replace the previous one (keeps sweeps,
drops duplicates); identical consecutive values are dropped (the CC-mode duplicates above).

## 5. Replay engine

The Rytm stays transport master in the **normal** replay: the user presses PLAY on the Rytm (or the Sidecar sends
`FA`, TRANSPORT RECEIVE is on by default), the Sidecar follows its step ticks and plays the events against them.

1. **Arm replay** on the Pi or the Rytm's remote SONG page.
2. **Restore the start state** (only when confirmed, section 7):
   - backup first: request kit X + pattern X + settings X of the current working state into
     `/.rytm/songs/<project>/.backup-<time>.json` (< 0.5 s);
   - send kit X, pattern X (and settings X when the take changed tempo mode / global settings), mutes via CC 94 per
     track (or a mod 2E "mutes" set), scene 0, tempo (clock-slave mode or the mod's tempo set), the Sidecar's own state;
   - wait for the Rytm's "Received" (the 0x3B kit report / a re-request of the kit's checksum to verify).
   The kit dump is applied in < 15 ms in the emulator and the sequencer keeps running [measured]; whether the analog
   voices click when the parameters jump is **not measured** (analog voices are stubbed). Do the restore while stopped
   and expect a click on held sounds; a "soft restore" (only CC / NRPN of the parameters that differ, sent over a
   bar) is the alternative while playing.
3. **Play**: an event scheduler on the host:
   - **pattern switches**: send `PC to` at least one step before the recorded `sw` tick (the Rytm queues it to the end of
     the playing pattern [measured]); for a take recorded with direct jump / temp jump, store and replay the play mode
     (PLAYMODE: settings X or a mod) or emulate with an earlier PC [inferred]; verify by the next 2D SWITCH and, if the
     Rytm switched elsewhere (pattern length changed since the take), re-anchor: events are relative to switches, so
     the rest of the song follows the Rytm.
   - **parameters**: sent at their tick, **stepped** (exactly what was recorded; the Rytm interpolates nothing).
     Optional interpolation between sparse points only for edited / drawn lanes, at most one message per parameter
     per 2 ticks. Sent after a pattern switch that changes kit (the kit load would reset them [public]).
   - **mutes**: CC 94 on the track's channel per changed bit (or one mod message for the mask).
   - **scenes**: CC 92 on the auto / selected channel (receive not verified in the emulator: test on hardware first,
     else a mod message).
   - **PERF**: CC 35..47 [public]; recording them needs pressure from the hardware, so PERF is "recorded if the Rytm sent
     it".
   - **tempo**: either (a) the Sidecar is clock master (CLOCK RECEIVE on, the Sidecar sends F8 at the recorded tempo
     curve, the Rytm follows), or (b) the mod's tempo set. (a) works stock but makes the Sidecar the transport master
     and costs the Rytm's own tempo display accuracy; (b) is the clean way.
   - **Sidecar**: its own params and notes go straight into core (no MIDI).
4. **Look-ahead**: the scheduler runs on the Pi with 1 step look-ahead for PCs and 0 for parameters (sent in the block
   the tick falls in; USB latency ~1 ms).

What cannot be replayed faithfully (honest list):
- **FILL**, trig / retrig / condition / p-lock edits, sound loads, kit reload, pattern copy / paste / clear: stock MIDI
  does not report them and has no input for them. With the mod reports they are shown on the timeline as markers;
  replaying them needs mod inputs (fill: easy; edits: not planned, the pattern dumps at first use carry the result).
- **Patterns edited after the take**: replay plays the current pattern data, unless "restore patterns" was chosen
  (sends the pattern dumps captured at first use, overwriting the working pattern each time it comes up: risky).
- **PERF pad pressure** curves are only as dense as the Rytm sent them.
- **LFO phase / random**: free-running LFOs and random trig conditions differ per run.
- **Analog drift and sample start** of the analog voices: the take is a performance, not audio; for an exact copy,
  record audio (Overbridge) alongside.
- **Kit restore clicks** while playing: not measured.

## 6. UI

### 6.1 The Pi timeline (OLED theme)

Mockups (scratchpad, rendered with the OLED theme's dot font and palette at 266 x 160 dots, 3 px a dot):
- `song_timeline_overview.png`: 64 bars; ruler with bar numbers and section flags (FUNC teal); PTN lane as pattern
  blocks (a temp jump dithered with a dotted outline); SCN scene blocks (teal); PRF curve (panel blue print); BPM line;
  MUTE: 12 rows, muted regions lit; parameter lanes (Rytm in blue print, Sidecar in teal); white playhead; bottom bar
  ARM / REPLAY / LOOP / FOLLOW / EDIT / zoom and BAR position.
- `song_timeline_edit.png`: 16 bars zoomed, event dots per 1/4 bar, a dotted teal selection across two lanes, a loop
  bracket on the ruler.
- `song_replay_confirm.png`: the restore confirmation (section 7).

Interaction: drag = scroll, pinch / +- = zoom (1 bar .. whole take), tap a lane name = expand / collapse, long-press
a block = context (rename section, set loop, delete), FOLLOW keeps the playhead at 40 %. Lanes appear only for
parameters that moved in the take (a 10-minute take typically touches 10-30 parameters); grouped per track, collapsed
by default to one "activity" row per track.

LVGL cost: one canvas per lane, redrawn on scroll only; the playhead is one object. Fits the existing sidecar_ui
dot-canvas approach (sidecar_dot.c).

### 6.2 On the Rytm

- **Start / stop is always the Rytm's PLAY / STOP** (stock transport, reported by 2D TRANSPORT).
- A remote **SONG page** in 0026 (opened from the Sidecar page list; the stock SONG key keeps its stock function
  [inferred: 0026 does not take it]): the screen shows `SONG  REC ARMED / REPLAY / OFF`, take name, bar position;
  trig keys 1..4 = ARM / REPLAY / MARK / LOOP, YES / NO confirm. Markers dropped with MARK become sections.
- While armed the Rytm's REC LED is not used (it stays the sequencer's live record); the Sidecar's status LED / screen
  shows armed.

## 7. Comparison and risks

| | Rytm song mode | Overbridge + DAW automation | Ableton + MIDI | **SONG RECORD** |
|---|---|---|---|---|
| pattern order | yes (rows, repeats, mutes per row, BPM per row) | via PC lanes | via PC clips | yes, by performance |
| knob moves over time | no | yes (Overbridge plugin params, 14-bit) | yes (CC / NRPN) | yes |
| scene / PERF / mutes over time | mutes per row only | yes | yes | yes |
| needs a computer | no | yes | yes | **no** (Pi) |
| restores the kit at start | per pattern's kit | the plugin's state | no | **yes (SysEx)** |
| recording style | programmed | record automation | record MIDI | **play it live, keep it** |
| editing | on the Rytm's screen | DAW | DAW | Pi touchscreen (phase 2), MIDI file export to a DAW (phase 3) |

Risks:
- **Overwriting unsaved work**: the restore writes the working kit / pattern / settings. Always (1) ask with a modal that
  lists what is overwritten and whether the Rytm has unsaved edits (0x3B / a kit X compare against the stored kit),
  (2) back up the working state first, (3) never write stored slots (only working buffers, ids 0x58..0x5D), never save
  on the Rytm on the user's behalf.
- **Changing MIDI CONFIG** is a global setting: ask, remember the old values, offer to put them back.
- **Feedback**: replayed CCs are not echoed [measured]; PCs are echoed (PROG CH SEND): the recorder is off during replay,
  or tags echoes by matching what it sent.
- **Kit changes inside a take** (patterns with different kits): replay parameters after the switch; the timeline
  shows kit changes on the PTN lane.
- **Clock**: the step ticks need rui firmware (mods). On a stock Rytm (no mods) the recorder falls back to MIDI clock
  (CLOCK SEND on) and PC + 4 clocks for switches: works, coarser (tempo.md: bursty clock).
- **Untested on hardware**: scene receive, PERF pressure, audio on a kit restore while playing, USB dump speed on the Pi.

## 8. MVP and phased plan

| phase | scope | Rytm side | Sidecar side | effort |
|---|---|---|---|---|
| **MVP** | record pattern queue / switch, mutes, tempo (report), scene / PERF / all CC + NRPN params, transport; start snapshot (kit X, pattern X, settings X via dump requests + Sidecar state); journal + JSON on STOP; replay pattern + mutes + params + scenes from a confirmed restore; read-only timeline (overview, zoom, follow) | stock MIDI + existing 2C / 2D (rui1032seq); no new mod code | core/song: recorder (event stamping from the StepClock), SysEx dump client (request / reassemble / checksum), journal writer, JSON writer / reader, replay scheduler, MIDI CONFIG check dialog; pi/ui: timeline page (dot canvas lanes) | recorder + dumps 3 d, files + journal 2 d, replay 3 d, timeline 3 d, tests in sim against rytm-emu (dtboot `--usb-midi` inputs, 2C / 2D) 2 d: **~2.5 weeks** |
| 2 | editing: move / delete / trim events, nudge pattern blocks, draw parameter lines, undo; sections / markers; loop a section on replay | remote SONG page (0026): ARM / REPLAY / MARK / LOOP | timeline edit tools, undo stack | ~1.5 weeks |
| 3 | mod gap-fillers: FILL report + input, tempo set, scene / PERF reports, EDIT reports, mutes mask set | 2D 0A..0C, host -> Rytm tempo / fill / mutes | events + replay of them | ~1 week |
| 4 | export: Standard MIDI File (one track per Rytm channel: PCs, CCs / NRPNs, mutes as CC 94, tempo map) for Ableton / any DAW; import edits back | none | SMF writer (+ reader) | ~3-4 days |
| 5 | Sidecar Mini (Teensy) variant: capped events, SD streaming | none | port of recorder / replay; no timeline (remote page only) | ~1 week |

Verification for the MVP: a sim test that drives rytm-emu headless with a scripted performance (`--nudge`, `--press`
BANK + T, MUTE + pads), records the USB output as the recorder would, then replays the JSON as `--usb-midi` events
into a fresh run from the same base and compares end state (pattern, mute mask, kit dump) and switch blocks.

## Measurements (rytm-emu, 2026-10-20) [measured]

Scripts and raw outputs: the session scratchpad `song/` (emu.py driver, exp1..exp6 batteries, `runs/*.log`); not
committed (snapshots and dumps are firmware-derived).

- MIDI CONFIG walk: GLOBAL > MIDI CONFIG > PORT CONFIG (DOWN x7, RIGHT = ENCODER DEST INT+EXT; DOWN x5, RIGHT = OUTPUT
  CH TRK CH; PARAM OUTPUT LEFT = NRPN), SYNC (CLOCK SEND, TRANSPORT SEND, PROG CH RECEIVE / SEND toggled with YES).
- Knob CCs need a full value step: `--turn` of 1 count changes the display without a CC; `--nudge` (4 counts, as the
  window's arrows) sends one CC / NRPN per step after a first "wake" nudge.
- Program change lead: 0x405c8d06 (queued pattern) and the PC appear in the same 1 M-instruction status tick, 4 F8
  periods before 0x405c8d04 (pattern) changes and the step byte 0x405c8cd0 returns to 0.
- Received PC while playing: queued at once, switched at the end of the playing pattern.
- Kit X round trip: request 0x68 -> 2998-byte 0x58 reply; checksum = sum(bytes 9..payload end) & 0x3FFF; one name
  byte changed and the dump sent back: applied stopped and playing.

## Built: MVP (branch song-mvp, 2026-10-20)

### How to use it

1. **Set the Rytm's MIDI CONFIG once** (the Sidecar reads it, never changes it): GLOBAL > MIDI CONFIG > PORT CONFIG:
   ENCODER DEST = INT+EXT, OUTPUT CH = TRK CH, PARAM OUTPUT = NRPN; GLOBAL > MIDI CONFIG > SYNC: PROG CH RECEIVE and
   PROG CH SEND on (TRANSPORT SEND on). The SONG page shows what is missing ("! ENCODER DEST INT, OUTPUT CH AUTO");
   **CFG** on the touchscreen / **T4** on the Rytm's SONG page show these steps.
2. **Arm**: touchscreen SONG page (nav column SONG; CLASSIC: the Song key) **ARM**, or the Rytm's remote SONG page
   (the last of the Sidecar pages: LEFT from SIDECAR; listed once a take exists on the card, the touchscreen armed or
   `song=show` asked) **T1**, or the console `song=arm`. Arming reads the Rytm's start state (working kit,
   pattern, settings, GLOBAL: the stock dumps) - the page says "armed: PLAY records".
3. **Perform**: press **PLAY on the Rytm**. Recorded: knobs of every track and the FX track (NRPN / CC), mutes, scenes,
   PERF (as far as the Rytm sends it), pattern changes (queued + the switch), tempo (with the mods' reports), FILL,
   transport, the Sidecar's send FX values, the tracks' machine changes (informational). **T3** / `song=mark` drops a
   marker. Knob moves while armed but stopped become the take's start values.
4. **STOP on the Rytm** ends the take: `/.rytm/songs/<project>/take-NNN.json` (numbered: the Pi has no clock). While
   recording, `take-NNN.jsonl` is the journal (written every bar or 64 events, closed / reopened every 4 bars); a journal
   left by a power loss is folded into `take-NNN.json` ("RECOVERED") the next time the Rytm names that project.
5. **Look**: the touchscreen's SONG page is the timeline (read-only): ruler with bars and markers, PTN blocks, SCN,
   PRF, BPM, the 12 mutes, then the parameter lanes that moved most (Rytm blue, Sidecar teal), the playhead. Drag
   scrolls, **- / +** zoom (whole take, 64 .. 1 bars), **FOLLOW** keeps the playhead in view, **< >** load the
   project's other takes (`song=load:NAME`, `song=list`).
6. **Replay**: stop the Rytm, **REPLAY** (touch) / **T2** (Rytm) / `song=replay`. Nothing is written until you
   **CONFIRM** (touch) / **YES** (Rytm) / `song=confirm`: the Sidecar then backs up the current working kit / pattern /
   settings into `.backup-NNN.json`, selects the take's start pattern (PC), writes the take's pattern X and kit X into the
   Rytm's **working** buffers (never a stored slot, never a save; settings X too only with `song=settings:on`), sets the
   mutes and its own send FX, and re-reads kit and pattern to verify ("restored (kit and pattern verified)"). Then
   **PLAY on the Rytm** replays: PCs ahead of each recorded switch (the recorded queue time, else two steps before),
   knobs / mutes / scenes / PERF at their ticks, the Sidecar's own straight into its engine; each Rytm switch re-anchors
   the rest. STOP ends it, and so does the take's end (fix1041): when the replay reaches the recorded STOP (a take
   without one: its last tick), the Sidecar sends a MIDI STOP (`FC`) on the replay's port, as the take was stopped,
   and the replay ends at once ("replay done at the take's end: STOP sent"; the dump hold released, the SONG page and
   the timeline idle). It needs TRANSPORT RECEIVE on (the default; the Rytm's GLOBAL X offset 35, [inferred] between
   CLOCK SEND 34 and TRANSPORT SEND 36 by the SYNC menu's order); with it off nothing is sent and the replay still ends
   ("stop the Rytm (TRANSP RCV off)"). There is no host-link way to stop the Rytm (no 0026 transport input). A STOP
   on the Rytm during the replay ends it as before. **CANCEL** / **NO** / **OFF** / `song=cancel` back out at any
   point before PLAY.
7. **Undo the replay** (0.3.0, UX review T14): **UNDO REPLAY** (touch: one wide key in **< >**'s place) / **T5**
   (Rytm: "T5: UNDO REPLAY" in the page's MIDI CONFIG row, T5 lit) / `song=undo` puts back everything the replay
   overwrote, from a **backup set** taken at confirm, while the Rytm is stopped, before anything is written:
   - [0] the pattern selected (P0): its pattern X, kit X, settings X, the mute mask (2D 05) and the Sidecar's send FX;
   - then every other pattern the take plays - its start pattern P1 first, then each its switches go to: a PC to it
     (stopped: at once) and its pattern X and kit X read ("restoring: backup of N patterns first"; ~0.3 s a pattern).
     On the Rytm a pattern keeps its working state over a switch, and the replay's knobs edit the kits of the patterns
     it plays, so each is backed up before the first byte goes out. A backup that fails sends the PC back to P0
     ("nothing written").
   - When the take's pattern X, once written into P1, selects another kit than P1's (the kit number in it), that kit
     is read before kit X goes over it, and its file is on the card first (kit2). When that file cannot be written
   the restore stops there (kit X not written: that kit untouched), kit2 is dropped and the undo kept.
   - At most 8 patterns. A take that plays more: P0 and its start only ("UNDO will cover the start only"), and the
     undo ends when the replay leaves its start.

   During the replay nothing is held or read: its timing is a replay's without UNDO. A switch the Rytm reports to a
   pattern not in the set (a chain on the Rytm, a pattern the user selects) ends the undo at once ("UNDO ended: the
   replay reached A08, not backed up").

   The undo puts the entries back newest first - PC, [pattern X], kit X, each verified - and P0 last with its settings
   X (when the replay wrote them), the mutes and the send FX, so the Rytm ends on P0 as it was ("replay undone (N
   patterns: kits and patterns verified)"). One press, no question. It reads the set from the RAM store only (the
   control core never touches the card for it; a file an idle pass) and checks every entry whole (every dump whole, its id right) before a
   byte goes out: a damaged or missing copy is refused with "nothing written". A PLAY during a restore or an undo
   stops it (never written into a playing pattern; the undo is kept). A restore can be cancelled until it writes;
   its backup files have 10 s to reach the card.

   **When it is offered:** from the replay's first write, while the Rytm is stopped (REPLAY READY too: the undo turns
   the replay off). It ends at the next edit - a CC / NRPN from the Rytm (a knob or a mute, also while the replay
   plays), a pattern change it reports (not one the Sidecar's own PC caused), the Sidecar's send FX, ARM, another
   replay once that one writes (a replay whose backup fails keeps the earlier undo) - after 30 s stopped (REPLAY READY
   counts; the Rytm playing holds the wait), or at a project change. Done once. A T5 press within a second after it
   ended says "UNDO EXPIRED" (T5 stays the page's that long, never the Rytm's trig). Not offered when the pattern
   before the replay was not known (no 2D 02 seen) and the replay changed it; refused while a project loads (its store
   filling: the sequencer's kLockLoading rule); not offered after a restart (the Pi cannot know what changed on the
   Rytm meanwhile). Trig edits on the Rytm are not seen over MIDI: the 30 s cover them.

   **On the card:** `.backup-S.json` (P0) and `.backup-S-K.json` (the others), S one of 8 slots in turn (the oldest set
   written over; never the slot of the set the undo reads). `.backups.txt` ("next N", "counts C1 .. C8") is advanced and posted before the set's files, so it
   never names a slot that holds the newest backup; it is read from the RAM store (no card call on the control core),
   and a reused slot's extra `-K` files are removed through fmt's queue (`fmt::post_remove`). Each file is formatted a
   piece an idle pass and written through fmt's posted queue (the card core's atomic write; each write's own result,
   `fmt::post_result`). Nothing of the replay is written to the Rytm before the set's files are on the card. Kept out of the touch editor's
   UNDO / REDO and NOTE EDIT's FUNC + YES: that history is the Sidecar's own sequencer notes; this writes the Rytm's kits.

Console: `song` (state), `song=arm|disarm|replay|confirm|cancel|mark|undo|check|list|next|prev|load:NAME|settings:on|off|show`,
`song=log:on` (each replayed message and the Rytm's switches with their ticks), `song=state` (the working kit /
pattern fingerprinted in the log).

### What is where

| part | files |
|---|---|
| events, MIDI CONFIG (GLOBAL X offsets measured: 32 PARAM OUTPUT, 34/36/37/38 CLOCK SEND / TRANSPORT SEND / PROG CH RECEIVE / SEND, 44 ENCODER DEST, 46 OUTPUT CH), dumps, base64, clock, recorder | `core/song/model.cpp` |
| the JSON file (written in pieces, read incrementally), the journal, recovery | `core/song/json.cpp` |
| replay scheduler, the confirmed restore (backup first), UNDO REPLAY's restore (`Restore::begin_undo`), the timeline's lanes | `core/song/replay.cpp` |
| engine glue: the MIDI tap, states, card jobs, console, lanes published for the display | `core/song/song.cpp`, `song.h` |
| the Rytm's remote SONG page | `core/ui/page_song.cpp` |
| the touchscreen timeline (OLED + CLASSIC) | `pi/ui/sidecar_song.c/.h`, `pi/engine/song_pi.cpp/.h` |
| tests | `core/tests/test_song.cpp`, `sim/song_e2e.py`, `pi/ui/host/shot.c` (song-* shots), `pi/engine/tests/test_usbmidi_pack.cpp` |

Shared files touched (small hooks): `core/seq/seq.*` (`set_clock_tracks`: BD ticks in the 2E clock mask while armed /
replaying), `core/engine.cpp` (`song::idle`), `core/host/core_c.cpp` and `pi/engine/bridge.cpp` (the byte tap, tick,
console), `core/ui/extra.h` + `page_scope.cpp` (the SONG page last, after INSERT), `core/Makefile`, `pi/engine/Makefile`,
`pi/engine/kernel.cpp` (the page's view), `pi/engine/usbmidi_pack.h` (2-byte program changes and SysEx pieces: dumps
longer than one 4096-byte record go out in 3069-byte pieces), `pi/ui/sidecar_ui.c` (page 10, the nav column's SONG, the
CLASSIC Song key), `pi/ui/host/Makefile` + `shot.c`.

### Verified

- UNDO REPLAY (0.3.0): test_song `test_restore` (every pattern the take plays backed up at confirm, in order, the PC
  back when the backup fails; the linked kit read before kit X and written first; more than 8 patterns: the start
  only; the backup's 10 s; cancel; a PLAY stops the writes), `test_undorun` (newest entry first, a linked kit first,
  P0 last with settings / mutes / send FX, each verified; a damaged entry or an unknown P0: nothing sent; PLAY) and
  `test_undo` against a fake Rytm whose patterns keep their own state (and, for one test, select kits by the number
  in their pattern X): P0 and the take's patterns all back, the mutes, the send FX, once; from RAM with the card's
  files gone; settings X; three patterns; a replay's message timing unchanged (knob, mute, PC on their recorded
  steps, no request during it) and a switch to an unlisted pattern ending the undo at once; a linked kit; a take of
  more than 8 patterns; 30 s (REPLAY READY too, held while playing); T5's "UNDO EXPIRED"; a knob, a mute, a knob while
  playing, a pattern the user selects, the send FX, ARM, another replay, a project switch; the slots in turn after a
  project load, a reused slot's extra file removed, `.backups.txt` on the card before its set's files; refused while
  loading / not in RAM / damaged; a Rytm not answering; PLAY during the undo; a power cut; a backup that cannot be
  written (the earlier undo kept), held 10 s, cancelled; the longest control-core pass of the backups and undos (thread
  CPU clock, < 0.5 ms on the Mac). test_fmt: `post_result` per write, `post_remove`. pi/ui/host shot.c `song-undo`.
- fix1041: test_song `test_replay` (the take's end: nothing before the recorded STOP's tick, one `FC` at it, once;
  without a STOP the take's last tick; TRANSPORT RECEIVE off: ended, no `FC`) and `test_engine` (the replay of the
  recorded take: `FC` in the STOP's step 47, the state off before the Rytm's own STOP report);
  pi/engine/tests `test_usbmidi_pack` (`FC` as one CIN 0xF packet).
- `make -C core test` (test_song: JSON round trip, incremental read, journal recovery with a cut last line, tick math
  with tempo changes / 2x speed / dropped ticks / pause / F8, the recorder, replay scheduling, the restore against a
  fake Rytm incl. nothing written without a backup, the engine end to end on a byte stream, the remote page's PNGs).
- `sim/song_e2e.py` in rytm-emu (rui1036, OS 1.73; the MIDI CONFIG walked on the emulated Rytm's screen): record
  PLAY + BD knob + SD mute + A02 queued + unmute + STOP -> the take; replay into a fresh emulator: restore verified,
  every knob event within 0 ticks of its recorded tick, mutes and the PC on time, the Rytm switching to A02 on the
  recorded bar, the end kit identical to the record run's. Found on the way: the Rytm's dump answers leave the object
  number out of the checksum (an A02 answer was refused), and the emulated Rytm sometimes leaves a dump request unanswered for a
  few seconds (asked again every 1.5 s, 5 tries; 3 / 3 e2e runs pass with it).

### Not replayed / known limits

Tempo (stock MIDI has no input: the replay follows the Rytm's own tempo; settings X restores the start tempo when
`song=settings:on`), FILL, trig / p-lock / pattern edits, sound loads, machine changes from the menu (the restored kit
carries the start machines), LFO phase, analog drift. Scene receive and PERF pressure are untested on hardware. A Pi
keyboard note played while a pattern dump streams out could cut it (the verify then reports it). Takes are numbered,
not dated. Not on hardware yet.

### Traffic to the Rytm (ui-batch, 2026-10-21)

On hardware the Rytm's own UI became slow while a take recorded (its MUTE and pattern menus opened late) and recovered a
while after STOP. `core/tests/test_traffic.cpp` counts what the host sends the Rytm (`to_rytm`), with the Pi's control
loop run every millisecond (remote UI, LFOs, poly, seq, song, card jobs) and a rui1036-like Rytm stream at 120 BPM: BD's
2C ticks, 2D 09 once a second, an NRPN knob turned 25 times a second with its 49 3 echo, a mute every 2 s. [measured, Mac
host build; the Rytm's side is the test's model of it, not the emulator]

| state | before | after |
|---|---|---|
| playing, remote page closed | 0 msg/s | 0 msg/s |
| arming (the snapshot: GLOBAL, kit, pattern, settings X) | 4 requests once | 4 requests once |
| recording, remote page closed (armed from the touchscreen / console) | 0 msg/s | 0 msg/s |
| **recording, the Rytm's SONG page open** | **63 msg/s, 1262 B/s: 28 commits (39), 33 texts (3D), 2 rects (38)** | **5.5 msg/s, 74 B/s: 1 commit, 1.5 texts, 3 keepalives** |
| playing, not armed, SONG page open | 4 msg/s (3C keepalives) | 4 msg/s |
| playing, the SEQ page open (dbg, test case 7) | 58.2 msg/s, 1520 B/s: 8 commits, 42 rects, 8 x 34 | 51.7 msg/s, 1096 B/s: 4 commits, 39.5 rects, 8 x 34 |
| a Rytm that never answers the snapshot | 5 tries 1.5 s apart a request (20 in 30 s) [code] | 4 tries, 1.5 s then 3 s apart (16 in 42 s) [measured] |

**Cause:** the remote SONG page redrew on every recorded event: its first line carries the take's event count, so a knob
turned while recording changed the page 25 times a second, plus the beat (BAR x.y, twice a second at 120 BPM) and the
blinking record dot. Each changed frame is a commit (39), and rytm-mods 0026 marks the Rytm's screen dirty on every
commit (design.md, `ev_tick`: "sets ctrl + 0x20 (dirty) when a new frame was committed"): the Rytm then runs its whole
stock draw and panel diff under our page at every 20 Hz UI tick - the menus the user opened (MUTE, the pattern list)
competed with that. The recorder itself sends nothing while it records (the clock mask's one 2E at arm and at stop
aside); with the page closed the Rytm saw no extra traffic. After STOP the take is saved and the page stopped changing.

**Fix (core/ui/page_song.cpp):** while recording or replaying the page changes at most about once a second: the event
count is refreshed once a second, the position shows the bar without its beat (the replay's position bar moves by whole
bars), the record dot is steady. The snapshot's dump requests (core/song/song.cpp) wait 1.5 s, then 3 s between tries
(4 in all): never more than one request in 1.5 s. Not changed: other remote pages that redraw while playing (the SEQ
page's playhead, SCOPE) - by design live; the touchscreen's VOICE / PAT pages' pattern dumps: next section.

### The touchscreen's PAT / VOICE pages: the pattern dump (patfetch, 2026-10-21)

The second cause of the slow Rytm UI while a take recorded: with the touchscreen's PAT or VOICE page open, the host asked
for the Rytm's whole pattern (the stock 0x64 request, a 14988-byte answer the Rytm builds from its working buffer) on
every pattern change at once and on every 2D 0B DIRTY 300 ms after the last. The mod sends DIRTY for any pattern write
(a live-recorded lock, a trig key), at most every 4 UI ticks (200 ms): a knob gesture with a pause of 300 ms or more cost
a full dump each time. [found by reading the code; the rates below measured in core/tests/test_traffic.cpp's case 6, the
Mac host build against the test's modelled stream: DIRTY every 200 ms for 0.6 s then 0.4 s still, every second;
"runs" = 3 pattern changes 100 ms apart every 4 s; 60 s each]

| pattern dump requests per minute | before | after |
|---|---|---|
| PAT page, playing, edits | 60 | 15 (one every 4 s at most) |
| PAT page, stopped, edits | 60 | 15 (1 s after the last edit, at most 4 s after the first) |
| PAT page, stopped, edits without a pause | 0 (postponed for as long as they last) | 15 |
| PAT page, stopped, pattern runs | 45 (each change) | 15 (one per run, the final pattern) |
| **PAT page, recording a take, edits** | **60** | **0** (one after STOP) |
| PAT page, recording, edits + pattern runs | 60 | 15 (the runs only) |
| **VOICE page, recording, edits (+ runs)** | **60 (60)** | **0 (0)** |
| VOICE page, playing, edits | 60 | 0 (after STOP) |
| VOICE page, stopped, edits | 60 | 15 |
| after the take's STOP | 0 | 1 |

**Fix (core/seq/rytm_dump.cpp):** a fetch at once only when a page opens (or `rytmpat=fetch`). A DIRTY while playing:
at most one fetch every 4 s (kPlayGapMs); stopped: 1 s after the last DIRTY (kQuietMs, trailing), at most 4 s after the
first (kStaleMs). A pattern change: 250 ms after the last of a run (kPatternMs). The VOICE page (it needs only its
track's p-locks) fetches on open and on a pattern change, never on DIRTY while the Rytm plays. While a take is armed or
recording (song.cpp: rytm_dump::set_hold(1)) no DIRTY fetch, and a pattern change only on the PAT page; while the song's
own dumps run (arming, restoring, replaying: hold 2) nothing; what waited is fetched once after (the take's STOP). One
request at a time, a lost answer asked again once (1.5 s), then nothing until the next trigger. The PAT page says why
it is behind: "edited: updates when edits settle" or "edited: updates after STOP" (in the warning colour).

### SRC / TBL / INST at 14 bits (rui1038, 2026-10-21)

GRANULAR SRC, WAVETABLE TBL and MULTISAMPLE INST carry a bank (docs/SRC-BANKS.md). The recorder takes them from the
Sidecar's own reports, never from the Rytm's CC / NRPN: a new event `src` (`[t,"src",{"trk":T,"v":V}]`, V = index << 7 |
bank << 4) whenever `ui::remote_track_src14(t)` changes (the SRC view's 2A, the host's own 48 writes: the bank list's
commit, a touchscreen pick), polled every 10 ms with the machines. On a track whose machine is one of the three, the
Rytm's NRPN 1:0 and CC 16 (SRC A) are not recorded. Replay sends `src` as 48 <track> 0 <index> <bank << 4> (as a knob
turn: the kit keeps it). [measured, rytm-emu rui1037: in NRPN mode the Rytm's NRPN does carry the fraction in its LSB,
so the bank; in CC mode CC 16 is the index only - the recorder ignores both for these machines] Tests: test_song
`test_src`.
