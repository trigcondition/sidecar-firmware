# Sequencer ideas for Sidecar poly tracks (ideas, 2026-10-22)

**Status (2026-10-22): E0, E1, E2 and the top 8 of section 7 are built on the host** (branches `seqx` and `seqtop`;
docs/SEQUENCER.md "Built: seqx" and "Built: seqtop", docs/POLYPHONY.md "7. E0"): P2, P1, M1, R1 + R1b, V1, P4 + M4,
X1, H1 + V3 (H1 without SPD). They are set on the console, the web page (Sound -> Sequencer) and the touchscreen's SEQ
page (KEY, PLAY, STEP). **The Rytm side of E0 is built** (branch `polycfg` + rytm-mods rui1048, emulator only;
docs/SEQUENCER.md "E0 on the Rytm"): POLY CONFIG is the last item of the Rytm's own track menu (FUNC + CHRO); the make
prompt is offered on every Sidecar machine. The Rytm's keys and pages for the top 8 come later (SEQUENCER.md "For the
Rytm side"). Every open question of section 8 is decided. Everything else below is still an idea.

This document collected ideas only (see the status above for what has been built since). It lists features for the Sidecar's own step
sequencer on poly tracks (docs/SEQUENCER.md) that the stock Analog Rytm MKII does not offer, or offers only for its own
tracks in a weaker form. Each idea says what it does musically, why it is new, how it fits the Rytm-first UI, what it
changes in the data model, the engine and the file, the timing risks, the effort and the dependencies. A shortlist
with a build order and the open questions for the user come at the end.

Tags, as in SEQUENCER.md:

- **[code]**: read in this branch (`core/seq/seq.h`, `pattern.cpp`, `seq.cpp`, `core/fmt/formats.cpp`).
- **[public]**: other instruments' features, recalled from their manuals and reviews, not checked on a device. One
  web search did not confirm the MC-707's play-direction modes, so they are taken from the user's brief.
- **[inferred]**: design reasoning, not measured.

Effort: **S** about 1-2 days, **M** 3-6 days, **L** 1-2 weeks or more (host, mod, tests and e2e together).

## 0. What exists today, and the constraints every idea must respect

### What the sequencer has [code]

- **Per track:** up to 4 seq tracks at once (`kTracks`), each with 8 pages x 16 steps (`kMaxSteps` 128).
  - Its own `length` (0 = AUTO, the Rytm track's length).
  - Its own `swing` (50..80, every second step).
  - Defaults `dnote`, `dvel`, `dlen`.
  - **No speed of its own.** The speed is the Rytm track's SCALE, read from the ticks. The file's track header has a
    speed byte that is written as 0 and skipped on read (`pack` writes `w.b(0)`, `unpack` calls `r.b()` and discards
    it).
- **Per step:** up to 8 notes, each with pitch, velocity, length and micro offset in 1/96 step (-96..96). A step also
  has:
  - CHD / INV / strum;
  - `prob` (1..100);
  - `flags`: bit 0 on, bit 1 muted, bits 2..7 the trig condition. **The flags byte is full.**
  - up to 12 locks `{param, pad, value14}`. **`Lock::pad` is unused in memory**, and a lock is 3 bytes in the file.
- **Trig conditions:** FILL / !FILL, PRE / !PRE, NEI / !NEI, 1ST / !1ST, A:B (B 2..8), and probability. All are on
  one COND knob (`kCondKnob` = 143 values, so it fits in one byte). `cond_pass_i` evaluates them when the step is
  planned.
- **Planner:** `plan()` plans each track from its phase-locked `StepClock`, `kLookahead` = 1536 frames (32 ms) beyond
  the next block, up to 16 steps a pass. The step index is `abs % len`, so **the step order is linear**. Swing is added
  to the step's frame. Locks are a timed event before the step's first note, with hold and restore as Elektron's
  p-locks. A queued pattern's first steps are planned ahead at the predicted switch (`update_prediction`).
- **Rytm UI:**
  - **TRK + TRIG** opens the SEQ view. GRID REC on a seq track is seq mode.
  - Tap trigs to place steps. Hold a trig + pads to enter notes. Hold a trig + A..H to set NOTE VEL LEN MICRO CHD INV
    STRUM SEL.
  - **TRIG** toggles between the notes and the TRIG view: COND, the track's defaults and SWING.
  - **Held trig + SRC / SMPL / FX** (FX again: INS) opens the lock pages. **Held trig + FLTR / AMP / LFO** only shows
    a "not locked per step" page today.
  - **Held trig + YES** previews the step. **Held trig + FUNC + REC / STOP / PLAY** copies, pastes or clears it.
  - **PAGE** moves through the pages; PAGE held shows the page map.
  - The poly track's menu (FUNC + DOWN on the SEQ page since updown1029; DOWN ux1027; YES before): BACK / TURN OFF / LENGTH / QUANTISE.
  - **UP is becoming the Sidecar modifier:** UP + TRIG (the KEYS map), UP + SRC / LFO / FX / YES, UP + T1..T13 (the
    touchscreen's pages and tools).
- **Touchscreen (TOUCH-EDITOR.md):** a piano roll with DRAW / CHORD / STRUM / HUMAN / SWING / KEY tools, pinch, a
  velocity lane and a lock lane, and 24 undo levels. **Humanise exists** as an edit. **The scale/key is a display
  preference only.**

### Constraints

1. **The Rytm is the transport master.** Its step ticks are the clock. Pattern switches come at the Rytm's master or
   change length, counted linearly. Any feature that changes the *order* of the Sidecar's steps must leave the *time*
   grid and the switch point alone. Direction, jumps and loops change which step plays at a tick, never when the tick
   comes.
2. **Plan ahead, deterministically.** Notes are planned up to 32 ms (one step at fast tempos) ahead and started
   `latency` early. A replan after an edit, and the planning of a predicted switch, must reach the same decision as
   the first plan. Random choices therefore need a **seeded RNG keyed on (pattern seed, track, absolute step)**, not
   the free-running `rng` that probability uses today. With that RNG, one step's "random" result is the same however
   often it is planned. The doc calls this **E1** below.
3. **Analog parameters cannot be locked per step.** SEQUENCER.md "Not built" explains why. Every per-step parameter
   idea here applies to the Sidecar machine's P1..P16, FX1 / FX2 P2..P5 and the INSERT slots, and later to the
   digital envelope of POLYPHONY.md 5.2. It never applies to the Rytm's analog FLTR / AMP.
4. **Device-agnostic.** core/seq speaks of tracks, steps, ticks and a voice. Every idea below lives in core/seq in
   those terms, and only the key map (which Rytm key does what) sits in the device's remote layer and mod. The
   Analog Four MKII (BACKLOG section 7) would get the same engine with its own key map. One thing differs by device:
   **what counts as "new"**. The A4 already has an arpeggiator and parameter slides of its own [public], so on the A4
   those two would be parity. Direction, jumps, per-lock conditions and the FX column stay new on both.
5. **Data safety (fix1042) and the format registry.** `.sqp` is registered at v3, reading v1..3
   (`core/fmt/formats.cpp`). Its version byte is "the lowest reader", and a v3 Sidecar refuses a v4 file (locked,
   never written). That is the safe outcome, but it also means an older Sidecar cannot play a v4 pattern at all.

### What stock 1.73 already has (so this doc does not claim it) [public; code where noted]

The Rytm's own sequencer, for its own tracks, already offers the following. Below, an idea is "new" only for what goes
beyond this list:

- trig conditions (FILL, PRE, NEI, 1ST, A:B) and probability per trig;
- **retrigs** per trig (rate, length, velocity curve);
- micro timing per trig;
- swing (per pattern);
- per-track length and speed (scale per track);
- the FILL key;
- **Euclidean mode** (1.70+: two generators, rotation, boolean op; POLYPHONY.md's euclid bytes [code]), plus
  rytm-mods 0002's euclid accents;
- p-locks and trigless locks;
- scenes and performance;
- pattern chains, song mode, and pattern change modes (direct / temp jump).

The Sidecar's seq tracks already have trig conditions, probability, micro timing, swing per track, length per track
and step locks of their own, because they reimplement these. Those features are parity, not ideas. Euclid (V6), the
stock-like part of ratchets (P4) and per-track speed (P2) are kept below only for what they add on top: chords and
spreads, ratios the Rytm lacks, and Euclidean distribution of chords, notes and locks.

### E0 — Every Sidecar machine on the sequencer, with VOICES per track (decided 2026-10-22, build first)

- Any Sidecar machine (METAL and other user machines, MD, DSP SAMPLE, VOICE/PLAITS, DAISYSP, JUNO, FM, MULTISAMPLE, WAVETABLE, GRANULAR) can be a poly/seq track. The analog Rytm tracks stay mono (hardware).
- **VOICES 1..8 per track**: 1 = mono with retrigger/choke (drum-like, a new note cuts the old); more = overlapping tails, chords, rolls. Default per machine: drums 1, tonal machines their current poly count.
- Built-in engines get an instance pool (N engine instances per track, voice allocation with steal-oldest/quietest, per-instance state); CPU/RAM per instance shown on STATUS and checked against the voice budget (Plaits ~0.74 % of a core per voice on the Pi 4).
- Pitch: a note maps to the machine's pitch/TUNE param; engines without meaningful pitch take the note as TUNE offset or ignore it (declared per machine).
- User machines (RMPA): the manifest declares `max_voices` (and whether pitch is meaningful); packages without it are mono. A new ABI flag lets a machine opt in to instancing.
- Rytm side: the poly-track entry (UP+TRIG / the make prompt) is offered for every Sidecar machine; the analog voice is silenced as today on seq tracks.
- All E1+ features (seed, direction, ratchets, per-lock conditions…) then work on every Sidecar machine.
- **Model the per-track poly config on the Analog Four's POLY setup** (user, 2026-10-22): besides VOICES, an allocation mode per track (e.g. RESET / ROTATE / REASSIGN-like behaviour: restart the same voice, cycle through voices, reuse a voice that holds the same note), a UNISON option (stack N instances with DETUNE and SPREAD/pan), and possibly a **poly group across tracks** (several Sidecar voices/tracks pooled into one poly instrument, as the A4 pools its voices). Check the A4 MKII manual's POLY section for the exact options and names, so the concept and wording carry over when the Analog Four is supported later (device-agnostic rule).
- **Where the poly config lives on the Rytm** (user, 2026-10-22): preferably as a **POLY CONFIG item in the Rytm's own track menu** (the stock TRACK menu, e.g. FUNC + CHRO's TrackMenuView with RELOAD / SAVE TRACK), so it sits where a Rytm user expects per-track settings. That needs a rytm-mods change: add an entry to a stock menu and open a Sidecar-drawn (or stock-styled) sub-page from it. Research in the emulator first: how the stock menu lists are built (item tables, RTTI MenuView classes), whether an entry can be appended safely, and how the 1042 "step aside" overlay rules interact. Fallback if menu injection is too risky: the poly track's YES menu (UP + TRIG) or a Sidecar page reached from the SIDECAR MENU.

### E0b — FREEZE: convert an analog track to a poly track (user idea, 2026-10-22; not built)

An analog Rytm track is mono hardware. FREEZE turns its current sound into a poly instrument: the AUTO-SAMPLER samples
a range of notes with the track's current settings, writes a multisample, and the track becomes a Sidecar poly track
on MULTISAMPLE with that instrument loaded. Like freezing a track in a DAW, but for poly use.

Most of the parts exist [code: docs/SAMPLING.md section 3]:
- the AUTO-SAMPLER already takes a Rytm track as a source (`asamp=src:track:T[:CH[:chro]]`: notes over the host link,
  the track's Overbridge stem recorded, the latency measured, takes trimmed and looped, `WAV + .sfz` written, the
  instrument put in the INSTRUMENTS pool list);
- E0 already makes any Sidecar machine a poly track, MULTISAMPLE with VOICES up to 8.

What FREEZE adds:
- **One action**: an item in the track menu (FUNC + CHRO, next to POLY CONFIG) or the poly-track make prompt on an
  analog track ("FREEZE TO POLY?"). It runs the auto-sampler on this track, names the instrument after the kit and
  track (e.g. `KIT 12 BT`), loads it on a Sidecar voice and turns the track into a poly track.
- **Pitch per take**: rather than rely on the Rytm playing the track chromatically from the AUTO channel [inferred,
  not run], lock TUNE per take through the param write (48) and trig the track. The Rytm's TUNE range is ±24 semitones
  [inferred], so 4 octaves at most around the sound's own pitch. Default: every 3 semitones over 2 octaves, 1 velocity,
  **2 round robins per note** (user, 2026-10-22: so repeated notes alternate rather than sound machine-gun like; the
  auto-sampler's `rr:2`, played back through `seq_length / seq_position`; the analog voice's free-running oscillators
  and noise make the two takes differ by themselves), hold long enough for the AMP envelope (from its settings), tail
  until silent.
- **Clean stems**: RS/CP, MT/HT, CH/OH and CY/CB share an Overbridge stem. Mute the partner track while sampling, and
  stop the sequencer (or sample only while stopped).
- **Undo / re-freeze**: the analog track's kit sound is untouched; UNFREEZE turns the track back to analog. Re-freeze
  after editing the sound replaces the instrument.

Limits:
- **A convenience, not a replacement** (user): a quick way to play an analog sound poly.
- **Static snapshot.** Parameter locks and LFOs on the analog sound's synth params no longer apply. Pitch, velocity,
  AMP level and the Sidecar's own locks still do. A slow LFO is baked into each take, so loops may not be seamless.
- **Time.** Sampling runs in real time: 9 notes × 2 takes × (hold 1 s + tail) is roughly 40–60 s.
- **Mono.** The stems are mono; the instrument is too.

Open questions: where the action lives (track menu or make prompt); the default note range and step; whether to offer
velocity layers (the Rytm's velocity-to-sound mapping makes this worthwhile on some machines); whether FREEZE also
copies the track's FX sends to the Sidecar voice's route.

### U1 — NOTE EDIT: scroll through every note with an encoder (Push 3 style; user, 2026-10-24; queued after rui1050 + CHOP)

Model: Ableton Push 3 manual 9.3.1 "Note Edit Mode" [public, read 2026-10-24]: one note is always highlighted; turning
the jog wheel moves the highlight through the clip's notes in the order they appear; the encoders edit the highlighted
note: Position (by 1/16), Nudge (offset from the 1/16 in %), Length, Pitch (Drum Pad on drum tracks), Velocity, Vel
Range (random velocity within a range), Probability. Pressing the wheel selects the note (adds to a selection);
with several selected, values show as a range and edits are relative; Select All / Deselect All; Delete removes the
highlighted or selected notes. Holding steps (9.3.2) edits step parameters instead.

For the Sidecar poly tracks:
- **Rytm:** a NOTES edit view on the SEQ/TRIG page of a poly track (entry e.g. TRIG pressed again from the RTRG / FX
  cycle, or a FUNC gesture; keys per core/ui/page_keys.h). The **LEVEL/DATA encoder** scrolls the highlight through
  every note of the pattern in play order (step, then pitch within a chord), across pages; the page follows the note.
  Encoders: A POS (steps, FUNC = 1/96 micro), B NUDG (micro offset), C LEN, D PTCH (semitones; in a scale: degrees),
  E VEL, F VRNG (velocity range, keyed RNG), G PROB (per note), H COND (per note, V2). (The idea as first written:
  YES toggled the selection and FUNC+YES selected all, NO deleted. As built - the design below - the **DATA push**
  selects, FUNC + DATA push all / none, **FUNC + NO** deletes, **FUNC + YES** undoes, and YES stays the track's menu; ux1027: DOWN is the menu, YES the Rytm's own - its quick saves; updown1029: FUNC + DOWN, UP / DOWN alone the Rytm's - CHRO's octave.)
  The trig LEDs show the highlighted step.
- **Touchscreen:** the piano roll highlights the same note (pulse) and follows; tap selects; the M8/Push-like info line
  shows the note's values.
- **Data:** POS/NUDG/LEN/PTCH/VEL/PROB exist per note already (step notes + micro); VRNG and per-note PROB/COND need
  v4 blocks (E2), written only when used.
- Depends on: V2 (conditions per note) for COND; the E1 keyed RNG for VRNG.

**The design for 0.3.0 (2026-10-27; built: docs/SEQUENCER.md "Built: NOTE EDIT").** The user's entry ideas (2026-10-26):
no step held, DATA walks every note; a step held, that step's notes; let go, on from there.

- **Does DATA reach the Sidecar?** Yes, with no mod change. rytm-mods 0026's catch mask (37) has a bit for every
  encoder 0..15; LEVEL / DATA is encoder 8 [measured, design.md 1022: `--turn 8`] and its push is key 0x78. A caught
  turn arrives as `32 08 <delta + 64> <func>` and the push as `31 00 78 <down>`, as A..H do (rui.c `enc_body`: any
  `idx < NENC` in the mask). The host never set bit 8 for a page with knobs (only the status pages', which edit nothing).
  The SEQ page now catches DATA and its push **while the track shown is a poly track**. On any other track they stay
  stock's (the track's LEVEL, FUNC + push the sound browser). The cost: on a poly track's SEQ page, DATA no longer sets
  the Rytm track's LEVEL. With the page closed (GRID REC alone), DATA is stock's: 0026 forwards only A..H under a held
  trig there, so NOTE EDIT lives on the SEQ page.
- **Enter:** turn DATA on a poly track's SEQ page (the notes view or a TRIG page; not under a prompt or a lock page).
  Line 1 says NOTE EDIT (fix1028: no title bar; the roll scrolls to the highlight). The first detent only lands the
  highlight; the next ones move it.
- **Where it lands:** the last highlighted note of this track and pattern, if it is still there and on the page shown;
  else the first note at or after the start of the page shown (after the last one: the pattern's first). Why not the
  user's other two:
  - "The pattern's first note" makes a user looking at page 3 scroll through two pages.
  - "Nearest the playhead" moves under the user while playing, and means nothing when stopped.
  - The page shown already is the playhead's page while playing (FOLLOW) and the PAGE key's page when stopped. Stopped on
    page 1, it is the pattern's first note. Coming back to the same page continues where you left.
- **DATA turned:** the next / previous note in play order: step, then pitch upward within a chord, across pages. The page
  follows the highlight, and FOLLOW waits while NOTE EDIT is on. It stops at the first and the last note: no wrap, so a
  fast spin cannot lose the place. **FUNC + DATA** moves by step: to the first note of the next / previous step with notes.
- **A step held:** the highlight jumps to that step's lowest note at the press, and DATA walks only that step's notes
  (the chord). Let go: DATA goes on through the whole pattern from there.
- **A tap:** on a step with notes, the highlight goes there. NOTE EDIT never toggles a step off, so a tap never deletes.
  On an empty step, it adds the track's note (as a tap does: the last chord played, else the default note) and
  highlights it. Pads under a held step enter notes as before.
- **Selection: the DATA push** toggles the highlighted note in or out (Push's wheel press). **FUNC + push:** every note
  of the track, or none when all are selected. **YES stays the poly track's menu**, so NOTE EDIT never takes it (the
  spec's YES / FUNC + YES would have lost the menu).
- **The knobs** act on the selection when it has notes, else on the highlighted note. The edits are relative, and values
  the targets do not share show as a range (`C4..C5`, `60..100`).
  - **A POS:** a step a detent; FUNC + A moves by 1/96 (as B). The note keeps its micro offset. A step that is full or
    already has that pitch is skipped over, and the track's end stops it. A step's only note moved into an empty step
    takes its trig along (condition, locks, ratchet, FX columns): that is moving a trig.
  - **B NUDG:** the micro offset, by 1/96 (FUNC: 1/8 step), within one step early to one late (the model's range).
  - **C LEN:** by 1/16 step (FUNC: a step).
  - **D PTCH:** by semitone (FUNC: octave). With the pattern's scale and QNT EDIT / PLAY, by scale degree (FUNC:
    semitone), as the step edit's NOTE knob. A pitch the step already has is skipped over.
  - **E VEL:** by 1 (FUNC: 8).
  - **F VRNG, G PROB, H COND:** not in 0.3.0. They need a per-note v4 block (E2: `{step, pitch, vrng, prob, cond}`), and
    are shown as `-`.
- **Delete: FUNC + NO** deletes the targets. The highlight moves to the next note (else the previous), and the selection
  clears. A step left empty with no locks is cleared as a trig off would clear it. NO alone is "back a level", as on
  every page: with a selection it clears the selection, else it leaves NOTE EDIT. A plain NO never deletes.
- **Leave:** NO; TRIG (it goes on to DEFAULTS, as TRIG from the notes does); the page closing; the track no longer a
  poly track.
- **Pattern switch / another track:** NOTE EDIT stays. The highlight looks for the same (step, pitch) there, else the
  first note at or after that step. The selection clears, because it named the old pattern's notes. A note that went
  meanwhile (an undo, the touchscreen) is looked up the same way at once.
- **The screen:**
  - The header: NOTE EDIT and the track.
  - Line 1: the step and the note's place in its chord (`S5 2/3`), the page (`PG1/4`), the selection (`SEL 4`).
  - The page's strip, with the highlighted step's box framed. The roll, with the highlighted note's bar boxed and the
    selected notes underlined.
  - The values in two rows of four: A..D `S5 +12 L0.75 C4`, E..H `V100 - - -`; the one turned last is inverted.
  - The footer: the knob turned last in long form (`PITCH C4..C5`), else `DATA:NOTE PUSH:SEL F+NO:DEL`.
- **Trig LEDs:** the highlighted step amber (as a held trig), steps with selected notes green, the playhead white, the
  rest as before.
- **Touchscreen:** the piano roll selects the same note, so its card shows the note's values. The window pages to it and
  scrolls to its pitch, and following the playhead waits. The Rytm's selection is framed a second time.

## E1 and E2: two enablers most ideas share

- **E1, the step map and the seed.** Every random choice in the sequencer comes from **one seeded generator**: chance
  and probability, random notes (R1), random jumps (V3), BROWN / RND directions (P1), mutate (V5), playback humanise
  (M5), Markov (V4) and the morph (F1).
  - The value is a pure hash of (seed, track, loop index, step, purpose), not a running stream, so planning a step
    twice gives the same answer.
  - Each pattern has a **SEED** (0..9999, or FREE). A track has a seed of its own; by default it follows the
    pattern's.
  - **Fixed seed:** the same result on every loop of the same index and on every playback, and it survives save,
    load and bundles (it is in the `.sqp`).
  - **FREE:** the seed is re-rolled at PLAY, or every loop with the "per loop" option. The RNG is still keyed, so
    replans within one loop agree.
  - **Loop index:** the hash includes the loop index (`abs / len`, the same count A:B uses). With a fixed seed, loop 3
    is always loop 3's result, and an A:B condition on the same step stays predictable: A:B decides *whether* the
    step plays, the seeded RNG decides *what* it plays. Optionally the seed can ignore the loop index ("same every
    loop"), so a random line repeats exactly like a written one.
  - Today's probability uses a free-running LCG (`rng` in `cond_pass_i`) [code]. Moving it onto E1 makes the existing
    50 % trigs repeatable too.
- **The step map.** One pure function, `step_for(track, abs) -> {index, flags}`, replaces `abs % len` in
  `plan_step`, `play_pos`, the LEDs' playhead and the conditions' loop count.
  - Inputs: the track's direction, step-loop state and jump table, and the E1 RNG.
  - The Rytm's own playhead (stock writes it on the trig LEDs) stays linear. The host already owns the LEDs in seq
    mode and on the SEQ page (0x34 rewrites the passing playhead), so it draws the *mapped* step. With the page closed
    and outside seq mode, the Rytm's LEDs show the Rytm track's linear position, which is acceptable.
  - The "loop" that A:B and 1ST count is still `abs / len` (one cycle of the length). With pendulum it is one round
    trip, `2 len - 2` [inferred].
  - Effort S-M. Every play-order idea depends on it. Tests: the same `abs` gives the same step over replans,
    predictions and a reload.
- **E2, .sqp v4 with an extension trailer.** v1..v3 readers ignore bytes between the last track and `END`. `unpack`
  reads `nt` tracks and checks only `END` at the end [code], so a trailer *could* be skipped by old readers. That
  would be unsafe, though: the old Sidecar would then save the pattern back without the trailer and lose the data.
  - Proposal: bump to **v4 once** (older Sidecars lock the file, as fix1042 intends).
  - In v4, the new data is a trailer of tagged chunks: `{u8 id, u8 flags (bit 0: required), u16 len, payload}` per
    track and per step range.
  - A v4+ reader keeps the chunks it does not know **verbatim** on save, as class-S files keep unknown `key=value`
    lines (`fmt::kv_keep`). When an edit moved the steps under an unknown per-step chunk, the reader drops that chunk
    and logs it. A *required* chunk it does not know locks the file (`kNewer`).
  - After this, later features add chunk ids and need no new version. The registry entry becomes v4, reading 1..4.
    `sim/protocol.py sqp_pack` mirrors it, and the golden and fuzz files get a v4 case.
  - Effort S-M.
  - The track header's dead **speed byte** can carry idea P2 without the trailer: old readers skip it.
  - `Lock::pad` can carry the per-lock condition (V1) in memory. In the file, a v4 lock grows to 4 bytes, or the
    condition goes in a chunk.

## 1. Playback and time

### P1. Play direction per track: FWD / REV / PEND / PING / RND / BROWN

- **Musically:** the same steps in another order.
  - REV reads a riff backwards.
  - PEND (pendulum: 1..16..1, the end steps once) and PING (the end steps twice) bounce.
  - RND picks any step.
  - BROWN (a random walk: +1 mostly, sometimes 0, -1 or +2) drifts, but it keeps the riff's local shape. This is the
    Cirklon / Oxi / Hapax / MC-707 family [public].
- **New vs stock:** the Rytm plays forward only. Its euclid "rotate" is not a direction.
- **Rytm UI:**
  - The TRIG view with no trig held shows the track's NOTE VEL LEN SWING on A..D today. **E = DIR, F = SPD** (P2),
    **G = LEN**, H stays free.
  - The footer names the mode (`DIR PEND`). The trig LEDs show the mapped playhead (E1).
  - The poly track menu gets a PLAY MODE row too, for users who look there.
- **Touchscreen:** a direction chip in the header. The playhead line jumps to the step that plays. A faint "next
  step" ghost for RND / BROWN helps to see the movement.
- **Data:** `Track.dir` (u8) and `Track.dir_seed`, in an E2 track chunk.
- **Engine:** E1's `step_for`. RND / BROWN use the keyed RNG, so a replan and a predicted switch agree. Micro offsets
  stay attached to their step, so a note with a negative micro on a step that RND picks plays early relative to *its*
  tick, as written. Swing follows the *tick's* parity, not the step's, so the groove survives REV and RND [inferred;
  open question 3].
- **Timing risk:** low. Only the step chosen changes, not the frame. The only subtle part is the planner's "a note up
  to a step early" window: that is computed per tick, so it does not change.
- **Effort:** M (with E1).
- **Depends on:** E1, E2.

### P2. Per-track speed, independent of the Rytm track (polyrhythm)

- **Musically:** a poly pad at 1/2x under a 1x drum track, or 3/4x for a 4-against-3. Together with the existing
  per-track `length` this gives full polymeter and polyrhythm. The Digitakt 2 and Tonverk ratios include 3/4 and 3/2
  [public].
- **New vs stock:** the Rytm's per-track scale offers 1/8x..2x, but only in its own SCALE menu, and it governs the
  Rytm track. Here, the Sidecar track runs its own ratio over a Rytm track that keeps its own scale. Free ratios
  (2/3, 5/4, 7/8) are new.
- **Rytm UI:** the TRIG view (nothing held), F = SPD: `1/8 1/4 1/2 3/4 1 3/2 2`, plus FUNC for the odd ratios.
  "FOLLOW" (the Rytm's) stays the default.
- **Data:** the track header's **dead speed byte** [code]: 0 = follow the Rytm, else an index into a ratio table.
  Old readers skip it.
- **Engine:** the track's step clock today is the Rytm track's tick rate. A ratio r means a virtual step clock,
  `abs_v = floor(abs_rytm_steps x r)`, with sub-tick positions from `frame_of()`. The planner already plans by frame,
  so this is a second, fractional clock derived from the same anchor.
  - The pattern switch and A:B still count the Rytm's linear cycles.
  - At ratios > 1, more than one Sidecar step falls between two ticks. Raise `guard` 16 and test at 300 BPM x 2.
- **Timing risk:** medium. When the Rytm track's own speed changes mid-pattern, the anchor must re-derive, as
  `StepClock::tick` re-anchors on a jump.
- **Effort:** S-M.
- **Depends on:** nothing. The byte is there.

### P3. Step loop and "hold" (MC-707 step loop, Octatrack/OP-Z style)

- **Musically:** while held, the track loops a range (1, 2, 4 or 8 steps from the current one, or a range of held
  trigs). The rest of the pattern keeps time and resumes in place when released. It is a live stutter for one poly
  track.
- **New vs stock:** the Rytm's retrig repeats one trig. It cannot loop a range of steps per track.
- **Rytm UI:**
  - In seq mode: **hold UP + two trigs** to loop from the first to the second, or UP + one trig for a 1-step loop.
  - Release UP to resume. FUNC latches the loop.
  - The LEDs show the range in the loop colour.
  - UP + trig in seq mode must be checked against the KEYS map.
- **Touchscreen:** drag across the page strip to loop that range (a performance gesture).
- **Data:** none (performance state). An optional "saved loop" could be an E2 chunk.
- **Engine:** E1 maps `abs` into the loop range while the loop is active. Entering the loop replans the steps already
  planned in the 32 ms window (`cancel_ons` + `planned[r]` back, as `drop_prediction` does).
- **Timing risk:** medium. A press within 32 ms of a step either catches it or not. Quantise the start to the next
  step, and say so.
- **Effort:** M.
- **Depends on:** E1.

### P4. Ratchets and rolls per step, poly-aware

- **Musically:** a step fires n times (2..8) within its length or within a set span. Options:
  - a velocity curve (flat, up, down, up-down);
  - a **pitch ramp per repeat** (the chord moves up an octave, or by scale degrees, across the repeats);
  - a **note spread** (the repeats walk the chord's notes, a strummed ratchet).
  This is the trackers' Rxy, the M8's RET, the Pyramid's and the Oxi's ratchets [public].
- **New vs stock:** the Rytm has retrigs on its own tracks (rate, length, velocity curve). It has no retrigs on a
  Sidecar seq track, and no pitch or note spread on any track. The poly spread is new everywhere.
- **Rytm UI:**
  - The TRIG view under held trigs has NOTE VEL LEN MICRO / COND STRUM CHD INV today.
  - Add a second TRIG page: **press TRIG again** cycles NOTES -> TRIG -> RTRG. RTRG has A RATE (1/2..1/32 or n
    in the step), B CNT, C VCRV, D PCRV (pitch curve), E SPRD (chord walk), F..H free.
  - The LED colour marks a ratcheted step.
- **Touchscreen:** a ratchet glyph on the bar (ticks inside the note). Tapping the glyph edits the count.
- **Data:** per step `rtrg {count u8, rate u8, vcurve u8, pcurve s8, spread u8}`, about 5 B, in an E2 step chunk.
  In memory the Step grows by 5 B (about 2.5 KB a pattern; fine on the Pi).
- **Engine:** `plan_step` expands the repeats into note events at plan time.
  - An 8-note chord x 8 repeats is 64 note-ons in one step. Check the event queue's size (`dropped_queue`) and the
    voice pool (6 notes a voice, the oldest stolen). The default spread mode keeps the chord, so a full-chord ratchet
    may steal its own tail.
  - Length clipping: each repeat ends before the next starts.
- **Timing risk:** low per event (frame-accurate). The risk is load on the event queue at fast tempos.
- **Effort:** M.
- **Depends on:** E2.

### P5. Swing groups and groove templates

- **Musically:** swing today delays every second step by one amount. Grooves (MPC / Renoise groove tables) instead
  give each of 2, 3, 4 or 16 steps its own offset and velocity scale, for example a "lazy 3rd 16th" or a shuffle
  that differs between the 1st and 3rd beat. Groove templates can be shared across tracks (the Cirklon's and
  Hapax's groove per track [public]).
- **New vs stock:** the Rytm has one swing per pattern. The Sidecar has one per track. Per-step groove tables are new.
- **Rytm UI:** the TRIG view (nothing held), H = GRV: OFF, SWING (today's), or 1..8 templates.
- **Touchscreen:** a groove editor, 16 sliders for time and velocity, and "extract the groove from the Rytm's own
  pattern". The Rytm's micro timings are in the pattern dump (core/seq/rytm_dump), so the Sidecar track can lock to
  the drums' feel.
- **Data:** the per-track groove id in an E2 chunk. The templates live in a project file (a new registered format,
  `groove`, class P).
- **Engine:** `plan_step`'s swing line generalises to `fs += groove[tick % n].t x per`, plus a velocity scale.
- **Timing risk:** low.
- **Effort:** M. Extracting the groove from the Rytm is an extra S.
- **Depends on:** E2. The "from the Rytm" part depends on the pattern dump (built).

### P6. Polymeter helpers: per-page lengths and "length per cycle"

- **Musically:** the last page can be shorter (a 13-step page 4 for a 61-step riff). Or the length can change every
  n loops (16, 16, 16, 12), the classic phrase turnaround without a second pattern.
- **New vs stock:** the Rytm has one length per track.
- **UI:** the poly track menu, LENGTH row: `PAGES x STEPS` and a TURN option.
- **Data:** a small E2 track chunk.
- **Engine:** in E1, `len` becomes a function of the cycle. A:B counts cycles whatever their lengths.
- **Effort:** S once E1 exists.
- **Depends on:** E1, E2.

## 2. Variation and chance

### R1. Random notes within a scale, with a seed (the user's idea)

- **Musically:** a step (or a whole track) plays notes chosen at random within a range and the track's scale. It is a
  generative melody or chord voicing that stays in key and can be **repeated exactly** when its seed is fixed. Turn
  the seed and you get a new, equally repeatable variation: "seed 41 is the good one".
- **New vs stock:** the Rytm has probability per trig, but no random pitch, and nothing random on it is repeatable.
  The Sidecar's own probability is not repeatable today either (a free-running LCG) [code].
- **What it picks from:**
  - **Range:** a root note plus OCT 1..4, or LOW..HIGH notes.
  - **Scale and key:** from the track's SCALE (M1: the pattern's key, with a per-track override). Without M1 it is
    chromatic.
  - **Weighting:** FLAT / ROOT (the root 3x) / CHORD (chord tones of the step's CHD, or of the current chord with
    M3, 3x) / LOW or HIGH (a skew toward one end) / STEP (small intervals from the last note: a random walk in
    pitch).
  - **Per step:** a step whose note is "RND" (H1's RND command, or a note flag) picks its pitch, and for a chord, n
    notes without repeats.
  - **Per track:** a track mode "RND NOTES" applies the rule to every step that has notes. The written pitches are
    then ignored, but the rhythm, velocity and length stay.
- **The seed:** E1's seed (the pattern's SEED, or the track's own). Fixed means the same notes on every loop of the
  same index, on every PLAY, and after save / load / bundles. FREE re-rolls at PLAY (or every loop).
  - "Same every loop" leaves the loop index out of the hash, so a 16-step random line repeats like a written one.
    "Per loop" makes loop 1, 2, 3 ... different, yet each is reproducible.
  - **Conditions and loop counters:** the hash includes the A:B loop index, so a 1:2 step plays on the same loops as
    without randomness, with the same notes each time. Conditions decide *if* a step plays, the seed decides *what*
    it plays. A probability on a random step draws from the same seed (with another "purpose" key), so it is
    repeatable too.
- **Print:** bake the current random result into real notes. You choose the loop: "print loop n", or the last loop
  played. The result goes into the steps as plain notes, the step's RND flag is cleared, and it is one undo level.
  Printed notes are never touched by the seed again.
- **Rytm UI:**
  - The TRIG view's **RANDOM** page (nothing held, TRIG pressed until it shows; M1): **A SEED** (0..9999; turned fully down: FREE),
    **B RANGE** (OCT), **C WGHT**, **D MODE** (OFF / STEPS / TRACK). FUNC + turn moves the seed by 100.
  - Under a held trig, the TRIG view's NOTE knob gets a value "RND" below the lowest note.
  - **Print:** the poly track menu, PRINT RND row (YES prints the last loop).
  - The footer shows `SEED 41 FIX` or `SEED FREE`.
- **Touchscreen:** random steps show as hatched bars at their range, and the notes they play this loop as ghost
  notes. A SEED chip shows the seed: tap it to type one, swipe it for ± 1, and a dice icon re-rolls it (picks a new
  fixed seed). A PRINT button prints.
- **Data:** the pattern's SEED (u16 plus a FREE flag), and the track's seed / mode / range / weight. These go in E2
  chunks (about 8 B a track). The per-step RND flag is a note pitch value >= 0x80, shared with M3's functions, so it
  needs v4.
- **Engine:** at plan time in `plan_step`, `pitch = pick(hash(seed_eff, track, loop, step, k), scale, range,
  weight)`. Cheap and deterministic, so a replan or a predicted switch plays the same notes.
- **Effort:** M.
- **Depends on:** E1, E2; M1 for the scale (it works chromatically without it).

#### R1b. A modulatable seed, for performance variation (the user's addition)

The seed becomes a parameter that the Sidecar's LFOs and the Rytm's SCENES and PERF can move. Each whole seed value
is one repeatable variation, so a modulation walks through known variations instead of adding noise.

- **Behaviour:**
  - The effective seed is `SEED + offset`. The offset is the modulation scaled to **SEED RANGE** (n variations,
    2..64) and **quantised to whole seeds**, so seed 41 + 3 always sounds like seed 44.
  - **When a change takes effect: SEED QUANT = STEP / LOOP (default) / BAR.** A change is picked up when the next
    step, loop or bar is planned. Steps already planned (32 ms ahead) keep the old seed, so nothing glitches
    mid-note.
  - **Printed and written notes are unaffected.** Only random content follows the seed: R1's notes, and, as chosen by
    SEED AFFECTS = NOTES / +CHANCE / +ORDER / ALL, also chance, jumps, BROWN / RND direction and mutate.
  - **Sweeps are predictable.** The mapping from modulation to offset is monotonic and has a little hysteresis at
    each boundary, so a value sitting on a boundary does not flicker between two seeds. Sweeping the PERF knob from 0
    to 127 always walks the same seeds in the same order (41, 42, ... 41 + n - 1). With LOOP quantisation, the value
    at the loop boundary wins, so a sweep within one loop lands on one seed.
  - A:B and the loop counters are untouched (the seed changes *what*, never *if*), so conditions stay as written.
- **(a) The Sidecar's LFOs:** a new LFO target, **SQ1..SQ4 SEED** (the seq tracks' seeds), beside V1..V8 and FX1 /
  FX2 (docs/lfo.md). The LFO is sampled at the quantise point, a sample-and-hold. A slow sine or triangle walks back
  and forth through the variations. An LFO synced to the pattern gives a repeating *sequence* of variations, so
  loop 1 is seed 41, loop 2 seed 43, and so on, the same every PLAY. It needs a target kind in core/lfo, the LFO
  page's target list and its file's target names.
- **(b) The Rytm's SCENES and PERF.** Two routes:
  1. **Through a machine parameter (no mod change).** A Rytm scene or PERF can lock any parameter of the track,
     including the Sidecar machine's SRC / SMPL slots. Those values reach the host as the voice's reports
     (`seq::params_in`, before the LFOs) [code]. If a poly machine reserves one slot as SEED MOD (a SMPL page slot,
     beside CHD INV SPR VOI), a scene lock or a PERF assignment on it moves the seed, and the seq quantises it as
     above.
     - Pro: no new protocol, scene morphs and PERF depth work as on any parameter, and the setting lives in the
       Rytm's own kit.
     - Con: it costs one machine slot on every poly machine (a user machine too: a convention in the user-machine
       SDK). The Rytm interpolates a PERF move, so the quantising and the hysteresis matter.
  2. **Through the Rytm's scene / PERF state.** The song recorder already knows scenes as CC 92 (sent with the right
     MIDI CONFIG) and PERF as CC 35..47 (pressure, from the hardware only) [SONG.md, measured / public]. SONG.md
     proposes mod reports of the scene and PERF state that do not depend on MIDI CONFIG, but the 2D kinds it named
     (0B / 0C) are now DIRTY and FILL, so new kinds would be needed. A per-track table would then map scene n to a
     seed offset and PERF amount to an offset range.
     - Pro: no machine slot.
     - Con: a mod change, a per-track table to edit, and PERF only with real pads.
  - **Recommendation:** route 1 first (it works today on the Pi with no Rytm change, and the morph is the Rytm's
    own). Route 2 later, if a machine slot is too costly.
- **Rytm UI:** the RANDOM page gets **E SRNG** (SEED RANGE), **F SQNT** (SEED QUANT) and **G SAFF**
  (SEED AFFECTS). The footer shows the effective seed live (`SEED 41+3`).
- **Touchscreen:** the SEED chip shows the base, the offset and the source (`41 +3 LFO2`).
- **Data:** about 4 B more per track in R1's chunk. The LFO target goes in the LFO file (a new target name; an older
  Sidecar drops the line, as class S files keep unknown lines).
- **Effort:** M for (a) and route 1; M more for route 2.
- **Depends on:** R1; core/lfo (a); a machine-slot convention (b1) or a mod report (b2).

### V1. Conditions and probability per lock (the user's idea)

- **Musically:** the notes play every time, but the filter-opening lock on step 5 applies only 1:4, or 30 % of the
  time. Or the chord plays always and the FX send lock only on FILL. The pattern stays recognisable while its timbre
  varies, a variation that note conditions cannot give.
- **New vs stock:** Elektron conditions are per trig. No Elektron box conditions a single lock [public].
- **Rytm UI:**
  - On a lock page (held trig + SRC / SMPL / FX), **hold the encoder's push** to switch that knob to its COND (or
    FUNC + turn). The knob then shows `1:4` / `30%` instead of the value, and the label gets a "?" mark.
  - An encoder push alone still clears the lock (as today), so the long press is the new gesture. Release returns
    to the value.
  - The bottom bar shows `S5 LEV:34 1:4`.
- **Touchscreen:** the lock lane gets a small "?" over a conditional lock. Tapping the lock shows its card with VALUE
  and COND.
- **Data:** `Lock::pad` -> `cond` (the COND knob's 0..142 code in one byte) [code: the pad exists and is unused]. The
  file's lock grows from 3 to 4 bytes in v4.
- **Engine:** a lock event carries its set. Locks whose condition fails are left out of the set at plan time.
  - "Hold and restore": a failed lock means "this param goes back to its base on this step", the same as a step that
    does not lock it.
  - A per-lock PRE / NEI makes little sense. Restrict the lock's COND to FILL / !FILL, 1ST / !1ST, A:B and the
    probability, and do not let a lock's result set PRE.
  - Uses E1's RNG for the probability, so a replan agrees.
- **Timing risk:** none (the same event).
- **Effort:** M (most of it is the knob gesture on the Rytm and its mod).
- **Depends on:** E2 (the file), the KEYS-map check of a long encoder push.

### V2. Conditions per note within a step

- **Musically:** the chord's root always plays, its 7th only 50 %, the top note only on FILL. Chords breathe without
  separate steps.
- **New vs stock:** stock has no notes per step at all (one per trig).
- **Rytm UI:** the held step's SEL knob (H) already selects a note (0 ALL, 1..n). With a note selected, the TRIG
  view's COND edits *that note's* condition.
- **Touchscreen:** a "?" on the note bar; the note card gets COND.
- **Data:** `Note` gains `cond` (u8). There is no spare byte today (`fresh` is the playing flag), so the Note grows
  from 6 to 7 B. In the file it is a v4 step chunk.
- **Engine:** in `plan_step`'s note loop, one test per note.
- **Effort:** S-M.
- **Depends on:** E2. It shares V1's knob work.

### V3. Random jumps: to a step, to a page, bounded

- **Musically:** a step can carry "after this step, jump to step N" (the trackers' Bxx / Dxx). With a chance, the
  pattern sometimes takes a detour. A random jump picks a target within a window (±4 steps, the same page, any page)
  or from a set (pages 1 and 3 only). This gives MC-707-style random [public, brief] while keeping the riff's
  vocabulary. "Return after n steps" turns a jump into an excursion.
- **New vs stock:** the Rytm has no jump trigs.
- **Rytm UI:** the FX column (H1) holds JMP commands. A shortcut: **held trig + PAGE + target trig** sets "jump to"
  (PAGE held is the page map, so the target can be on any page).
- **Touchscreen:** an arrow from the step to its target over the roll, with the chance as its label.
- **Data:** an FX-column command (H1).
- **Engine:** E1's `step_for` now has state (the last step's jump result). Jumps are therefore evaluated in order,
  step after step, as the planner already plans them. A replan restarts from the last *played* step's state, not
  from `abs % len`. That is the one stateful part of E1, and its tests matter.
  - The pattern switch still counts linearly (constraint 1).
- **Timing risk:** medium. A replan after an edit inside the 32 ms window must replay the same jump decisions, which
  the keyed RNG (E1) gives.
- **Effort:** M-L.
- **Depends on:** E1, H1.

### V4. Markov / "next step" chains

- **Musically:** for each step, a small table of "what comes next" weights. The track is a little machine that
  improvises within transitions the user chose. It is BROWN generalised, the Hapax's generative modes and the
  trackers' chance columns taken further [public].
- **New vs stock:** nothing like it on the Rytm.
- **Rytm UI:** too dense for the 128x64 screen beyond one shortcut. In the FX column, a step's NXT command picks 2
  targets with a weight.
- **Touchscreen:** a "transitions" overlay with arrows and their thickness, made by drawing from step to step. A
  "learn" button builds the table from the order of the steps as they are played live (with P3 / V3).
- **Data:** an E2 chunk per track: up to 4 `{target, weight}` per step.
- **Engine:** E1 again (stateful, keyed RNG).
- **Effort:** L.
- **Depends on:** E1, E2, V3's state handling.

### V5. Mutate / drift within a scale

- **Musically:** each loop, with a set amount, some notes move by a scale degree, some velocities move, and some
  steps swap with their neighbour. The pattern slowly evolves and never leaves the key. "Drift back" pulls it toward
  the original. Like the Torso T-1's variation, the Oxi's random and the Hapax's "mutate" [public].
- **New vs stock:** the Rytm can only randomise via its own conditions.
- **Rytm UI:** the poly track menu gets a MUTATE row (AMT, SCOPE: pitch / vel / time / order, and LOCK to keep the
  result). FUNC + NO on the SEQ page reverts to the saved pattern (it already reloads `pNNN.sqs`).
- **Touchscreen:** a MUTATE tool, a dice icon on the track chip, and the drift amount as a slider. The original shows
  as ghost bars under the mutated notes.
- **Data:** two options, and the open question 5 is which one.
  - **Playback-only** (recommended): the mutation is computed from the seed and the loop count at plan time, so the
    stored pattern never changes. "LOCK" writes the current result into the steps (one undo level).
  - **Destructive:** each loop edits the steps.
- **Engine:** at plan time, a transform of the step's copy, keyed on (seed, track, loop, step) through E1's RNG.
  The scale comes from H1 / M1's key.
- **Effort:** M.
- **Depends on:** E1, and the scale/key of M1.

### V6. Euclidean distribution of chords, notes and locks (not of trigs)

- **Not new:** a Euclidean *trig* pattern. Stock 1.70+ has a Euclidean mode, and rytm-mods 0002 adds accents.
- **What a poly track adds:** the stock Euclid places trigs. A poly-track version distributes *content* over trigs
  that already exist:
  - a chord progression's chords over n pulses (M3);
  - a chord's notes over the pulses (pulse 1 the root, pulse 2 the 3rd, ...): a Euclidean arpeggio;
  - a lock's values over pulses, for example a cutoff accent on 3 of 8;
  - ratchets on pulses (Torso's "repeats" [public]).
  It can also run past 64 steps (up to 128).
- **Reuse the Rytm's own settings:** the host already reads the Rytm's pattern (core/seq/rytm_dump, for the PAT page),
  including the euclid bytes. A seq track could *follow* the Rytm track's Euclid PL1 / PL2 / RO: the poly track's trigs
  come from the Rytm's own generator, and only the notes are the Sidecar's. The pattern dump is fetched, not streamed,
  so following live edits needs a report from the mod [inferred].
- **Rytm UI:** the poly track menu, EUCL row: FOLLOW RYTM / OFF; DIST: chord / notes / lock.
- **Data:** an E2 track chunk.
- **Engine:** at plan time, a pulse counter per track (deterministic per loop).
- **Effort:** M.
- **Depends on:** E2; M3 for chords, the dump report for FOLLOW. **Not on the shortlist.**

### V7. Fill variations per track

- **Musically:** the FILL key does more than gate conditions. Each track can have a FILL *variant*: another page
  (play page 8 while FILL is held), a ratchet multiplier, a direction (REV while FILL is held), or a density boost
  (every probability +30 %).
- **New vs stock:** stock FILL only gates FILL conditions.
- **Rytm UI:** the poly track menu gets a FILL row: OFF / PAGE n / REV / RTRG x2 / +PROB.
- **Data:** an E2 track chunk (2 B).
- **Engine:** FILL arrives as 2D 0C while held (a latched FILL is not reported: fix1042). Plan-ahead means FILL acts
  up to 32 ms late. The step just planned keeps its old decision, as conditions do today.
- **Effort:** S-M.
- **Depends on:** E1 for PAGE / REV.

## 3. Harmony

### M1. Scale and key per pattern, with quantise and scale-aware edits

- **Musically:** a pattern gets a key (C minor) and a scale.
  - Note entry from the encoders steps through scale degrees instead of semitones.
  - Chords built by CHD become diatonic: CHD "triad" on degree 2 of C minor is D dim.
  - Random note ideas (V5, H1's RND) stay in key.
  - Out-of-scale notes are marked, and the user chooses to quantise them or leave them.
- **New vs stock:** the Rytm has no key or scale for its chromatic tracks.
- **Rytm UI:** with nothing held, TRIG again cycles the track pages: DEFAULTS (today's) -> **SCALE** -> RANDOM (R1).
  SCALE has A KEY, B SCL, C QNT (D / E: M5's humanise). The held step's NOTE knob moves by degree when QNT is on.
  FUNC still moves by semitone.
- **Touchscreen:** the KEY tool exists as a display preference (TOUCH-EDITOR.md). It becomes the pattern's real key,
  the field TOUCH-EDITOR.md already proposed.
- **Data:** per pattern `key` (u8) and `scale` (u8). Today the file's header has 3 u16 fields after the version, and
  two of them are written as 0, but old readers do not *check* them. Use an E2 pattern chunk to stay clean.
- **Engine:** none at playback unless "quantise live" is on (then a pitch map in `plan_step`). It is mostly an edit
  aid.
- **Effort:** M (the chord table's diatonic mode is the larger part).
- **Depends on:** E2. It is a prerequisite of M2, M3, V5 and H1's RND.

### M2. Live transpose from pads or a keyboard (Cirklon / MC-707 / Oxi "keyboard transpose")

- **Musically:** while playing, a pad press (CHRO) or a MIDI key sets the transposition of the poly track or tracks.
  This is the 80s sequencer trick: one riff played in many keys from the left hand. With M1 the transposition can be
  *diatonic*: the riff follows the scale and does not shift chromatically.
- **New vs stock:** the Rytm has no live transpose on chromatic tracks.
- **Rytm UI:**
  - Arm it from the poly track menu (TRANSPOSE: OFF / PADS / KEYS / BOTH), or with UP + a free trig (KEYS-map
    check).
  - When armed, CHRO pads on the seq track transpose instead of playing notes. The SEQ page shows `TRSP +5`.
  - A pad in the normal pad mode stays stock's, so this follows bug 5's rule: chromatic notes on the pads only in
    CHRO.
- **Touchscreen:** a TRSP chip, and a "latch" or "momentary" toggle.
- **Data:** the transposition is performance state. An optional "save transpose with the pattern" is an E2 byte.
- **Engine:** a pitch offset in `plan_step`, picked up at plan time, so it applies up to 32 ms late. Notes that already
  sound keep their pitch. Make "quantise the transpose to the next step / bar" an option.
- **Timing risk:** the 32 ms plan-ahead. The fix is to replan the not-yet-started events of the window on a change
  (`cancel_ons` + replan, as drop_prediction does).
- **Effort:** M.
- **Depends on:** M1 for diatonic transpose. core/keys.h has the keyboard side.

### M3. Chord progression per page / per bar

- **Musically:** define a progression once (Cm7, Abmaj7, Fm7, G7, one per page or per bar). Steps then hold *chord
  functions* (root, 3rd, 5th, "the chord", arp degree 2) instead of fixed pitches, so a bass line, a pad and an arp
  all follow the progression. Changing one chord re-harmonises every step. The Hapax's and Oxi's chord and harmony
  tracks, and the Orba/Ableton "follow chords" [public].
- **New vs stock:** nothing like it on the Rytm.
- **Rytm UI:** a progression row on the SEQ page's second view (one chord per page, edited with PAGE + CHD). A step's
  NOTE knob gets a "FN" range (functions) beyond the pitches.
- **Touchscreen:** a chord lane above the roll, with the chord stamp tool's chords dragged onto it.
- **Data:** an E2 pattern chunk (16 chords x {root, chd, inv}). A note gains a "function" form: `pitch` >= 0x80 means
  a function, an unambiguous use because pitches are 7-bit today [code: `& 0x7F` on read]. This needs v4 so old
  readers never see it.
- **Engine:** pitch resolution in `plan_step` from the current chord, cheap.
- **Effort:** L (the UX is the cost).
- **Depends on:** M1, E2.

### M4. Arpeggiator per step

- **Musically:** a step's chord is played as an arpeggio for the step's length, or for n steps:
  - patterns UP / DOWN / UPDN / RND / AS PLAYED / CONVERGE;
  - a rate;
  - an octave range;
  - a gate.
  Different steps can arp at different rates. It is the Oxi's arp modes and the A4's arp, but per step instead of per
  track [public]. The POLYPHONY.md research already notes an arp on the chromatic page as a common pattern.
- **New vs stock:** the Rytm has no arpeggiator.
- **Rytm UI:** the RTRG page (P4) gets an ARP mode: SPRD set to "arp" turns ratchets into an arpeggio (one note each,
  walking the chord). A arp mode, B rate, C octaves, D gate. P4 and M4 share the UI and the engine.
- **Touchscreen:** the arp's notes drawn as ghost ticks inside the chord's span.
- **Data:** P4's `rtrg` chunk with an `arp` mode byte.
- **Engine:** the expansion at plan time, as P4.
- **Effort:** S once P4 exists.
- **Depends on:** P4.

### M5. Strum and humanise as playback, not only as edits

- **Musically:** today strum and humanise are edits. They write micro and velocity into the notes (TOUCH-EDITOR.md).
  A *playback* humanise (a little new randomness each loop) keeps a loop from sounding copy-pasted. A "strum by
  velocity" (louder chords strum faster) feels like a hand.
- **New vs stock:** the Rytm has neither for its tracks.
- **Rytm UI:** the TRIG view's SCALE page (M1), D HUM T / E HUM V.
- **Data:** an E2 track chunk (2 B).
- **Engine:** E1's RNG keyed on (loop, step, note) in `plan_step`.
- **Effort:** S.
- **Depends on:** E1, E2.

## 4. Performance

### F1. Pattern variations / scenes for the sequencer, with morph

- **Musically:** each track holds up to 4 variations (A..D) of its steps: the verse, the busy version, the breakdown.
  You switch per track live, quantised to the bar, without a Rytm pattern change. Or you morph: per step, the
  probability crossfades from A's notes to B's (like the Octatrack's crossfader, applied to the notes). The Hapax's
  and Pyramid's track variations [public].
- **New vs stock:** Rytm scenes morph parameters, not steps. Its pattern change switches every track at once.
  - Rytm scenes *already* reach the Sidecar machine's params (a scene's SRC locks arrive as voice reports, and
    `seq::params_in` takes them as the base), so parameter morphing is covered.
  - This idea adds the missing part: **the steps themselves**.
- **Rytm UI:**
  - On the SEQ page, **UP + T13..T16** select variation A..D of the shown track (KEYS-map check; UP + T1..T13 are
    taken, so this may need T14..T16 + FUNC).
  - FUNC + UP + Tn copies the current variation to n.
  - Morph sits on the poly track menu, or on the LEVEL encoder while UP is held.
- **Touchscreen:** A B C D chips on the track header and a morph slider.
- **Data:** variations multiply the steps: 4 x 128 x ~104 B is about 53 KB per track in memory (fine on the Pi, the
  undo copies too). In the file each variation is a sparse track block, so v4 with a variation id per track block.
- **Engine:** the planner picks the variation at plan time. The morph uses E1's RNG per step.
- **Effort:** L.
- **Depends on:** E1, E2. It touches copy / paste / undo (each op on the shown variation).

### F2. Stutter / freeze on the playing step

- **Musically:** hold a key and the track repeats the step that is playing now, as a 1-step P3 loop with optional
  ratchet acceleration (the M8 / Octatrack beat-repeat for one poly track).
- **New vs stock:** the Rytm's retrig is per trig, not live on any step.
- **Rytm UI:** UP + YES in seq mode (KEYS-map check; YES held alone is the preview).
- **Effort:** S once P3 exists. **Depends on:** P3.

### F3. Reverse on the fly and "rewind"

- **Musically:** a momentary REV. While held, the track plays backwards from where it is, and on release it continues
  forwards from there. With "rewind", on release it jumps to where it would have been.
- **New vs stock:** not on the Rytm.
- **UI:** a performance key (UP + a trig) or the FILL variant (V7).
- **Effort:** S once P1 exists. **Depends on:** P1, E1.

### F4. Step mute / solo masks

- **Musically:** mute the steps of a mask (every off-beat, page 2, every 3rd step) on a track for a while, without
  editing. Solo shows a pattern's skeleton.
- **New vs stock:** the Rytm mutes whole tracks.
- **Rytm UI:** in seq mode, hold UP and tap trigs to toggle their "performance mute". The LEDs dim. Release UP and the
  masks stay; UP + NO clears them.
- **Data:** performance state. Bit 1 (muted) is the stored mute.
- **Effort:** S.
- **Depends on:** none.

## 5. Modulation

### X1. Parameter slides between locks (Octatrack / A4 slide trigs)

- **Musically:** a locked parameter glides from its value to the next step's lock over the step, instead of jumping.
  This gives filter sweeps across a bar from two locks. Per lock or per step.
- **New vs stock:** for the Sidecar machine's params and FX this is new. The Rytm's own slides (if any on the MKII)
  apply to its own params only [public, unverified].
- **Rytm UI:**
  - On a lock page, FUNC + an encoder's push toggles "slide" on that lock (V1's long press stays COND).
  - The knob's frame shows a diagonal mark. The step's LED gets the slide colour.
- **Touchscreen:** the lock lane draws a ramp between the steps.
- **Data:** one bit per lock. `Lock::pad` holds the COND code (0..142, 8 bits), so the slide needs a second byte in
  memory. In the file it is a v4 lock flags byte.
- **Engine:** today a lock set is one event. A slide needs a ramp, a value per render block (12 frames) between two
  frames, applied on the control side in `pre_block`, as the LFOs are. The "next lock" is known when the next step is
  planned (32 ms ahead). For steps further apart, the ramp starts when the target is planned. Plan the target step at
  the slide's start instead (look ahead in step indices, not frames).
- **Timing risk:** medium. The direction and jumps (E1) mean the "next step" is the *mapped* next step, which E1
  gives deterministically.
- **Effort:** M.
- **Depends on:** E1 (the next step), E2.

### X2. Per-step LFO depth / phase (Cirklon's aux, Hapax's "LFO to step")

- **Musically:** the track's Sidecar LFO (core/lfo) gets its depth or phase locked per step. A wobble on the long
  chords only, or the LFO re-synced on step 1 and step 9.
- **New vs stock:** Rytm LFO locks exist for its own LFO. The Sidecar's LFOs are not lockable per step: held trig +
  LFO shows "not locked per step" today [code].
- **Rytm UI:** **held trig + LFO** becomes the LOCK LFO page: A DEPTH, B RATE, C PHASE, D RETRIG, E..H for the second
  LFO. This finally gives the LFO key a function under held trigs.
- **Touchscreen:** the lock lane shows LFO locks in their own colour.
- **Data:** new lock params beyond `kLockParams` 32 (for example 32..39). An older Sidecar drops params >=
  `kLockParams` when it reads the file, as fx-ins already does [code]. Raise `kLockParams`.
- **Engine:** the LFO module reads a per-voice override at the step's lock event. Hold and restore, as with locks.
- **Effort:** M.
- **Depends on:** core/lfo's per-voice state, which needs checking.

### X3. Automation lanes (continuous, beyond steps)

- **Musically:** a lane per parameter, drawn on the touchscreen as a curve over the pattern's length, independent of
  steps. A slow filter sweep over 8 pages without 128 locks. Also record knob moves live into a lane (SONG RECORD
  already records knob moves as CC / NRPN for the song replay; this is the pattern-level counterpart).
- **New vs stock:** the Rytm's motion is p-lock recording on steps only.
- **Rytm UI:** recording only: in live REC, turning a Sidecar machine knob writes the lane, an option in the poly
  track menu, "REC KNOBS: LOCKS / LANE".
- **Touchscreen:** the editor (curve points, draw).
- **Data:** an E2 chunk per lane: points `{pos u16 (1/96 step), value u14}`, up to 64 a lane, 4 lanes a track.
- **Engine:** like X1's ramps, per block. A lane's value is the base under the locks (a lock wins on its step).
- **Effort:** L.
- **Depends on:** X1's ramp machinery, E2.

## 6. The tracker-style FX column (H)

### H1. Two per-step command columns

- **Musically:** each step gets two "FX" slots `{command, value}`, as the M8's FX columns, Renoise's effect columns
  and the Polyend Tracker's FX [public]. It is a compact, general place for the ideas above that are *per step*,
  plus small extras:

| cmd | value | meaning |
|---|---|---|
| RTG | n / rate | ratchet (P4's quick form) |
| DLY | ticks | delay the step by n/96 (a whole-step micro; micro per note stays) |
| CUT | ticks | cut every note of the step after n/96 |
| CHN | 0..100 | chance for this column's other command |
| RND | range | random note within ±range (scale degrees with M1) |
| RNV | range | random velocity ±range |
| JMP | step | after this step, go to step (V3) |
| JRN | window | random jump within ±window / page (V3) |
| LOP | n x | loop the last n steps x times, then continue (a written step loop) |
| SKP | n | skip the next n steps (a written "jump forward") |
| OCT | ±n | octave shift for this step |
| TRS | ±n | transpose this step (degrees with M1) |
| SPD | ratio | from here, the track's speed changes (P2) |
| DIR | mode | from here, direction (P1) |
| FIL | page | on FILL, play this page instead |

- **New vs stock:** the Rytm has no command column.
- **Rytm UI:** under held trigs, TRIG cycles NOTES -> TRIG -> RTRG (P4) -> **FX**: A CMD1, B VAL1, C CMD2, D VAL2. The SEQ page's strip marks steps with
  commands. This is the densest page and the one most worth the touchscreen.
- **Touchscreen:** a "tracker view" toggle on the SEQ page: rows = steps, columns = notes / vel / FX1 / FX2. Every
  tracker user would feel at home, and it is the natural place for H1 on a big screen.
- **Data:** per step 2 x 2 B, in an E2 step chunk (Step +4 B in memory).
- **Engine:** some commands act at plan time on the step (RTG, DLY, CUT, RND, RNV, OCT, TRS, CHN), cheap and
  deterministic with E1's RNG. Some change the step order (JMP, JRN, LOP, SKP, DIR, SPD) and go through E1's stateful
  step map.
- **Timing risk:** as V3. DLY beyond a step needs the planner's "a note up to a step early" window to also cover
  "late": today notes may be up to one step late (micro ±96), so DLY should be clamped to ±96 as well.
- **Effort:** L (the commands are each small; E1's state, the UI and the tracker view make it L).
- **Depends on:** E1, E2. It can be cut into slices: plan-time commands first, order commands later.

## 7. Shortlist: the top 8 and a build order

The enablers come first. They are small, and nearly every idea below needs them:

- **Step 0, E1 + E2** (S-M): the deterministic, keyed step map (`step_for`) and `.sqp` v4 with the chunk trailer.
  The registry entry moves to v4 (reads 1..4); `sim/protocol.py`, the golden and fuzz files follow.

Then the top 8, in build order (**all built on the host, seqtop 2026-10-22**: docs/SEQUENCER.md "Built: seqtop"):

| # | idea | why it is on the list | effort | depends on |
|---|---|---|---|---|
| 1 | **P2 per-track speed ratios** (3/4, 2/3, 5/4 ... over the Rytm track's own scale) | the cheapest real gain: the file already has the byte, the planner plans by frame; the Rytm's per-track scale is parity, the ratios beyond it are new | S-M | none |
| 2 | **P1 play direction** (FWD REV PEND PING RND BROWN) | the user's idea; the Rytm plays forward only; it proves E1's step map | M | E1, E2 |
| 3 | **M1 scale/key per pattern** (+ M2 live transpose after it) | R1, V5, M3 and H1's RND all need a key; it also makes CHD diatonic | M (+M) | E2 |
| 4 | **R1 + R1b seeded random notes in a scale, with a modulatable SEED** | the user's favourite: range, scale, weighting, per step or per track, a fixed or FREE seed per pattern / track, print to notes; the seed stepwise-modulated by the Sidecar's LFOs (SQn SEED target) and by Rytm scenes / PERF through a machine SEED MOD slot, quantised to step / loop / bar; one seed makes chance, jumps and directions repeatable too | M (+M) | E1, E2, M1 |
| 5 | **V1 conditions / probability per lock** | the user's idea; no Elektron box conditions a single lock; `Lock::pad` is free | M | E2 |
| 6 | **P4 + M4 poly ratchets with pitch / chord spread and the per-step arp** | the Rytm's retrigs are parity; a chord walk, pitch ramp and arp per step are not, and the Rytm has no arp | M (+S) | E2 |
| 7 | **X1 parameter slides between locks** | the lock system's missing half, on the same per-block path as the LFOs | M | E1, E2 |
| 8 | **H1 FX column + V3 random / bounded jumps** (plan-time commands first, then JMP JRN LOP SKP) | the user's MC-707 / tracker idea; one home for every per-step extra; the touchscreen's tracker view; its jumps and chance use R1's seed | L | E1, E2 |

Not in the top 8, and why:

- **V6 Euclid:** stock 1.70+ has a Euclidean mode, and rytm-mods 0002 adds accents. The poly-track version only adds
  the distribution of chords, notes and locks, and following the Rytm's own Euclid. It is worth it later, not first.
- **P3 step loop and F2 stutter:** great live tools, small once E1 exists. A strong ninth.
- **V5 mutate / drift:** needs M1 and R1's seed first; a natural follow-up to R1.
- **F1 variations / morph:** large, and it touches copy / paste / undo.
- **M3 chord progression:** large UX cost.
- **X3 automation lanes:** large; X1 first.
- **V4 Markov chains:** large; V3 first.

Suggested phases:

1. E1 (with the seed) + E2, then P2 and P1. One branch, one Rytm mod change: the TRIG view's E / F knobs.
2. M1, then R1. R1b's LFO target and the SEED MOD slot come next.
3. V1, then P4 / M4. The Rytm-side gestures are the long encoder push and TRIG's RTRG page.
4. X1, M2.
5. H1 (plan-time commands), then V3 (order commands), then P3 / F2.

Each phase needs:

- unit tests in `core/tests/test_seq.cpp`, including "the same seed and `abs` give the same step, notes and decision
  over a replan, a predicted switch and a reload";
- an e2e case in `sim/seq_e2e.py` against rytm-emu;
- a v4 golden file.

## 8. Open questions for the user

1. **v4 and older Sidecars:** is it acceptable that a pattern using any new feature is *locked* (not playable) on a
   0.3-era Sidecar? The alternative is "plays without the new features, and its save drops them", which fix1042's
   rule argues against.
   **Decided (seqx, the doc's recommendation):** locked. Only a pattern that uses a new feature is written as v4 (a
   fixed seed, a track's own seed, a poly config, a kept block); every other pattern stays v2 / v3 and plays on an
   older Sidecar as before.
2. **Key budget:** several ideas want UP + trig gestures in seq mode, plus a long encoder push on lock pages and a
   third TRIG page. Which gestures does the planned KEYS map (UP + TRIG / SRC / LFO / FX / YES, UP + T1..T13) leave
   free? Should performance gestures (P3 step loop, F4 masks, F1 variations) use UP + trigs *only in seq mode*, where
   T1..T16 are the host's anyway?
   **Decided (2026-10-22, the recommendation):** performance gestures use UP + trigs only in seq mode, and never a key
   the UP map already uses (UP + T1..T13, UP + TRIG / SRC / LFO / FX / YES / AMP, UP + REC / STOP / PLAY with a held
   trig). Host side only for now: every feature on the console, the web page and the touchscreen's SEQ page; the
   intended Rytm keys are documented (SEQUENCER.md "For the Rytm side"; none of the top 8 needs UP).
3. **Swing under REV / RND:** should swing follow the tick (the groove stays, the recommendation) or the step (the
   step keeps its own lateness)?
   **Decided (2026-10-22):** the tick (built: under any direction and under jumps; FWD keeps the step's own parity, as
   before).
4. **The seed's scope and default:** should a new pattern start with a fixed seed (repeatable unless you choose FREE)
   or with FREE? Should the seed be per pattern with per-track offsets (the proposal), or fully per track? And should
   today's probability trigs move onto the seed (repeatable 50 % trigs, a behaviour change)?
   **Decided (seqx):** per pattern, a track may have its own (the proposal; "same every loop" per track); a new
   pattern starts FREE (a new seed at each PLAY: today's feel, and no v4 file until a seed is set); today's probability
   draws from the keyed RNG (E1's text: repeatable with a fixed seed, and a replan always agrees).
9. **SEED from scenes / PERF:** is one machine slot (SEED MOD on the SMPL page of every poly machine, and a convention
   for user machines) an acceptable price for scene / PERF control without a mod change? Or should the mod report the
   scene and PERF state instead?
   **Decided (2026-10-22):** a machine slot (SEED MOD), no mod change. Built: `seq=smod:T:src=N` reads the voice's
   param N as it plays (scene / PERF locks and the Sidecar's LFOs included); naming the slot SEED on the Rytm is the
   Rytm side.
10. **SEED QUANT default:** LOOP (the safest, the proposal), BAR, or STEP?
   **Decided (2026-10-22):** LOOP (built; STEP and BAR selectable).
5. **Mutate:** playback-only (the stored pattern never changes, LOCK bakes it) or destructive?
   **Decided (2026-10-22):** playback-only; LOCK bakes it (V5 is not built yet).
6. **The tracker view:** is a tracker-style list view on the touchscreen wanted beside the piano roll, or is the FX
   column enough on the Rytm's TRIG page?
   **Decided (2026-10-22):** the FX column on the Rytm's TRIG page first; the host data model and the touchscreen / web
   editing now (built); a full tracker view later.
7. **The A4 later:** should the A4 port reuse its own arp and slides (parity there) or always use the Sidecar's (the
   same behaviour on both boxes)?
   **Decided (2026-10-22):** the Sidecar's (the same behaviour on both boxes).
8. **Per-track speed ratios:** only Elektron's set (1/8..2, with 3/4 and 3/2), or free ratios (2/3, 5/4, 7/8) for
   polyrhythm, with a slower FUNC knob range for them?
   **Decided (2026-10-22):** Elektron's set plus 2/3, 5/4 and 7/8 on the FUNC range (built).
