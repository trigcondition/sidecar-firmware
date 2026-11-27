# The MIDI machine

A machine that sends MIDI instead of playing a note on the Sidecar: a track's trigs, notes and locks go out on a chosen destination and channel. No audio and no voice CPU.
Backlog 7o. Code: `core/midim/midim.h` (read its head first), rytm-mods 0021 and 0026 ("7o" in their design.md). Design and the owner's decisions: kept outside the repo.

## On the Rytm

Choose MIDI in the machine list (machine 50, on every track). Everything is a sound word: saved with kits and sounds, locked per step, modulated by the Rytm's and the Sidecar's LFOs.

| page | A | B | C | D | E | F | G | H |
|---|---|---|---|---|---|---|---|---|
| SRC | DST | CHAN | BANK | SBNK | PROG | PB | AT | MW |
| SMPL | VAL1 | VAL2 | VAL3 | VAL4 | SEL1 | SEL2 | SEL3 | SEL4 |

- **DST:** OFF, DIN (the Rytm's own MIDI OUT, the default), USBC (the computer), USBA (every USB MIDI device in the Pi's USB-A ports). Locks work: a note's off follows the note.
- **CHAN:** 1..16.
- **BANK, SBNK, PROG:** 0 is OFF; else CC 0, CC 32 and the program change send the value minus one. All three go together, in that order, when one changes.
- **PB:** 0 is OFF; 64 is centre. **AT, MW:** 0 is OFF.
- **SELn:** 0 is OFF; else VALn goes out on CC SELn - 1.
- **FLTR, AMP:** do nothing here: the voice is silent. No chord knobs: chords come from the Sidecar sequencer's poly tracks, pads in CHRO and keyboards.

Notes come from trigs (NOTE, VEL, LEN), pads, the Sidecar's poly tracks and keyboards on the Pi. A trig is mono on its track; those others are polyphonic.

## The path

Rytm trig -> `44` (a report as `04`, new command because no voice flag was left) -> `voice_report` -> the note machines' entries (`core/nmach.h`) -> `core/midim`. USB destinations go through `g_AsampMidi` to
core 0. DIN goes to the Rytm as `4F` and out of its MIDI port through the OS's sender.

USB-A never waits: `pi/engine/midiout_q.h` holds a queue per device, one transfer in flight (`pi/circle/circle-midi-async.patch`, applied by `pi/engine/build.sh`). What cannot go is shed clock first, then values, then note-ons; note-offs last, here, in `g_AsampMidi` and on the Rytm's DIN ring. A note-off refused is tried again for 1 s. The Rytm's removal ends the USB notes. An echoed note (a DAW's thru) is dropped: `midim::echo`, 200 ms, slower loops are not caught.

Held notes are in a 64-entry table; see the head of midim.h for what ends them. Non-note data has a byte budget (DIN 2400 B/s); notes never wait. Clock stays the Rytm's: its stock settings
reach DIN and USB-C, and its realtime bytes are forwarded to USB-A while a voice sends there. With two boxes (decided 2026-11-26)
each box's clock goes to USB-A only while its own voices alone send there; while both boxes' voices send to USB-A, no clock goes.

Console: `midim`. Tests: `core/tests/test_midim.cpp`, rytm-mods `tools/emu/mi_emu.py`, `sim/midim_e2e.py`.
