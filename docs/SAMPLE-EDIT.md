# SAMPLE EDIT: cut, fade and save a sample from the pool

A small sample editor for the **SAMPLES** pool (docs/POOLS.md). It loads one sample whole, lets you set a region, run a
function on it (crop, normalise, trim, fades, reverse, gain), undo the last one, and save the result as a **new file**
next to the original. The original is never written. Pi 4 / Pi 5 and the Mac tests only; the Sidecar Mini (Teensy) has
no editor.

The Rytm's screen and the touchscreen show the same edit and stay in step. You can use either, or both.

## Opening it

- **On the Rytm, from a track:** on a DSP SAMPLE track's SRC view, push knob **B**. The editor opens the track's
  sample, and the region is the track's STA..END.
- **On the Rytm, from the pool:** on the POOL page (the SAMPLES list), pick a row and press **EDIT**. The region is the
  whole sample, and no track is attached.
- **From the MENU:** the **SAMPLE EDIT** row opens the selected track's sample. If an edit is open already, it goes back
  to that edit.
- **On the touchscreen:** the SAMPLE EDIT page appears by itself when an edit opens. While an edit is open, the VOICE nav
  key leads back to it.

Opening another sample while the edit has unsaved changes is refused with `SAVE OR DROP THE EDIT`. A track that is not
a DSP SAMPLE track is refused with `NOT A DSP SAMPLE TRACK`.

## The Rytm page

The header shows the file name. The waveform fills the middle: the region is solid, the rest is dithered, with a marker
at the start and one at the end. Under it are the region in seconds and the zoom (`*` marks unsaved changes). The
footer shows the function, a progress figure while a job runs, or a short message.

| control | does |
|---|---|
| knob **A** | picks the function |
| knob **B** | moves the region start |
| knob **C** | moves the region end |
| knob **D** | zoom (0 to 8, each step halves the window) |
| **FUNC** + a knob | the same, 8 times coarser |
| **YES** | runs the function on the region |
| **FUNC + YES** | the save choice (below) |
| **NO** | undoes the last function. With nothing to undo and no changes it leaves the page; with unsaved changes it asks `DISCARD EDIT?` |
| push **A** | plays the region once on the track (only when opened from a track) |

## The touchscreen

The page has a waveform you can drag (the start and end markers, and pan and zoom), ZOOM -, ZOOM +, PLAY, UNDO, SAVE
and CLOSE keys, and one key per function: CROP, NORM, TRIM, FD IN, FD OUT, REV, GAIN+3 and GAIN-3. There is no function
select knob; each key runs its function at once. PLAY needs a track. SAVE opens the same choice as on the Rytm. CLOSE with
unsaved changes asks to discard them.

## The functions

Each function works on the region (START to END), except trim, which looks at the whole sample.

| function | does |
|---|---|
| CROP | keeps only the region; the sample gets shorter |
| NORMALISE | scales the region so its peak is -0.1 dBFS. A silent region is left alone (`SILENT`) |
| TRIM | cuts leading and trailing silence: everything below -50 dBFS at the two ends goes, with a 10 ms fade-out at the new end. If all of it is below, nothing changes (`ALL SILENT`) |
| FADE IN | a linear fade from 0 to 1 over the region |
| FADE OUT | a linear fade from 1 to 0 over the region |
| REVERSE | plays the region backwards |
| GAIN +3 | +3 dB (a factor of 1.41254) |
| GAIN -3 | -3 dB (a factor of 0.70795) |

The editor works in floating point at full depth, so a gain over 1.0 is not clipped while you edit. Clipping happens
only when the file is written and in the preview. There is **one level of undo**: NO (or UNDO on the touchscreen) takes
back the last function, and after that there is nothing to undo until the next function. `NOTHING TO UNDO` says so.
While a function runs the editor is busy (a long sample takes a moment) and refuses other keys with `BUSY`.

## Live play

With an edit opened from a track, the track plays the edit, not the original. After each change the editor prepares a
new play buffer, and the track uses it from its **next trig**. A note that is already sounding keeps the buffer it
started on. A held LOOP note can keep the editor busy until it is re-triggered or released, because the editor never
writes a buffer a voice is still reading.

If you close the editor without saving, the track plays the original again.

## Saving

**FUNC + YES** on the Rytm (SAVE on the touchscreen) asks what to do. Knob A picks, YES confirms, NO goes back.

- **SAVE + USE** (only when the edit was opened from a track): saves, then points the track's SMP at the new row. If the
  new length differs from the one the track's STA and END were set for, STA becomes 0 and END 120. The footer says
  `SAVED, T3 USES A012`.
- **SAVE:** saves and adds the file to the pool; the track stays on the original.
- **CANCEL:** back to the edit.

After a save, the editor edits the new copy: it is the source now, and there is nothing to undo and no unsaved change.

**The name:** `<stem>-ed<N>.wav` in the original's folder, where N is one past the highest `<stem>-ed<k>.wav` already
there, starting at 1. A trailing `-ed<k>` on the original is dropped first, so `kick.wav` saves as `kick-ed1.wav`, and
`kick-ed1.wav` saves as `kick-ed2.wav`. The new file goes into the SAMPLES pool, in the original's bank or the next one
with room.

## Formats and limits

- **In:** WAV, 16, 24 or 32-bit integer PCM, mono or stereo, any rate. Float, 8-bit and AIFF files are refused
  (`NOT 16/24/32-BIT PCM WAV`).
- **Out:** the same bit depth, channels and rate as the source. A 32-bit source keeps 24-bit precision in the parts you
  did not edit. WAV chunks such as smpl loops and cues are not carried into the copy.
- **Length:** at most **87 s** per sample (`TOO LONG (MAX 87 S)`). The sample is held whole in RAM, with a second copy
  for undo and play buffers for live play. If there is not enough RAM, the open is refused with `NO RAM`. A streaming
  "long sample" mode is planned.
- Card work runs in small pieces on the card core, so playing is not interrupted.

## Putting samples on the Pi: the Inbox

On the web page, the **Files** tab has a drop zone: **Drop samples here**, or **Choose files**. The files go to
`/Samples/Inbox` (made when missing) and each WAV is added to the SAMPLES pool with `pool=smp:take:PATH`: the bank the
view shows, or the next one with room. Then open it in SAMPLE EDIT from the POOL page. Other files are uploaded, not added.

## Console

`sedit` shows the status line. `sedit=open:track:T`, `open:code:C`, `close`, `start:F`, `end:F`, `region:A:B`,
`nudge:S|E:N[:c]`, `zoom:L`, `window:A:B`, `fn:N`, `apply[:NAME]` (NAME is crop, norm, trim, fadein, fadeout, rev, gain+,
gain-), `undo`, `save`, `save:use`, `preview`. The touchscreen sends these same lines.

## Notes

- When a key closes a Sidecar page on the Rytm, the remote now goes back to the page it came from. SONG and PROJECT used
  to land on SIDECAR. This holds for all Sidecar pages, not only this one.
- The Rytm firmware is unchanged: the page uses the existing remote UI.
- No stretch, slicing or recording here; see docs/CHOP.md for slicing.
