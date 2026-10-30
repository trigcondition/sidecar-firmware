# LFOs on the Sidecar (`core/lfo.*`)

Extra LFOs that run on the Sidecar: 8 on the Pi (and the Mac), 4 on the Sidecar Mini. Each one modulates one target,
set on the Rytm's remote LFO page (UP + LFO, or the SIDECAR MENU's LFO row; TRK + LFO still works), on the Pi's web page (LFO tab) or on the Mini's
console page (LFO tab). Settings are saved per kit, as the Rytm's own LFOs are (1026).

## Targets

| target | what moves | how |
|---|---|---|
| **V1..V8** | a Sidecar voice's P1..P16 (the machine's own names: HRM, CUT, STA, ...; user machines too) | in the engine, on top of the Rytm's value (its knob, a p-lock): `x.p[k] = base + depth x wave`, clamped to 0..127.99. The base is the Rytm's last report (0x03 / 0x04), so p-locks and knob moves still count. A built-in machine reads the value at its next 12-frame block (Plaits interpolates within the block); a user machine gets a parameter call at most every 5 ms. |
| **FX1, FX2** | the send FX's P2..P5 (HALO, CHORUS, ...; names from the TYPE's labels) | `fx::control.knob` around the kit's value (0x20); 7-bit steps. A structure knob (RINGS P3, CLOUDSEED P2) refades when the LFO crosses a zone. |

Selectors the engine reads only at a report or a trig (6-OP FM's PAT, DSP SAMPLE's SMP) do not follow an LFO.

**No Rytm targets (1026).** The first version also had the Rytm's own sound parameters (BD..CB, FX) as targets, sent as
MIDI CCs. A CC sets the Rytm's stored value, so an LFO there changed the instrument's state (a kit saved while one ran
kept the modulated value): removed from the code, the page, the web UIs and the target names (an old file's line on
BD..CB / FX is skipped). A Rytm-side modulation path over SysEx may come later.

## The engine (control rate)

`lfo::tick(us)` runs from the platform's control loop (the Pi's core 3 `bridge::tick`, the Teensy's `loop()`), where
`engine_sysex` runs, between blocks; it works once a millisecond at most: every LFO's phase and wave, the targets'
sums (two LFOs on one parameter add), the values written where they changed (a voice by more than 1/256 of a step).
`voice_params_changed(vi)` then makes the machine take them as a 0x03 would, without the report's selections.

- **Waves:** SINE, TRI, SAW+, SAW-, SQR, S&H (a new value each cycle), RND (smooth random: one value to the next across
  the cycle) and **DESIGN**: 16 steps drawn by the user, each a value (-64..63) and a shape - HOLD (the value until the
  next step), RAMP (a line to the next step's value), SMTH (an S-curve to it). 2 bytes a step, 32 bytes an LFO.
  Presets: RAMP, TRI, STEPS, RND.
- **Rate:** free, 0.02..20 Hz (`0.02 x 1000^(rate / 127)`), or synced to the Rytm's MIDI clock: 1/32, 1/16T, 1/16,
  1/8T, 1/16D, 1/8, 1/4T, 1/8D, 1/4, 1/2T, 1/4D, 1/2, 1BART, 1/2D, 1BAR, 1BARD, 2BAR, 4BAR, 8BAR. While the Rytm plays
  the phase is the song position (24 ppqn, F8 / FA / FB / FC / F2 through `fx::clock_in`), interpolated between ticks at
  the measured tempo; stopped, or no tick for 250 ms, it runs on at the last tempo (120 BPM before any).
- **Retrig:** FREE; TRIG - the target's trig: a voice's 0x04; for FX any trig (a voice's, or a Rytm track's note);
  PLAY - the Rytm's Start.
- **Depth:** -64..63 parameter steps around the base (negative inverts the wave).

## The remote LFO page (`core/ui/page_lfo.cpp`)

First of the pages drawn by code. ux1027: off the LEFT / RIGHT ring - UP + LFO (or the SIDECAR MENU's row) opens it,
it replaces the Rytm's screen with no page dots; LEFT / RIGHT step the LFO (the design view: the step), as A; NO (from
the design view: the main view first) or UP + LFO again leaves, back where it came from; leaving drops the design view.
Catches only its knobs (all eight) and the stock page keys every page takes.

- **Main view:** the wave over one cycle with a dot at the phase (moved at most every 50 ms: a few tiles a frame), then
  A **LFO** (1..N), B **TGT** (OFF, V1..V8, FX1, FX2), C **PAR** (by name), D **DEP**, E **WAVE**, F **RATE** (FUNC:
  x8), G **SYNC** (FREE / CLK), H **TRIG** (FREE / TRIG / PLAY). **YES** opens the design view.
- **Design view:** the 16 steps' curve with the playhead (when the wave is DESIGN), the selected step marked. The
  trig keys **T1..T16** select a step (caught only in this view), or A; B the value (FUNC: x8), C the shape, D a preset
  (applied on the turn). YES or NO back to the main view.

## Control and storage

- **Console** (both boards, and the web pages' commands): `lfo` (a summary line; the Mini's web console also gets
  `@{"t":"lfo","lfo":{...}}`), `lfo=N:KEY:VALUE` with KEY `tgt` (off v1..v8 fx1 fx2), `par`, `wav` (0..7),
  `rate`, `sync` (0/1), `trig` (0..2), `dep` (-64..63), `st:I:V:S` (one design step), `des:<48 hex>` (per step
  the value + 64 in two hex digits and the shape in one), `pre:ramp|tri|steps|rnd`, `off`. The Pi's `/status` carries
  `"lfo":{"n":8,"l":[{tgt,par,pn,np,wav,rate,rn,sync,trig,dep,base,des,ph}, ...]}` (`base`: unused since 1026).
- **Web:** `web/lfo.js` is both pages' LFO tab (the Mini's module; the Pi's `www/index.html` gets it, with `web/seg.js`,
  in place of its `/*@include ...@*/` lines - `build.sh` and `tests/mock_web.py`): per LFO the target and parameter, the
  wave / sync / retrig choice groups (seg.js Pending: pending until the device shows it), rate and depth sliders,
  and the design drawn with the mouse or touch (drag over the steps with the brush's shape) plus the presets.
- **Card, per kit (1026):** `/.rytm/lfo/<project>/kNNN.txt` (kit 1..128; the pool's safe name for the project), a line
  per LFO `N TARGET PARAM WAVE RATE SYNC RETRIG DEPTH BASE DESIGN`, written by `idle_save()` a second after the last
  change, loaded at boot, with each project (0x10) and when the Rytm names another active kit; the old kit's unsaved
  edits go to its own file first. The Rytm names its kit with `F0 7D 52 54 3B 01 <kit 0..127> F7` (provisional,
  rytm-mods 0026 in rui1026: after each hello and when the active kit changes; `lfo::kit_changed`). **Migration:** a kit
  without its own file starts from the project's `/.rytm/lfo/<project>.txt` (the pre-1026 per-project file, left as
  it is); a Rytm build that never names its kit keeps using that per-project file. `/status` and `:lfo` carry
  `"kit":K` (1..128, 0 unknown).

## Cost

Teensy (stereo release build, `RMP_NO_UPLOAD=1 teensy/host_stereo.sh`, against b81dca5): ITCM +0 B (headroom 10776 B:
no libm or newlib parsers in the LFO code, `fx::clock_in` cold), RAM1 variables +256 B (locals 41568 -> 41312 B, floor
40 KB), RAM2 +32 B (heap 8704 -> 8672 B), flash code +13 KB. State: 41 B of settings and ~28 B of phase a LFO, twice as
many target slots as LFOs (~24 B each).

## Tests

- `core/tests/test_lfo.cpp` (`make -C core test`): the shapes and DESIGN's steps, free rates, sync to a MIDI clock with a
  tempo change, stop and start and SPP, retrig on a trig / a note / PLAY, depth and clamping around a voice's base,
  the base restored, FX knobs, no Rytm target (BD refused, never a CC, a Rytm note retrigs an FX target), the console
  and design hex, the JSON, the per-kit files (3B: the project's file as a new kit's default, kNNN.txt written, the
  old kit's unsaved edits saved on the switch, another project on the same kit), the
  per-project file, the page through the remote host (its masks, T5 and B in the design view, NO back).
- `sim/rui_e2e.py lfo` (rytm-emu): the page on the emulated Rytm (masks, D, YES / T5 / B / NO, the panel equals the
  frame), not a single CC to the Rytm. `sim/rui_render.py`: the page's views as PNG (`lfo_*.png`).
- `node web/lfo.test.mjs` (the tables against `core/lfo.cpp`, no BD..CB target, the design's hex and curve, drawing, the mock device),
  `node --test pi/engine/tests/page_lfo.test.mjs` (the includes, the inlined code).
