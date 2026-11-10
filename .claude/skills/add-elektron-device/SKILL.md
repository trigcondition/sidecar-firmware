---
name: add-elektron-device
description: Add support for another Elektron box (Digitakt, Analog Four, Syntakt, Digitone, Model:Cycles and so on) to the Sidecar engine through a device profile, with or without a firmware mod on the box. Covers deciding what "support" means for that box, the profile table, the hello and trig/params messages, the audio row for Overbridge, state folders, host tests and emulator-only testing. Use it whenever someone wants the Sidecar to work with a box other than the Analog Rytm MKII, asks about device profiles, core/device, porting the Digitakt or A4 work to a new box, or a second box beside the Rytm, even if they only say "can the Sidecar run on my Digitone?".
---

# Add an Elektron device to the Sidecar

The Sidecar was built for the Analog Rytm MKII. A **device profile** (`core/device/device.h`) gives every
assumption about the box one home, and a box picks its profile with a hello. It is a place to plug a port into,
not official support. The Rytm is the default and must never notice the layer.

Read first, in this order:

1. `docs/device-profiles.md`: the messages, pacing, the brackets, audio, state folders, remote UI on a box.
2. `core/device/device.h`, then the two worked examples: `core/device/digitakt.*` (a box with a stereo mix,
   `kBracketMix`) and `core/device/a4.*` (per-voice returns, remote UI, user slots, chords).
3. `core/tests/test_device.cpp`: a minimal second box, tested on the Mac without hardware.
4. The Digitakt page on the docs site (https://trigcondition.github.io/sidecar-expander/developers/digitakt/) for
   the mod side and the emulator.

## 0. Decide what level of support this is

Ask the user which of these they want, and say what each costs. Most ports should start at level 1.

| Level | What the box does | Box firmware | Where the work is |
|---|---|---|---|
| **1. Stock box, no mod** | Overbridge audio to and from the Sidecar, and the Sidecar's machines played from the box's MIDI notes (one MIDI channel per voice) | stock, untouched | the profile's audio row, the USB id, the note adapter; this repository only |
| **2. Profile with a firmware mod** | the box's tracks pick Sidecar machines on their own pages, report trigs and final parameters (after locks and LFOs), maybe show the remote UI | a mod on the box's OS | this repository (profile, tests) and a separate mod repository for the box |

Level 1 is safe and useful on its own: nothing is flashed. Level 2 needs reverse engineering of the box's OS, an
emulator, and a flash at the user's own risk. Do not start level 2 work without the user saying so explicitly.

## 1. Facts about the box, with sources

Before writing a profile, collect and cite (a comment next to every constant):

- **USB ids** per mode (Overbridge mode, USB MIDI, audio/MIDI). Only the Overbridge mode streams.
- **The stream**: channels each way, the block size, which channel is what (MAIN L/R, tracks, inputs). The
  driver (`pi/engine/ob_usb.cpp`) expects alt 3 on interfaces 1 and 2, interrupt IN 0x83 and OUT 0x03, each block
  `32 + 7 × channels × 4` bytes. A box with another shape needs driver work; say so.
- **Tracks and voices**: how many tracks can play a Sidecar voice (`voices` ≤ 8), how many parameters a track can
  send (≤ 16).
- **Where the voices come back**: each voice on its own channel through the box's analog path (the Rytm, the A4),
  or one stereo mix on the main output (the Digitakt).
- **Screen**: a 128 × 64 screen of the Rytm's layout can show the remote UI pages (`kRemoteUi`).

Allowed sources: Elektron's manuals, the user's own observation of their box (USB descriptors, captures,
listening), the open Overwitch project read for facts only (GPL-3: nothing copied), and measurements in an
emulator. **Never guess** a USB id, a channel count or a sample format: ask the user to read it from their device
and give them the command or capture to do it.

## 2. Write the profile

A `device::Profile` in a file of your own under `core/device/` (`<box>.h` / `<box>.cpp`). Fill in `id`, `name`,
USB ids, `tracks`, `voices`, `params`, `head`, the three commands, `machine`, `sound_tag`, `map`/`fixed`, the
bracket, `audio`, `controls`, `features`, `state_dir`, `out_allow`, `in_allow`.

- **The id**: 14 bits; `1` Rytm, `2` Digitakt mk1, `3` Analog Four MKII; `0x3F00` and up for private experiments.
  A public port asks the maintainer for a number (an issue on the repository).
- **The bracket**: `kBracketFull` when the box's own filter, amp and FX shape each voice (per-voice returns);
  `kBracketMix` when the voices come back as one stereo mix and the Sidecar's stages stand in for the box's
  (`controls` maps the box's FILTER/AMP pages onto them). Pick the one that is true; do not fake per-voice routing.
- **Parameters**: if the mod reports like the Rytm, list those commands in `in_allow` and the voice byte carries
  the machine, so each track picks its own machine (the Digitakt does this). Otherwise `map`/`fixed`. Add a
  `translate` function only if neither can say it.
- **`out_allow`** lets nothing through by default. Open only what the box understands; a small CPU should not get
  the Rytm's pages, pool tables and label lists.
- **`state_dir`** (like `/.digitakt`) keeps the box's projects apart from the Rytm's `/.rytm`. Shared content
  (`/Samples`, `/Plugins`, patch libraries) stays shared. The first hello restarts the Pi once into that folder.
- **Register** it in the list in `device.cpp`.

What stays Rytm-only and should not be forced onto a box: kits, patterns, project names, send FX reports, the
labels and presets of the user-slot messages. See the "What is behind the profile" table in the guide.

## 3. Test on the Mac, no hardware

```sh
make -C core engines
make -C core build/test_device && core/build/test_device
make -C core test            # the whole core suite; ends with ALL CORE TESTS PASS
tools/run_portable_tests.sh  # the CI's test check
```

Write the test before the code: a hello is accepted with status 1; a trig and a params message from the box play
the right voice with the right P1..P16; a message of the wrong length or a track past `voices` is dropped and
counted; `out_allow` blocks what the box must not get; a Rytm hello still returns to the Rytm. **Every existing
test must still pass**: the Rytm's path is one pointer test and one compare per incoming SysEx, nothing on the
audio path, and project formats do not change. A change that costs the Rytm more than that is wrong.

For level 1, test the audio row against the driver's descriptor check and the note adapter's channel-to-voice
mapping in the Pi tests (`pi/engine/tests`).

## 4. The mod (level 2 only)

The mod lives in its own repository for that box (the Digitakt's is `trigcondition/sidecar-digitakt`), never in
this one. Its job, per `docs/device-profiles.md`:

1. **Hello** `F0 7D 52 54 4F 01 <id hi> <id lo> 01 [name] F7` at USB port-up and every few seconds.
2. **Trig** in the block it plays, with the values it plays with; **params** when they change; **note-off** if a
   machine holds.
3. **Pace** like the Rytm side: latest value wins (one pending snapshot per track, no queue), at most one params
   message per track every ~10.7 ms, trigs at once ahead of params, never wait in an interrupt, repeat the
   current values every ~0.5 s so a lost message repairs itself.

How to work on it:

- **Emulator first.** `trigcondition/analog-emu` runs Elektron OSes on emulated hardware. If it falls short for
  the box, fix the emulator first, in its own pull request. Every change to the mod is rehearsed there before any
  real unit.
- The user supplies the stock OS file. **No firmware image, OS section, dump or anything derived from one goes into
  any repository**, and no decompiled code into this one.
- Flashing a modded OS is at the user's own risk and is the user's decision, every time. Keep a stock OS file at
  hand to recover. You never flash a device yourself.
- Before any test that sends audio to a real box, remind the user to turn speakers and headphones down: a wrong
  channel count or byte order is loud full-scale noise. Keep the engine's fade-in and its -12 dBFS limit on every
  path toward the box.

## 5. Say what was tested

The pull request (a branch off `dev`, against `dev`, per `CONTRIBUTING.md`) and any docs page say exactly where it
ran: host tests, the emulator, a real box. "Emulator only, never on a real <box>" is a fine and honest status;
the Digitakt shipped that way. Mark anything unconfirmed as "(to verify)".

## Checklist

- The level agreed with the user; level 2 only on their explicit say-so.
- Every new constant has a cited source; nothing guessed.
- Profile in its own file, registered, with a bracket that tells the truth about the box.
- `test_device` covers the hello, the messages, the drops and `out_allow`; the whole core suite and the portable
  tests pass, Rytm behaviour unchanged.
- No firmware or derived data in the diff; no personal data.
- Tested-on stated plainly.
