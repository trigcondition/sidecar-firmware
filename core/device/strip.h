// core/device/strip.h - a kBracketMix box's filter and amp pages on the Sidecar's voices (device.h, docs/device-profiles.md).
// In that bracket a voice never passes through the box's own filter or amp, so the box's values drive stages the Sidecar has:
//   filter   the voice's INSERT slot A (core/fx/insert.h): FILTER (LP / HP) or EQ (a bell), its knobs held as p-locks, so nothing
//            is written to the card per knob turn, and the envelope as its LFO offset on CUT / FREQ. Slot A is the strip's on such a
//            box. An open low-pass with no envelope is the slot off: nothing runs.
//   amp      the voice's OUT LEV and PAN (core/fx/out.h), the attack / hold / decay envelope on LEV, both as LFO offsets.
// Control rate, on the control core: lfo.h's millisecond and the stages' own 3 ms smoothing. So an envelope is an approximation of
// the box's, with no audio-rate edge, and a value from 0 attack passes the voice's own transient untouched. No gate is reported:
// HOLD 127 (the box's NOTE) holds until the next trig; the filter's release follows a note-off. A box that sends no controls report
// leaves everything alone. The times' curve is ours, not measured from a box.
#pragma once
#include <stdint.h>

#include "device/device.h"

namespace core {
namespace device {
namespace strip {

void report(const Controls& c, int voice, const uint8_t* values);   // { <value> <fraction> } x c.n
void trig(int voice);                                               // restarts the voice's envelopes
void off(int voice);                                                // a note-off: the filter's release
void tick(const Controls& c, uint32_t us);                          // the control loop: a millisecond at most
void reset();                                                       // tests, a boot: nothing reported yet
float amp_env(int voice);                                           // tests: 0..1
float flt_env(int voice);

}  // namespace strip
}  // namespace device
}  // namespace core
