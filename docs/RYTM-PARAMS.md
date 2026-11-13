# The Rytm's own sound parameters, modulated from the Sidecar (0.5.0)

From 0.5.0 the Sidecar's LFOs, the XY pad, MIDI learn, the MORPH fader and the sequencer's p-locks on poly tracks can
move the Analog Rytm MKII's **own** sound parameters: a track's filter, amp, LFO, synth and sample page, its trig
velocity and VELOCITY MOD / AFTERTOUCH amounts, and the FX track's delay, reverb, distortion and compressor.

It needs the Rytm firmware mod 0.5 (the hello's caps bit 64). On an older Rytm the targets stay in the files, but
nothing is sent and the pages show them as unavailable (`rp` says "needs Rytm mod 0.5").

**The kit is never written.** The Sidecar sends an offset and an override for each parameter. The Rytm adds them in its
audio interrupt after its own modulation (its LFO, velocity, PERF and scene). A kit saved while an LFO runs keeps the
knob's value, not the modulated one. When the Sidecar stops sending (USB unplugged, the Pi off), the Rytm lets every
parameter go back to its own value over about 20 ms, 250 ms after the last message.

## Targets

| name | what | how |
|---|---|---|
| **RT1..RT12:** `SPD FAD SPH DEP` | the track's LFO (speed, fade, start phase, depth) | an offset in steps |
| **RT1..RT12:** `P1..P8` | the synth page, shown by the track's machine's own labels (all 53 machines, read from the Rytm's ROM: `TUN`, `DEC`, ...; the "2" machines have their own order, e.g. BD HARD 2's P1 is `TUN`). An empty slot `--` is not sent; DISABLE, BT IMPROVED and FUNC. GEN. have none | an offset |
| **RT1..RT12:** `TUN FIN BR STA END LEV` | the sample page | an offset |
| **RT1..RT12:** `FENV FATK FDEC FSUS FREL FRQ RES` | the filter page | an offset |
| **RT1..RT12:** `ATK HLD DEC OVR VOL PAN ACC DEL REV` | the amp page | an offset |
| **RT1..RT12:** `VEL` | the trig's velocity (the note's own; what velocity routes see). Trigs the Rytm's sequencer plays only: pads and live play keep theirs | an offset |
| **RT1..RT12:** `VM1..VM4`, `AT1..AT4` | the VELOCITY MOD and AFTERTOUCH route amounts | an offset in displayed amount steps (+64 is the stock +64) |
| **RT1..RT12:** `MUL WAV MOD SMP LOP FTYP` | list words (LFO multiplier, wave, mode; sample slot, loop; filter type) | whole values only: the morph and a p-lock, never an LFO or the pad |
| **RFX:** `TIM X WID FDB DHPF DLPF DREV DVOL DOV` | the delay | an offset |
| **RFX:** `PRE RDEC RFRQ GAI RHPF RLPF RVOL` | the reverb | an offset |
| **RFX:** `AMT SYM` | the distortion | an offset |
| **RFX:** `THR MUP MIX CVOL` | the compressor (threshold, makeup, dry/wet, volume) | an offset |
| **RFX:** `CATK CREL RAT SEQ` | the compressor's lists (attack, release, ratio, sidechain EQ) | whole values only: the morph |
| **RFX:** `SPD FAD DEP` | the FX track's LFO | an offset |

Not offered: the LFO's destination (`DST`). On a track playing a
Sidecar machine, the synth and sample words are the machine's P1..P16 and are not sent as Rytm targets: use `V1..V8`
for those.

Where to find them: the LFO page's TGT list has one entry per Rytm track (BD .. CB), whose PAR list is the Sidecar voice's
params (when the track plays a Sidecar machine) and then the Rytm's own; RFX is the last target ([lfo.md](lfo.md)). The XY /
MORPH picker's TRACK kind lists them the same way.

The names are saved as written (`RT3:FRQ`, `RFX:TIM`) in the Sidecar's own files only (LFO, XY, scenes, `.sqp`). A
file with an RT target locks that file on an older Sidecar build, as any line the build cannot read does.

## How each source acts

- **LFO, XY pad, MIDI learn** add an **offset** to the Rytm's value. Two sources on one word add up. The Rytm
  clamps the sum to the parameter's range. The LFO's depth is in steps, as on the Sidecar's own targets.
- **The MORPH fader** (scenes A and B) sends an **override**: a value and a mix. With a lock in both A and B, the value
  goes from A to B and the mix is 1. With a lock in A only, the value is A's and the mix fades out from 1 to 0 as the
  fader goes to B. The Rytm's own value plays under the rest. B only works the other way round. A list word switches
  at halfway.
- **A p-lock on a poly track** (the sequencer's FLTR page F..H `FRQ RES ENV`, the AMP page, the LFO page) sends an
  override at the step's time: the value, mix 1, at once (no ramp). It holds until the track's next trig without that
  lock, a failed lock condition, or STOP. The override goes out about 2 ms after the step's event is applied, so it
  lands with the note on the Rytm (`rp=lead:US` tunes it). Under a one-sided morph, the p-lock is the other side's
  value. On a pinned track, the envelope words stay with the Sidecar's own
  envelope and are refused as locks: `FENV FATK FDEC FSUS FREL` (the FLTR page's ENV among them) and `ATK HLD DEC`.
- **A Rytm scene lock** on the same word beats a Sidecar override, as the Rytm's own scenes do. Offsets still add on top.

## Traffic

Messages go at most every 5 ms and carry only the parameters that changed (an offset by 1/16 step or more). Every live
parameter is sent again every 100 ms; this keeps the Rytm's 250 ms timeout from firing. The whole feature stays under
16 KB/s on the USB link on average. Above that the 5 ms grid stretches. Offsets and overrides take turns, so a busy
set of LFOs never holds back a morph or its release (measured: a release within 7 ms with 40 offsets moving every ms;
the bound tested is 20 ms). A release goes before any other override.

Each record ramps on the Rytm over its own update interval: the grid's, longer when the cap stretches it or when more
parameters change than one message holds, and a slow LFO's own pace (one that moves 1/16 step every 30 ms ramps over
30 ms). So a moving value is a line, not a staircase. A return to 0 ramps at the grid's pace. The wire carries one
ramp per message: parameters of different paces in one message share the shortest one (a slow LFO next to a fast one
still steps a little), and when offsets and overrides both fill the cap, each ramps over its own wait, about half
the real gap.

A p-lock's override goes at its step's time, outside the grid. Its bytes still count: the debt (at most 100 ms of the
cap) holds the next grid sends, so the average stays under the cap. The Rytm holds 128 live parameters, and so does
the Sidecar's table (inactive ones count too). More are refused and counted (`full`, `dropped`).

A kit change, a project change or a reset of the modulation (the layer's restore) clears every track on the Rytm.
After a kit change, nothing but the CLEAR goes out until the sources have switched to the new kit (the XY pad and the
scenes release, the LFOs reload), so no old kit's value plays on the new one.

## Console

| command | answer |
|---|---|
| `rp` | the state: on / no link / needs Rytm mod 0.5, the kit, live parameters, bytes a second, what was sent, refusals, the lead, and the Rytm's last status |
| `rp=list` | every live parameter: its offset, override, mix, lock, and whether it is inactive |
| `rp=status` | asks the Rytm for its status (slots, drops, timeouts); `rp` shows it |
| `rp=clear` | clears every track on the Rytm now and forgets every entry; a source sends again when it moves (an XY pad held still or a stopped LFO stays at 0 on the Rytm until then) |
| `rp=lead:US` | the p-lock lead in microseconds (default 2000) |

The web status (`/status` JSON) carries `"rp":{"on","live","bps","refused","full","bad","sent"}`. `bad` counts messages
the Sidecar refused to send because a byte was out of range (a bug: it should stay 0); `rp` shows it too.

## The wire (host -> Rytm)

`F0 7D 52 54 5A 01 <op> ... F7`: 04 OFFSET, 05 OVERRIDE, 06 CLEAR, 07 STATUS (answered `2D 01 11`); a refusal comes back
as `2D 01 10 <op> <1 refused, 2 full>` and never switches scenes, PERF or mutes to their CC fallback. Bytes and fields:
`core/mod/rytm_param.h`, `sim/protocol.py` (`pm_offset`, `pm_override`, `pm_clear`, `pm_status`), and the Rytm side's
arithmetic as a model: `sim/pm_model.py`.

## Tests

`core/tests/test_rytm_param.cpp` runs against a fake Rytm (`core/tests/pm_fake.h`, the same arithmetic as
`sim/pm_model.py`). It covers the names, the bytes, the sender's policy (grid, refresh, cap, caps 64, hello, kit,
link), an LFO, the XY pad and learn, the exclusions, refusals, the morph's parts and the velocity words; and the
review's cases: overrides with offsets saturating the cap (a release in 20 ms at most), the sender's cost, a kit change
with the XY pad and an LFO live, p-lock bursts against the cap, the mix clamp, a lock pending across a long link loss,
the ramp per record, `rp=clear`.
`core/tests/test_seq.cpp` covers the poly p-locks (the jump a lead after the step, the release at the next trig and at
STOP). The LFO, XY and scene file tests cover the names in the files. `sim/tests/test_pm_model.py` covers the model.

**[open, hardware only]**
- How the 5 ms grid sounds on FRQ, RES, VOL and PAN, and clicks on a jump.
- Which parameters move a ringing note.
- p-lock timing and tuning the lead.
- The Rytm interrupt's headroom.
- USB pacing next to audio.
- How velocity feels.
- A SAVE KIT while modulation runs.
