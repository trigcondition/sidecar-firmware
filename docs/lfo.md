# LFOs on the Sidecar (`core/lfo.*`)

Extra LFOs that run on the Sidecar: 26 on the Pi (and the Mac), two a Rytm track (0.5.0), 4 on the Sidecar Mini. Each
one modulates one target,
set on the Rytm's remote LFO page (UP + LFO, or the SIDECAR MENU's LFO row; TRK + LFO still works), on the Pi's web page (LFO tab) or on the Mini's
console page (LFO tab). Settings are saved per kit, as the Rytm's own LFOs are (1026).

## Two a track (0.5.0, the owner's LFO FINAL of 2026-11-12)

- **26 on the Pi:** T1..T12 and the FX track have an **A** and a **B** each (`core/lfo.h` `track_of` / `slot_of`: LFO i is
  track i / 2's A for an even i, its B for an odd one). Names: `T1A` .. `T12B`, `FXA`, `FXB` (the console, the file).
- **A** may target anything, the Rytm's own parameters (RT1..RT12, RFX) included. **B** targets the Sidecar's own only
  (voices, inserts, the master, the send FX, the sends, the OUT routes): it never uses the Rytm link. `lfo::set`, the
  console (`cannot`), the page's TGT list (it ends before RT1), the web's target lists and a file's line (not read: the
  file is kept, 1042) all hold to it.
- **No cap** on the LFOs on the Rytm: 13 A's at 20 Hz take ~12.6 KB/s of the sender's 16 KB/s. When the sender is
  saturated (offsets that changed wait at their grid slot for 100 ms on end: `mod::rp_saturated`, held a second) while an
  LFO is on the Rytm (`lfo::link_full`), the page's footer says **RYTM LINK FULL: SLOWER** (the screen fits 24 characters),
  the touchscreen's banner and the web's LFO tab **RYTM LINK FULL: LFOs update slower** (`/status` `"lfo":{"full":1}`).
  The LFOs then update slower (oldest first: none starves). **Releases and p-locks keep priority:** a p-lock's jump is due
  outside the grid; a release (an override's mix to 0, an offset whose source let go: `rp_release`) goes at its op's next
  grid slot even with the credit spent, first in its message.
- **Old files:** a per-kit (or project) file from before (numbered lines, 8 LFOs) is read as it is: old LFO i becomes
  track i's A, its target kept; the next save writes the 26 named lines (`T1A ...`), which an older build does not read
  (so it never writes over them). The Mini keeps its 4, numbered as before.
- **The Rytm's screen:** UP + LFO shows the selected track's two (the Rytm's 49 1 2). A, LEFT / RIGHT go between A and B;
  FUNC + A steps the track the page shows until the Rytm selects another. With today's 0026, which reports sound tracks
  only (`dp_tick`: `if (t < 0 || t > 11) return;`), the FX track's pair is reached with FUNC + A; a 0026 that sends
  `49 1 2 0C 7F <"FX" in 12 B>` when the FX track is selected makes the page follow it too (`ui::remote_lfo_track`; an
  older host ignores the message: it takes 49 1 2 for tracks below 12 only).
- **The web** (`web/lfo.js`, `web-next`): a track bar (T1..T12, FX) and that track's pair; it follows the Rytm's selected
  track (`"trk"`) when that changes. The touchscreen has no LFO view of its own; it shows the link-full banner.

## Targets

| target | what moves | how |
|---|---|---|
| **V1..V8** | a Sidecar voice's P1..P16 (the machine's own names: HRM, CUT, STA, ...; user machines too) | in the engine, on top of the Rytm's value (its knob, a p-lock): `x.p[k] = base + depth x wave`, clamped to 0..127.99. The base is the Rytm's last report (0x03 / 0x04), so p-locks and knob moves still count. A built-in machine reads the value at its next 12-frame block (Plaits interpolates within the block); a user machine gets a parameter call at most every 5 ms. |
| **FX1, FX2** | the send FX's P2..P5 (HALO, CHORUS, ...; names from the TYPE's labels) | `fx::control.knob` around the kit's value (0x20); 7-bit steps. A structure knob (RINGS P3, CLOUDSEED P2) refades when the LFO crosses a zone. |

| **RT1..RT12, RFX** (0.5.0) | the Rytm's own sound parameters: a track's filter, amp, LFO, synth and sample words, VEL (the Rytm's sequenced trigs only: pads and live play keep theirs), VM1..4 / AT1..4; the FX track's delay, reverb, distortion, compressor | an offset the Rytm adds after its own modulation (it needs the Rytm mod 0.5; the kit is never written): [RYTM-PARAMS.md](RYTM-PARAMS.md). The file keeps the Rytm's word number (`3 RT3 30 ...` for RT3:FRQ) |

**Where the targets are on the page (0.5.0).** The TGT knob lists OFF, then **the 12 Rytm tracks one by one (BD SD RS CP BT
LT MT HT CH OH CY CB, never the voice pairs)**, then FX1, FX2, the inserts, outs, master, MFX, the sends, and RFX last.
Choosing a track gives one PAR list: the Sidecar voice's P1..P16 (by the machine's labels) when the track plays a Sidecar
machine, then the track's own Rytm parameters (FRQ, RES, FENV, ATK ...). A stock analog track lists the Rytm's only. This
is a display order over the same stored targets: the file, the kit and the wire keep `V1..V8` + P and `RT1..RT12` + word
exactly as before, so an old project loads the same targets. Tracks that share a Sidecar voice (RS/CP, MT/HT, CH/OH,
CY/CB) share its params: the voice's P1..P16 are listed under ONE track of the pair, the first that plays a Sidecar machine
(a Rytm that has not reported yet counts as one), and the other track lists its own Rytm params only; a stored V target is
shown under that track. If neither track of the pair plays a Sidecar machine, a stored V target stays listed under the
pair's first track while it is the stored one (stepping PAR into the Rytm params drops it).
The touchscreen's XY / MORPH picker lists the tracks the same way (TRACK, in place of VOICE and RT).

Selectors the engine reads only at a report or a trig (6-OP FM's PAT, DSP SAMPLE's SMP) do not follow an LFO.

**No Rytm targets (1026).** The first version also had the Rytm's own sound parameters (BD..CB, FX) as targets, sent as
MIDI CCs. A CC sets the Rytm's stored value, so an LFO there changed the instrument's state (a kit saved while one ran
kept the modulated value): removed from the code, the page, the web UIs and the target names (an old file's line on
BD..CB / FX is skipped). 0.5.0 brings them back as **RT1..RT12 / RFX** over the Rytm mod's SysEx path, which never
writes the kit ([RYTM-PARAMS.md](RYTM-PARAMS.md)).

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
  A **LFO** (the shown track's A / B, "T3 A"; FUNC: the track; the Mini 1..4), B **TGT** (OFF, V1..V8, FX1, FX2), C **PAR** (by name), D **DEP**, E **WAVE**, F **RATE** (FUNC:
  x8), G **SYNC** (FREE / CLK), H **TRIG** (FREE / TRIG / PLAY). **YES** opens the design view.
- **Design view:** the 16 steps' curve with the playhead (when the wave is DESIGN), the selected step marked. The
  trig keys **T1..T16** select a step (caught only in this view), or A; B the value (FUNC: x8), C the shape, D a preset
  (applied on the turn). YES or NO back to the main view.

## Control and storage

- **Console** (both boards, and the web pages' commands): `lfo` (a summary line; the Mini's web console also gets
  `@{"t":"lfo","lfo":{...}}`), `lfo=N:KEY:VALUE` (N 1..26 or the name, `lfo=T3B:dep:-12`) with KEY `tgt` (off v1..v8 fx1 fx2), `par`, `wav` (0..7),
  `rate`, `sync` (0/1), `trig` (0..2), `dep` (-64..63), `st:I:V:S` (one design step), `des:<48 hex>` (per step
  the value + 64 in two hex digits and the shape in one), `pre:ramp|tri|steps|rnd`, `off`. The Pi's `/status` carries
  `"lfo":{"n":26,"pt":1,"kit":K,"full":0,"trk":T,"l":[{tgt,par,pn,np,wav,rate,rn,sync,trig,dep,base,des,ph,pns}, ...]}` (`base`: unused since 1026;
  `pns`: every parameter's name as `pn` gives it, so the PAR list reads "P2 HRM" on every row).
- **Names:** a parameter is shown by its name, never a bare P number: a voice's by the machine it plays now (its pages,
  its SMPL page, a user machine's manifest, MD's TYPE: `core/ui/page_lfo.cpp` `voice_label`), a send FX's and an
  INSERT slot's by its TYPE. One the machine leaves unused shows `-` (the LFO page: `P9 -`). The same names on the
  MORPH and XY pages (touchscreen and Rytm) and the web's MORPH page (`/status` `scene` `pl` / `vl`).
- **Web:** `web/lfo.js` is both pages' LFO tab (the Mini's module; the Pi's `www/index.html` gets it, with `web/seg.js`,
  in place of its `/*@include ...@*/` lines - `build.sh` and `tests/mock_web.py`): per LFO the target and parameter, the
  wave / sync / retrig choice groups (seg.js Pending: pending until the device shows it), rate and depth sliders,
  and the design drawn with the mouse or touch (drag over the steps with the brush's shape) plus the presets.
- **Card, per kit (1026):** `/.rytm/lfo/<project>/kNNN.txt` (kit 1..128; the pool's safe name for the project), a line
  per LFO `N TARGET PARAM WAVE RATE SYNC RETRIG DEPTH BASE DESIGN`, written by `idle_save()` a second after the last
  change, loaded at boot, with each project (0x10) and when the Rytm names another active kit; the old kit's unsaved
  edits go to its own file first. The Rytm names its kit with `F0 7D 52 54 3B 01 <kit 0..127> F7` (provisional,
  rytm-mods 0026 in rui1026: after each hello and when the active kit changes; `lfo::kit_changed`). A line is
  `NAME TARGET ...` (0.5.0: `T3A`, `FXB`; numbered on the Mini and in the older files, above). **Migration:** a kit
  without its own file starts from the project's `/.rytm/lfo/<project>.txt` (the pre-1026 per-project file, left as
  it is); a Rytm build that never names its kit keeps using that per-project file. `/status` and `:lfo` carry
  `"kit":K` (1..128, 0 unknown).

## Cost

Teensy (stereo release build, `RMP_NO_UPLOAD=1 teensy/host_stereo.sh`, against b81dca5): ITCM +0 B (headroom 10776 B:
no libm or newlib parsers in the LFO code, `fx::clock_in` cold), RAM1 variables +256 B (locals 41568 -> 41312 B, floor
40 KB), RAM2 +32 B (heap 8704 -> 8672 B), flash code +13 KB. State: 41 B of settings and ~28 B of phase a LFO, twice as
many target slots as LFOs (~24 B each).

## Tests

- `core/tests/test_lfo26.cpp`: the 26 (names, console, JSON), A / B (set, console, the page's lists, a file's B line), an
  old file's migration and its save, the page's pair (the selected track, FUNC + A, the FX track by 49 1 2 12), the CPU.
  `core/tests/test_rytm_param.cpp` R10 (the link full: the page's footer, the JSON) and R11 (a morph's release, a
  p-lock, an LFO's release under a saturated sender); `core/tests/small/test_lfo_small.cpp` (the Mini's 4).

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
