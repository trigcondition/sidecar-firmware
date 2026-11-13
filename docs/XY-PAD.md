# XY pad

The touchscreen as a "Kaoss pad" controller (0.5.0, Pi). A finger on the pad moves up to four targets on X and four
on Y at once, each with its own depth. Set up on the touchscreen only; saved per kit, with a project default.

Tags: **[code]**; **[measured]** run on the Mac (host tests, screenshots); **[open]** only the hardware can show it.

## Where to find it

Settings > Send FX > **XY** (beside Morph). The page is the pad (X right, Y up, a dot at the finger with a short trail,
a ring at a held position), **HOLD** and **SETUP** beside it, the position and the eight slots under them.

- **HOLD** off: lifting the finger puts every target back. On: the values stay where the finger lifted, until the
  next touch or HOLD off (HOLD off while a held state plays releases it).
- **SETUP**: X's and Y's four slots, each a target key and a depth slider (-100..+100 %, negative inverts). The target
  key opens the picker: the kind (PERF, MORPH, SCN, TRACK (a Rytm track's Sidecar voice params, then its own Rytm params), FX, SEND, LOOP, INS, BUS, MFX, OUT, SEQ, ARP), then its
  index and parameter with - and +; **SET**, **NONE** (clears the slot), **BACK**. **PAD** returns to the pad.
- Both UI styles (OLED, SIDECAR), every screen size.

## What a slot does

The targets are the LFOs' targets (the same names as in the LFO files), plus **MORPH**, the scenes' fader. A slot is a
source of the modulation layer (source 40), so it sums with the LFOs and MIDI learn and the layer clamps the sum.
With the position p 0..1 and the depth d -1..1:

| target | while touching |
|---|---|
| a parameter the Sidecar reads (V1..V8, FX, SEND) | base + d x p x the range's span (the base is never written) |
| an offset target (INS, BUS, MFX, OUT) | + d x p x 127 on the knob |
| the Rytm's own sound parameters (RT1..RT12, RFX; 0.5.0) | + d x p x 127 steps, added on the Rytm after its own modulation; the release sends offset 0 (the Rytm never stored anything). [RYTM-PARAMS.md](RYTM-PARAMS.md) |
| a value-only target (SEQ, ARP, LOOP, RYTM: PERF and SCN) | from the range's low end (d > 0) or high end (d < 0): lo + d x p x span, or hi + d x p x span. On two slots (X and Y), it starts from the first slot's end once and both slots' moves add |
| MORPH | the fader set to its position before the touch + d x p x 16383 |

The release with HOLD off: every offset back to 0; a RYTM target sent once more at its zero point (the value at
position 0: the Rytm keeps what it was sent last, the Sidecar cannot read its real value back), offered until the
Rytm sender's 5 ms spacing has let it out; MORPH back to its position before the touch, unless another source moved
the fader since. A RYTM slot cleared or retargeted while touching or held returns its macro the same way; a MORPH slot
cleared gives the fader back to its position before the touch (or to the MORPH slots left).

A kit or project change while touching releases first and drops a held state; that finger's further positions are
ignored until it lifts. No RYTM zero point is sent then: the Rytm already has the new kit. SAVE KIT / RELOAD KIT of
the kit in use is no change: the finger and a held state stay.

The pad moves only in a build with the modulation layer (`CORE_MOD_LFO` 1, the default). A rollback build
(`CORE_MOD_LFO` 0, the LFOs on their own write path) shows the page and takes the commands, but nothing moves.

## Console

The UI's only way in (the page never calls the engine's modules):

- `xy`: the state, e.g. `xy: hold off, idle 0 0, X1 RYTM:PERF1 +100, Y2 MORPH -50`
- `xy=pos:X:Y` (0..16383 each): a touch or a drag. The page sends it at most every 30 ms while a finger drags; a move
  held back goes out once its 30 ms are up, also while the finger rests, and the release sends the last position. No
  reply.
- `xy=up`: the finger lifted or the press was lost. No reply. If the engine still says touching with no finger on the
  page (an `xy=up` lost in a full console queue), the page sends `xy=up` again after 200 ms, then every 500 ms.

The display does not log `xy=pos`, `xy=up` (nor the MORPH slider's `scene=fader:`), about 33 lines a second during a
drag.
- `xy=hold:0|1`
- `xy=set:x|y:SLOT:TARGET:DEPTH` (SLOT 1..4, TARGET a target name or `MORPH`, any case, DEPTH -100..100)
- `xy=clear:x|y:SLOT`

## Saved per kit

As the scenes: `/.rytm/xy/<project>/kNNN.txt` per kit (001..128), `/.rytm/xy/<project>.txt` the project's default (a
kit without its own file; the setup while the Rytm has named no kit). Written a second after the last edit (a position
is no edit), read from the RAM store. Part of the kit's SAVE / RELOAD KIT, project bundles, renames and slot copies.

```
#sidecar xy 1
H 1                  HOLD on
X 1 RYTM:PERF1 100   X's slot 1: PERF macro 1 at +100 %
Y 2 MORPH -50        Y's slot 2: the morph fader at -50 %
```

Blank lines and `#` comments (indented too) are skipped. A line this build cannot read (an unknown keyword or target,
a slot above 4, a depth out of range or malformed) locks the file:
what it can read is loaded, the file is not written again this session, and `xy` ends with `XY LOCKED (NEWER FILE)`.

## Not in 0.5.0

Web or Rytm-side setup, MIDI out of the pad, recording pad motion.

## Tests

`core/tests/test_xypad.cpp` (depths, inversion, sums with an LFO, value-only and RYTM targets, MORPH, HOLD, the release,
kit changes while touching, the commands, the files and the lock), the golden file `core/tests/golden/xy/v1/`,
`pi/engine/tests/test_xypad_pi.cpp` (the view the UI gets), the host screenshots `pi/ui/host` (`xy-*`: a drag that must
end in `xy=up`, HOLD, SETUP, the picker). **[open]** the touch feel and the 30 ms throttle on a real screen, real
Rytm targets.

Code: `core/mod/xypad.{h,cpp}`, `pi/engine/xypad_pi.{h,cpp}`, `pi/ui/sidecar_xy.{h,c}`.
