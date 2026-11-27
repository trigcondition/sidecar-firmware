> **Not in 0.5.2.** The master looper is switched off in 0.5.2 (`core/sidecar_features.h`, `SIDECAR_LOOPER 0`): no LOOP page on the
> Pi screen or the Rytm, no Loop page in the web, the console's `loop` commands answer "loop: not in this release", and the engine
> keeps no ring, records and plays nothing and never mutes a Rytm track or moves a PERF macro. A `loop.txt` on the card is left
> alone. The code stays built and tested with the switch on; the redesign (the loop at unity on the POST path) turns it back on.
> The LFO and MIDI-learn target lists still show LOOP:BAL and LOOP:LEV in 0.5.2; they set a value nothing reads. The next release
> hides them. Read this page as the design notes of that work.

# Looper: per-track or master grid? (2026-10-08)

## What the Rytm gives a USB host

- **Rytm -> host, per voice: dry.** The analog voices are digitized after their own
  chain (filter, drive, amp envelope); those signals feed the digital delay and
  reverb, whose returns go back into the analog mixer, and MAIN is that analog mix
  digitized again (rytm-emu docs/RYTM-PORT.md, codec link and USB audio
  [measured]). The 12 channels to the host are MAIN L/R, the 8 voices, AUDIO IN
  L/R, so a per-voice channel has no delay, reverb or master compressor/drive.
  Manual 14.6: tracks keep "their individual output" whatever the FX routing.
- **Rytm -> host, MAIN L/R: everything**, FX and master processing included.
- **Host -> Rytm, stereo into the main path** [OS 1.73 disassembly, inferred;
  not measured]: besides the per-voice inputs (`voice_output_mix`, before the
  filter and gated by the amp envelope), two routines read the input ring every
  block: `0x4010eb26` adds ring channels 2/3 to the delay/reverb return buffers
  into DAC words 12/13 (the return before the master analog distortion and
  compressor), and `0x4010ec04` reads channels 0/1 into DAC words 14/15 (after
  them). Their gain depends on the USB IN routing setting (0x413189bf, 0x413189c7,
  0x8000ee74), which the manual (14.6) describes as PRE-FX ("affected by the
  effects") and POST-FX ("not affected"). rytm-emu leaves these silent: it does
  not model the USB controller, so no USB mode is ever active.

## Per-track looper: not a good fit

A loop of one track would be recorded dry: its delay and reverb tails and the
master drive/compressor are missing. Played back into the main path, it would
sound different from the live track; played into the voice input instead, it
would go through the filter a second time and only sound while the track's amp
envelope is open. Track mutes and send changes would also not affect it the way
they affect the live track.

## Master grid (the plan)

Record MAIN L/R (the finished sound) and play it back on the POST path, so
nothing is processed twice:

- TRK + REC enters looper mode. The 12 pads are clip slots; UP/DOWN pages through
  more slots (like chromatic mode pages notes).
- An empty pad: records the master from the next bar for N bars (4 by default),
  then plays it from the SD card, looping.
- A playing pad: stops it at the end of the bar; another pad takes over
  (one clip playing at a time, Octatrack-style), so patterns, mutes and kits can
  change underneath and the loop is dropped on a bar line.
- Clips stream from SD (docs/PLAN.md, step 4); timing from the Rytm's clock.

Rytm side (a mod): catch TRK + REC and the pads in looper mode, keep the pads'
normal action from running, light them by clip state, and report pad presses and
the bar position to the Teensy (the EXT report channel). Teensy side: record
MAIN from the Overbridge stream to SD, play clips into MAIN L/R.

## In the emulator (2026-10-08)

rytm-emu now runs Overbridge for real (`--usb-host`, USB CONFIG = OVERBRIDGE;
rytm-emu docs/RYTM-PORT.md): host channel 0 (MAIN L) reaches the post-FX return,
channel 2 (MAIN FX L) the pre-FX return [measured, DAC words 15 and 12]. The
Rytm's 12 output channels reach the host over the link (`R` messages).

`sim/obhost.py` has the looper core (`Looper`): a rolling recording of MAIN L/R
and a loop mixed into MAIN L/R. `--loop-test AT,SECS,UNTIL` exercises it: with
the pattern playing, at AT s it loops the last SECS s of MAIN; the Rytm's STOP
then leaves the loop sounding until UNTIL s [measured: sound continued after STOP,
silence after UNTIL].

Open: the loop came back about 18 dB down (the emulator's MAIN return level, or a
firmware gain not traced yet: calibrate against a real Rytm); bar sync (clock);
the Rytm-side looper mode (TRK + REC, pads as clips, pages); SD streaming on the
Teensy.

## Looper mode (2026-10-08)

rytm-mods 0022-looper: TRK + REC enters looper mode; pads are clip slots, UP/DOWN
pages; reports `05 on page` and `06 slot down` (SysEx). `sim/obhost.py` acts on
them: empty slot -> record 4 bars from the next bar, then loop; slot with a clip
-> play from the next bar; playing slot -> stop at the next bar; with the
transport stopped, at once. Bars from MIDI clock (CLOCK SEND, TRANSPORT SEND on).
Checked end to end in rytm-emu (`--usb-host`, OVERBRIDGE): 7.56 s recorded at 127
BPM (4 bars), loop continues after STOP, the pad stops it.

## Resampling and level (0.5.0 master looper)

The ring records the master as heard: the loop plus the live Rytm. A grab while a loop plays (a resample) therefore holds both, at
about twice the level of the first grab when the Rytm is not muted. After one or two resamples with the Rytm live the master can clip:
turn LEV down for the resample (LEV 100 is the level as grabbed), or use auto-mute / LIVE VIA so the live Rytm is down under the loop
(then a resample is the loop alone, as the Octatrack's). The page and the console say nothing about this: it is the design.

The Pi plays a loop out on POST, which reaches the Rytm's main about 10.9 dB down, so the looper makes that up (x3.508). A quiet
loop comes back at the level it was grabbed (and a resample of a loop alone keeps its level). POST itself tops out about 10.9 dB
under the Rytm's full scale, so a loud loop cannot come back as loud as it was heard: the make-up is capped per loop.

## OUT: POST or PRE, and the level cap

LOOP page, encoder G (second row, third cell: OUT, POST / PRE; `loop=out:post|pre`; saved as `out=post|pre` in loop.txt).

- **POST** (default): the loop goes onto the Rytm's POST return (the Pi's outputs 0/1). The Rytm's USB IN levels are fixed in
  Overbridge mode, so this return always reaches the main 10.9 dB down, after the Rytm's own distortion and compressor.
- **PRE**: the loop goes onto the PRE return (outputs 2/3), added to whatever is already there (voices on OUT PRE, the send FX's
  PRE return, the computer's OUT, which the master FX take first). PRE reaches the main only 2.0 dB down (make-up x1.259 instead of x3.508), but it
  enters before the Rytm's analog distortion and compressor, so a loud loop is shaped by them a second time.

The limiter and the cap, on both. When a loop is grabbed it is copied to the store, and once, while it is copied (core 3, never in the audio
thread), run through a look-ahead peak limiter: stereo-linked, 1.5 ms look-ahead (the gain is down before the peak arrives), a smooth
20 ms release (about 60 ms to settle), at most 6 dB of gain reduction. Its threshold is the level at which the loop, played with the
make-up OUT has now at LEV 100, reaches the ceiling: 0.98 / make-up, so -11.1 dBFS on POST (x3.508) and -2.2 dBFS on PRE (x1.259).
The limiter does not touch the audio: it writes a gain envelope next to the grab (one 16-bit value every 16 frames, 2 bytes per 16 frames:
1.6 % on top of the audio, 190 KB for 16 bars), which the audio thread interpolates and multiplies in. Under the threshold the envelope is
exactly 1 (and after a release it snaps back to exactly 1). Over it the peaks come down, so the whole loop can play with more
gain: a drum loop with 0.9 FS transients over a 0.1 FS body comes back about 7 dB louder (RMS) than with the cap alone. Each grab
is limited on its own copy, a second grab never touches the first (a resample is a new grab, limited again). A grab that is still
being copied plays from the ring with the cap from its peak so far (not yet limited), and moves to the limited copy when it is
done (the envelope fades in over 10 ms as the cap's gain eases up: no step). KEEP saves the grab as it was, the original, never the
limited loop.

Then the cap: the loop is played with the make-up, or less if that would put the loop alone above 98 % of full scale: gain =
min(make-up x LEV x BAL, 0.98 / peak) with the peak of the limited loop. So one loop never clips and the soft knee (it starts at the
same 98 %) never shapes a loop alone. A loop whose limited peak is under -11.1 dBFS (POST) or -2.2 dBFS (PRE) keeps the whole make-up;
a louder one is as much quieter as it is louder than that, after the limiter's 6 dB: a peak at 0 dBFS is 5.1 dB quieter on POST than
heard. That is why POST can still be quieter than a heavily compressed live mix: POST reaches the Rytm's main 10.9 dB down, so a mix that
sits near full scale cannot be played back at its level through it; the limiter and the cap trade its peaks for the loop's body. PRE is
the way to get a loud loop back at full level, at the price of the Rytm's compressor and distortion again. The limiter is set for the
OUT the loop was grabbed with; if OUT is changed later the cap still holds (a loop grabbed on PRE and played on POST is only less limited
than it could be). The cap follows the loop (each loop its own peak: a second grab never changes the one that plays), LEV and BAL are
inside it (the cap is on the final gain: LEV 127 never takes a loop over the ceiling, LEV below 100 makes it quieter), and the knee stays
the backstop for several loops, or the Sidecar's own output, summed. A resample's loop is a new grab with its own peak.

## Calibrating the POST return (`loop=cal`)

`loop=cal` sends three clicks onto POST 0/1 and listens for them in the Rytm's MAIN. Besides the POST latency it now measures
the return's gain: the click's peak out / the peak that comes back (the result line shows it, `POST LAT 12 X3.51 10.9DB`). A gain
in 1.5 .. 6.0 is saved as `postgain` (thousandths, in loop.txt) and replaces the x3.508 on POST; anything else is ignored. It
is measured on POST only: PRE keeps its x1.259. Play nothing loud on the Rytm while it runs (a live signal at the click raises
the peak back and lowers the gain it finds).

Switching OUT while a loop plays crossfades between the pairs over 10 ms (no click).

Known limits of the cap: a loop that starts playing from the ring while its copy is still running uses the peak found so far (not yet limited; the gain steps down at once when a louder part is copied,
and it moves to the limited copy with a 10 ms fade when it is done); the peak includes the 64 frames before the loop's start; the measured POST gain
comes from a 24-frame click, which the Rytm's converters round off a little, so it can read slightly high (the cap still holds), and
a Rytm playing loud during `loop=cal` lowers it: keep the Rytm quiet while it runs. The soft knee never bends what is already on
the pair (a hot PRE or POST signal): only the loop's share is shaped, into the headroom left.
