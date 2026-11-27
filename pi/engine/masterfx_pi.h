// pi/engine/masterfx_pi.h - the master FX (core/fx/master.h, docs/MASTER-FX.md) in the Pi engine. The chain runs on render
// core 2, after the send FX. Process, in the OUT fill on core 0, is the one place that touches the channels: it sends
// what is on 0/1 and 2/3 to core 2 and adds what comes back on 0/1, crossfading between that and the direct path. Nothing
// is written to the channels while no chain is set, or while it is bypassed and faded: the output is then bit for bit
// what it was without master FX (take playback comes after it, so a take or a looper is not processed twice).
//
// The latency the chain adds on POST is the ring back to core 0, kPrime frames (2 ms: the send FX's margin against core
// 3's longest pass) plus up to one URB's frames; the chain's own effects add none. The direct path adds none. Bypass keeps
// the chain running (tails go on, the core time stays, re-engaging is instant).
//
// The limiter (core/fx/limiter.h) is the last thing on 0/1 while SRC is MIX or the chain plays: the stems and the chain's output
// come to it unclipped; what was on 0/1 before them (POST voices, the OUT, POST returns) is integer and may have saturated. It adds its 48 frames (1 ms)
// to all of 0/1, both sides of the chain's crossfade alike, and comes in before them and leaves after them. 2/3 and 4..11
// keep their timing; take playback, after it, is not limited. Hosted (no Circle headers): ob_usb.cpp and cores.cpp call it.
//
// Two links (phase 3(d), decided 2026-11-14: the boxes work independently and identically): one instance per box (slot),
// each its own rings, crossfades, limiter and stats, its own settings (core::master::control_of(box)) and chain
// (ins::engine(box)), rendered after that box's render pass (box 0's on core 3 during RenderBlock's wait, core 2 on an 8-voice engine; box 1's on core 1: cores.cpp MasterCore, 2026-11-22), processed in that box's own stream's OUT fill on its
// device row's pairs (a box without a PRE pair of its own, pre == post, has only POST). SRC MIX and stereo=measure are the
// Rytm's (box 0). The calls without a box are box 0's on the Rytm's pairs.
#pragma once
#include <stdint.h>

namespace core {
namespace out {
struct Device;
}
}  // namespace core

namespace masterfx_pi {
const unsigned kRing = 1024, kPrime = 96, kMaxPerRender = 96, kFadeFrames = 480;   // frames; the crossfade 10 ms
const unsigned kStemPrime = 64;                                                    // SRC MIX: the stems' queue, frames
// The returns' gains into the mix differ: measured on the real Rytm (2026-11-01, stereo=measure, master FX neutral: POST
// -10.9 dB, PRE -2.0 dB), and the stems reach MAIN about at unity [inferred]. So what is moved or added onto 0/1 is raised
// to keep its level in the mix: 2/3 by +8.9 dB, the stems by +10.9 dB. What was on 0/1 stays at unity. The Rytm's own pan
// law keeps L + R constant and linear (sidecar-mods re/subsystems/compressor-hardware.md), so a centred stem has 1 on each side and a hard one 2 on one
// side; stems 5 and 6 (MT/HT, CH/OH) read 0.85 dB hot against what they put in MAIN, and are trimmed.
constexpr float kPreOnPost = 2.786f, kStemOnPost = 3.508f, kStemHotTrim = 0.9067f;
const unsigned kGainFrames = 512;   // a stem's pan or its coming off main: at most this long, end to end
const unsigned kLimFade = 240;      // the limiter in and out, 5 ms: the direct signal against itself kLook frames later
// core 0, the OUT fill, after AddReturn and before take playback: out[n] as about to be sent
void Process(int32_t (*out)[12], unsigned n);
void Process(int box, const core::out::Device& dev, int32_t (*out)[12], unsigned n);   // box's, on dev's pairs
// SRC MIX: core 0, the IN completion: the Rytm's stems IN 2..9 while it is asked for (WantStems): each stem panned as the
// Rytm reports it (master::report), none that is still on main; no report: all of them, centred. box: the Rytm's slot
// (its stream's, slot 0 or 1: its slot's SRC, TRIM and report, master::rytm()); Process mixes them on its stream only
// (dev == core::out::kRytmMk2). The calls without a box are box 0's.
bool WantStems();
bool WantStems(int box);
void Stems(const int32_t (*stems)[8], unsigned n);
void Stems(int box, const int32_t (*stems)[8], unsigned n);
void Render();          // box 0's render core (cores.cpp MasterCore), after its pass: the frames queued
void Render(int box);   // ... box's, after its own send FX
bool Command(const char* p);   // core 3: mfx=stats (logged): since the last one, its render core's load, underruns, drops
// The stats, for the display (any core, plain reads)
struct Stats { uint32_t frames, under, drop_in, drop_out, stem_under; };
Stats Read();
Stats Read(int box);
float Wet();            // the crossfade's position, 0 direct .. 1 the chain's output (core 0 writes)
float Wet(int box);
void Reset();           // a new stream (core 0): the rings' frames forgotten; tests
void Reset(int box);    // box's stream (re)started
}  // namespace masterfx_pi
