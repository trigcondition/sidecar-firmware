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

A box with `state_dir` set (`/.dt` for the Digitakt) keeps its projects, settings and pools there, and the Rytm keeps
its own in `/.rytm`, so they never mix. Shared content stays shared: `/Samples`, `/Plugins`, `/JUNO`, `/DX7` and the rest of
the card's top level. The engine's own code keeps saying `/.rytm/...`. Every file call passes through one path function per
port (`full` in `pi/engine/platform_pi.cpp`, `host_path` in `core/host/platform_host.cpp`), and those call
`device::state_path`, which swaps the folder. With the Rytm it returns the same pointer after one test.

The Pi's own files are in `/.sidecar` whatever the box (decided 2026-11-21): the network, the pairing, the screen and CPU
clock, the black box, the update and web app, and the indexes of the shared libraries (the card index, `library/`,
`dx7.bin`: one index of `/Samples`, `/JUNO`, `/DX7` for every box). See `core/device/sidecar_dir.h`; a card from an older
build has them in `/.rytm` and the first boot moves them. A box's folder holds only that box's state.

**Slots by attach order, folders by box type (decided 2026-11-21), and nothing restarts.** The first box to attach is slot 0,
the second slot 1, whatever their types; each is in its type's folder (`/.rytm`, `/.a4`, `/.dt`) from then on, on
whichever transport it spoke (`device::slot_of`), and its project, lists, store and settings are read from that folder at its
attach (`project_attach_step`, either slot). No file stores a slot number. A box attaches with its hello (`4F`), the Rytm with
its 35, and on the Pi any box with its USB device (`device::usb_attach`
at enumeration: the same type is the same slot both ways). Status 3 (restart) is no longer sent, and `/.rytm/device` chooses
nothing any more.

**At boot** slot 0 is presumed the Rytm's (`Link::presumed`): its project is read from `/.rytm` as it always was, so a *stock*
Rytm (no mods, so no 35) is slot 0 as it always was. Its first message confirms it. A box whose hello (or USB) comes before
any word from it takes slot 0 instead, and its folder is read into it.

**A box that leaves** (`device::release`: its USB device gone, its transport down, another box's hello or a Rytm's 35 on its
transport): its voices, transport, arpeggiator and MIDI notes stop, its transports route nowhere, and its edits are saved into
its own folder (`Link::parting`, `device::leaving()` until `idle_save` has settled). Until then only that same box may take the
slot back; another box's hello is answered status 4, wait (or it takes the other slot when that one is free). The box that stays
never moves: its slot, voices, folder and transport stay as they are (the base, `g_base`, is slot 0 while held, else the slot
that is). A box that comes back takes the lowest free slot and loads from its folder (the same slot it left, with nothing else
there since: its state is as it saved it).

**Two boxes at once** take both slots; a third box's hello is answered status 4, wait. A box's stream is claimed when it
enumerates, its hello or not (one stream a slot).

A box without `state_dir` shares `/.rytm`. A box sends no project name unless it can, so it stays on the project its folder
last had: `_default` on a new folder.

A bundle exported on one device can be imported on another: their contents have the same format. Nothing checks that today.

## Several devices

One Sidecar on the Pi (or the Mac, `CORE_MSAMP` builds) serves two boxes at once, any two of different types in either order
(the Rytm, an A4, a Digitakt), slot 0 the first attached and slot 1 the second (see "State folders" above). The two do completely
unrelated work. **Nothing of a project is shared:** each box has its own
folder, project, pools, sequencer, scenes, LFOs, effects, clock, transport, song and pages, and differs from the other only
in its profile's numbers. A part the second box does not have yet is a named gap (listed below), not a design choice. The
Teensy has one slot (`kVoiceSlots` 1), so everything there stays one.

What a box chose lives in one `device::Link` per box (`g_links[kLinks]`, `kLinks` 2): who holds it (`held`, the Rytm too),
its box (`box`, null for the Rytm), its folder. Each transport (0: the platform's host link, the USB-C box's MIDI or the
emulator's TCP link; 1: the other box's, the Teensy relay's) carries one slot at a time or none (`device::slot_of`,
`route_of`, -1 none). Which physical port a box is on (USB-C or the Teensy relay) is decided at run time and
changes nothing here.

### How a slot's state is reached

- **The slot in hand.** On the control core, `device::cur()` is the link whose message is being handled or whose state is
  being stepped; `prj_slot()` is its index. A box's messages are handled inside its link's `device::Scope`, and each module's
  tick runs once per live slot inside that slot's Scope (`device::live`: held, saving after its box left, or the base).
  Outside any Scope it is `g_base`: slot 0 while it is held, else the slot that is, so code that never names a slot does
  what it always did.
- **The Rytm's own machinery** (its params, scene and QPERF, kSnd, rytm_dump and patwrite, the calibration and its stems, the
  recorder's fxin, the JUNO / FM names to it, its MIDI CONFIG and SONG replay, `hooks.transport`) follows its slot:
  `device::rytm_at(s)`, `rytm_slot()` (-1 none), `rytm_home()` (its slot, else 0, where its state is read). Never slot 0 as
  such. On one slot (the Teensy) `rytm_at` is always true, as slot 0 always was.
  On the Pi, its pattern dumps and FILL's answers are read from whichever transport is routed to its slot
  (`bridge::rytm_byte(t, b)` through `device::rytm_route(t)`: transport 0 unless it carries another box, transport 1 while it
  carries it), in its Scope.
- **Per-slot module state** is one struct per slot, `[kVoiceSlots]` (or `Per<T>`), read through an accessor of the slot in
  hand (`prj()`, `pools::S()`, `ins::control_of(slot)`, `out::control_of(slot)`, `fx::control_of(slot)`,
  `master::control_of(slot)`).
- **Render and other cores** never read `cur()`: they take the slot from the voice (`slot_of_voice(vi)`).
- **Files:** every file call goes through `state_path`, which uses the slot in hand's folder on the control core, or the
  card request's (`device::CardScope`, set around each request the card core runs; `cardq::async` and the pool jobs carry
  the slot they were posted for). The web's own calls use the box its page serves (`g_web_link`).

### What each box has of its own

| part | where | notes |
|---|---|---|
| project, its name and saves | `core/pool/pool.cpp` (`prj()`) | read from its folder at its attach (either slot); a 0x10 names its own box's project |
| pool lists, JUNO / FM lists, the RAM store | `pools::S()`, `user/lists.cpp`, `store` | GRANULAR, WAVETABLE, MULTISAMPLE, CHOP resolve a voice's rows through its own slot; a file both lists hold loads once |
| LFOs and the modulation layer | `core/mod` | a box's layer takes its own voices, its INSERT chains, its send FX and master; kSnd (the Rytm's track sends) is the Rytm's slot's |
| INSERT and OUT | `ins::control_of(slot)`, `out::control_of(slot)` | buses, master chains, route; another slot's file holds its bus, ROUTE, PEQ, MFX and SFX lines; AMP plays on every box's voice and master chains, each box its own 6 (+2 master) (decided 2026-11-26); PRE / POST / TRK trims per slot (`stereo=trim` the slot in hand's, `trimpre=` / `trimpost=` every slot's) |
| output stage | `pi/engine/output.*`, `side_out.h` | beside a Rytm, slot 1's voices go to a side stage paced by its own box's stream; alone, a box has the base stage |
| send FX and master | `fx::control_of(slot)`, `master::control_of(slot)` | own TYPE, knobs, route (`sendfx.txt`), rooms; slot 0's TYPE and knobs stay the Rytm's kit 0x20s. The Rytm's instance, in either slot, is paced by its stems from its own stream's IN (`sendfx_pi::FromRytm(slot)`, SRC MIX `masterfx_pi::Stems(slot)`; its report 50 in `master::rytm()`); another box's by its voices |
| user slots U1..U5, user effects UFX1..UFX4 | `um.cpp` | four effects per box, own `/.a4/fxslots`; own effects budget; plugins get their own box's transport. A Rytm in either slot gets its own fxslots names in its 0x21 list, its own labels, and its own 2 s BASE / WIDTH refresh (`sendfx_step` runs in each slot's Scope) |
| scenes and the XY pad | `core/ui` | |
| sequencer | `seq.cpp` (`Per<T>`) | a box's tracks on its own voices; own mute gate; 31 / 32 / 08 counted per slot (`seq=ui[:S]`) |
| kits | `bundle/kits` | own queue, RELOAD ALL scan and records in its own folder; a box's 3B ends only its own voices' MIDI and arp notes |
| MIDI clock | `host_link.cpp` `engine_realtime_link` | each transport's F8 FA FB FC F2 clock its own slot's send FX grid, LFOs, arpeggiator and plugins |
| the Pi-wide jobs' clock | `rec_pi::Transport(slot, ..)`, `samp::transport(slot, ..)` | each on the clock of the box it belongs to (decided 2026-11-26): a sampler take follows the box it was armed for (the slot in hand: `box=S:samp=arm`, the page's box), its PLAY / next-bar start, STOP, bars and tempo; the recorder records the box whose PLAY started it while armed (its output stage, FX returns, IN channels and meters; a manual `rec=start` the base box's; decided 2026-11-26); the bundles' card writes slow and the storage bench is refused while any box plays (a box that stops or leaves clears its own); the MIDI machine's clock out to the Pi's USB-A devices is the box's whose voices alone send there, none while both boxes' do |
| fxin, keys, MIDI learn, preview | | read from a box's folder at its attach, saved there |
| remote UI | `ui/remote.cpp` | own pages, canvases, track tables and display reports; the touchscreen follows the box whose page opened or whose 49 came last |
| SONG RECORD | `core/song` | own takes, journal, clock, lanes; each transport's whole MIDI stream taps its own slot's song. The A4's start snapshot, restore and UNDO REPLAY use its own dumps and GLOBAL MIDI settings |
| arpeggiator, transport out, all sound off | `arp`, `hooks.transport_box(slot, ..)` | a box's STOP and CC 120 end only its own voices and its own send FX |

### Voices on two links

The Pi and the Mac render 16 engine voices (`core::kVoices`); the Teensy renders 8, and `-DCORE_VOICES=8` builds that engine
anywhere. A box names 8 (`kBoxVoices`: the voice byte's low 3 bits, a project's tracks). Slot 0 (the first attached) plays
engine voices 0..7 and slot 1 (the second) 8..15, whatever their types. Each box keeps its own voice count (an A4's 4, a Digitakt's
8). The wire never changes: `device::voice_in` turns a report's voice byte into the link's engine voice where the message is
handled (`host_sysex_slot`, `voice_report`), and `box_voice` turns it back.

- **Engine state** (machines, render arrays, the stream and sample-RAM state, MULTISAMPLE, WAVETABLE, GRANULAR, CHOP,
  LIVE IN, MIDI, user machines, poly FM, the sequencer's instances, the arpeggiator) is sized `kVoices` and indexed by
  engine voice. `render_voices` takes the 16-wide ring. The Pi splits each slot's eight as one box's always were (`split=4`:
  0-3 and 8-11 on core 1, 4-7 and 12-15 on core 2), and slot 1's only while a box has it, so a lone Rytm costs what it did.
- **Per-box state** stays 8 wide, one set per slot, indexed by the box's own voice: the INSERT chains (`ins::kVoices`,
  chains V1..V8), OUT's knobs and a profile's `out::Device` rows, the output stage, a project's files.
- **Resets:** `link_reset(why, slot)` silences that slot's eight only, with its own send FX; a box's hello leaves the
  Rytm's voices playing.

### Voices and CPU admission

Decided 2026-11-21 (`core/device/admit.h`). Each box asks for its type's voices (`Profile::voices`: the Rytm 8, the A4 4,
the Digitakt 8). `voices=` in cmdline.txt lowers a type's count (`voices=rytm:6,a4:2,dt:4`, each 0..8, never above its
profile's: a box's reports past its profile's voices are dropped anyway).

When a box attaches (`device::select`, both by hello and by USB), its voices are admitted against the Pi's boot bench,
scaled to the CPU clock setting, so that each render core stays under 80 % in the worst case:

- **A voice** costs the worst machine of the bench (any voice may play any machine) and its POST EQ (`bench ins OFF+OFF` / 8).
- **INSERT types are admitted when set** (decided 2026-11-22, like AMP below): a voice slot's type plays only while its
  voice's render core stays under 80 % with the types already admitted there, each at its own boot bench row (`bench ins a
  slot of each type`, a sixteenth of T+T on 8 voices x 2 slots; `admit::type_bp`, `ins_fits`, `voice_core`; the check is
  `ins::set_cpu_check`, set by `device.cpp`). Otherwise the slot plays dry and its strip shows `NO CPU`
  (`ins::Control::refused`), and each idle pass admits it again once there is room (`ins::regrant`). A type that plays is
  never refused afterwards; a box attaching later counts it. Before, every voice reserved the worst TYPE pair. A voice on OUT
  PRE / POST runs its chain in stereo, so its types count twice their row there (`device.cpp` `chain_mul`; a stereo-mix
  box has every voice on POST); a type already playing when its voice goes stereo is shed if its core is then over. Gap:
  the bus chains stay in the box row.
- **A box's chains:** its send FX, bus and master chains at their worst set, dry, measured as one bench row
  (`master::box_bench`), counted on the core that runs its send FX. Before that row (LT1, 2026-11-22) the sum of
  overlapping rows.
- **AMP is admitted when a slot loads a capture** (decided 2026-11-22), not reserved at attach: an AMP slot (a base-box
  voice's, or the master pair's) starts playing only while its render core stays under 80 % with the AMPs already playing
  there (`admit::amp_fits`, `amp_core`; the check is `amp::set_cpu_check`, set by `device.cpp`). Otherwise it plays dry
  and shows `NO CPU` (`amp::kCpu`) until there is room (another AMP stops, a box leaves). AMPs playing when a box attaches
  count against that box: the first ask wins. AMP plays on the base box's chains only, the other box's AMP slots play dry;
  the cap of 6 voice AMP slots (`MAX 6`) still holds. `/status` `admit` has `amps1` / `amps2`, the AMPs counted per core.
- **Under load** (LT3, 2026-11-22): each row alone on core 0 reads low against the two-box load, where all four cores share
  the L2. The model scales them (`admit.h` `kAmpLoadPct` 500 since LT4 (300 at LT3), `kVoiceLoadPct` 135, `kChainLoadPct` 110; the boot's `admit:`
  line prints them): the base box's master AMP pair measured ~26 % of a core against the bench's 2 x 4 %. With them,
  reserved at attach (an AMP on every base-box voice, the pair on core 2), a Rytm or Digitakt base box was over on core 2
  even alone; hence AMPs on load and the master chain on core 3 (below). Model with LT3's bench
  (`design/rebalance-core2/MODEL.md` in the project files): Rytm+A4 4/4, Rytm+Digitakt 8/8, A4+Rytm 8/8, with room for one
  AMP a core beside an A4 and none beside two 8-voice boxes.
- **Where the chains run** (decided 2026-11-22): slot 0's send FX on render core 2, slot 1's send FX and master chain on
  render core 1, each after its voices (`cores.cpp` `ChainCore`). Slot 0's master chain (the base box's while it is held,
  with its AMP pair) runs on core 3 during RenderBlock's wait for cores 1 and 2 (`MasterCore`, `admit::master_core`; an
  8-voice engine keeps it on core 2): the stats line prints its time as `c3 mfx`, inside `c3 own`. Core 3 is counted
  (decided 2026-11-23 after LT6: at 1200 MHz slot 0's master AMP pair there made core 3's passes 10-19 ms and the engine
  underran; with it off, none): `admit::core3_bp` is core 3's control work (13.4 %) and slot 0's master chain (8 %, its POST
  EQ and send TYPEs) as fixed shares at 1500 MHz, plus every master INSERT type admitted (its bench row, twice: stereo)
  and the master AMPs playing, within 70 % (`kC3LimitBp`: core 3 also waits for cores 1 and 2). The send TYPEs as master inserts (CHORUS, COMB, SPACE,
  RINGS, CLOUD, HALO) count at their own boot rows, `bench sfxins T x4` / 4 (`admit::master_slot_bp`; stereo already, so
  once). A master type or AMP with
  no room plays dry and shows `NO CPU`; at a lower clock `shed_over` sheds the master AMPs, then the master types. At 1500
  the pair fits (61.6 %), at 1200 one AMP of it. `/status` `admit` has `core3Bp` and `amps3`. LT1 found core 2 full with both boxes' chains on it (~95 % in config A) while core 1 was
  ~70 % idle. Fixed by slot, never by which box is the base, so each box's rings keep one render core whatever attaches
  or leaves; core 3 waits for both cores' chains before a pass whose base box changed (AMP's master rows follow the base).
- **Placement:** a box's voices are its 0..n-1; 0..s-1 render on core 1 and s..7 on core 2, s its split: `split=` (4), or
  half the voices its box asks for if fewer (an A4's 4 as 2 + 2), so a box's voices share both cores with the chains.
  Core 3 puts each slot's split in every pass, so cores 1 and 2 never split a slot differently.

The first box keeps its voices. The second gets the most that keep every core its voices land on under 80 % (a core
already over by the chains alone takes none of them), never more than it asked for. Its reports past them are dropped
(`host_sysex_slot`), the render cores skip those voices (`device::voice_admitted`), the box is told by a popup on its
screen (`SIDECAR 4/8 VOICES` / `CPU FULL`, for 30 s while its screen can take one), and `/status` `slots[]` has `voices`,
`asked` and `short`, with `admit` (bench, MHz, the limit and each render core's worst case now, in 1/100 % of a core).
When its partner leaves, a box gets back what it asked for, once the partner's slot is no longer kept for it (saving, then `slothold=`, 5 s by default): a plug or unplug on one of the Pi's hub ports knocks the box on the other port off and back (re-enumerated; seen on a unit 2026-11-21, 8 of 8), and a box back to its kept slot keeps its place in the attach order, so a first box stays first with its voices. A clock change after an attach does not take voices away; it
applies to the next attach (the boot log's `admit at N MHz, the second box's voices:` line lists each pair). No bench
(`bench=0`, the host, the Teensy): nothing is refused.

Not settled yet: the costs are an upper bound from the boot bench rows; the two-box worst-case measurement on a unit (full
voices, the worst machine, expensive user FX on both boxes) checks them before the rule ships.

### Audio

Each box's audio stays its own: each slot's voices go back to its own box's stream (`CObUsb::Slot` is the core slot
`usb_attach` gave it), each paced by its own box's clock. No audio crosses between boxes and nothing is resampled. (`a4=1`, `docs/pi-engine.md`, is the
older stock-firmware path: an A4 with no mod streamed beside the Rytm, audio only, resampled into the Rytm's PRE pair by
`pi/engine/drift_src.h`. An A4 with the Sidecar mod takes slot 1 instead.)

### One per Sidecar by design

The Rytm's own parameters, scene and QPERF, its 0x21 / 0x22 names and core_c's host API belong to the Rytm and follow its
slot (`rytm_at`, `rytm_home`). The Pi's own files (network, pairing, screen, black box) are in `/.sidecar`, no box's.

### The console's box selector

Console commands run for the base box (`g_base`) as they always did. `box=S:<command>` runs one in another box's slot (design
point 8): S is a slot (`0`, `1`) or a box type by its folder's name (`rytm`, `a4`, `dt`), which names the same box whatever its
attach order (`device::box_prefix`). Inside it, the voice forms (`gr=`, `wt=`, `msamp=` `voice:V` and `pick:V:..`,
`livein=voice:V`) take that box's own voice V (0..7), so `box=dt:wt=voice:2` forces the Digitakt's voice 3 whichever slot it is
in. A selector naming no box attached now is logged and does nothing.

The Rytm's project slots (`prjslot`, its 4B reports) and the RENAMED FROM? question on its PROJECT page run only for the Rytm's
slot (`rytm_at`): another box's project name only names its project, in its own folder.

### Named gaps

What the second box does not have yet, or has differently:

- **The web's bus meters and spectrum strip** show the base box's send FX (`/status` `slotFx` has both slots' counters).
  The recorder, its meters and `stereo=measure` are either box's (decided 2026-11-26: the recorder takes the box whose
  PLAY started the take; `box=S:stereo=measure` measures that box's own stage and MAIN IN).
- **Sequencer on slot 1:** a lock's base for P1..P16 is 64 (the box's knob value is not known here); no SEED MOD source; no
  latency calibration (`seq=cal` is the Rytm's slot only; a box keeps `seq=lat`); the touch editor's undo history is the
  Rytm's slot's.
- **Kits:** the A4's mod sends no 4C yet, so its slot's kit records exist only once it reports SAVE / RELOAD KIT.
- **SONG RECORD on the A4:** its stopped program change is not traced, so a restore or undo writes only once its mod has
  reported the pattern selected (2D 02); `midi_config()` is the Rytm's.
- **Readers off the control core** (`gr` / `wt` / `msamp` `entry_note`, the `srclib` browser, `page_gfx`, `preset_index`)
  read the slot in hand's lists.
- **A box replaced before its saves land:** a different box can take its slot only once `idle_save` has settled; until then
  it waits (status 4) or takes the other slot. A Rytm presumed at boot who never spoke gives slot 0 up at once (no save first).
- **An Engine of SONG RECORD** keeps the MIDI CONFIG kind (the Rytm's default or none) its slot had when it was first made.
- **Two boxes of one type:** see "Two boxes of one type" below for what is built and what is still a gap.
- **Cost:** two boxes' expensive user effects on one core (core 2) are not measured on the Pi yet (CPU admission's upper
  bound stands in for it until then: "Voices and CPU admission").

## Two boxes of one type

Two Rytms (or two A4s, two Digitakts) on one Pi, design decided 2026-11-26. A box is its type **and its DeviceUID**: the stock
firmware's 32-bit unit ID, the answer to the Elektron MIDI RPC `0x03` (answer `0x83`) and to the USB vendor request Overbridge
sends (`0xC0`, bRequest 0, 8 bytes). Nothing is written to the box. The USB serial cannot tell two units apart: every Elektron
box says `000000000001`.

- **Identity.** `Link::uid` (0: not known) beside `held`. A slot is box (p, uid)'s when the holder is p and the UIDs match
  (`device::slot_of_box`); a holder whose UID was not known (the presumed Rytm, a box by its hello) takes the first known UID
  while it is the one box of its type. A box whose UID is not known, with a box of its type already here, waits (status 4).
- **Where the UID comes from.** On the Pi, `ObReadUid` (`pi/engine/ob_usb.cpp`) sends the vendor request when the box's
  stream enumerates and passes the UID to `usb_attach`. When that gives nothing, the core asks the MIDI RPC on the box's own
  transport every 500 ms for 2 s (`device::ask_uid`), never while its stream is frozen (`device::g_frozen`). The Teensy, one
  slot, never asks.
- **Folders.** A box's state is in `<type folder>/<uid>/` (`/.rytm/3fa2c001/`, `/.a4/...`, `/.dt/...`; 8 lowercase hex
  digits). `<type folder>/last` names the UID attached last: a box whose UID is not known, alone of its type, uses that folder,
  and the presumed Rytm reads it at boot. A card no UID was ever read for keeps using the type folder itself, as before.
- **Migration.** The first box of a type to attach with a known UID inherits the type folder's old layout: every entry that is
  not a UID folder, `last` or a Pi-wide name is renamed into its UID folder (FAT: a rename, nothing copied). A marker
  `.moving` is written first and removed last; a boot that finds it finishes the move. Nothing is overwritten or deleted. On
  the Pi the move runs on the card core (`cardq::kFnReq`), after the card requests queued before it. **An older build on a
  migrated card** sees an empty `/.rytm` (as with the `/.sidecar` move): say so in the release notes.
- **Names.** Each UID folder has `name.txt`, default the type and the UID's last 4 hex digits (`Rytm 3fa2`). `/status`
  `boxes` rows are keyed by type and UID and carry `uid` and `label`; the web page renames a box with a double-click
  (`box=<uid>:name=<text>`).
- **MIDI to its box.** Circle's USB MIDI device names its USB device (`pi/circle/circle-midi-usbdevice.patch`), so
  `CObUsb::SetMidiDevices` matches each transport to the stream of the same USB device, not by strings, and tells the core
  (`device::set_midi_slot`): a hello or a Rytm's 35 on that transport is that slot's box.
- **The console's selector** also takes `rytm2` (the second of a type) and a box's UID (8 hex digits, or the last 4).
- **The Rytm per slot.** `rytm_home()` is the Rytm in hand when the slot in hand is a Rytm; a Rytm in slot 1 takes its send
  FX from its kit's 0x20s, not the SFX lines; arp's track tables are each Rytm's own.

Named gaps of this part:

- **Byte order (step 0, 2026-11-26):** the units answer the vendor request with `00 00 00 01` and then the UID MSB first,
  and that UID is the MIDI RPC 0x03 answer. This was read from Overbridge's and Transfer's own records of the bench Rytm, A4
  and Digitone, not by a run of ours. `ObReadUid` reads bytes 4..7 big-endian. The emulator's `usb-rytm` sends its `uid`
  property as 8 big-endian bytes, so the folder is the property's low 32 bits; its leading word is 0, not 1 (nothing reads it).
- **The second Rytm's pattern view and FILL:** the pattern dump reader and FILL's writer are one, the first Rytm's.
- **Two A4s, two Digitakts:** covered by the same code, not run.
- **Two Rytms on the computer:** the mirror and the relay forward the UID request to their own box; not run with two units.
- **Two Rytms' FX cost** on one core: measure in the QEMU FX load gate, then on units.

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
