// pi/engine/sendfx_pi.h - the send FX (core/fx/send_fx.h) in the Pi engine: the Rytm's Overbridge IN voice stems
// (channels 2..9) in from the IN completion, core::fx::SendFx on render core 2 (after its voices, the block already
// published), the stereo return added in the OUT fill onto host -> Rytm 2/3 (MAIN FX, PRE: through the analog distortion
// + compressor) and 0/1 (MAIN, POST: clean), each FX on the pair its P1 (ROUTE: 0..63 PRE, 64..127 POST) names, and
// STUTTER's insert (the stems' sum, stutter_fx.h) on its slot's pair too. Measured 2026-10-16 on the real Rytm. The
// engine's own per-track voice output is untouched. Off (no work anywhere) while neither FX has a playable TYPE.
//
// Two SPSC rings: stems (core 0 IN -> core 2), returns (core 2 -> core 0 OUT). The return path holds kPrime frames
// before it starts (2 ms: the same margin as the voices' FIFO target, against core 3's longest pass between
// blocks); an empty return ring gives silence, is counted and primes again. Hosted (no Circle headers): ob_usb.cpp
// and cores.cpp call it.
#pragma once
#include <stdint.h>

namespace sendfx_pi {
const unsigned kRing = 1024, kPrime = 96, kMaxPerRender = 96;   // frames
// core 0, before the cores start: builds the SendFx in mem (Bytes() long, 64-byte aligned: kernel.cpp reserves it
// once at boot from Circle's heap, so its ~4.8 MB is not in the image's .bss). Hosted: Fx() builds it on first use.
unsigned long Bytes();
void Init(void* mem);
bool On();                                                // a playable TYPE on FX 1 or 2
void FromRytm(const int32_t (*stems)[8], unsigned n);    // core 0, IN completion: IN 2..9 (int32 full scale)
// fx-ins (core/fx/insert.h): core 0, the OUT fill: the Sidecar voices as rendered (after their INSERT chains) -> their
// sends to the 3 buses, queued on core 0 for FromRytm, which pairs them frame by frame with the stems (none queued:
// zeros; a backlog past kVoiceLag frames is dropped, so the two never drift apart). Bus 1 / 2 take them into send FX 1
// / 2's input, bus 3 (a dry bus with its own chain) on render core 2 after the send FX (Render).
// feed (optional; the output stage's Taps::feed): each voice's stand-in for its Rytm stem while it is routed OUT PRE /
// POST (core/fx/out.h: the stem is silent then), full scale 1.0, added into that stem in FromRytm, paired the same way:
// the track's FX1 / FX2 send levels apply to it as to the stem, with the same lag.
const unsigned kVoiceLag = 256;
void FromVoices(const int16_t (*v)[8], unsigned n, const float (*feed)[8] = nullptr);
void Render();                                            // core 2, after its block: the queued stems
void AddReturn(int32_t (*out)[12], unsigned n,            // core 0, OUT fill: onto 2/3 and 0/1 (saturating);
               float (*slots)[4] = nullptr);              // slots: each FX's own return as added (left as is when none)
// The console's sendfx commands (core 3, cores.cpp's Command); the answer logged; `sendfx` adds the stats since the
// last `sendfx`: frames, render-core 2 load, underruns, ring drops, the return's peak.
bool Command(const char* p);
// MIDI CC 120 with the note adapter off (with it on, core::NoteAdapter makes the same request): both FX fade out,
// clear and return (core::fx::all_sound_off); logged.
void AllSoundOff();
// What the Rytm's kit (or the console) set, for the display (any core, plain reads: a stale value at worst): each FX's TYPE
// name ("OFF" when none) and the 12 tracks' send levels (0..127) to FX 1 and FX 2. The console's `killfx` is the display's
// "kill tails": the same request as CC 120 (core::fx::all_sound_off), logged.
struct View {
  char type[2][12];
  uint8_t send[2][12];
  uint8_t post[2];   // 1 = this FX returns POST on 0/1 (P1 64..127, or forced), 0 = PRE on 2/3
};
void Read(View* v);
// MIDI clock / Start / Continue / Stop / SPP from the Rytm (MidiIn's realtime callback): STUTTER's grid.
void Clock(uint8_t status, uint16_t spp);
}  // namespace sendfx_pi
