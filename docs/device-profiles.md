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
- Back: the Rytm's own hello (0x35) returns to the Rytm. A box with `kRemoteUi` says hello to the pages with 0x36.

## What a device must send

All bytes after `F0` are 7-bit. `<value>` is 0..127, `<fraction>` is in 1/128 steps.

**1. Hello**, always with the Sidecar's identity, when your USB port comes up and every few seconds after that:

```
F0 7D 52 54 4F 01 <id hi> <id lo> 01 [name bytes] F7
```

`id` is 14 bits: `1` the Rytm, `2` the Digitakt mk1, `3` the Analog Four MKII, `0x3F00` and up for private experiments. Ask for a number when a
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
| Remote UI pages | off for a box unless `kRemoteUi`: the Rytm, the A4 and the Digitakt ("Remote UI on a box"). A page that needs a feature the box lacks is hidden (`extra_for_box`) |
| Kits, patterns, project names, send FX reports | Rytm only: a box's messages cannot reach them. The `Feature` bits only declare them so far |
| User slots | a box that reports the U1..U5 flags plays them (the A4, the Digitakt); the web's User machines tab fills them, in the box's own folder, and so does the USER SLOTS page on a box with `kRemoteUi` (docs/user-machines-pi.md). A box whose profile lets 19 in and 1A, 1D out gets a slot's labels too (the Digitakt: 19 what 0 only, the names and defaults of 1A, 1D's generations); enum names, presets and assigning (1B, 1C) stay the Rytm's, and the Digitakt's profile does not let 1E in |
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
| Remote UI | yes | by `features`: the Digitakt has it |
| Kits, send FX reports | yes | no (`features`) |

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

| | Rytm MKII | Digitakt mk1 | Analog Four MKII |
|---|---|---|---|
| USB id, OVERBRIDGE mode | 1935:0b48 | 1935:0b2c | 1935:0b47 |
| Rate, frames a block | 48 kHz, 7 | the same | the same |
| Box -> host | 12 channels, 368 B | 12 channels, 368 B: MAIN L R, tracks 1..8, INPUT L R | **8 channels, 256 B**: MAIN L R, tracks 1..4, INPUT L R; the driver widens it to the 12-channel layout |
| Host -> box | 12 channels, 368 B | **2 channels, 88 B**: MAIN L R | **6 channels, 200 B**: MAIN L R, tracks 1..4 |
| Feedback | none: interrupt transfers, the device's IN paces | the same | the same |
| OUT -> IN loop (`loop_frames`, LIVE IN's latency) | 289 frames | 0: no way back | 197 frames, 4.1 ms, measured on a unit, through the analog voice |
| The voices back | each on its channel, through the track | one stereo mix on the main output | each on its channel, through the voice's analog filters and amp |
| Own voices in box -> host | yes, on the stems and MAIN | **no**: USB return is not in IN's MAIN | yes, on the voice's channel |

**The A4's insert points** [read in OS 1.56, 0x400febf0]: host channels 2..5 join each voice's noise word on the way to the DAC, x a gain that follows the voice's amp from 0.25 up, so they reach the analog voice as its noise does, and its amp must be open: a track that plays. Channels 0 / 1 join the effects' return x USB TO MAIN's gain. An A4's USB id claims nothing by itself (`audio_by_hello`): a stock A4 beside a Rytm stays out of the Rytm's way. Its hello picks `/.a4`, the one restart follows, and from that boot on the driver claims it. Before that the engine hears its hellos and nothing else (`pi/engine/hello_gate.h`), and sends it nothing. `note_out`: the sequencer's notes on an A4 voice also go to it as MIDI, channel = voice, so its amp opens, and the mod's report of that very note is dropped. `chords`: a track's chord, mono or poly, reaches the engine as one 04 a note, because the A4's sequencer puts a whole chord in one event and its keys and MIDI give a note a block [measured in the emulator, OS 1.56, against the unit's one trig a step on a mod that read one slot]. Different notes on a voice, each within `kChordMs` of the last, are a chord: the first is the trig it always was on the voice's machine, each further one sums into the voice's channel (`inst::chord_on`: instances for PLAITS, JUNO, DAISYSP and user machines that declare them; `poly::chord_add` for 6-OP FM), and 0x11 lets all of them go. MD and mono user machines have no second note. A seq track, the arpeggiator and a live MIDI binding keep their own notes. All of a chord enters one analog voice, the voice of its track's number. `trk10`: the profile's trim on the voices' own channels, +18 dB for the A4 (`stereo=trim:trk:DB10` changes it live, until the box changes), because that gain rests low; 0 for the Rytm and the Digitakt, whose path stays bit for bit. A trim up goes before the voice's limit: one knee toward full scale instead of the -12 dBFS cut (`Output::Own`, `pi/engine/output.cpp`). At +12 dB the ceiling is where it was (0.25 x 3.98), the voice just reaches it softly; at +18 dB a peak past full scale goes through that knee.

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
when the Rytm may not have enumerated yet (`/.rytm/device` says `1 usb` then). One box streams: the second keeps its MIDI, with no audio (the exception is `a4=1`, under "Several devices").

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

A first step of this exists: `a4=1` (`docs/pi-engine.md`) streams an Analog Four MKII beside the Rytm, stock firmware, audio only: no link, no hello,
no voices. The Rytm's stream stays the clock and the A4's is resampled to it (`pi/engine/drift_src.h`). Its USB MIDI is no host link (`pi/engine/usb_box.h`).

Also: a way to tell the links apart (each USB MIDI device calls its own handler; the Rytm path's message has no port on it
yet), and a remote UI that belongs to one link.

## Remote UI on a box

A box with a 128 x 64 screen of the Rytm's layout (byte = page + 8 x column) can show the pages. List `kRemoteUi` in `features`, `Profile::screen` for its voice names and the bottom bar's words, and these commands, with the 0x30 family's version byte 1 after the command:

- `out_allow`: 36 37 38 39 3A 3C. The box gets the frame's rectangles and commit, keepalive, catch mask, splash and close. No 3D, 3E, 3F or 34: the engine draws the text into the frame for a box (`wire_ready`, `preview()`), because its text routine is the Rytm's.
- `in_allow`: 30 31 32: opened or closed, a key in the Rytm's codes, a turn.
- 0x36 is the box's hello to the pages: `<uptime hi> <uptime lo> <caps>`, uptime in 50 ms ticks, caps bit 0 understood 36. The engine reads it as a 35. Not 35: that asks for `/.rytm`, and an older engine would restart on it. Send it only after the 4F answer said accepted (status 1).

The rules are the Rytm's (`mods/0026-remote-ui` in sidecar-mods): the box draws the last committed frame while the page is open, and 1 s with no message from the host gives its own screen back. An open is the user's alone. So the engine asks a box for no open: `remote_show_page` with the page closed stays quiet there. SCOPE, its sources and the meters take `screen->names` too, each voice on IN 2 + the voice; a name `-` is a channel the box does not fill, skipped. The status page shows `voices` voices by `screen->names`, and voice pages join the ring once a voice has reported. On the A4 the pages shown are SIDECAR, SYSTEM, the voices, REC, SCOPE, USER SLOTS (`kUserSlots`, where the platform has the user machines: LEFT on SIDECAR), the MENU, LFO and WEB UI; POOL, SAMPLE, AUTO, CHOP, SEQ, SONG, POLY, INSERT, MASTER, ARP, PROJECT and KEYS need features it lacks. SCOPE's source list still names the Rytm's tracks. The Digitakt shows the same set, its voices named T1..T8 (`kDigitaktScreen`), with the bottom bar's words "UP + YES: PAGES"; its mod sends the keys in the Rytm's codes and leaves on its stock page keys, as the A4's does.

## What the page is told

`/status` has `boxes`, one row for every Elektron box on the USB-A ports (`pi/engine/box_list.h`), and always the box this boot serves, `on` 0 while it is unplugged.
A row: `key`, `id` (the profile's), `n` (first, second of that type), `name`, `role`, `on`; with a profile also `tracks`, `voices`,
`params`, `feat` (the `Feature` bits) and `state`. Roles: `host` served, `waiting` its mod said hello while another box is in use, `listening` no hello heard, `spare`
known and not used, `other` no profile. So two Rytms, or two A4s and a Rytm, are separate rows with separate keys, and the page counts them.

The key is the type and Circle's `umidiN`: it tells two plugged-in boxes apart and may change when one is replugged or the Pi restarts. Two boxes of a type served
at once also need their own state folders, keyed by something that stays with the box, such as its USB serial number; Circle's MIDI device does not carry that yet.
The engine serves one box and one state folder per type today.
With two A4s one is heard for a hello, the other waits for it to go.

The page (`pi/engine/www/index.html`) shows the row of boxes, puts the served box's name in its text and hides what `feat` rules out (`data-needs`, `data-lacks`).

## Rules that do not change

With a Rytm the layer adds one pointer test and one compare per incoming SysEx, on the control core, and nothing on the
audio path. Project formats do not change. A change to this layer must keep it that way.

Code in this repository is GPL-2.0-or-later. Do not put firmware images or anything derived from them in a repository.
