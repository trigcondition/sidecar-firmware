// pi/engine/side_out.h - the side output stage's two ends (two links, phase 3(b)). While a box sits beside a Rytm
// (core/device/device.h side_slot), slot 1's voices 8..15 go into g_OutputSide: a FIFO of their own, slot 1's OUT
// settings (core::out::control1; its INSERT chains are its own alone or paired), filled by render passes of their own
// (pick: cores.cpp) and pulled by slot 1's own box stream (Pull: ob_usb.cpp), so each box's clock paces its own voices.
// The base box's stage (g_Output, the primary stream) is what it always was. A lone Rytm or a lone box has no side stage.
// Header only: the transport's tests build it as it is.
#pragma once
#include <stdint.h>
#include <string.h>

#include "device/device.h"
#include "output.h"

extern Output g_OutputSide;   // shared.h (declared here too: the transport's tests have their own shared.h)

namespace side_out {

// Which stage the control core renders a block for next: 0 the base box's, 1 the side stage's, -1 neither (both FIFOs
// within a block of their target). Both short: the emptier first, so a burst of one never starves the other.
inline int pick(uint32_t base_level, bool side_on, uint32_t side_level, uint32_t target, uint32_t block) {
  const bool b = base_level + block <= target, s = side_on && side_level + block <= target;
  if (b && s) return side_level < base_level ? 1 : 0;
  return b ? 0 : s ? 1 : -1;
}

// The consumer's (the side box's stream, core 0). g_restart: its stream (re)started, fade in again at the next pull.
inline volatile bool g_restart = true;
inline bool g_on = false;   // the last pull had a side stage to pull from
inline void Restart() { g_restart = true; }

// n frames for slot `slot`'s own stream (dev: its box's device row): its voices through the side stage, or silence
// (and nothing popped) while that slot has no side stage. voices, feed (optional): as Output::Pull's (feed: Taps::feed, the
// PRE / POST voices' stand-ins for their stems: the Rytm's send FX when it is the side box; left as is while silent).
// lr, pairs (optional): Taps::lr / pairs, for the recorder when it records this box (rec_pi::Slot)
inline void Pull(int slot, const core::out::Device* dev, int32_t (*out)[Output::kChannels], uint32_t n,
                 int16_t (*voices)[Output::kVoices] = nullptr, float (*feed)[Output::kVoices] = nullptr,
                 int16_t (*lr)[2 * Output::kVoices] = nullptr, float (*pairs)[4] = nullptr) {
  const bool on = core::device::side_slot() == slot;
  if (on && (!g_on || g_restart)) {   // a new stream, or the side stage just began: from an empty FIFO, faded in
    g_restart = false;
    g_OutputSide.SetDevice(dev);
    g_OutputSide.Reset();
  }
  g_on = on;
  if (!on) {
    memset(out, 0, sizeof(int32_t) * Output::kChannels * n);
    if (voices) memset(voices, 0, sizeof(int16_t) * Output::kVoices * n);
    return;
  }
  if (!feed && !lr && !pairs) {
    g_OutputSide.Pull(out, n, voices);
    return;
  }
  Output::Taps t;
  t.feed = feed, t.lr = lr, t.pairs = pairs;
  g_OutputSide.Pull(out, n, voices, &t);
}

}  // namespace side_out
