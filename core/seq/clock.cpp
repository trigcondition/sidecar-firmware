// core/seq/clock.cpp - see seq.h: the Rytm's frames -> the host's render frames, and the per-track step clock. Cold.
#include "seq/seq.h"

#if CORE_POLY
#include "platform.h"

namespace core {
namespace seq {

CORE_COLD double speed_mult(int speed) {
  static const double k[7] = {2.0, 1.5, 1.0, 0.75, 0.5, 0.25, 0.125};
  return speed >= 0 && speed < 7 ? k[speed] : 1.0;
}

CORE_COLD double step_frames(double bpm, int speed) {
  if (bpm <= 1.0) return 0;
  return 48000.0 * 60.0 / bpm / 4.0 / speed_mult(speed);
}

CORE_COLD void Follower::reset() {
  n = w = 0;
  offset = 0;
  valid = false;
  last_blk = -1;
}

CORE_COLD int64_t Follower::unwrap(uint32_t blk28) {
  constexpr int64_t kMod = 1 << 28;
  blk28 &= (uint32_t)(kMod - 1);
  if (last_blk < 0) return last_blk = blk28;
  int64_t d = ((int64_t)blk28 - (last_blk & (kMod - 1)) + kMod) % kMod;
  if (d >= kMod / 2) d -= kMod;   // a little older (a state message after a tick)
  if (d < -15000 || d > 1500 * 3600) {   // the Rytm restarted (its counter from 0) or a long gap: a new base
    n = w = 0, valid = false;
    return last_blk = blk28;
  }
  int64_t b = last_blk + d;
  if (d > 0) last_blk = b;
  return b;
}

CORE_COLD void Follower::sample(int64_t host_arrival, int64_t rytm_frame) {
  int64_t o = host_arrival - rytm_frame;
  if (valid && o < offset - 48000) n = w = 0;   // far earlier than ever: a new link / time base
  win[w] = o;
  w = (w + 1) % kWindow;
  if (n < kWindow) ++n;
  int64_t m = win[0];
  for (int i = 1; i < n; ++i)
    if (win[i] < m) m = win[i];
  offset = m;
  valid = true;
}

CORE_COLD int StepClock::tick(int step, int len, int sp, double f, double tempo_period, bool restart) {
  if (len < 1) len = 1;
  int adv;
  if (restart) {   // 1042: the Rytm's own step - 0 after PLAY and a bar's switch, the old position after a DIRECT / TEMP
    abs = step, adv = 0;   // JUMP (rytm-mods 0026 1042: its 03 at the new pattern's first step); was 0: a jump played
                           // the new pattern from its step 0 at the old position
  } else if (last_step < 0) {   // joined while it plays: the Rytm's own step
    abs = step, adv = 0;
  } else {
    adv = ((step - last_step) % len + len) % len;
    if (adv == 0) adv = len == 1 ? 1 : 0;   // a repeated tick (a resend) moves nothing
    abs += adv;
  }
  double p = tempo_period > 0 ? tempo_period / speed_mult(sp) : 0;
  if (p <= 0 && on && adv > 0 && last_frame > 0) {   // no tempo yet: measured from the ticks
    double m = (f - last_frame) / adv;
    if (m > 100 && m < 96000) p = period > 0 ? period * 0.75 + m * 0.25 : m;
  }
  if (p <= 0) p = period;
  bool reanchor = restart || !on || sp != speed;
  if (!reanchor && period > 0 && p > 0 && (p > period * 1.01 || p < period * 0.99)) reanchor = true;   // a tempo change
  period = p;
  rytm_len = len, speed = sp, last_step = step, last_frame = f;
  if (!reanchor && period > 0) {
    double err = f - frame_of((double)abs);
    double lim = period * 0.5 > 4.0 * kBlkFrames ? period * 0.5 : 4.0 * kBlkFrames;
    if (err > lim || err < -lim) reanchor = true, ++jumps;
    else anchor += err * 0.15;   // the stamps are block-quantised: pulled, not followed
  }
  if (reanchor) anchor = f, anchor_abs = abs;
  on = true;
  return adv;
}

}  // namespace seq
}  // namespace core
#endif
