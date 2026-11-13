# FILL: p-lock a track's trigs with a range of values

FILL writes one target of one track (the sample, the engine, a preset, the velocity) onto that track's trigs as the
Rytm's own p-locks, from a FROM .. TO range of values, in order or at random: the owner's "p-lock all the trigs of that
track with those samples, in order or at random, like the fill functions in Polyend Tracker". 0.5.0, Pi 4 / Pi 5 and
the Mac tests (`CORE_POLY`); the Sidecar Mini (Teensy) has no FILL. Design: `notes/design sampler-lock-fill`.

Tags as in the other docs: **[measured]** run on the Mac or in rytm-emu and recorded; **[code]** read in the source;
**[open]** to be checked on the real Rytm.

## 1. For players

### Opening it

- **UP + RIGHT** on the Rytm's own screen (hold UP, press RIGHT) opens FILL on the selected track. It needs the 0.5.0
  Rytm firmware (0026 with hello caps2 bit 0); the Sidecar claims the two keys after every hello. **UP + RIGHT** again
  or **NO** closes it.
- The **SIDECAR MENU**'s FILL row opens it too, on any Rytm firmware with the remote screen.
- The page is not on the LEFT / RIGHT page ring: LEFT / RIGHT change the target.
- `fill=keys:off` on the console gives UP + RIGHT back to the Rytm (the claim dropped: `4A 7E 0`) until `fill=keys:on`.
  A planned reboot or power-off of the Sidecar drops the claim too, before the USB stream stops; a Rytm whose USB port
  goes down drops it by itself.

### The knobs

| Knob | | |
|---|---|---|
| A | TARGET | the track's targets (see 2): the machine's main one first, VELOCITY last; also LEFT / RIGHT |
| B | FROM | the first value: a pool row, an engine, a preset, 1..127. FUNC held: a bank (a pool) or 16 at a time |
| C | TO | the last value; below FROM fills downwards |
| D | ORDER | UP, DOWN, PING, RND, SHUF, SPRD |
| E | TRIGS | ALL, EMPTY (only trigs with no value of the target yet), LOCKED (only those with one) |
| F | EVERY | 1..16, counted over the trigs TRIGS leaves |
| G | OFFSET | 0..EVERY-1 |
| H | SEED | 0..999 for RND and SHUF; a new one each time the page opens |

- **YES** fills. **FUNC + YES** clears the target on those trigs (the lock removed, a velocity back to the default).
- **FUNC + NO** undoes the last fill or clear (one level). The undo is gone after a pattern change or a project load,
  and it refuses (`CHANGED SINCE: NO UNDO`) when the filled target was edited since, on the Rytm or by a take: it never
  writes the old values over newer ones. Edits to other parameters or other tracks don't matter: the undo puts back
  only that target of that track.
- Defaults: the machine's main target, ORDER UP (VELOCITY: RND), TRIGS ALL, a fresh seed. Empty or OFF pool rows and
  empty preset rows are skipped. SAMPLE's FROM / TO: the rows of the track's last quick-load batch (RIGHT in the
  sample list, several files in one opening: FROM the first row, TO the last), else the sample the kit plays.
- Only trigs are filled: trigless steps and steps past the track's length are left alone. In NORMAL scale mode the
  pattern's master length counts (at most 64) [measured: rytm-emu, 0026's SCALE report].

### What it writes, and when

- The values become p-locks in the Rytm's **working pattern**: the pattern is read (SysEx), the one target patched,
  written back and read again to check it. Everything else in the pattern stays byte for byte.
- **Stop the Rytm first.** While it plays, YES answers `STOP TO FILL`. (A write while playing was measured clean in
  the emulator, check E4; `fill=playing:on` allows it.)
- **A fill is kept like an edit made on the Rytm's keys:** it survives a power-off, and SAVE PATTERN saves it as any
  edit. [measured: rytm-emu, OS 1.74 (and 1.73)] The fill is written as the active pattern's own dump (0x54, pattern
  n), which reaches both of the Rytm's copies of the pattern: the one it plays and the stored one its power-off save
  keeps. Until 0.5.0 FILL wrote the working-buffer dump (0x5A), which reached only the copy it plays: the fill played,
  then was gone after a power-off (also after SAVE PATTERN). Before the Sidecar has seen the Rytm's pattern (no
  pattern report yet), YES answers `PATTERN UNKNOWN` and nothing is written. [open] Check on the real Rytm.
- **A fill made while a SONG take is armed is undone by the take's Restore**, as any other edit made then. The SONG
  refuses to arm, to confirm a replay or to undo while a fill is writing (`a FILL is writing the pattern`), and a fill
  stops (`SONG BUSY`, nothing written) when the SONG starts its own dumps. The Restore still writes the old way (until
  a later release): it changes only the copy the Rytm plays, so a fill it removed comes back after a power-off.

### The messages

| Note | Meaning |
|---|---|
| `FILLED 8: FN+NO UNDO` / `CLEARED 8` | done and verified; 8 trigs written |
| `NO CHANGE` / `NOTHING TO CLEAR` | the pattern already held those values |
| `NO TRIGS TO FILL` | no trig on the track (within its length, through TRIGS / EVERY / OFFSET) |
| `NO VALUES IN RANGE` | FROM .. TO holds only empty or OFF rows |
| `LOCKS FULL (72)` | the pattern's 72 lock slots are used; nothing written |
| `STOP TO FILL` | the Rytm plays |
| `SONG BUSY` | the SONG is reading or writing the pattern; nothing written |
| `PATTERN BUSY` | the pattern kept changing while it was read (3 tries); nothing written |
| `CHANGED MEANWHILE: CHECK` | the pattern was changed or switched while the fill was writing; nothing written back. Look at the pattern |
| `NOT TAKEN: UNCHANGED` | the read-back showed the pattern as before the write, twice: the Rytm didn't take it; nothing written back |
| `VERIFY FAILED: RESTORED` | the read-back differed; the pattern as read before was written back |
| `NOT CONFIRMED: CHECK IT` | no read-back, and the pattern moved meanwhile, so nothing was written back. Look at the pattern |
| `NO ANSWER` | the Rytm didn't answer the pattern request (5 tries) |
| `UNDONE` | the undo is done |
| `UNDO EXPIRED (PATTERN)` / `UNDO EXPIRED (PROJECT)` | a pattern change or a project load since the fill |
| `CHANGED SINCE: NO UNDO` | the filled target was edited since; nothing written |

## 2. Targets

The track's machine says what it can fill (`core/seq/filltargets.h`) [code]:

| Machine | Target | Stored as | Values |
|---|---|---|---|
| SAMPLER, DSP SAMPLE | SAMPLE | P11 (lock type 10) + fine | the SAMPLES pool's rows A001..H127 [measured] |
| GRANULAR, WAVETABLE, MULTISAMPLE | SOURCE, TABLE, INST | P1 (type 0) + fine | the rows of its pool list [measured] |
| MD | ENGINE, TYPE | P9 (type 8), P10 (type 9) | the engines; TYPE: the types of the track's ENGINE |
| PLAITS | ENGINE | P1 (type 0) | the curated models |
| DAISYSP | ENGINE | P1 (type 0) | MODAL, STRING, RESON |
| JUNO, 6-OP FM | PRESET | P1 = PAT (type 0) | the project's preset rows |
| a user machine (U1..U5) | from its knobs | its P | PRESET (its presets), SAMPLE (pool slot 1..127), list knobs (their names); at most 4 |
| every track | VELOCITY | the track's velocity array | 1..127 |

MD's TYPE lists the types of the engine the track plays now; a TYPE fill over trigs whose ENGINE is p-locked to other
engines writes type numbers those engines read as their own types.

## 3. How it is built

- `core/seq/lockfill.*` (pure): the pattern record (13101 B: 13 tracks x 641 B at +4, 72 lock slots x 66 B at +0x2091,
  a 12-byte tail with the master length and the scale mode), the steps a fill writes, the value order, fill / clear /
  undo state, and `same()`, the verify's comparison: the Rytm keeps its lock list in its own order, so locks compare
  as a set, both ways [measured: rytm-emu].
- `core/seq/patwrite.*`: one job at a time. 0x64 n asks for the active pattern n by number (its working buffer); an
  answer (0x54, 14988 B) whose byte 9 is not n is never taken (near a queued switch a 0x6A answered the queued
  pattern [measured: rytm-emu 1.74]); the answer is dropped
  and asked again when a DIRTY (2D 0B) or a pattern change (2D 02) comes in meanwhile; the patch; the write (0x54 to
  the active pattern's number, in 3069-byte pieces: it reaches the stored copy a power-off keeps, which a 0x5A does
  not; no 2D 02 seen yet, or none since a Rytm hello: `PATTERN UNKNOWN`, nothing written; a project load since the
  start: `PROJECT LOADED: STOPPED`, nothing written or written back); the read-back. From the write on, a pattern change, or
  more DIRTYs than the write's own two [measured: rytm-emu, a received 0x54 makes two], means the user changed the pattern: nothing is written back. A
  read-back equal to the pattern before the write is asked once more (a late answer); equal again, the job ends
  `NOT TAKEN` without writing anything back. A read-back that comes in while the SONG holds the dumps ends the job
  `SONG BUSY`.
- `core/ui/page_fill.cpp`: the page, the undo (its pattern count from the job's read, the project count, and the
  target as the fill left it).
- `core/seq/filltargets.*`: the registry.

Tests: `core/tests/test_lockfill.cpp` (the record against golden captures from rytm-emu, a fake Rytm that re-sorts the
lock list, every writer path, the page), `core/tests/test_browser.cpp` (the quick-load batch).
