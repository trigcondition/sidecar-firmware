# Sidecar | Expander: firmware

> **`dev` is the development branch: unstable, not a release.** It carries work after Sidecar 0.5.2 and can need matching
> builds of both repos; flash at your own risk; keep your 0.5.2 firmware for rollback. Use `main` for a release. Pull
> requests go to `dev` (see CONTRIBUTING.md).

The source of the Sidecar firmware: the engine that runs on a **Raspberry Pi 4B or Pi 5** on the USB port of an Analog
Rytm MKII, or of an Analog Four MKII, and adds voices, effects and a sequencer to it. One SD card boots both boards. The
Pi 4B is the main target, and the one the measurements are made on; the Pi 5 scales its HDMI picture.

> **Sidecar Mini (Teensy 4.1) is phased out.** It is frozen at 0.1.0: no new features. Its source stays in `teensy/`
> and in the history (commit `101a3fb`) for reference, and the 0.1.0 release stays downloadable.

This repository holds the source of **Sidecar 0.5.2**, the feature release after 0.5.1.2. It updates the Pi from the web page
(Settings > Update, one `.scu` file) and has Rytm files to flash too (two elemod files, OS 1.74 only; see below for what the Pi does
without them). **What 0.5.2 adds** since 0.5.1.2 / 0.5.1.1, in plain words:

- **MORPH** is on: the scenes A and B and the fader that morphs between them, from the QUICK PERF pot, the MIDI page's learn
  rows, the XY pad or the MORPH page (Rytm, touchscreen and web). **Scene edit on the Rytm's own pages:** hold PAGE + T15 / T16
  and turn a knob to lock that parameter in scene A / B; **press the knob** to remove that lock again. A Sidecar machine's source
  word (the SRC page's sample, table or preset) is not lockable under a side, and the readout shows over that view. The Rytm's own
  parameters morph on the Rytm when its mod can (the older path stays for stepped words and older mods).
- **SIDECHAIN** (INSERT FX and MASTER FX, a stage on every insert chain): PUMP from the trigs and DUCK from a source, with the
  key's band (KBAS, KWID) and MON to hear the key; a view on the Rytm (DOWN on the EQ view) and Mix > Sidechain in the web page.
- **WAVETABLE v2:** a second table (TBL B, POS B, MIX), nine warps and eight filter types; **MAKE WT** turns a sample region into
  a table in SAMPLE EDIT; table B has its own pool browser. Old WARP values above 14 sound different and chords are gone (a sound
  with a chord plays its root): see docs/WAVETABLE.md.
- **MIDI page:** LEARN FADER, LEARN SCENE A and LEARN SCENE B rows.
- **CHORD REC:** live recording is mono by default; a per-track setting records chords.
- **Themes:** the pixel font as an accent in the SIDECAR contrasts and a header status strip with the Rytm's pattern and tempo.
- **FILL and the connect:** the 0.5.1.2 fixes (FILL claims its keys without a hello, the CONNECTED screen) and a **re-hello**: a Pi
  that missed the Rytm's hello asks for it again (a Rytm with the 0.5.2 mod answers).
- **The looper is not in 0.5.2.** It is switched off in the build (no LOOP page, nothing recorded, played or muted on the Rytm)
  and comes back redesigned in a later release (docs/looper.md).

**Flash the Rytm's 0.5.2 files too:** Pi 0.5.2 with Rytm 0.5.2 is the tested pair. The Pi 0.5.2 is built to keep working with a
Rytm still on 0.5.1.1 (what worked with 0.5.1.1 keeps working), but that pair was not tested on hardware. The new Rytm-side parts
need the 0.5.2 files: scene edit on the Rytm's own pages (PAGE + T15 / T16, the knob press), the Rytm-side morph, table B's pool
browser, the Rytm's answer to the re-hello, and the new labels for WAVETABLE's P13..P16.

Release notes: <https://trigcondition.github.io/sidecar-expander/release-notes/#sidecar-052>.

Sidecar 0.5.1.2 was a small Pi-only fix after 0.5.1.1 (the Rytm files stayed 0.5.1.1's). Its fixes: FILL opens after the Pi starts
or updates while the Rytm is already on (the Pi claims the keys on the first Rytm message it hears and again about every 2 s), the
CONNECTED screen no longer stays up or comes back, and `/status` shows `hellos` and `capsKnown`
(<https://trigcondition.github.io/sidecar-expander/release-notes/#sidecar-0512>).

Sidecar 0.5.1.1 was the fixes release after 0.5.1 (a fourth number: it sits between 0.5.1 and
0.5.2). It updates the Pi from the web page (Settings > Update, one `.scu` file) on a Pi that runs 0.5.1 or 0.5.0, and the Rytm
side has two small elemod files to flash (the Pi 0.5.1.1 also runs with the Rytm still on 0.5.0). **What 0.5.1.1 adds** since
0.5.1, in plain words:

- **Download the black box** from Settings > About in the web page: one file to send with a bug report, secrets masked.
- **Lag diagnostics:** a "Lag now" marker and the Rytm's report timing in the black box; loading a project no longer holds
  the control pass for 10 ms.
- **Catalog audio previews:** listen to a community machine in the catalog before you install it (the browser streams it).
- **Plugin API 2 `pool_info` / `pool_read`** now take the SAMPLES list code (`bank * 128 + row`, banks A..H) as well as bank A's
  slot 1..127; slot values 128..1016 used to mean the host's internal line and now mean the list code (128 is bank B's OFF).
  No known plugin used them. The SDK has `rmp_pool_code` and `rmp_pool_code_of_knob`.
- **Fixes:** PLAITS SPEECH no longer drones after stop when HARM moves into a word bank; the remote UI sends no SRC frames
  while the Rytm's machine list covers the view; UP + T5 opens the FX page (Send FX), as the sidebar's FX key does; no endless
  loop when POOL is opened from a PRESET page, and DONE returns to where it came from; FILL's undo and the pattern writer
  survive the Rytm repeating its project report; SPACE has no runaway hiss before the tape has heard a signal; the library
  walk no longer writes past its level table at 12 folder levels deep.
- **Versions** may have a fourth number: the update check and the web page's version note read it (0.5.1 < 0.5.1.1 < 0.5.2);
  plugins and the catalog keep three numbers (docs/versioning.md).

Sidecar 0.5.1 added the new touchscreen themes (SIDECAR in the GRID layout language, GRID, and the contrast options High blue,
High green and Viper) with sections and sub-page rows, HOME as Overview and Perform, bigger touch targets and touch controls, the
**catalog** in the web page, the **BP303** bass machine, the headless remote service on the USB-C cable (the protocol is open,
docs/remote-ui.md), and a fix for a USB-C cable pulled in the middle of traffic. Sidecar 0.5.0 added the SAMPLER machine, a better CHOP, SAMPLE EDIT, FILL,
the Rytm's own parameters on the XY pad, MIDI learn and poly-track p-locks, A and B LFOs, user send effects, the MIST texture machine,
the new web page (web-next), updating from the web page and SD card images. MORPH, sidechain, the MIDI page's learn
rows and a new WAVETABLE came in 0.5.2.
The release notes have each item's status: <https://trigcondition.github.io/sidecar-expander/release-notes/#sidecar-0511>.

- Release and binaries: <https://github.com/trigcondition/sidecar-firmware/releases/tag/v0.5.2> (the update file,
  the SD card images, the Pi card files, the user machines and effects, the starter content, the release zip); 0.1.0 and
  0.2.0: <https://github.com/trigcondition/sidecar-expander/releases>
- Documentation, the browser patcher and the web console: <https://trigcondition.github.io/sidecar-expander/>
- The Rytm side (the OS mods the Sidecar talks to) and the A4 mod: [sidecar-mods](https://github.com/trigcondition/sidecar-mods),
  its 0.5.2 release (OS 1.74 only): <https://github.com/trigcondition/sidecar-mods/releases/tag/v0.5.2>

> **Unofficial, experimental software.** Not affiliated with, endorsed by or supported by Elektron. No Elektron
> firmware, and nothing extracted from it, is in this repository.

## Layout

| path | what |
|---|---|
| `core/` | the engine core: voices, machines, send FX, LFOs, presets, the remote pages' UI, the recorder, the device profiles; host tests in `core/tests` |
| `teensy/` | Sidecar Mini, frozen at 0.1.0 (for reference): the `voiceloop` sketch, USB host to the Rytm, the plugin loader (`libraries/RytmPlugins`), the Overbridge block codec (`libraries/RytmHost`), setup scripts for the engine libraries |
| `pi/` | Sidecar: the bare-metal engine on Circle (`pi/engine`, `pi/circle`), the display UI (`pi/ui`), Linux experiments (`pi/linux`) |
| `plugins/` | user machine plugins (`.a64.rmp` on the Pi): the six starter machines `sc_*` (MIT), NUSAW, VOX, HUSH1, RAFFO, BP303 (MIT ports), MD (`mdkit`), SOPHIE (`sophie`, MIT), MODEL E (`modele`, GPL-2.0-or-later, its own licence notes), MIST (`mist`, Plugin API 3, MIT), the user effects `ufx_*` (PHASER, BBD, NIMBUS, GALACTIC; PINGPONG as the example), Noise Plethora packages, test plugins |
| `third_party/` | vendored third-party sources: mdkit (CC0 1.0, the MD machine), TONE3000's NAM A2-Lite engine (MIT, the AMP insert) |
| `sdk/` | the plugin SDK: ABI header, linker scripts, the Teensy Audio shim |
| `sim/` | the Mac-side simulator, protocol and end-to-end tools (some need the emulator, [analog-emu](https://github.com/trigcondition/analog-emu)) |
| `web/` | the Sidecar Mini web console (WebSerial), frozen at 0.1.0 |
| `content/` | the starter content generator (Juno-106 and DX7 patch libraries) |
| `tools/` | packaging, version, plugin pack/install, the portable test runner |
| `docs/` | design notes and measurements |

The version is in `VERSION` (`docs/versioning.md`).

## Build

Third-party engine sources (Mutable Instruments Plaits, DaisySP, Circle, LVGL, ...) are not vendored: the setup scripts
clone each at a pinned commit into git-ignored folders; mdkit and TONE3000's NAM engine are vendored in `third_party/`, ModelDigi in `plugins/modele/third_party/`. See `NOTICE`.

- Engine core on macOS: `make -C core engines && make -C core test` (the last line says `ALL CORE TESTS PASS`).
- Sidecar (Pi 4B and Pi 5): Docker, `pi/engine/build.sh` (`docs/pi-engine.md`, `docs/pi5.md`); one SD card for both boards: `pi/circle/make-sd-dual.sh`.
- Sidecar Mini, frozen at 0.1.0: arduino-cli with Teensyduino, `teensy/run_sketch.sh` (`docs/TEENSY-FIRST-TEST.md`); build it from commit `101a3fb`.
- Plugins: `make -C plugins/sc_acid` and so on (`docs/plugins.md`).
- Starter content: `content/make_content.sh`.

The build scripts were written on macOS; some use BSD `sed -i ''` and macOS tool paths.

## Tests

```
tools/run_portable_tests.sh                  # Linux or macOS: no hardware, no firmware, no cross toolchain
make -C core engines && make -C core test    # macOS
```

Both run on every pull request (`.github/workflows/test.yml`).

## Written offer of source

The released binaries (Sidecar for Pi 4B and Pi 5, version 0.5.2; Sidecar Mini, version 0.1.0) contain GPL and LGPL code, among it Circle
(GPL-3.0-or-later, with its `addon/rp1dsi` display driver linked into the Pi 5 kernel) and the Teensyduino core and SPI library
(LGPL-2.1). The complete corresponding source of version 0.5.2 is this repository at tag `v0.5.2` (of Sidecar Mini 0.1.0: commit `101a3fb`), plus the third-party
sources the build scripts fetch at the pinned commits named in `NOTICE` (Circle with `pi/circle/*.patch`). For three years
from the release, anyone who received a binary can also ask for it on a physical medium for the cost of that medium, or for
the materials needed to relink the Teensy firmware with a modified LGPL part: open an issue in this repository.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). AI coding agents: read [AGENTS.md](AGENTS.md) first.

## Licence

GPL-2.0-or-later (`LICENSE`), except where a file says otherwise; third-party parts and their licences: `NOTICE`,
`THIRD_PARTY_LICENSES`, `licenses/`. The Pi binaries are distributed under GPL-3.0-or-later (they link Circle, and
WAVETABLE's warps and filters in `core/wavetable/wavetable.cpp` are derived from Vital by Matt Tytel,
GPL-3.0-or-later; `NOTICE`).
