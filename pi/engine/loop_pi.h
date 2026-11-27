// pi/engine/loop_pi.h - the master looper on the Pi (core/loop/loop.h, spec-050 section 3): its RAM at boot (loop_plan.h),
// the two audio hooks the Overbridge driver calls (ob_usb.cpp: the IN completion and FillOut, both core 0), and KEEP's
// card writer in core 0's idle loop (loop_keep.h).
#pragma once
#include <stdint.h>

#include "loop_plan.h"

namespace core { namespace out { struct Device; } }

namespace loop_pi {

// Boot, before the pool RAM takes what is left (bridge::begin): loopram=MB (default by the board's RAM, loop_plan.h),
// a ring for each slot and the store split between them (one looper per box, core/loop/loop.h), core::loop::init for each.
// Logs "loop ring 32 MB x 2, grabs N MB x 2" or why it is off.
void Initialize();
bool WantsIn(unsigned nSlot);   // slot nSlot's ring exists (its box's IN completion hands its MAIN over)
// slot nSlot's IN completion: n frames of its box's MAIN L / R, interleaved, that arrived with render frame `arrival` for the
// last of them, `lost` frames missing before them (the Overbridge counter's gap)
void InBlock(unsigned nSlot, const int32_t* lr, unsigned n, unsigned lost, uint64_t arrival);
// slot nSlot's FillOut, after the master FX: its loop onto its box's returns in F; renderFrame the first frame's
void OutMix(unsigned nSlot, int32_t (*F)[12], unsigned n, uint64_t renderFrame);
// a box's stream starts in slot nSlot (CObUsb): its looper plays on that box's returns (its device row's POST / PRE; none: POST only)
// with that box's make-ups (the Rytm's measured POST / PRE; another box: 1 until loop=cal measures its POST return)
void SetBox(unsigned nSlot, const core::out::Device* pDev);
void Poll();      // core 0's idle loop: a KEEP job's next piece (each slot's in turn, one at a time)
bool Busy();      // a KEEP is being written

}  // namespace loop_pi
