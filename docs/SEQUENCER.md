# A Sidecar-side sequencer for Sidecar tracks (research + design, 2026-10-20)

Question: can the Sidecar run its own step sequencer for its poly tracks (FM, MULTISAMPLE, ...)? It would store the
patterns on its SD, keep time with the Analog Rytm MKII's transport and step clock, and use the Rytm's TRIG keys, LEDs,
PAGE key and encoders as the editor. More than 4 pages would be possible. The Rytm's own hooks (pattern change,
copy/paste/clear, SCALE, record, mutes) would keep the Sidecar's data consistent. This is an alternative or a
complement to chords stored as p-locks in the Rytm pattern (POLYPHONY.md, built: 0x28, CHD/INV/SPR/VOI).

Tags, as in POLYPHONY.md:

- **[measured]**: run in rytm-emu: OS 1.73 + rytm-mods `rui1031`, base `sections/us_rui1031.snap` (clock 7400 M, tempo
  127.0, pattern A01, kit 1), card `us_rui1031.img`, `--usb-host --usb-midi-real`, `DTEMU_STRICT=log`. The runs are
  listed in "Measurements". 200 M instructions = 1 emulated second.
- **[code]**: read in the 1.73 disassembly (rytm-mods `build/mainos_emac.dis`), not executed.
- **[inferred]**: follows from measured or code facts, not checked directly.
- **[public]**: public manuals and docs (Elektron, MCL), recalled, not checked against a device.

No hardware was used. Nothing was flashed. No firmware bytes are in this document, only addresses and behaviour.

## Answer in short

| area | verdict | basis |
|---|---|---|
| 1. Clock and position | **Feasible and precise.** Each track's current step, speed and length sit in a few bytes that the audio interrupt updates. The pattern index and the pattern-switch moment are known. A per-block poll (or one detour) can report each step tick, timed to the audio block. | [measured] |
| 2. Hooks (copy/paste/clear, SCALE, mutes) | **Feasible.** Every operation was triggered in the emulator and its function identified: pattern, track, page and trig copy/paste/clear, track length and speed, mutes. They act on the *working* pattern and the *current* track. A report needs only "op + current pattern + current track". | [measured] |
| 3. Step UI on the Rytm's keys | **Feasible with 0026's machinery.** Caught TRIG keys place no stock trig, the 16 trig LEDs can be overridden with play-position rewriting, and PAGE and the encoders can be caught. Missing: a "seq mode" that catches them *without* the remote page open, and the 4 page LEDs (40..43). | [measured] 0026 + [inferred] |
| 4. Recording | **Feasible on the host.** The pads already send every note-on and note-off. The host quantises to its own grid using the step ticks. A detour at `live_rec_trig` (already used by 0026) keeps stock from writing on a Sidecar-sequenced track. | [measured] POLYPHONY.md + [code] |
| 5. Sync | **Rytm is master.** Step ticks with block timestamps give a phase-locked host clock. The host can render *ahead* by its known output latency, which makes these tracks tighter than today's Rytm-triggered Sidecar trigs. Pattern changes, chains and songs all pass one switch point. | [measured] + [inferred] |
| 6. Storage | **One small file per (project, pattern)**, `/.rytm/seq/<project>/pNNN.sqp`, like `/.rytm/lfo/<project>/kNNN.txt`. It stays consistent through stock copy/paste/clear only if those are hooked. SysEx/Transfer dumps and stock-OS edits are invisible to it. | [inferred] |
| 7. Versus p-locked chords | Complementary, not a replacement. P-locks: portable, stock storage, limited voicings, 64 steps. Sidecar seq: free polyphony, more than 64 steps, the host's own timing, but the data lives off the Rytm. | analysis |
| 8. Recommendation | Build the **MVP for one track type on the Pi**: transport sync, 16 x N pages, trig-hold + knobs, LEDs, live record, per-pattern file, follows pattern changes. Then the hooks, then song/chain. Roughly **3 weeks for the MVP**. | section 8 |

## 1. Clock and position

### 1.1 What the sequencer keeps [measured]

A diff of the sequencer state across snapshots taken one step apart while playing, plus write watches, gives this:

| address | size | meaning | evidence |
|---|---|---|---|
| `0x405c8c90` | long | playing (0 / 1) | 0 before PLAY, 1 after |
| `0x405c8cd0 + t` | 13 B | **current step of track t** (0-based) | +1 per 1x step; wraps at the track's length (BD poked to 12: 0x0b -> 0x00; 64-step tracks run to 0x12+); a 2x track advances 2 per step |
| `0x405c8cea + t` | 13 B | the step before (the one last fired) | always step - 1 |
| `0x405c8cdd + t` | 13 B | 24-PPQN tick within the step (0 .. ticks-1) | compared against the speed table |
| `0x405c8de0 + t` | 13 B | a second per-track step count (wraps at `+0x354` of the track, the change/master length) | [code] 0x4009b810..0x4009b856 |
| `0x405c8d24` | 1 B | pattern speed index; `0x405c8d25 + t`: track t speed index (0..6 = 2x, 3/2x, 1x, 3/4x, 1/2x, 1/4x, 1/8x) | 02 = 1x on every track; the 2x track read 00 |
| `0x405c8c94` | word | master step count (end-of-pattern test against the master length) | [code] 0x4009bae6..0x4009bb10 |
| `0x405c8c95` / `0x405c8c99` | 1 B | global step / previous step | as the per-track ones |
| `0x405c8cfd` | 1 B | tick within the beat (mod 24) | wraps 0x13 -> 0x01 over 6 ticks |
| `0x405c8d04` | 1 B | **active pattern index 0..127** (A01 = 0) | A01 -> A02 -> A05: 0, 1, 4, stopped and playing; SRAM mirror `0x800067bb` |
| `0x405c8d05` / `0x405c8d06` | 1 B | the pattern being switched to / the previous one | set at the switch (0x4009bb2a) |
| `0x405c8b20` | long | the sequencer's musical clock: **900 000 units per 24-PPQN tick** | +5 400 000 per 1x step (6 ticks) |
| live track (917 B, `pattern_track`) `+0x2c5` | 1 B | **track length** (steps) | 16 / 64 / 18 in the base; poking 12 made BD wrap at 12 |
| live track `+0x2c7` | 1 B | track speed index | poked 0 -> the track ran at 2x |
| `0x407c23c3` | word (BE, odd address) | **mute mask**, bit t = track t | MUTE + SD -> 0x0002, + CH -> 0x0102; SRAM mirror `0x8000ed9a` |
| `0x419ca2ac` | long | audio block counter, 1500/s (0021's 60 s window) | 0021 design [measured there] |

The per-track step bytes are copied each tick to a second 13-byte set at `0x42ac13ad` (with the ticks and previous
steps beside it at 0x42ac13a0..0x42ac13e8) [code: 0x4009b8be..0x4009b8e2]. That looks like a snapshot for other
readers [inferred].

### 1.2 Where it runs and what it costs [measured]

- The sequencer clock `0x4009b048` is called from the **audio interrupt** (break: return into 0x40099xxx, history
  0x4012165c, interrupt level 2 on the idle task's stack). It ran **203/s at 127 BPM = 96 PPQN** (407 calls in 2 s,
  stopped or playing).
- Its per-track step loop (`0x4009b728`, then 13 tracks at `0x4009b752`) runs every 4th call, **51/s = 24 PPQN at
  127 BPM**. It advances `+0x405c8cdd[t]`, and when that wraps at the speed table's ticks (`0x401db448`: 3, 4, 6, 8,
  12, 24, 48) it increments the step at **`0x4009b80c`** (`moveb %d7,%a4@`). It wraps the step at the track length
  (`+0x2c5`), or at the pattern's length when the pattern is in NORMAL scale mode (pattern `+85683`, mode byte
  `+85689`) [code].
- `trig_fire` (0x4009a9bc, `(track, ctx, step, slot)`) is called **once per track per step**, trig or not: 221 calls =
  13 tracks x 17 steps in 2 s of play. So it is the natural per-step hook.
- `0x400996e2(track)` is called when a track wraps at its change/master length: 8 calls, which is the 8 tracks of length
  16/18 that wrapped once [measured].
- **Pattern switch** (sequential): at the end of the master length, `0x40099e70` (the next-pattern provider) is called
  and `0x405c8d05` is written at **`0x4009bb2a`**. It then posts a message to a task (`0x400013b2` on queue
  `0x40287c80`), and every track's step restarts at 0. A01 -> A02 queued at step 7 switched exactly at step 16 [measured:
  counted once, counters 04 four steps after]. Other writers of `0x405c8d04` (0x4009a000, 0x4009b19a, 0x4009b9e8)
  were not hit by a sequential change. They are likely the start, direct-jump and temp-jump paths [inferred].
- Cost of a report hook: polling 13 step bytes and the pattern byte once per audio block is about 40 instructions x
  1500/s, roughly 0.03 % of the CPU [inferred from instruction counts]. A detour at 0x4009b80c costs a few instructions
  per track step. Either way it is negligible next to 0021's per-block work.

### 1.3 What the host gets today [measured]

- **Transport over USB MIDI** (the base has TRANSPORT SEND on, CLOCK SEND off):

  | action | sent |
  |---|---|
  | PLAY (stopped) | `FA` |
  | PLAY while playing (pause) | `FC` |
  | PLAY while paused | `FB` (continue; no song position pointer) |
  | STOP | `FC` |
  | STOP again (stopped) | `FC`, CC 120 on channels 1..13, `FC`, `F2 00 00` |

  No `F8` clock at all in this base, and none when CLOCK SEND is off. With CLOCK SEND on, tempo.md measured the clock
  bursty (2.6 vs 18.7 ms intervals) and 0.2 % fast. **MIDI clock is not a good time base** for sample-accurate
  rendering [measured, rytm-mods re/subsystems/tempo.md].
- **Tempo:** `0x40287c9c` in 1/120 BPM [measured, tempo.md]. It is not reported to the host today.
- **Trig reports 0x04/0x11:** one per voice per block, with no track, step or length (POLYPHONY.md 2).
- **Kit 0x3B, machines 0x33, project name 0x10** (`0x407c89d3`, 16 B) [measured, 0023/0026]. **No pattern index, no
  step, no length, no swing.**

### 1.4 Precision against the Overbridge stream

The Overbridge IN packets carry a frame counter (+7 per 7-frame packet) written by `ob_in_write` (0x40005c6e) in the
same audio interrupt as the sequencer [code, symbols]. A report sent from that interrupt in block B can carry the
block counter `0x419ca2ac`. With one "sync" message pairing the block counter and the OB frame counter, the host maps
every step tick to an exact frame of the stream it receives. The tick's sub-block position is then within one block
(32 frames, 0.67 ms) [inferred]. For sub-block accuracy, the report would also carry the tick's phase within the block
(the scheduler's event time, the 900 000-unit clock; 0x8000fd50 holds per-event times [code: trig_fire's caller
0x4009b5f0]). This is not needed for the MVP.

## 2. Hooks: what the Rytm does, where it can be reported

All were triggered by key gestures in the emulator, with `--count` on the candidates, so the function and the gesture
are tied [measured]. Every one of them acts on **the working pattern** (`pattern_active` 0x400af040) and, for track,
page and trig operations, on **the current track** (`current_track_pattern` 0x400af076). A report therefore needs
only `<op> <pattern index 0x405c8d04> <track>`, plus the page or trig mask where it applies.

| gesture (1.73) | stock function (entry) | UI handler / popup | report |
|---|---|---|---|
| FUNC + REC (not GRID REC), or in the BANK menu | `0x400abde0` pattern copy: 85 760 B of the working pattern to a heap clipboard [code] | 0x400cc23e "COPY PATTERN" | `COPY PAT p` |
| FUNC + STOP | `0x400abec0(pat, 2)` (is the last op undoable?) then `0x400abff6(pat, 1)` paste | 0x400cc3aa "PASTE PATTERN" / "UNDO PASTE" | `PASTE PAT p` / `UNDO` |
| FUNC + PLAY | `0x400ac11c(pat, 1)` clear | 0x400cc47e "CLEAR PATTERN" | `CLEAR PAT p` |
| GRID REC, FUNC + REC / STOP / PLAY | track copy `0x400c2efc(tp)`, paste `0x400c4754(tp, 1)`, clear `0x400c494a(tp, 1)`, undo test `0x400c3808(tp, kind)` | 0x4003c626.. "COPY/PASTE/CLEAR TRACK", "UNDO PASTE/CLEAR" | `COPY/PASTE/CLEAR TRK p t` |
| GRID REC, PAGE held + FUNC + REC / STOP | page copy `0x400c4620`, paste `0x400c3342` (with `0x400c35fa`), clear `0x400c444e` [code for clear] | 0x4003d8d8.., "COPY/PASTE/CLEAR PAGE" | `... PAGE p t page` |
| GRID REC, TRIG held + FUNC + REC / STOP | trig copy `0x400c4acc`, paste `0x400c396a` | "COPY %d TRIG%s", "PASTE %d TRIG%s" | `... TRIGS p t mask` |
| CLEAR LOCKS (gesture not run; trig held + FUNC + PLAY [inferred]) | `0x400c1cc0` [code] | "CLEAR LOCKS" | `CLEAR LOCKS p t mask` |
| SCALE (FUNC + PAGE), track LEN with UP/DOWN | `0x400c0b36` length setter (writes `+0x2c5`), then `0x400c2a2c` x 13 (recomputes `+0x354`) | SCALE menu, ADV mode | `SCALE t len` |
| SCALE, track speed | `0x400c0c20` (writes `+0x2c7`); `0x400c0d04` writes `+0x2c6` [code] | | `SCALE t speed` |
| SCALE, master LEN / CHNG / scale | pattern fields `+85683` (master length, word), `+85685`, `+85689` (mode), `+85690` (speed) [code: the step loop reads them]; setters not pinned | | `SCALE PAT ...` |
| MUTE mode + pad | mute mask `0x407c23c3` (BE word) | | `MUTES mask` (poll) |
| SOLO (fix1049) | solo mask `0x407c23c5` (BE word, bit t = track t) [measured, rytm-emu: CC 93 Track Solo on SD's channel -> 0x0002, BD's 0x0001; mirrors `0x8000ed9e`, `0x4131899d`] | | `SOLO mask` (poll, rui1049) |
| RELOAD PATTERN / SAVE PATTERN | 0x400cd866.. "PATTERN RELOADED!", 0x400cd8fc.. "PATTERN SAVED!" (`0x400af5e0`, `0x400ae588`) [code] | | `RELOAD/SAVE PAT p` |
| RELOAD TRACK | `0x400af64a` [code] | "TRACK %d RELOADED" | `RELOAD TRK p t` |
| song / chain | paste song `0x400bc290`, copy chain `0x400bba1c`, song load `0x400b1654` [code]; the playing order is resolved by the next-pattern provider `0x40099e70` at the switch | | covered by the switch report |
| pattern received by SysEx | 0x400a1a7e.. "Received PATTERN %c%02d" [code] | | `RECV PAT p` |
| live record | `live_rec_trig` 0x400b45e0 (0026 detours it), `live_lock_rec` 0x400af6f6, `live_rec_active` 0x40036370 [measured, 0026 1030] | | `REC on/off` (poll) |

**Cheapest way to report: not one detour per function.** Two kinds of hook cover it all:

1. **A poll in the audio interrupt or the 20 Hz UI tick** (0026's `ev_tick` already runs there) for state:
   playing, pattern index, the 13 steps, 13 lengths and speeds, pattern length/mode, mutes, record state, tempo.
   When something changes, send it. No code site is patched [inferred: the reads are plain bytes].
2. **Entry detours on the 12 edit functions above**, each a 6-byte `jmp` to a stub. The stub queues
   `<op, pattern, track, arg>` and runs the displaced instructions. These run in the UI task. The send must use 0026's
   try-lock rule: never wait on USB, queue and retry at the next tick.

What stock does to its own data in each case, and what the Sidecar file must mirror:

- **Pattern copy** copies the whole working pattern (85 760 B live), so a Sidecar clipboard should hold the whole
  Sidecar pattern at COPY time. **Paste** overwrites the working pattern. It is undoable by a second paste (UNDO
  PASTE), so the host keeps the previous content for one undo. **Clear** is undoable the same way.
- **Track / page / trig** operations are the same, per current track. Page = the 16 steps of the visible page.
  Pasting trigs targets the held trigs.
- **Paste on another pattern:** select the pattern (BANK + Tn), then paste. Stock pastes into the *new* working
  pattern. The report's pattern index is the destination.
- **SCALE** changes length and speed only. The steps beyond the new length keep their data in stock (the length is a
  play limit) [inferred from the setter writing one byte]. The Sidecar does the same (seq1112 below: shortening hides,
  it never deletes). Growing is the other half: the Rytm fills the pages a longer length adds with a copy of the
  pattern as it stood (1 -> 2 pages duplicates page 1; 2 -> 4 copies pages 1-2 into 3-4) [public: the Rytm's manual,
  "when the length is increased the pattern is copied"]. The Sidecar does that too.
- **Mutes** are per project, saved in the working state (the mask is inside `0x405c8e10..0x407c89cb`, the power-off
  image) [measured address, inferred scope].

## 3. Step UI on the Rytm

### 3.1 What exists (0026) [measured, rytm-mods 0026 design 1022/1026]

- **Keys:** with the remote page open, the catch mask (0x37) takes any key code, T1..T16 included (0x00..0x0F). A
  caught trig key **places no trig**: stock's trig keys have no path before the key event, unlike the pads. Presses
  and releases go to the host (0x31) with FUNC state. The key's owner is fixed at its press, so there are no stuck keys.
- **Encoders:** A..H and LEVEL can be caught individually. The host gets deltas (0x32) with a FUNC flag. A local echo
  (0x3F) shows the value at once.
- **Trig LEDs:** 0x34 sets T1..T16 to palette colours (0..111). The play position that stock keeps writing is
  rewritten to the host's colour as it passes, so the host owns the 16 LEDs completely while the page is open.
- **Screen:** the host draws the whole OLED (0x38/0x39) or text slots (0x3D).
- RECORD, PLAY and STOP always stay stock's. That is what we want: the Rytm stays transport master.

### 3.2 What is missing

1. **"Seq mode" without the remote page.** Today keys are caught only while the page is open (TRK + YES). A Sidecar
   track sequencer should feel stock: select the track, press GRID REC (RECORD), and the keys edit the Sidecar
   pattern. Proposal: the host marks tracks as Sidecar-sequenced (new host -> Rytm message, section 5.4). The mod
   then catches T1..T16, PAGE and (while a trig is held) the encoders whenever **current track is a seq track** and
   **GRID REC is on, or a trig is held**. Everything else stays stock. `rui_key` already sees every key first
   (core-ar2 `ev_key`), so this is a new rule in the same function [inferred]. The track-selection and GRID-REC tests
   are `track_index_of(project + 48)` and the handler's own test `0x4003625c(view + 108)` [code: the track copy handler 0x4003c626
   gates on it].
2. **The page LEDs.** PAGE's 4 LEDs are panel LEDs 40..43 (rytm-emu `panel::PAGE_LEDS`) [measured in the emulator's
   panel model]. 0x34's rewrite covers LEDs 0..15 only. The same UART rewrite can cover 40..43 [inferred: same
   mechanism, other LED ids].
3. **Trig-hold editing display.** While a trig is held, the screen should show the step (NOTE / VEL / LEN / CHORD /
   micro). Either the remote page opens automatically while a seq trig is held, or 0x3D text slots are drawn over the
   stock screen. Today 0x3D needs the page open.

### 3.3 The Rytm's own track: empty or dummy trigs?

- **Empty track (recommended).** The Sidecar plays its own notes. The Rytm track holds no trigs, so stock GRID REC
  shows nothing for it, and the seq mode LEDs replace that view anyway. Copy, paste and clear of that Rytm track then
  copy "nothing", which is harmless. **Prerequisite:** the Sidecar's audio enters the Rytm through the voice's analog
  chain. With no Rytm trig the analog AMP never opens, unless POLYPHONY.md 5 Mod B (the analog pin: AMP held open on
  Sidecar poly tracks) is in. Until then, the workaround is one stock trig on step 1 with AMP ATK 0 / DEC INF
  (POLYPHONY.md 5.5) [inferred from the 5.x measurements].
- **Dummy trigs (one per step that has Sidecar notes).** Stock would then fire 0x04 at exactly those steps. That is a
  free, sample-tight clock, and the stock UI shows where notes are. But: there are 64 steps at most, every Sidecar edit
  must be mirrored as a stock trig write (more mod code, the write paths `trig_place` 0x400c25b4 / `trig_flags_set`
  0x400c2416 are known), a stock retrig re-opens the analog envelope, and pages > 4 cannot be shown. **Not
  recommended** except as the "pin not available yet" fallback.
- Either way the host **swallows 0x04s on seq tracks** (as poly.cpp already swallows 0x04 behind a bound chord).

### 3.4 More than 4 pages

The Rytm has 4 page LEDs, and stock caps a track at 64 steps (4 x 16). Design for 1..16 pages (16..256 steps):

- **PAGE** (caught in seq mode) steps through the pages. **PAGE held**: the 16 trig LEDs become a page map: pages in
  the pattern dim (43 blue), the edited page bright (47), the playing page white (111). A trig key while PAGE is held
  jumps to that page (the Digitone/Tonverk "page select by trig" idea [public]).
- **The 4 page LEDs** show the position within the current group of 4 pages (as stock does for 1..4). The group
  (1-4, 5-8, 9-12, 13-16) is shown by the remote header `PG 6/12` and, while PAGE is held, by the trig LEDs.
- **Play position:** in seq mode the host draws the playing step itself (white) only when the playing page is the
  edited page. Otherwise it blinks the playing page's LED in the page map. **Follow** (FUNC + PAGE in stock is SCALE,
  so use NO + PAGE, or a page toggle) follows the play head across pages.
- Length per track: the Sidecar track's own length (1..256) and speed are set on the Sidecar's SCALE page (remote),
  not in the Rytm's SCALE menu, which still governs the Rytm's own tracks and the master length (pattern change point).

## 4. Recording

- **Live:** the pads already send every note-on and note-off on the track's (auto) MIDI channel, with the full chord
  [measured, POLYPHONY.md 2]. A host keyboard is the second source. The host quantises each note to its own step grid
  using the step clock (section 5): step = floor((t - t_step0) / step_len + 0.5), micro = the remainder (in 1/24 of a
  step, as the Rytm's microtiming unit [code, symbols: MICROTIMING_MAX]). Velocity comes from the note-on. Length
  comes from the note-off (in steps plus 1/24ths). Overdub: notes are added, a held chord makes a chord step.
- **Record state:** live record = RECORD + PLAY. `live_rec_active` (0x40036370) is the test both stock record paths
  use [code], and 0026 already calls it [measured]. Poll it in the UI tick and report `REC on/off`. **GRID REC**
  (step record) is the view state the copy handlers test (0x4003625c) [code], and needs the same poll.
- **Keep stock from writing:** on a seq track stock would record one note per step into the Rytm track
  (POLYPHONY.md 2). 0026's detour at `live_rec_trig` (0x400b45e0) can return early for seq tracks [inferred: that
  detour already decides per track for 0x28]. Knob locks while recording (`live_lock_rec` 0x400af6f6) would also be
  refused on seq tracks, and the host records them itself if wanted.
- **Step record:** in seq mode with GRID REC: trig key = toggle a step (with the last played note/chord), trig held +
  pad(s)/keyboard = set the step's notes (a chord when several), trig held + encoders = NOTE / VEL / LEN / micro / CHD
  (Digitone style) [design]. Optional "STEP REC: JUMP" style entry (stock has this mode string [code]): pads enter
  notes at a cursor, and a trig key moves the cursor.
- **Mutes:** a muted seq track: the host stops starting notes and releases held notes on mute (mask poll).

## 5. Sync design

### 5.1 Who is master

The **Rytm**. Its transport (PLAY/STOP/pause) stays stock, the pattern-change point stays stock (master length,
sequential / direct / temp jump), and the tempo is the Rytm's. The Sidecar follows.

### 5.2 The clock the host uses

- **Step ticks from the Rytm** (new report, section 5.4): sent in the block where track t's step advances, with the
  block counter. The host keeps, per seq track, its own position: `pos = (pos + 1) mod own_length` at each tick of
  that track. The Sidecar track's length and speed are its own. It can tick off a chosen "clock" Rytm track (default:
  the seq track's own Rytm track, which then runs at 16 steps 1x, or follows the Rytm's SCALE if the user wants).
- **Between ticks:** the host interpolates with the tempo it measured from the ticks (or the reported tempo word), so
  micro timing, swing and note lengths land in frames.
- **Rendering ahead (the latency fix):** today a Rytm-sequenced Sidecar note sounds late by the host's FIFO plus USB
  plus the Rytm's input buffer: about 6.5 ms at a 256-frame FIFO on the Teensy, 10.7 ms with the Mac bridge
  [inferred, HOST-BRINGUP.md]. A Sidecar sequencer knows its next notes in advance, so it can start them **L frames
  early**, where L = the measured round trip. Then they land on the Rytm's analog trigs' frame. This is the strongest
  technical argument for the Sidecar sequencer. L must be calibrated once (play the Rytm's own click into the OB input
  and correlate) [design].

### 5.3 Events

| event | the Rytm does [measured unless noted] | the Sidecar does |
|---|---|---|
| PLAY | `FA`, every track's step restarts at the first tick | position 0 on every seq track; schedule from the first tick |
| pause (PLAY while playing) | `FC`; the counters stay | release held notes (or keep tails), freeze positions |
| continue (PLAY while paused) | `FB`; it continues from the stored counters | continue from frozen positions |
| STOP | `FC`; second STOP adds CC 120 on all channels and `F2 00 00` | release all; second STOP: hard stop (kill tails) |
| tempo change | the 900 000-unit clock moves faster or slower; ticks follow at once [inferred] | re-estimate step length from ticks; notes already scheduled keep their frame |
| pattern change (sequential) | at master length: `0x405c8d05` set at 0x4009bb2a, every step to 0 | load the new pattern's file (prefetched when the change is *queued*, section 6) and start at 0 on the same tick |
| direct start / direct jump / temp jump | other writers of `0x405c8d04` [code, not run] | same, on the report; direct jump keeps the step position (to be measured) |
| chain / song | the next-pattern provider 0x40099e70 picks the pattern [code] | covered by the switch report; no song model on the Sidecar |
| fill | FILL key; trig conditions FILL / !FILL [public] | report FILL state (not located; a poll of the key state `is_key_held` 0x40081bbc works [code]) and evaluate FILL conditions on the host |
| mute | mask `0x407c23c3` | stop new notes, release held ones |
| swing | per-step swing bit + pattern swing (menu 0x400676a6 [code]; value not located) | report the pattern swing amount; apply it on the host's even steps |

### 5.4 New messages (proposal; 0x2C..0x2F are unused in sim/protocol.py and core's dispatch [checked 2026-10-20])

0022's router must pass them, as it was extended for 0x28.

    Rytm -> host  2C 01 <blk3 blk2 blk1 blk0 (7-bit groups)> <n> {<track> <step lo> <step hi>} x n
                  step ticks in this block (tracks marked as seq tracks only, or one "clock" track)
    Rytm -> host  2D 01 <kind> <args...>        state and edit events:
                  01 TRANSPORT <state 0 stop / 1 play / 2 pause>
                  02 PATTERN <now 0..127> <queued 0..127 | 7F>        (queued: polled from the UI side)
                  03 SWITCH <from> <to> <blk...>                      (the moment it switches, audio interrupt)
                  04 SCALE <track | 7F pattern> <len lo> <len hi> <speed> <mode>
                  05 MUTES <mask lo> <mask hi>
                  0D SOLO <mask lo> <mask hi>     (rui1049) the soloed tracks; the host silences every track not soloed
                     (its notes released, the PRE / POST voices' gates closed) as it does a muted one; none soloed: the
                     mutes alone. An older host ignores 0D (an unknown kind); an older Rytm never sends it
                  06 REC <live 0/1> <grid 0/1>
                  07 TEMPO <bpm*120, 3 x 7 bits>
                  08 EDIT <op> <pattern> <track | 7F> <arg lo> <arg hi>   op: copy/paste/clear x pat/trk/page/trigs,
                     undo, reload, save, received
                  09 SYNC <block counter> <OB frame counter>          (once a second)
    host -> Rytm  2E 01 <seq-track mask lo> <hi> <flags>     which tracks the Sidecar sequences (keys/LEDs/rec rules)
    host -> Rytm  2F 01 <page LEDs 4 x colour>               the 4 page LEDs while in seq mode (or 0x34 extended)

Sizes: 2C is at most 1 + 4 + 1 + 13 x 3 bytes, at 24 PPQN x 13 tracks in the worst case. Normally 1-2 tracks.

## 6. Storage

### 6.1 Format (versioned, small, streamable)

One file per (project, pattern): `/.rytm/seq/<project>/pNNN.sqp`, NNN = 000..127 (A01 = 000), as LFO's
`/.rytm/lfo/<project>/kNNN.txt` per kit (core/lfo.cpp). It is small enough to load in one read within the time between
"queued" and "switch" (at least one step, usually a whole pattern).

    header   'SQP1' u8 version, u8 ntracks, u16 flags, u32 crc32 of the rest
    track    u8 rytm_track (0..11), u8 machine hint, u16 length (1..256), u8 speed, u8 swing,
             u8 default note, vel, len; u16 nsteps_used; then the used steps only:
    step     u16 index, u8 flags (trig, mute, accent, slide, cond present), s8 micro (1/24 step),
             u8 cond, u8 prob, u8 nnotes (1..8), nnotes x {u8 note, u8 vel, u16 len (1/24 step), }
             u8 nlocks, nlocks x {u8 param, u8 value}
    end      'END\0'

The format is sparse, so an empty 256-step track costs ~20 B, and a 64-step 4-note-chord track ~2 KB. A text form is
also possible (as the LFO files are), but the binary one loads faster on the Teensy. The Pi can expose JSON in the web
UI and save the binary.

### 6.2 Keys and consistency

- **Project:** the name at `0x407c89d3` (0x10). Names are not unique or stable: SAVE AS / rename creates a new key
  [inferred]. Mitigation: on a 0x10 with an unknown name, offer "link to <old project>" in the web UI (bundles spec 8
  lists the same open question).
- **Pattern:** `0x405c8d04` (new 2D 02). This is the key's weak point: anything that moves Rytm data without a hook
  breaks the link.

| Rytm-side action | Sidecar sees it? | file action |
|---|---|---|
| pattern copy -> paste (any slot) | yes (2D 08, destination = working pattern) | copy file content of the COPY source to the destination; keep the old for UNDO |
| pattern clear | yes | clear the seq tracks of that pattern (keep for UNDO) |
| track/page/trig copy-paste-clear on a seq track | yes | same on the track/page/steps (page = 16 Sidecar steps of the *Rytm's* visible page; in seq mode those keys are the Sidecar's anyway) |
| SAVE PATTERN / RELOAD PATTERN | yes | Sidecar keeps a saved copy per pattern: reload restores it (stock semantics) |
| project SAVE / LOAD / new | name changes (0x10) | switch folder; an unknown name starts empty |
| project copy/rename on the Rytm (project manager) | only the new name later | not followed (link manually) |
| SysEx / Transfer pattern dump in | "Received PATTERN" handler [code], can be hooked | mark the Sidecar pattern stale (ask: keep / clear) |
| stock OS / no Sidecar attached | no | nothing; on reconnect the files are as they were |
| power-off | no message needed | the Sidecar saves on every edit (debounced) |

- **Bundles:** add `seq/<project>/*.sqp` to the project bundle (spec section 2). It is plain content keyed by pattern
  index, so an imported bundle restores the sequences exactly.

## 7. Comparison

| | (a) p-locked chords in the Rytm pattern (built) | (b) MCL on MegaCommand [public] | (c) Digitone / Tonverk [public] | **Sidecar sequencer (this)** |
|---|---|---|---|---|
| where the data lives | Rytm pattern (stock) | MegaCommand SD (grid slots per project) | the device's pattern | Sidecar SD per project/pattern |
| notes per step | 1 root + chord from a 15-entry table (CHD/INV/SPR/VOI) | MD: 1 per track (poly by grouping tracks); ext/piano-roll tracks: real poly | several per trig (Digitone II up to 4) | free (8 per step in the format) |
| steps | 64 (4 pages) | 64 MD tracks, 128 piano roll | 64 (Digitone), 128 (Digitone II / Tonverk) [public, not checked] | 256 (16 pages) |
| lengths per note | the Rytm's LEN | own | per trig | per note, 1/24 step |
| works without Sidecar | data kept (portable) | no | n/a | no |
| copy/paste/clear | stock, data follows | MCL's own | stock | via hooks (section 2), else stale |
| songs, chains, fills, conditions | all stock | MCL's own song/chain (grid rows) | stock | follows the Rytm's switch; conditions/fills reimplemented |
| timing | the Rytm's trig, block-tight, late by the Sidecar's latency | MIDI to the MD, MegaCommand clock | internal | **ahead-rendered: can cancel the Sidecar's latency** |
| UI | stock TRIG + knobs; chord on SRC page | MegaCommand screen + MD keys | native | Rytm keys/LEDs in seq mode + remote page |
| effort | done (0x28, CHD...) | n/a | n/a | large (section 8) |

UX:

- **Pros of the Sidecar sequencer:** real poly lines and voicings (not a chord table), per-note lengths
  (legato pads, overlapping notes), more pages, its own track length and speed (polymeter beyond 64), tighter timing,
  and live-record of real chords from pads or a keyboard (no 72-lock-slot budget).
- **Cons:** two sources of truth. The user must understand that "this track's notes are in the Sidecar". A stock OS,
  a Transfer backup or a project shared without the bundle loses them. The edit UX must feel stock or it will feel
  bolted on. Some stock features (trig conditions, retrigs, p-lock by knob on the stock page) need reimplementing.
- **Who wants which:** chords on a beat (stabs, house chords) are fine with (a). Melodic and harmonic parts
  (arps, pads with voice leading, 32-step progressions) want the Sidecar sequencer. **Both should exist**, chosen per
  track: a track is "Rytm-sequenced (chord locks)" or "Sidecar-sequenced".

## 8. Recommendation and phased plan

**Do it, Pi first, one track type, after the analog pin (POLYPHONY.md 5 Mod B), or with the one-trig AMP workaround
until then.** The Teensy (Sidecar Mini) is frozen at 0.1.0 with no poly (core/poly.h). The sequencer engine itself is
small and could run there later for mono note tracks.

| phase | content | mods (rytm-mods) | host (rytm-teensy) | effort |
|---|---|---|---|---|
| **MVP** | one Sidecar note track per seq-marked Rytm track; transport sync; 16 x 1..16 pages poly notes; trig-hold + knobs (NOTE VEL LEN CHD micro); LEDs incl. page map; live record from pads/keyboard; per project/pattern file; follows pattern changes | 2C step ticks + 2D TRANSPORT/PATTERN/SWITCH/REC/TEMPO/SYNC (UI-tick poll + one block poll); 2E seq mask; seq-mode key/encoder/LED rules in 0026 (no page needed); page LEDs 40..43; `live_rec_trig` refusal on seq tracks; emulator suites as rled/rui | core/seq: pattern model, clock follower (ticks -> frames), ahead scheduler into poly.cpp voices, .sqp load/save + prefetch on queue, recorder (quantise + micro), seq page on remote UI; sim tests against rytm-emu (ticks vs 0x04 timing) | mods ~5 d, host ~8 d, tests/integration ~3 d: **about 3 weeks** |
| 2 | the hooks: copy/paste/clear pattern/track/page/trigs, undo, reload/save, SCALE reports, received-pattern stale mark; bundle export/import | 12 entry detours + 2D 08 EDIT + 2D 04 SCALE | clipboard + undo per level, file ops | ~1.5 weeks |
| 3 | latency calibration (ahead render), swing, mutes, FILL, trig conditions/probability | swing value located + reported; FILL poll | conditions engine, calibration tool | ~1 week |
| 4 | songs/chains polish, direct/temp jump measured, multi-track (several seq tracks), arps on seq tracks | measure the other `0x405c8d04` writers | arp, per-track speed UI | ~1 week |

Risks: (1) every edit gesture depends on the 0026 key machinery staying robust on hardware (it is, as of 1026/1027,
in the emulator); (2) USB send from the audio interrupt (2C) must follow 0021's rules (free requests, try-lock), with
ticks dropped and counted rather than ever blocking; the host must survive a missed tick by re-locking to the next
one and the 2D 03 SWITCH; (3) user confusion about where data lives: show a "SIDECAR SEQ" badge on the remote header
and on the Rytm's stock screen (text slot) when a seq track is selected.

## Measurements (rytm-emu, 2026-10-20) [measured]

Base `sections/us_rui1031.snap` (7400 M), firmware `fw/AR173_us_rui1031.syx`, card `us_rui1031.img` (read only),
`--usb-host --usb-midi-real`, `DTEMU_STRICT=log`; snapshots read with dtpeek; scripts are throwaway (session
scratchpad).

| run | events | result |
|---|---|---|
| play0..5 | PLAY @7600 M; snapshots 1 step apart | per-track step `0x405c8cd0+t` 0a, 0b, 0c, 0d, 0f; previous `0x405c8cea`; tick `0x405c8cdd`; playing `0x405c8c90` = 1; speeds `0x405c8d25+t` = 02 |
| sc0..8 | as above + poke BD length 12, SD speed 0 | BD wraps 0x0b -> 0; SD advances 2 per step; base tracks' lengths 16/64/18 seen in the wraps (`+0x2c5`) |
| watch2 | PLAY, read/write watch `0x405c8cd0` | play-start init at 0x4009b8d6 (copy to 0x42ac13ad) |
| cnt_idle / cnt_play | `--count` | 0x4009b048 203/s (96 PPQN @127), step loop 0x4009b728 51/s (24 PPQN), trig_fire = step increments 0x4009b80c = 221 (13 x 17), 0x400996e2 = 8 |
| brk | `--break 0x4009b048` | audio interrupt (history 0x4012165c), IPL 2 |
| tr | PLAY, PLAY, PLAY, STOP, PLAY, STOP, STOP | FA, FC, FB, FC, FA, FC, FC + CC120 x 13 + FC + F2 00 00; no F8 |
| pc7760..9000, pcc | PLAY; BANK_A + T2 at step 7 | switch at step 16; `0x405c8d04/05` 0 -> 1; counters restart; only `0x4009bb2a` hit (once) |
| st0/st2/st5 | stopped; BANK_A + T2 / T5 | `0x405c8d04` = 0 / 1 / 4 (mirror `0x800067bb`) |
| mu0/1/2 | MUTE + SD (+ CH) | `0x407c23c3` = 0000 / 0002 / 0102 (BE), mirror `0x8000ed9a` |
| k_pcopy / k_ppaste / k_pclear / k_bank | FUNC + REC / STOP / PLAY; BANK menu | 0x400abde0 / 0x400abec0 + 0x400abff6 / 0x400ac11c; callers 0x400cc25c / 0x400cc426 / 0x400cc4fe, args (working pattern, 1) |
| k_gcopy / k_gclear | GRID REC + FUNC + REC, STOP / PLAY | 0x400c2efc, 0x400c3808 + 0x400c4754 / 0x400c494a |
| k_pgcopy | GRID REC, PAGE held + FUNC + REC / STOP | 0x400c4620 / 0x400c3342 + 0x400c35fa |
| k_trcopy | GRID REC, T1 held + FUNC + REC; T5 held + FUNC + STOP | 0x400c4acc / 0x400c396a |
| sc_menu, sc_up, sc_r | FUNC + PAGE (SCALE, ADV mode shown); DOWN x 2; RIGHT x 2 + UP | 16/16 -> 14/16 via 0x400c0b36 (x2, then 0x400c2a2c x 26); speed via 0x400c0c20; encoders A..F did nothing in this menu |

Not measured: direct start / direct jump / temp jump; song and chain playback; FILL; swing's storage; the OB frame
counter's RAM address; anything on a real Rytm; the analog chain with an empty Rytm track (needs Mod B).

## Built: the host half of the MVP (seq-mvp, 2026-10-20)

Pi and Mac (CORE_POLY builds). The Rytm half is rytm-mods 0026 "seq" (seqmod); the wire format is sim/protocol.py's
"The Sidecar sequencer" section, the reference for both. Final layouts (changed from 5.4 after the emulator
measurements): no master tick (2C carries tracks 0..11 only); 2E `<mask lo7> <mask hi5> <flags> [<clock lo7> <clock
hi5>]`, flags bit 1 resync; 2D 02 `<now> <next | 7F>`; 2D 08 SEQMODE `<on> <track>`; 2F `<p1..p4>` page 1..4.

| part | where | what it does |
|---|---|---|
| model + file | core/seq/pattern.cpp | per seq track (4 at once) 8 pages x 16 steps, 8 notes a step, each pitch / vel / len / micro in 1/96 step (micro -96..96); CHD/INV/strum kept per step; **all 128 steps are always stored** (`Step step[kMaxSteps]`, dense) and `length` is only a play and UI limit, so a step past the length is hidden, not gone - in the pattern, on the card (`pack` writes every step index) and back when the length grows; `.sqp` v1 sparse binary, CRC32 (byte-identical to sim/protocol.py `sqp_pack`) |
| clock | core/seq/clock.cpp | Follower: Rytm frames -> host render frames by the fastest arrival over 24 samples; StepClock: per track anchor + period (tempo x speed, measured until a tempo comes), pulled 15 % toward each block-stamped tick, re-anchored on a jump or a tempo / speed change |
| planner | core/seq/seq.cpp | each render block (pre_block, the Pi's RenderBlock / the Mac's core_c_render): steps planned once their earliest note (a step early) is within 32 ms; a note's on = its step frame + micro, mapped to host frames, minus `latency` (default 192 frames, `seq=lat:N`); fired at the block it falls in (+-6 frames); into poly FM's pool or MULTISAMPLE's live notes (poly::seq_on / seq_off, channel 16) |
| transport | seq.cpp | PLAY (or a first tick): every clock restarts, step 0 plays at once (late by design); pause releases and freezes, continue re-anchors; STOP releases, saves; ticks stamped before a stop / pause are ignored |
| switch | seq.cpp | 2D 02's next is prefetched by idle(); at 2D 03 the old pattern's steps from the switch on are cancelled, the new one installed (prefetched copy, else read now), every clock restarts; a dirty pattern goes to a save buffer |
| record | seq.cpp `midi` | pads (poly's channel binding) and a host keyboard (`poly=bind`): REC + PLAY records at the nearest step with the micro offset kept (QNT OFF) or snapped (1/16), length from the note-off; overdub; a note recorded ahead of its step's planning is skipped once (no double with the monitoring) |
| keys | seq.cpp `ui_key / ui_enc` | in seq mode or on the SEQ page: trig tap = toggle (with the last chord played), trig held + pads / keys = the step's notes, trig held + A..H = NOTE VEL LEN MICRO CHD INV STRUM SEL; PAGE = next page, PAGE held = page map, PAGE + trig = that page; without a held trig on the SEQ page: A TRK, B SEQ on/off, C LEN, D QNT, E FOLLOW, F LATENCY |
| LEDs | seq.cpp | trig LEDs: note 47 blue, chord 79 cyan, playing 111 white, held 63 amber, past the length 2; page map 43 / 47 / 111; 2F for the PAGE LEDs; 34 sent by seq itself in seq mode with the page closed |
| page | core/ui/page_seq.cpp | SEQ (last of the pages drawn by code; listed once a track is FM / MULTISAMPLE, a seq track exists or seq mode came): SIDECAR SEQ badge, pattern / page / length / transport, the step strip, a piano roll of the page (a lane per pitch, starts notched: strums read as staircases, the playhead inverted); held: keyboard (each octave's C named over it, C4 = 60; its window kept while the step's notes fit), timing lane, the 8 values |
| storage | seq.cpp | `/.rytm/seq/<project>/pNNN.sqp`, `tracks.txt` (mask, latency, quantise, E0's poly configs); saved 1 s after the last edit and at STOP. fix1049: tracks.txt too waits 1 s after its last change (it was written at every change: a POLY CONFIG knob's every step), and both are written by the card core through the card queue (`fmt::write_atomic`, `.tmp` / `.prev`), never on the Pi's control core (core 3: a card write there is a dropout, heard as the playing note retriggering while the knob turned); a pattern read or a sync write (a project change) waits for a posted one |

Tests: core/tests/test_seq.cpp (model, file, follower with jittery arrivals, step clock with block-quantised stamps,
ahead timing within 48 frames of the Rytm's block and before the tick could arrive, strum, mutes, record OFF / 1/16,
pause / continue, step entry, LEDs, the page's frames, stop + save, the prefetched switch), sim/tests/test_protocol_seq.py
(messages, file, the core reading Python's file byte for byte), sim/seq_sim.py (a simulated Rytm: WAV, timelines, page +
LED PNGs).

Open (not in the MVP): the first step after PLAY and after a pattern switch is not ahead-rendered (a switch-point
prediction needs the master length); sub-block accuracy (within one 12-frame render block, and the Rytm's 32-frame
stamps); latency calibration (`seq=lat:N` by hand); copy / paste / clear hooks and SCALE reports (phase 2); swing,
conditions, FILL; more than 4 seq tracks at once.

## Built: phase 2 (seq-p2 + rytm-mods rui1033seq2, 2026-10-20)

Pi and Mac (CORE_POLY builds). The Rytm half is rytm-mods 0026 "1033" (design.md); the wire format is sim/protocol.py's
"The Sidecar sequencer" section. Emulator only: nothing was flashed. Tags as above.

| part | Rytm (0026 1033) | host (core/seq) |
|---|---|---|
| **Analog pin (Mod B)** | a detour at the voice frame sender 0x4007a538: on a pinned track's voice (its owner track, 0x40290dd8), the amp records held open with stock's own ATK-0 records (the start command once, then the hold: retrigs masked), the filter envelope depth 0, the velocity word 0x7f00 (the VCA at velocity 127; a trig block's VCA rescaled) [measured: sq_emu pin / pinvel]. 2E's optional pin mask (7 bytes) pins the host's poly tracks; seq tracks are pinned unless flags bit 2. 46 closes a silent pinned track (stock's decay record), not silent: one start again | 2E with the pin part only when needed (older builds take 3 / 5 bytes); `seq=pin:T:on|off`, `seq=pinseq:on|off`; 46 when a pinned track's voice has had nothing sounding or planned for 400 ms (poly's voice activity, the planned events), cleared at once when a note is planned (32 ms ahead) |
| **Edit hooks** | the 14 stock edit functions wrapped (detours at their entries): 2D 0A `<op> <pattern> <track> <ok> <src> <arg>` after each, with the function's result (an undo check reports whether it undid); in seq mode a host-held trig / PAGE + FUNC + RECORD / STOP / PLAY is caught and reported as trigs / page copy, paste, clear (src 1) | pattern / track / page / trigs clipboards and one undo per level, as stock: PASTE / CLEAR keep the previous content, UNDO (ok 1, the same kind) restores it; seq mode's own page / trig paste or clear undone by repeating the gesture; every op replans the track(s) from the next step and saves (debounced) |
| **SCALE** | 2D 04 per track (length, speed) and the pattern's (master length, speed, mode, change length), polled at 20 Hz, on a change and in each resync; FUNC + PAGE reaches stock's SCALE in seq mode | a seq track with its own length follows a length changed on the Rytm (AUTO follows anyway); a longer length copies the pages it adds (seq1112 below); the speeds and the pattern's lengths are kept (the view, the prediction); `seq=len:T:N` |
| **Switch prediction** | 02 PATTERN keeps the playing pattern until its 03 (the active-pattern byte moves a step early) | the queued pattern's first steps planned ahead at the next multiple of the change length (mode 1) / master length from the pattern's step 0, at the pattern speed; the 03 at that frame (half a step) keeps them, anything else drops them and plans the old pattern again; queued inside the boundary's last step: the next boundary |
| **Latency calibration** | (none) | `seq=cal[:T]`, automatic once when a pinned seq track exists, the Rytm's stream comes and the transport is stopped: 5 clicks (1 kHz, 2 ms, -9 dBFS) rendered into the track's voice, found again in the Rytm -> host stream of that voice; the median round trip (a spread under 480 frames) becomes the latency. Set by hand (`seq=lat`, the page's F): kept (`latset=1` in tracks.txt) |
| **Swing** | not located (see below) | per track, `seq=swing:T:50..80` (Elektron's scale: every second step late by swing / 50 - 1 of a step), in the file's swing byte |
| **Mutes, FILL** | 2D 05 (1032), 2D 0C FILL (the key, 0xb2) | a muted track starts no note and its notes end (1032); FILL kept in the view (no trig conditions yet) |
| **Pattern dirty, the Rytm view** | 2D 0B DIRTY from a rolling checksum of the working pattern (4 KB a UI tick) and every edit report | core/seq/rytm_dump: the stock SysEx dump of the active pattern (its working buffer) on the consumer's request (`rytmpat=on`, rytm_dump::set_active), each pattern change (250 ms after the last of a run) and DIRTY, paced (stopped: 1 s after the last, at most 4 s after the first; playing: one every 4 s at most; not while a song take records, nor on the VOICE page while playing: docs/SONG.md "The touchscreen's PAT / VOICE pages"); the answer read beside the platform's MIDI parser (14988 B: longer than its SysEx buffer), checked (checksum, length), the 13101-byte record kept for the touch view's decoder |

The dump request [measured, rytm-emu]: `F0 00 20 3C 07 00 64 01 01 <n> 00 00 00 05 F7`, n = the active pattern (its answer
is the working buffer, an unsaved edit included). The community's `0x80` working-buffer number answers the same, but
0x80 is not a MIDI data byte: rytm-emu's byte-stream link cut the SysEx there; on the Rytm's own USB-MIDI packets it
passes. The answer: `F0 00 20 3C 07 00 54 01 01 <pattern> 00`, the record's 4-byte header (`00 00 00 05`), the other 13097
bytes 7-bit packed (14968 B), a 14-bit checksum (the sum of the bytes from `<pattern>` up to it) and a 14-bit length
(the message's size - 10), F7: 14988 bytes. sim/protocol.py's rytm_dump packs a record to the Rytm's own answer byte for
byte. The record decoder is pi/ui/rytm_pattern; on the Pi, pi/engine/rytmpat_pi.cpp registers with rytm_dump::on_record,
decodes on core 3 and hands the pattern to the PAT page (docs/TOUCH-EDITOR.md); the page open = rytm_dump active.

Swing [code, not measured]: the swing menu (0x400676a6) shows `+50` of byte +5 of an object the pattern's vfunc 40
returns (0x400aaf14); the 1.73 TEMPO screen in rytm-emu shows BPM only, and turning its encoders moved no pattern byte.
Pattern +85691 is read with each track's +0x2c6 by trig_fire (0x4009aca4); its meaning is not established. So the host
keeps its own swing.

Tests: core/tests/test_seq.cpp (pattern / track copy-paste-undo-clear on the host data, page and trig gestures with
their undo, SCALE following, the 2E's pin part and the 46, swing's offset, the predicted switch: A02's step 0 planned
15 frames from the Rytm's switch block and fired 196 frames before its 03 arrived, the calibration on a simulated 713-frame
round trip: 714, the dump: request, parse, a damaged one refused, DIRTY and pattern-change triggers),
sim/tests/test_protocol_seq.py (the new messages, the dump's packing and the core reading it), rytm-mods
tools/emu/sq_emu.py (pin, pinvel, edits, seqedit, scale, dirty, fetch), sim/seq_e2e.py p2 / nopin (below).

End to end [measured, rytm-emu seq-p2 + rui1033seq2 over the Overbridge link, the real core as the host:
`sim/seq_e2e.py p2 nopin`]: CY a seq track with no trig on the Rytm (no envelope start in the base pattern), one host
note on its step 1 (6-OP FM on voice 6), BD a stock trig on step 1 as the reference.

- The latency calibrated itself after the connect: 5 clicks on CY's voice, 698..723 frames, 700 taken.
- FUNC + REC on A01, A02 selected, FUNC + STOP: the host's p001.sqp is p000.sqp byte for byte.
- CY's stem carries the host's notes through the pinned amp; with the pin off (`seq=pinseq:off`) the host plays and the
  stem stays silent.
- From the second bar CY's onsets land -11, -23, -26, -23 frames from BD's (the first of them A02's step 0, the switch
  predicted: `predicted 1 (ok 1 miss 0)`); an earlier run: -17..-63. The first bar after PLAY is late by the latency
  (PLAY cannot be predicted). The residual is the two sounds' onset shapes and the 32 / 12-frame blocks.
- The Rytm's pattern dumps came on the start, the placed trig (DIRTY) and the pattern changes (13101-byte records).
- Found on the way and fixed: a seq note on an empty Rytm track never made its voice render (no 0x04 makes it live,
  and a link reset's CC 120 silenced it until a trig): `voice_wake`. rytm-emu's analog model read the live frame
  between the consumers' and the pin's writes: it now takes the frame as sent. 1032's 02 PATTERN reported the queued
  pattern as active a step before its switch (the host then restarted early): 1033 keeps the playing one until 03, and
  the host waits for the 03 when 02's new pattern is the queued one.
- The emulator's LT (and SD, MT/HT, RS/CP) get envelope starts from the base pattern at PLAY [measured: DTEMU_AMP_TRACE]:
  a test track must be truly empty (CY, BD here) for "silent without the pin" to mean anything.


## Built: seq-ux (seq-ux + rytm-mods rui1036, 2026-10-20)

Changes after the first hardware use ("works well!"). The Rytm half is rytm-mods 0026 "1036" (design.md); the wire is
in sim/protocol.py. Emulator only: nothing was flashed. Tags as above.

### How to use it (on the Rytm)

1. **Open the SEQ view: UP + TRIG** (rui1046; TRK + TRIG from rui1037 still works; rui1036 used FUNC + LFO). It
   works on any track: select the track (TRK + pad), then hold TRK and press TRIG. The Rytm shows the Sidecar's SEQ page
   for that track. The Pi's touchscreen switches to its SEQ piano roll on the same track. To go back, press TRK + TRIG
   again, or NO.
2. **Make a track a seq track.** If the track is no seq track yet, TRK + TRIG asks **MAKE BD A SIDECAR SEQ TRACK?**.
   YES makes it one (saved per project in `tracks.txt`, as `seq=track:T:on` did). NO goes back. A track needs a
   Sidecar poly machine on its SRC page: 6-OP FM, MULTISAMPLE, WAVETABLE or GRANULAR. On an analog machine, JUNO (mono
   in this build: core/poly has no JUNO pool) or another mono Sidecar machine, the page explains why instead.
   (Since E0 / rui1048: every Sidecar machine; only an analog track is explained - "E0 on the Rytm" below.)
   **Turn it off:** on the SEQ page with no trig held, turn B down. **TURN SEQ OFF ON BD?** YES turns it off, NO keeps
   it. The steps stay in the pattern and on the card; turn B up (YES) to get them back.
3. **Edit steps** as before: GRID REC on the seq track (seq mode), tap trigs, hold a trig + pads for notes, hold a trig +
   A..H for NOTE VEL LEN MICRO CHD INV STRUM SEL. **REC still switches to the track's recording view.**
4. **Parameter locks.** Hold one or more trigs and press a page key:
   - **SRC**: A..H lock P1..P8 of the track's Sidecar machine.
   - **SMPL**: A..H lock P9..P16 (for example CHD INV SPR VOI).
   - **FX**: A..D lock FX1 P2..P5, E..H lock FX2's.
   The page shows **LOCK SRC** (or SMPL / FX) with the machine's knobs. A locked knob is framed and its label inverted,
   as the Rytm marks a p-locked knob. The bottom bar shows the step and the value (`S5 LEV:34 LOCK`). Turning a knob
   locks it on every held step (FUNC: x 8). **NO** clears every lock of the held steps; **an encoder's push** clears
   that knob's lock. Letting go of the trigs ends the lock page; a held step is not toggled.
   - Locking an empty step makes a **lock trig**: locks without notes, which change the sound under a ringing note.
   - FLTR, AMP and LFO show a page explaining that the Rytm's analog side and the Sidecar's LFOs are not locked per
     step (below).
5. **Where locks show:**
   - trig LEDs: a step with locks is magenta (a lock trig dim magenta);
   - the remote SEQ page: a tick over the step strip per lock (1..6 px), a lock trig as a hollow box;
   - the touchscreen piano roll: a lock lane over the velocity lane (a bar per locked step, brighter with more locks).

### How the locks play [measured, core/tests/test_seq.cpp, sim/seq_e2e.py lock]

- Storage: per step up to 12 locks {param, 14-bit value}. Param 0..15 is the machine's P1..P16, 16..23 is FX1 / FX2
  P2..P5. They live in the `.sqp` file, now **version 2** (a lock is 3 bytes). Version-1 files still load: their lock
  bytes were never written, and they are read as an integer value. A step with only locks is stored too. Copy / paste
  / clear / undo of patterns, tracks, pages and trigs carry the locks with the steps.
- Playback: the planner pushes each step's lock set as an event on the step's frame. When notes start early (negative
  micro), the event moves to the earliest of them, minus the latency. It fires in the render block it falls in, before
  that block's note-ons. So the lock lands within one 12-frame block of the note (test: the lock frame equals the
  note's due frame). The set is copied at planning time, so a predicted pattern switch plays the new pattern's locks.
- Hold and restore, as the Rytm's p-locks: a step with notes or locks applies its set. Its params get their locked
  values, and every param the track locked before goes back to its base. A step with neither changes nothing, so a lock
  holds under a ringing note until the track's next trig or lock trig. STOP gives every base back.
- The base is the Rytm's value. A 03 / 04 report that arrives during a lock becomes the new base: the lock stays and
  the new value comes back after it (`seq::params_in` in voice_report, before the LFOs). An LFO on a locked param uses
  the lock as its base. FX locks are global: the last track that locked an FX param owns it. They go through
  fx::knob_filter (the LFOs' filter chained after ours).
- End to end [measured, rytm-emu rui1036 + the real core over the Overbridge link, `seq_e2e.py lock`]: CY (an empty
  Rytm track, 6-OP FM on voice 6) plays C3 on steps 1 5 9 13, with LEV (P7) locked to 12 on steps 5 and 13. Every note
  peak in the host's render: 6523 6518 6524 6518 on steps 1 / 9, 782 on every step 5 / 13 (12 / 100). The lock is
  applied on its step and given back at the next trig.

### Not built

- **Analog locks (FLTR / AMP / the Rytm's FX as per-step CC / NRPN): skipped.** A CC sets the Rytm's *stored* kit value
  (core/lfo.h, 1026: why the LFOs' CC targets went), so every locked step would rewrite the kit and leave it dirty. The
  host does not know the analog params' base values, so it could not restore them. The USB MIDI hop also adds 1-3 ms
  and is processed in the Rytm's MIDI task, so it would not be sample accurate. A clean version needs the Rytm's own
  lock path (track_lock_set on a trigless lock trig, as 1030 writes CHD..VOI), but on a seq track the Rytm plays no
  trig of its own, so those locks would not fire. Open.
- Locks on JUNO, PLAITS and the other mono machines: a seq track cannot be one (see 2.).

### Protocol additions (sim/protocol.py)

| dir | message | meaning |
|---|---|---|
| Rytm -> host | `30 01 <0x10 + t> 01 00` | TRK + TRIG (rui1036: FUNC + LFO): the SEQ page of track t (an old host shows SIDECAR) |
| host -> Rytm | `47 01 <list> <replace> <total> {<slot> <13 B>} x <= 8` | the source lists' names (list 0 GRANULAR SRC, 1 WAVETABLE TBL, 2 MULTISAMPLE INST): the Rytm's own value texts (`rui_names`) |
| host -> Rytm | `48 01 <track> <param> <value> [<frac>]` | P(param + 1) set as a knob turn (the touchscreen's GRANULAR pick becomes the Rytm's SRC; `rui_write`) |
| Rytm -> host | `48 01 <track> <param> <status>` | 0 written, 1 refused (shared SRC records on another machine), 2 no such param |
| Rytm -> host | `31 01 00 <0x70..0x77> <down>` | the SRC view's encoder pushes (not caught): P1's keeps the source list open |

Tests: core/tests/test_seq.cpp (lock model and file v1 / v2; the make / why-not / off prompts with YES / NO; the lock
pages SRC / SMPL / FX / FLTR: turns, FUNC, pushes, NO, a lock trig, the LEDs; the locks played and restored, a
report under a lock, STOP; 47 once per change, 48 per track; PNGs seq_make / seq_explain / seq_off / seq_lock_src /
seq_locks), sim/tests/test_protocol_seq.py (v2 file, v1 read, 47 / 48), pi/ui/host (test_seq_touch; the shots' lock
lane), sim/seq_e2e.py `ux` (Rytm screenshots: make prompt, LOCK SRC, the SEQ page with locks) and `lock`; rytm-mods
`sq_emu.py funclfo lockkeys` (rui1037: `trktrig lockkeys yes`), `sv_emu.py push names`.

### The Pi display from the Rytm (rytm-mods rui1036b)

Hold **UP** and press **T1..T5** to switch the Pi touchscreen's page (MegaCommand style):

| key | page |
|---|---|
| T1 | HOME |
| T2 | SEQ (the piano roll) |
| T3 | PAT (the Rytm's own pattern) |
| T4 | VOICE: the selected track |
| T5 | SET (the settings) |

- VOICE depends on the selected track's machine:
  - GRANULAR or WAVETABLE: the machine view (grains, or the table's frames).
  - Any other machine, analog tracks included: the VOICE page. It shows the machine's name, its SRC knobs P1..P8 as
    big dials with the Rytm's own labels, a live scope and level of the track's Overbridge stem, and the track's p-lock
    heatmap from the Rytm's pattern dump.
- While on VOICE or the machine view, selecting another track on the Rytm (TRK + pad) switches the page to that track.
  The SEQ piano roll and the machine view follow the selected track too.
- TRK + TRIG (rui1036: FUNC + LFO) still opens the SEQ page on the Rytm itself.

Why UP: rytm-mods 0026 design.md "1036b" has the probe of every hold key + trig on the stock OS. Only the arrows add
nothing to a trig press anywhere.

Wire: Rytm -> host `49 01 01 <page>`, `49 01 02 <track> <machine> <name 12>`, `49 01 03 <track> {<value> <label 4>} x 8`
(core/ui/remote.h remote_display_request / remote_selected_track / remote_track_info). Pi: pi/engine/voice_pi.cpp
(requests, VOICE's data on core 0) and pi/ui/sidecar_voice.c (the page; both themes).

Limit: the scope reads the remote UI's input tap. While the Rytm shows a Sidecar remote page other than SCOPE, that page
turns the tap off at each frame, so the VOICE scope may stay flat until the page closes.

## Built: rui1037 (song-mvp + rytm-mods rui1037, 2026-10-21)

Emulator only: nothing was flashed. The Rytm half is rytm-mods 0026 "1037" (design.md, with the stock probes).

- **TRK + TRIG opens the SEQ view** (was FUNC + LFO). It sends the same 30 (`30 01 <0x10 + t> 01 00`), so the host
  code is unchanged. FUNC + LFO is the stock LFO page again. Stock 1.73 binds nothing to TRK + TRIG [measured, rytm-emu,
  rytm-mods `rled_emu.py stocktrktrig`: it leaves what TRIG alone leaves].
- **Step preview: hold a trig + YES.** In seq mode (GRID REC on a seq track), or on the SEQ page, hold one or more trigs
  and press YES. The held steps' notes play on the track's voice:
  - the whole chord, each note at its own velocity and length, with its micro offset (a broken chord plays as
    written, counted from the press);
  - the first held step's p-locks are applied before its notes (as `plan_step` does; a track with locks gets its bases
    back for a step without any). Stopped, the bases come back after the last note;
  - the transport stopped or playing. Nothing advances, nothing is recorded, and the step is not toggled when its
    trig is released.
  The notes go through the planner's events (`seq::preview`, pre_block starts them at the next block), and each one
  ends after its own length. So no note hangs whatever becomes of YES's release, and STOP / `release()` drop them as
  any planned note. The step length is the track's step at its tempo (the clock's period while it runs, else the
  reported tempo, else 120 BPM). YES's release goes where its press went (the preview). YES under a prompt still
  answers the prompt. On the Rytm, YES under a held trig only goes to the host in seq mode, so other tracks keep
  stock's YES. Stock's trig held + YES is YES alone [measured: `rled_emu.py stockyes`], so nothing is lost.
- `seq` stats: `previews N`.

Tests: core/tests/test_seq.cpp (the preview stopped: the chord's 3 notes at the next block, P2's lock on, each note
off after half a step, the base back, the step unchanged, nothing recorded; playing: the 3 notes while BD's ticks run;
no pool note held after either, `poly::held_notes`); sim/seq_e2e.py `preview` [measured, rytm-emu + rytm-mods rui1037
+ the real core]: CY with C3 E3 G3 on step 1, stopped, GRID REC on CY, T1 held + YES: the chord in the host's render
(peak 9411), seq stats on 3 off 3; T1 held alone: silent. `ux` with TRK + TRIG (the make prompt) passes as before.
rytm-mods: `sq_emu.py trktrig lockkeys yes`.

## dbg: the hardware report on rui1037 / fe1e176 (2026-10-21, emulator only)

- **A chord made by turning on an empty held step came out at C0.** `note()` took the last chord played from every note
  poly's sink saw, also the Rytm's own channel notes with no Sidecar voice (an analog track's trig, an unbound pad), and
  an empty step's first turn (`edit_step` -> `fill_default`) started from that chord. Now only notes a Sidecar voice
  plays count as "played", and an empty step's first turn starts from the track's default note (`dnote`, 60) - a tap
  still takes the last chord played. A pattern file whose `dnote` is 0 loads as 60. [measured: test_seq "fresh" (the
  Rytm's note 0 on an unbound channel, then T10 held + CHD: 0 4 7 before, 60 64 67 now); `sim/seq_e2e.py fresh`: T3
  held, A +4, B, CHD +1 -> 64 68 71]
- **The Rytm's UI slow with the SEQ page open while playing.** The page's playhead (the strip's frame, the roll's
  column) moves at most every 250 ms; the trig LEDs (34) show every step. [measured: core/tests/test_traffic.cpp case 7,
  120 BPM: 39 commits 8.0 -> 4.0 a second, 58.2 -> 51.7 messages, 1520 -> 1096 B a second; with rytm-mods rui1037b in
  rytm-emu the Rytm's stock redraws under the page 100 -> 55 per 100 UI ticks (43 with the page closed)]
- **The make prompt's YES with TRK still held** was the Rytm's TRK + YES (rui1037): fixed on the Rytm (rui1037b);
  `sim/seq_e2e.py makeheld` checks it end to end. **PLAY MODE under the page** (from MUTE mode) reaches stock in
  rui1037b; the host's mask is unchanged (FUNC + PLAY MODE, the KIT menu, stays caught).
- `seq=steps:T`: a console line with track T's steps and their pitches (tests, debugging).

## fix1040: the hardware report on rui1039 + the TRIG view and trig conditions (2026-10-21, emulator only)

The Rytm half is rytm-mods 0026 "1040" (design.md). Nothing was flashed. Tags as above.

- **Step copy / paste / clear on a seq track.** Hold one or more trigs and press FUNC + REC (copy), FUNC + STOP (paste
  onto the held steps; a shorter clipboard repeats) or FUNC + PLAY (clear them). rui1039 sent RECORD / STOP / PLAY to
  stock whenever the SEQ page was open (seq mode opens it), so stock copied / pasted / cleared the whole track and the
  host followed [measured, rytm-emu + the real core]. rui1040 catches the gesture with the page open too and reports
  the trig ops (2D 0A 13 / 14 / 15, src 1, the held mask); PAGE held + FUNC + ... is the page's. Without a held trig the
  track / pattern ops work as before. The host marks the held keys used on these reports (their release toggles
  nothing). [measured: `sim/seq_e2e.py stepedit` -> steps 1:60 5:60 from 1:60 3:64 after copy T1, paste T5, clear T3]
- **TRIG on a seq track.** In seq mode TRIG goes to the host (the Rytm sends it with the page closed as well): the page
  closed, it opens the SEQ page on the notes; open, TRIG toggles the SEQ page between the notes and the **TRIG view**,
  as stock's TRIG page beside the grid. Under held trigs TRIG toggles the view too (it ends a lock page).
  - TRIG view, held steps: NOTE VEL LEN MICRO / **COND** STRUM CHD INV of the first held step; turning edits every held
    step (FUNC: coarse). COND is one knob as on the Rytm: `---` (none), FILL, !FILL, PRE, !PRE, NEI, !NEI, 1ST, !1ST,
    1:2, 2:2, 1:3 .. 8:8, then 1% .. 99%.
  - TRIG view, nothing held: the track's defaults for new steps, NOTE VEL LEN, and SWING (50..80).
- **Held trig + a page key** as before: SRC / SMPL / FX the lock pages with the held step's values (locked knobs framed);
  FLTR / AMP / LFO the page saying they are not locked per step (the Rytm's analog side; 1036 "Not built"). Release:
  back to the view you were on.
- **Trig conditions** (core/seq seq.h `kCond*`, `cond_pass`): per step, evaluated by the planner at the step:
  - A:B: true on loop A of every B, loops counted per track from PLAY or a pattern switch (the step clock's absolute
    step / the track's length);
  - 1ST / !1ST: the first loop since PLAY / the switch;
  - FILL / !FILL: the Rytm's FILL key held (2D 0C, since rui1033; a latched FILL is not reported);
  - PRE / !PRE: this track's last condition result; NEI / !NEI: the track before's (track 1 has none: false). PRE and
    NEI do not set the result; every other condition and a probability do (as on Elektron boxes);
  - the probability (1..99 %) as before. Results start false at PLAY and at each switch.
  The condition lives in the step's flags (bits 2..7), so copy / paste / undo carry it. The `.sqp` file is **version 3**
  only when a step has a condition (older Sidecars keep reading files without any); sim/protocol.py mirrors it.
  Console: `seq=cond:T:S:C` (C the COND knob's index), `seq=steps:T` prints `S:pitch..[cond]`.
  [measured: core/tests/test_seq.cpp (each condition's rule, PRE / NEI, 50 % -> 490 / 1000, the knob order, the file
  v2 / v3, 1:2 over three loops planned 2 of 3, the TRIG view); `sim/seq_e2e.py trigview cond` (1:2 on step 2 of 4:
  notes on 8 vs 10 without)]
- Limits: a condition is evaluated when the step is planned (32 ms ahead); an edit that replans the track can evaluate
  a step's PRE / A:B result again. Conditions are not shown on the touchscreen piano roll yet.
- **Bug 5: leaving a seq track** (hardware, rui1039 + 5cc980e: "after selecting SD the pads and trigs still play BD's
  6-OP FM, as if BD were in CHRO"). The rule (the user's): chromatic notes on the pads only in the Rytm's CHRO mode,
  never forced by seq mode; in the normal pad mode the pads are stock's. Measured with the real core
  [`sim/seq_e2e.py padsel`]:
  - The Rytm's normal-mode pads send note t (0..11, the pad's track) on the auto channel; CHRO sends the selected
    track's chromatic notes (12..) there. poly's live binding (a channel bound to the voice whose 0x04 coincides with
    its note-ons) bound the auto channel to BD's voice from BD's own pad hits, so afterwards every pad played the 6-OP
    FM voice. Now notes 0..11 on the auto channel (keys `auto_channel()`, default 14) are pad triggers: no binding, no
    Sidecar note, no step entry, no recording (core/poly.cpp `midi`). Measured without that gate: ch 14 bound to BD's
    voice from the normal BD pads and SD's pad played a Sidecar note.
  - A selection change (the Rytm's 49 02) unbinds a channel bound to another voice (`poly::selected_voice`): CHRO on
    SD after CHRO on BD plays SD's own chromatic notes, no Sidecar voice. CHRO on the seq track still plays its poly
    voice (and enters notes under held trigs).
  - The SEQ page the seq mode / TRK + TRIG opened closes when the selection moves to a track that is no seq track
    (`seq::selected_track`; another seq track: the page follows it), so the trig keys are stock's again (rui1039: T2
    placed a host note on BD after SD was selected). BD's pattern keeps playing.
  - TRK + pad under the page (rytm-mods rui1040): stock gets the caught TRK's press before the pad, so TRK + SD selects
    SD also in CHRO / with the page open.
  Tests: core/tests/test_seq.cpp (a pad trigger under a held trig enters nothing, a CHRO note does), `sim/seq_e2e.py
  padsel` (seq mode off on the SD pad, the page closed, no binding / no Sidecar note from normal pads, BD's steps
  unchanged and playing, TRK + BD back to seq mode, CHRO on BD plays its poly voice, TRK + SD in CHRO under the page
  selects SD and unbinds), rytm-mods `sq_emu.py trkpad`.
- **fix1040b: a seq track's own pad** (hardware, rui1040 + 73bd0aa: "in the normal pad mode BD's pad no longer plays
  its sound"). Bug 5's gate silenced it: on a seq track the voice's 0x04 makes no note (the Rytm's own trigs on a seq
  track must not sound: `seq::owns_voice`), and BD's pad trigger (note 0 on the auto channel) used to start the voice
  through the pad binding, which the gate now forbids. Now the pad trigger and the voice's 0x04 are matched within
  kBindMs, in either order (`poly::seq_pad`, `pad_trigger`): the 0x04 then plays as any Sidecar track's trig (its note,
  its CHD / INV / SPR / VOI chord, the pad's velocity) on the track's voice, once, and the pad's release (its trigger
  note-off) ends it. Still no binding and no step entry from pad triggers; a 0x04 with no pad (the Rytm's own trig) makes
  no note; another track's pad plays that track's own sound; a non-seq Sidecar track's pad plays through the 0x04 path
  as before (no double trigger). The `poly` console line counts `seq pads`. [measured: test_seq (pad first, 0x04 first,
  no pad, SD's pad); `sim/seq_e2e.py padsel`: BD's three pad presses -> 3 seq pads, SD's pads -> none]

## fix1042: data safety, the Rytm's RELOAD / SAVE, pattern jumps, the CHRO leftover (2026-10-21, emulator only)

The Rytm half is rytm-mods 0026 "1042" (design.md). Nothing was flashed. [measured] = core/tests/test_seq.cpp,
sim/tests/test_core_um.py, rytm-emu + rui1042 (DTEMU_STRICT=1).

- **A pattern file that does not load is locked (data safety).** A `.sqp` from a newer Sidecar (version above this
  build's), with a bad CRC, cut short, or there but not readable: `load_into` used to leave the pattern empty and return
  false, both callers ignored it, and the next edit saved the empty pattern plus the edit over the file. Now the pattern
  index is locked until the project changes or the file loads: edits stay in memory, `save_pattern` refuses (log `seq:
  pattern A05 not saved: its file is locked`), the SEQ page's footer says `A05 FROM NEWER SIDECAR: LOCKED` / `A05 FILE
  DAMAGED: LOCKED`, the file is never written or deleted. `seq` stats: `load_fails`, `save_refused`. [measured: newer,
  CRC-bad and truncated files, each edited, switched away and back: the bytes on the card unchanged]
- **Project change wrote the old project's edit into the new project's folder.** pool.cpp's set_project took the new
  name before `seq::project_changed` saved the old pattern, so the save went to `/.rytm/seq/<NEW>/pNNN.sqp`, over the
  new project's file of that index. Now `seq::project_leaving()` saves first, under the old name. [measured: SEQTEST's
  unsaved edit lands in SEQTEST's file; nothing written into OTH] **For the bundles branch:** this touches
  set_project's order (one call before `strcpy(project, ...)`); the project-name / folder function itself is unchanged.
- **tracks.txt**: written by tmp + rename; there but not readable: not written over until the next project load.
- **The same rule for the other per-project files** (the audit of every card reader that rewrites its file): the LFO and
  INSERT kit files (`/.rytm/lfo`, `/.rytm/ins`) with a line this build does not read (a newer Sidecar's target / FX type,
  damage), or there but not openable, are not written over (the settings change in memory, logged); the pool file
  (`/.rytm/pools`) there but not openable, or with a line longer than a path (cut at 159), likewise. [measured:
  test_lfo, test_insert, test_pool: each file's bytes unchanged after an edit]
- **User slot map / notes** (core/user/um.cpp): a `/.rytm/slots/<PROJECT>.txt` or `.inf` that does not parse (one
  unknown line fails the whole map, rmp_slots.h) is never written over: the slots change in memory only, logged.
  [measured: test_core_um `test_damaged_map_kept`; before, assigning U3 replaced a two-slot map with `U3 ...` alone]
- **The Rytm's SAVE / RELOAD PATTERN and SAVE / RELOAD TRACK** (2D 0A ops 16..19; the PATTERN menu FUNC + SCNE, the TRACK
  menu FUNC + CHRO). SAVE PATTERN writes the working pattern as `pNNN.sqs` (the saved state); SAVE TRACK puts that track
  into it (the others as saved, or as they are now when there is no saved state yet). RELOAD PATTERN / TRACK restores
  from `pNNN.sqs`, keeping the working copy first as `pNNN.prv`; with no saved state (the Sidecar never saw a SAVE of it)
  the steps are kept. A locked pattern is neither saved nor reloaded. **For the bundles branch:** `pNNN.sqs` / `pNNN.prv`
  belong in the bundle beside `pNNN.sqp`. [measured: test_seq; rytm-mods `sq_emu.py savereload`]
- **DIRECT JUMP / TEMP JUMP** (FUNC + BANK_C / D): the new pattern plays from the old position at once. The Rytm's 03
  now comes at its first step (rui1041: at its step 0, 2 s late, or never for TEMP JUMP), and the step clock restarts at
  the tick's step (was 0: the new pattern played from its step 0 at the old position). [measured: test_seq "jump": A02's
  step 11 planned 212 frames (the latency) ahead of the Rytm's step 11 after a jump at step 9; `sq_emu.py jump`]
- **The CHRO leftover.** TRK + another pad with the page open sounded one note on the old seq track's voice: stock sends
  the pad's note before the selection report. The page's TRK (31) now marks TRK held in poly; a note on the auto
  channel while it is held is the selection's (no voice, no step entry); cleared at TRK's release, the page's close or
  after 10 s. [measured: `seq_e2e.py padsel` live pad notes 4 -> 5 before, unchanged now; test_seq]
- **FILL** reaches the host while held, the page open or not [measured: `sq_emu.py fill`]; a latched FILL does not.

Still open: SAVE PROJECT / project copy / rename on the Rytm (the saved state per pattern follows SAVE PATTERN / TRACK
only; the project's folder follows the name: bundles); song mode and chains pass the same switch report (not measured
end to end here); multi-speed edge cases only as the existing ticks / SCALE tests cover them (no new case); card files
the audit found that still drop unparsed lines on their next save (the JUNO / DX7 lists, GRANULAR's picks) or can reuse a
take name when the takes folder cannot be listed (song.cpp, the bundles branch's area); the LFO / INSERT / pool writes are not
tmp + rename yet (a power cut mid-write).

## rui1046: UP is the Sidecar's modifier (2026-10-22, emulator only)

The user's request: one modifier for every Sidecar shortcut, UP (TRK is the stock Rytm's busiest key; UP held alone
shows the map). The Rytm half is rytm-mods 0026 "1046" (design.md); the table is `core/ui/page_keys.h`
(docs/UI-THEMES.md "The device's shortcuts"). Nothing was flashed. (rui1052 later reordered T1..T8 into the
touchscreen sidebar's order and made UP's map adapt to whether a touchscreen is up - host side only, the same mod.)

- **UP + TRIG** opens / closes the SEQ page (TRK + TRIG still works). YES on its make prompt with UP still held is the
  page's YES, as with TRK.
- **Step copy / paste / clear, one-handed:** on a poly track (seq mode) hold the trigs first, then UP + REC (copy),
  UP + STOP (paste onto the held steps) or UP + PLAY (clear them): the same 2D 0A 13 / 14 / 15 (src 1, the held mask)
  as trig + FUNC + REC / STOP / PLAY, which still works. On a poly track with a trig held the Rytm takes UP at once (no
  wait: stock never saw that trig, so its held trig + arrow has nothing to do there). UP first and then a trig is UP +
  Tn (a page request), so the order is: trig, then UP.
- **Without a held trig UP + REC / STOP / PLAY is the stock transport, nothing new**: a stray UP is harmless
  [measured, rytm-mods `sq_emu.py upseq`: UP + PLAY in seq mode without a trig -> 2D 01 1, no 0A].
- On a normal (stock) track nothing of this applies: UP and the held trig are stock's as before [measured, rytm-mods
  `rled_emu.py upstock`].

## Built: seqx - E0, E1 and E2 (2026-10-22, host only; docs/SEQ-IDEAS.md)

Host side only: no rytm-mods change, no Rytm-screen UI. The Rytm's POLY CONFIG entry (its track menu, FUNC + CHRO) is
built since (rui1048, below, "E0 on the Rytm"); the RANDOM page waits for the sequencer's next branch.

### E0: every Sidecar machine on a poly track, VOICES per track

- **Every Sidecar machine can be a seq track** (`can_seq`: only the analog machines are refused now; JUNO, PLAITS,
  DAISYSP, DSP SAMPLE, MD and the user machines get the make prompt like 6-OP FM). A seq track's own Rytm trigs
  make no note on any machine; its pad in the normal pad mode plays its voice (fix1040b's rule, now for every machine).
  (DIGIDRUM was replaced 2026-10-29 by the built-in MD on the same flag 0x40, `core/md/md.h`, docs/MD-BUILTIN.md: a
  track on DIGIDRUM plays as MD, mono, the note a hit.)
- **The poly config per track** (`seq.h` PolyCfg), the Analog Four MKII's POLY CONFIG words [public, recalled; the
  Elektronauts threads on ALLOCATION]: **VOICES** 0 AUTO / 1..8, **ALLOCATION** REASSIGN (default) / ROTATE / RESET,
  **UNISON** 1..8 with **DETUNE** (127 = +-50 cents at the stack's edges) and **SPREAD** (pan, on a voice on OUT PRE /
  POST), **GROUP** 0..4 (the data model of a poly group across tracks; not played yet - docs/POLYPHONY.md 7).
- **Where it is stored (decided):** in the pattern (a v4 block per track), and as the Rytm track's **default** in
  `tracks.txt` (`poly=T:V,A,U,D,S,G`): a track plays its pattern's config, else its default - the last one set on it in
  this project. So a track keeps its voicing across patterns as an A4 keeps POLY CONFIG in its kit, a pattern may still
  differ, and nothing new is written to the card on the control core (the pattern's and the tracks' existing saves).
  Per kit was the alternative: the A4's place, but the Sidecar's seq data has no per-kit file, and a new per-kit card
  file would be new card work on core 3.
- **Where a note goes** (`core/inst.h`, the sequencer's `note_on` / `note_off`): a machine with its own pool (6-OP FM,
  MULTISAMPLE, WAVETABLE, GRANULAR) - that pool, VOICES capping the notes held at once (the oldest held one released
  for a new one; AUTO: no cap, exactly as before E0); one instance resolved (VOICES 1, DSP SAMPLE, a mono user machine)
  - the voice's own machine as a Rytm trig plays it (`voice_play`): a new note retriggers / chokes the old; more - the
  **instance pool**: 32 instances shared by the voices (4 seq tracks x 8), each with its own PLAITS / DAISYSP /
  JUNO / user-machine state, allocated per the track's ALLOCATION (steals: the same pitch held retriggers its
  lane; then the quietest released tail, then the oldest held note), stacked by UNISON, rendered into the voice's
  channel on its render core after the voice's own sound, before its INSERT chain, at the pool's headroom gain (1 /
  sqrt of the sounding weight, as poly.h) and through the soft limiter. Released and below -80 dB for 10 ms: free.
  CC 120, the link reset and a machine change silence them.
- **The note's pitch per machine** (`inst_alloc.h` Decl.pitch): the note is the pitch (+ TUN / FIN) on PLAITS, JUNO,
  DAISYSP, DSP SAMPLE and the note machines; a user machine
  declares `pitch: none` for a fixed-pitch drum (the host sends 60).
- **Defaults (AUTO):** PLAITS 4, JUNO 6, DAISYSP 4, MD 1 (mono only: a drum), DSP SAMPLE / GRAIN 1 (mono only: one card stream a
  voice), 6-OP FM 6, WAVETABLE 8, GRANULAR 6 (their pools' counts); MULTISAMPLE's pool holds 16 a voice, and AUTO
  leaves it at 16 as before E0 (the table's 8 is only the most VOICES can set) [code: inst.cpp]; a user machine 1 when it is a drum,
  else min(4, its max).
- **CPU and RAM:** per instance the bench's mean % of a Pi 4 core (PLAITS 0.73, JUNO 0.54, DAISYSP 0.97)
  [measured, docs/pi-engine.md]; the budget check holds a voice's instances and its own machine under 20 % of a core
  (80 % of the guard's 25 %) at the worst warm block, 1.8 % [measured] (every built-in fits 8); a user machine at 2.5 x
  its declared mean [inferred], converted from its Teensy permille (permille x 0.158 % of a Pi 4 core [inferred from
  PLAITS' two measured figures]). One instance holds 35 KB (its engines' state and its 16 KB Plaits scratch; heap at boot);
  the 32 user-machine instance states 64 KB each (um's state limit, heap at boot). STATUS (touchscreen) shows
  `PLAITS x4` on the voice's row and `INST 12/32 0.61% 35K` on the VOICES panel's title (the instances in use, one's
  measured cost, its RAM); the console's `inst`, /status `"inst"` per voice: path, instances, notes, declared and
  measured cost, RAM, steals.
- **User machines (RMPA):** the manifest's `max_voices` (1..8) and `pitch` (`note` / `none`) become the header's flags
  (`rmp_format.h` RMP_HF_*; the header is outside the package's CRC); `max_voices` > 1 sets RMP_HF_INSTANCES, the
  opt-in (the plugin promises its state holds everything render() writes). Old packages (flags 0) stay mono, and an
  older host ignores the flags: backward compatible both ways. `um::inst_*` gives each instance its own state of the
  slot's package (voice_init / voice_enter on first use, voice_leave when the slot's image changes), with the voice's
  params as its own state gets them (presets, offsets).
- **Console:** `seq=poly:T` (show), `seq=poly:T:auto`, `seq=poly:T:voices=4,alloc=rotate,unison=2,detune=30,spread=64`
  (any of the keys, or their first letters); the answer names what it resolves to: `-> PLAITS: 8 instances, 4 notes at
  once (5.84 % a core)`. `inst` (per voice), `inst=on:V:NOTE:VEL[:VOICES[:UNISON[:ALLOC]]]` / `inst=off:V:NOTE|all`
  (tests). **Web page:** Sound -> Poly tracks (a row per seq track: VOICES, ALLOCATION, UNISON, DETUNE, SPREAD, Set;
  the instances line). **Touchscreen:** the SEQ page's POLY tool (VOICES, ALLOCATION, UNISON keys; the info line what
  it resolves to).

### E1: the seeded RNG and the step map

- `core/seq/rng.h`: `rng_key(seed, track, step, loop, purpose, k)` - a hash (splitmix64's finaliser), so a step planned
  twice (a replan after an edit, a predicted switch, a second PLAY with a fixed seed, a reload) gets the same answer;
  each decision names its purpose (kRngProb, kRngNote, ...: never renumbered). The value for (41, 0, 5, 3, prob) is
  pinned in the test: a change of the hash changes every saved seed's music.
- **The seed:** per pattern (FREE by default: a new one at each PLAY, kept through pauses and switches; or fixed
  0..9999, saved), a track may have its own; "same every loop" leaves the loop index out. Console: `seq=seed:free`,
  `seq=seed:41`, `seq=seed:hold:N` (FREE held, for A/B listening and the tests), `seq=tseed:T:N|follow|same:0|1`.
- **Probability** now draws from it (`cond_pass_i(.., draw)`): with a fixed seed the same coins every PLAY, and a
  replan never flips a coin [measured: test_seqx, 4611 replans in 4 bars, the same notes; saved, project reloaded,
  the same notes].
- **The step map** `step_for(len, abs) -> {index, loop}` replaces `abs % len` in the planner, the playhead, the
  recorder and the conditions' loop count; forward only, identical to the old arithmetic for every length 1..128 and
  count [measured: test_seqx, exhaustive to 4 x 128 + 3; test_seq unchanged and green].

### E2: .sqp v4

- v4 = v3 + an optional trailer before END: `XB <u16 count>`, per block `<u8 id> <u8 flags> <u16 len> <payload>`
  (flags: 1 required, 2 per-step, 4 a track's: payload[0] its Rytm track). Known blocks: 1 the pattern's seed, 2 a
  track's seed, 3 a track's poly config. Written only when there is a block; a pattern without stays v2 / v3, byte for
  byte as before.
- A block this build does not know is kept verbatim (1 KB a pattern) and written back; a per-step one is dropped
  (counted: `pack_dropped`) when its track's steps changed since the load (a crc of the step section), a track's one
  when its track is gone; a **required** unknown block locks the file as a newer Sidecar's (`unpack_error`
  kUnpackNewer); a known block shorter than its payload is damage.
- The registry (`core/fmt/formats.cpp`): `sqp` current 4, read natively from 4, the chain 1 -> 2 -> 3 -> 4 (each pure on
  bytes: the v2 writer's body, the version byte); an older Sidecar's entry (v3) refuses v4 as kNewer (locked) [measured:
  test_fmt]. Golden files v4/p002.sqp (a fixed seed, a track's seed and poly config, an unknown block) and the expected
  upgrades of v1..v3; the fuzz target takes them. `sim/protocol.py` sqp_pack / sqp_unpack mirror it. Bundles warn only
  above v4 now.

### Review (a separate reviewer pass) and what stays as it is

Fixed after the review: a switch from PLAITS / DAISYSP to a note machine now silences the instances (the engine
signature had no MULTISAMPLE / WAVETABLE / GRANULAR); an instance's engine state is re-made only when another voice or
machine used it last (`last`), not after every tail went quiet - no Plaits re-init per note on the control core; a
user machine's instance states leave (voice_leave) when the voice's instances are dropped; a JUNO PAT change reaches a
reused instance; `seq=seed:free` clears the seed (no v4 file for nothing); the Rytm's 0x11 no longer ends the
sequencer's mono note; a seq track's pad on a mono machine ends at its release; a v4 unknown block too big to keep is
counted and logged; `pack` with a short buffer writes nothing past it; tracks.txt's poly lines parse clamped,
malformed ones skipped [test_seqx]. Left as it is, said here:

- **Existing seq tracks on JUNO / PLAITS / user machines** (only reachable before through the console's
  `seq=track:T:on`): their Rytm trigs no longer sound, and AUTO gives them instances (PLAITS 4, JUNO 6). Intended (E0).
- A note planned from the **predicted next pattern** takes the poly config of the pattern playing at its note-on; at a
  switch between two patterns with different configs the first notes may use the old one (one reconfiguration).
- A user machine's instance note counts as a trig for its preset hold / lock logic (`pv_params(.., true, ..)`), as the
  voice's own trig does.
- The 32 user-machine instance states (64 KB each, 2 MB) are taken from the heap at boot, a user machine or not.
- The registry's v3 -> v4 upgrade relabels the version byte; no caller writes upgraded bytes back (the card's path is
  `seq::unpack` / `pack`, which keep the lowest version).

### Tests

`core/tests/test_inst_alloc.cpp` (declarations, the budget, every allocation mode, mono retrigger, steal order, UNISON,
the config's words), `core/tests/test_inst.cpp` (the engines: PLAITS / JUNO / DAISYSP instances, MD mono, the mono
path, DSP SAMPLE mono, FM's cap, machine change and CC 120, the shared pool full, no card time on the control core),
`core/tests/test_seqx.cpp` (the RNG and step map, the v4 file and unknown blocks, the planner's repeatable coins, a
PLAITS poly track through the sequencer: a MAJ7 on VOICES 3 plays 3 instances at once, the config in v4 and
tracks.txt), `sim/tests/test_core_um.py test_instances` (a real RMPA package with the flag: two sines at once on one
voice; without it: mono), `tools/tests/test_rmp_pack.py` (the flags), `pi/engine/tests/page_poly.test.mjs` (the web
rows), `pi/ui/host` shots (POLY panel and STATUS, both sizes, text-fit).


## E0 on the Rytm: POLY CONFIG in the track menu, the make prompt on every Sidecar machine (polycfg + rytm-mods rui1048, 2026-10-22, emulator only)

docs/SEQ-IDEAS.md E0's Rytm side. rytm-mods 0026 design.md "1048" has the firmware's half and its research.

### Where it lives: the Rytm's own track menu

The user's preferred place: the stock **track menu (FUNC + CHRO)**, where a Rytm user finds RELOAD / SAVE TRACK. Its
last item is now **POLY CONFIG** (after TRIGS SETUP, PAD SCALE, RELOAD TRACK, SAVE TRACK and the U1..U5 slot items;
FUNC + DOWN moves five rows). The research [measured in rytm-emu, rytm-mods design.md 1048]: the menu's items are built
once at boot by its constructor, from four std::function objects each, as two mods already add theirs (0012's SLICE
LOOP, 0021's U1..U5); its constructor has one caller, so rui1048 appends after that call - the stock items and the other
mods' stay as they are (`rled_emu.py menustock`: every earlier item and its YES as before, the stock four as on stock
1.73). The fallbacks (the poly track's YES menu, the SIDECAR MENU only) were not needed; the SIDECAR MENU lists POLY
CONFIG too (after SEQ: the Rytm's selected track).

YES on POLY CONFIG with a Sidecar that has drawn: the menu closes and the Rytm opens the page on **30 page 0x20 + t**
(`kPagePoly`, `sim/protocol.py RUI_PAGE_POLY`); no Sidecar: the popup NO SIDECAR, the menu stays. The message names a
device-neutral page ("POLY CONFIG of track t"); the Rytm specifics are the device table's row `poly` (`FUNC+CHRO`,
core/ui/page_keys.h). An older host opens its SIDECAR page instead.

### The POLY CONFIG page (core/ui/page_poly.cpp)

| knob | what | values |
|---|---|---|
| A VOIC | VOICES | AUTO (the machine's default), 1..8 |
| B ALLC | ALLOCATION | REASSIGN (REASGN) / ROTATE / RESET |
| C UNIS | UNISON | OFF, 2..8 |
| D DTUN | DETUNE | 0..127 (FUNC: by 8) |
| E SPRD | SPREAD | 0..127 (FUNC: by 8) |
| F GRP | GROUP | OFF, 1..4: stored, not played yet (POLYPHONY.md 7) |

- Each turn is the console's `seq=poly:T:<key>=<value>` (`seq::command`): the pattern's config and the track's default
  in `tracks.txt`, exactly as the console, the web page and the touchscreen's POLY tool. No core/seq change.
- **The info line** under the title is what the config resolves to now (inst::resolve_for, describe_short's figures):
  `4 NOTES 4 INST 2.92%` (+ ` CUT` when the budget or the machine's maximum cut it); the machine is left out (the Rytm's
  line holds 25 characters; the touchscreen's says `PLAITS 4 INST ...`).
- The machines with their own pool (6-OP FM, MULTISAMPLE, WAVETABLE, GRANULAR) take VOICES only: B..E read `-` and say
  `FM: VOICES ONLY` when turned.
- A Sidecar track that is **no poly track yet**: the config shown, `PLAITS: NOT A POLY TRACK`, **YES makes it one**
  (`seq=track:T:on`, the make prompt's YES); nothing is edited before (no pattern track made for it).
- An **analog track**: `BD PLAYS ANALOG: / THE RYTM'S VOICE IS MONO / FOR A POLY TRACK PICK A / SIDECAR MACHINE ON
  SRC` (E0b FREEZE's future place: "YES: FREEZE TO POLY", not built).
- **NO**: back to the stock screen when the track menu opened it (one level up is the menu's place); from the SIDECAR
  MENU, back to SIDECAR as every page. Off the LEFT / RIGHT ring. The track: the Rytm's request's, until the Rytm
  selects another (then that one, as INSERT follows the selection).

### The make prompt on every Sidecar machine

UP + TRIG (TRK + TRIG) on a track that is no poly track asks **MAKE BD A POLY TRACK?** for every Sidecar machine
(PLAITS, JUNO, DAISYSP, DSP SAMPLE, 6-OP FM, MULTISAMPLE, WAVETABLE, GRANULAR, user machines: `can_seq` since
E0). Its third line now says the machine and its notes at once from its poly config (`PLAITS, 4 NOTES`; AUTO: the
machine's default), its fourth where POLY CONFIG is (`FUNC+CHRO: POLY CONFIG`, from the device table). The explain
prompt (kPromptExplain) is reached only on an analog track now: its old reasons ("MONO IN THIS SIDECAR.", "A MONO
SIDECAR MACHINE.", "POLY TRACKS NEED 6-OP FM, MULTISAMPLE, WAVETABLE OR GRANULAR") are gone; it says the analog voice
is mono and where to pick a Sidecar machine, with FREEZE's place marked in the code. The SEQ page is in the page list
for a track on any Sidecar machine (was: the four pool machines only).

### Tests

`core/tests/test_poly_page.cpp` (the page from 30 page 0x21 on SD / PLAITS: not a poly track, YES, each knob as
seq=poly with the info line, clamping, 6-OP FM VOICES only, GROUP stored, an analog track, NO -> 3A 0, the SIDECAR
MENU's entry on the selected track and NO -> SIDECAR; the make prompt on all nine built-in machine kinds and the analog
explain; renders `core/build/rui_pages/poly_*`, `make_*`), `core/tests/test_ui.cpp` (the MENU's one more row),
`sim/rui_e2e.py polycfg` (rui1048 + the real core: BD to PLAITS, FUNC + CHRO, DOWN x 9, YES -> 30 page 0x20, the core's
POLY CONFIG page, YES makes BD a poly track, knob A x 3 -> VOICES 3, NO -> the stock screen) and `polymake` (UP + TRIG
on each machine kind and on the analog BD); rytm-mods `bk_emu.py polymenu`, `rled_emu.py menustock`.

## Built: seqtop - the top 8 of SEQ-IDEAS (2026-10-22, host only; docs/SEQ-IDEAS.md section 7)

Host side only: no rytm-mods change and no new Rytm-screen page. Every feature is on the console, the web page (Sound ->
Sequencer) and the touchscreen's SEQ page; the Rytm's keys and pages for them are listed at the end ("For the Rytm
side"). The open questions 2, 3, 5..10 of SEQ-IDEAS section 8 are decided (the doc's recommendations, user 2026-10-22).
Each feature is one commit, its tests green at it.

### P2: per-track speed ratios

- A seq track's own speed, a ratio of the Rytm track's step clock (which already runs at the Rytm track's SCALE):
  FOLLOW (1:1, as before), Elektron's 1/8 1/4 1/2 3/4 3/2 2 and the free 2/3 5/4 7/8 (Q8: a plain turn walks
  Elektron's set, FUNC every ratio, by size: `speed_step`).
- The planner counts the track's own steps: Sidecar step v starts at the Rytm position v x den / num of the track's
  phase-locked clock; lengths, micro offsets and swing scale with the track's step. The switch prediction plans the
  next pattern at its own speed from the predicted switch. The playhead, the LEDs, the recorder and the step preview
  follow. A:B and 1ST count the track's own loops (Elektron's per-track scale does the same); the pattern switch
  stays the Rytm's.
- 2x at 300 BPM (two Sidecar steps between two Rytm ticks): nothing late but the first step after PLAY [measured:
  test_seqtop]. Console `seq=spd:T:R`.

### P1: play direction per track

- FWD REV PEND (the ends once; a loop is 2 len - 2) PING (the ends twice; 2 len) RND (a keyed draw per position) BROWN
  (a walk from step 1 each loop: +1 50 %, 0 15 %, -1 20 %, +2 15 %), in `core/seq/rng.h step_for` (pure). RND / BROWN
  draw from E1's keyed RNG (kRngDir), so a replan, a predicted switch and a second PLAY with a fixed seed play the same
  order; FREE re-rolls at PLAY; "same every loop" repeats one order. The time grid and the switch point stay
  (constraint 1).
- **Swing under a direction follows the tick** (Q3): every second tick late, so the groove stays; FWD keeps the step's
  own parity, as before. Console `seq=dir:T:D`.

### M1: scale and key per pattern

- KEY C..B, SCALE CHR MAJ MIN DOR PENT PHR LYD MIX LOC HMIN MMIN PMAJ BLUES WHOLE (the first five the touchscreen KEY
  tool's), QNT OFF / EDIT / PLAY. OFF: an edit aid only (the roll's rows, R1's random notes keep to it). EDIT: the NOTE
  knob under a held trig moves by scale degree (FUNC: by semitone) and CHD / INV build diatonic chords (CHD MAJ on
  degree 2 of C minor is D dim; 7 on degree 5 of C major is G7). PLAY: also every note quantised when it plays (a tie
  goes down); the written notes stay.
- `core/seq/music.cpp` (pure): the masks, `quantise`, `degree_move`, `diatonic_chord`, `set_chord_in`. Console
  `seq=key:K:S[:Q]`, `seq=key:off`, `seq=qnt:Q`, `seq=quantise:T` (a track's written notes moved into the scale; two on
  one pitch kept once).

### R1 + R1b: random notes in the scale from the seed; the modulatable SEED

- A track's RND mode OFF / STEPS (the steps flagged random) / TRACK (every step with notes: pitches drawn; rhythm,
  velocity, length, micro kept). A step's n notes are drawn without repeats from LOW .. LOW + 12 x OCT (1..4) in the
  pattern's scale (CHR without one), weighted FLAT / ROOT (the key 3x) / CHORD (the step's CHD, else the key's triad,
  3x) / LOW / HIGH (a slope) / STEP (near the written note: the stateless reading of "small intervals"). The draw is
  E1's keyed RNG (kRngNote; seed, track, step, loop), so replans, predicted switches, a second PLAY with a fixed seed and
  a reload play the same notes [measured: test_seqtop, and the `top` e2e on the emulator: two PLAYs, the same notes].
- PRINT (`seq=print:T[:LOOP]`): a chosen loop's notes (default the loop playing, else the first) written into the steps,
  the flags / TRACK mode off.
- **SEED MOD (R1b)**: the seed + a whole offset 0..RANGE-1 (RANGE 2..64, default 16) from a machine param of the
  track's voice (`src` P1..P16) **as it plays** - after the Rytm's scene / PERF locks and the Sidecar's LFOs, so a scene
  lock, a PERF assignment or an LFO on that param moves the seed with no mod change (Q9: a slot the machine leaves
  free; 6-OP FM's P9..P12 are). Sampled when the QUANT unit begins - STEP / LOOP (default, Q10) / BAR (16 steps) - with
  hysteresis (+-0.25 of a seed) at each boundary, and kept per (play segment, pattern, unit), so a replan reuses the
  offset the step got. AFFECTS NOTES (R1, FX RND / RNV, the RND arp) / +CHANCE (probabilities, lock and CHN chances) /
  +ORDER (RND / BROWN, random jumps: sampled per loop) / ALL.
- Console `seq=rnd:T:mode=..,low=..,oct=..,wght=..`, `seq=rndstep:T:S:0|1` (sets STEPS when RND was OFF),
  `seq=print:T[:L]`, `seq=smod:T:src=..,range=..,quant=..,aff=..`, `seq=x:T` (a track's seqtop settings, the
  effective seed `41+3`).

### V1: conditions and probability per lock

- `Lock::pad` is the lock's own condition: the COND knob's codes without PRE / NEI (a lock never sets or reads them):
  FILL / !FILL, 1ST / !1ST, A:B on the step's loop, 1..99 % from E1's keyed RNG (kRngLock, the param as k). A lock whose
  condition fails is left out of its step's set: its param back to its base on that step, as a step that does not lock
  it. The step's own condition decides first. A new lock is unconditional; a lock's new value keeps its condition.
- Console `seq=lcond:T:S:P:C` (P as `seq=lock`, or `all`: every lock of the step; C `---` `fill` `!fill` `1st` `!1st`
  `A:B` `N%`).

### P4 + M4: chord-aware ratchets / rolls and the per-step arp

- A step's RTRG: CNT 2..16 repeats within its longest note (RATE AUTO) or at 2 1 2/3 1/2 1/3 1/4 1/6 1/8 steps, GATE %
  of the interval, the velocity along VCRV FLAT / UP / DOWN / UPDN, the pitch moved PCRV per repeat (semitones; degrees
  with the scale and QNT on). CHORD (the whole chord each repeat), WALK (the repeats walk the chord: a strummed
  ratchet), and M4's arps UP DOWN UPDN RND PLAYED CONV over OCT 1..4 octaves (CNT 0: one pass).
- Expanded when the step is planned (`rt_expand`, pure), after R1's pitches and before M1's QNT PLAY. The event queue
  holds 1024 now (512 before): 8 notes x 16 repeats at 300 BPM drop nothing [measured]. Console `seq=rtrg:T:S:K=V,..`
  / `off`.

### X1: parameter slides between locks

- A step's lock of a param can slide (`Step::slide`, a bit per param) to the param's next lock: the next step in play
  order (direction and jumps included) that locks it, less than a loop on, reached at that step's start; without one it
  is a plain lock. A predicted switch before the target: no slide.
- The ramp runs on the control side in `pre_block` every 4 render blocks (1 ms), re-applying the track's lock set
  through the lock path (machine P1..P16 under the LFOs, FX P2..P5, INSERT params). A trig between the slide and its
  target does not cut it; the target's own lock, STOP and the next slide do. Removing a lock takes its slide. Console
  `seq=slide:T:S:P|all:0|1`.

### H1 + V3: the tracker-style FX column with its jumps

- Two columns per step. Plan-time: RTG n, DLY n (the whole step n/96 late, its locks too), CUT n, CHN p (this column's
  chance for the other's command), RND r, RNV r, OCT n, TRS n, FIL p (with FILL held, page p's step plays instead).
  Order (V3, through the step map): JMP n, JRN w (+-w, 0 anywhere), LOP NxX, SKP n, DIR d (to the loop's end).
- The walk (`walk_step`) is a pure function of the track, the seed and the count: it begins again at each loop's start
  and follows the direction's own rules between commands, so a track without order commands maps exactly as
  `step_for` [measured: every direction]; a cached walk makes the planner's next count one step of work. Random
  commands draw from kRngFx / kRngJump. Swing under jumps follows the tick. Console `seq=fx:T:S:C:CMD:V` / `off`.
- **Not built:** SPD (a speed change mid-pattern: the track's clock would have to be piecewise).

### The file: .sqp v4 blocks 4..12

| id | block | payload | flags |
|---|---|---|---|
| 4 | a track's speed (P2) | track, speed id | track, required |
| 5 | a track's direction (P1) | track, direction id | track, required |
| 6 | the pattern's key / scale (M1) | key, scale, QNT | required only with QNT PLAY |
| 7 | a track's random notes (R1) | track, mode, low, oct, weight, 16 bytes of step flags | track, per-step, required |
| 8 | a track's SEED MOD (R1b) | track, src, range, quant, affects | track, required |
| 9 | conditional locks (V1) | track, {step, param, cond}.. | track, per-step, required |
| 10 | ratchets / arps (P4 / M4) | track, {step, cnt, rate, vcrv, pcrv, mode, oct, gate}.. | track, per-step, required |
| 11 | sliding locks (X1) | track, {step, param}.. | track, per-step, required |
| 12 | FX columns (H1) | track, {step, cmd1, val1, cmd2, val2}.. (any step: a jump on an empty one) | track, per-step, required |

Written only when used (a pattern at the defaults stays v2 / v3 byte for byte, tested per block); required, so a seqx
Sidecar (v4 without them) locks such a file as a newer Sidecar's (Q1's rule) instead of playing it without them; a
value this build does not know (a speed / direction / scale / mode / command id) also locks it. `sim/protocol.py`
mirrors every block; golden `core/tests/golden/sqp/v4/p003.sqp` carries all of them (read and written back byte for
byte by the core: test_fmt; the fuzz target takes it). No registry change: v4 reads 1..4 as before.

### The UI (host side)

- **Console:** the commands above; `seq=played[:T]` (the last notes started, `v/a:step:pitch`; the e2e's view of the
  order). The model's own commands live in `core/seq/xcmd.cpp` (no clock, no card): core 3 applies them and replans,
  and the touchscreen's host render applies the same lines.
- **Web page** (Sound -> Sequencer): the pattern's KEY / SCALE / QNT / SEED (FREE); per seq track SPD, DIR, RND (mode,
  low, octaves, weight), SEED MOD (P, range, quant, affects, the offset now); a step editor (track, step: RTRG, FX 1 /
  2, random notes, a lock's condition and slide), filled from /status `"seqx"` (`seqx_json`).
- **Touchscreen** (SEQ page): **KEY** is the pattern's real key now (it follows a key set elsewhere), with QNT and the
  seed (FREE / FIX, - / +). **PLAY** (new): speed, direction, random notes (800x480: also octaves, weight, PRINT).
  **STEP** (new): the selected note's step - ratchet 2 3 4 8 or arp UP / UPDN / RND, FX 1 (OCT+ OCT- RND2 JMP1 SKP1),
  random notes, its locks' condition (--- 50% 1:2 FILL) and slide. The velocity lane marks a step's extras (R F ~ ? /).

### Decisions (SEQ-IDEAS section 8, 2026-10-22)

Q2: performance gestures use UP + trigs only in seq mode, never a key the UP map already uses (none of the top 8 is
one; P3 / F4 / F1 follow it). Q3: swing under REV / RND follows the tick. Q5: mutate playback-only, LOCK bakes it (V5
not built). Q6: the FX column on the Rytm's TRIG page first; the host data model and the touchscreen / web editing
now; a full tracker view later. Q7: the A4 later uses the Sidecar's arp and slides. Q8: Elektron's ratios plus 2/3,
5/4, 7/8 on FUNC. Q9: SEED from scenes / PERF through a machine slot (SEED MOD), no mod change. Q10: SEED QUANT LOOP.

### Behaviour changes

- **A replan starts at the first step that has not started** (all tracks, FOLLOW included): before, a replan between a
  step's start and its tick's arrival (the tick comes a round trip late) planned that step again, a second, late note.
- **A trig turned off clears the whole step** (its locks as before, now its ratchet, FX, random flag and slides too).
- **The touchscreen's KEY tool is the pattern's key** (it was a display preference): it shows what the pattern has
  (CHR by default, so a new pattern's roll is no longer lit as C minor) and a tap sets it. The OLED tool row names its
  keys short (CHRD STRM HUM SWG) to hold nine.
- The event queue is 1024 events (twice the RAM: ~40 KB).

### The planner's cost

[measured: test_seqtop on the Mac (M-series); a Pi 4 core is about 3-4x slower, inferred] A plain track: 10.6 us a
step (pre_block's whole share); every seqtop feature on (BROWN with jumps, MAJ7 chords drawn at random, ratchets and
arps, 50 % lock conditions, slides on every lock, STEP-quantised SEED MOD): 44 us a step, the longest block 27 us
(a Pi 4: ~0.1 ms, 5 % of a 2 ms block, the first planning burst after PLAY).

### For the Rytm side

Built in rui1050 (below: "Built: rui1050"), with the deviations listed there. The keys are checked against the UP map (UP + T1..T13, UP + TRIG / SRC / LFO / FX / YES / AMP, UP + REC /
STOP / PLAY with a held trig stay as they are); none of them needs UP.

- **TRIG view, no trig held** (today A NOTE B VEL C LEN D SWING): **E DIR**, **F SPD** (FUNC: the free ratios), G LEN.
  TRIG again cycles the track's pages: DEFAULTS -> **SCALE** (A KEY, B SCL, C QNT) -> **RANDOM** (A SEED: fully down
  FREE, FUNC +-100; B RANGE (OCT), C WGHT, D MODE, E SRNG, F SQNT, G SAFF). Footer `DIR PEND`, `SEED 41+3 FIX`.
- **TRIG view, trigs held**: TRIG cycles NOTES -> TRIG -> **RTRG** (A RATE, B CNT, C VCRV, D PCRV, E MODE (CHORD WALK
  and the arps), F OCT, G GATE) -> **FX** (A CMD1, B VAL1, C CMD2, D VAL2). The NOTE knob's value **RND** below the
  lowest note: the step's random flag.
- **Lock pages** (held trig + SRC / SMPL / FX): a **long encoder push** switches that knob to its COND (`1:4`, `30%`,
  the label gets a `?`); **FUNC + encoder push** toggles its SLIDE. The bottom bar `S5 LEV:34 1:4`.
- **The poly track's menu** (FUNC + DOWN on its SEQ page since updown1029; DOWN ux1027; YES before): PLAY MODE (the direction), PRINT RND (YES prints the loop playing).
- **SEED MOD**: the poly machines' free slot named SEED on their SMPL page (a convention for user machines too).
- Mod-side state the host needs: none new (the 2C ticks, 2D 0C FILL and the voice reports carry it).

### Review (a separate reviewer pass) and what stays as it is

Fixed after the review [each a case in test_seqtop `test_review`]: a slide's target was searched with a walk per
sliding lock (BROWN, 128 steps, 12 sliding locks: 643 us in one block on the Mac) - one walk ahead per planned step now,
BROWN walks through the cached step map, and "has order commands" is cached per track (0.8 us); SEED MOD with QUANT
STEP and AFFECTS ORDER re-sampled a loop's order mid-loop (the loop offsets shared an 8-entry ring with the steps') -
own ring now; JRN and the walk's CHN drew the same on every visit to a step in a loop (keyed by the step: a loop could
cycle 2-3-4) - keyed by the position in the loop; two overlapping LOPs never finished (one counter) - a counter per LOP;
a step delayed by DLY (or swing) past the next step's start was cut by a replan and lost - events carry their step's
grid frame; the web step editor cleared a lock's condition on any Set and refused DIR / LOP values - it shows them as
the console takes them and sends a lock's condition / slide only when changed. Also: an 8-note chord x 16 repeats plays
every repeat (128 notes; 64 before), a track turned off ends its slides, SEED MOD's sampled offsets go when its setting
changes, a file's out-of-range values lock it instead of being clamped, a slide flag of a param the step does not lock
is not loaded, the key alone (CHR, no QNT) writes no block unless R1's ROOT weight uses it, the NOTE knob's FUNC on CHR
moves an octave again, the Pi's /status buffer for "seqx" is 24 KB.

Left as it is, said here: a slide toward a target whose step then does not play (its condition, mute) holds the target
value; a step whose early (negative micro) note started before a replan plays that note again; a just-recorded note on
a ratcheted step sounds again in its first pass; PRINT uses the current SEED MOD offset, not the one each printed step
used under STEP quant; a long-rate ratchet (2 steps x 16) schedules notes far ahead (they outlive a switch); the
touchscreen hard-codes three COND codes (50 %, 1:2, FILL).

### Left open / not built

SPD in the FX column; a per-track scale override (R1 uses the pattern's); a dedicated LFO target SQn SEED (the SEED
MOD slot takes any LFO on that param); M2 live transpose; the step preview plays a ratcheted step's plain notes; PRINT
is not an undo level of its own; the tracker view; a slide's ramp drawn on the touchscreen (the lane marks it `/`).

### Tests

`core/tests/test_seqtop.cpp` (each feature through the real planner with simulated Rytm ticks: the positions, the
pitches, replans every 7 blocks, a second PLAY, a predicted switch across speeds, each block's bytes and the defaults'
byte identity, unknown values locking the file, the cost), `core/tests/test_fmt.cpp` (golden p003 read and written
back), `sim/tests/test_protocol_seq.py test_seqtop_blocks` (the mirror), `pi/engine/tests/page_seqx.test.mjs` (the web
section), `pi/ui/host` shots (`seq-key-qnt`, `seq-play`, `seq-step`: every new key tapped, the model following, both
themes and sizes, text-fit), `sim/seq_e2e.py top` on rytm-emu (rui1045, DTEMU_STRICT=1: two PLAYs, the same order, REV
at speed 2, the ratchet, the octave, the random note in C minor, the file's blocks read back).

## Built: rui1050 - the Rytm side of seqtop (seqrytm + rytm-mods rui1050, 2026-10-24, emulator only)

The plan of "For the Rytm side" above, on the Rytm's keys and screen. The pages are the host's (core/seq/seq.cpp ui_key /
ui_enc, core/ui/page_seq.cpp); the Rytm build adds UP's map over an open page and names SEED on two machines.

### How to use it (on the Rytm, a poly track in GRID REC)

| where | keys | what |
|---|---|---|
| SEQ page, no trig held | TRIG | cycles NOTES -> **DEFAULTS** -> **SCALE** -> **RANDOM** -> NOTES |
| DEFAULTS (`SEQ TRIG`) | A NOTE B VEL C LEN D SWNG (the new steps', as rui1040), **E DIR**, **F SPD** (FUNC: also 2/3 5/4 7/8), **G TLEN** (the track's length, as the notes view's C) | footer `DIR PEND  SPD 3/2` |
| SCALE (`SEQ SCALE`) | A KEY, B SCL, C QNT (the pattern's) | footer `KEY C MIN QNT EDIT` |
| RANDOM (`SEQ RANDOM`) | A SEED (fully down FREE; FUNC +-100), B RNG (octaves), C WGHT, D MODE (OFF STEPS TRACK), E SRNG (SEED MOD's range; fully down OFF; up from OFF takes the machine's SEED slot), F SQNT, G SAFF | footer `SEED 41+3 FIX` / `SEED FREE` |
| trigs held | TRIG | cycles NOTES -> **TRIG** -> **RTRG** -> **FX** -> NOTES (from a lock page: TRIG). A hold on the notes view starts on the notes; on a track page on its step page (TRIG the first time) |
| TRIG (`SEQ TRIG STEP`) | A NOTE (turned below the lowest note - C-1, or the scale's lowest degree with QNT EDIT - shows **RND**: the step's random flag; up again: the written notes), B VEL C LEN D MICR E COND F STRM G CHD H INV | |
| RTRG (`SEQ RTRG STEP`) | A RATE, B CNT (OFF, 2..16), C VCRV, D PCRV, E MODE (CHORD WALK UP DOWN UPDN RND PLAYED CONV), F OCT, G GATE | |
| FX (`SEQ FX STEP`) | A CMD1, B VAL1, C CMD2, D VAL2 (a new command starts on its own value; LOP's VAL: N, FUNC: X) | |
| lock pages (trig held + SRC / SMPL / FX) | turn: the lock (as before); push: clears it (now at the release); **long push** (0.4 s): that knob edits its lock's **COND** (label `PAT?`, a turn steps --- FILL !FILL 1ST !1ST the A:Bs 1..99 %, another long push: the value again); **FUNC + push**: its **SLIDE** on / off | footer `S5 LEV:34 1:4 SLD`; a lock with a condition has a dot top left, a slide a tick top right |
| the poly track's menu (FUNC + DOWN on its SEQ page, updown1029; DOWN ux1027; YES before) | rows BACK, TURN OFF, LENGTH, QUANTISE, **PLAY MODE: <dir>** (YES / RIGHT the next, LEFT the one before; the menu stays), **PRINT RND (THIS LOOP)** (YES: seq=print, the loop playing) | footer `PRINTED: 3 STEPS` / `NO RANDOM NOTES TO PRINT` |
| any open Sidecar page | UP held alone 0.4 s | UP's map (the KEYS page) over the page; the page again at the release (rui1050's 49 1 5) |

### SEED MOD's slot

`seq::seed_slot_of`: DAISYSP P11 (its SMPL B, named SEED by rytm-mods 0021 rui1050, was FB), DSP SAMPLE P6 (its SRC F, named
SEED, was FB; the host's own DSP SAMPLE SRC view says SEED too), 6-OP FM and JUNO P12 (free, but the Rytm's SMPL page there
is stock's: the knob still reads BR). PLAITS, MULTISAMPLE, WAVETABLE, GRANULAR and user machines have none: E SRNG
says `NO SEED SLOT ON IT` (the console's `seq=smod:T:src=N` still takes any param). A user machine names a free knob SEED in
its manifest (the convention: then `seq=smod` with that P).

### Deviations from the plan

- **G is labelled TLEN** (the plan: LEN): C is already LEN (the new steps' note length); two LENs on one page read as the same.
- **SEED named on two machines only** (the plan: every poly machine): the others have no free param, or their SMPL page is
  stock's and 0021's page code has no room (rytm-mods design.md 1050).
- **A short push clears its lock at the release** (before: at the press), so a long push can be told from it.
- **FUNC is caught by the SEQ page** (both sides: stock keeps it), so a push knows FUNC. No mod message.
- The NOTE knob's RND is also on the notes view's held-step NOTE (the same knob).
- [measured, rytm-emu] The Rytm itself drops a turn of an encoder ~0.3 s after that encoder's push is let go (stock does the
  same): after the long push, wait a moment before turning to the condition.

### Behaviour changes

- TRIG without a held trig no longer toggles the notes / trig values: it cycles four pages (three presses more back to the
  notes). Under held trigs it cycles TRIG RTRG FX and the notes, and the view after the release stays the track's.
- The poly track menu has no title line (the header says POLY TRACK and the track) and six rows.
- UP held alone with a page open shows the map (rui1050 Rytm; an older Rytm: nothing, as before).

### Tests

core/tests/test_seq.cpp (the rui1050 block: every page and gesture through remote.cpp's 31 / 32 as the Rytm sends them, the
renders `core/build/seq/rui1050_*.png`, text-fit on), `sim/seq_e2e.py r50dir r50spd r50key r50seed r50rtrg r50fx r50cond
r50slide r50menu` (rui1050 on rytm-emu, the real core), `sim/rui_e2e.py upmapover`; rytm-mods `rled_emu.py upmappage
stocklockpush`. The two UP-map tests check the protocol, not the map's contents - that the Rytm sends `49 1 5 1` / `5 0`
around the hold, that a tap sends neither, that `UP + KEY` appears and goes, and that UP + **T3** still arrives as the
page request `3`. T3 is PAT before and after rui1052's reorder, so they need no change for it.

## Built: seq1112 - LENGTH grows, the pages copied (2026-10-25, host only)

The Rytm fills the pages a longer track length adds with a copy of the pattern as it stood: **1 -> 2 pages duplicates
page 1; 2 -> 4 copies pages 1-2 into pages 3-4.** The Sidecar's own seq tracks now do the same, and shortening still
only hides. No mod change (host only; the mod is unchanged at rui1051).

| part | where | what it does |
|---|---|---|
| the copy | `core/seq/pattern.cpp` `grow_pages(Track&, from_len, to_len)` | each step the growth reveals takes **step i % from_len**, as a whole `Step` assignment. That single rule gives the Rytm's two cases exactly (16 -> 32: steps 16..31 from 0..15; 32 -> 64: 32..47 from 0..15 and 48..63 from 16..31) and generalises to a part page (20 -> 32 fills 20..31 from steps 0..11) |
| what comes with it | the `Step` struct is the whole of a step | the notes (pitch, velocity, length, micro), the trig's on / mute bits and its **condition**, PROB, CHD / INV / strum, all 12 **p-locks with their values and their own conditions** (machine P1..P16, the send FX's, the INSERT's), the **slide** bitmask, the **ratchet / arp** (`Rtrg`), the two **FX columns** and the random-notes flag. Nothing is copied field by field, so nothing can be forgotten |
| what is not touched | per track and per pattern | `length` itself, the trig defaults, swing, speed, direction, the random and SEED settings, the poly config, the pattern's key / scale / QNT |
| shortening | nothing at all | `set_length` calls `grow_pages` only when the effective length grows. Every one of the 128 steps is always stored, so a shrink just moves the play limit: the steps stay in the pattern, are written to the card by `pack` (it walks every step index, not the length) and come back when the length grows again |
| growing back | `grow_pages` skips a page that already holds steps | a page the growth reveals is filled **only when every step it reveals is untouched** (`step_used`: a compare against a zero `Step`, so a condition, a ratchet or an FX column with no note counts too). A page hidden by an earlier shortening is therefore revealed as it was and never painted over |
| where | `core/seq/seq.cpp` `set_length(Track&, r, len)` | the one place a track's length is written: the SEQ page's C (and the TRIG DEFAULTS view's G TLEN), `seq=len:T:N` (the console, the web page, the sim and the Pi bridge) and the Rytm's 2D 04 SCALE report. The compare is on `eff_len` (AUTO resolved against the Rytm's own length), so leaving AUTO for the same number of steps copies nothing. A file load (`unpack`) and `seq=demo` write `length` directly and never copy |

### Behaviour changes

- **A longer length copies pages.** It did nothing before. A track at 16 steps taken to 32 now has two identical pages
  instead of a second empty one. The copy is of the pattern *as it stands at that moment*, so an edit made after the
  growth is page 2's alone.
- **A growth into pages that already hold steps copies nothing.** This is what makes "shortening hides" true in both
  directions: 32 -> 16 -> 32 gives page 2 back exactly, locks and conditions included, rather than a fresh duplicate of
  page 1.
- **An FX column's jump is copied verbatim**, as a page paste copies one: a `JMP 3` duplicated onto page 2 still points
  at step 3, not at step 19. That is the Rytm's own page-copy behaviour [inferred: not measured on hardware].
- A step with extras but no note or lock is still dropped by the file (`pack` skips `!n && !nl`), as before; `toggle()`
  clears the whole step, so that state is transient.

### Tests

`core/tests/test_seqtop.cpp` `test_grow_pages`: a page 1 carrying a note, a condition, PROB, a machine lock, an INSERT
lock and a send FX lock (each with its own lock condition), a slide, a ratchet, both FX columns, the random flag and
CHD / INV / strum; 1 -> 2 asserts page 2 is **byte-identical** to page 1 (`memcmp` over the whole `Step`), 2 -> 4
asserts pages 3-4 against 1-2; page 2 is then edited, shrunk away and grown back and every byte of it must return;
the hidden steps are asserted through a `pack` / `unpack` round trip; a part-page grow, a shrink and an unchanged
length are asserted to copy nothing; `grow_pages` itself is checked for the 0 / shrink / over-`kMaxSteps` corners.
`core/tests/test_seq.cpp` "seq1112": the same through the console with the Rytm's SEQ page open, rendered
(`core/build/seq/seq1112_1_one_page.png`, `_2_grown_page2.png`, `_3_shrunk_hidden.png`, `_4_grown_back.png`) - the
page LEDs and the roll read from the images, not from a return value.

### Left open / not built

- Nothing asks the Rytm what it did to **its own** track's pages on a SCALE change; the host copies on its own report
  of the new length. If a future measurement shows stock tiles differently for a non-doubling length, `grow_pages` is
  the one function to change.
- The master (pattern) length is still only mirrored, never stored, so a PATTERN-scale length change moves what every
  track plays without copying anything.


## rui1051 fixes (hardware reports on rui1050, 2026-10-24, emulator only)

- **A track's length while stopped.** A seq track that follows the Rytm's length (`length` 0) took it from the Rytm
  track's own length byte (2D 04 per track) until the clock's ticks said otherwise. On PATTERN scale (2D 04's pattern
  report, mode 0) every track plays the pattern's length, so a 32-step pattern showed one page while stopped (the trig
  LEDs, PAGE's pages, the touchscreen's grid) and two once playing. `eff_len` now takes the pattern's length on PATTERN
  scale [inferred: the Rytm's per-track byte on PATTERN scale is not measured]; `View::rytm_len` says it too.
  core/tests/test_seq.cpp "rui1051": PATTERN scale 32 with the track byte 16, stopped -> 2 pages, PAGE -> page 2, T3 ->
  step 19 lit, T1 dark.
- **The Rytm's edit page on the touchscreen.** `sidecar_seq_view_t::edit_page` (pi/engine/seqed_pi.cpp: the SEQ page's
  `View::page` when it shows that track): stopped, when it changes, the touch editor's window moves to hold it (playing,
  the window follows the play head as before). Render: `pi/ui/host/shots*/*-seq-stopped-page2.png`.
- **The trig LEDs after PAGE while stopped** (rytm-mods 0026 rui1051, design.md "1051"): a 34 whose paint missed the LED
  lock is owed until painted; `sim/seq_e2e.py pagestop` (page open and closed): T1, PAGE, T3 stopped -> steps 1 and 19,
  the trig LEDs page 2's.
- **Leaving GRID REC** (rui1051): stock's LED colours and selectors written back twice; `sim/seq_e2e.py recleds`.

## fix1051b: a track made a poly track on a used pattern took no trigs (hardware report on rui1051, 2026-10-25, host only)

Report: CH made a poly track (POLY CONFIG's YES, the machine WAVETABLE) on the pattern that was already being played
drew "EMPTY PAGE" on the SEQ page and its step keys added nothing, while BD - a poly track since the pattern loaded -
edited normally in the same session; reconnecting the Rytm did not help; on a new pattern the same conversion worked.
Two defects, both from the same split: **the seq tracks are the project's** (`tracks.txt`'s `mask`, `load_tracks`)
while **their steps are the pattern's** (its `kTracks` = 4 `Track` slots, `pattern.cpp track_of`).

- **Defect 1, the reported one: the slots were full.** `set_seq` set the mask bit and then asked for a slot; with all
  four taken it got none, so the track was *called* a poly track while `toggle` (a step key's tap) and the SEQ page's
  roll read a track that was not there - no trig, and the roll's "EMPTY PAGE" because there was nothing to draw. A
  pattern fills up through ordinary use: a track made poly there once keeps its slot after TURN OFF (by design - its
  steps are kept), the Rytm's own stock clipboard ops are mirrored onto whatever track they name (also by design,
  fix1042), and all of it is packed into `pNNN.sqp` (a slot with no steps is still a track record) and comes back on
  every load. That is why a used pattern was full and a new one was not. The machine is irrelevant - WAVETABLE was a
  coincidence; every kind behaves the same.
- **Defect 2, measured on the hardware (mask `0x717`, seven tracks, two tracks in the pattern).** The mask is the
  project's, so a track made poly while another pattern was loaded got its slot in *that* pattern only - and
  `set_seq`'s first line (`on == in_mask(r)`: already a seq track) meant it never asked again. In every other pattern
  it stayed a poly track with no steps and **no sound from either side**: it is pinned, and the Rytm's own trig on a
  seq track makes no note.
- **Fix.** `seq_track(r, create)` is the one allocator for a seq track's slot: with every slot taken it reclaims one
  held by a track that is no seq track and has no steps (that track's length and poly config go with it). `set_seq`
  takes the slot before it commits - no slot and no reclaim, and the mask bit is dropped again with the reason in the
  log, so the track stays a stock one instead of a poly track that cannot be edited. Making a track poly also marks the
  pattern dirty (its new, empty track is saved with it). `reconcile_mask`, at every pattern install, gives each seq
  track a slot in the pattern just loaded while one can be had (the ones that have one keep it, the lowest-numbered of
  the rest take what is free): an existing project's stale bits become real storage on the next load, which is the
  migration half. A track that fits nowhere keeps its bit (another pattern may hold its steps) and its SEQ page now
  says **NO TRACK IN THIS PATTERN** instead of EMPTY PAGE, so the state is never invisible again.
- The Pi's touch editor made a slot for any track it was asked about (`pi/engine/seqed_pi.cpp track_for`, while
  `seq_touch.cpp` already answers "T%d is not a seq track" for the rest): it now makes one only for a track in the mask.
- **Not changed:** the Rytm's stock clipboard ops still land on a track that is no seq track, and the SEQ page still
  edits the track it shows (both tested behaviour, fix1042). Those slots hold real steps, so the reclaim leaves them
  alone and the four-a-pattern limit applies as documented.
- **Superseded 2026-10-26 by "Poly tracks per pattern" below** (the user: the poly setting belongs to the pattern):
  the project-wide mask and `reconcile_mask` are gone; the bullets here describe the build they fixed.
- **Decided (2026-10-25): the mask is not capped at `kTracks`.** A project may legitimately mark more poly tracks than
  any one pattern can store (the hardware's seven), because different patterns may want different poly tracks; a cap
  would turn "4 poly tracks a pattern" into "4 a project". The excess is reconciled as far as it fits, logged, and
  shown on the page. `seq=track:T:off` (or the page's YES -> TURN OFF) clears a bit and un-silences that track on the
  Rytm. (A per-pattern mask was the third option and was not taken: it changes the on-card format.)
- **What the slot does NOT do** [measured on the hardware 2026-10-25, pattern B07, `mask 0x737 pinned 0x737 silent
  0x737`, slots held by tracks 1, 2, 3; sequencer stopped, pads pressed one at a time]. A slot is **step storage**. It
  does not gate a live pad: a pad press goes through the voice's machine and the instance allocator (`poly::seq_pad`,
  `inst_alloc.h`), which need no pattern slot. An earlier guess here - that a masked track with no slot is silent from
  both sides - was **wrong, and the device disproved it**. Measured, with 9 of 12 pads as that guess said and 3 against
  it:
  | pad | mask | slot | heard |
  |---|---|---|---|
  | CP MT HT CB | no | no | the Rytm's own sound (as expected: the host is not involved) |
  | BD | yes | yes | its WAVETABLE (the host plays it) |
  | BT | yes | **no** | **a CHOP sample** - a Sidecar machine with an instance sounds from a pad with no slot at all |
  | LT CH OH CY | yes | no | silent |
  | SD | yes | **yes** | **silent** - both halves present and no sound |
  | RS | yes | yes | **the Rytm's own voice**, although bit 2 is in `silent 0x737` |
  So what a pad is heard through is **the track's machine**, not its slot: a Sidecar machine sounds when the host has
  something to play on it, an analog machine sounds from the Rytm. `silent` (0x46) is not a mute - it asks the Rytm to
  close a **pinned** track's VCA after 400 ms with nothing sounding or planned, and it is cleared the moment a note is
  planned (`silence_tick`). The dead pads are therefore "the Sidecar had nothing to play on that voice", not "the mask
  silenced the track".
- **Open, from the same measurement** (neither is this fix's, and neither is explained): **SD** silent although it has
  a machine, a mask bit and a slot; and **RS** sounding from the Rytm although its bit is in `silent 0x737`. One guess
  for RS - that its SRC is an analog machine, which would make it expected rather than a bug - is **neither
  established nor refuted**; see the next bullet for why the obvious test did not settle it.
- **There is no host reading of a track's real SRC machine** [measured 2026-10-25]. `seq=poly:T`'s `-> MACHINE: ...`
  tail is **not** it: it comes from `inst::resolve_for` -> `inst::machine_now(vi)`, which reads the host engine's own
  per-voice flags and **falls through to `kPlaits`**, and has no analog case at all. On the device every one of the
  twelve tracks reported a tail, CP and MT among them (`-> PLAITS`) while both demonstrably played the Rytm's own
  sound and are not in the mask - so the tail cannot tell a Sidecar track from an analog one, and RS reporting PLAITS
  is no evidence either way. (That reading was also taken with the Rytm powered off and after this fix was installed,
  so its kit state may be stale as well.) The authoritative value is `ui::remote_track_machine(t)` - the voice byte of
  the Rytm's 0x33, `0x7F` = analog, `-1` = never reported - and **no console command and no `/status` field exposes
  it**; its only consumers are the UI pages. What would discriminate, all needing the Rytm powered on with the link up
  so a fresh 0x33 has arrived: the **POLY CONFIG page** of that track (`page_poly.cpp` draws "<T> PLAYS ANALOG: / THE
  RYTM'S VOICE IS MONO" straight from that byte), the **SEQ page's make prompt** on a non-seq track (same source), or
  the Rytm's own SRC page as ground truth. Worth adding the per-track voice byte to `/status`: this reading was wanted
  twice in one session and does not exist.
- **For SD**, the `poly` stats line does discriminate, and these causes are worth separating: a pad press that raises
  `seq_pads` but not `notes` means the machine had nothing loaded (no bug); `swallowed` means a stale live pad binding
  ate its 0x04 (a bug, the family rui1040's `selected_voice` and 1042's `rytm_trk` address); `trk_pads` means it was
  taken as a track selection (same family); nothing raised means the pad's note-on never paired with the voice's 0x04
  inside `poly::kBindMs` (30 ms), which drops the trig on a seq track (`poly::seq_pad`, fix1040b) - a latent timing
  bug and a candidate for intermittent dead pads on poly tracks. Take the same two readings on a pad that **does**
  sound (BT did) in the same session: the difference between them is the question.
- **Cost:** `reconcile_mask` is `kRytmTracks` x `kTracks` pointer comparisons on the control side, no card work and no
  save (it marks nothing dirty, so merely visiting a pattern writes no file; the first edit saves as any edit does).
  Nothing on the load path got slower - this matters, see docs/pi-engine.md "Underruns and the start-up stall".
- **No format change.** `pNNN.sqp` and `tracks.txt` are written and read exactly as before.
- `seq=clear:T` on a track that is no longer a seq track empties its slot, and the next conversion reclaims it.
- core/tests/test_seq.cpp's pattern-switch case changed with the behaviour: a seq track now has an (empty) track of its
  own in a pattern it is loaded with, where it used to have none.
- core/tests/test_polymake.cpp: four tracks made poly in the pattern and turned off again (the four slots taken), then
  CH (WAVETABLE) and OH (PLAITS) converted and a step key tapped on each - both trigs land; then a pattern switch, where
  both keep their mask bit and have to be given a track again, and a step key lands with no conversion; then four real
  seq tracks and a fifth conversion refused (no mask bit).

## Built: poly tracks per pattern (2026-10-26, branch poly-pattern; device check pending)

Spec: docs/superpowers/specs/2026-10-26-per-pattern-poly-design.md. The user's report: a new pattern after a cleared kit
"stuck" (every track reporting RYTM, yet `mask 0x737 pinned 0x737 silent 0x737`: trigs blue, no regular trig addable),
and TURN OFF + YES did not hold. Host only; the Rytm firmware is unchanged (the 2E already carries everything).

**The membership model.**

- **Which tracks are poly is the pattern's.** Each `Track` slot has an `on` flag (`seq.h Track::on`). A track is poly in
  pattern P when P has a slot for it and that slot is on. A new / empty pattern has none: plain Rytm.
- **`mask` is derived, never set** (`seq.cpp remask` / `mask_of`): the current pattern's slots that are on **and** whose
  Rytm track plays a Sidecar machine (`ui::remote_track_machine(r) != 0x7F`) **or has not been reported yet** (`-1`: a
  Pi reboot keeps the capture until the Rytm's next 33). Recomputed at every install (load, switch, prefetched switch),
  after every edit that moves slots (`on_edit`: pattern / track paste, clear, undo, reload), after `set_seq`, and in
  every `tick()` (a 33 changed a machine). Everything downstream reads it as before: the 2E, the pin, the 46,
  `in_mask`, `owns_track` / `owns_voice`, the pads, the pages, the planner.
- **YES** (the make prompt, POLY CONFIG's YES, `seq=track:T:on`) sets the current pattern's slot on; **TURN OFF**
  (the SEQ page's track menu - FUNC + DOWN since updown1029 - + YES, `seq=track:T:off`) sets it off: its steps are **kept** (silent), YES again brings
  them back. Both mark the pattern dirty (the flag is in its file).
- **fix1028: TURN OFF in step recording** (hardware: "the poly steps are still displayed and the poly mode display is
  still visible until NO and GRID REC off"). The host's seq mode ends as soon as its track leaves the mask (`remask` ->
  `seqmode_end`: the Rytm's own rule, rui.c `sq_tick`, so its 2D 08 off follows the 2E): held trigs, our trig LEDs and
  the page seq mode opened go at once, and a TURN OFF also closes the page TRK + TRIG opened (as `selected_track`).
  The SEQ page shown some other way draws a track that is no poly track as `NOT SEQUENCED` (no strip, no roll, the
  Rytm's own trig LEDs). test_polypat case 11.
- **fix1028 follow-up, "Ask on make"** (the user's ruling). The SEQ page's make prompt (YES) on a track whose slot
  still holds kept notes (an earlier TURN OFF) turns it on and asks `RESTORE n NOTES?` (`kPromptRestore`): YES keeps
  them; NO clears them as an edit - the track before it is a level in the platform's history (`UndoHooks`, the
  touchscreen's), so FUNC + YES on the poly track's SEQ page (now the undo outside NOTE EDIT too) or the touchscreen's
  UNDO bring them back. No kept notes: no second question. `seq=track:T:on` (the console) and POLY CONFIG's YES turn
  the track on with its kept notes, as before. The touchscreen has no make-poly control (its empty view points to the
  console and the Rytm's SEQ page), so it needs no question. test_polypat case 12.
- **fix1028 review.** The poly track's menu, its OFF and RESTORE questions belong to their pattern and track: a switch
  or load to another pattern, or the track leaving the poly tracks (the console, POLY CONFIG, a Rytm machine), drops
  them (`prompt_drop_stale` in `install` / `remask` / `prompt_answer`), so their YES / NO never acts on another
  pattern's track. NO on a track emptied meanwhile takes no undo level; while a project loads it is refused and said;
  without an undo history (no UndoHooks) the question says `NO: CLEARED FOR GOOD`. The kept count is the notes within
  the track's length, and steps with locks only are named (`+ 1 STEP OF LOCKS`). The SEQ page of a track that is no
  poly track asks the Rytm for no trig key (`seq_trigs` 0) and its trig keys / PAGE are not the sequencer's (no hidden
  edit of kept steps). Known: between the host's early seq-mode end and the Rytm's 2E (a few ms) a trig press is
  nobody's. test_polypat cases 11 (more), 13.
- **A track playing a Rytm machine is released while it does**: out of `mask` (its notes released, locks back), its
  slot, flag and steps untouched; a Sidecar machine again (this kit or a later one) captures it again.
- **The Rytm's stock CLEAR PATTERN** (2D 0A `kPClear`) empties the Sidecar's pattern too, its slots and their flags
  with it: every poly track of the pattern turns off (a cleared pattern is a new one, plain Rytm - the "sticky poly"
  the user reported must not come back that way). The Rytm's UNDO of it brings the slots, flags and steps back. A
  pattern / track PASTE carries the flags along (the slot is copied whole); a track CLEAR empties the steps and keeps
  the flag. (Ruled 2026-10-26, review round 1; test_polypat case 9.)
- **The Pi's touch editor** edits a seq track's slot only (`seq::edit_seq_track`: in the mask now); an off or released
  slot answers "T%d is not a seq track", and its chips list the mask's slots only (review round 1).
- **A single-track 33** (one track's machine changed in the SRC list) names that track only: the others stay
  unreported (`remote_track_machine` -1) rather than analog, so after a Pi reboot they keep their capture (review round
  1; before, the first 33 set every track it did not name to 7F).
- **POLY CONFIG** is unchanged: per pattern (`kBlkPoly`), `tracks.txt`'s `poly=T:` lines the default.
- **`tracks.txt`'s `mask=`** is no longer read for membership. It is still written (set / cleared with each YES /
  TURN OFF, as an older build wrote it) so an older Sidecar reading the project plays it as it did.

**Slots.** Still `kTracks` (4) a pattern. `slot_for(r)`: the track's own, a free one, else one that holds **no steps and
is off**; a slot that holds steps (on or off) or is on is never taken. A fifth YES with four such slots is refused with
the reason (log, and the page's message "4 POLY TRACKS IN PATTERN"). `reconcile_mask` (fix1051b) is gone: a pattern's
slots are what the pattern holds. The Rytm's stock clipboard ops still create slots on plain tracks (fix1042): those are
off.

**The file.** v4 tagged block **`kBlkOn` = 13**, per track `<track> <on 0|1>`, **not required** (an older v4 Sidecar
keeps it verbatim). A file without it (every pattern saved before) reads by the rule **a slot that holds steps is on, an
empty slot off** - so existing patterns keep their poly tracks where they have steps, and the empty slots
`reconcile_mask` scattered become plain tracks. The block is written only where the rule would read it otherwise (an
empty slot that is on, a slot with steps that is off): a pattern whose flags follow the rule stays v2 / v3, byte for
byte as before. `sim/protocol.py` `sqp_pack` / `sqp_unpack` mirror it (a track's `on`).

**The switch.** The Rytm must hear the next pattern's capture before it plays that pattern's first step. With a pattern
queued and prefetched (`nxt`), `tick()`:
1. puts the next pattern's **new** seq tracks in the 2E's **clock** part (`ck`: ticks only, nothing captured) so they
   have a step clock and the prediction can plan their first steps (`update_prediction` fills `pred_abs` for a track
   whose clock arrived after the prediction was made; `plan()` plans a track from the current pattern while it is a
   seq track there, from the predicted one while it is one there);
2. **`kSwitchLead` (1536 frames) + the latency before the predicted switch** sends the next pattern's mask whole
   (`st.mask_early` counts these). Not at the moment the prediction is made: that can be bars early, and the old
   pattern's remaining steps would play with the new capture.
A miss drops the prediction: the current mask goes out again. Without a prediction (stopped, a direct jump, no ticking
track and no clock) the new mask goes out at the install, as before. Known edge: a stock trig with a late micro timing
in the old pattern's last ~36 ms on a track the next pattern captures is suppressed.

**Tests.** core/tests/test_polypat.cpp - the spec's eight, each written failing first (30 failed checks against the
old code): a new pattern with a cleared kit (mask 0, a 2E with 0); A {CH} -> B {} -> A {CH}; TURN OFF holds across
step recording and a reload, B and C untouched, YES brings the steps back; a Rytm machine releases, a Sidecar machine
captures again; the old-file rule and the block; four slots with steps refuse a fifth, an off empty slot is taken, an
on empty one never; the switch's 2E at the prediction (1748 frames before the switch block) with the new track's
first step planned ahead; a Pi reboot keeps the capture. test_polymake's switch and WIDE cases changed with the model
(the next pattern is plain until a track is made poly there). Emulator: sim/rui_e2e.py `polypat` (below).


## Built: NOTE EDIT, Push 3 style (2026-10-27, branch note-edit; host, Pi display and emulator; device check pending)

docs/SEQ-IDEAS.md U1 has the design and the reasons. BACKLOG 5a's scope for 0.3.0: POS, NUDG, LEN, PTCH and VEL. VRNG,
per-note PROB and COND need a per-note v4 block, so they are not built. Their knobs show `-` and their footer says
`NOT YET`. **No Rytm firmware change**: rytm-mods 0026 already forwards any encoder 0..15 the catch mask (37) names, and
LEVEL / DATA is encoder 8 with its push at key 0x78. The SEQ page now sets those two bits while the track it shows is a
poly track (`seq::data_wanted`).

### How to use it (on the Rytm, a poly track's SEQ page: TRK + TRIG, or TRIG in GRID REC)

| keys | what |
|---|---|
| DATA turned | NOTE EDIT. The first detent lands on the last highlighted note if it is still on the page shown, else on the page's first note. Then it moves note by note in play order (step, then pitch upward in a chord), across pages, and the page follows. It stops at the ends. |
| FUNC + DATA | by step: the first note of the next / previous step with notes |
| a trig held | its lowest note highlighted; DATA walks its chord only; A..E edit that step's highlighted note (the selection is left alone; an empty held step gets the track's note at the first turn); let go: DATA goes on through the whole pattern. **TRIG** with the trig still held: its step pages as before (TRIG: NOTE VEL LEN MICR COND STRM CHD INV, RTRG, FX) - NOTE EDIT comes back at the next hold |
| a trig tapped | a step with notes: the highlight goes there (never toggled off). An empty step: the track's note added (as a tap) and highlighted |
| A POS | step by step (FUNC: by 1/96, as B). It skips steps that are full or hold the pitch; the track's end stops it. A step's only note moved into an empty step takes the trig along (condition, locks, ratchet, FX columns) |
| B NUDG / C LEN / E VEL | the micro offset by 1/96 (FUNC 1/8 step) / the length by 1/16 step (FUNC a step) / the velocity by 1 (FUNC 8) |
| D PTCH | by semitone (FUNC: octave). With the pattern's scale and QNT EDIT / PLAY: by degree (FUNC: semitone). It skips a pitch the step already holds |
| DATA push | the highlighted note in / out of the selection (the footer says `SELECTED n` for a second: the Rytm drops a DATA turn ~0.55 s after the push); the knobs then edit every selected note, relative, and the values show as ranges (`C4..F4`, `80..100`) |
| FUNC + DATA push | every note of the track selected, or none when all are |
| FUNC + NO | deletes the selection (else the highlighted note). More than one note: the footer asks `F+NO AGAIN: DEL n NOTES`, a second FUNC + NO within 2 s deletes. The highlight goes to the next note; a step left without notes is cleared as a trig off (its locks too) |
| FUNC + YES | **undo**: the newest level of this track in the platform's history - the touchscreen's (pi/ui/seq_touch), so its UNDO / REDO and the Rytm's walk one history. One level a gesture (a knob's detents within 1 s are one) |
| NO | back a level: the selection cleared first, then NOTE EDIT left |
| TRIG | leaves NOTE EDIT (on to DEFAULTS, as from the notes) |
| FUNC + DOWN | the poly track's menu (updown1029; DOWN ux1027, YES before - YES and UP / DOWN alone are the Rytm's now; NOTE EDIT stays under it) |

The screen (fix1028, hardware: "only room for 4 rows of notes; the current note should always be in the viewport"):
- No title bar. Line 1: `NOTE EDIT BD` at the left; `S5 2/3 5/12` at the right (the note's step and place in its
  chord, its place among those DATA walks), or `S5 2/3 SEL 3`.
- The page's roll, rows 8..38 (was 19..31), with no strip (the trig LEDs show the steps): a lane a pitch, 3 or 4 rows
  each. The highlighted note's bar is framed and selected bars are hatched.
- More pitches than lanes (10 at 3 rows): a window over them that keeps the highlighted note in view
  (`ui::ne_window`). It moves the least, keeps a lane of margin where the range allows, and does not move while the
  highlight stays inside; with a selection it follows the highlight. A notch at the right of the roll's top / bottom row
  says there are pitches over / under the window.
- The values in two rows under a rule, each with its letter: `S5 N+0 L.75 C4` / `V100 - - -` (ranges over a
  selection: `S1..9 0..3 .75-1 C4..F4` / `80..100`). The knob turned last is inverted; its name is in the footer.
- The footer: that knob in long form (`PITCH C4..F4 ALL`), DATA's `NOTE 5/12 S9 F4` (`CHORD 2/3 ..` with a trig held),
  else `PUSH:SEL F+NO:DEL NO:BACK`.

The trig LEDs: the highlighted step amber (as a held trig), steps with selected notes green.

Another pattern or track keeps NOTE EDIT on. The same (step, pitch) is looked up there, else the first note at or after
that step, and the selection clears. A note that went meanwhile (the touchscreen, an undo) is looked up the same way as
soon as the page or the LEDs draw next; a turn before that lands on it without moving on. PAGE away, and DATA lands on
the new page's first note.

**Touchscreen** (pi/ui/sidecar_seqed.c, pi/engine/seqed_pi.cpp): the view carries `ne_on ne_step ne_pitch ne_sel ne_gen`
and a note's flags bit 1 (selected).
- The editor selects the highlighted note, so its card shows the values.
- It pages the window to that note, and scrolls the note's row to the middle when it is off the window or in its top
  four rows (the card sits above the note).
- The playhead does not move the window while NOTE EDIT is on, and the Rytm's PAGE does not move it either.
- The Rytm's selection gets a second frame (amber on CLASSIC, the accent on OLED).

### Code

- core/seq/seq.cpp: the `ne_*` block before `ui_key`, `note_edit()`, `note_selected()`, `note_edit_range()` and
  `data_wanted()`, the LEDs, and FOLLOW held while NOTE EDIT is on.
- core/seq/seq.h: `NoteEdit`.
- core/ui/page_seq.cpp: `ne_draw`, and the roll's marks.
- core/ui/remote.cpp: `send_mask`'s `data`.
- core/ui/page_keys.h: the KEYS rows DATA, DATA PUSH and FUNC+NO.

A note is (step, pitch): a step never holds a pitch twice (`add_note`). The selection is a 128-bit pitch mask per step,
moved with every POS / PTCH edit.

### Behaviour changes

- On a poly track's SEQ page, DATA no longer sets the Rytm track's LEVEL, and its push no longer opens stock's sound
  browser. On any other track and page they are stock's as before.
- FOLLOW does not move the page while NOTE EDIT is on: the page is the highlight's.

### Tests

- core/tests/test_noteedit.cpp, written failing first: the mask (DATA and the push caught on a poly track, not on a plain
  one), the landing, walking a chord and pages, FUNC by step, a held step's scope, a tap that never deletes, a tap that
  adds, A..E, the trig moving along with POS, the skips, the selection with relative edits and ranges, FUNC + push
  all / none, FUNC + NO, NO's levels, YES's menu, TRIG, the LEDs, a vanished note, PAGE then DATA, another pattern.
- The renders: `core/build/seq/noteedit_*.png`.
- pi/ui/host shot: `seq-noteedit` in both themes and both sizes.
- Emulator: `sim/rui_e2e.py noteedit` (RUI_TAG=rui1051, the real core). It runs three times, each from a fresh core
  card, with the panel's screenshots: the walk to S5's D4; D PTCH; two notes selected by the DATA push; VEL on both
  (`V90`, `SEL 2`); FUNC + NO deleted both (`2 NOTES DELETED`, the steps `1:60,64 9:65`).
- [measured, rytm-emu rui1051] **The Rytm drops a DATA turn that comes ~0.55 s after the DATA push is let go** (a turn
  1.1 s after it arrives). This is the same stock behaviour rui1050 measured for A..H, with a longer window here. On the
  device: after a push, wait a moment before turning. A mod could forward those turns (0026's `enc_body`), but this
  build changes no firmware.

### Review round 1 (2026-10-27)

A read-only review (its probe: each scenario a failing check first in test_noteedit's `review_round1`):
- **C1:** POS on a whole chord lost its trig data. Now a step whose every note moves, into a step that will be free,
  moves whole (decided before anything is removed, in the move's order, so a run of chords shifts). That covers its
  condition, locks, ratchet, FX columns, CHD / INV / strum and mute.
- **I1:** a single note with locks moved onto an occupied step left nothing behind. The source now stays a lock trig.
- **I2:** PTCH by degree merged C4 and C#4. A shift where two moved notes land on one (step, pitch) is skipped.
- **I3:** with a trig held, A..E edit that step's note, never a selection elsewhere. An empty held step gets the
  track's note. TRIG under the held trig reaches the old step pages.
- **I4:** undo through `seq::UndoHooks` (the Pi: seq_touch `save_level` / `undo_level`), FUNC + YES, one level a
  gesture. FUNC + NO on more than one note asks first.
- **I6:** `ne_move` now uses bitmaps (the targets, the landings) and per-step counters: O(n) a tried shift. The dense
  worst case (128 x 8 notes, 512 selected, every shift blocked) takes POS 100 us and PTCH 38 us on the Mac's CPU clock
  (test bound 3 ms; Pi 4 est. x34: ~3.4 ms).
- **I5:** the DATA push's footer `SELECTED n` (1 s). The firmware fix is in docs/BACKLOG.md.
- **Minors:**
  - LEN sets the track's default length from the edited note.
  - A move no longer uses `Note::fresh` (the selection rides in its own array).
  - FUNC + NO clears a step left with locks only.
  - Stale selection bits are pruned on every data change.
  - A tap beyond the track's length adds nothing (`BEYOND THE LENGTH`).
  - Entering with a trig held starts on its step.
  - A..D labelled.
  - The playhead's LED wins over a selected step.
  - The touchscreen follows only between gestures, and a note the touchscreen moves (seqed=move) takes the
    highlight / selection with it (`seq::note_moved`).
  - The emulator test checks VEL on the selection, the confirmation, and that DATA / its push are stock's again on a
    plain track (no 32 08 / 31 78 reached the core; the panel shows stock's `TRK LEVEL` readout).

### Review round 2 (2026-10-27)

Each finding is a failing check first, in test_noteedit's `review_round2`:
- **N1:** a move refused at the track's end left landing marks behind, and they blocked later moves. The landing bitmap
  is now cleared at every call, and after each tried shift from cached (step, pitch) landings.
- **I6:** the shifts a detent tries are capped: POS 16, PTCH 12 semitones / 7 degrees / 2 octaves. Past the cap the
  footer says `NO ROOM THERE` / `NO PITCH FREE`.
  - Degree steps go through a table of the scale's pitches: each target's degree index is computed once, then each
    step is O(1). It equals `degree_move` for every pitch (tested in D dorian).
  - `seq::note_edit_work()` counts the targets checked.
  - The probe's G2 (128 notes by degree, every degree up blocked) costs 791 units and ~11 us CPU. G3 (POS, 512
    selected, every step full) costs 7120 units and ~44 us. Both are timed (bound 250 us, timing_retry) and counted
    (<= cap x n).
- **N2:** the undo's one-level-a-gesture key is now the knob, the track, the pattern and the highlighted note as the
  last detent left it, with nothing edited elsewhere since. DATA, the push, another track or pattern start a new
  gesture.
- **N3:** the track after the last NOTE EDIT gesture is kept. When the track changed outside NOTE EDIT since (the step
  pages, a pad into a held trig, live recording, the Rytm's ops), that state becomes a level before the next level, and
  before an undo. The Rytm's FUNC + YES, and the touchscreen's UNDO (seqed_pi calls `seq::undo_prepare`), take back the
  outside edit alone, and REDO brings it back. A touch edit's own level clears that pending state (`seq::level_taken`).
  The test drives the step page's COND and a pad note into a held trig (mark_dirty, the path live recording takes). The
  clocked live recording itself is not driven here.
- **N4:** a project change clears the history: `UndoHooks::clear`, called from `seq::project_changed`. On the Pi that is
  `seq_touch::reset`. A card change loads the project again through the same path.
- **The delete question:** it is dropped by DATA, the push, or another track or pattern, so a second FUNC + NO deletes
  only what the first asked about.
- **Minors:**
  - A refused gesture makes no level.
  - A held empty step filled and turned is one level.
  - The undo is refused while the project loads, before it takes a level.
  - FUNC + NO on a held trig's step pages is that page's lock clear, not NOTE EDIT's delete.
  - The duplicate playhead LED line is gone.
  - `note_moved` checks the pattern.
  - SEQ-IDEAS U1's first sketch now says what was built.

- The degree table differs from `degree_move` in one place, the range ends. Where `degree_move` would move a note only
  partway and clamp it at 0 / 127 (1,421 of the re-review's 79,872 cases), the detent is now refused with
  `NO PITCH FREE`, so a selection keeps its shape. Everywhere else the two agree.

### Review round 3 (2026-10-27): the N3 cover

Probes P1, P2 and P4 are failing checks first, in `review_round3`. Its history is shaped as seq_touch's (undo and redo
per track and pattern), and the touchscreen's UNDO / REDO run as in seqed_pi.
- **NB-1:** the touchscreen's UNDO / REDO left the snapshot of the state after the last gesture stale, so a later
  FUNC + YES brought an undone edit back. `undo_prepare` now runs before seqed=undo and seqed=redo, and ends the snapshot.
- **NB-2:** "changed outside" used the global edit count, so an edit on any track made a level (P2: 19 levels for one
  gesture with SD edited between the detents). It is now the track's bytes against the snapshot (a memcmp; ~15 KB
  copied a gesture), in the cover and in the one-level-a-gesture check.
- **NB-3:** the cover check now runs before the snapshot is replaced by another track's gesture, and in `install()`
  before a pattern goes. An outside edit on A, then a gesture on B, then back to A: FUNC + YES takes back the outside
  edit alone.
- `level_taken` checks the pattern.

### Next (not built)

- **F VRNG, G PROB, H COND per note:** a v4 block `kBlkNote` (per track: `{step, pitch, vrng, prob, cond}` for the notes
  that have any), the planner's draw for VRNG and PROB from E1's keyed RNG (kRngNote), COND evaluated per note as a
  step's.
- A **pulse** on the touchscreen's highlighted note.
- **Tap a note on the touchscreen** to move the Rytm's highlight there.

## ux1027: YES is the Rytm's on the poly SEQ page, DOWN its menu; pages that replace a stock screen have no siblings (2026-10-27, host only; device check pending)

The user (0.3.0): "when you're editing a poly seq, you cant use yes + play or yes + scene to quick save a kit or
pattern ... use the down key ... to go to the poly track settings", and "a bunch of remote ui pages still show as part
of the remote set ... it should simply replace that page".

- **YES on a poly track's SEQ page** (seq mode, the page UP + TRIG opened, NOTE EDIT) is no longer in the catch mask
  (37): it reaches the Rytm at its press, so the Rytm's own YES-held combos (YES + KIT / PLAY / SCNE ..., its quick
  saves) work under the page. rytm-mods 0026 decides per key at the press (rui.c `key_owner`: a key the mask names is
  the host's, else stock's; FUNC caught is both sides'), so the host names YES only while it wants it
  (`seq::yes_wanted`): a question on screen (the track menu, MAKE / RESTORE n NOTES? / TURN OFF?), **FUNC held** (FUNC
  + YES: the undo - FUNC first, then YES, as the user presses it), or **a trig held** (trig + YES: the step preview).
  The host sees FUNC and the trigs at their press (both caught), and its next frame (the first `remote_tick` after
  the key, at least kInputMs = 8 ms after the previous frame: tested in test_noteedit) sends the new mask. That is the
  host's share only; the end to end time (the 31 over USB, the host, the 37 back, the mod applying it) is not measured
  on the device or in rytm-emu. A YES pressed within that window goes to the Rytm as FUNC + YES (no undo). The coming
  mod flash closes it (rytm-mods 0026: while a Sidecar page is open, YES goes to the host whenever FUNC or a page trig
  is down). YES pressed first and FUNC after is stock's (stock 1.73 binds nothing to FUNC + YES on
  the grid screens [not measured]). No mod change.
- **DOWN** (no trig held) opens the track's menu (BACK / TURN OFF / LENGTH / QUANTISE / PLAY MODE / PRINT RND); in the
  menu UP / DOWN pick a row, YES chooses, NO back, as before. DOWN was the SIDECAR ring's "next page" here before (the
  page had no use of its own for it), a ring the page no longer has (below); with a trig held, or on a track that is no
  poly track, DOWN does nothing. Footers: `GRID REC: EDIT  DOWN:MENU`, the R2 hint `OFF: DOWN, TURN OFF`; KEYS row
  `DOWN  MENU (POLY SEQ)` (core/ui/page_keys.h, the touchscreen's map too). FUNC + DOWN and PAGE held + DOWN do
  nothing. (updown1029, below: the menu is FUNC + DOWN now, DOWN alone the Rytm's.)
- **No siblings** (core/ui/remote.cpp `standalone`): a page off the LEFT / RIGHT ring - SEQ, INSERT (UP + FX / UP +
  AMP), LFO (UP + LFO, now off the ring; the SIDECAR MENU still lists it), POLY, POOL, CHOP, MENU, KEYS - has no page
  dots and LEFT / RIGHT / DOWN / PAGE are its own or nothing, never the ring's. Inside: INSERT's RIGHT the EQ view and
  LEFT back out (unchanged), LFO's LEFT / RIGHT the LFO before / after (the design view: the step), as knob A. Leaving:
  NO (back where it came from - the stock screen closes the page), or the same UP + key again: UP + LFO / UP + FX on
  the page it put up leaves as NO does (host side: the 30 page byte again, forgotten at any other page change); UP +
  TRIG leaves SEQ as before (the mod's rui1037). UP + AMP again re-aims the OUT view at the selected track, as rui1047
  (no leave). UP + FX on UP + AMP's page stays on INSERT. Leaving LFO drops its design view.
- Tests: test_noteedit (`ux1027_keys`: the mask with and without YES, DOWN, the timing, LEFT / RIGHT), test_lfo,
  test_insert, test_seq / test_polypat (the menu on DOWN), test_ui (the ring never lands on LFO).

## updown1029: UP / DOWN are the Rytm's on the poly SEQ page, FUNC + DOWN its menu (2026-10-29, host only; device check pending)

The user: "in poly mode i want to use the chromatic mode / pads to play notes. i cant change octaves now with the up
and down arrow. instead of down for the poly track menu it should function down".

- **UP / DOWN on the SEQ page** are no longer in the catch mask (37) by default: they reach the Rytm at their press, so
  in CHRO pad mode the arrows shift the pads' octave under the page. The host names them only while it wants them
  (`seq::updown_wanted`): **a question on screen** (the track menu's rows; under MAKE / RESTORE n NOTES? / TURN OFF?
  they do nothing, as the trig keys) or **FUNC held** (FUNC + DOWN: the menu). Under a held trig they stay the Rytm's
  (the page has no use for them; stock never saw the poly trig, so its arrow is the octave too). NOTE EDIT uses
  neither. The other Sidecar pages (SIDECAR, INSERT, LFO, POLY, POOL, CHOP, MENU ...) catch UP / DOWN as before.
- **FUNC + DOWN** (no trig held, no PAGE held, a poly track, the page shown) opens the track's menu; DOWN alone no
  longer does. In the menu UP / DOWN pick a row (FUNC held or not), YES chooses, NO back, as before. FUNC is caught on
  both sides (the Rytm keeps it held), but the DOWN is caught while FUNC is held, so the Rytm sees FUNC alone.
  ux1027's review 6 kept FUNC + DOWN out so a DOWN inside a FUNC chord opened no stray menu; with FUNC the menu's
  own modifier now that reason is gone (the host binds nothing else to FUNC + DOWN). Footers: `GRID REC:EDIT
  F+DOWN:MENU`, the R2 hint `OFF: F+DOWN, TURN OFF`; KEYS row `FUNC+DOWN  MENU (POLY SEQ)`.
- **UP** is still the Sidecar's modifier: its combos (UP + T1..T13, UP + TRIG / LFO / FX / AMP / SRC / YES, trig + UP +
  RECORD / STOP / PLAY) are the mod's, decided by UP held whoever owns it (rytm-mods 0026 rui1046 `UPH()`), so a
  stock-owned UP keeps them. The host never acted on UP on the SEQ page (remote.cpp on_key: fix1049).
- **The window** (as ux1027's YES): the mask naming DOWN goes out at the first frame after FUNC's press (>= kInputMs =
  8 ms host side, end to end unmeasured). A DOWN pressed inside it goes to the Rytm as FUNC + DOWN (no menu; whatever
  stock does with FUNC + DOWN under CHRO [not measured]); a second FUNC + DOWN opens it. A mod-side rule (while a
  Sidecar page is open, UP / DOWN to the host whenever FUNC is down) would close it, as for YES.
- Tests: test_noteedit (`ux1027_keys`: UP / DOWN out of the mask, in it with FUNC held and under the menu, DOWN alone
  no menu, FUNC + DOWN the menu, its rows), test_seq / test_polypat (the menu on FUNC + DOWN; UP / DOWN under a
  question; INSERT still catches them).
