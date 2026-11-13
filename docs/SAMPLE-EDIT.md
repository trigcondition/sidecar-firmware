# SAMPLE EDIT: cut, fade and save a sample from the pool

A small sample editor for the **SAMPLES** pool (docs/POOLS.md). It loads one sample whole, lets you set a region, run a
function on it (crop, normalise, trim, fades, reverse, gain), undo the last one, and save the region as a **new file**
next to the original. The original is never written. Pi 4 / Pi 5 and the Mac tests only; the Sidecar Mini (Teensy) has
no editor.

The Rytm's screen and the touchscreen show the same edit and stay in step. You can use either, or both.

## Opening it

- **On the Rytm, from a track:** on a DSP SAMPLE or SAMPLER track's SRC view, push knob **B**. The editor opens the track's
  sample, and the region is the track's STA..END.
- **On the Rytm, from the pool:** on the POOL page (the SAMPLES list), pick a row, turn knob **D** to **EDIT** and press
  **YES**. The region is the whole sample, and no track is attached. Another list gives `EDIT: SAMPLES ONLY`, and a row
  without a sample gives `NO SAMPLE IN THE ROW`.
- **From the MENU:** the **SAMPLE EDIT** row opens the selected track's sample. If an edit is open already, it goes back
  to that edit.
- **On the touchscreen:** the SAMPLE EDIT page appears by itself when an edit opens. While an edit is open, the VOICE nav
  key (and UP + T4 on the Rytm) leads back to it. Selecting another track on the Rytm while VOICE shows keeps VOICE
  following the track; it does not pull the display back to the edit.

A project change on the Rytm closes the editor without saving, so the new project's sample never plays the old edit.

Opening another sample while the edit has unsaved changes is refused with `SAVE OR DROP THE EDIT`. A track that is not
a DSP SAMPLE or SAMPLER track is refused with `NOT A SAMPLE TRACK`.

## The Rytm page

The header shows the file name. The waveform fills the middle: the region is solid, the rest is dithered, with a marker
at the start and one at the end. When the edit goes over 0 dBFS (after a GAIN +3, say), the waveform is scaled down so
its peak fits and a dotted line on each side shows where 0 dBFS is: what is outside the lines will clip in the file.
Zoomed in, a bar under the waveform shows which part of the sample the window shows. Under it are the region in seconds
and the zoom, `X1` to `X256` (`*` marks unsaved changes). The footer shows the function (`E:CROP  YES:DO`), a progress
figure while a job runs, or a short message.

| control | does |
|---|---|
| knob **A** | moves the region start. One detent is 1/128 of the shown window |
| knob **B** | moves the region end, the same way |
| **FUNC** + A or B | jumps the marker to the next (turn right) or previous (turn left) transient, one transient a detent. `NO MORE TRANSIENTS` when there is none that way |
| knob **C** | zoom (0 to 8, each step halves the window). FUNC does nothing here |
| knob **D** | moves the zoomed window through the sample (1/16 of the window a detent; with **FUNC** 8 times that). At X1 there is nowhere to go |
| knob **E** | picks the function |
| **YES** | runs the function on the region |
| **FUNC + YES** | the save choice (below) |
| **NO** | undoes the last function. With nothing to undo and no changes it leaves the page; with unsaved changes it asks `DISCARD EDIT?` |
| push **A** | plays the region once (see Live play) |

The 0.4.4 Rytm mods already pass knob E to Sidecar pages (the ARP page uses it), so this works without a Rytm update.

### Markers on zero crossings

The start and the end always land on a zero crossing, so a cut does not click. A marker goes to the crossing nearest
the place asked for, within 256 frames either side (5.3 ms at 48 kHz). For a stereo sample that is first a frame where
both channels are within -50 dBFS of zero; if there is none, a crossing of the two channels' sum. A turn of A or B only
looks past the marker in the direction of the turn, so every detent moves it. If there is no crossing in reach (a DC
offset, a very low note), the marker goes to the place asked for, as it is. The ends of the sample (0 and the last
frame) are kept as they are. The region in seconds shows the snapped places. This holds for the knobs, for a drag on
the touchscreen and for `sedit=start:` / `end:` / `nudge:`; `sedit=region:A:B` sets the places exactly.

### Transients

After every change (the load, a function, an undo) the editor looks for transients in the background: a few
milliseconds of work, never in the audio path. It cuts the channels' sum into hops of 256 frames. A hop is a transient
when its mean square is over -46 dBFS (RMS) and at least 4 times (+6 dB) the louder of the two hops before it, and it
is at least 50 ms after the last transient. The place is the first frame in that hop over half the hop's RMS, and the
marker lands on the zero crossing at or just before it (else the nearest one). FUNC + A or B right after a change can
say `FINDING TRANSIENTS` for a moment. It keeps the first 2048 transients of a sample. The start never jumps past the end, nor the end before the start. Transients are
on the Rytm only; the touchscreen has no transient keys.

### The window follows the marker

Zoomed in, a start or end that leaves the window (a knob turn, a transient jump) scrolls the window with it, so the
marker is an eighth of the window inside the edge. Knob D moves the window on its own, without moving the markers.

## The touchscreen

Drag a marker (start or end, when you touch within reach of it) to move it; the new place is sent when you let go, and
the editor puts it on the nearest zero crossing (as above). Drag anywhere else on the waveform to pan the window. Zoom
is by the keys only: ZOOM - and ZOOM + double or halve the window around its middle, down to 64 frames. There is no
drag or pinch zoom. Zoomed in, a bar bottom left shows where the window is in the sample. Over 0 dBFS the waveform is
scaled to fit and 0 dBFS is dotted, as on the Rytm.

Below are the keys PLAY, UNDO, SAVE and CLOSE, and one key per function: CROP, NORM, TRIM, FD IN, FD OUT, REV, GAIN+3
and GAIN-3. There is no function select knob; each key runs its function at once. The function keys, UNDO, SAVE and PLAY
work only while the editor is ready (not loading or busy). UNDO needs something to undo. PLAY plays on the track the
edit was opened from; opened from the pool, on the Rytm's selected track if it is a DSP SAMPLE or SAMPLER track, else on the
first such track (`NO SAMPLE TRACK` when there is none). SAVE opens the same choice as on the Rytm. CLOSE with unsaved changes asks to discard them.

## The functions

Each function works on the region (START to END), except trim, which looks at the whole sample.

| function | does |
|---|---|
| CROP | keeps only the region; the sample gets shorter |
| NORMALISE | scales the region so its peak is -0.1 dBFS. A silent region is left alone (`SILENT`) |
| TRIM | cuts leading and trailing silence. The floor is the louder of -50 dBFS and the level of the first 50 ms plus 12 dB. It keeps 2 ms before the sound and 10 ms after it, then fades out over 10 ms at the new end. If all of it is below the floor, nothing changes (`ALL SILENT`) |
| FADE IN | a linear fade from 0 to 1 over the region |
| FADE OUT | a linear fade from 1 to 0 over the region |
| REVERSE | plays the region backwards |
| GAIN +3 | +3 dB (a factor of 1.41254) |
| GAIN -3 | -3 dB (a factor of 0.70795) |

The editor works in floating point at full depth, so a gain over 1.0 is not clipped while you edit. Clipping happens
only when the file is written and in the 16-bit play buffers (live play and PLAY). There is **one level of undo**: NO (or UNDO on the touchscreen) takes
back the last function, and after that there is nothing to undo until the next function. `NOTHING TO UNDO` says so.
While a function runs the editor is busy (a long sample takes a moment) and refuses apply, undo, save and preview with `BUSY` (moving the markers and zoom still work).

## Live play

While an edit is open, every DSP SAMPLE or SAMPLER track whose SMP is the edited row plays the edit, not the original file: pads,
chromatic and the sequencer's trigs alike. That is the track the edit was opened from, but also an edit opened from the
POOL page, and any other track on the same row. The edit plays from RAM: after each change the editor writes the edit
into a 16-bit play buffer in small pieces on the control core (never in the audio path), and hands it to the voices in
one step; a track uses it from its **next trig**. A note that is already sounding keeps the buffer it
started on. A held LOOP note can keep the editor busy until it is re-triggered or released, because the editor never
writes a buffer a voice is still reading.

If you close the editor without saving, the track plays the original again.

After a plain **SAVE**, the tracks still point at the original row (SAVE does not change any track's SMP). So while the
editor stays open, the original row keeps playing the edit, including the changes you make after the save; the new
row plays the file as it was saved. Closing the editor gives the original row its own file back. After **SAVE + USE**,
the track points at the new row and the edit plays there.

If there is not enough RAM for the two play buffers, the editor still opens, but without live play: the footer says
`NO LIVE PLAY: RAM` once, the tracks play the file, and PLAY says the same.

## Saving

**FUNC + YES** on the Rytm (SAVE on the touchscreen) asks what to do. Knob A picks, YES confirms, NO goes back.

- **SAVE + USE** (only when the edit was opened from a track): saves START to END, then points the track's SMP at the new
  row. If the new length differs from the one the track's STA and END were set for, STA becomes 0 and END 120. The footer says
  `SAVED, T3 USES A012`. A write the Rytm answers busy is sent again every 20 ms for up to 1 s; still busy then, the
  footer says `USE: RYTM BUSY` (the copy is saved and pooled; set SMP, STA and END on the Rytm by hand). A write the
  Rytm refuses (for instance locked on the steps held) says `USE: RYTM REFUSED`. Each track's writes are followed on
  their own, and a project change drops them. An older Rytm build that does not answer: nothing more is said.
- **SAVE:** saves START to END and adds the file to the pool; the track stays on the original.
- **CANCEL:** back to the edit.

SAVE writes only the region, START to END, with the functions applied, as on the Digitakt or the Octatrack. No CROP is
needed first: move the markers and save, and the new file is that part. With the markers around the whole sample it
writes the whole.

After a save, the editor edits the new copy: the region saved is the whole edit now (START at its start, END at its
end), it is the source, and there is nothing to undo and no unsaved change.

With SAVE + USE the track's SMP points at the new row; when the new file's length differs from the one the track's STA
and END were set for (a region short of the whole always does), STA becomes 0 and END 120, so the track plays the whole
new file. A save of the same length keeps STA and END. On a SAMPLER track the same words are its SMP (P11), STRT (P13)
and END (P14): the same writes.

A copy whose audio is identical to a row the pool already has (for instance the whole sample saved with no function
run) is not added twice: the footer says `SAVED: SAME AUDIO AS A003` (the row that holds it), and SAVE + USE points the
track at that row. The file is still written to the card.

**The name:** `<stem>-ed<N>.wav` in the original's folder, where N is one past the highest `<stem>-ed<k>.wav` already
there, starting at 1. A trailing `-ed<k>` on the original is dropped first, so `kick.wav` saves as `kick-ed1.wav`, and
`kick-ed1.wav` saves as `kick-ed2.wav`. The new file goes into the SAMPLES pool, in the original's bank, else the shown
bank or the next one with room. The pool entry waits for the library to list the new file (a moment on a big card);
if that takes more than 5 s, the footer says `SAVED, NOT IN POOL` and the file is on the card, ready to add by hand.
The engine's own card index (made at boot) learns the new file at once, so the new row plays and shows its waveform
straight away, not only after the next boot. It holds up to 128 such files a boot; past that the footer says
`SAVED: REBOOT TO PLAY`: the row is in the pool and plays after the next boot. A copy whose file name is longer than the card
index holds (39 characters with `.wav`) is saved and pooled but cannot play: the footer says `SAVED: NAME TOO LONG`,
and SAVE + USE leaves the track on its sample. Rename the file on the card and add it again. The new file also shows in the card
browser's file list of its folder; a folder that had no WAV at boot shows only after the next boot.

## Formats and limits

- **In:** WAV, 16, 24 or 32-bit integer PCM, mono or stereo, any rate. Float, 8-bit and AIFF files are refused
  (`NOT 16/24/32-BIT PCM WAV`).
- **Out:** the same bit depth, channels and rate as the source. A 32-bit source keeps 24-bit precision in the parts you
  did not edit. WAV chunks such as smpl loops and cues are not carried into the copy.
- **Length:** at most **87 s** per sample (`TOO LONG (MAX 87 S)`). The sample is held whole in RAM, with a second copy
  for undo and play buffers for live play. If there is not enough RAM, the open is refused with `NO RAM`.
- Card work runs in small pieces on the card core, so playing is not interrupted.

## Putting samples on the Pi: the Inbox

On the web page, the **Files** tab has a drop zone: **Drop samples here**, or **Choose files**. The files go to
`/Samples/Inbox` (made when missing) and each WAV is added to the SAMPLES pool with `pool=smp:take:PATH`: the bank the
view shows, or the next one with room. Then open it in SAMPLE EDIT from the POOL page. Other files are uploaded, not added.
Uploading a file with the same name again overwrites the file in `/Samples/Inbox`. With the same content, the pool
answers `IN POOL` with the row it already has. With changed content, which row it ends up in is not fixed: upload
changed files under a new name.

## Console

`sedit` shows the status line. `sedit=open:track:T`, `open:code:C`, `close`, `start:F`, `end:F`, `region:A:B`,
`nudge:S|E:N[:c]`, `jump:S|E:N` (transients), `zoom:L`, `pan:N[:c]` (knob D), `window:A:B`, `fn:N` (a number, or a name: short or as shown, any case), `apply[:NAME]` (NAME is crop, norm, trim, fadein, fadeout, rev, gain+,
gain-), `undo`, `save`, `save:use`, `preview`. The touchscreen sends these same lines.

## Notes

- When `DISCARD EDIT?` YES closes SAMPLE EDIT on the Rytm, the remote goes back to the page the editor was opened from.
  Other Sidecar pages keep their own behaviour.
- The Rytm firmware is unchanged: the page uses the existing remote UI.
- No stretch, slicing or recording here; see docs/CHOP.md for slicing.
