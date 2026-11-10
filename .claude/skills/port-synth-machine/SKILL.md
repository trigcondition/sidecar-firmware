---
name: port-synth-machine
description: Port an open-source synth, drum engine or DSP voice to the Sidecar as a user machine (an .rmp package that plays in the Rytm's U1..U5 slots on the Pi). Covers the licence check, the plugin ABI, mapping controls to P1..P16, presets, mono or poly, the CPU budget, host tests and packaging. Use it whenever someone wants to bring a synth, a drum machine, a VCV/Faust/DaisySP/Mutable-style module, an emulation or "an engine from GitHub" to the Sidecar, asks how to write or package a user machine or plugin, or asks why their .rmp is refused, glitches or sounds wrong, even if they never say "port" or "user machine".
---

# Port a synth as a Sidecar machine

A user machine is a plain C or C++ sound generator behind a fixed C ABI, built for the Pi's AArch64 CPU, packed
into an `.rmp` file and played in one of the Rytm's user slots U1..U5. No kernel build and no Rytm flash: copy it
to the card and assign it.

The full reference is the docs page "Writing user machines"
(https://trigcondition.github.io/sidecar-expander/developers/writing-user-machines/). Read it once before you
start a port; this skill is the order of work and the things that go wrong. The header `sdk/rmp/rmp_api.h` is the
ABI's source of truth.

## 0. The licence gate (before any code)

Do this first, because a port that cannot ship is wasted work. Find the upstream licence file and the files'
headers, and note the repository URL and the exact commit.

| Upstream licence | What to do |
|---|---|
| MIT, BSD, ISC, zlib, Apache-2.0 | Fine. Keep the upstream text as `LICENSE.<name>` in the plugin folder. The glue may be MIT (`plugins/nusaw`, `plugins/raffo`) |
| GPL-2.0-or-later, GPL-3.0, LGPL | Fine, but the package as a whole is then GPL: ship its complete source next to the `.rmp`. `plugins/modele` is the pattern: `LICENSE`, `NOTICE`, and `make package` adds a source tarball |
| GPL-2.0-only | Ask the maintainer first: the repository is GPL-2.0-or-later and the Pi binary GPLv3 |
| Non-commercial (CC-BY-NC, "NC" clauses), "personal use only" | Does not fit the GPL. Leave that code out (the Noise Plethora packages are built without its NC framework) or ask the author for a licence in writing |
| No licence at all | No permission. Ask the author and get the answer in writing, or stop |
| Derived from a manufacturer's firmware, ROM dumps, decompiled code, sample sets ripped from hardware | Stop. Nothing derived from firmware goes into this project |

Also: give the machine your own name. No product or company names in `name`, `short` or the knob labels; describe
a model in the README ("follows the layout of ...") with a trademark line, as `plugins/modele/NOTICE` does.

Tell the user the result of the gate in one or two lines before you continue.

## 1. Read the engine before you design anything

Find out, from the upstream code:

- **Sample rate** it assumes (the Sidecar runs 48 kHz and asks for 12 samples a block). An engine fixed at
  44.1 kHz keeps its rate inside and resamples per voice (`plugins/mdkit` does this).
- **Its parameters**: how many, their ranges and curves, which are discrete (lists, switches).
- **Note and gate**: does it hold while the gate is open, does it glide, is it a one-shot drum.
- **State and memory**: per-voice state size (64 KB limit), any `malloc`, `new`, static constructors, file I/O,
  threads, `double` maths, big tables. Each of these needs a plan (below).
- **Mono or stereo**: `render` is mono. A stereo engine is summed, or its stereo part dropped.
- **Cost**: a rough guess of the work per sample, so you know whether one voice or eight can play.

Write these down as a short port plan and show it to the user before you start writing code.

## 2. The knob map: P1..P16

The Rytm gives a machine 16 parameters, 0..127 with a fraction. P1..P8 are the SRC page; P9..P16 are on the SMPL
page when `"page2": "params"`. Labels are 4 characters on the Rytm (`short`) and 12 for the long name.

- Put what a player turns most on P1..P8. Tuning, the tone controls, the envelope a player shapes per sound.
- A level knob (LEV) is customary near the end. The Rytm's own filter, amp and FX follow the machine anyway, so
  do not spend knobs on an output stage it already has.
- An engine with more than 16 controls: combine. A bipolar knob can hold two (MODEL E's `G<>A`: glide left,
  attack right), a list can hold a range and a wave (36 names from 0.4.0), and a fixed internal value is fine for
  what nobody turns.
- Discrete choices are `"look": "list"` knobs. Read them with exactly `zone = (int)value * count >> 7`, so what
  plays matches the name the Rytm shows.
- A type knob that renames the others: `type_labels` (`plugins/mdkit/md.json`).
- Curves: map 0..127 to the engine's useful range musically (exponential for time and frequency), and make 64 a
  sensible centre for bipolar knobs.

Show the user the map as a table (P, short, long, range, default) before you code it. It is the part they will
live with, and changing it later breaks their saved sounds.

**Keep the `id` stable** across versions: each sound remembers a fingerprint of it, and a new id silences the
users' sounds.

## 3. Write the plugin

Start from a folder under `plugins/<name>/` (Docker mounts only the repository): `Makefile`, `manifest.json`,
the glue C file, and the upstream code vendored unchanged in `third_party/<name>/` or fetched at a pinned commit.
Copy the layout of the closest example:

| Example | Shows |
|---|---|
| `plugins/sc_acid`, the other `sc_*` | small, MIT starter machines |
| `plugins/nusaw` | a third-party MIT engine, poly instancing, `make package`, `make check` |
| `plugins/raffo` | a mono synth with glide |
| `plugins/modele` | a GPL engine: NOTICE, source tarball, 36-name lists |
| `plugins/mdkit` | a drum engine: a union of states, `type_labels`, resampling, no click on a type change |
| `plugins/v2_test` | the pool sample calls |

Rules the build and the engine enforce, and how to meet them:

- **No allocator.** Everything mutable goes in the per-voice state (`state_bytes`). Replace `malloc`/`new` with
  arrays in the state, sized for the worst case. The state arrives zeroed before `voice_init`.
- **No static constructors** or function-local statics with dynamic init: the packer refuses them. Construct in
  `voice_init`. C++ works without exceptions or RTTI.
- **No host symbols** (`printf`, `malloc`): the link fails. Prefer `sinf`, `powf`, `expf` over the double ones,
  which pull in large tables.
- **Globals are shared** by every voice of the slot, rendered on two cores at once. Only constants, tables built
  in `init`, and the host table may live there.
- **Flush denormals** yourself in feedback paths and filters.
- **Keep control calls short.** `params`, `note_on`, `note_off`, `voice_init` run on the control core between
  blocks, without a guard: a fault there reboots the engine, and slow ones delay the audio feed. Spread heavy
  setup over `render` blocks.
- **`render` writes, it does not add**, mono floats around ±1, for exactly `n` samples. No waiting, no logging.
- A hang in `render` reboots the engine; the guard does not cut an endless loop.

Mono or poly: by default a machine is mono (one state, glide works). For chords on a poly track set
`"max_voices"` 2..8; then everything `render` writes must be in the state. A drum sets `"pitch": "none"` and
usually `"max_voices": 1`, and lets its own decay end the hit.

Presets: a `"text": "preset"` knob picks files from `/PRESETS/<id>/` on the card; native patch files go through
`load_entry` (`"preset_ext"`). Mark the other knobs `preset`, `offset` or `free`.

## 4. Measure the cost

The voice budget is 125 µs a 12-sample block at 1500 MHz, but four voices share a core, so a machine meant for a
full kit should stay under about 50 µs a block. Timings on a Mac are about 13 times too good for a Pi 4 core.

- Copy a `make check` (`plugins/nusaw`, `plugins/modele`): it runs the package on an arm64 Mac as the Pi runs it
  and prints the cost against a known machine.
- Set `cpu_permille` honestly from that: the Pi reads it as `permille × 0.158 %` of a Pi 4 core per instance and
  cuts poly VOICES by it.
- Too slow: compute coefficients in `params` not per sample, use tables built in `init`, lower the internal
  oversampling, use NEON-friendly loops (`-mcpu=cortex-a72` is already set), or drop poly.
- The Pi's web page shows the measured time per voice once the machine has played (`GET /um`).

## 5. Test it on the host

Both checks run without a Sidecar; the docs page has the scripts.

```sh
make -C plugins/<name> a64                    # the package; the last line prints id, image and bss sizes
make -C plugins/<name> lib<name>.dylib        # the Mac library through the same plugin table
make -C core engines lib                      # once: the engine's core library
```

- `tests/native.py`: the Mac library through `sim/rmpvoice.py`: pitch is right, the output is finite and inside
  ±1, a note decays or releases, every list zone plays.
- `tests/core.py`: the real `.rmp` in the real engine core (`sim/coreengine.py`): it scans, assigns
  (`um=assign:1:<id>`), plays, and the slot reports `READY`.
- Compare against the upstream engine where you can (render the same notes and parameters in both), as
  `plugins/mdkit`'s check does against mdkit's own render.
- Listen: render a short WAV of a few notes over the knob range (`make demo` in `plugins/modele`) and give it to
  the user. Numbers miss aliasing, clicks and wrong curves.

Run the repository's tests before a pull request: `tools/run_portable_tests.sh`, and on macOS
`make -C core engines && make -C core test`.

## 6. Package and document

- `make package` (copy from `plugins/nusaw`, or `plugins/modele` for GPL): the `.rmp`, a `LICENSE.txt` with every
  licence involved, `NOTICE.txt`, `SHA256SUMS`, and for GPL code the complete source tarball.
- `NOTICE` in the plugin folder: where the code came from (URL and commit), what you changed, which licence covers
  what. Add the plugin to the repository's top-level `NOTICE` if it goes in the repository.
- `README.md` in the plugin folder: what it is, the knob table, mono or poly, the measured cost and state size,
  what was tested (Mac, emulator, a real Sidecar) and what was not.
- Say plainly what has not run on hardware yet. Untested on a Pi is fine; claiming otherwise is not.

## 7. Contributing it

Work on a branch off `dev` and open the pull request against `dev` (`CONTRIBUTING.md`). One machine per pull
request, with its tests, and the description says what it was tested on. No firmware, no personal data
(home-directory paths, e-mail addresses) in code, docs or commits.

To share a machine without a pull request, publish the `make package` output, with its licence and, for GPL
code, the source.

## Checklist before you call it done

- Licence gate passed and written into `NOTICE`; own machine name.
- `make a64` packs without warnings; `make check` passes; cost measured and `cpu_permille` set from it.
- Knob map agreed with the user; list knobs use the zone formula; `id` final.
- Output finite and inside ±1 over every knob's extremes; no clicks on a note, a type change or a release.
- A demo WAV rendered and handed to the user.
- `make package` output complete; README says what was and was not tested.
