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
  USB drives (`USB:`), the Pi 5's NVMe SSD (`NVME:`, Circle's `addon/nvme`, experimental; `nvme=0` skips it), exFAT behind `STOR_EXFAT=1` (off), and the storage bench `stor=bench[:VOL]`: `docs/STORAGE.md`.
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
- **The pairing code gates port 7310** (below, "Pairing code"): a TCP connection answers nothing but `PAIR <code>`, `AUTH <id> <secret>` and `BYE` until it is authenticated, because this is the port that replaces the firmware. `sdboot.py` does it for you: `SIDECAR_PAIR_CODE=12345678 uv run ... sdboot.py install` the first time per Pi, then it is remembered in `~/.config/sidecar/pair.json` and later runs never send the code again. The serial `:sdf` path is **not** gated (physical access, and the way back in when the network side is locked out).
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
| 0 | the xHCI interrupt (Overbridge IN/OUT, OUT paced by IN), USB plug-and-play, the Rytm's USB MIDI (read only), the console, the stats a second, the watchdog, the clean stop; the self-test transport's pacer; the card queue (`cardq`: the streams' reads, the pool RAM's loads and the POOL lists' reads and writes (docs/POOLS.md), then the recorder's writes) |
| 1, 2 | voices 0-3 / 4-7 of each 12-frame block (`split=`), each voice timed by the guard; core 2 then runs the send FX (below) |
| 3 | the MIDI bytes (parser, note adapter) and console commands into the core, blocks while the FIFO is below its target, card work otherwise (FatFs, DSP SAMPLE; fix1041 and the pools build: the machines' loads, the pool RAM's loads and the POOL list files are posted to core 0's card queue instead - docs/SRC-BANKS.md section 7, docs/POOLS.md) |

At boot, before the render loops start, every core runs the bench in turn (cores 0, 1, 2, then 3). See "Measurements".

## Options (`cmdline.txt`)

| option | default | |
|---|---|---|
| `out=main\|tracks` | tracks | MAIN L/R carries the sum of the voices (stock Rytm), or each voice goes on its track's Overbridge channel (`ob::kVoiceToRytm`, mod build) |
| `fifo=N` | per link: usb 96 (confirmed with the Rytm), tcp 288 | output FIFO target in frames (24..1024), for every link when given; latency added: N / 48 ms. Without it each link sets its own default when it becomes the host (`FifoUsb`, `FifoTcp` in `kernel.h`; "FIFO target" below) |
| `notes=on\|off` | off | MIDI channels 1..8 play voices 0..7 (the note adapter) |
| `voicebudget=P` | 50 | a voice's budget in % of a block (5..100), the share measured at 1500 MHz: it is scaled by 1500 / the CPU clock (`cpumhz=`; "CPU clock" below). A voice is muted only when it is over budget while its render core is overloaded (its block took 80 % of a block or more), 200 blocks (50 ms) in a row; a burst never mutes. A muted voice is unmuted by `:unmute` or by a change of its machine |
| `cpumhz=1500\|1200\|1000` | 1500 | the CPU clock option (Settings > System; `/.rytm/display.txt` has it too, cmdline wins). 1500 is the board's full clock; "CPU clock" below |
| `split=N` | 4 | voices 0..N-1 render on core 1, N..7 on core 2 (1..7) |
| `display=0\|1` | 1 | the HDMI display and its USB touch, drawn from core 0's loop (`docs/pi-display.md`); `width=` `height=` (Circle's) set the frame buffer's size, else the firmware's mode |
| `bench=0\|1` | 1 | the boot bench (6 machines x 4 cores, 1 s of audio each; see Measurements). A core that does not finish its bench in 20 s is fatal (`bench: core N did not finish ...`, clean stop, reboot into the bootloader); `bench=0` then boots without it |
| `tohost=0\|1` | 1 | the engine's host-link replies (1D slots, 1A EMPTY, 1B, 0E listings, 14 pool, 16/17 names, 15) go to the Rytm's USB MIDI OUT (umidi1, cable 0), as the Teensy's do; 0 drops them (counted), the behaviour before 2026-10-16 |
| `selftest=1` | 0 | start the self-test transport at boot (no Rytm) |
| `poolram=MB` | 1024 (2 / 4 GB boards), 3072 (8 GB) | the pool RAM (docs/POOLS.md section 5): one hash-keyed sample cache shared by GRANULAR, DSP SAMPLE and MULTISAMPLE, from the high heap at boot (at most its free space less 64 MB, else the low heap's less 192 MB). It replaces MULTISAMPLE's `msampram=` arena (no longer taken) and DSP SAMPLE's own sample RAM |
| `sampleram=MB` | 256 | 0: DSP SAMPLE streams from the card only; any other value turns its RAM on, which is now the pool RAM (`poolram=`), so the size is not used (the Teensy build keeps the old sample RAM) |
| `capch=N` | markers | the 4th channel of the `w` capture: IN channel N (0..11), or the note-on markers (default, 12 or more) |
| `recbuf=MB` | auto | the recorder's memory, reserved once at boot from the low heap (19.2 MB of it the files' 768 KB write buffers, the rest the ring, every slot of it used); not given, on a board with a high heap (2 GB and up: the pool and MULTISAMPLE take theirs there): half the low heap's free space past 192 MB, 64..256 MB (a Pi 4: 256 MB, about 55 s of all 25 channels at 24-bit); not given on a 1 GB board: 64 MB, as before; given: as given, at most 1024; 0: recording off (the boot log says which, and when there was no room) |
| `web=0\|1` | 1 | the web page at http://sidecar.local over the Ethernet (below, "Web page"); 0: no HTTP daemon and no mDNS (the network stays up for `link=tcp`) |
| `paircode=0\|<8 digits>` | (absent: a random code at every boot) | the pairing code that gates the network (below, "Pairing code"). `paircode=0` turns the gate **off**: everything on the network is open, as it was before 0.3.0 - a development card only, and the engine says so at boot, once a minute, in `/health`, in `/status` and on the STATUS page. `paircode=13572468` keeps the gate but fixes the code: this is the way to run a unit with no display at all, and it is the alternative to the code being written to `/sidecar-paircode.txt`. Anything else is ignored (the gate stays on with a random code) |
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
| `notemach=V:NAME` | voice V (0-7) plays machine NAME on its MIDI channel V+1: `plaits juno fm daisy dsps grain md` (`md`: the built-in MD, core/md/md.h, in DIGIDRUM's `dd` place since 2026-10-29). Defaults that sound: PLAITS VA, JUNO factory patch 0, 6-OP FM ROM voice 0, DAISYSP MODAL, MD FM BD, DSP SAMPLE pool slot 1 (the pools build: SAMPLES row A001) (`grain`: the same in GRAIN from the sample RAM, dense: every retrigger stops sounding grains) |
| `drone=V\|all\|off` | the engine's own notes for tests: every 0.5 s a note-off and a note-on (note 48 + 2V) on voice V or on all eight |
| `notes=0\|1` | MIDI notes off/on (as `notes=` at boot) |
| `out=main\|tracks` | the output mode (as `out=`) |
| `fifo=N` | the FIFO target (as `fifo=`; the Task 7 sweep). It stays across `link=` switches until the next `fifo=` |
| `fxin=1\|0\|?` | the IN L/R source (below, "IN L/R source"): 1 the Rytm's FX return on Overbridge IN L/R, 0 its AUDIO IN, ? ask the Rytm; `fxin` alone reports. Needs the rui1024 Rytm build |
| `unmute` | clear every mute and any pending `overtest` |
| `faulttest=V` | guard test: voice V's next render faults (a write to an unmapped address): the voice is muted "crashed" and the others play on |
| `overtest=V` | guard test: voice V renders 300 us long for 5 blocks: muted "over budget" |
| `hangtest=V` | guard test: voice V renders 30 ms long once. A render core misses the 20 ms limit, which is fatal: clean stop, then reboot |

Core 0 runs these itself:

| command | |
|---|---|
| `selftest=on\|off` (or `1\|0`) | the self-test transport (below). It is refused while the Rytm is connected, and switches itself off when the Rytm's stream or USB MIDI appears |
| `wdtest` | watchdog test: core 0's loop hangs and the BCM watchdog reboots the Pi in 4 s (no clean stop) |
| `pair` | the pairing code's state (below, "Pairing code"): the gate on / off / fixed, how many devices are paired, how many sessions. **Never the code itself** - the console's output is the log the page serves |
| `pair=show` | the code on the Rytm's screen again (30 s), for when the popup was missed |
| `pair=revoke` | every paired device and every session dropped: each one needs the code again (also `sdboot.py`'s) |

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
- without `/.rytm/project`, `/.rytm/projects/_default/samples.pool.txt` (the pools build: DSP SAMPLE's slot 1 is the SAMPLES list's A001; before, `/.rytm/pools/_default.txt`) naming that file in A001, so DSP SAMPLE's note 48 plays 220 Hz.

Each file is written whole (a `.tmp` file, then a rename), and nothing that exists is touched.

### The stats (a line a second)

- `engine: fifo min A max B target T under U (restart R, streams S) | blocks N max M us | cpu c1 x c2 y c3 z % | pass max P us | <machine %> | midi M B (lost L) sysex S notes N ch C,... | to-host sent B B M msgs fail F dropped D | peak P/1000 | C`
  - `under` counts empty-FIFO frames (sent as silence) **while playing**: the FIFO had reached its target since the last `Output::Reset()` (a new stream). Anything non-zero there is a glitch worth looking at. `restart` counts the empty frames before that (the refill after a new stream, by design) and `streams` the `Reset()`s: a `streams` that climbs during a session means the stream is being re-negotiated, a different bug. See "Underruns and the start-up stall" below.
  - `cpu` is each core's busy share.
  - `pass max` is core 3's longest working pass of its loop (the longest the FIFO waits for a refill).
  - `lost` counts the Rytm's USB MIDI bytes dropped by a full input queue (the `tcp:` line has its own).
  - `notes N ch C,...`: the note-ons this second and the MIDI channels (1..16) they came on; only 1..8 play voices.
  - `to-host sent`: the engine's bytes and messages sent to the Rytm's USB MIDI; `fail` the sends that failed (one turns sending off until umidi1 is found again: a Rytm that stops reading costs one 3 s stall of core 0, Circle's bulk limit, not one per message); `dropped` the bytes not sent (no umidi1, `tohost=0`, after a failure, the TCP/self-test transports' own drops, or a full queue).
  - `peak` is relative to full scale; the limit is 250.
- `thermal: ...` lines come once per crossing of the temperature warning (below), not per second; the stats line's last field is the temperature.
- With `link=tcp`, the `tcp:` line (every 10 s) ends `wanted max W, poll gap max G us`: the most frames the emulator had asked for at once, and core 0's longest gap between two polls of the link.
- With the Rytm, spike3's `rytmob:` line follows: IN/OUT blocks a second, `gap`, `lost`, and so on.
- `engine: muted !PLAITS(6) ...` lists the muted voices. Each new mute is also logged once with its reason.

### Underruns and the start-up stall (2026-10-25)

**What the counters mean.** `Output` keeps two empty-FIFO counts and a stream count (`output.h`). `Reset()` (a new Rytm stream, the TCP link, the self-test, a TCP-to-USB switch) empties the FIFO, fades in again and disarms. The steady-state counter is armed when a stream is up (`streams > 0`), the target is known (`SetFifoTarget`, from `Configure` / `LinkFifoTarget`) and the FIFO has reached `target - 12` frames (the control core fills to within one render block of the target) since that `Reset()`. An empty frame counts as `under` when armed and `restart` otherwise; the gate is per `Reset()`, not per boot, so an unplug and replug mid-session is gated again. Nothing is called "startup": the bucket also catches every later stream restart. Before this, one number mixed both and read 500 to 6000 after every boot while the audio was fine. `/status`: `underruns`, `restartUnderruns`, `streams`; the STATUS page's tile shows `restart N, streams N, glitches while playing`. Keep `restart`: a marginal PSU, a bad cable or a throttling unit shows up there.

**The finding: a start-up stall (deferred).** With the buckets right, `under` was not 0 after boot: it is a real dropout. Measured on hardware (Pi 4, Rytm streaming, 3 to 8 voices of PLAITS, CHOP, WAVE, DSPS): 4 to 7 gaps of the control core (core 3) of 2 to 50 ms inside the first ~2.7 s of the stream, then none in the next 371 s under that load and heavy web polling. The worst seen was 49.7 ms; typical boots 7 to 11 ms, so the variance is wide. The cause, from the frozen start-up trace: core 0's **card service** (`bridge::card_service`) runs dozens of times in the first ~1.5 s, 2 to 4.5 ms each, and whole core-0 loop iterations reach 10 to 27 ms (one 250 ms); the gaps line up with them. That is the SD card loading samples as the Rytm and the engine start. It starves core 3 (not the `wfe`, not the mailbox, not periodic: all measured and ruled out: `wfe_us` was 0 to 84 us in every gap; the firmware calls take 0 to 1.5 ms; no gaps after the first seconds).

**Decision: deferred.** Up to ~50 ms of dropout can occur in the first few seconds after connecting the Rytm while samples load; none after that. Nobody has heard it, and it is bounded to the load. Not a 0.3.0 blocker (user, 2026-10-25). See docs/BACKLOG.md section 7.

**Likely fix, for whoever picks it up.** Pace the card reads against the audio the way `bundle_pi::io_rule` does for the bundle job and the sample library (pieces of 16 KB with a 2 ms gap, no writes while the Rytm plays): the card queue's service (`bridge::card_service (8)` in core 0's loop, `CKernel::Run`) must not run long stretches back to back while a stream's FIFO is shallow. Then re-measure with the trace below: `under` should stay 0 through a replug. A bigger FIFO is **not** the fix: the worst case, 50 ms, would add 50+ ms of latency to hide a stall that should not exist.

**The diagnostics (leave them on in a release build).** All in `/status`, all on one scale: frames pulled since the last `Reset()` (48 per ms).
- `outDiag`: where the counts fell (`preResetPulled`, `resetLevel`, `firstPullLevel`, `armAt`, `armLevel`, `restartFirst/Last`, `underFirst/Last`, `sinceReset`; -1: did not happen).
- `stall`: the control loop's worst gap between two passes since the Reset, with the time of each section of that pass (`midi keys cmds tick render idle`); all small means the core was away.
- `gaps`: the last 8 gaps over 2 ms, `[gap us, atFrames, wfe us, cntkctl, DAIF]`.
- `mb` (the last 16 calls of at least 200 us) and `mbBoot` (the first 64 calls in the first 10 s of the current stream, **frozen** when full, cleared by the next stream): core 0's firmware calls and loop sections (`mbtrace.h` lists the ids; id 30 is a whole loop iteration of 1 ms or more). `:ctlwait=poll` makes the control loop spin instead of `wfe` for an A/B.
- Cost (by reading the code, not measured): a few compares and an add per `Pull`, six counter reads and a 64-bit divide per control-loop pass (about every 152 us idle), two counter reads per wrapped core-0 section; no locks, no allocation, all plain words with racy reads. Cheap enough to leave on.
- **Design lesson: `/status` only answers 20 to 40 s after boot and `/log` holds ~25 s, so anything needed about boot must be frozen, not rolling.** A rolling ring was overwritten before anyone could read it (this one, and the USB hub descriptor line earlier the same day). Freeze at the event (`mbBoot`), or keep a counter that carries enough to reconstruct it (`outDiag`).
- Two bugs this found on the device that the host tests passed: arming at an unset target (target 0 armed at once), and a trace ring flooded by once-a-second reads.

## The RAM store (2026-10-26)

**The rule: after a project has loaded, core 3 (the control core) never calls FatFs.** Every card call on core 3 is an audible dropout (the FIFO holds 2 ms; a card read or write is 5 to 40 ms and waits behind core 0's card service). Spec: docs/superpowers/specs/2026-10-26-ram-store-design.md; plan: docs/superpowers/plans/2026-10-26-ram-store.md.

**How.** `core/store/` holds one project's card files in RAM as bytes keyed by path (`store::get` / `put` / `mark`; a path is unknown, absent, present or locked, and an unknown or locked one is never written over). At a project load core 3 registers the paths it wants (the 129 LFO and 129 INSERT files, the sequencer's pattern, state and track files) and fills the store from its idle pass, one bounded step at a time: each wanted folder is listed once by the card core (`cardq::kListReq`), a path not in the listing is absent with no card operation of its own, a listed one is read whole and held. Kit changes (LFO, inserts), pattern switches and the prefetch then read the store. While the fill runs, edits to the loading project are refused with a visible "loading" reason (never accepted and dropped). The sample browser lists folders from the library's RAM index (card core builds it, core 3 reads it lock-free), not from FatFs.

**Writes.** Edits change the store at once and reach the card later, as atomic writes (write a `.tmp`, rename; `fmt::recover` at boot) that core 3 only posts: `fmt::post_begin / post_end` for one write, `fmt::post_queue` for a copy queued behind it (one entry per path, a newer text replaces the queued one and goes to its tail, no cap and no wait). The card core runs them (`cardq::kAtomicReq`). A write the card core has not taken after 200 ms stays posted (it is never taken back and run on core 3); it is counted in `held`. An edit still inside its 1 s settle (the LFO / INSERT / pattern savers wait for the edits to stop) is not posted yet and is not counted anywhere below. The cost of this design is the power-cut window: an edit sits in RAM until its write lands (the settle, plus the queue). A cut inside it loses the edit and nothing else: the file on the card is the whole previous one (the boot's `fmt: boot sweep` lines restore a file left between the two renames).

**`/status` `"store"`**: `{"files":N,"bytes":M,"ready":true,"unwritten":K,"oldestMs":T,"held":H,"heapFree":F}`.
- `files`: paths held as present (`store::present()`); `bytes`: total bytes held (`store::bytes()`); `ready`: the last fill is complete (`store::ready()`; false while a project loads).
- `unwritten`: writes posted and not yet on the card: `cardq::pending()` (requests in the card queue, not yet taken by the card core) plus `fmt::unwritten()` (fmt's queued writes, plus the slot's write once the card core has taken it - running or held; while it still waits in the card queue it is `cardq::pending()`'s, so each write is counted once; fmt reads plain counters, never its list). Zero when the card is caught up.
- `oldestMs`: the age in ms of the oldest of those (`cardq::oldest_ms()` from the oldest request's `posted_us`, and the oldest queued or slot write in fmt, taken from when it was first queued); 0 when none. This is the age of the oldest unwritten edit after it was posted, so a card that is stuck shows as a climbing `oldestMs` with `unwritten` above 0.
- `held`: how many posted writes the card core had not taken after 200 ms since boot (`fmt::post_held()`, also in the stats line); a climbing `held` means the card core is held up. The edits are safe in RAM and land in order when it runs again.
- `heapFree`: bytes of the low heap (where core's `new` allocates, the store's buffers too) never yet handed out (`CMemorySystem::GetHeapFreeSpace(HEAP_LOW)`: Circle's untouched heap, not counting freed blocks waiting on its free lists). Circle halts with an "Out of memory" panic once an allocation would reach into the heap's last 256 KB - `new` never returns 0 there - so this is the room left before that, not a soft limit.

**`/status` `slowSx`** stays (the longest SysEx/MIDI handlers on core 3, with their ids), and so do `gaps` and `stall` above. The temporary kit-change profile (`kitProf`, `core/prof.h`, the `rytmtx=` console command) is gone.

**Measurements (Pi 4, the user's project, the Rytm on).**
- Kit change (the Rytm's `3B`) on core 3: **39.8 ms before, 0.155 ms after (LFO 125 us + INSERT 30 us)** (device, Task 4; the 40 ms were the per-kit LFO and INSERT files read byte by byte plus a failed open and a fallback open for each).
- Boot fill: 258 files in 7.3 s after the project name on the first version (one failed open and one exists per absent path); the folder listing (Task 5) replaces those with a few card operations (1031 to 8 in the host test). To be measured on the device.
- Pattern switch: about 43 ms of dropout a switch before (7 switches, underruns +14,638, docs/BACKLOG.md); after Task 4 a 14.1 ms idle gap remained at the switch (the pattern prefetch's card read); the pattern files now come from the store (host test: card time of a switch 10,442 us to 0). To be measured on the device.
- Browsing unseen folders in the sample browser: core 3 idle gaps of 12 to 42 ms before (17.8 / 30.1 / 41.8 / 12.2 ms, plus 1.2 to 1.7 ms per `cd`); the listing now comes from the RAM index (host test: ~36 us for a 1000-file folder). After: to be measured on the device.

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
  the measured ns a block. With SPACE (2026-10-23, TYPE 5: 545 KB a slot) `SendFx` is 5.88 MB, taken from the heap at
  boot (`BootAlloc`, not .bss); SPACE with its spring ~4.1 us a block (1.6 % of core 2), beside CLOUDSEED AMBIENCE ~19 us
  (7.6 %), both estimated from the Mac x13.2.
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
- **Files:** `/Recordings/<take>/<NN>_<NAME>.wav`. The take's folder (2026-10-27, BACKLOG 5d) is
  `<slug>_<YYYY-MM-DD_HHMM>` once the Pi has a wall clock (local time; `_2`, `_3` ... when that name is on the SD or
  the destination already, one more than the highest), `<slug>_take-NNN` before (no clock since boot: one more than
  the highest NNN of that slug). The slug is the Rytm project's name (lowercase `a-z 0-9`, every run of anything else
  one `-`, none at the ends, at most 24; `sidecar` with no project), or the name `rec=name:` set (the web page's *Take
  name*; empty: the project's again). Older takes (`take_004`) keep their names. The clock: the web page sends its
  own (`POST /time` with `epoch` and `tz`, minutes east of UTC) when it opens and when it is paired; the Pi has no
  RTC (the Pi 4) and runs no NTP client. `rec_pi` sets Circle's timer from it, so FatFs dates every file and folder it
  makes from then on (`get_fattime`; `/fs/ls` lists them as `"t"`). PCM
  48 kHz, 24-bit (default) or 16-bit (truncated), mono or stereo. The header is 512 bytes (RIFF, fmt, a JUNK pad,
  data), so the samples start on a sector and every write is whole 512-byte sectors. Every file of a take starts on
  the same IN frame and has the same length. The voices and FX are the frames the OUT fill made around that IN
  block (a FIFO with up to 16 blocks of lead, 2.3 ms: the OUT fill can run several blocks behind the IN completion). The round trip to
  the drum machine (about 6 ms) is not compensated.
- **Space up front:** each file is opened with a contiguous area reserved (`f_expand`, up to 64 MB, half the free
  space over the files at most; a refusal halves the size for that file and the next ones, a refusal at 4 MB or 1.5 s
  spent ends the reserving for the take - the start line logs the MB and the ms; the worst case at a take's start is
  those 1.5 s plus one full FAT scan, the call that passes them still running to its end), then synced once (its directory
  entry owns the area at once: a power cut never leaves the chain unowned). Its writes touch no FAT until the area is
  used up (with 25 files the FAT sectors no longer take turns in FatFs's one window); the close writes each file's
  final length into the marker, then cuts the file to its data (`f_truncate`). The tree's FatFs needs
  `FF_USE_EXPAND 1` (pi/circle/circle-fatfs-expand.patch, applied by pi/engine/build.sh with a marker guard, FatFs
  alone rebuilt; without it a take records as before). A file's staging is 768 KB, written in 64 KB pieces one after the other (long runs a file).
- **Power loss:** while a take records, the header's sizes are a placeholder (the largest whole-frame size a
  32-bit RIFF allows), and one file a 2 s round is `f_sync`ed (its length reaches the directory). With space reserved
  the file's size is the reserved area's, so `.recording` (the marker) holds each file's length as of its last sync
  (on the SD patched after each sync, on a drive once a sync round; a failed patch is logged once and tried again): `recover` cuts the file back to that and
  writes the header (never the area's leftovers); a cut that fails keeps the marker. A header with real sizes (a
  clean close) is trusted everywhere: a file longer than it is cut to it, its sizes never grown to the file's
  (`recwav::fix` grows only the placeholder) - the boot scan and `rec=fix:` included. A file cut by a
  power loss holds everything up to its last sync and still opens (most readers take the data to the end of the file).
  The next boot scans `/Recordings/*/*.wav` and writes the true sizes into any header of ours that does not match its
  file's length (`rec: ... sizes fixed`). The files of a cut take start on the same frame but can end at different
  points (each at its own last write and sync). A clean stop writes the sizes itself (and RIFF's pad byte after an odd
  data size).
- **Limits:** a take stops by itself before any file reaches 4 GB (FAT32), checked against the widest
  file (stereo 24-bit: about 4 h 8 min). A failed write (card full) stops the take and closes the files.
- **To a USB drive** (2026-10-26; docs/STORAGE.md "Recording to a USB drive"): `rec=vol:SD|USB|USB2|USB3|NVME` (the
  Record tab's *Record to*), refused while armed or recording; a drive that is not mounted is refused at start / arm,
  never the SD instead. The take goes to `USB:/Recordings/<take>/`, is called `USB:<take>` in the lists, `/status` and
  the `play=` / `rec=del:` commands, and `<VOL>:/Recordings/.recording` names it while it records (the take fixed by
  `Recorder::recover` when the drive is mounted again; `rec=fix:VOL` checks a whole volume, on the task the command
  came from). The web page adopts the Pi's destination (it never pushes one when it opens). `/fs` never deletes or
  renames the take being recorded (any volume). Measure the drive with `stor=bench:USB` first.
- **Path:** the IN completion (core 0's xHCI interrupt) copies the selected samples of each block into a ring
  of fixed slots in the reserved memory, together with the OUT fill's voice/FX frames (a 512-frame SPSC between
  the two). A full ring drops the block and counts it, and the block is later written as silence, so the files
  keep time and stay aligned. Core 0's loop (`rec_pi::Poll`, once a pass) deinterleaves into a 768 KB buffer
  a file and writes one 64 KB piece of a whole buffer a pass (`f_write`, sector-aligned), so the network and the
  display run between writes. Nothing allocates or blocks on the audio side, and nothing logs there.
- **Start/stop:** manual, or armed: the drum machine's MIDI PLAY (FA, or FB continue) starts a take at the next IN
  block, STOP (FC) ends it, and the recorder arms again for the next PLAY. A manual stop disarms. The realtime
  bytes come through core 3's MIDI parser (`rec_pi::Transport`, then the send FX's clock as before).
- **Console:** `rec` (the state), `rec=start`, `rec=stop`, `rec=arm`, `rec=ch:LIST` (ids or names, comma
  separated: `all`, `none`, `in`, `voices`, `master`, `main_l`, `bd`, `rs-cp`, `voice1`, `fx2`, ...; `+id`/`-id`
  add or remove), `rec=bits:16|24`, `rec=name:PREFIX` (1-24 of `A-Z a-z 0-9 _ -`; `rec=name:` alone: the project's
  name), `rec=vol:SD|USB|...` (above),
  `rec=fix:VOL`. Changes need a stopped recorder.
  `rec=del:TAKE` deletes a take (its files in `/Recordings/TAKE/`, then the folder; `TAKE` a folder name of
  `A-Z a-z 0-9 _ -`, no path): refused while a take records or closes; the take's playback is stopped first.
- **Log:** `rec: <take> started: N files, 24-bit, channels ..., KB/s, ring N slots (ms)`; every 10 s while
  recording a progress line; at the end `stopped: frames, KB in N writes, card KB/s, worst write ms, ring max %,
  dropped N blocks, OUT side short N`; `the card is behind: N blocks dropped` at most once a second.
- **Page:** one checkbox a source in groups, Select all / None, the record button (Record / Arm, Stop / Disarm),
  the elapsed time, a peak meter a selected channel (the last second's peak from `/status`, while armed or
  recording), free space and the time left at the selection, the card's speed and worst write, a red warning on
  dropped blocks. Options: manual or armed, 24/16-bit, the take name. The selection's rate is shown as a share of
  the destination's write speed (`/status` rec `speedKBps`: `stor=bench`'s 64 KB writes, kept in
  `/.rytm/recspeed.txt`; not measured: 4 MB/s for the SD, 6 for a USB drive, 50 for the NVMe SSD); over 70 % Record
  asks first (the take still starts on OK). Below them the takes, with their date, sorted by date (newest first) or
  name (a click on the column; the Files tab's list the same) (download each file through `/fs/get`, delete). The
  selections are kept in the browser (`localStorage`, `sidecar.rec`) and sent to the Pi when the page opens.
  The Files tab has a Recordings button.
- **Display:** the Status panel shows a red `REC h:mm:ss` while a take records, `REC armed` while armed (OLED: in the
  title bar); a tap on it opens the touchscreen's **REC page** (`pi/ui/sidecar_rec.c`, docs/UI-THEMES.md "REC"), as
  does UP + T7 on the Rytm (rui1052's `49 1 7`, rui1038's `49 1 8` before it; with a display the Rytm's own screen
  stays where it is, `core::ui::remote_display_owns`; without one, or on the Sidecar Mini, UP + T7 opens the Rytm's
  REC page as before, and TRK + YES pages to it either way). The page drives the recorder through the same console commands (`rec=start`,
  `stop`, `arm`, `ch:+ID` / `ch:-ID`, `ch:all|none`, `del:TAKE`).
- **Meters for the display** (`Recorder::set_meters`, `meter_peaks`): while the REC page shows (`rec_pi::UiWant`),
  `in_block` keeps each of the 12 IN channels' peak and `out_block` each voice's and each FX return's L / R, in any
  state (selected or not, idle too: `wants_in` / `wants_side` say yes for that), a compare a sample in the code that
  already touches the audio. `rec_pi::UiFill` takes them (exchange to 0) at most 25 times a second; they are apart
  from `Status::peak`, so the web page's `/status` and the display never take each other's peaks. Off (the page
  hidden): nothing is computed.
- **The display's takes list:** the newest 64 folders of `/Recordings` (by time stamp, then name; newest first, a
  vertical drag on the rows scrolls them a row at a time, the header says which show), read one step a
  loop pass (the listing, then one take's folder: its WAVs' sizes, the first file's header for the length) and only
  while no take records or closes; read again when the page opens, when a take ends and after a delete.
- **Playback** (`rec_play.h`, 2026-10-18): one channel of one take into the Overbridge return's MAIN L / R (a mono file
  on both), added in the OUT fill after the voices, the computer's OUT and the send FX returns (`ob_usb.cpp` FillOut),
  so the voices' paths (`out=main|tracks`) stay as they are. Core 0's loop reads 30 KB a pass into a 1.37 s ring
  (0.5 s held before the sound starts); the OUT fill only pops. 10 ms fades in, out on a stop, on a channel change
  (which resumes the new channel at the same frame) and when the ring runs low before the file's end (the card fell
  behind: faded out, `card too slow`, counted). Never while a take records: a start is refused, a take that starts
  (YES, the console, the Rytm's PLAY on an armed recorder) fades playback out. Level 0..127 (gain (n/127)^2; 100 by
  default, -4 dB; 127 as recorded). A take that ends becomes the one selected. Remote UI: the REC page (B / C / D,
  `core/ui/README.md`); console: `play`, `play=start|stop|pause|scan|take:N|ch:N|level:N|start:TAKE/NN|toggle:TAKE`; `/status`'s
  `"play"`; the web page: a button per channel in the takes list and Stop. Pi only (the Teensy has no recorder).
  The touchscreen's REC page (TAKES) uses `play=toggle:TAKE` (play / pause that take, or switch to it: MASTER if
  the take has one, else its first file) and `play=stop`; `Player::pause()` fades out and keeps the frame, `play()`
  then resumes there (`Status::paused`; a stop, another take, a scan that changes the selection or a recording start
  forget it); the page reads `pl_state` / `pl_take` / `pl_pos_ms` / `pl_len_ms` from `UiFill`.
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

## Sample library and formats (2026-10-22, docs/LIBRARY.md)

- `lib_pi.cpp`: the library's step in core 0's loop right after the bundle job's step (only when that did nothing,
  the card queue served first), the bundle job's pause rules (`bundle_pi::io_rule`: 16 KB reads every 2 ms and no
  writes while the Rytm plays or a song take records, nothing while the Overbridge recorder records), never during a
  bundle job, never on core 3. `lib_pi::begin` after `bundle_pi::init` (the card mounted, the bundles' boot recovery
  done).
- Console (core 0): `lib=status|rescan|verify|bench|find:<sha256>|hash:<path>` (the pools build: the POOL lists are `pool=smp|wt|inst:...`, the browsers `browse=...`; docs/POOLS.md).
- `/status` `"library":{state,files,bytes,hashed,pending,pct,dups,dupBytes,missing,unreadable,recordings,waiting,
  logLines,readOnly}`; the Status tab's *Library* line; the touchscreen STATUS page's *Library* row.
- Writers report their files (`lib::touched` / `lib::moved`, all on core 0): the web's uploads with the sha256 made
  from their chunks (`fsweb::Backend::stored`), deletes and renames, the sampler's and auto-sampler's WAV / `.sfz`
  files with their hash, the recorder's takes and deletes (indexed only), sdfiles' PUTs, a bundle import's files.
- `File::nextEntry` (platform_pi.cpp): a folder's next entry by one `f_readdir`, no file opened (the walk).
- The format registry (`core/fmt`) is in the kernel; `display.txt` keeps the lines this build does not read.
- Measurements to make (L5): `lib=bench` (sha256 and CRC-32 MB/s from RAM, the card's read MB/s) on a Pi 4 and a Pi 5;
  the first pass over a 20 GB card with the Rytm streaming 8 voices (0 underruns, the pass max under 2 ms); the boot
  walk's time for 100 000 files; a publish's time at 100 000 files. Mac / card-model figures: docs/LIBRARY.md "Cost".

## Tests

- On the Mac:
  - `make -C pi/engine/tests` runs spsc, output, midi_in, notes, guard, records, emu_proto, usbmidi_pack (the replies' USB MIDI packets), sendfx_pi (the send FX's rings, priming, route, underrun, each FX's own return) paircode (the pairing code: the limiter, sessions, the card's file, the 7310 lines; "Pairing code" below) and recorder (the WAV header and its boot fix, the channel lists, a take's files lined up sample for sample, 16-bit, a ring overrun written as silence, arming by PLAY/STOP, the display's meters: every channel while idle, apart from `/status`'s peaks).
  - `make -C pi/engine/tests` also runs `test_bundleweb` (the project bundles' endpoints over core/bundle; it links the core's Mac library: `make -C core engines` first, the library is built by the rule) and `node --test pi/engine/tests/*.mjs` the page's tests (`page_project.test.mjs`: the Project tab and the download's service worker; `page_lib.test.mjs`: the Status tab's Library line).
  - `make -C pi/engine/tests content` runs the self-test content on the core's Mac port: DSP SAMPLE plays 220.0 Hz, and the bench renders every machine.
  - `uv run python pi/engine/tests/test_pi_run.py` runs `pi_run.py`'s analysis on synthetic captures and log lines, including the bench lines.
  - `python3 pi/engine/tests/test_sdboot.py` runs `pi/circle/sdboot.py` against stand-in engines on the SD file port: the pairing code's handshake, and a pre-0.3.0 engine with no gate at all ("Pairing code" below).
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
- Console extras for the acceptance (core 3): `:pool=S:path` sets the current project's pool slot S to the card file `/Samples/path` and sends 14 to the host (`pool slot S = path: ok`; `:pool=S:` empties it); in the pools build that is SAMPLES row A00S (`pool slot S (A00S) = path: SET`), and `:pool=smp|wt|inst:...` edits the POOL lists (docs/POOLS.md). `:juno=ren:S:NAME`, `:juno=del:S`, `:juno=swap:A:B`, `:juno=save:V[:NAME]` (the patch JUNO voice V plays now, its six knobs as they are) and `:fm=ren|del|swap`, `:fm=save:V` (the ROM voice FM voice V plays, while the list is empty) edit the project's JUNO / FM lists and send 16 / 17 (`core/user/lists.h`). Commands are up to 191 characters. `:reportlog=1` logs each 0x04/0x03 report (`report 04 voice V flags FF P1..P4 a b c d`); `:reportlog=0` turns it off.
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

- **Everything here is behind the pairing code** since 0.3.0, except the page itself, `/health`, `/log` and the pairing endpoints: see "Pairing code" below for the full open set and for what `/fs/*` can reach.
- Name: the network's hostname is `sidecar`. Circle's mDNS daemon answers `sidecar.local` (A) and publishes the service `Sidecar | Expander` (`_http._tcp`, port 80; `dns-sd -B _http._tcp` lists it). A second Sidecar on the same network becomes `sidecar-1.local`. `pi/circle/circle-mdns.patch` (applied by `build.sh`, rebuilds `lib/net`) makes Circle's daemon read every question (with name compression), accept the QU bit and any case, and answer AAAA with an NSEC "A only" record: without it macOS's `getaddrinfo` (curl, browsers) waited 5 s for an IPv6 address before using the A record (measured 2026-10-16: `curl http://sidecar.local/` timed out at 5 s; with the patch 7 ms lookup, 13 ms for the page; `dns-sd -Q sidecar.local AAAA` shows "No Such Record").
- Addresses: the Pi has no DHCP client on the direct cable, so its address is built in or set in cmdline.txt (`ip=`, `netmask=`, "Phase 1" above). For a Mac on a self-assigned link-local address (no manual IP), build with `PI_IP=169.254.61.60 PI_NETMASK=255.255.0.0 ./build.sh kernel` (any free 169.254.x.y/16): the Mac then reaches it with no network settings at all. The Mac's manual 192.168.77.1/24 and the default 192.168.77.2 work too.
- Endpoints: `GET /` the page; `GET /status` JSON (host, ip, uptime, link, gadget mode, Sidecar up, computer audio, Overbridge streaming, the Rytm's USB MIDI, temperature (`temp`) and its warning (`tempWarn`), out, notes, drone, FIFO target, underruns, and the page's own request count and longest handler time in us); `GET /log?since=N` the console log from byte N (first line `START END`, then the bytes; the next poll asks `since=END`), from a 64 KB ring that the logger fills next to the serial port (`CLogTee`: any core may log, one spin lock); `POST /cmd` with `c=<command>` (form encoded): the serial console's own parser (`CConsole::Command`, the `:` lines), logged as `web: command :...`; `GET /lists` the project's pool and JUNO / FM lists and each voice's machine (`core/user/lists.h`, published by core 3 when it changes); `GET /pool?l=smp|wt|inst&b=A..H[&u=1]` one bank of a POOL list with the lists' status and the RAM figure (`pi/engine/poolweb.h`, read only). `wdtest` is refused from the web (its reboot ends a RAM kernel; with sdboot the card's engine comes back).
- The page polls `/status` every second and `/log` every 0.8 s. Tiles: CPU of cores 1/2/3, FIFO min..max and target, underruns, Overbridge (IN packets/s, gap, lost: parsed from the `rytmob:` line), Rytm attached (stream or USB MIDI), USB-C gadget (alt settings, computer audio, late), temperature, the page's own cost. Controls: notes on/off, out main/tracks, computer audio on/off, a machine per voice (`notemach=`), drone on/off, FIFO target, and a free command line. The per-second stats lines are hidden in the log view by default (a checkbox shows them). The **Pool** tab (the pools build): the three POOL lists, banks A..H, the RAM figure, the rows' states, Accept, Replace, Unload, Select unused, Undo (docs/POOLS.md section 6); the Files tab's +SMP / +WT / +INST and Fill bank. The **Presets** tab (before: "Pool & presets", with the DSP SAMPLE pool, now in the Pool tab): the JUNO and FM lists (rename, move up, delete, save what a voice plays now) and each assigned user slot's presets in `/PRESETS/<id>/` (rename, delete through `/fs`, save current with `um=save`).
- Project bundles (2026-10-22, docs/BUNDLES.md): the Project tab and `GET /bundle/projects|list|status|plan|zip`,
  `GET /bundle-sw.js`, `POST /bundle/export|verify|import|cancel|finish|delete`, `POST /time` (the page's clock, sent when
  it opens), `GET /usb/status` (`pi/engine/bundleweb.h`). Every POST passes the strict same-origin check. The bundle job
  runs in core 0's loop (`bundle_pi::poll`, one 64 KB piece a pass, only while the card queue is idle; writes wait while
  the Rytm plays or a song take records, everything waits while the recorder records); `/status` has
  `"bundle":{phase,done,total,waiting,steps,held,maxRead,maxWrite,maxUs}`. A cut import is finished or undone at boot
  (`bundle::recover` in `engine_begin`). `MaxContent` is 192 KB (the raw page is ~130 KB).
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
- Credentials: the password goes only in the POST body; the server wipes its copies (the form buffer, the decoded value) after use; it is never logged, echoed or in `/status`. The *passphrase* is never stored either — what `/.rytm/wifi.txt` keeps is the derived 64-hex key. That file is a credential at rest: until 0.3.0 `GET /fs/get?path=/.rytm/wifi.txt` served it to anyone on the network, which is now fixed twice over ("Pairing code" below: the gate, and `/fs/*` reaching content only). It is still readable by whoever holds the card — see "Threat model" there. SSIDs are not logged either: the scan line gives a count, and the log tee (`CLogTee`, serial and the page's log) blanks the name in any `SSID '...'`/`ssid="..."` line (wpa_supplicant's own). `/cmd` refuses `wifi=` commands from a URL (only a POST body) and logs `wifi=join:...` redacted as before.
- CSRF: join, forget and power are accepted only from the page itself (`wifi_conf.cpp` `same_origin`): the Host header must be one of the Pi's names (`<hostname>.local`, its IP; this also stops DNS rebinding — but only on the endpoints that call `FromPage`, which before 0.3.0 did not include `/fs/*`) and the Origin header (else the Referer) must be `http://<that host>`; a request with neither is refused. `/cmd` refuses a request whose Origin/Referer is another site's (tools without those headers still work). The headers come from `pi/circle/circle-http-origin.patch` (Circle's HTTP daemon keeps Host, Origin, Referer; applied by `build.sh`). On top of that, since 0.3.0 the Wi-Fi POSTs also need the **pairing code** (the next section): the same-origin check stops another site's page, the pairing code stops the person on the network. **Remaining risk:** the transport is plain HTTP on a LAN, so the Wi-Fi password crosses it once in the clear when it is sent, and anyone who can watch the network can take a paired client's session token as it goes past and use it. The gate is a lock on the door, not encryption: see "Pairing code" > "Threat model".
- Tests: `make -C pi/engine/tests` (`test_wifi_conf`: the checks, wifi.txt round trip, the atomic save and restore with failing writes and renames, recovery, the trial marker, same-origin, SSID redaction, the scan list); `node --test pi/engine/tests/page_wifi.test.mjs` (the page's check, and the flow against `mock_web.py`: the refusals, a scan, a wrong password failing and reverting, a good join, forget; nothing echoes the password or logs the SSID). `mock_web.py` serves fake networks; the password `wrong-pass` fails, an SSID not in the scan is not found, `MOCK_TRIAL=` sets the trial's seconds (default 6).
- Not yet on hardware: the scan and a join (the user's network).

### Pairing code (2026-10-25)

Before 0.3.0 everything the Pi served was open to anyone on the LAN. The only check was the same-origin one above, which stops a hostile web page but not a person on the network: `GET /fs/get?path=/.rytm/wifi.txt` returned the Wi-Fi key, `GET /fs/ls?path=/` the whole card, and TCP 7310 **replaces the firmware** with no credential at all (confirmed against a running unit, 2026-10-25). The pairing code closes that.

**The scheme.** The device makes an **8-digit code** at every boot (`paircode::Gate::init`: one digit per random byte from the SoC's hardware generator mixed with the timer, bytes of 250 and above drawn again so every code is equally likely). It is shown on the **Rytm's screen** (a host-drawn popup, `SIDECAR PAIR CODE` + `1234 5678`, 30 s; always on the SIDECAR page's PAIR line, UP + T10, and in SIDECAR MENU > WEB UI) and on the **touchscreen's STATUS page** (the LINKS panel's title line). It is in no log line, no `/status`, no `/log` and no answer of any endpoint.

**It does not persist, and does not need to.** A new code is drawn at every boot and shown on the screens; nothing stores it. A device that has paired once does not need it again — it resumes with its own secret — so there is nothing to keep. The one exception is a unit with no display at all: see "No display".

A client enters the code once, in the body of `POST /pair`. It gets a **session token** (128 bits, this run only) and, if it asked to be remembered, a **client id** (32 bits, not a secret) and a **client secret** (128 bits), of which the card keeps only SHA-256. Every later request carries the token in the **`X-Sidecar-Auth` header** — a header another site's page cannot set, and one a browser does not keep in its history. After a reboot the client trades its id and secret for a new token (`POST /pair/resume`), so the owner does not retype the code.

Over the wire: the code once, the secret once per run, the token on every request that is not in the open set below.

#### The code on the Rytm's screen

A **small modal in the middle of the 128x64 panel**, drawn over whatever is showing — a Sidecar page, the connect splash, the SRC view. A status popup defers to a page that is open, which is right for a status popup and wrong for the one credential needed to use the device at all; `remote_popup`'s new `over` flag is that distinction. `caps36` and `legacy` still refuse: an older mod genuinely cannot draw it, so it fails, says so in the log, and points at the touchscreen and `cmdline.txt paircode=<8 digits>`.

**It overlays, it does not replace.** The page is drawn into `work` as usual and the box goes on top, so the diff sends only the box's tiles. Coming back uses the mechanism the remote UI already has for a page change — `view.invalidate()`, and the page redraws itself from the model — so no page state lives in pixels and none can be lost. The Rytm draws each text item itself, in a box it clears, so any of the page's items under the modal are blanked while it is up and the page's own redraw puts them back. `test_ui` asserts the restored frame is **byte for byte** the one from before, slots included, and writes `build/rui_pages/pair_modal_{0_page,1_over,2_back}.png` to be looked at.

Geometry: an 80x32 box at (24,16) on the 128x64 panel, double-bordered, filled so the page does not show through, two centred lines in `kRSmall` — "SIDECAR PAIR CODE" and "1234 5678", nine characters each, inside 76 px of usable width. Text slots 62/63, above anything a page uses.

**It does not disturb playback.** It draws in the remote UI's existing tick on the control core, in the same place a page frame is already drawn: no new work on an audio core, no allocation, and about 40 tiles over MIDI when it appears and again when it goes — the cost of one page change.

**When it appears** (these three are defaults chosen here, not the user's requirement, and are meant to be easy to change):

| trigger | |
|---|---|
| boot | once, with the new code |
| the Rytm connects | each new hello (a cable, a Rytm restart) while no device is paired yet, at most once a minute (`paircode::HelloShow`) |
| `pair=show` | the explicit route, from the console |
| the page's dialog opens for real | `POST /pair/show`, sent by the page once it knows it is unpaired (no saved pairing, or a stale one), and by its **Show it on the Rytm** button — whoever asks, **at most once every 10 s** (`paircode::ShowGap`, fixed). The answer says `{"shown":true}` or the `wait` left, and the button says "Shown on the Rytm" or "Wait N s" |

**Not on a page load (fixed 2026-10-29).** It used to be `GET /`, but a page load never carries the token (it is a header the page's own `fetch` sets), so every reload of a *paired* owner's tab flashed the code over the Rytm. The page now asks for it itself, after `/pair/status` and a resume have shown that it really is unpaired; a session lost to a reboot resumes first and shows nothing.

**Kept until the Rytm is there (fixed 2026-10-29).** The boot's request used to be given up after ten refused tries, and the Rytm's first hello comes well after the engine's boot, so the modal never appeared; and a hello arriving while it was owed or up dropped it. Now a refusal before the Rytm's first hello is "no Rytm yet" and is not counted (`bridge::rytm_greeted`), and a hello keeps an `over` popup when the new link can draw it (`test_ui`: "survives a hello").

The trigger is deliberately **not** `/pair/status`: the dialog polls that every 5 s while unpaired, so it would redraw the modal over the Rytm for as long as anyone sat on the pairing screen. `GET /` happens once per load. It is rate-limited because `/` is open to anyone on the network: without that, someone could put a box over the Rytm's screen at will while the user is playing. Nothing leaks either way — the code is already permanently on the touchscreen and neither screen is visible from the network — so this is a disruption limit, not a disclosure one. A burst of reloads collapses into one modal (`Show()` counts requests, not modals).

**The gap is a fixed 10 s (2026-10-29; review L1 accepted).** It used to start at 30 s and double up to 10 min, refusing silently, so on a Sidecar with no touchscreen — whose only screen is the Rytm's — the button stopped working after a few presses. Anyone on the network can now put the box up every 10 s. That is accepted: it only shows the code to whoever stands at the Rytm, which is physical access (outside the threat model), and any key on the Rytm takes it off.

**When it goes:** after **30 s**, at a key on the Rytm (with the page closed the Rytm's own splash rule; over an open page the key is eaten), or at once when any device pairs. The case is someone reading eight digits off a small screen and typing them into a browser on another device — too short is as bad as not showing it. **Fixed 2026-10-29:** it used to go after about a second — the Rytm ends a `36` splash after 1 s without a host message and the popup sent none, and its 12 s asked for 244 ticks in a 7-bit byte (116 went). It now sends a `3C` every 0.5 s and the `36` again (at most 127 ticks) before the last one runs out (`test_ui`: "held 30 s"); a key or a turn on the Rytm ends it there (the Rytm's splash rule) and, as the Rytm reports it, here too, so no `36` brings it back. It never traps the screen: the code is valid for the whole boot and `pair=show` brings it back. Pairing clears it so it does not linger once the job is done.

Not verified here: whether the `36` frame wakes a Rytm whose screen has gone to sleep. The modal pre-empts the Sidecar's own connect splash, but the Rytm's own sleep is its side of the link.

#### What is open

`web.cpp`'s `Open()` is an **allowlist**: anything not named in it needs the code, so an endpoint added later is gated unless someone deliberately opens it. (It was a gate*list* first, which is how `/fs/get` ended up in neither list.)

| open | why |
|---|---|
| `/`, `/index.html` | the page itself, so someone can load `sidecar.local` and see the dialog. It needs no other file: the CSS, the JS and the wordmark font are all built into the page. |
| `/bundle-sw.js` | the download's service worker. The browser fetches it on its own, without the page's header; it is a built-in static script with nothing of the device in it. |
| `/health` | alive, the build and commit, uptime, temperature, `streaming`, `rytmMidi`, `display`, the supply's flags and warning, underruns, and whether the gate is on — enough to tell whether a unit is working, and nothing of what is on it. It is a function of its own (`CKernel::WebHealth`), not a filtered `/status`, so a field added to `/status` cannot appear in it by accident. |
| `/status` | open by the user's decision (2026-10-26: "open as specified" — seeing whether a device works on a shared network matters more than hiding its settings). It carries names and settings (host, address, machines, the project's state) but **no credential**: no pairing code or token and no Wi-Fi key (checked against a live answer). Anything secret added to `WebStatus` would be readable by anyone on the network. |
| `/log` | deliberate, and unredacted. It is this project's whole diagnostic basis — the MIDI-gate verification, the touch-failure timeline and the underrun hunt all came out of it — and a tester must be able to send a URL. Its steady state is counters; name-bearing lines (`project "..."`, `/Samples/...`, recording names) roll out of the 64 KB ring in about 25 s. This is a judged trade, not an oversight. |
| `POST /pair`, `/pair/status`, `/pair/resume`, `POST /pair/show` | pairing has to work before you are paired. `/pair` is rate-limited (below); `/pair/resume` takes a 128-bit secret and is open because without it "remembered across reboots" cannot work at all; `/pair/show` puts the code on the Rytm's screen (never in the answer: `shown` and the `wait` left), at most once every 10 s. |

**Everything else is gated**, including all of `/fs/*` (reads included), `/bundle/*`, `/wifi/*`, `/um`, `/lists`, `/pool`, `/time`, `/usb/*`, `/cmd` and TCP 7310. A refusal is HTTP 200 with `401 not paired …` in the body (Circle's error pages carry no message) plus an `X-Sidecar-Pair: required` header the page watches for; the body is JSON or `"<code> <message>"` to match what that endpoint's client reads (`WantsText`).

#### What the file API can reach

Separately from the gate, and underneath it: **`/fs/*` is content, TCP 7310 is firmware, `/.rytm/` is neither and is reachable through neither.**

`fsweb::reachable()` decides whether a path exists as far as `/fs/*` is concerned, for **every** operation — `ls`, `get`, `put`, `del`, `mkdir` and both ends of a `mv` — and denies by default. The content folders are `/Samples`, `/Plugins`, `/Recordings`, `/Bundles`, `/MULTISAMPLE`, `/WAVETABLES`, `/GRANULAR`, `/PRESETS`, `/MACHINES`, `/NAM`, `/CHOP`, `/JUNO` and `/DX7` — the set the engine and the page actually use (docs/LIBRARY.md, docs/POOLS.md, docs/BUNDLES.md, docs/CHOP.md, docs/plugins.md). The card's root lists those folders and nothing else. Anything outside them answers `404 no such path`.

This is worth having even with the gate in front of it, because it holds against an **authenticated** attacker — a tester who paired, or anyone with a stolen token — and because it closes two specific holes: `/fs/get` served `/.rytm/wifi.txt` (the Wi-Fi key) and, on a headless unit, `/sidecar-paircode.txt`; and `read_only()`'s "below the root: only overlays/ is the firmware's" meant a **write** could land on `/.rytm/pair.txt`, where putting a chosen device hash is a way past the pairing code itself. `sdboot.py` on 7310 is where `cmdline.txt` and `engine.img` are legitimately written, and it is authenticated. `/bundle/verify` already confined its `path=` to `/Bundles/*.sidecar.zip` or a stick's `SIDECAR/` folder (`bundle_path_ok`, `stick_path_ok`), so it needed no change.

`/fs/*` also gained the same-origin layer it never had: reads strict (`same_origin`; the page sends a `Referer` on its own GETs, a DNS-rebound page's `Host` is not ours), the four writes lenient (`not_cross_site`, so a tool that sends neither header still works). A tool reading through `/fs/get` must now send `-H "Origin: http://sidecar.local"` as well as the token.

#### The page while it is unpaired

The Sidecar refuses everything but the open set, so the page's pollers would otherwise do nothing but make work on core 0 — which the audio shares — for as long as the tab is open. Measured on the device before this was fixed: `1992 not paired` in 24 minutes on a tab sitting on the dialog, about 2.5 requests a second, indefinitely, including a unit left on overnight.

While unpaired the page sends **only `/pair/status`, every 5 s** (0.2 requests a second, measured against the mock). `/status`, `/wifi/status`, `/um`, `/lists`, `/pool`, `/bundle/status` and `POST /time` are not sent at all; the log poller keeps its place but fetches nothing, so the view catches up the moment the code is entered. The 5 s tick is what lets the page notice a reboot (a new code), the gate being turned off, or this browser being paired from elsewhere. Everything resumes on a successful pair, and `/time` is sent then.

The download's service worker does not retry either: a refusal is HTTP 200 with the code in the body, so without a check it would take the refusal's text for zip bytes. It looks for `X-Sidecar-Pair` and fails with a message instead.

#### Two addresses, one browser

A browser keeps `localStorage` **per origin**, and `http://sidecar.local` and `http://192.168.1.5` are different origins. A device paired at one is an unpaired browser at the other, the code has to be entered again with no explanation, and `/.rytm/pair.txt` ends up with two entries for one browser. Nothing can be done about that from inside the page — the other origin's storage is invisible to it — so the dialog says it instead: when the Sidecar remembers devices and this browser is not one of them, it names both addresses and offers the other one. `/pair/status` carries the mDNS name for that (`host`), which mDNS already broadcasts to the whole network.

If `sidecar.local` does not resolve in a browser but the IP works, that is the browser or the OS resolver, not the gate: the page itself is in the open set and never goes near the `Host` check, so a gate problem cannot look like "the site does not load". `curl http://sidecar.local/health` from a shell settles which it is in one command.

#### When nothing stays paired

`/pair/status` reports `"hdr"`: whether **that request** carried `X-Sidecar-Auth`. The page always sends it once it holds a token, so

```
curl -s "http://sidecar.local/pair/status" -H "X-Sidecar-Auth: $TOKEN" | grep -o '"hdr":[a-z]*'
```

answering `"hdr":false` means the header is not reaching `GetContent` at all — a kernel built without `pi/circle/circle-http-auth.patch`. That looks exactly like "the code is accepted and then nothing stays paired", because it is: `POST /pair` carries the code in its body and succeeds, and every request with the token it returns is refused. `web.cpp` calls `GetRequestAuth()`, so a tree without the patch does not build; a tree where `lib/net` was not rebuilt after it does.

#### Sessions, remembered devices, revoking

At most 16 sessions (the least recently used goes). Port 7310 takes none: there the connection itself is what is authenticated, and before 2026-10-29 every `sdboot.py` connection's `AUTH` made one that only pushed a browser's out. A session ends after **an hour with no gated request** and after **a day** whatever happens. An hour is long enough that nobody is interrupted mid-session; the day is a hard ceiling. A browser notices neither, because it resumes silently with its stored secret.

Remembered devices live in `/.rytm/pair.txt`, written exactly as `/.rytm/wifi.txt` is (`pair.txt.part` written, the current file kept as `pair.txt.prev`, then the rename; a failure leaves the file as it was). One line per device: `c=<8 hex id> <64 hex SHA-256 of the secret> <name>`. **What identifies a device is its secret, not its address** — an IP or a MAC is spoofable, changes with DHCP and differs between Wi-Fi and Ethernet. **Sixteen are kept** (eight until 2026-10-29): a studio has a handful of browsers, each counted twice if it is used at both `sidecar.local` and the IP, plus `sdboot.py`. When the table is full, the **least recently used browser** goes (with its sessions) rather than a dead end — **never `sdboot.py`'s record while a browser's is there to go instead**: it is used once in a while, for an install, and is how firmware is recovered, so the old oldest-first rule (and plain LRU) always picked it. "Used" is this run's last pair or resume; after a reboot the file's order (oldest first) decides. The file's size is checked at compile time (`kFileMax`), so a longer header can never make a save fail silently.

**Revoking:** the page's Status tab > Access has *Forget this device* and *Forget every device* (the second also ends `sdboot.py`'s access); the console's `pair=revoke` does the same; and deleting `/.rytm/pair.txt` from the card does it offline.

#### A saved pairing that has gone stale

A browser and `sdboot.py` both retry their saved credential on every start, so what happens when it is no longer good decides whether an ordinary user can lock themselves out. Two rules:

- **Only `pair()` counts towards the lockout.** The lockout exists to stop someone guessing eight digits, and `resume()` takes a 128-bit secret, which is not guessable — rate-limiting it protects nothing. Counting it meant a browser holding a stale entry locked its own owner out by being reloaded five times, and because the limiter is shared with port 7310 that also blocked `sdboot.py install`, the way firmware is recovered. A device must not be able to lock its owner out through its own saved state.
- **A failed resume is `kNoDevice`, not `kBadInput`.** "The pairing code is 8 digits" is a complaint about something the user typed; a stale credential is not that, and saying it sent a user hunting for a typo in a code they had entered correctly. The page clears its store and says the saved pairing is no longer valid.

A lockout does not block `resume()` either, and a successful one clears **the lockout of the address it came from** — that one only. Before 2026-10-29 it reset everyone's, so the owner's browsers resuming on every reload kept an attacker's lockouts at one minute instead of sixteen. It never refills the global backstop either.

The page checks the shape of what it stores, on write and on read (`pairId`, `pairSecret`: 8 and 32 hex). A junk entry — `undefined` written as a string when a field was missing, or anything left by an older build — is dropped locally instead of being sent. Both halves are tested: `test_paircode`'s `test_stale_device` for the rules, `page_net.test.mjs` for the store, and `page_pair.test.mjs` end to end (21 stale resumes leave pairing unlocked and the owner can still pair).

#### Rate limiting

It is a short code, so online guessing is the realistic attack. The compare is constant time (`paircode::ct_equal`, no early exit). Since 2026-10-29 the limiter is **per peer** — the client's IPv4 address, the same key on the web page and on TCP 7310 — with a **global backstop**:

- **Per peer:** at most **one attempt a second**, and **five wrong ones lock that address** for 1 minute, then 2, 4, 8 and 16 on each further run of five; a correct code from it clears both. Someone else on a shared Wi-Fi guessing locks **themselves** out, not the owner — before, one limiter for everyone let anyone on the network keep pairing (and `sdboot.py install`) locked for good. The table holds 16 addresses; a new one takes the stalest that is not locked out, and **when all 16 are locked out a new address is refused** rather than evicting one (which would start it afresh and let a 17th address walk past the limit).
- **Global backstop:** 30 wrong codes for everyone together, one back every 10 s (6 a minute). When it is empty every address waits, the owner's too, for at most 10 s at a time. An attacker's own addresses are locked while it refills, so keeping it empty for long takes about **20 addresses guessing at once**; keeping it busy bounds guessing at 6 a minute, about 8 600 a day, so a code (which changes at every boot anyway) has about a 1 in 11 600 chance a day against an attacker who can sustain that. `/status`'s `"pair":{"locked":N}` is the backstop's wait.
- **Not counted at all:** a malformed code (not 8 digits — a bare `POST /pair`, a `GET /pair`, a 7-digit typo: it cannot match, so counting it protects nothing), any resume, and the right code.

Every refused attempt is still counted for the log, including the ones the limiter turns away before any compare, and at most **one log line every 10 s** reports how many came (`take_report`) — otherwise an unauthenticated flood would push the log's tail out, and `/log` is open for the attacker to watch it work. A lockout does **not** lengthen on attempts it refuses outright, because that would let anyone keep an address locked for good.

Devices that are already paired keep working throughout, and a remembered device resumes whatever the limiter says. **What an attacker on the network can do is delay a *first* pairing** (a new browser), for as long as they keep guessing from enough addresses; they cannot touch a paired browser, `sdboot.py` once it is paired, or the serial console. Pair the devices you need before you go somewhere you do not trust. A physical "allow pairing now" button on the touchscreen / Rytm is for 0.3.1.

#### The random generator

The code, every session token and every client secret come from the SoC's hardware generator (mixed with the timer). It is **tested before it is trusted** (2026-10-29, BACKLOG F4): at boot `paircode_pi::Init` draws sixteen raw words and `Gate::init` sixteen mixed ones; if any two are alike the generator is stuck and the gate **refuses everything** for that boot — `POST /pair`, `/pair/resume` and 7310's `PAIR`/`AUTH` answer that the random number generator failed its self-test, the touchscreen's PAIR row and the Rytm say `RNG FAILED`, `/health` says `"pair":"rng-failed"`, `/pair/status` `"rng":false`, the MIDI bridge will not turn on, and the log says it in capitals. A loud refusal rather than a guessable code. The serial console (and `:sdf`) still works; a restart tries again. The test catches a dead or stuck generator, not a subtly biased one — nothing this cheap can.

#### Port 7310

A line protocol, so it has its own step: `PAIR <code>` answers `OK <client-id> <secret>` (and remembers the client), `AUTH <id> <secret>` answers `OK`. Until one succeeds the connection answers only `ERR unauthenticated: …` and `BYE`, with one log line a connection so hammering it cannot fill the log.

`PAIR` and `AUTH` are limited per address like the page (above), and make no session. The port serves **one connection at a time**, so an idle one used to hold it — and every install — for good: a connection now gets 10 s of silence and 20 s in all to send `PAIR`/`AUTH`, then 60 s of silence once it has. `pi/circle/sdboot.py` does this: it reads `SIDECAR_PAIR_CODE` (or asks on the terminal, and fails with a clear message when there is no terminal), keeps what it is given in `~/.config/sidecar/pair.json` (0600, one entry per host, `SIDECAR_PAIR_FILE` moves it), and `AUTH`s from then on. The first install on a Pi:

```
SIDECAR_PAIR_CODE=12345678 uv run --with pyserial python pi/circle/sdboot.py install
```

**An engine from before 0.3.0 has no gate at all**, and upgrading one has to work: the client asks it to list the card first, and if that is answered instead of refused it goes ahead unauthenticated with one line saying so. The decision is made on the engine's own answer, never on a version string, so the only way to skip authentication is for the engine to demonstrably not ask for it. Without this, every upgrade from a pre-0.3.0 kernel stopped with "run it again with SIDECAR_PAIR_CODE=…" for a code that device does not have (found on hardware, 2026-10-25).

**The serial `:sdf` path is not gated**: whoever has the serial header has the board, and it is the way back in when the network side is locked out.

#### First pairing, for the owner (0.3.0: the gate is on by default)

A card made by `pi/circle/make-sd*.sh` has no `paircode=` in `cmdline.txt`, so the gate is **on** with a new random code at every boot. Read the code off the **touchscreen** (STATUS page, the LINKS panel's PAIR line) or the **Rytm**: a box in the middle of its screen at boot and when it connects, for 30 s (again whenever an unpaired page asks, or on `pair=show`). **With no touchscreen** the Rytm is the only screen, and the code is there on demand: **SIDECAR MENU > WEB UI** (UP + YES, then YES on SIDECAR, WEB UI) shows it, and the SIDECAR page (UP + T10) has it on its PAIR line. Then, once per device:

- **The web page:** open `http://sidecar.local/`, type the 8 digits, keep *remember this device* ticked, **Pair**. That browser resumes by itself from then on, across reboots. **Show it on the Rytm** in the dialog puts the code back on the Rytm's screen. Pairing at `http://sidecar.local` does not carry over to `http://<the IP>` (two sites to a browser): pair at the one you use.
- **`sdboot.py`** (installs, `ls`, `cmdline` …): the first run once with the code, `SIDECAR_PAIR_CODE=12345678 uv run --with pyserial python pi/circle/sdboot.py install`; it keeps a client id and secret in `~/.config/sidecar/pair.json` (0600) and never needs the code again for that Pi. A deploy script needs nothing else: export `SIDECAR_PAIR_CODE` for its first run against a freshly paired card, or run `sdboot.py ls` once by hand.
- **A script that uses HTTP** (`curl … /cmd`): pair once with `curl -s -d code=12345678 -d remember=1 -d name=deploy http://sidecar.local/pair` (it answers a `token`, a `client` and a `secret`: keep the last two), and at each later run trade them for a token with `curl -s -d client=<id> --data-urlencode secret@$HOME/.config/sidecar/deploy.secret http://sidecar.local/pair/resume` — the secret **from a file** (0600), not on the command line, where it lands in the shell's history and in `ps`. Send the token as the `X-Sidecar-Auth` header (`curl -H "X-Sidecar-Auth: $TOK" -d c=notes=1 http://sidecar.local/cmd`), or as `tok=` in a POST's body. **Never in the URL**: `?tok=` is refused since 2026-10-29 (BACKLOG F7) — a URL lands in history, proxy logs and `Referer` headers. A body `tok=` is not a CSRF hole: the token is not ambient (no cookie), so another site would have to know it.
- **The touchscreen** needs nothing: it is the device.

**SIDECAR MENU > WEB UI** (on the Rytm): **WEB UI** ON / OFF — the web page and port 7310 from the network (OFF: every page and endpoint answers "the web UI is off on the Rytm (SIDECAR MENU > WEB UI)", port 7310 refuses every line as unauthenticated with that text; the serial `:sdf` still works), kept in `/.rytm/pair.txt` as `web=0` (default on; deleting the file turns it on again; pair.txt missing after a cut between its save's renames, `pair.txt.prev` is read and written back). With the gate off (`paircode=0`) or the RNG failed, pair.txt is never read or written, so WEB UI OFF holds for that boot only; **CODE**; **PAIRED DEVICES**; **NEW CODE** (a fresh random code for this boot; the paired devices stay paired; not on a fixed `paircode=<8 digits>`); **FORGET DEVICES** (YES twice: every device and session, as `pair=revoke`). The gate itself is not offered there: `cmdline.txt paircode=0` stays the dev card's bypass. The Rytm MIDI bridge's port is not part of WEB UI.

A development card keeps `paircode=0` (or a fixed `paircode=<8 digits>`) in its `cmdline.txt` until its owner removes it: `sdboot.py cmdline` shows the line, `sdboot.py cmdline "<the line without paircode=0>"` writes it.

#### For 0.3.1

- **F8:** the client secret sits in the browser's `localStorage` for good, with no rotation and no list of paired devices on the page (only the count, *Forget this device* and *Forget every device*).
- **TLS** (or a challenge-response that never sends a bearer): see "Threat model".
- **The physical "allow pairing now" button** (review M1): the touchscreen or the Rytm opens a 2-minute window in which the global backstop is skipped (per-address limits still apply) and the code is drawn again.
- **L2:** `/status`'s `"pair":{"locked":N}` and `/pair/status`'s `wait` tell an attacker when the backstop refills: a free timing hint. Minor; drop or coarsen it.
- **L4:** `Ms()` is `GetUptime () * 1000` as a `u32`, which wraps after 49.7 days. The limiter's differences survive it; `Client.used`'s ordering does not (after a wrap the most recently used browser can be evicted first). Seconds, or 64 bits.
- **F5, judged not worth a change:** `/pair/status` answers `paired` for a token at any rate, outside the limiter. It is the same oracle every gated endpoint already is (a 401 or not), against a 128-bit token, so limiting this one endpoint would protect nothing; it does not keep a session alive (`check_quiet`).

#### No display

If you are locked out, this is the paragraph you want.

With a touchscreen the code is on the STATUS page; with a Rytm it is on its screen. A unit with **neither** would have nowhere to show it, so when no panel came up the engine writes this boot's code to **`/sidecar-paircode.txt` at the card's root** (Finder-visible, like `sidecar-wifi.txt`), rewritten at every boot and deleted again as soon as a unit boots with a display. `/fs/*` cannot reach the card's root at all and every read needs the code, so it is not readable over the network; it is readable by whoever holds the card, which is the deliberate limit of this scheme (see "Threat model").

Two things are said out loud rather than done quietly. If `cmdline.txt` asked for a display (`display=1`, the default) and none came up, the line is a warning, because the owner thinks that unit has a screen. And if the Rytm will not take the popup — `core::ui::remote_popup` drops one when the Rytm's hello had no `36`, on a legacy build, while its Sidecar page is open or during a splash — the engine retries for ten seconds and then says so, with what to do next; before, the request was consumed before the drop and the code was silently never shown.

**A unit that should never write its code down takes a fixed one instead: `cmdline.txt paircode=<8 digits>`.** That is the unattended path, and it is loud in its own right (below).

#### The development bypass

`cmdline.txt paircode=0` turns the gate off entirely. It is loud in four places, so a dev card can never be mistaken for a secured one: a `LogError` at boot ("THE PAIRING CODE IS OFF … anyone on this network can install firmware …"), a warning line **once a minute** for as long as it runs, `"pair":"off"` in `/health` and `"pair":{"gate":"off"}` in `/status`, and `pair code OFF (cmdline)` in the alert colour on the STATUS page's LINKS panel. `paircode=<8 digits>` keeps the gate but fixes the code; that is loud the same way (`fixed`, `PAIR 1234 5678 FIXED` in the alert colour). Anything else in `paircode=` is ignored and the gate stays on with a random code — garbage never weakens it.

#### Threat model

**Card credentials are protected against the network, not against physical possession.** That is a deliberate scope decision. The asset being defended is a unit on a shared or untrusted network; someone holding the card is out of scope.

It is also what this hardware can do. There is no secure element and no passphrase entered at boot, so any key the firmware used to decrypt the card would have to live on the card or inside the firmware image — both readable by whoever holds the card — and the Wi-Fi PSK must stay in usable form or the Pi cannot rejoin a network by itself. **Encrypting credentials at rest here is not achievable; do not attempt it.** `/.rytm/wifi.txt` therefore stays as it is, holding the derived 64-hex key (never the passphrase).

What the gate does **not** do: the transport is plain HTTP and a raw TCP socket on a LAN. Anyone who can watch the traffic can take a session token, or the secret once a run, or the code the one time it is typed, and use it. There is no TLS, no replay protection and no binding of a token to an address — `crypto.subtle` is unavailable in a non-secure context, so a challenge-response that never sends a bearer would mean hand-rolling SHA-256 in the page. The gate stops the person on the network who has not seen the screen. Treat the network as the outer lock.

#### A page left open across an install

The page is built into the kernel (`hosted/web_page.h`), so **an open tab keeps running the page from the kernel it was loaded from**. After an install it still shows the old UI until it is reloaded, which cost real time once: a validation message that had been reworded looked like a bug in the new build. It is not the service worker — that one intercepts `/bundle-dl/` only and caches nothing — it is simply a tab that was never reloaded. The pairing dialog prints the build and commit that is answering (`/health`, open) for exactly this reason: if it does not match what you installed, reload the page.

#### Code and tests

`pi/engine/paircode.{h,cpp}` (portable: the code, the compare, the limiter, sessions, the card's file, the 7310 lines), `pi/engine/paircode_pi.{h,cpp}` (the card, the hardware RNG, the boot messages, `/status`, the STATUS row, the console's `pair*`), the perimeter and `/pair*` in `web.cpp`, `/health` in `kernel.cpp`, the confinement in `fsweb.cpp`, the 7310 step in `sdfiles.cpp`, `pi/circle/circle-http-auth.patch` (Circle's HTTP daemon keeps `X-Sidecar-Auth`; applied by `build.sh` after `circle-http-origin.patch`), the dialog and the Access section in `www/index.html`.

- `make -C pi/engine/tests` (`test_paircode`): the code's shape and spread, the constant-time compare, use before init refusing rather than falling open, the limiter and its doubling lockout, the 10 s report throttle, sessions and both expiries, `check_quiet` not keeping a session alive, the card's file and its atomic save with failing writes and renames, a full file of the longest names, a device surviving a reboot, revocation, nine devices, the `cmdline.txt` parsing, the 7310 `PAIR`/`AUTH` lines, and that no answer, no error text and no file but the recourse one ever holds the code.
- `make -C pi/engine/tests` (`test_fsweb`): every denied path refused for every operation — `/.rytm/wifi.txt`, `/.rytm/pair.txt`, `/cmdline.txt`, `/config.txt`, `/engine.img`, `/overlays`, and near-misses like `/Samples2` — both ends of a `mv`, the content folders still working, and the root listing showing content only.
- `node --test pi/engine/tests/page_pair.test.mjs`: the open set read out of `web.cpp` and checked against `mock_web.py`; every route **derived from the sources** (web.cpp's own comparisons and bundleweb.h's endpoint list) that is not open proved to refuse an unpaired client, so a handler in neither list fails the test; the confinement holding for a **paired** client; a wrong code refused then locked; the token in the header and as `tok=` in a POST's body, and refused in the query string; a remembered device surviving a restart; revocation; `paircode=0` both ways; and the code in no answer and no log line.
- `python3 pi/engine/tests/test_sdboot.py`: `sdboot.py` against stand-in engines on the SD file port — a pre-0.3.0 one (no gate: it works, says so once, and never asks for a code), a gated one (pairs once with `SIDECAR_PAIR_CODE`, then `AUTH`s from what it stored, 0600), a gated one with no code and no terminal (it says the *device* is asking and where to read the code), a wrong code, and a stale stored secret falling back to the code.
- `make -C core test` (`test_ui`): the modal over an open Sidecar page — it is on screen, it is a small box and not the whole frame, nothing of the page's text is left under it, the page comes back byte for byte when it times out, it can be taken off early (a device paired), and a legacy Rytm still refuses it. It writes the three PNGs named above.
- `node --test pi/engine/tests/page_net.test.mjs`: the token rides in `X-Sidecar-Auth` on this page's own requests only, a refusal reopens the dialog — and a refusal of a request sent *before* a pairing cannot undo it (the generation on `PAIR.tok`). That last one is the bug the user hit: "it confirms it is correct and saved, but the dialog stays on top of the page".
- Verified in a browser against the mock with `localStorage` cleared: the page loads from `/` alone, shows the dialog, says "waiting for the pairing code", `/log` and `/health` answer, everything else is refused, and typing the code brings the whole page up.
- **Not yet checked on hardware** (no Pi here): the Rytm popup actually showing the code, the STATUS page on the real panel, the hardware RNG, and a real `sdboot.py install` over 7310.

### Rytm MIDI bridge (2026-10-27, 0.3.0)

The Rytm's USB MIDI on **TCP port 7330**, for hardware testing: Iced-Cream's tools (analogrun, analogusb.py, elkflash,
elkbench) talk to a real Rytm through the Sidecar exactly as they talk to analog-emu's `--usb-midi-tcp`. How to use it:
`docs/HARDWARE-TESTING.md`. The code: `pi/engine/midibridge.h` (portable: the parser and queue, the copy, the gate and
its challenge, the blocks, the pace, the status JSON), `pi/engine/usbmidi_merge.h` (portable: the Poll that merges
everything into the Rytm's USB MIDI OUT; `CObUsbTransport::Poll` is its Circle side), `pi/engine/midibridge_pi.{h,cpp}`
(the port, its two tasks, `/status`, the page's endpoints, the console), the copy in `kernel.cpp`'s `MIDIPacketHandler`,
the Status tab's **Rytm MIDI bridge** section in `www/index.html`, and the Mac's relay `tools/rytm_bridge.py`.

**The stream.** Raw MIDI bytes both ways, no framing. Everything the Rytm sends is copied to the client, and the Sidecar
keeps getting all of it. A newer authenticated client replaces the older one on the Pi; the Mac relay lets one tool at a
time through (below).

**The merge (client to Rytm, `usbmidi_merge.h`).** The client's bytes are parsed into whole messages (running status
expanded, realtime bytes as messages of their own, a SysEx only once its F7 is in, at most 38912 bytes - the largest RPC
message, 37457, framed) and queued (256 KB; a full queue stops the reading, TCP's own flow control). Each Poll sends the
keys, the computer's MIDI and the engine's records first, then the bridge's, **paced** by a token bucket at 250 KB/s
(below the Rytm's ~300 KB/s, so a Poll does not sit in a blocking bulk transfer) and 512 bytes a Poll at most. One USB
MIDI cable means a half-sent SysEx holds everything else back: the bridge's (so a key waits 156 ms at most, the largest
SysEx at 250 KB/s - measured 149 ms in the test), the computer's, and the engine's (a song dump's 3069-byte pieces; the
keys and the computer's now wait for those too - before 0.3.0 they could land between the pieces - and an engine SysEx
whose next piece does not come within 250 Polls lets them go again, counted). **Realtime never waits:** the computer's
clock and transport (F8..FF from the DAW) go out as CIN F packets inside any held SysEx, in the Poll they arrive (USB
MIDI 1.0 allows that on one cable); only its other messages wait, in order - and once something waits only F8 and
FE still overtake it, so start / continue / stop keep their place behind a Song Position Pointer. **A failed send**
(Circle's bulk transfer fails after 3 s) - the bridge's, the engine's, the keys' or the computer's - ends that Poll and
turns all sending off for 5 s (doubling to 60 s while the retries fail too) or until a new umidi1, drops the bridge's
queue including a half-sent SysEx and the computer's deferred messages (counted): one 3 s stall per retry, never
one per chunk or per Poll, never the rest of a SysEx with a hole in it, and a Rytm that was only busy gets the Sidecar's own link back
by itself. The bridge reaches the Rytm with `link=usb` only.

**The copy (Rytm to client).** Non-blocking: the USB MIDI interrupt stages each message and publishes it to the relay
task only once whole (256 KB queue); the relay sends with a 2 ms limit. A client that stops reading loses whole messages
(`drops`), never part of one. Realtime bytes inside a SysEx are sent after its F7.

**Security.** The stream carries SysEx, which includes OS upgrades and +Drive writes, so:

- **Off at every boot**, and only with a pairing code: with `paircode=0` (cmdline.txt) it refuses to turn on at all,
  since anyone could then read the token from the page. On for this boot from the web page (Status > Rytm MIDI bridge:
  `POST /midibridge/set`, paired and behind the strict same-origin check) or the serial console (`midibridge=on`).
  `/cmd` refuses `midibridge=` (its origin check is the lenient one).
- **A challenge, not a password on the wire.** The Pi sends `SIDECAR-MIDIBRIDGE 1 <nonce>` (128 bits, new for each
  connection) and the client must answer `BRIDGE <HMAC-SHA256(token, nonce)>` within 1 s. The token is 128 bits from
  the hardware RNG, **new at every turn-on**, and shown only on the web page (`GET /midibridge`, paired, same-origin) -
  never in the log, `/status` or the console. A sniffed exchange cannot be replayed; someone who also sniffs the
  page's `GET /midibridge` (plain HTTP) has the token, and an active attacker on the network can take over the TCP
  stream after the handshake (there is no channel binding) - the pairing code's own limit. **Use it on a trusted
  network only, and turn it off when the test is done**: each turn-on makes a new token.
- **Lockouts per address, never the bridge off.** Five wrong answers from one address lock that address out for 10
  minutes (16 addresses tracked; a lock is not evicted by fresh ones). Nothing on the network can turn the bridge off. A
  first line that is not a `BRIDGE` line - an HTTP request a web page made to `sidecar.local:7330` - is refused and not
  counted, so a page in the tester's own browser cannot lock the tester's address out. The leftover bytes after a hello
  stay with that connection until it is accepted.
- **The pairing reset closes it.** *Forget every device* and `pair=revoke` turn it off, the switches off, and forget
  the lockouts. The client's address is logged at every connect and close; it is in `GET /midibridge` (paired) but not
  in the open `/status`.
- **Blocked by default, each with its own switch** (off at every boot and with the bridge, needing the bridge on;
  re-checked when a queued message would start, so a switch turned off drops what was queued under it):
  - **OS upgrades**: the MIDI RPC's OsUpgradeStart / Write / End (0x50..0x52) and the OS `.syx` image's own messages
    (`F0 00 20 3C <dev> 00 7E|7F`, as `Analog-Rytm_MKII_OS1.74.syx`). Switch: *allow OS upgrades* / `midibridge=os:on`.
  - **+Drive writes, as an allowlist**: every RPC command but the confirmed reads needs the switch, so an unknown id
    fails closed. The reads (elektroid's `src/connectors/elektron.c`): ping 01, version 02, UID 03, screenshot 04,
    storage info 05, query 09; the sample file system's read dir 10, file info 22 23, reader 30-32; the raw file
    system's read dir 14, file info 26, reader 33-35; the data store's list 53 and reader 54-56. The writes this
    blocks include the sample FS (11 12 20 21 40-42), the raw FS (15 16 24 25 43-45), the RAM slots (18 19) and the
    data store (57-5D: write, move, copy, clear, swap). Switch: *allow +Drive writes* / `midibridge=data:on`.
    **analogrun's data writes (`data put`, `roundtrip`, `clear`) need it on.**
  - The RPC command is decoded whatever the device byte, except the Rytm's own dumps and requests (device 07, id
    0x50-0x6F) and analoghealth's channel (device 7D; the stock OS drops it, which suggests the RPC parser checks the
    device byte).
  - Not filtered: kit / pattern / sound dumps (a test plan may overwrite the working kit), and the factory protocol's
    `#UPGRADE` text commands (unknown transport, `rytm-mods/re/subsystems/os-upgrade.md`).

**`/status`** has `"midibridge":{"on","osUpgrade","dataWrites","state":"off|waiting|connected","port","connects",
"replaced","refused","lockedAddrs","toRytm","toRytmMsgs","fromRytm","drops","dropBytes","blocked","blockedData",
"tooLong","junk","flushed","queued","rytmFail","rateBps"}` - no token, no address. The console's `midibridge` logs the same.

**Tests.** `make -C pi/engine/tests` (`test_midibridge`): **the real Poll** (`usbmidi_merge.h`) with a fake endpoint -
the order with keys, the computer's SysEx and the engine's song-dump pieces every Poll and no packet inside anyone's
SysEx; the pace over a second (at most 250 KB + the bucket); the largest SysEx holding a key 149 ms; a send failing in
the middle of a SysEx (one failed send, the queue and the half SysEx dropped, nothing more sent and the others dropped
until a new umidi1, then whole messages again); an engine failure; no Rytm; the computer's SysEx holding everyone; an
engine piece whose next one never comes; the retry after a failed send (5 s, then 10 s while it keeps failing); the
computer's clock during the largest bridge SysEx and an engine song dump, every F8 out in the Poll it arrived. A Rytm
that is enumerated but not reading: one send a Poll whatever is queued, including the retries, and the deferred
messages dropped and counted; a Song Position Pointer then Continue during a hold keep their order. And the parser (F9/FD, an over-long SysEx cut by a status, an OS start split at
every byte boundary, a budget under 3 while open), back-pressure, the blocks (any device byte, every write and read id,
the real `.syx` bytes, kit dumps whose 12th byte looks like a command, analoghealth's channel, a switch turned off with
messages queued, and every RPC id 00-7F and its reply id through the allowlist), HMAC-SHA256 against RFC 4231 and against Python's for the relay's answer, the hello line and its
leftover, the gate (replay with a new nonce, the per-address lockout and its expiry, non-BRIDGE lines not counted, a new
token at each enable), the status JSON (no token and no address in `/status`), and the copy. `tools/tests/test_rytm_bridge.py`
(run by the same make): the relay against a fake Sidecar speaking the challenge - raw bytes both ways, an
HTTP POST refused before it reaches the Sidecar, a silent client closed after 1 s without ever taking the slot (and,
with the opt-in `--passive`, a tool that reads first let through, its later text still refused), a wrong
token, one tool at a time and eight racing, the token from a 0600 file or the environment only, `::1`. `node --test
pi/engine/tests/page_pair.test.mjs`: `/midibridge` and `/midibridge/set` refused to an unpaired client, and their web.cpp
branch on the strict same-origin check with the switches from the body only. **Not yet checked on hardware.**

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

## Temperature warning (2026-10-25)

The SoC's temperature is read once a second (the stats line's reading: one mailbox call; no other code calls it, and
`/status`, the STATUS page and the warning all use that reading, so a VideoCore stall is paid once a second at most).
`pi/engine/thermal.h` turns it into one warning state:

- **On at 80 C, off below 75 C** (75 / 70 until 2026-10-29, then briefly 78 / 73; the user: the unit does not feel hot
  and the Pi has its own throttle). 79 stays warned, 75 stays warned, 74 clears; a unit sitting at 80 does not flicker
  (tests: `pi/engine/tests/test_thermal.cpp`). A reading of 0 (the mailbox failed) changes nothing.
- **The clock Circle lowered.** Above `socmaxtemp` (78 on the card, Circle's maximum) `CCPUThrottle` drops the ARM clock to
  600 MHz and raises it 3 C below, so by 80 C the clock has usually been lowered already. `thermal::clock_low_mhz` (the
  clock read back well below the setting, the SoC at 75 C or more; a lower Settings clock is not "lowered") makes it
  honest: warned, the banner says `SoC 80 °C - clock lowered to 600 MHz.` (OLED `SOC 80C - CLOCK LOWERED TO 600 MHZ`), the
  Rytm's popup `SOC 80C CLOCK LOWERED / TO 600 MHZ (UP+T10)`, the STATUS page's CPU title line `600 MHz: clock lowered
  (hot)` in the alert colour (OLED `CLOCK LOWERED`) and `/status` `"clockLow":600` (the web page's Temperature tile).
  Lowered under the warning (78-79 C): the same STATUS line and tile, quietly - no banner, no popup.
- **Where it shows.** The STATUS page's CPU panel (the temperature, right of the FIFO row, in the alert colour with
  HOT); a banner over the touchscreen's title bar (the same one as the supply's warning, which wins when both are on);
  `/status` `"tempWarn"` (and `"clockLow"`) and the web page's Temperature tile; the Rytm's screen once (below).
- **The log.** One line per crossing, not per sample: `thermal: SoC 81 C, warning on (from 80 C, off below 75 C),
  warning N this boot, ARM clock M MHz` (one mailbox call for the clock, at the crossing only) and `thermal: SoC 74 C,
  warning off (peak 81 C)`.
- **The Rytm's popup: once per crossing, not once per boot.** A unit that cooled and heated again is news again; once per
  boot would stay silent for the rest of the evening after the first crossing. The popup (`SIDECAR RUNNING HOT / SEE STATUS
  (UP+T10)`, or the clock-lowered text above, through `core::ui::remote_popup`) is offered every second until the Rytm's remote UI has shown it (its count
  of shown popups moves), and dropped when the warning clears first. It **does not override** a page the user is looking at
  (unlike the pairing code): the banner and STATUS say it already, nothing is lost by waiting, and a popup that wipes a page
  mid-edit for a condition that is not an emergency (the firmware throttles at 80 C, 85 C is its hard limit) is the wrong
  trade. It appears when the page closes, if the warning still stands.

## CPU clock (2026-10-25)

Settings > System > CPU clock: **1500** (the default: the board's full clock, 1500 MHz on the Pi 4; a Pi 5 keeps its own
maximum), **1200** or **1000 MHz**. It is also a console command (`cpumhz=1200`; `cpumhz` alone prints the state), stored as
`cpumhz=` in `/.rytm/display.txt` (not written while it is the default), and `cpumhz=` in cmdline.txt wins. It is applied
at boot before the boot bench, so the bench measures the clock the engine plays at.

- **Mechanism: runtime, not config.txt's `arm_freq`.** `arm_freq` needs a reboot, lives in a boot file the file API
  deliberately cannot write, and could not be a Settings choice; set in config.txt it would also move Circle's idea of its
  maximum (what `CCPUThrottle` calls "maximum" is the firmware's maximum clock, believed to follow `arm_freq`; not checked on
  the unit), which is the one real advantage: nothing fights. The runtime way takes a tap. Circle's `CCPUThrottle` only knows its minimum (600 MHz) and maximum, so a lower
  clock is set through the mailbox (`SET_CLOCK_RATE`, one call, and one to read it back, on core 0 when the setting changes:
  a tap during playback can be heard once). While the clock is held below the maximum the kernel does **not** call
  `CCPUThrottle::Update`: it would put the maximum back whenever the SoC is cool (its restore step). Back at 1500, the
  maximum is set through `CCPUThrottle::SetSpeed` and `Update` runs again. What `Update` does at 1500 (it lowers to the
  minimum above `socmaxtemp`, 60 C unless cmdline.txt says otherwise, and back below 3 degrees under it) is unchanged;
  in a capped mode it is not there, the firmware's own limits (soft 80 C, hard 85 C; `get_throttled` bits in `/status`
  `power`) are what remain, and the lower clock is the thermal lever. Check on the unit that `Update` is not already
  dropping the clock in normal use (the `thermal:` line at a crossing logs the ARM clock).
- **The voice budget follows the clock.** `guard: voice budget` is not derived from the bench: it is `voicebudget=` percent of
  a 12-frame block (250 us), 50 % = 125 us by default (25 % = 62.5 us until 2026-10-26: a Teensy-era figure), the same figure at any clock, against a worst bench block of 2-4 %
  at 1500 MHz. A render takes proportionally longer at a lower clock, so the same 125 us would mean 1500 / 1000 = a third
  less work and a voice would be muted for work it was allowed before. The budget is scaled by 1500 / clock
  (`cpuclock.h` `budget_ticks`): 125 us at 1500, 156 us (62.5 %) at 1200, 187 us (75 %) at 1000, at most a whole block, and
  never tighter than the configured share on a board faster than 1500. It is raised before the clock drops and lowered after
  it rises. The log line is unchanged at 1500 (`test-on-pi.sh` expects it); otherwise it says `voicebudget 50 % scaled for
  1000 MHz`. `/status` has `"cpu":{"setting","mhz","budgetUs"}`, STATUS shows the clock on the CPU panel's title.
- **The boot bench** runs after the clock is applied, so its lines (`at N MHz`, voices a core at 80 %) are for that clock.
  A change at run time logs `bench rescaled: voices a core at 80 % (1500 -> 1000 MHz, linear, not re-measured): plaits 109->72 ...`:
  linear in the clock, an estimate (the memory-bound parts scale less); the bench is not re-run, it loads every core.
- **The case fan** (the GeeekPi case) is controlled only by the case board: on / off in its own display menu, not by the
  Pi, and not through `gpiofanpin=` (that Circle option would make `CCPUThrottle` drive a GPIO fan instead of the clock). From
  software a lower clock is the only thermal lever; at 1000 MHz the temperature banner says to check the case fan.

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

(Since 2026-10-29 the bench's last row is `md`, the built-in MD in DIGIDRUM's place; not measured on a Pi yet. The table
below is the 2026-10-15 run's, `dd` included, kept as measured.)

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
