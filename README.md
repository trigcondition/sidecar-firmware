# Sidecar | Expander: firmware

> **`dev` is the development branch: unstable, not a release.** It carries work after Sidecar 0.4.2 and can need matching
> builds of both repos; flash at your own risk; keep your 0.4.2 firmware for rollback. Use `main` for a release. Pull
> requests go to `dev` (see CONTRIBUTING.md).

The source of the Sidecar firmware: the engine that runs on a **Raspberry Pi 4B or Pi 5** on the USB port of an Analog
Rytm MKII, or of an Analog Four MKII, and adds voices, effects and a sequencer to it. One SD card boots both boards. The
Pi 4B is the main target, and the one the measurements are made on; the Pi 5 scales its HDMI picture.

> **Sidecar Mini (Teensy 4.1) is phased out.** It is frozen at 0.1.0: no new features. Its source stays in `teensy/`
> and in the history (commit `101a3fb`) for reference, and the 0.1.0 release stays downloadable.

This repository holds the source of **Sidecar 0.4.2**: Sidecar 0.4.1 with **Overbridge passthrough on the Pi 5** too.
The computer sees the Rytm through the Sidecar (from PR 19 and PR 20 by [Iced-Cream](https://github.com/Iced-Cream)),
chosen with the USB-C mode switch (CLASS COMPLIANT / OVERBRIDGE) on the web page, the Rytm's SIDECAR menu and the
touchscreen, now on the Pi 4 and the Pi 5. A Pi 5 that uses its USB-C for data gets about 1.5 A from a computer or hub
port; a USB-C power + data splitter gives it full power. The Rytm keeps its **Rytm firmware 0.4.0** (OS 1.74 or OS 1.73:
no reflash). 0.4.1 fixed USB dropouts that stopped the Sidecar's voices, brought the Rytm back after a cable pull, added
the USB hub re-arm (PR 15, Iced-Cream), fixed DSP SAMPLE after a machine change and the web page's buttons in Chrome, and
put MODEL E in the plugins zip. 0.4.0 added master FX, the SAT saturator, the MIDI machine, the arpeggiator and arp
locks, the USER SLOTS page, SOPHIE, the JUNO and 6-OP FM preset browser, MD's own page, Tonverk `.elmulti` instruments,
the Pi 5's Touch Display 2 layout and the first release for the **Analog Four MKII** (OS 1.56, with the A4 mod). The
release notes have each item's status: <https://trigcondition.github.io/sidecar-expander/release-notes/#sidecar-042>.

- Release and binaries: <https://github.com/trigcondition/sidecar-firmware/releases/tag/v0.4.2> (the Pi SD cards, the
  user machines, the starter content); 0.1.0 and 0.2.0: <https://github.com/trigcondition/sidecar-expander/releases>
- Documentation, the browser patcher and the web console: <https://trigcondition.github.io/sidecar-expander/>
- The Rytm side (the OS mods the Sidecar talks to) and the A4 mod: [sidecar-mods](https://github.com/trigcondition/sidecar-mods),
  its 0.4.0 release, which 0.4.1 and 0.4.2 use unchanged (one elekloader module per Rytm OS): <https://github.com/trigcondition/sidecar-mods/releases/tag/v0.4.0>

> **Unofficial, experimental software.** Not affiliated with, endorsed by or supported by Elektron. No Elektron
> firmware, and nothing extracted from it, is in this repository.

## Layout

| path | what |
|---|---|
| `core/` | the engine core: voices, machines, send FX, LFOs, presets, the remote pages' UI, the recorder, the device profiles; host tests in `core/tests` |
| `teensy/` | Sidecar Mini, frozen at 0.1.0 (for reference): the `voiceloop` sketch, USB host to the Rytm, the plugin loader (`libraries/RytmPlugins`), the Overbridge block codec (`libraries/RytmHost`), setup scripts for the engine libraries |
| `pi/` | Sidecar: the bare-metal engine on Circle (`pi/engine`, `pi/circle`), the display UI (`pi/ui`), Linux experiments (`pi/linux`) |
| `plugins/` | user machine plugins (`.a64.rmp` on the Pi): the six starter machines `sc_*` (MIT), NUSAW, VOX, HUSH1, RAFFO (MIT ports), MD (`mdkit`), SOPHIE (`sophie`, MIT), MODEL E (`modele`, GPL-2.0-or-later, its own licence notes), Noise Plethora packages, test plugins |
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

The released binaries (Sidecar for Pi 4B and Pi 5, version 0.4.2; Sidecar Mini, version 0.1.0) contain GPL and LGPL code, among it Circle
(GPL-3.0-or-later, with its `addon/rp1dsi` display driver linked into the Pi 5 kernel) and the Teensyduino core and SPI library
(LGPL-2.1). The complete corresponding source of version 0.4.2 is this repository at tag `v0.4.2` (of Sidecar Mini 0.1.0: commit `101a3fb`), plus the third-party
sources the build scripts fetch at the pinned commits named in `NOTICE` (Circle with `pi/circle/*.patch`). For three years
from the release, anyone who received a binary can also ask for it on a physical medium for the cost of that medium, or for
the materials needed to relink the Teensy firmware with a modified LGPL part: open an issue in this repository.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

GPL-2.0-or-later (`LICENSE`), except where a file says otherwise; third-party parts and their licences: `NOTICE`,
`THIRD_PARTY_LICENSES`, `licenses/`.
