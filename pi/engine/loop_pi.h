// pi/engine/loop_pi.h - the master looper on the Pi (core/loop/loop.h, spec-050 section 3): its RAM at boot (loop_plan.h),
// the two audio hooks the Overbridge driver calls (ob_usb.cpp: the IN completion and FillOut, both core 0), and KEEP's
// card writer in core 0's idle loop (loop_keep.h).
#pragma once
#include <stdint.h>

#include "loop_plan.h"

namespace loop_pi {

// Boot, before the pool RAM takes what is left (bridge::begin): loopram=MB (default by the board's RAM, loop_plan.h),
// the ring and the store from the heap, core::loop::init. Logs "loop ring 32 MB, grabs N MB" or why it is off.
void Initialize();
bool WantsIn();   // the ring exists (a Rytm's IN completion hands its MAIN over)
// The IN completion: n frames of MAIN L / R, interleaved, that arrived with render frame `arrival` for the last of them,
// `lost` frames missing before them (the Overbridge counter's gap)
void InBlock(const int32_t* lr, unsigned n, unsigned lost, uint64_t arrival);
// FillOut, after the master FX: the loop onto F[f][0..1]; renderFrame the first frame's (Output::PulledFrame)
void OutMix(int32_t (*F)[12], unsigned n, uint64_t renderFrame);
void Poll();      // core 0's idle loop: a KEEP job's next piece
bool Busy();      // a KEEP is being written

}  // namespace loop_pi
