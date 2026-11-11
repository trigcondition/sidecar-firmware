# Master FX

One stereo chain after the Rytm's own FX, on the POST return (Pi; the Mac for tests; not the Sidecar Mini).

Tags: **[measured]** run on the Mac or read from a real Rytm's `stereo=measure`; **[code]**; **[inferred]** not run.

## What it can and cannot process

The Sidecar can add to the Rytm's output. It cannot take anything out of it: Overbridge OUT 0/1 and 2/3 are added into the analog
mix. MAIN on Overbridge IN is the finished analog mix plus what the Sidecar returned [measured: POST and PRE are both in it], so
running it through a chain and sending it back is a feedback loop. So there are two modes, and one switch, **SRC**, saved per kit.

| | **SRC RET** (default) | **SRC MIX** (through mode) |
|---|---|---|
| needs | nothing | the tracks to process off ROUTE TO MAIN on the Rytm (GLOBAL > AUDIO ROUTING, or the kit's TRACK ROUTING). A voice left on main stays analog and out of the chain |
| in the chain | voices on PRE and POST, the send FX returns, bus 3, the computer's OUT, `out=main`'s sum | all of that, and the stems off main: every such Rytm voice and TRK voice, after its track's filter, amp and drive, panned as on the Rytm |
| not in the chain | the Rytm's analog mix; voices on TRK | the Rytm's delay and reverb (SEND TO FX is its own page: they stay on, in MAIN, outside the chain) |
| Rytm's distortion and compressor | as ever on the analog mix; 2/3 leaves them while a chain runs | only process the Rytm's FX returns routed PRE: not the voices |
| latency | +3 ms on what goes through the chain (2 ms the way back, 1 ms the limiter); bypassed, none | analog tracks about 5.9 ms later (282 frames [measured: the return path]), +1 ms the limiter, +2 ms with a chain on |

Bypass in SRC MIX keeps the mix: the stems stay on 0/1, with the limiter, and only the chain leaves. So does an empty chain
(no TYPE in any slot, the POST EQ flat): SRC MIX alone sums the stems off main [code, `test_masterfx_pi` src_mix_empty_chain].

The Rytm's own controls in through mode:

| | where it acts | what the Sidecar does |
|---|---|---|
| filter, amp, overdrive, track LEVEL, VOL, velocity, accent | before the stem | nothing: it is in the stem |
| MUTE | stops trigs, before the stem (a tail rings, as on a stock Rytm) | nothing |
| PAN | after the stem | reads it from the Rytm's report 50, p-locks and LFOs in, and pans the stem by the Rytm's law (L + R constant, linear), within 50 ms |
| ROUTE TO MAIN | after the stem | the same report: a stem still on main stays out of the sum, so nothing doubles |
| delay, reverb (SEND TO FX) | fed from the voices | nothing: they stay in MAIN. They cannot join: MAIN holds the Sidecar's own return, so summing it would feed back |
| master distortion, compressor | analog, after the main bus | they now hear only the Rytm's FX returns routed PRE. The chain's COMP is the bus compressor |

Report 50 needs a Rytm with this 0.4.0 0030-fx-in. On older mods every stem is summed, centred: mono, and all tracks must be taken off main by
hand or they double. The page says which.

Take playback (and the looper, later) comes after the chain: it is not processed twice. The recorder's source 24 POST stays the
voices' pair before the chain. MASTER (IN 0/1) holds the chain's output, the Rytm's mix around it.

## Levels

The returns are not unity [measured on a real Rytm, master FX neutral: POST -10.9 dB, PRE -2.0 dB into MAIN IN]. What was on 0/1 stays
at 0 dB, going through the chain or not. What moves onto 0/1 is raised to keep its place in the mix: 2/3 by +8.9 dB
(`kPreOnPost`), the stems by +10.9 dB (`kStemOnPost`, taking a stem to MAIN as unity [inferred]), MT/HT and CH/OH 0.85 dB less
(they read hot against what they put in MAIN [measured on a Rytm, 2026-10]). Constants, from one run; `stereo=measure` checks them
and always measures the plain return: the chain, the stems and the limiter stand aside while it runs.

**STEM TRIM** comes before the sum: 0 to -24 dB, default -6, smoothed, for every stem alike. POST's full scale is -10.9 dB on the main,
so at the levels above a loud kit asks 0/1 for far more than it has [measured on a Rytm: 8 x full scale, 18 dB over, with no trim]. The
trim sets how much of that the limiter has to take and how hot the chain is driven: the stems reach the chain unclipped. Its
default leaves through mode 6 dB under the analog level of a stem that fits.

## The limiter

The last thing on OUT 0/1 (`core/fx/limiter.h`, core 0, the OUT fill): a stereo-linked lookahead peak limiter. One gain on both sides, so the
image holds; 1 ms of lookahead (48 frames), the gain a straight line down that arrives before the peak leaves the delay; release a one-pole of
100 ms; ceiling 0.98 of 0/1 [measured on the host: nothing over it, 36 dB over included]. Below the ceiling a signal comes out unchanged, later.

- **When:** SRC MIX, or a chain playing (not bypassed). With SRC RET and nothing set, or bypassed in SRC RET, it is out and not a
  sample is touched. `stereo=measure`: out.
- **The stems and the chain are not clipped before it,** so their sum can stand 18 dB over and come out whole. What is already on 0/1 before
  the stems (the POST voices, the computer's OUT, the POST returns) is summed as 32-bit integers and saturates at full scale first: a hot voice
  plus a reverb return can flat-top, and the limiter then only scales that down. The chain's output has a bound of its own, far above it (+24 dB, linear to 70 %), there only so nothing runs away: with SRC
  RET the chain is protected by this same limiter, which comes in with it.
- **Fades:** the limiter comes in first (its delay filled, then 5 ms), then the stems and the chain (10 ms); they leave first, then it. So
  nothing over full scale ever passes on the direct side of its crossfade. That crossfade is the plain signal against itself 1 ms later:
  the one place the 1 ms shows, a short comb, no step.
- **Timing:** it adds 48 frames to all of 0/1 while it runs, the same on both sides of the chain's crossfade, so that crossfade is as it was.
  2/3 and the track channels keep their timing: with no chain playing, voices on PRE lead voices on POST by 1 ms. Take playback
  comes after it: not limited, and without the 1 ms.
- **Reading:** `gr` in `/status` (dB, the deepest of the last second or so, per block's lowest gain), the same in `mfx=stats`, on the web page.
  0: out, or nothing to do.

## The chain

Four slots in a row, then a POST EQ, then LEV.

- **One FX code path.** Slots 1, 2 are chain M1 of `ins::Engine`, slots 3, 4 chain M2, with its POST EQ. Every INSERT TYPE works, SAT among them (docs/INSERT-FX.md): 15 frames each, tape on the mix is the use it was built for.
- **AMP**: its own cap, 2 instances, outside the voices' 6. One instance on (L + R) / 2 on both sides, or an L/R pair (two instances,
  each side its own). `MAX 2` past that.
- **Send FX types as inserts**: CHORUS, COMB, SPACE, RINGS, CLOUD, HALO: through `SendFx::Insert`, the same
  fades, zones and clears, the same output [measured: under 1e-4 of the send FX's own return after a settling time].
  0.5.0: the buses B1..B3 take them too (docs/INSERT-FX.md), and the room is shared: **4 at a time, the master and the buses
  together**, on two insert `fx::SendFx` of two slots each (the send FX's own is apart). A fifth is refused and the slot keeps
  what it has; the console says why (`mfx: RINGS: 4 send FX inserts at most, the master and the buses together (4 in use)`),
  the Rytm's F TYPE passes them over with `SEND FX INSERTS: 4 IN USE` in the footer, the web pages grey them out with a line.
  Mono in, as on the buses. A MIX knob each (127: only the return). CRUSH and RINGMOD exist as INSERT types.
- **STUTTER** is not a master type: its ring is mono; idle, it would fold the chain to mono.
- **BYPASS** crossfades over 10 ms. The chain keeps running, so tails stay and re-engaging is instant; its core time stays too.
  Bypassed and faded, the 12 channels are bit for bit what they were [measured]. Every slot off: no work on any core.

| key | what | range |
|---|---|---|
| SRC | RET / MIX | per kit |
| BYP | on / bypass | per kit |
| LEV | the chain's output | 0..127, 100 unity |
| TRIM | every stem, before the sum (SRC MIX) | 0..-24 dB, default -6, per kit |
| TYPE, P1..P4 | as in docs/INSERT-FX.md; the send types' P2..P5 | |
| MIX | a send type's dry / wet | 0..127 |
| POST EQ | HPF LPF LOW HIGH | |

No p-locks. LFO targets (0.5.0, core/lfo.h): `M1`, `M2` for the slots (params as an INSERT chain's: A P1..P4, B P1..P4,
the POST EQ), `MFX` for LEV and MIX1..4, and (0.5.0) B1A..B3B: the bus slots' send TYPE MIX (docs/INSERT-FX.md).

## Where to set it

- **Rytm**: page MASTER, host-drawn, so it takes no Rytm code space: on the LEFT / RIGHT ring and in the SIDECAR MENU; no UP key (T1..T13 are
  taken, a new key is a Rytm mod). A..D P1..P4, E SLOT, F TYPE, G MIX or MODE, H SRC, YES bypass, RIGHT the EQ view (H LEV there).
- **Touchscreen**: Settings > Send FX > Master. Every size. Tap a TYPE for the next, ON, SRC, the AMP's mode; the knobs are shown, set from the Rytm or the web.
- **Web**: the Send FX tab; the STEM TRIM and the limiter's reading only here and on the console.
- **Console**: `mfx`, `mfx=byp:0|1`, `mfx=src:ret|mix`, `mfx=lev:V`, `mfx=trim:DB` (0 or -1..-24), `mfx=S:type:T`, `mfx=S:p1..p4:V`, `mfx=S:mix:V`, `mfx=S:pair:0|1`,
  `mfx=S:off` (S 1..4), `mfx=eq:hpf|lpf|low|high:V`, `mfx=eq:off`, `mfx=reset`, `mfx=stats` (core 2's load, underruns, the limiter's reduction).

## Saved per kit

In the INSERT file (docs/INSERT-FX.md): `M1 A COMP 50 60 40 20`, `AMP M2 A file`, `PEQ M2 40 0 64 70`, and only off its defaults
`MFX BYP SRC LEV MIX1 MIX2 MIX3 MIX4 PAIRS [TRIM]` (`PAIRS`: a bit a slot; `TRIM` in dB, left out at -6: an older line loads at the default). A kit without them plays and saves byte for byte as before [measured].
A kit without its own file plays the project's default (`/.rytm/ins/<project>.txt`), its MFX line and SRC with it; a
Sidecar bundle, the Rytm's SAVE KIT to another kit and RELOAD KIT carry the line too (core/bundle/kits.h).
An older Sidecar does not read these lines: it keeps such a file as it is and plays the kit without a chain.

## Signal path, threads

```
core 0, FillOut: Pull -> computer OUT -> AddReturn -> masterfx_pi::Process -> take playback -> taps
                                                       the stems (MIX), 0/1 + 2/3 to core 2, wet back on 0/1, the crossfade, the limiter
core 0, IN completion: masterfx_pi::Stems: each stem x its L, R gains (report 50 and the trim, ramped at most 10 ms) into a stereo queue
core 2, after its block and the send FX: masterfx_pi::Render -> core::master::render
core 3: settings, /NAM/ loads as for voices (no card on it, cardq)
```

The added latency is the way back: `kPrime` 96 frames, the send FX's margin against core 3's longest pass, plus a fill: 98 frames
(2.04 ms) at the default 7-frame URB [measured on the host]. The limiter, after that, adds 48 (1 ms) to whatever is on 0/1: 146 frames
(3.04 ms) through the chain [measured on the host]. The effects add none. The direct path, with the limiter out, adds none.

## Cost

See `test_master`, `bench mfx` in the boot log (a chain without AMP; add 2 x `bench amp` for a pair). Memory: +11.8 MB (0.5.0: the two insert SendFx, shared with the buses; 5.9 MB before),
+280 KB (2 AMP engines), +128 KB (DELAY lines). The limiter runs on core 0: about 5 ns a frame on a Mac [measured], 40-60 cycles a frame on
a Pi [inferred], under 0.3 % of core 0 at the worst; 384 B of delay.

## Tests

`core/tests/test_master.cpp` (TYPEs and refusals, against the send FX, noise, AMP cap and pair, per kit, old kits, console, the Rytm page,
control core, cost), `pi/engine/tests/test_masterfx_pi.cpp` (bit for bit, latency, clicks, PRE moved, a stalled core, SRC MIX,
`stereo=measure`, report 50, the limiter alone and on 0/1), `pi/engine/tests/page_mfx.test.mjs`, `pi/ui/host` shots `mfx-ret`, `mfx-mix`, `mfx-through`; on the Rytm's side sidecar-mods
`tools/emu/fxin_emu.py TAG mix`.
