# usb=mirror: the box itself on the USB-C side

Current state (2026-11-06): run on a Pi 4 and a Pi 5 with an Analog Rytm MKII on OS 1.74, the review changes included,
with Windows (Pi 4) and macOS (Pi 4 and Pi 5); see Tested below.

**Overbridge passthrough: Pi 4 since 0.4.1; Pi 5 since 0.4.2.** The Pi 5 kernel is built with it again (0.4.1's
was not). A Pi 5 that uses its USB-C for data gets about 1.5 A from a computer or hub port: on a powered hub port (about 1.5 A) the 0.4.2 test unit ran without under-voltage, while an earlier setup on a hub port was power-starved and reset when Overbridge started, and a direct Apple Silicon port did not enumerate it; for full power it
needs a USB-C power + data splitter with USB-PD pass-through. See "The Pi 5" below.

`usb=mirror` on the card's cmdline.txt (with `gadget=sidecar`, the default) replaces the Sidecar's own USB-C device
(1209:0004, UAC2 12x12 + MIDI) with the attached box as the computer would see it plugged in directly. The aim is that
the box maker's own software (the Overbridge engine and plugins, the transfer tool) works through the Sidecar, while the
Pi keeps its own Overbridge stream with the box and adds to it.

The mode presents the box maker's USB IDs to the computer. It stays opt-in and is never on in a public card or build
default. The box's own descriptors are read from it and relayed at run time; none are stored in the repo or the build.

## How it works

- **Which device the computer sees.** The Sidecar device (1209:0004) at boot and whenever no box is there. The box once
  it has been read. The Sidecar device again after the box has been gone for `mirrorhold=` seconds (default 10, up to 600),
  so a box that reboots, as after an OS update, comes back as itself. Each switch is a replug: the gadget leaves the bus
  (`SoftDisconnect`), waits 300 ms, and comes back with the other descriptors and endpoints (`SoftConnect`). A box with
  other descriptors while one is shown switches the same way.
- **Descriptors.** When the box is configured on the USB-A side (`ob_usb.cpp`), its device and configuration descriptors,
  plus every string descriptor they name, are read into RAM (`MirrorAttach`). They are served unchanged. None of the box's bytes are in the repo: the unit test's descriptor is made up (`tests/test_mirror_desc.cpp`).
- **Endpoints** (`mirror_desc.h`). Each endpoint address the configuration names gets one gadget endpoint, sized to the
  largest `wMaxPacketSize` of any alternate setting. Packets of a smaller alternate are short packets, so nothing is
  resized at SET_INTERFACE. Roles:
  - vendor-class `0x83` / `0x03`: the audio stream;
  - every other vendor-class endpoint: a pipe (the async channel);
  - MIDI streaming: the MIDI port.
- **Audio IN.** Each IN block of the box goes to the computer as it came, cut to the channels of the computer's alternate (`ob::block_bytes`).
- **Audio OUT.** The computer's OUT blocks are decoded into the same OUT ring the UAC2 device feeds. `ToRytm` adds them to what the engine sends. The box stays the clock.
- **Alternate settings.** The computer's SET_INTERFACE is kept on the gadget side and not forwarded. The box stays at the alternate the engine opened (alt 3), because the Pi's own stream needs it.
- **Pipes.** Each packet is relayed to the same endpoint on the box. An OUT packet is NAKed to the computer until the box
  has taken it. The box's IN request is renewed only once the computer has the last packet.
- **MIDI.** Same path and filters as the default mode (`midi_filter.h`): the host link stays between the Pi and the box,
  and `usbmidi=os:` still decides whether OS upgrades pass. See "What the filters cover" below.
- **Class and vendor requests.** Each one is forwarded to the box (`CObUsb::MirrorControl`). EP0 NAKs until the box
  answers, then returns the answer, or a STALL if the box stalls or is gone. One request is in flight at a time.
- **The box going away.** Its URBs in flight never complete, so when it goes (`MirrorBoxGone`) a pipe OUT packet the
  computer sent is dropped and the endpoint takes the next one, and a request forwarded to the box is STALLed. A box
  that comes back inside `mirrorhold` (a reboot after an OS update) starts from a clean state. A completion that arrives
  for an earlier attach is ignored (each attach has its own generation).
- **A halted pipe.** A failed transfer halts the endpoint on the xHC and on the box. `CObUsb::MirrorPipePoll` (core 0's
  loop) sends the box CLEAR_FEATURE(ENDPOINT_HALT), resets the host endpoint, and resubmits an IN pipe: every 100 ms for
  the first five tries, then once a second. The mirror line counts these (`reset`). Circle reports a refused Reset
  Endpoint as success, so the endpoint's state is read after it: still halted (or in error), the pipe stays stopped and
  is tried again after the backoff (`halted: the host refused its reset`), and `cleared, running again` is logged only
  once a reset took.
- **Interfaces of the audio stream.** `MirrorAttach` never sends SET_INTERFACE to them, so the Pi's own stream stays at
  alt 3. A pipe on such an interface is relayed only if it is at alt 3.
- **Overbridge's per-block bytes.** Bytes 4-31 of each OUT block (byte 1 = the first non-zero one's index, or 0xFF)
  carry data from the Engine, such as its latency-check marker; the box answers the marker with a pulse on IN. A
  computer block with any of them set has them copied into the Pi's own OUT block a fixed distance away
  (`mirrormargin=` blocks, 8, up to 256, past where the Pi's OUT stood at the stream's first block), so the Engine
  measures the same latency on every try. The mirror line counts them (`extra`, `lost`, `late`, `anchor`; see the
  counters below).
- **The name Overbridge shows.** For an Elektron box (vendor 1935), the answer to vendor request 1 (IN, the 32-byte device
  name Overbridge shows in its Devices tab and plugin picker) gets a suffix, " (SC)" by default, so the unit reads
  "Analog Rytm MKII (SC)". A name the computer sets (OUT request 1) reaches the box without the suffix. Overbridge
  keys the unit on its PID and the unit ID of request 0, which pass unchanged, so plugins stay paired with the unit
  whether it is direct or through the Sidecar [I, read from Overbridge 2.26.10]. `mirrorname=off` turns it off;
  `mirrorname=TEXT` sets the suffix (an underscore for a space, a space put in front). The USB product string is left
  as the box's, since the computer names the MIDI and audio ports from it.

## What the filters cover

`usbmidi=os:` (`usbmidi_os.h`) filters MIDI only. In mirror mode two more paths reach the box, and both are relayed
unfiltered:

- the class and vendor requests on EP0 (Overbridge's name, unit ID, version and stream requests among them);
- the async channel (the vendor-class pipes).

So `usbmidi=os:off` does not stop a tool that updates the box over a vendor request or a pipe. The OS updates tested
through the mirror (the transfer tool and a SysEx flasher) went over USB MIDI: the mirror line's MIDI counters carried
the whole update to the box [hw]. Whenever `usb=mirror` is on, whatever `usbmidi=os:` is, the engine logs a warning at
start that the vendor requests and the pipes are relayed unfiltered.

## Circle change

`pi/circle/circle-gadget-mirror.patch` (applied by `pi/engine/build.sh` after `circle-gadget-reset.patch`; it defines
`CIRCLE_GADGET_MIRROR`) makes two changes, `circle-gadget-softconnect.patch` adds a third and `circle-gadget-stray.patch` a fourth:

1. **IN and OUT endpoints can share a number** (`0x83` + `0x03`). Circle kept one endpoint object per number. An OUT endpoint now goes to a second table when an IN endpoint already has its number. A gadget without shared numbers (the default Sidecar) never uses that table.
2. **A class or vendor request can be answered later.** The new hook is `OnControlRequest() -> ControlDeferred`, completed by `CompleteControl()`, with `OnControlAborted()` called when the host moves on. The default returns `ControlNotHandled`, so other gadgets take the old path unchanged.
3. **A replug without a cable.** `SoftDisconnect()` sets the controller's soft-disconnect bit and deactivates the endpoints. `SoftConnect()` sets the core up again, has the gadget remake its endpoints from its current descriptors, and clears the bit.
4. **A SETUP while a request waits no longer crashes** (`circle-gadget-stray.patch`, both boards). While a deferred request waits for the box, nothing is armed on EP0. A SETUP the host sends then (it gave up on the request) is still taken by the core: in buffer DMA mode it is written just past the last OUT transfer, and SETUP done is raised. Circle's `FinishTransfer()` asserted on it: the Pi 4 reset at a live switch to OVERBRIDGE when the Overbridge app started (2026-11-06, caught on serial: `dwusbgadgetendpoint.cpp(550)`). Now the SETUP is read back from where the core wrote it and handled as any: it ends the deferred request (`abort`, and `(setup N)` in the mirror line). An answer from the box is not armed while such a SETUP waits for its interrupt. A completion for an endpoint with no transfer armed is counted (`stray`) and ignored. Nothing else of Circle's EP0 or endpoint flow changes. A first version (0.4.1 rc5) reworked EP0 and the endpoint disable more widely and broke the Sidecar's own audio, MIDI and the mirror's streams on the Pi 4; it is gone. `pi/engine/tests/test_gadget_ep0.cpp` runs the patched Circle code on a model of the core.

## The setting: USB-C mode

Like the Rytm's own USB CONFIG, the Sidecar's USB-C port is one thing at a time, picked on the Sidecar. OVERBRIDGE is
opt-in, never a default:

| mode | word | the computer sees |
|---|---|---|
| CLASS COMPLIANT (default) | `class` | the Sidecar device: UAC2 audio 12 in / 12 out and USB MIDI (`docs/usb-audio-midi.md`) |
| OVERBRIDGE | `overbridge` | the attached box itself, passed through (this page), the Sidecar device while no box is read |

MIDI only and off are still cmdline.txt's `gadget=midi` / `gadget=0` (another gadget, chosen at boot): with either the
setting has nothing to apply to, and the three places below say so ("not here"). They would be further modes of the same
setting (`pi/engine/usb_mode.h`: its `Mode`, words and `modes()`); not done yet.

**Where it is set.** All three send the console's `usbmode=` (`POST /cmd` with the pairing code's `X-Sidecar-Auth`, as
every setting's write; `usbmode=overbridge` only with the strict same-origin check, as `POST /midibridge/set`, since it
widens what reaches the box; `class` and `cmdline` keep `/cmd`'s usual check):

- the web page, Sound tab, **USB-C mode: Class compliant / Overbridge**, with what USB-C is now beside it (the Sidecar
  device, the Rytm itself, replugging; "(cmdline.txt usb=)" when the mode comes from there). Overbridge asks first
  (a confirm with the warning below); the warning stays under the row while it is set;
- the touchscreen, Settings > **Sound**, USB-C MODE (CLASS / OVERBRIDGE), with what USB-C is now and OVERBRIDGE's short
  warning under it, before and after it is picked;
- the Rytm, **SIDECAR MENU > USB-C** (YES on the SIDECAR page, UP + YES, then the USB-C row): UP / DOWN or knob A pick CLASS
  COMPLIANT or OVERBRIDGE, YES sets CLASS COMPLIANT, OVERBRIDGE takes YES twice (as FORGET DEVICES on WEB UI; NO keeps),
  NO goes back. The page shows which one is SET, NOW (SIDECAR DEVICE, THE BOX, REPLUG...), and OVERBRIDGE's short
  warning whenever it is under the cursor or set. It is drawn by the Pi (rytm-mods 0026's remote UI): no Rytm update.

A mode this unit cannot run is greyed (web, touchscreen) or says NOT IN BUILD (Rytm): OVERBRIDGE without the mirror's
Circle patches (`CIRCLE_GADGET_MIRROR`: the Pi 4 build and, since 0.4.2, the Pi 5 build have them; the Pi 5
build of 0.4.1 did not, its `/status` `usbMode.modes` was `["class"]`), both without the Sidecar gadget. The console
refuses it too, with "not here (no circle-gadget-mirror.patch)" (also logged at boot for `usb=mirror` or a stored
`usbmode=overbridge`: "not in this build", CLASS COMPLIANT runs).

**Stored, and which wins.** The choice is kept on the card as `usbmode=class|overbridge` in `/.rytm/display.txt` (written
only once one is chosen). Where the mode comes from, first match wins:

1. `usbmode=` in `/.rytm/display.txt` (chosen on the Sidecar);
2. cmdline.txt's `usb=` (`mirror`: OVERBRIDGE, anything else: CLASS COMPLIANT), as before the setting;
3. CLASS COMPLIANT.

So a card with `usb=mirror` in cmdline.txt and nothing stored stays OVERBRIDGE, and a choice made on the Sidecar wins over
cmdline.txt (unlike the display's keys, where cmdline wins: the Sidecar's own control has to work on a card someone set
up by hand). `usbmode=cmdline` on the console drops the stored choice, so cmdline.txt (or the default) holds again;
deleting the line from display.txt does the same at the next boot. `/status` says which: `"usbMode":{"set":"class|overbridge",
"from":"card|cmdline|default","runs":...,"modes":["class","overbridge"],"shown":"sidecar|box|switching|none","boxUp":bool,"board":4|5}`
(`runs`: what runs, CLASS COMPLIANT where the unit cannot run the one set; `modes`: what it can run, `[]` without the
Sidecar gadget; `shown`: what the computer sees now; `boxUp`: the box has been read and is up; `board`: 4 or 5; both boards show OVERBRIDGE the same way). The console's `usbmode`
alone logs the same.

**Applied at once.** A change switches USB-C live, with no restart; the switch itself runs at one point of the engine's
main loop, whichever place asked for it. To CLASS COMPLIANT while the box is shown: the replug the mirror makes when the
box goes (below), USB-C leaves the bus (`SoftDisconnect`), waits 300 ms and comes back as the Sidecar device; the box's
async pipes stay open: while the computer sees the Sidecar device, the box's IN packets are drained (renewed at once,
counted in the mirror line's `parked`, not relayed) and no halt is cleared on EP0; once the box is shown they are relayed
as before. To OVERBRIDGE: a box read before is shown again; a box that is up and not read yet is read now
(`CObUsb::MirrorWanted` -> `MirrorAttachLate` -> `MirrorAttach`: its strings on EP0, its pipe endpoints made on the xHC,
a SET_INTERFACE for a pipe interface at alt > 0) **while the Pi's own Overbridge stream with the box runs**. That is new:
until the setting, the box was always read inside its Configure, before the stream started. It is not yet run on
hardware. Then USB-C replugs as the box. The computer sees an unplug and a plug-in, so its audio and MIDI apps lose the
device for that moment: change it with the computer's DAW stopped, and **never during a transfer or an OS update**
through the mirror (the replug cuts it).

**Older builds.** A build from before the setting ignores `usbmode=` (the line is kept), so after a downgrade cmdline
`usb=` decides again, and the stored choice comes back after the next upgrade.

**Tested** (2026-11-06): unit tests only (`pi/engine/tests/test_usb_mode.cpp`, `test_mirror_desc.cpp`'s pipe gate,
`core/tests/test_ui.cpp`'s SIDECAR MENU > USB-C, `pi/engine/tests/page_seg.test.mjs` and `page_pair.test.mjs`, the
touchscreen's `pi/ui/host/shot.c`) and an object-only build for both boards. Not yet on hardware: the switch both ways
while streaming, the read of a box at run time, CLASS COMPLIANT after an OVERBRIDGE session.

**The warning** (short on the touchscreen and the Rytm, in full on the web page and here): in OVERBRIDGE the box's
vendor requests and Overbridge's async channel reach the box unfiltered, so an OS update from the computer (the transfer
tool, Overbridge) can reach the box. `usbmidi=os:` filters MIDI only ("What the filters cover" above); the engine also
logs it when the setting turns OVERBRIDGE on with `usbmidi=os:` not off.

## Known limits

- **Box gone, inside mirrorhold.** The box is still shown and its requests are STALLed until the switch.
- **A switch mid-update.** An OS update whose box takes longer than `mirrorhold` to come back sees the Sidecar device meanwhile.
- **IRQs during a switch.** SoftConnect's core reset and setup (some 125 ms on the Pi 4, 150 ms on the Pi 5: a 100 ms wait after the soft reset) run at task level with the gadget's interrupts off; the Overbridge stream on USB-A keeps running [I].
- **No forwarding of the computer's alt or interface choices.** Alternate settings stay on the gadget side, so a vendor request that only works at another box alternate is not handled. Requests that change the box's stream state also reach the box and may disturb the Pi's own stream.
- **One control request at a time.** A second one while the first is in flight is STALLed.
- **Which box.** The one the Pi's Overbridge driver claims. A box whose profile streams only after its Sidecar mod's hello (the Analog Four MKII) is not claimed without it, so a stock one is not mirrored.

## The Pi 5

**Since 0.4.2** (0.4.1 left it out). The Pi 5's USB-C is also its power input. A Pi 5 that uses its USB-C
for data gets about 1.5 A from a computer or hub port: on a powered hub port (about 1.5 A) the 0.4.2 test unit ran without under-voltage, while an earlier setup on a hub port was power-starved and reset when Overbridge started, and a direct Apple Silicon port did not enumerate it. Full power and data together need a USB-C power + data
splitter with USB-PD pass-through. `pi/engine/build.sh` puts the mirror patches on the Pi 5's tree
(`circle-engine-pi5usb`) as on the Pi 4: `CIRCLE_GADGET_MIRROR` is defined there, OVERBRIDGE can be picked on the web
page, the touchscreen and the Rytm, and `usb=mirror` / `usbmode=overbridge` run it. A tree built by 0.4.1 (without
them) gets its patched files back clean and the whole list again at the next build (the patch list's checksum,
`.pi5-gadget-sum`, changed).

The Pi 5's USB-C gadget is the same DWC2 core as the Pi 4's (same core version, 7 IN + 7 OUT endpoints besides EP0,
dedicated FIFOs, 4080 FIFO words) and the same Circle driver with `circle-pi5-gadget.patch` on it, so
`circle-gadget-mirror.patch`, `circle-gadget-softconnect.patch` and `circle-gadget-stray.patch` go on the Pi 5's tree
unchanged, after `circle-gadget-reset.patch` as on the Pi 4; `tests/test_pi5_flags.sh` checks the order, that they apply
on top of the Pi 5 stack, define `CIRCLE_GADGET_MIRROR` for `RASPPI=5` and that the gadget sources compile. What differs:

- **FIFOs.** The Pi 5 sizes them as Linux does: RX 558 words, TX 512 words for IN endpoints 1-5 and 256 (1024 bytes)
  for 6-7, against 384 each on the Pi 4. A mirrored endpoint is at most 512 bytes (`CMirrorEP`), so it fits either way.
- **SoftConnect.** The core comes back through the Pi 5's own setup (`InitCorePi5`, `usbinit=`, default 4: a soft
  reset, the PHY fields, DCTL as Linux), as at boot: some 150 ms at task level with the gadget's interrupts off (the
  Pi 4: some 125 ms). It connects before EP0 is made, as at boot; the host's reset waits until the interrupts are on.
- **The box's side.** The Rytm is on the RP1's xHCI on a root port, not behind the Pi 4's VL805 hub: the same Circle
  xHCI driver, so the pipes, the forwarded requests and the halt recovery are the same code.
- **USB-C is also the power input**: the computer reaches it through a powered hub or dock (docs/pi5.md "USB device
  side"); each switch is a replug on that hub's port. A computer or hub port gives a Pi 5 that uses USB-C for data
  about 1.5 A; full power needs a USB-C power+data splitter with USB-PD pass-through.

## Tested

On a Pi 4 with an Analog Rytm MKII on OS 1.74, the computer on Windows [hw]:

- the default mode is unchanged (the Sidecar device, 12 in / 12 out plus MIDI, no underruns);
- the computer sees 1935:0B48 with the box maker's driver;
- Overbridge passes its latency check and the plugin syncs both ways; over 30 minutes no per-block bytes were lost or
  late, with no control stalls;
- per-track audio IN, and OUT to the Rytm;
- unplugging brings the Sidecar device back after `mirrorhold`; replugging brings the Rytm back in about 0.6 s, its name
  with " (SC)" and the plugin still paired;
- the transfer tool browses, elkflash `info` works through the mirrored MIDI port, and OS updates went through with
  `mirrorhold=60`, from the transfer tool and from a SysEx flasher.

On macOS with Overbridge Engine 2.26.10, 2026-11-06 [hw], on a Pi 4 and on a Pi 5 (a test build with the mirror patches; not
in 0.4.1): Overbridge through the mirror, and the Rytm's USB-A cable pulled for a short and for a long time;
`lost 0`, `late 0` on both.

The 0.4.2 build on a Pi 5, macOS, the USB-C on a powered hub port (about 1.5 A, no under-voltage) [hw]: 4 of 4 live
CLASS COMPLIANT <-> OVERBRIDGE switches with the Overbridge Analog Rytm app: 12 channels, the async channel streaming,
no reset, 0 underruns; the Mac's log had no "Audio callbacks have ceased" and no kIOReturnError.

The new offset after a replug, on the Pi 4 [hw]: a replug with the plugin streaming gave `lost 1126`, `late 128` before
it, and 0 and 0 after it.

In the emulator (rytm-emu over TCP): the default mode unchanged, and with `usb=mirror` and no box the Sidecar device
stays up.

The review changes of 2026-11-05 on the same unit [hw]:

- with the plugin streaming and the transfer tool browsing, the Rytm unplugged and replugged about 2 s later: the async
  IN pipe halted and was cleared and resumed, a request that came while the box was gone was STALLed, the computer never
  saw the Sidecar device, and Overbridge and the transfer tool carried on;
- unplugged past `mirrorhold`: every request STALLed while the box was away, the Sidecar device back after 60 s, the
  Rytm back with " (SC)" on replug;
- an OS update (stock 1.74) with elkflash over the mirrored MIDI port: 0 resends, the box back 0.3 s after its reboot
  dropped it, inside `mirrorhold`.

The Pi log's `mirror` line counts every path. The counters most often asked about:

- `in ... drop`: the box's IN blocks dropped because the queue to the computer (64 blocks, about 9 ms) was full, so the
  computer took fewer blocks than the box sent for that long. Counted in blocks of 7 frames.
- `out ... over` and `under`: the computer's OUT audio when the ring the Pi adds to its own stream (512 frames) was full,
  or empty when the Pi needed a frame. Counted in frames, not blocks.
- `ctl ... stall`: requests the computer was answered with a STALL. That covers a request while the box is gone, the box
  going away with a request at it, a second request while one is in flight, and the box's own STALLs.
- `pipe ... reset`: CLEAR_FEATURE(ENDPOINT_HALT) sent to the box for a halted pipe, every try counted. A box that is
  unplugged and plugged back in shows a few.
- `out ... extra`, `lost`, `late`, `anchor`: Overbridge's per-block bytes. `extra` counts the computer's blocks that
  carried any. `lost`: dropped because the queue toward the box (128 entries) was full. `late`: dropped because the
  Pi's block they were due in had already gone (the offset was too small). `anchor`: how often the offset was set
  again after the first time, because the computer's stream fell behind the Pi's or the Pi's counter went back. A
  steady stream shows `lost 0`, `late 0` and `anchor` not climbing (it does climb while the box is away inside
  `mirrorhold` and the computer keeps streaming).
- `midi ... filtered N/M (last …)`: SysEx messages (N) and USB MIDI packets (M) the filters dropped toward the box, and
  the head of the last message dropped, in hex (up to 13 bytes, `-` when none yet). The head tells which rule took it:
  `f0 f7` is the empty SysEx, and `f0 00 20 3c …` with `usbmidi=os:off` an OS update held back.
