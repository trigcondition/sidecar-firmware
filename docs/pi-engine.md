# The Pi engine (`pi/engine`)

`pi/engine` is a bare-metal Circle kernel for the Raspberry Pi 4. It runs the engine core (`docs/core.md`) as a voice engine for the Rytm. A stock Rytm MKII in OVERBRIDGE mode is its USB clock. The Rytm's MIDI notes play every built-in machine, and the output goes back to the Rytm over Overbridge.

Spec: `docs/superpowers/specs/2026-10-14-pi-engine-design.md`. Plan: `docs/superpowers/plans/2026-10-14-pi-engine-usb.md`. Spike: `docs/pi-circle-spike.md`.

## Build and run

- Once: `pi/circle/build.sh`, which builds the Circle checkout with spike2's two xHCI patches. Then `pi/engine/build.sh`, which builds:
  - the `circle-engine` tree (`--multicore`, FatFs, the SD driver);
  - the engines' sources;
  - the kernel, `pi/engine/kernel8-rpi4.img`. It links Circle (GPLv3), so the built image is GPLv3 as a whole.
  Everything builds in Docker (image `rytm-circle`).
  The Raspberry Pi 5: `RASPPI=5 pi/engine/build.sh` -> `pi/engine/kernel_2712.img` (its own Circle tree, no USB-C gadget), the SD set by `pi/circle/make-sd-pi5.sh`: `docs/pi5.md`.
- After a change: `pi/engine/build.sh kernel && CIRCLE_TREE=circle-engine pi/circle/flash.sh ../engine 45`.
  - `flash.sh` builds the `.hex` and sends the reboot magic (`RYTM-REBOOT`, 921600 baud) to the running kernel.
  - It then uploads through the serial bootloader and logs SECS seconds to `pi/circle/out/engine-<time>.log`.
  - The kernel lives in RAM only: the SD card is not touched, and the next reboot or power cycle boots the card's `engine.img` again (below).
  - Use SECS >= 45 (a 1.55 MB kernel: the go about 29 s after the start). A flasher killed while it writes can hang the Mac's FT232R adapter: the Mac's writes then never complete, so the Pi hears nothing, not even the reboot magic (measured 2026-10-16). `uv run --with pyusb python pi/circle/sdboot.py usbreset` (a USB reset of the adapter, Homebrew's libusb) clears it; earlier "the Pi wedged until a power cycle" was probably this.
- The Pi boots `sdboot` from its SD card: Circle's serial bootloader, patched to start the card's `engine.img` when no flasher answers within 1 s (next section). Without the sdboot lines in `config.txt` it boots Circle's stock serial bootloader as before (`docs/pi-circle-spike.md`, "flash.sh").
- The SD card's FAT32 partition is also the engine's card: `/Samples`, `/JUNO`, `/DX7` and the core's `/.rytm/` index. The first boot writes the self-test content (below).
- Options go in `cmdline.txt` on the SD card, one line, as for the spikes. The firmware hands it to every kernel, the SD's `engine.img` and a flashed RAM kernel alike. Without one, the build's defaults apply (`build.sh`'s `PI_IP`, `PI_NETMASK`, `EMUHOST`, `GADGET`). `pi/circle/sdboot.py cmdline` shows it, `sdboot.py cmdline "notes=on out=main ip=..."` writes it with the card in the Pi (the original kept as `cmdline.txt.bak`). The card had no `cmdline.txt` on 2026-10-16.

## Boot from the SD card (sdboot)

Power on: the engine runs from the card within about 11.5 s (reboot to its first stats line), with no Mac; the display shows the boot screen from 0.27 s of the kernel on. A crash, a guard stop or a watchdog reboot ends in the card's engine too, instead of a bootloader waiting for a reflash.

- The card's boot partition (2026-10-16): the firmware (`start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `armstub8-rpi4.bin`), `config.txt`, Circle's stock serial bootloader as `kernel8-rpi4.img` (unchanged), and:
  - `sdboot.img`: `pi/circle/sdboot.patch` on Circle 51.1.1's `tools/bootloader` (921600 baud), built by `pi/circle/build-sdboot.sh` into `pi/circle/out/sdboot.img`;
  - `engine.img`: a 32-byte header (`SDCRENG1`, the length, two running sums over the 32-bit words) and the raw `kernel8-rpi4.img`; `engine.img.prev`: the one before;
  - at the end of `config.txt`, between `# sidecar sdboot begin` and `# sidecar sdboot end`: `[all]`, `kernel=sdboot.img`, `initramfs engine.img 0x02000000`, `disable_splash=1` (no rainbow square), `boot_delay=0`. Everything above them is the user's, unchanged; `config.txt.bak` is the original.
- The boot: the firmware loads `sdboot.img` as the kernel and `engine.img` at 0x02000000. sdboot arms the BCM watchdog (15 s), checks the header and the sums, prints `sdboot: engine.img N bytes ok, starting in 1 s`, then `IHEX-F`, and waits 1 s for a byte.
  - No byte: the engine is copied to 0x80000 (30 ms) and started as the stock bootloader starts a flashed kernel; the watchdog is off again, and the engine starts its own.
  - A byte (flashy's records come within ms of `IHEX`): it is the stock serial bootloader, with one change: an upload that stalls for 5 s (a flasher killed, a cable pulled) prints `#ERR:timeout` and goes back to the 1 s window, so the card's engine starts after about 6 s. Before, the bootloader waited for a power cycle.
  - No valid `engine.img` (missing, truncated, bad sums): `sdboot: no engine.img (the serial bootloader only)`, and it waits for the flasher as the stock one does.
  - sdboot prints its own lines before `IHEX-F`: a byte that comes while it still prints overran the 32-byte RX FIFO, and flashy's fast mode failed (measured).
- Settings: `cmdline.txt` (above). There is no separate settings file.
- `pi/engine/sdfiles.h`: the running engine reads and writes the card's boot files. A file is written to `NAME.new`, read back and checked (CRC-32), then renamed into place; the old file is kept as `NAME.prev`, and `config.txt`/`cmdline.txt` keep their first version as `NAME.bak`. Two ways in, the same commands: TCP port 7310 (the network; `sdport=0` turns it off; built but not yet measured: no Ethernet link on 2026-10-16) or the serial console's `:sdf` (about 45 KB/s, base64 lines each acknowledged). `pi/circle/sdboot.py` (run as `uv run --with pyserial python pi/circle/sdboot.py ...`) tries TCP for 3 s, then the serial port; `SIDECAR_LINK=serial` goes straight to serial. One process on the serial port at a time.
- The firmware's tryboot: `TRYBOOT` sets the firmware's one-shot reboot flag (mailbox tag 0x00038064) and reboots; that boot reads `tryboot.txt` instead of `config.txt`, the next one `config.txt` again. `sdboot.py tryboot` uses it to boot a new sdboot from the card once, before it is made permanent.

| what | command (from the worktree's root) |
|---|---|
| dev flash (RAM only, as before) | `pi/engine/build.sh kernel && CIRCLE_TREE=circle-engine pi/circle/flash.sh ../engine 45` |
| install a build to the card | `uv run --with pyserial python pi/circle/sdboot.py install` (the running engine writes `pi/engine/kernel8-rpi4.img` as `engine.img`; about 35 s on serial) |
| the build before | `sdboot.py previous` (swaps `engine.img` and `engine.img.prev`) |
| a new sdboot | `pi/circle/build-sdboot.sh`, `sdboot.py bootloader` (as `sdboot-try.img`), `sdboot.py tryboot` (one boot with it), then `sdboot.py commit` |
| the card's files | `sdboot.py ls`, `sdboot.py get NAME [OUT]`, `sdboot.py scratch` (the write path's test: 64 KB written, read back, deleted) |
| off again (the stock bootloader at the next boot) | `sdboot.py disable` (removes the block), or `sdboot.py restore-config` (`config.txt.bak` back) |
| the serial adapter hangs | `uv run --with pyusb python pi/circle/sdboot.py usbreset` |

- Restore by hand (the card in a Mac): copy `config.txt.bak` over `config.txt`; `sdboot.img*` and `engine.img*` can then be deleted. `kernel8-rpi4.img` (the stock bootloader) was never changed.
- Risks: a broken `sdboot.img` made permanent leaves the Pi without a boot until the card is restored in a Mac (hence `tryboot` first); the engine's watchdog then still resets it, into the same sdboot. `config.txt` is replaced by two renames: a power cut between them (milliseconds) leaves no `config.txt`. Port 7310 writes the boot partition for anyone on the network. A build that dies at boot on the card (a fatal bench, a guard stop) reboots into itself, each time through the 1 s window, so `flash.sh` still gets in.

Measured 2026-10-16 (Pi 4 4GB, the card in the Pi, the Rytm attached, no Ethernet link):
- Scratch write, read back, delete: PASS. Installs: `engine.img` (1.56 MB) 35-38 s on serial, read back and checked on the Pi.
- tryboot: the firmware read `tryboot.txt`, `kernel=sdboot.img` under the last `[all]` replaced `[pi4]`'s `kernel8-rpi4.img`, and the engine came up; the next reboot read `config.txt`.
- Reboot to the engine (the card's config.txt path): `reboot:` at 0 s, sdboot's line and `IHEX-F` at 5.7 s, `starting the engine` at 6.7 s, the copy done at 6.8 s, Circle's first line at 9.9 s, the engine's banner at 10.1 s, its first stats line at 13.6 s. The stock bootloader's `IHEX-F` came at 5.2-5.3 s; sdboot's check adds about 0.4 s.
- The kernel constructor's `m_ActLED.Blink (3)` (from the skeleton) held the boot 2.1 s before Circle's first line; without it: the banner at 8.0 s, the first stats line at 11.5 s.
- `disable_splash=1` and `boot_delay=0`: no change in the timing (sdboot's line at 5.7-5.8 s with and without; the firmware's own time). What is left: the firmware 5.7 s, sdboot's window 1 s, Circle before its first line 1.0 s, the engine's init 3.5 s (the boot bench included; `bench=0` skips it).
- The boot screen (`pi/ui/sidecar_splash.h`): drawn 9.6 ms after the banner (0.27 s), LVGL's home screen at 2.56 s as before (2.55 s without it).
- `:wdtest` (core 0 hangs): the watchdog's reboot at 21 s, the card's engine at 25.7 s, no Mac.
- flash.sh (unchanged: flashy and its reboot magic) through sdboot: upload 22.9 s, the go 29 s after the start, the RAM kernel runs; the card's files unchanged.
- An upload killed after 12 s: `#ERR:timeout` 5 s later, the card's engine 1 s after that.
- Not measured: a cold power cycle (the user's one-step test: power the Pi off and on; the engine's banner and stats lines should come on the serial port, and `sidecar.local` once the Ethernet link is up).
- The Rytm: stock OS 1.73 in OVERBRIDGE mode, on one of the Pi's USB-A ports.
  - Spike3's setup: the playing pattern's track 1 sends note 36 on MIDI channel 1 (MIDI CONFIG > PORT CONFIG: OUTPUT TO USB).
  - Nothing else is needed on the Rytm (measured 2026-10-15): USB CONFIG = OVERBRIDGE, the sequencer playing, and a track that sends notes on channels 1..8 (channel N plays voice N-1). The stats line's `notes N ch C,...` shows which channels send.
  - Plug-and-play: the Rytm can come and go while the kernel runs.
- Without a Rytm: the self-test transport (`selftest=1` or `:selftest=on`, below).

## Cores

| core | does |
|---|---|
| 0 | the xHCI interrupt (Overbridge IN/OUT, OUT paced by IN), USB plug-and-play, the Rytm's USB MIDI (read only), the console, the stats a second, the watchdog, the clean stop; the self-test transport's pacer |
| 1, 2 | voices 0-3 / 4-7 of each 12-frame block (`split=`), each voice timed by the guard; core 2 then runs the send FX (below) |
| 3 | the MIDI bytes (parser, note adapter) and console commands into the core, blocks while the FIFO is below its target, card work otherwise (FatFs, DSP SAMPLE) |

At boot, before the render loops start, every core runs the bench in turn (cores 0, 1, 2, then 3). See "Measurements".

## Options (`cmdline.txt`)

| option | default | |
|---|---|---|
| `out=main\|tracks` | tracks | MAIN L/R carries the sum of the voices (stock Rytm), or each voice goes on its track's Overbridge channel (`ob::kVoiceToRytm`, mod build) |
| `fifo=N` | per link: usb 96 (confirmed with the Rytm), tcp 288 | output FIFO target in frames (24..1024), for every link when given; latency added: N / 48 ms. Without it each link sets its own default when it becomes the host (`FifoUsb`, `FifoTcp` in `kernel.h`; "FIFO target" below) |
| `notes=on\|off` | off | MIDI channels 1..8 play voices 0..7 (the note adapter) |
| `voicebudget=P` | 25 | a voice's budget in % of a block (5..100); three blocks over in a row mute it |
| `split=N` | 4 | voices 0..N-1 render on core 1, N..7 on core 2 (1..7) |
| `display=0\|1` | 1 | the HDMI display and its USB touch, drawn from core 0's loop (`docs/pi-display.md`); `width=` `height=` (Circle's) set the frame buffer's size, else the firmware's mode |
| `bench=0\|1` | 1 | the boot bench (6 machines x 4 cores, 1 s of audio each; see Measurements). A core that does not finish its bench in 20 s is fatal (`bench: core N did not finish ...`, clean stop, reboot into the bootloader); `bench=0` then boots without it |
| `tohost=0\|1` | 1 | the engine's host-link replies (1D slots, 1A EMPTY, 1B, 0E listings, 14 pool, 16/17 names, 15) go to the Rytm's USB MIDI OUT (umidi1, cable 0), as the Teensy's do; 0 drops them (counted), the behaviour before 2026-10-16 |
| `selftest=1` | 0 | start the self-test transport at boot (no Rytm) |
| `sampleram=MB` | 256 | DSP SAMPLE's sample RAM (0 streams from the card) |
| `capch=N` | markers | the 4th channel of the `w` capture: IN channel N (0..11), or the note-on markers (default, 12 or more) |
| `recbuf=MB` | 64 | the recorder's memory, reserved once at boot from the heap (4.4 MB of it the files' 192 KB write buffers, the rest the ring: 22 s of 12 IN + master at 24-bit); 0: recording off |
| `web=0\|1` | 1 | the web page at http://sidecar.local over the Ethernet (below, "Web page"); 0: no HTTP daemon and no mDNS (the network stays up for `link=tcp`) |
| `oburbs=1..16` `obblocks=1..8` `obout=paced\|free\|off` `oboutdelay=N` `obctr=zero\|in` | 8, 1, paced, 0, zero | spike2's Overbridge host tuning, unchanged (`docs/pi-circle-spike.md`) |

## Console (921600 baud, the flasher's serial cable)

Keys:
- `w`: a 10 s capture of IN MAIN L/R, OUT MAIN L as sent, and the 4th channel (`capch=`). It is dumped as base64; `pi/circle/capture.py` writes the WAV. It works on the Rytm's stream or on the self-test's output, and the dump takes 30 s to 2 minutes.
  - While it dumps, the core's log lines (core 3's `trig voice` lines) are held back and counted (`capture: dump sent (N core log lines held back)`). Another core's bytes written into a dump line garbled it: with the Rytm's notes at 8 a second, 311 of 134000 lines, and the frames after them shifted (every channel read as full-scale noise).
  - The markers channel marks the MIDI arrivals only (no trigger marker); `spike3/latency.py` then takes the voice's onsets in OUT as sent.
- `s`: the stats now.

Commands are `:<command>` and a newline, at most 47 characters. The control core (core 3) runs them:

| command | |
|---|---|
| `notemach=V:NAME` | voice V (0-7) plays machine NAME on its MIDI channel V+1: `plaits juno fm daisy dsps grain dd`. Defaults that sound: PLAITS VA, JUNO factory patch 0, 6-OP FM ROM voice 0, DAISYSP MODAL, DIGIDRUM EFM-BD, DSP SAMPLE pool slot 1 (`grain`: the same in GRAIN from the sample RAM, dense: every retrigger stops sounding grains) |
| `drone=V\|all\|off` | the engine's own notes for tests: every 0.5 s a note-off and a note-on (note 48 + 2V) on voice V or on all eight |
| `notes=0\|1` | MIDI notes off/on (as `notes=` at boot) |
| `out=main\|tracks` | the output mode (as `out=`) |
| `fifo=N` | the FIFO target (as `fifo=`; the Task 7 sweep). It stays across `link=` switches until the next `fifo=` |
| `fxin=1\|0\|?` | the IN L/R source (below, "IN L/R source"): 1 the Rytm's FX return on Overbridge IN L/R, 0 its AUDIO IN, ? ask the Rytm; `fxin` alone reports. Needs the rui1024 Rytm build |
| `unmute` | clear every mute and any pending `overtest` |
| `faulttest=V` | guard test: voice V's next render faults (a write to an unmapped address): the voice is muted "crashed" and the others play on |
| `overtest=V` | guard test: voice V renders 300 us long for 5 blocks: muted "over budget" |
| `hangtest=V` | guard test: voice V renders 30 ms long once. A render core misses the 20 ms limit, which is fatal: clean stop, then reboot |

Core 0 runs two commands itself:

| command | |
|---|---|
| `selftest=on\|off` (or `1\|0`) | the self-test transport (below). It is refused while the Rytm is connected, and switches itself off when the Rytm's stream or USB MIDI appears |
| `wdtest` | watchdog test: core 0's loop hangs and the BCM watchdog reboots the Pi in 4 s (no clean stop) |

An unknown command logs `unknown command "..."`. A long line is dropped whole.

### The self-test (no Rytm)

The transport's clock is the generic timer instead of the Rytm's IN stream:
- Core 0 pulls 7-frame blocks from the output stage at 48 kHz (6857 blocks/s, as Overbridge OUT would).
- It feeds the `w` capture.
- It plays synthetic MIDI on channel 1: note 48, velocity 100, a note-off and a note-on every 0.5 s. The notes go through the real path: parser, note adapter, core. Voice 0's machine is set by `:notemach=0:NAME`.
- It sends nothing to USB.

Its stats line: `selftest: OUT <blocks>/s notes <n> slips <n>`.

On every boot, `bridge::selftest_content` writes the card content the self-test needs, unless it already exists:
- `/Samples/pi-test/sine440.wav` (2 s, 440 Hz);
- without `/.rytm/project`, `/.rytm/pools/_default.txt` naming that file as pool slot 1, so DSP SAMPLE's note 48 plays 220 Hz.

Each file is written whole (a `.tmp` file, then a rename), and nothing that exists is touched.

### The stats (a line a second)

- `engine: fifo min A max B target T under U | blocks N max M us | cpu c1 x c2 y c3 z % | pass max P us | <machine %> | midi M B (lost L) sysex S notes N ch C,... | to-host sent B B M msgs fail F dropped D | peak P/1000 | C`
  - `under` counts empty-FIFO frames (sent as silence).
  - `cpu` is each core's busy share.
  - `pass max` is core 3's longest working pass of its loop (the longest the FIFO waits for a refill).
  - `lost` counts the Rytm's USB MIDI bytes dropped by a full input queue (the `tcp:` line has its own).
  - `notes N ch C,...`: the note-ons this second and the MIDI channels (1..16) they came on; only 1..8 play voices.
  - `to-host sent`: the engine's bytes and messages sent to the Rytm's USB MIDI; `fail` the sends that failed (one turns sending off until umidi1 is found again: a Rytm that stops reading costs one 3 s stall of core 0, Circle's bulk limit, not one per message); `dropped` the bytes not sent (no umidi1, `tohost=0`, after a failure, the TCP/self-test transports' own drops, or a full queue).
  - `peak` is relative to full scale; the limit is 250.
- With `link=tcp`, the `tcp:` line (every 10 s) ends `wanted max W, poll gap max G us`: the most frames the emulator had asked for at once, and core 0's longest gap between two polls of the link.
- With the Rytm, spike3's `rytmob:` line follows: IN/OUT blocks a second, `gap`, `lost`, and so on.
- `engine: muted !PLAITS(6) ...` lists the muted voices. Each new mute is also logged once with its reason.

## Send FX (M2, 2026-10-16)

SEND FX 1 and 2 (`core/fx/send_fx.h`, docs/core.md "Send FX"; route A of
`docs/superpowers/specs/2026-10-16-sidecar-send-fx-research.md`), wired by `sendfx_pi.*` and a few hook lines:

- **In:** the IN completion (`ob_usb.cpp`, core 0) copies the Rytm's voice stems, IN 2..9, into a ring, only while
  an FX has a playable TYPE (`fx::any_on()`: off costs nothing).
- **Render:** render core 2, after it has published its block (`cores.cpp` Worker: the block is not delayed), runs
  `fx::SendFx` on what is queued (at most 96 frames a call). The voices' render state is not touched; the send FX's
  state is core 2's alone; the values come from the control core (0x20, the console) as bytes.
- **Out:** the OUT fill (`ob_usb.cpp` FillOut, after the Sidecar's computer OUT) adds the return onto host -> Rytm
  2/3 (MAIN FX: PRE, before the Rytm's analog distortion and compressor) and 0/1 (MAIN: POST, clean), each FX on the pair
  its P1 names (0..63 PRE, 64..127 POST; STUTTER too; `sendfxout=fx|main` forces all, `slot` is the default),
  saturating. The engine's per-track voice channels and its MAIN mix are unchanged. The return ring holds 96 frames
  (2 ms, the voices' FIFO target) before it starts; empty, it gives silence, is counted and primes again.
- **Size and load (2026-10-16, RINGS + CLOUDSEED):** `SendFx` is 2.69 MB of .bss (each slot holds every type, so
  both can run any TYPE, two CLOUDSEEDs included; the kernel ends at ~4.8 MB of its 8 MB). Estimated on core 2 from
  the Mac x13.2: two CLOUDSEEDs ~33 us a 12-frame block (~13 % of the core) on top of its voices; `:sendfx` reports
  the measured ns a block.
- **Values:** 0x20 from a Rytm with 0025 (`hwsfx1b`-like builds) sets the per-track sends and each FX's TYPE + P1..P5;
  the console overrides them until the next 0x20 for that track (a stock Rytm sends none): `:sendfx=1:chorus`,
  `:killfx` (the display's Kill tails: the CC 120 request), `:sendfx=2:comb`, `:sendfx=off`, `:sendlvl=K:T|all:V`, `:sendknob=K:I:V`, `:sendfxout=fx|main` (kept in
  `/.rytm/sendfx.txt`). `:sendfx` shows the values, then a stats line since the last `:sendfx`: frames, core 2's
  send-FX load, ns a 12-frame block, underruns, ring drops, the return's peak, faults.
- Not on the TCP link (rytm-emu): it carries no IN stems to the Pi yet (the hello asks for no R frames).

## IN L/R source (2026-10-18, rytm-mods 0030-fx-in)

The rui1024 Rytm build (rytm-mods `0030-fx-in`) can put its delay + reverb return (stereo) on the Overbridge IN L/R
(channels 10/11 of the 12 the Sidecar receives) instead of the codec inputs. Sound tab: **IN L/R source: Inputs / FX
return** (default Inputs; kept in `/.rytm/fxin.txt` as `src=fx` or `src=in`). Host link (`sim/protocol.py`, `core/host_link.cpp`,
shared with the Mini): `26 <v>` host to Rytm (1 FX return, 0 AUDIO IN, 0x7F ask) and `27 <s>` Rytm to host (1 on, 0 off),
sent for every 26 and unasked each time the Rytm's USB port comes up (the Rytm resets itself to off on a disconnect).

- After every connect (the unasked `27 00`) the engine sends `26 01` again while the setting is FX return, and asks (`26 7F`)
  with the first host-link message after its own boot. A `27 00` answering our own `26 01` is not answered again (no loop).
- The page, `/status` (`"fxin":{"want":0|1,"state":"on|off|unknown|unsupported","rytm":1|0|-1}`) and the console (`fxin`)
  show the **Rytm's confirmed state**, not the setting. `unsupported`: asked 3 s ago, never answered, an older Rytm build
  ("not supported by this Rytm build").
- While on, the recorder names channels 10/11 `11_FX_L.wav` / `12_FX_R.wav` (the file numbers are the same; `fx_l` and
  `fx_r` also parse as channel names) and the Rytm's SCOPE page calls its sources `FX L` / `FX R` (the meters' label `FX`).
  A take keeps the names it started with.
- **Never send channels 10/11 back into the Rytm through Overbridge** (the computer-audio return, a loop on the mixer):
  the FX return would feed the FX again. The page warns while the switch is on.
- Tests: `make -C core test` (`test_host_link`: 26/27, no loop, persistence, names), `python3 -m unittest sim/tests/test_protocol_fxin.py`,
  `node --test pi/engine/tests/page_fxin.test.mjs` (the page; `MOCK_FXIN=old python3 pi/engine/tests/mock_web.py` an older Rytm),
  `RYTM_MODS=<rytm-mods checkout> python3 sim/fxin_e2e.py` (rytm-emu with the rui1024 build and a base from `tools/emu/us_base.sh rui1024`:
  the core sends 26 01 from the card's setting, the Rytm answers 27 01 and the reverb tail is on 10/11; after a USB re-plug the unasked
  27 00 makes it send 26 01 again).

## Recording (2026-10-17)

The Record tab of the web page (and the console) records the Overbridge IN channels, the Sidecar's voices and the
send FX returns to the card, one WAV a source, all sample-aligned. `recorder.*` (portable), `rec_wav.h` (the
header), `rec_pi.*` (Circle: FatFs, the commands, `/status`).

- **Sources** (the file's number is the source's id): `00_MASTER` (IN 0/1, stereo), `01_MAIN_L` `02_MAIN_R` `03_BD`
  `04_SD` `05_RS-CP` `06_BT` `07_LT` `08_MT-HT` `09_CH-OH` `10_CY-CB` `11_IN_L` `12_IN_R` (IN 0..11, mono; `11_FX_L` `12_FX_R` while the Rytm's FX return is on them, "IN L/R source" below),
  `13_VOICE1`..`20_VOICE8` (the Sidecar's voices as rendered, before the output map, mono), `21_FX1` `22_FX2` (each
  send FX's own return before the limiter, STUTTER's on its slot, stereo; `SendFx::Process`'s new `slots` output).
- **Files:** `/Recordings/<prefix>_<NNN>/<NN>_<NAME>.wav`, the next free NNN for the prefix (default `take`). PCM
  48 kHz, 24-bit (default) or 16-bit (truncated), mono or stereo. The header is 512 bytes (RIFF, fmt, a JUNK pad,
  data), so the samples start on a sector and every write is whole 512-byte sectors. Every file of a take starts on
  the same IN frame and has the same length. The voices and FX are the frames the OUT fill made around that IN
  block (a FIFO with up to 16 blocks of lead, 2.3 ms: the OUT fill can run several blocks behind the IN completion). The round trip to
  the drum machine (about 6 ms) is not compensated.
- **Power loss:** while a take records, the header's sizes are a placeholder (the largest whole-frame size a
  32-bit RIFF allows), and one file a 2 s round is `f_sync`ed (its length reaches the directory). A file cut by a
  power loss holds everything up to its last sync and still opens (most readers take the data to the end of the file).
  The next boot scans `/Recordings/*/*.wav` and writes the true sizes into any header of ours that does not match its
  file's length (`rec: ... sizes fixed`). The files of a cut take start on the same frame but can end at different
  points (each at its own last write and sync). A clean stop writes the sizes itself (and RIFF's pad byte after an odd
  data size).
- **Limits:** a take stops by itself before any file reaches 4 GB (FAT32), checked against the widest
  file (stereo 24-bit: about 4 h 8 min). A failed write (card full) stops the take and closes the files.
- **Path:** the IN completion (core 0's xHCI interrupt) copies the selected samples of each block into a ring
  of fixed slots in the reserved memory, together with the OUT fill's voice/FX frames (a 512-frame SPSC between
  the two). A full ring drops the block and counts it, and the block is later written as silence, so the files
  keep time and stay aligned. Core 0's loop (`rec_pi::Poll`, once a pass) deinterleaves into a 192 KB buffer
  a file and writes at most one whole buffer a pass (`f_write`, sector-aligned), so the network and the display
  run between writes. Nothing allocates or blocks on the audio side, and nothing logs there.
- **Start/stop:** manual, or armed: the drum machine's MIDI PLAY (FA, or FB continue) starts a take at the next IN
  block, STOP (FC) ends it, and the recorder arms again for the next PLAY. A manual stop disarms. The realtime
  bytes come through core 3's MIDI parser (`rec_pi::Transport`, then the send FX's clock as before).
- **Console:** `rec` (the state), `rec=start`, `rec=stop`, `rec=arm`, `rec=ch:LIST` (ids or names, comma
  separated: `all`, `none`, `in`, `voices`, `master`, `main_l`, `bd`, `rs-cp`, `voice1`, `fx2`, ...; `+id`/`-id`
  add or remove), `rec=bits:16|24`, `rec=name:PREFIX` (1-24 of `A-Z a-z 0-9 _ -`). Changes need a stopped recorder.
- **Log:** `rec: <take> started: N files, 24-bit, channels ..., KB/s, ring N slots (ms)`; every 10 s while
  recording a progress line; at the end `stopped: frames, KB in N writes, card KB/s, worst write ms, ring max %,
  dropped N blocks, OUT side short N`; `the card is behind: N blocks dropped` at most once a second.
- **Page:** one checkbox a source in groups, Select all / None, the record button (Record / Arm, Stop / Disarm),
  the elapsed time, a peak meter a selected channel (the last second's peak from `/status`, while armed or
  recording), free space and the time left at the selection, the card's speed and worst write, a red warning on
  dropped blocks. Options: manual or armed, 24/16-bit, the take name. Below them the takes (download each file through `/fs/get`, delete). The
  selections are kept in the browser (`localStorage`, `sidecar.rec`) and sent to the Pi when the page opens.
  The Files tab has a Recordings button.
- **Display:** the Status panel shows a red `REC h:mm:ss` while a take records, `REC armed` while armed.
- **Playback** (`rec_play.h`, 2026-10-18): one channel of one take into the Overbridge return's MAIN L / R (a mono file
  on both), added in the OUT fill after the voices, the computer's OUT and the send FX returns (`ob_usb.cpp` FillOut),
  so the voices' paths (`out=main|tracks`) stay as they are. Core 0's loop reads 30 KB a pass into a 1.37 s ring
  (0.5 s held before the sound starts); the OUT fill only pops. 10 ms fades in, out on a stop, on a channel change
  (which resumes the new channel at the same frame) and when the ring runs low before the file's end (the card fell
  behind: faded out, `card too slow`, counted). Never while a take records: a start is refused, a take that starts
  (YES, the console, the Rytm's PLAY on an armed recorder) fades playback out. Level 0..127 (gain (n/127)^2; 100 by
  default, -4 dB; 127 as recorded). A take that ends becomes the one selected. Remote UI: the REC page (B / C / D,
  `core/ui/README.md`); console: `play`, `play=start|stop|scan|take:N|ch:N|level:N|start:TAKE/NN`; `/status`'s
  `"play"`; the web page: a button per channel in the takes list and Stop. Pi only (the Teensy has no recorder).
- **Measured** (2026-10-17, the Pi's 55 GB card, the drum machine streaming a playing pattern, Wi-Fi):
  | take | result |
  |---|---|
  | 12 IN + master, 24-bit (13 files, 1.97 MB/s), 5 min | 14 405 195 frames (300.1 s) in every file; 0 dropped blocks, 0 audio underruns, 0 Overbridge gaps; card 9.2 MB/s inside the writes (3080 writes of 192 KB), worst write 100 ms; ring at most 5928 of 131072 slots (4.5 %, 0.9 s of its 19 s); core 0's `sleep1msMaxUs` at most 2.4 ms (avg 1.01 ms); `/status` median 26 ms, one at 473 ms; every header and size right, MASTER L/R = MAIN_L/MAIN_R in 38400 sampled frames |
  | all 23 sources, 24-bit (3.57 MB/s), 60 s | 0 dropped, 0 underruns, 0 OUT side frames short (14-21 with a 4-block lead: now 16); card 5.5 MB/s (23 files at once), worst write 137 ms, ring at most 23 % of 9.6 s; `/status` median 86 ms |
  | the self-test (synthetic IN: a click every second on all 12 at once), 23 sources | the clicks on the same frame in all 13 IN-based files; VOICE1 sounds (notes=1); the self-test's clock slips while core 0 writes (it is paced by core 0's loop, the stream is not) |
  | a take cut by the watchdog (`wdtest`), 16-bit | the next boot: `4 fixed in 213 ms`; all 4 files open with Python's `wave` (5.1-10.2 s of the 13 s, each to its last write) |
  All 23 sources at 24-bit leave this card 1.5x headroom; the IN channels alone 4.7x.
- **/status:** `"rec":{"state":0 idle|1 armed|2 recording|3 closing,"arm","ready","bits","prefix","take","ch":[ids],
  "frames","bytes" (KB),"drops","silence" (frames),"outShort","cardKBps","needKBps","worstWriteMs","writes","ringMs",
  "ringUsed","ringMax","ringSlots","freeMB","totalMB","error","peak":[23 per mille]}`.

Web: at most 4 HTTP connections are served at once (`CWebServer::MaxWorkers`); more are closed at once and counted
(`/status` `web.refused`). Without it, a browser's requests piled up while the Pi was away (the page's 1 s `/status`
poll) arrived together after a boot, and Circle's 10 clients plus Wi-Fi's tasks passed `MAX_TASKS` (20): `sched:
System limit of tasks exceeded`, a reboot, three times on 2026-10-17. The page itself now keeps one `/status` (and one
`/um`) request at a time, given up after 4 s.

## Tests

- On the Mac:
  - `make -C pi/engine/tests` runs spsc, output, midi_in, notes, guard, records, emu_proto, usbmidi_pack (the replies' USB MIDI packets), sendfx_pi (the send FX's rings, priming, route, underrun, each FX's own return) and recorder (the WAV header and its boot fix, the channel lists, a take's files lined up sample for sample, 16-bit, a ring overrun written as silence, arming by PLAY/STOP).
  - `make -C pi/engine/tests content` runs the self-test content on the core's Mac port: DSP SAMPLE plays 220.0 Hz, and the bench renders every machine.
  - `uv run python pi/engine/tests/test_pi_run.py` runs `pi_run.py`'s analysis on synthetic captures and log lines, including the bench lines.
- `pi/engine/tests/pi_run.py` (on the Pi over serial; `--flash` first builds and uploads):
  - `[--selftest] engines [--machines ...]`: each machine on voice 0, its trigs, its stats, and a 10 s capture checked for level and pitch;
  - `engines --rytm-notes V,... [--note N]`: the same on the Rytm's own notes (its sequencer playing, no drone), each machine on the listed voices, the pitches of note N (default 36);
  - `[--selftest] gaps SECS [--fifo N] [--machines M,...]`: all 8 voices droning, no underflows or gaps after a 30 s warm-up; every 10 s core 3's longest pass and the FIFO's lowest level; `--machines dsps` puts DSP SAMPLE on every voice;
  - `bench [LOG]`: the boot bench's lines from a flash log, the checks, and the Markdown table for the Measurements below;
  - `analyse WAV`: a capture on disk.
- `pi/engine/tests/guard.py mute|fatal|watchdog|reboot`: the guard, the fatal path, the watchdog and the clean stop, run on the self-test transport.
- `pi/engine/test-on-pi.sh`: every on-Pi check in order, the watchdog step last, then a PASS/FAIL line per step. About 45 minutes.
  - `test-on-pi.sh STEP...` runs single steps: `task3-selfcheck task4-boot task4-selftest task5-guard task5-fatal task5-reboots task6-content task6-engines task6-gaps task7-bench task7-fifo task5-watchdog`.
  - `test-on-pi.sh --dry-run` lists the steps.
  - Every flash must show the kernel's `==== rytm-pi engine` banner. Without it the Pi is not answering: the remaining steps are skipped (SKIP in the summary) with "power-cycle it".
  - The steps that need the Rytm are listed in it as `DEFERRED (Rytm)` comments.
- `pi/engine/tests/phase1.py` and `pi/engine/test-emulink-on-pi.sh`: the emulator link's acceptance (next section).

## Phase 1: rytm-emu over Ethernet

Plan: `docs/superpowers/plans/2026-10-14-pi-engine-emulator-link.md`. The Pi is the Overbridge host of rytm-emu, which runs the rytm-mods mod build (`fw/AR173_us_us1.syx`, local and git-ignored there). Nothing is flashed to the Rytm. In tcp mode a still-attached real Rytm gets silence and its MIDI is dropped.

### The cable and the network

- A cable from the Mac's USB-C hub Ethernet port straight to the Pi 4's Ethernet port. There is no DHCP server, so both ends use static addresses.
- The one user step, from the plan, quoted exactly: **Set the Mac's USB-C hub Ethernet port to a manual IP: System Settings > Network > the hub's Ethernet (e.g. "USB 10/100/1000 LAN") > Details > TCP/IP > Configure IPv4: Manually, IP address `192.168.77.1`, subnet mask `255.255.255.0`, router empty.** Nothing in this repo configures the Mac's network; `test-emulink-on-pi.sh` only checks it and prints this step when it is missing.
- The Pi's side, in `cmdline.txt`:

| option | default | |
|---|---|---|
| `ip=` | 192.168.77.2 | the Pi's static address (Circle's GENET Ethernet, no gateway) |
| `netmask=` | 255.255.255.0 | |
| `emuhost=IP:PORT` | 192.168.77.1:7301 | the emulator's link |
| `link=usb\|tcp` | usb | the engine's host at boot |

- Console: `:link=usb`, `:link=tcp` or `:link=tcp:IP:PORT` switches the host at run time (core 0), so no SD edit is needed. One host feeds and drains the engine at a time. The boot logs `network: 192.168.77.2/255.255.255.0 ...` and `link: usb`. If the network did not initialize (`network: init failed`), `link=tcp` is refused with a warning and the engine stays on usb.
- The TCP link's sending is bounded per loop pass: at most 8 messages or 5 ms of sending, the rest at the next pass. Circle's send timeout applies per TCP segment, so each segment may wait 250 ms and a whole send that takes over 500 ms loses the link (it reconnects). The worst single pass is one message's segments at the minimum MSS (23 x 250 ms, about 6 s), inside the 15 s watchdog. `N` requests before the hello's answer are kept and answered once it comes (`early N` counts them; rytm-emu opens with N 512 and asks again only as frames arrive), and the frames wanted are capped at 48000 (`clamped`).
- Console extras for the acceptance (core 3): `:pool=S:path` sets the current project's pool slot S to the card file `/Samples/path` and sends 14 to the host (`pool slot S = path: ok`; `:pool=S:` empties it). `:juno=ren:S:NAME`, `:juno=del:S`, `:juno=swap:A:B`, `:juno=save:V[:NAME]` (the patch JUNO voice V plays now, its six knobs as they are) and `:fm=ren|del|swap`, `:fm=save:V` (the ROM voice FM voice V plays, while the list is empty) edit the project's JUNO / FM lists and send 16 / 17 (`core/user/lists.h`). Commands are up to 191 characters. `:reportlog=1` logs each 0x04/0x03 report (`report 04 voice V flags FF P1..P4 a b c d`); `:reportlog=0` turns it off.
- The TCP transport's stats line, once a second: `connected to IP:PORT, hello yes (granted 7), frames N/s in R requests, wanted W, silence S, midi in A B (lost L) out B B dropped D, connects C, skipped K B, early N E, clamped F`.

### The link protocol

rytm-emu's Overbridge link (`rytm-emu/src/oblink.rs`, documented in `rytm-emu/docs/RYTM-PORT.md`). The emulator listens with `--ob-link 0.0.0.0:7301`. A host that sends the hello `H 1 flags` gets:
- bit 0: the USB MIDI pair `M` (u16 LE length + bytes) both ways;
- bit 1: no `R` frames;
- bit 2: compact voice frames `a` (u32 LE n, then n x 8 x i16 LE, voices in physical order), paced by the emulator's `N` requests.

Hosts without `H` see the old stream. The Pi's host side is `pi/engine/emu_proto.*` (Mac-tested in `tests/test_emu_proto.cpp`) and `pi/engine/tcp_link.*`. Every 10 s the emulator prints `overbridge link: host connected|none, flags 0x7, frames short N, usb OUT frames short M, midi to host X B, from host Y B`.

`sim/emu_link.py` is a minimal Python host for the same messages: the hello, silent `a` frames for every `N`, and the Rytm's USB MIDI decoded. It tests the emulator side without the Pi.

### The emulator base

`sections/us_us1.snap` no longer loads in the current rytm-emu ("tag for enum is not valid"): the snapshot format changed. It is left untouched. `pi/engine/tests/phase1.py base` makes a new base, `rytm-emu/sections/us_us1.v2.snap` and `.img` (git-ignored there), as rytm-mods' `tools/emu/us_base.sh` does:
- a cold boot of the mod build on a copy of `sections/led.img`, with the startup dialogs answered;
- GLOBAL > MIDI CONFIG > SYNC: TRANSPORT SEND on and USB CONFIG = OVERBRIDGE;
- the card saved with the snapshot, so the pair resumes without a card mismatch.

It takes about 10 s. `phase1.py` and `test-emulink-on-pi.sh` make it themselves when it is missing or does not load. Every emulator run uses a scratch copy of both files, because `rytm` writes its card back at exit, and a window run with `--snapshot` saves to that path.

### The acceptance (`pi/engine/tests/phase1.py`)

- `phase1.py tracks`: four tracks on Pi machines, set by DIN CC 15: BD PLAITS 44, SD 6-OP FM 40, LT DSP SAMPLE 45, CH DAISYSP 51. The pattern A01 is cleared (FUNC + CLEAR) and recorded by key events, then played. The checks:
  - each voice is triggered with its machine (the Pi's `trig voice` lines) and sounds on its channel of the USB stream (`--usb-wav`);
  - a knob turn on CH (STR, its whole range) changes the sound: the channel's third-octave spectrum before and after differ by more than 3 dB and three times each window's own halves (the spectral centroid did not see STR: under 2 % for +8 detents, 3.4 % for the whole range);
  - a p-lock on SD's step 5 reaches the engine (`:reportlog=1`);
  - the screenshot `pi/circle/out/phase1-sd-src.png` shows SD's PAT as a ROM voice name (17); a second run on LT's SRC page shows SMP `sine440` (14).
- `phase1.py gaps SECS`: the same setup for SECS seconds. The emulator's `usb OUT frames short` and the Pi's `under` stay constant after a 20 s warm-up, and no voice is muted.
- `phase1.py --emu-only tracks|gaps SECS`: the emulator side alone, with no Pi, serial port or network. The link is on 127.0.0.1 and `sim/emu_link.py` is the host. It checks the base resume, the hello, the CC 15 machines and the recorded pattern (the Rytm's own 0x04 trigs per voice and machine flag), the Rytm's USB MIDI to the host, the frame rate and the counters. It also checks that the p-lock on SD's step 5 and the knob turn on CH reach the Rytm's 0x04 trig parameters (P2). The host's view is kept in `pi/circle/out/phase1-standin.txt`.
  - What it verifies: the emulator, the mod firmware, the key and knob events, the link protocol (hello, `N`/`a` pacing, the MIDI pair) and what the Rytm sends a host.
  - What it does not: the Pi engine. The stand-in answers with silent voice frames and does not run the engine, so the engine's sound, its trig handling, the knob's effect on the sound, the pool and names replies, and Circle's TCP path are not exercised. Those are the Pi run's checks (`phase1.py tracks` / `gaps` without `--emu-only`).
- Knob turns: the firmware ignores a knob's first turn (it only shows the parameter; measured with a debugger, the stock OS does the same). A wake turn followed by one large turn left the parameter unchanged, so `phase1.py` turns a knob as a wake turn plus one-detent turns 50 ms apart (`knob_turns`, as rytm-emu's `nudge_steps`).
- LT's SMP: the base has SMP 107 on LT (its 0x03 reports, P11), beyond the pool, so DSP SAMPLE played silence (`rms 0.0000`). `set_machines` also sends CC 27 (the sample page's SMP) = 1 on LT's channel, the slot `cmd_tracks` gives `pi-test/sine440.wav`. Not knob turns: the emulator's turns of the SMP knob are lossy (40 one-detent turns 0.15 s apart moved it 13, 30 four-detent turns 75). The Pi's DSP SAMPLE trig line shows `smp mix sta end lev`.
- LT's track in the base's A01 is 64 steps long, and grid recording places its steps on page 1 only. LT therefore trigs a quarter as often as the 16-step tracks (2 trigs per 64 steps, about 7 in a tracks run). The checks need at least 2 trigs per track.

### The deferred checks (`pi/engine/test-emulink-on-pi.sh`)

Run once the Pi is power-cycled, cabled, and the Mac's IP is set. It runs these steps in order and stops at the first failure:

- [ ] `mac-ip` (Task 5 Step 1): an interface at 192.168.77.1/24. If there is none, the script prints the user step and exits.
- [ ] `flash` (Task 4 Step 5): build and flash; `network: 192.168.77.2/255.255.255.0`, `link: usb`, no fault; `pi_run.py --selftest engines` ALL PASS (USB unchanged by the scheduler and network). With `RYTM=1` it also checks the rytmob `gap 0` line and runs `pi_run.py engines`.
- [ ] `ping` (Task 5 Step 2): three replies from 192.168.77.2.
- [ ] `bringup` (Task 5 Step 3): the Pi checks:
  - `link: tcp`, `connected ..., hello sent`, `hello yes (granted 7)`;
  - frames 48000/s +-1 %;
  - `midi in` growing and `midi out` > 0.

  No `project:` check: the mod build sends `10 <project>` only around its EXT machines' trigs (measured 2026-10-15: 40 s idle from the v2 base, and PLAY without the CC 15 machines, bring only 19s), and the core logs `project:` only when the name changes. The 10's handling is covered on the Mac by `core/tests/test_host_link.cpp` (the first 10, a repeat, a change, the first 10 after a start on the stored project), and `phase1.py --emu-only tracks` shows the mod's `10 PRESETS` reaching a host over the link.

  The emulator checks: `host connected from 192.168.77.2`, `flags 0x7`, `usb OUT frames short` constant, and `midi to host` / `from host` growing.
- [ ] `console` (Task 6 Step 3): `pool slot 1 = pi-test/sine440.wav: ok`, `report log on`.
- [ ] `phase1-tracks` (Task 7 Step 2): `phase1.py tracks` ALL PASS, then Read the screenshot (PAT a ROM voice name) and rerun on LT/SRC (SMP `sine440`).
- [ ] `phase1-gaps` (Task 7 Step 3): `phase1.py gaps 600` ALL PASS.

`test-emulink-on-pi.sh --dry-run` lists the steps, and `test-emulink-on-pi.sh STEP...` runs single ones.

### Results

| check | result |
|---|---|
| Pi: network up, `link: usb`, USB regression | PASS 2026-10-15 (169.254.61.60/16, link-local; `pi_run.py --selftest engines` ALL PASS) |
| ping the Pi | PASS |
| Pi TCP frames/s (target 48000) | PASS: 47747..48223/s in the 10-minute run, 48090/s in bringup |
| four tracks: trigs on the Pi, each channel sounds | PASS: BD, SD, LT (DSP SAMPLE sine440, rms 0.0193, after LT's SMP = 1 by CC 27), CH |
| SD PAT shows a ROM voice name (17); LT SMP shows `sine440` (14) | pending (deferred) |
| knob turn: CH sound change | PASS: band spectrum 12.4 dB apart (the windows' own halves 2.3 dB) |
| p-lock: SD step 5 P2 values | PASS |
| 10 minutes: usb OUT frames short, Pi underruns, mutes | PASS at the TCP default 288: none after the warm-up, FIFO min 63, wanted max 231, no mute |

Mac only, 2026-10-15, `phase1.py --emu-only` with `sim/emu_link.py` as the host:
- `tracks`:
  - the v2 base resumes, the hello is granted 0x7, and the stand-in served about 47000 frames/s;
  - the Rytm sent 85 KB of USB MIDI to the host (0x10 x10, 0x19 x50, 0x03, 0x04, 0x11, 0x18);
  - its 0x04 trigs show all four tracks on the CC 15 machines with the recorded pattern: voice 0 x20 (flags 0x00), voice 2 x21 (0x48), voice 3 x7 (0x20), voice 4 x39 (0x10);
  - the screenshot shows SD's SRC page with the mod build's 6-OP FM labels (PAT TIM MOR DEC / LPG TUN LEV LEN);
  - first run: FAIL on the p-lock (P2 8192 in every SD trig). Cause: the knob turns (a single large turn; the firmware also ignores a knob's first turn). After the switch to `knob_turns`: P2 8192 -> 9344 on SD's p-locked step, CH's P2 8192 -> 10112 after its knob turn, ALL PASS.
- `gaps 40`: 0 usb OUT frames short over 5 reports, ALL PASS. `gaps 60` after the knob change: 0 over 7 reports, ALL PASS.

## Web page (http://sidecar.local)

The Pi serves its own page over the on-board Ethernet, like Ableton Move's move.local: status, the live log and the console's controls, from any browser on the Mac, no serial cable. On by default (wired, no radio); `web=0` in cmdline.txt turns it off. Code: `pi/engine/web.{h,cpp}`, the page `pi/engine/www/index.html` (built into the kernel by `build.sh` as `hosted/web_page.h`).

- Name: the network's hostname is `sidecar`. Circle's mDNS daemon answers `sidecar.local` (A) and publishes the service `Sidecar | Expander` (`_http._tcp`, port 80; `dns-sd -B _http._tcp` lists it). A second Sidecar on the same network becomes `sidecar-1.local`. `pi/circle/circle-mdns.patch` (applied by `build.sh`, rebuilds `lib/net`) makes Circle's daemon read every question (with name compression), accept the QU bit and any case, and answer AAAA with an NSEC "A only" record: without it macOS's `getaddrinfo` (curl, browsers) waited 5 s for an IPv6 address before using the A record (measured 2026-10-16: `curl http://sidecar.local/` timed out at 5 s; with the patch 7 ms lookup, 13 ms for the page; `dns-sd -Q sidecar.local AAAA` shows "No Such Record").
- Addresses: the Pi has no DHCP client on the direct cable, so its address is built in or set in cmdline.txt (`ip=`, `netmask=`, "Phase 1" above). For a Mac on a self-assigned link-local address (no manual IP), build with `PI_IP=169.254.61.60 PI_NETMASK=255.255.0.0 ./build.sh kernel` (any free 169.254.x.y/16): the Mac then reaches it with no network settings at all. The Mac's manual 192.168.77.1/24 and the default 192.168.77.2 work too.
- Endpoints: `GET /` the page; `GET /status` JSON (host, ip, uptime, link, gadget mode, Sidecar up, computer audio, Overbridge streaming, the Rytm's USB MIDI, temperature, out, notes, drone, FIFO target, underruns, and the page's own request count and longest handler time in us); `GET /log?since=N` the console log from byte N (first line `START END`, then the bytes; the next poll asks `since=END`), from a 64 KB ring that the logger fills next to the serial port (`CLogTee`: any core may log, one spin lock); `POST /cmd` with `c=<command>` (form encoded): the serial console's own parser (`CConsole::Command`, the `:` lines), logged as `web: command :...`; `GET /lists` the project's pool and JUNO / FM lists and each voice's machine (`core/user/lists.h`, published by core 3 when it changes). `wdtest` is refused from the web (its reboot ends a RAM kernel; with sdboot the card's engine comes back).
- The page polls `/status` every second and `/log` every 0.8 s. Tiles: CPU of cores 1/2/3, FIFO min..max and target, underruns, Overbridge (IN packets/s, gap, lost: parsed from the `rytmob:` line), Rytm attached (stream or USB MIDI), USB-C gadget (alt settings, computer audio, late), temperature, the page's own cost. Controls: notes on/off, out main/tracks, computer audio on/off, a machine per voice (`notemach=`), drone on/off, FIFO target, and a free command line. The per-second stats lines are hidden in the log view by default (a checkbox shows them). The "Pool & presets" tab: the project's pool (assign a slot from a /Samples browser, clear it), the JUNO and FM lists (rename, move up, delete, save what a voice plays now) and each assigned user slot's presets in `/PRESETS/<id>/` (rename, delete through `/fs`, save current with `um=save`).
- Cores: Circle's scheduler and network stack are core 0's, so the HTTP and mDNS daemons are tasks there, run while core 0's loop sleeps its 1 ms (`m_Scheduler.MsSleep (1)`). The Overbridge stream and the USB-C gadget run in core 0's interrupts, which the tasks never mask; cores 1-3 (render, control) are untouched. `CWebStart` waits for the Ethernet link before starting the daemons, so the boot is not delayed and a cable plugged in later still works.

### Wi-Fi without a cable: sidecar-wifi.txt (2026-10-19)

A visible first-setup file at the card root (FAT root, visible in the Mac Finder), for a Pi with no cable and no way to open the page:

```
ssid=MyNetwork
psk=MyPassword
```

Optional: `band=2g|5g|any` (default 2g), `open=1` (an open network, no psk), `password=` as an alias of `psk=`, `#` comments; CRLF, a UTF-8 BOM, spaces around `=` and quotes around values are tolerated. At boot, before Wi-Fi starts (`CWifi` constructor, `wificonf::import_card`), the file is checked like the page's join (`check_join`: SSID 1-32 bytes, passphrase 8-63 printable or 64 hex, open takes no psk) and imported exactly like "Connect and save": `/.rytm/wifi.txt` by the atomic save with `on=1` and the derived 64-hex key (never the passphrase), the previous file kept as `wifi.txt.prev`, `wifi.try` "trying", the 90 s fallback. The file is then deleted, also when it was rejected; if the delete fails its contents are overwritten and it is renamed to `sidecar-wifi.used`. A rejection writes `sidecar-wifi-error.txt` (a short reason, never the SSID or password); a success removes an old one. One log line, never the SSID or password. Tests: `tests/test_wifi_conf.cpp` `test_import`.

### Wi-Fi settings (2026-10-18)

The Status tab's **Wi-Fi** section: on/off, a network list from a scan (signal bars, a lock for secured networks, the band), "Other network…" with a name field, a password field (show/hide, never pre-filled), "open network", **Connect and save**, **Forget network**, and the current connection (Ethernet or Wi-Fi, the address; the stored network's name only here). Code: `pi/engine/wifi.{h,cpp}`, the portable part `pi/engine/wifi_conf.{h,cpp}`, the endpoints in `web.cpp`. Research and the bring-up: `docs/research/pi-wifi.md`. Wi-Fi stays off in the release (no `/.rytm/wifi.txt`); turning it on here works over Ethernet.

- Endpoints: `GET /wifi/status` (the only answer with an SSID in it: mode, chip, on, net `ethernet|wifi`, ip, the stored SSID, the last scan's networks `{ssid,rssi,ch,band,secured}` (one per SSID, the strongest, sorted by signal, hidden ones left out, at most 24), the join's state `none|restarting|trying|up|failed` with its reason and seconds left); `GET /wifi/scan` starts a scan (the chip's escan on core 0's Wi-Fi task, 100 ms sleeps, stopped after 5 s; never on the audio cores; refused while Wi-Fi is off, while a join is tried, and on Wi-Fi while it is not associated); `POST /wifi/join` with `ssid=`, `psk=`, `open=1` in the form body (never the URL); `POST /wifi/forget`; `POST /wifi/power` with `on=0|1`. Answers: `{"ok":true}` or `{"ok":false,"code":N,"error":"..."}`. `/status`'s `wifi` member has no names any more (`stored` true/false, `networks` a count, `join` the state).
- Checks (`wifi_conf.cpp` `check_join`, the page the same): SSID 1-32 bytes without control characters; a password of 8-63 printable characters or 64 hex digits; an open network only with the explicit flag and no password. The error texts never quote the input.
- Storage: wifi.txt keeps its format (one key added: `open=1`), holds the 64-hex key (PBKDF2 of the passphrase, made on the Pi), never the passphrase, and is replaced atomically: `wifi.txt.part` written, the current file renamed to `wifi.txt.prev` (no file before: a `.prev` saying `on=0`), then `.part` renamed in place; a boot after a cut between the renames finishes it.
- One network at a time (Circle's single network stack, `wifi.h`): a join cannot switch the running Pi from Ethernet to Wi-Fi, and Circle's DHCP client keeps its lease when the Wi-Fi network changes, so **Connect and save restarts the Pi** (2 s after the answer) and the new network is tried at that boot. `/.rytm/wifi.try` says `state=trying`. Not up (associated and a DHCP lease) within `CWifi::TrialSecs` (90 s; the supplicant restarted once at 40 s, the watchdog's cure for a slow first association, seen 27 s to 3 min): `wifi.txt.prev` goes back, `wifi.try` says `state=failed` with `reason=wrong_password` (wpa_supplicant's WRONG_KEY lines, read from the logger's event queue), `no_dhcp` (associated, no lease), `not_found` (never associated) or `not_started` (the chip), and the Pi restarts on the previous setting: Ethernet when Wi-Fi was off, so a typo cannot lock it out. Ethernet is not used while the Pi is on Wi-Fi; the page says so, follows the Pi at `sidecar.local` if the computer is on the new network, and shows the failure after the revert. The console's `wifi=join:` and `wifi=on` with a stored network write the same trial, tried at the next boot.
- Forget: the network (and `wifi.txt.prev`, which holds a key too) removed, `on=0`; on Wi-Fi, Ethernet from the next boot.
- Credentials: the password goes only in the POST body; the server wipes its copies (the form buffer, the decoded value) after use; it is never logged, stored, echoed or in `/status`. SSIDs are not logged either: the scan line gives a count, and the log tee (`CLogTee`, serial and the page's log) blanks the name in any `SSID '...'`/`ssid="..."` line (wpa_supplicant's own). `/cmd` refuses `wifi=` commands from a URL (only a POST body) and logs `wifi=join:...` redacted as before.
- CSRF: join, forget and power are accepted only from the page itself (`wifi_conf.cpp` `same_origin`): the Host header must be one of the Pi's names (`<hostname>.local`, its IP; this also stops DNS rebinding) and the Origin header (else the Referer) must be `http://<that host>`; a request with neither is refused. `/cmd` refuses a request whose Origin/Referer is another site's (tools without those headers still work). The headers come from `pi/circle/circle-http-origin.patch` (Circle's HTTP daemon keeps Host, Origin, Referer; applied by `build.sh`). **Remaining risk:** the page has no login (a pairing code is in the backlog): anyone on the same network can open sidecar.local and change the Wi-Fi settings or run console commands, and plain HTTP can be read on the network (the password crosses the LAN once, in the clear, when it is sent).
- Tests: `make -C pi/engine/tests` (`test_wifi_conf`: the checks, wifi.txt round trip, the atomic save and restore with failing writes and renames, recovery, the trial marker, same-origin, SSID redaction, the scan list); `node --test pi/engine/tests/page_wifi.test.mjs` (the page's check, and the flow against `mock_web.py`: the refusals, a scan, a wrong password failing and reverting, a good join, forget; nothing echoes the password or logs the SSID). `mock_web.py` serves fake networks; the password `wrong-pass` fails, an SSID not in the scan is not found, `MOCK_TRIAL=` sets the trial's seconds (default 6).
- Not yet on hardware: the scan and a join (the user's network).

### Measurements

- 2026-10-16, Pi 4 on the Mac's link-local Ethernet (169.254.61.60/16), the Rytm off: `sidecar.local` resolved by `dns-sd -G v4` and `getaddrinfo` (7 ms), the page 12.2 KB in 13 ms, `/status` and `/log` answered; `dns-sd -B _http._tcp` lists `Sidecar | Expander`.
- [ ] With the Rytm streaming: the page open and polling for 10 min, `rytmob: ... gap 0 lost 0`, `sidecar: ... late 0/0/0`, engine `under 0`, and `/status`'s `web.maxus` (the longest handler) logged. Not yet measured: the Pi hung after another kernel was flashed (power-cycle, then `CIRCLE_TREE=circle-engine ../circle/flash.sh ../engine`).

### Phase 2: the same over USB-C (plan, not built)

Move's way: plugging the Pi into the Mac's USB-C alone gives sidecar.local. What it takes, and why it is not built yet:

1. A USB network function in our composite device (`sidecar_gadget.*`), next to UAC2 12/12 and USB MIDI: CDC-NCM (macOS's driver since 10.15; preferred: several Ethernet frames per transfer) or CDC-ECM (fallback, simpler: one frame per bulk transfer, also native on macOS). Each is an interface association with a communication interface (an interrupt IN endpoint for the link and speed notifications) and a data interface with alternate setting 0 (no endpoints) and 1 (bulk IN + bulk OUT), plus the CDC header, union and Ethernet functional descriptors and a MAC address string. NCM adds the NTB parameters request and NTH16/NDP16 framing.
2. Endpoints and FIFO RAM: the Pi 4's DWC2 has a fixed endpoint count and shared FIFO RAM; UAC2 12 in / 12 out at 24-bit high speed (isochronous, up to 1 KB+ per microframe each way) and MIDI already use 5. Three more (interrupt IN, bulk IN, bulk OUT) need the TX FIFO split to be checked against the isochronous IN's (circle-gadget.patch sizes them); this is the main risk to the audio.
3. A Circle network device bound to that function (a `CNetDevice` whose send queues a frame on the bulk IN, whose receive takes frames from the bulk OUT). Circle's `CNetSubSystem` runs one network device: Ethernet or the USB link, not both, unless the network layer is patched for two interfaces (routing by subnet). The simple first step: `net=usb` uses the USB link instead of the Ethernet.
4. A tiny DHCP server on that link (UDP 67: answer DISCOVER and REQUEST with one fixed address, e.g. the Pi 10.77.0.1, the Mac 10.77.0.2/24, no router so the Mac keeps its own internet route) - or no DHCP at all and both ends link-local (macOS self-assigns 169.254/16 on an interface without DHCP after a few seconds; the Pi then needs link-local autoconfiguration, ARP probing for a free 169.254 address).
5. mDNS on that interface: unchanged once it is the network device.

Effort: about 3-5 days (descriptor and class requests 1, the net device and NCM framing 1-2, DHCP 0.5, the endpoint/FIFO budget and a long audio run with the link busy 1). Risk: medium-high, all of it on core 0's USB-C path that carries the 12-channel audio; every step needs the `sidecar_audio_test.py` long run at 0 dropouts. Recommended order: ECM first (smaller), `net=usb` either-or, link-local on both ends (no DHCP), then NCM and two interfaces if it proves worth it.

## Safety

- Nothing is sent to the Rytm except Overbridge blocks, enumeration, SET_INTERFACE and, on its USB MIDI port, the engine's host-link replies (since 2026-10-16; `tohost=0` drops them): 1D slots (every 5 s and on 10), 1A EMPTY / 1B empty lists / NOT INSTALLED for the user slots, 0E listings, 14 pool table, 16/17 JUNO and FM names, 15 JUNO sliders - the messages the Teensy sends. No settings, no firmware, nothing that writes the Rytm's storage. The Rytm's firmware is never touched.
- Every output path has a 200 ms fade-in after a stream starts and a hard limit at -12 dBFS. An empty FIFO sends silence.
- A voice is muted (`!MACHINE` in the stats) until `:unmute` when:
  - it goes over its budget three blocks running;
  - it faults while it renders. Its core recovers and the other voices keep playing.

  Built-in machines have no name message to put `!NAME` on the Rytm's screen. Plugins (v2) will use 1A's MUTED state.
- A render core that does not finish a block within 20 ms is fatal: clean stop, then reboot.
- Reboots (the flasher's magic, a fatal error) stop the Overbridge stream first: no resubmits, the URBs drained, alt 0 on interfaces 1 and 2.
  - A hang of core 0 reboots through the watchdog (4 s) without that stop.
  - A fault on core 0 only stops resubmitting before Circle's panic.
  - A hung boot bench is fatal like a hung render core (`bench=0` skips the bench).
  - In both cases the Rytm may then need a power cycle (spike3).
- The Pi's SD card is written: the core's `/.rytm/` index and the self-test content.
- Plaits' noise generator is one state shared by cores 1 and 2, so noise engines are not bit-reproducible on the Pi. Determinism tests stay on the Teensy and the Mac.

## Measurements (Pi 4 4 GB, 1.5 GHz, 2026-10-15, the Rytm off: the self-test transport)

`pi/engine/test-on-pi.sh`, every step PASS. Logs in `pi/circle/out/` (git-ignored). Spike3's reference: PLAITS at 1500 MHz, a mean of 0.95 % of a core per voice, worst HARMONIC 1.98 % (`docs/pi-circle-spike.md`).

- Boot: the upload of the 597 KB kernel takes 8.4 s at 921600 baud; flashy's "go" comes 14-15 s after the start into a running kernel (the reboot magic and the bootloader's start), 9-10 s into a waiting bootloader. Then the banner at 0.26 s of kernel time, the boot self-check at 0.88 s, cores 1-3 up by 1.04 s, the first stats line at 3.32 s (task4-boot, `engine-20261015-053133.log`).
- Boot self-check: 1 s of PLAITS VA rendered in 9050 us (0.9 % of real time), peak 13975 (task3-selfcheck, `engine-20261015-053028.log`).
- Temperature: 46-48 C idle (the stats line).
- After a reboot the serial bootloader's `IHEX-F` comes 5.2-5.3 s after "reboot:" (clean stop), 9.4 s after the watchdog test's hang (4 s watchdog).

### Boot bench

`engine-20261015-071142.log` (task7-bench). Each value is a mean % of a core per voice, over 1 s of audio (note 48, a retrigger on 55). Every core gives the same output (the per-core lines' peak: plaits 14198, juno 5359, fm 7849, daisy 11095, dsps 8043, dd 17925, the same as the Mac's core).

| machine | c0 | c1 | c2 | c3 | worst block | voices a core (80 %) |
|---|---|---|---|---|---|---|
| plaits | 0.73 % | 0.73 % | 0.73 % | 0.73 % | 3.6 % (budget 25 %) | 109 |
| juno | 0.54 % | 0.54 % | 0.54 % | 0.54 % | 2.2 % (budget 25 %) | 148 |
| fm | 1.22 % | 1.22 % | 1.22 % | 1.22 % | 3.6 % (budget 25 %) | 65 |
| daisy | 0.97 % | 0.97 % | 0.97 % | 0.97 % | 2.6 % (budget 25 %) | 82 |
| dsps | 0.47 % | 0.47 % | 0.47 % | 0.47 % | 2.1 % (budget 25 %) | 170 |
| dd | 0.23 % | 0.23 % | 0.23 % | 0.23 % | 2.0 % (budget 25 %) | 347 |

The worst block is core 0's (its first renders, cold caches); on cores 1-3 it is 0.6-1.8 %.

### The core split

Voices 0-3 on core 1 and 4-7 on core 2 (`split=4`) hold: the worst machine (fm) costs 1.22 % of a core per voice, far under the 20 % that four voices a core at 80 % allow.

### FIFO target: 96 on USB (confirmed with the Rytm, 2026-10-15)

`FifoUsb` stays 96 frames (2 ms of added latency). Measured with `pi_run.py gaps 120 --fifo N [--machines dsps]` on the stock Rytm (OVERBRIDGE, its sequencer sending note 36 on channel 1 throughout), all 8 voices droning. "pass max" is core 3's longest pass in each 10 s window after the warm-up; "FIFO min" the lowest level after it (in brackets: over the whole run, the machine switches included).

| target (frames) | self-test, `gaps 120` | Rytm, the mixed machines | Rytm, DSP SAMPLE x8 from the sample RAM | Rytm, DSP SAMPLE x8 streaming from the card (`sampleram=0`) |
|---|---|---|---|---|
| 48 | PASS (no underflow after the warm-up, OUT 6854..6864/s) | PASS: pass max 233-245 us, FIFO min 23 (0: 17 underruns at the switch to the mixed machines, a 1405 us pass) | PASS: pass max 315-322 us, FIFO min 16 (9) | FAIL: pass max 1052-1060 us every window, FIFO 0, 1260 -> 5860 underruns |
| 72 | PASS (OUT 6852..6864/s) | PASS: 233-244 us, FIFO min 47 (30) | - | PASS, no margin: 1171-1184 us, FIFO min 12 (6) |
| 96 | PASS (OUT 6857..6863/s) | PASS: 233-244 us, FIFO min 68 (49) | PASS: 315-322 us, FIFO min 64 (28, a 1479 us pass at the switch to DSP SAMPLE) | PASS: 1177-1179 us, FIFO min 36 (31); with plaits/juno/fm on 6 voices: 1104-1610 us, FIFO min 36 |
| 192 | PASS (OUT 6857..6863/s) | PASS: 233-260 us, FIFO min 167 (77) | - | - |

Every Rytm run: `gap 0 lost 0`, IN/OUT 6855..6868 blocks/s, no mute, no fault.

The open item (the USB target against core 3's card work) is answered:
- Streaming from the card, each DSP SAMPLE refill blocks core 3 for about 1.2 ms, every 10 s window. 48 frames (1 ms) underflows; 72 (1.5 ms) passes with 6 frames to spare; 96 keeps 31.
- A DSP SAMPLE prepare (a voice's sample or STA changed, `voice V: ready NAME (N us)`) reads the file's head from the card even when the sample plays from the sample RAM: 1088-1318 us, so a pass of 1.4-1.7 ms. Switching voice 2 between `dsps` and `grain` (STA 0 <-> 60) took the FIFO down to 16-26 of 96. The other machines' switches stay at 305-330 us.
- So 96 is the smallest target with margin: the smallest all-PASS target (72) plus one step. Core 3's blocking was not reduced. Two ways to do it if the latency matters later: skip the stream's head read while the sample plays from the RAM, and split a refill's card read into smaller pieces.

#### The TCP link (rytm-emu): 288

The emulator link has its own default, 288 frames (6 ms); USB keeps 96, because the Rytm's latency matters and the emulator is a test harness. `fifo=` (cmdline or console) still sets either. Measured 2026-10-15 with `phase1.py gaps 600 [--fifo N]` (Pi 4 on the Mac's link-local Ethernet, rytm-emu us_us1.v2, four tracks playing, LT streaming sine440):

| target | core 3 wake | Pi underruns after warm-up | emulator `usb OUT frames short` | FIFO min | wanted max | result |
|---|---|---|---|---|---|---|
| 96 | sev only | 148 -> 3489 | - | - | - | FAIL (earlier run) |
| 192 | sev only | 96 more, in 3 bursts | - | - | - | FAIL (earlier run) |
| 480 | sev only | 7 -> 7 | constant | 221 | 448 | PASS |
| 288 | sev only | 7 -> 11 | constant | 25 | 357 | FAIL |
| 192 | event stream | 14 -> 14 | 42899 -> 43059 | 0 | 483 | FAIL (the emulator ran short) |
| 288 | event stream | 130 -> 130 | constant | 88 | 231 | PASS |

Two causes, both on the Mac's side of the clock:
- The emulator asks in bursts. It keeps 512 frames asked for, and the Mac schedules it unevenly, so one ask can be up to 483 frames (`wanted max`). Core 0 polls the link about every 1.2 ms (up to 3.4 ms, `poll gap max`) and answers from what the FIFO holds then, so the FIFO has to cover a burst. Below 288 the emulator runs short. Its frames/s per second stay 47579..48447 in steady state, so the network keeps up.
- A lost wake-up on core 3. Core 0's `sev` after a pull could be consumed by a `wfe` inside core 3's own work (a spinlock), and core 3 then slept until core 0's next pass. The FIFO stayed empty for 2 ms and the link sent silence (the Pi's underruns at 192 and at 288). Core 3 now runs the generic timer's event stream (`cores.cpp` Control: an event every 8192 ticks, 152 us), so its `wfe` never sleeps longer than that. Core 3's longest pass stays under 330 us (`pass max`).

Smoothing the bursts on the emulator side was not cheap: they come from the host's scheduling of the emulator's thread, not from its protocol. A 288-frame FIFO on the link covers them.

### Long run and engines

| run | self-test | Rytm |
|---|---|---|
| `gaps 600`: all 8 voices droning | PASS: OUT 6857..6864/s throughout, underflows 7 (all at the start, none after), pacer slips 0 -> 7 over 600 s, cpu c1/c2/c3 4.0/2.3/4.5 %, no mute | PASS 2026-10-15, with the Rytm's notes (ch 1, 6-8 a second) on top: gap 0 lost 0, under 0 -> 0, cpu c1/c2/c3 4.0/2.3/4.5 %, pass max 233-244 us, FIFO min 71 (36 at the switch), no mute, no fault; IN 6860/s, OUT 6855..6867/s at 650 s |
| `engines`: note 48 per machine, a 10 s capture | PASS: plaits -12.9 dBFS 131.1 Hz; juno -25.0 dBFS 65.4 Hz (factory patch 0 sounds its 16' stop: 65.41 Hz); fm -19.8 dBFS 65.7 Hz (ROM voice 0, SOLID BASS, has its carrier at a 0.5 ratio: 65.41 Hz); daisy -15.7 dBFS, strongest mode 626 Hz (MODAL is inharmonic: 78, 210, 392, 626, 906 Hz, as on the Mac); dsps -18.2 dBFS 220.2 Hz; dd -12.0 dBFS | PASS 2026-10-15 on the Rytm's own notes (`pi_run.py engines --rytm-notes 0`: ch 1, note 36, 28 trigs in 4 s), OUT MAIN L: plaits -12.9 dBFS 65.6 Hz (want 65.4); juno -24.2 dBFS 32.7 Hz; fm -16.5 dBFS 32.6 Hz; daisy -14.8 dBFS, strongest mode 312 Hz (626 / 2); dsps -18.2 dBFS 110.9 Hz (want 110.0); dd -12.0 dBFS; grain -23.1 dBFS, -25.6 dBFS in the last 3 s. Every machine: OUT 6854..6866/s, under 0, no mute. The first run's juno/fm pitch read 1000 Hz: the autocorrelation search started inside the zero lag's lobe, which a 32.7 Hz note keeps high past 1 ms; it now starts at r's first negative value |
| latency: the voice in MAIN after the Rytm's drum on the same step | - | 2026-10-15, FIFO 96, DIGIDRUM EFM-BD on ch 1 note 36 (`capture.py` + `spike3/latency.py --jump 1.5`): **median 4.71 ms** (4.44-5.52 ms, 70 of 70 notes); OUT -> IN loop 289 samples (6.02 ms, -10.9 dB); MIDI arrival -> the voice's onset sent: median 1.87 ms, max 2.00 ms (the FIFO); the note-on reaches the Pi 3.26 ms before the drum's onset in MAIN. Spike3's 2.9 ms had an 18-frame FIFO; the 96-frame FIFO adds its 1.6-2 ms |
| guard | PASS: over budget (longest render 302 us, budget 62 us) and crash mutes, logged at the next stats line (< 0.1 s); faults 1-3 on core 2 resumed at task level (EL1t); fatal hang: clean stop, reboot; 3 clean stops on the magic; the watchdog reboot | the same on the stream: pending (Rytm) |
