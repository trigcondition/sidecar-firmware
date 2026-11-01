# Device profiles: porting the Sidecar to another box

The Sidecar was built for the Analog Rytm MKII. This layer gives every assumption about the box it serves one home, a
`device::Profile`, and lets the box pick its profile at a hello. It is a place to plug a port into, not official support
for other boxes. The Rytm is the default: it never says hello here and nothing changes for it.

The code: `core/device/device.h` (start there), `core/device/device.cpp`, the test `core/tests/test_device.cpp` with a
minimal second box, and a worked example: `core/device/digitakt.*` here and the mod in the separate `sidecar-digitakt` repository.

## How it works

```
your firmware mod --- USB MIDI SysEx ---> host_sysex (core/host_link.cpp)
                                            |  a hello 4F picks the profile
                                            |  your messages become the engine's 04 / 03 / 11
                                            v
                                          the voices, P1..P16, render
```

- In: from an accepted hello on, only your trig, params and note-off messages get in. Anything else from your box is
  dropped, so it cannot reach a Rytm-only path.
- Out: what the engine sends goes through your profile's `out_allow`, which by default lets nothing through. A box with
  a small CPU is not loaded with the Rytm's pages, pool tables and label lists.
- Back: the Rytm's own hello (0x35) returns to the Rytm, unless your profile has `kRemoteUi`.

## What a device must send

All bytes after `F0` are 7-bit. `<value>` is 0..127, `<fraction>` is in 1/128 steps.

**1. Hello**, always with the Sidecar's identity, when your USB port comes up and every few seconds after that:

```
F0 7D 52 54 4F 01 <id hi> <id lo> 01 [name bytes] F7
```

`id` is 14 bits: `1` the Rytm, `2` the Digitakt mk1, `0x3F00` and up for private experiments. Ask for a number when a
port is public. The Sidecar answers once per hello:

```
F0 7D 52 54 4F 01 <id hi> <id lo> <status> F7       status: 1 accepted, 0 no such profile, 2 another protocol,
                                                            3 the Sidecar restarts into your state folder: hello again
                                                            4 a Rytm has the Sidecar now: go on saying hello
```

Reading the answer is optional. A repeated hello changes nothing and costs the Sidecar one short message, and it finds
your box again when the Sidecar boots after it. A hello that changes the profile silences every voice once.

**2. Trig**, in the block it plays, with the values it plays with:

```
F0 <head 3 bytes> <cmd_trig> <track> <note> <velocity> { <value> <fraction> } x params F7
```

**3. Params**, when they change:

```
F0 <head 3 bytes> <cmd_params> <track> { <value> <fraction> } x params F7
```

**4. Note-off** (optional; for a machine that holds):

```
F0 <head 3 bytes> <cmd_note_off> <track> F7
```

Keep `head` as `7D 52 54` unless you have a reason. The Pi's MIDI pass-through keeps exactly that head away from the
computer's port, and it tells the Pi's note adapter that a mod is reporting. A message of the wrong length, or with a
track at or past `voices`, is dropped and counted (`device::dropped()`).

## How to pace them

This is what keeps the box's UI alive. The Rytm side has run these rules since before 0.2.0, and they are in the worked
example:

1. **Latest value wins.** One pending snapshot per track, overwritten by every change. No queue of changes: values that
   were never sent are dropped on purpose.
2. **Cap the rate.** At most one params message per track every 16 audio blocks of 32 frames (10.7 ms, about 94/s).
3. **Trigs go at once** and carry the current values, ahead of any params.
4. **Never wait in an interrupt.** Enter the OS's sender only when it cannot block; otherwise leave the message pending
   for the next try, and coalescing keeps it current. Find out whether your OS's sender may be entered from the render
   at all. On the Digitakt the example sends from a task the render wakes.
5. **No answer per message.** The Sidecar does not acknowledge reports. Repeat the current values slowly (the example
   does so every 0.5 s) so a lost message repairs itself.

For modulation faster or deeper than this, use the Sidecar's own LFOs (`core/lfo.h`) instead of sending more.

## What to implement, step by step

1. **Pick the machine.** `Profile::machine` is the voice byte's machine flag (`core/machines.h`). Read that machine's
   parameters: `sim/protocol.py` for the stock machines, `core/md/md.h` for MD.
2. **Write the table.** A `device::Profile`, in a file of your own under `core/device/`. Fill in: `id`, `name`, your USB
   ids, `tracks`, `voices`, `params`, `head`, the three commands, `machine`, `sound_tag` (0 unless the machine reads
   one), `map` and `fixed`. `map[k]` is the parameter of yours that becomes P(k + 1), or -1 for `fixed[k]`.
3. **If your mod reports like the Rytm**, list those commands in `in_allow`: they pass as they are, with the machine in each voice
   byte, and `map` and `fixed` are not used. The Digitakt does this, so one track can play PLAITS and the next JUNO.
   **Add a `translate` function only if neither can say it**, for example a different byte layout.
4. **Register it.** Add it to the list in `device.cpp` for a built-in box, or call `device::add()` from a test.
5. **Test it on the Mac**, without any hardware, as `core/tests/test_device.cpp` does: `make -C core engines`, then
   `make -C core build/test_device && core/build/test_device`.
6. **Write the mod.** Report a track's final parameters (after locks and LFOs) and its trigs, paced as above.
7. **Rehearse it in an emulator** before anything goes on a real unit. Keep a stock OS file at hand to recover.
8. **Pick a bracket and fill in the audio row** (see "Brackets" and "Audio"). With an audio row and your USB id in the profile, the
   Pi's Overbridge driver claims the box; no driver change for a box whose stream has the same shape.

## What is behind the profile, and what is not yet

| Assumption | Where it is now |
|---|---|
| Message ids of trig, params, note-off; SysEx identity | the profile |
| Track, voice and parameter counts | the profile (`voices` <= `kVoices` = 8, `params` <= 16) |
| Parameter layout | the profile's `map` / `fixed`, `in_allow` for the engine's own, or `translate` |
| What the engine may send to the box | the profile's `out_allow` |
| USB vendor and product id | the profile; the Pi's Overbridge driver claims a box by them (`device::by_usb`) |
| Channels each way, block sizes, a stereo mix or per-voice returns, return and stem channels | the profile's `audio`, an `out::Device` row |
| Which of the box's pages reach which of the Sidecar's stages | the profile's `controls` |
| Remote UI pages | off for a box unless `kRemoteUi`; in practice Rytm only, the pages are drawn for its screen |
| Kits, patterns, project names, send FX reports, user slots | Rytm only: a box's messages cannot reach them. The `Feature` bits only declare them so far |
| `kVoices`, `kBlock`, P1..P16 inside the engine | stay the engine's: a profile fits a box into them |
| The word "Rytm" in UI strings, and the engine's message builders | unchanged |
| Device -> host channels of the driver's consumers | 12 (the recorder, the sampler, the stems, the Sidecar's USB-C): a box with another count is refused until they follow |

## Brackets

What a box can do differs a lot, and a port should say so rather than fake it. A profile names its bracket and picks its options from
the tables; a box that fits one needs no new code.

| | `kBracketFull` | `kBracketMix` |
|---|---|---|
| Example | Analog Rytm MKII | Digitakt mk1 |
| `audio.ret` | `kPerVoice`: each voice on its own channel | `kStereoMix`: all voices on one stereo pair |
| What shapes a voice | the box's own filter, amp, sends and master FX | nothing of the box: the Sidecar's stages |
| `controls` | none | the box's pages, mapped onto those stages |
| The OUT route (TRK / PRE / POST) | per voice | none: every voice POST |
| Remote UI, kits, send FX reports | yes | no (`features`) |

### Controls

`Controls` lists what each value of the box's controls report does:

```
F0 7D 52 54 50 <track> { <value> <fraction> } x Controls::n F7
```

The targets are `device::Control`: `kCtlFreq`, `kCtlReso`, `kCtlType`, `kCtlFltEnv`, `kCtlFltA/D/S/R`, `kCtlAmpA/H/D`, `kCtlVol`, `kCtlPan`.
`type` turns the box's filter type values into `kFltOff`, `kFltLp`, `kFltHp` or `kFltBell`. Pace it with the params report (same cap per
track, in turns), and before a trig when a value changed, so a lock is in place when the note starts.

The stages behind them (`core/device/strip.h`), on the control core:

- **Filter:** the voice's INSERT slot A, FILTER or EQ, its knobs held as p-locks, so turning a knob writes nothing to the card. Slot A belongs
  to the strip on such a box. An open low-pass with no envelope turns the slot off, so a default sound costs nothing.
- **Amp:** OUT LEV and PAN of the voice in the mix.
- **Envelopes:** at the control rate (1 ms, then the stages' 3 ms smoothing), so an approximation of the box's. No attack passes the voice's
  transient untouched. The times' curve (1 ms .. 10 s) is ours, not measured from a box. No gate is reported, so an amp HOLD of 127 holds
  until the next trig, and the filter's release runs from a note-off.

The Digitakt's table is in `core/device/digitakt.cpp`: FILTER page 1 and AMP's ATK HOLD DEC VOL PAN. OVER, DEL, REV, BASE, WDTH, SRR and ROUT reach
nothing. TYPE OFF, LP and HP are what they say, EQ_1..EQ_5 are one bell (our EQ has one width).

## Audio

The driver is `pi/engine/ob_usb.cpp`, its log prefix `rytmob` for every box. It opens alt 3 on interfaces 1 and 2, with interrupt IN 0x83 and OUT 0x03,
and each direction's block is `32 + 7 x channels x 4` bytes. It refuses a descriptor that does not match the row. OUT goes out block for block with IN, so
the sizes do not matter to the pacing: the box's stream is the clock.

| | Rytm MKII | Digitakt mk1 |
|---|---|---|
| USB id, OVERBRIDGE mode | 1935:0b48 | 1935:0b2c |
| Rate, frames a block | 48 kHz, 7 | the same |
| Box -> host | 12 channels, 368 B | 12 channels, 368 B: MAIN L R, tracks 1..8, INPUT L R |
| Host -> box | 12 channels, 368 B | **2 channels, 88 B**: MAIN L R |
| Feedback | none: interrupt transfers, the device's IN paces | the same |
| The voices back | each on its channel, through the track | one stereo mix on the main output |
| Own voices in box -> host | yes, on the stems and MAIN | **no**: USB return is not in IN's MAIN |

Eight voices do not fit two channels: 8 x 4 bytes x 48 kHz needs 256 B a packet, and the endpoint takes 88. So on a Digitakt the voices have one
stereo pair. They are never in the Digitakt's filters, FX or track levels, a track's mute stops its trigs but not a tail, and what comes back over USB
has no Sidecar in it, so the sequencer's latency click and `stereo=measure` have no way back, and `loop_frames` is 0. The computer's OUT 3..12 on the
USB-C port go nowhere. The -12 dBFS ceiling of the mix is the Rytm's safety stage, and it has not been judged for a Digitakt.

A Digitakt in USB MIDI mode (1935:000d) or USB AUDIO/MIDI (1935:102c) stays with Circle's own drivers: MIDI, and no stream. A box's audio row follows its USB id, and a
state folder follows its hello, so a stock Digitakt gets the stereo mix and whatever folder the card last chose.

**Sources for the Digitakt's stream.** [Overwitch](https://github.com/dagargo/overwitch) by David García Goñi (GPL-3.0-or-later): its device table and
engine, read for facts only; no code of it is here. [The community Digitakt port](https://github.com/soejrd/digitakt-sidecar)'s measurements on a real unit (descriptors, 88 B OUT, host channels 0 / 1 on MAIN L / R,
47,999.5 frames/s, 4 minutes without a gap). The firmware itself in an emulator, for the descriptor bytes, 2 channels and the FILTER and AMP values.

## State folders

A box with `state_dir` set (`/.digitakt` for the Digitakt) keeps its projects, settings and pools there, and the Rytm keeps
its own in `/.rytm`, so they never mix. Shared content stays shared: `/Samples`, `/Plugins`, `/JUNO`, `/DX7` and the rest of
the card's top level. The engine's own code keeps saying `/.rytm/...`. Every file call passes through one path function per
port (`full` in `pi/engine/platform_pi.cpp`, `host_path` in `core/host/platform_host.cpp`), and those call
`device::state_path`, which swaps the folder. With the Rytm it returns the same pointer after one test.

The Pi's own files stay in `/.rytm` whatever the box: the network, the pairing, the screen, the black box
(`kMachine` in `core/device/device.cpp`). A folder of its own also means its own copy of what is derived from the card,
such as the sample library's index: built once, and it does not touch the Rytm's.

**The folder is chosen once per boot, before the first file is read, and never changes while the engine runs.** The engine
reads the card at boot, long before a box can speak, and a stock Rytm never says hello, so nothing can wait for one. Instead:

1. `device::boot()`, first thing in `engine_begin`, reads `/.rytm/device`: the id of the last box. No file, or a Rytm's id:
   the Rytm and `/.rytm`, exactly as before this layer existed.
2. A hello from a box that needs another folder is answered with status 3. The Sidecar finishes its saves, writes its choice
   to `/.rytm/device` and restarts. Your next hello is accepted.
3. A Rytm with the Sidecar mods says 35 at its port-up. If the Sidecar is on a box's folder, that asks for `/.rytm` and one
   restart in the same way.

**What a box without a hello gets:** the state the last run chose. On a card that has never met another box that is the Rytm,
`/.rytm`. A *stock* Rytm (no mods, so no 35) is known by its USB id: the Pi's Overbridge driver sees it, and on another box's folder
that asks for `/.rytm` and one restart, unless a box said hello this boot. Another box's USB id changes nothing: its hello decides.
A board with no restart hook (`hooks.device_restart`, Pi only) answers status 0 and stays.

**A Rytm and a Digitakt both on USB:** whichever got there first keeps the Sidecar, with no restart loop. While a Rytm is on USB, a hello that would
leave `/.rytm` is answered status 4, wait, and takes effect after the Rytm goes; so is one in the first 10 s of a boot the Rytm's id caused,
when the Rytm may not have enumerated yet (`/.rytm/device` says `1 usb` then). One box streams: the second keeps its MIDI, with no audio.

A box without `state_dir` shares `/.rytm`. A box sends no project name unless it can, so it stays on the project its folder
last had: `_default` on a new folder.

A bundle exported on one device can be imported on another: their contents have the same format. Nothing checks that today.

## Several devices

One Sidecar serves one box today, and nothing here builds more. What a box chose already lives in one `device::Link`
(`g_link`), not in loose globals, so a second link does not start with a rewrite. Three places still assume one link: the
output hook (`hooks.to_rytm` has no link to pass), `state_path` (the file layer has no link to ask), and `host_sysex`, which
does not say which port a message came from.

A second device at the same time would need:

- **A clock master.** One box leads, the others follow: MIDI clock from the leader, or the Sidecar's. The sequencer, the LFOs
  and the recorder each take one clock today, and two boxes' transports must not fight over it.
- **Voice ownership.** Eight voices shared out: which link owns which voice, what happens when a box asks for one it does not
  own, and how a box's `track` maps to a voice other than its own index. The translation is the place for it. `link_reset`
  silences every voice today and would have to touch only the link's own.
- **Separate project state.** Today there is one loaded project in RAM (pools, LFOs, inserts, the sequencer) and one folder,
  chosen once a boot. Two folders at once mean per-link state in all of those, and every file call knowing its link. This is
  the largest part. The boot-time choice and the restart would go. The restart is not acceptable while another box plays.
- **Audio return.** The Pi's Overbridge host serves one device, with one stream and one clock. A second box needs a second
  USB stream, and the two boxes' sample clocks differ, so one has to be resampled, or its audio goes out somewhere else. The
  Pi's USB bandwidth and CPU budget need measuring first.

Also: a way to tell the links apart (each USB MIDI device calls its own handler; the Rytm path's message has no port on it
yet), and a remote UI that belongs to one link.

## Rules that do not change

With a Rytm the layer adds one pointer test and one compare per incoming SysEx, on the control core, and nothing on the
audio path. Project formats do not change. A change to this layer must keep it that way.

Code in this repository is GPL-2.0-or-later. Do not put firmware images or anything derived from them in a repository.
