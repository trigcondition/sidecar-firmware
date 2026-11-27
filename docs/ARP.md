# The arpeggiator

A track's held or sequenced chord played as a pattern, in time with the Rytm. Any Sidecar machine, polyphonic or not, and the MIDI machine. Backlog 7ae. Code: `core/arp/arp.h`
(read its head first), `core/ui/page_arp.cpp`, rytm-mods 0021 and 0026 ("7ae"). Design and the owner's decisions: kept outside the repo.

## On the Rytm

**UP + FLTR** opens the ARP page of the selected track (also in the SIDECAR MENU). Settings are per sound: saved with the sound and the kit, carried by the pool and +Drive, power-off too. An old sound
reads OFF.

| knob | name | values |
|---|---|---|
| A | MODE | OFF, UP, DOWN, UPDN, RND, PLAY (as played) |
| B | SPD | 1/4, 1/8, 1/8T, 1/16 (default), 1/16T, 1/32 |
| C | RNG | 1, 2, 3 octaves |
| D | GATE | 25, 50 (default), 100 % (legato) |
| E | SWNG | 50 (straight), 58, 66 %: every second note late |

What it plays: a Rytm trig (until its LEN or the pad's release; its CHD chord on 6-OP FM, MULTISAMPLE, GRANULAR; a WAVETABLE trig is one note, its P13..P16 are TBL B, POS B, MIX and FTYPE), the Sidecar sequencer's poly notes (whatever a step holds, until their own LEN), the pads
in CHRO, keyboards on the Pi. With the mode on, keyboards and pads reach any Sidecar machine, not just the polyphonic ones. One note sounds at a time. On a poly track it plays with that track's POLY CONFIG;
elsewhere on the voice's own machine, mono. With MODE OFF nothing changes.

On a Sidecar seq track a step can lock any of the five for that step only: hold the step, press FLTR (`docs/SEQUENCER.md` "Arp locks"). Not on a plain Rytm track.

An ARP track is pinned (its amp held open, as a seq track's): the notes are the host's, the Rytm's envelope does not gate each one, and the amp and filter envelopes do not shape them. Not on the MIDI machine.

## Time

The grid is the Rytm's: the sequencer's step clock (`seq::beat_map`, BD's step ticks when no seq track runs), `latency` early as the sequencer's notes. A chord that starts just after a grid point plays at once (a trig always does); else at the next point.
Stopped or paused: free running from the first note, at the last tempo. Keys and pads that are down when STOP comes run on; a trig's and the sequencer's notes end.

## Ending

Held notes gone: the note plays out its gate. A hold that ended before its first grid point (a short trig, a slow SPD) plays its first note at the release. STOP, another kit, a machine change, MODE OFF, a voice's silence, a link hello, the Rytm unplugged:
nothing is left playing. A new kit's first trig is spared: the Rytm reports the kit after it. Opening a page changes nothing.

## Wire

`45 01 <track> <lo> <hi>`, both ways, in `sim/protocol.py`. The Rytm stores 10 bits and never reads them.

Console: `arp`, `arp=T:mode=1,spd=3,rng=1,gate=1,swng=0`; a step's locks `seq=alock:T:S:F:V`. Tests: `core/tests/test_arp.cpp`, `sim/arp_e2e.py`, rytm-mods `tools/emu/ar_emu.py`.
