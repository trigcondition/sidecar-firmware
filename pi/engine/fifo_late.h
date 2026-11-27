// The measured FX guard's audio-buffer view (cores.cpp RenderBlock; docs/fx-measured-mute.md rule 4). Header only: its
// test builds it as it is.
//
// The FIFO is low under kLowBlocks blocks at a base pass's start. Each low run's first block is a dip (one control pass
// that came late or one render pass that fell short). Two dips within kHold base passes, one of them under a block, make the render core that took
// longer in the last block late, and keep it late for kHold passes from the second dip.
//   2026-11-25 LT12g P5: no core past a block, yet the FIFO drained 14 s: the FIFO's view decided.
//   LT12h P5: the control loop refills the FIFO over a pass, so only a pass's first block began low; late and on-time
//   blocks alternated and the late rule's 4 in a row never came: hence the hold.
//   LT12i P5: at 1 block the mute landed only once the FIFO was empty (138 underruns, a scrape): 2 blocks.
//   Two dips, not one: a single long control pass (pass max reaches ~3 ms in normal play) dips the FIFO once and refills
//   it; an overload dips it at every pass. Not for kGrace passes after a stream (re)start, whose FIFO begins empty.
//   LT13 P5 (2026-11-25): an overload the control loop cannot refill keeps the FIFO low at every pass, one low run, one
//   dip, and no hold (10:17, 108 low blocks, no mute). So a low run that gains nothing over kArm passes arms the hold as
//   a second dip does, and keeps it on while that lasts. After a single long control pass the FIFO climbs back, slowly
//   near a full load (from empty to 2 blocks can take ~100 passes at 96 %), but it climbs: no hold. Its length alone
//   would not tell the two apart.
//   The late core (blame): base and side passes take turns on cores 1 and 2, and the core that took longer in the last
//   pass alone flipped between them (LT13: the Rytm's slow slots on core 1 in side passes, the A4's work on core 2 in
//   base passes), so no core saw the late rule's 4 in a row. Both FIFOs drain at one base pass plus one side pass, so
//   the late core is the one that took longer over the last of each together.
#pragma once
#include <stdint.h>

namespace fifo_late {

struct State {
  static constexpr unsigned kGrace = 64;     // base passes after a stream (re)start
  static constexpr unsigned kLowBlocks = 2;  // under this many blocks in the FIFO: low
  static constexpr unsigned kHold = 32;      // base passes: the window for the second dip, and late after it (8 ms)
  static constexpr unsigned kArm = 8;        // passes of a low run: no gain over them arms the hold (2 ms)
  unsigned grace = kGrace, left = 0, streams = 0, since = kHold;   // since: base passes since the last dip
  unsigned run = 0, mark = 0;   // passes in a row begun low (the log's too); the level kArm passes back in the run
  bool low = false;                                                // the last base pass began low
  // LT14 P6 (2026-11-25, T511): two shallow dips (FIFO min 30, no underrun) at 1200 MHz armed the hold and the late core's
  // 8 us dearest (a CLOUD insert) went. Two dips arm it only once one of them went under a block (decided 2026-11-25:
  // "two dips must go deep"); an overload that never refills is the low run's (kArm), not the dips'.
  bool pair = false, deep = false, was_deep = false;   // this low run is a second dip; it / the last went under a block
  // one base pass: true when it is late. dip: set when this pass began a low run (the dips counted)
  bool step(uint32_t level, unsigned streams_now, uint32_t block, bool on, bool* dip = nullptr) {
    if (dip) *dip = false;
    if (streams_now != streams)
      streams = streams_now, grace = kGrace, left = 0, since = kHold, run = 0, low = false, pair = deep = was_deep = false;
    if (grace) {
      --grace;
      return false;
    }
    if (!on) return left = 0, run = 0, low = false, pair = false, false;
    const bool now_low = level < kLowBlocks * block;
    if (now_low && !low) {   // a dip: a second within kHold passes arms the hold, once one of the two went deep
      pair = since < kHold;
      was_deep = deep, deep = false;
      since = 0;
      if (dip) *dip = true;
    } else if (since < kHold) {
      ++since;
    }
    if (now_low) {
      if (level < block) deep = true;
      if (pair && (deep || was_deep)) left = kHold, pair = false;
    } else {
      pair = false;
    }
    if (!now_low) {
      run = 0;
    } else {   // a low run: no gain over kArm passes is an overload (LT13 P5), as a second dip
      if (!low || !run) mark = level, run = 0;
      if (run < 0xFFFF) ++run;
      if (run % kArm == 0) {
        if (level <= mark) left = kHold;
        mark = level;
      }
    }
    low = now_low;
    if (!left) return false;
    --left;
    return true;
  }
  // after a late mute (cores.cpp): the hold ends and the next takes two new dips, so one low-buffer event mutes one
  // effect while the FIFO refills (LT12k, 2026-11-25: one event muted 8 effects in ~40 ms)
  // A low run starts again too: a FIFO still low after the mute takes kArm more passes with no gain before the next.
  void rearm() { left = 0, since = kHold, run = 0, pair = false, deep = false, was_deep = false; }
  // the hold still on, without a pass of its own (the other FIFO's passes: cores.cpp)
  bool held() const { return grace == 0 && left > 0; }
};

// Admission's word at the limit (decided 2026-11-26 after VA1, T515): admission (core/device/admit.h) keeps each render
// core within kLimitBp (80 %) in its worst case, and the FIFO's view must not mute an effect on a core that measures
// within it. VA1 at 1200 MHz, admitted 79.77 %: the FIFO dipped and the late core's 8-9 us send effect was muted three
// times, with the late core measuring 74 % (core 1) and 63-69 % (core 2); every overload this rule is for (LT12g, LT12h,
// LT12j, LT13) measured 82-83 %. So a pass the FIFO makes late counts for its core only when the core measures at or over
// the limit (Load), or an output really ran empty within Under::kRecent passes (an underrun, not a dip: O1 P1 had +4 in
// its mute's 10 s, and a dropout is never left unanswered; decided 2026-11-26).
//
// Load: a render core's measured share as admission counts it: its time for one block of audio (its last base pass plus
// its last side pass, voices and chains, as blame() sums them: both boxes' 12 frames are due every 250 us) over a
// block's time, a running mean over ~2^kShift passes (~4-8 ms). Per block, not per wall second: the hold comes right
// after a long control pass, when the render cores catch up back to back, and a core's share of the wall time then
// looks near 100 % whatever its load; its time per block does not change. At a steady load it equals the stats'
// "c1 with chains" (VA1 O1 P1: 74 %), which admission's limit is checked against.
// cycle: the core's last base + side pass ticks; block: a 12-frame block's ticks.
struct Load {
  static constexpr unsigned kShift = 5;
  uint64_t mean = 0;   // << kShift
  bool primed = false;
  void step(uint64_t cycle) {
    if (!primed) primed = true, mean = cycle << kShift;
    else mean += cycle - (mean >> kShift);
  }
  unsigned bp(uint64_t block) const {   // in 1/100 % (bp, as admit.h)
    if (!block || !primed) return 0;
    const uint64_t v = (mean >> kShift) * 10000u / block;
    return v > 0xFFFFu ? 0xFFFFu : (unsigned)v;
  }
};
// The FIFO really ran empty lately: the outputs' underflow counts (output.h Underflows: glitches while playing, not a
// stream's restart) moved within the last kUnderRecent passes.
struct Under {
  static constexpr unsigned kRecent = 64;   // passes: two holds (State::kHold)
  uint32_t last = 0;
  unsigned since = kRecent;
  bool primed = false;
  void step(uint32_t underflows) {
    if (!primed) primed = true, last = underflows;
    if (underflows != last) last = underflows, since = 0;
    else if (since < kRecent) ++since;
  }
  bool recent() const { return since < kRecent; }
};
// a pass the FIFO makes late counts for its core (cores.cpp): the core measures at or over the limit, or a real underrun
inline bool counts(unsigned core_bp, unsigned limit_bp, bool underran) { return core_bp >= limit_bp || underran; }

// The render core a late pass is for: 1 or 2, the one that took longer over its last base pass and its last side pass
// together (each core's block, voices and chains: cores.cpp). No side stage: its blocks are 0 (cores.cpp clears them).
inline unsigned blame(uint64_t c1_base, uint64_t c1_side, uint64_t c2_base, uint64_t c2_side) {
  return c1_base + c1_side >= c2_base + c2_side ? 1 : 2;
}

// Both FIFOs, one pass at a time (cores.cpp RenderBlock): a pass is late when its own FIFO is (step), or the other's
// hold is still on (held: the two boxes take turns on cores 1 and 2, LT12j).
struct Pair {
  State base, side;
  // side: a side stage's pass (else the base box's); side_on: a side stage runs. dip: this pass began a low run
  bool step(bool is_side, uint32_t level, unsigned streams_now, uint32_t block, bool on, bool side_on, bool* dip = nullptr) {
    if (!is_side) return base.step(level, streams_now, block, on, dip) || (on && side_on && side.held());
    return side.step(level, streams_now, block, on, dip) || (on && base.held());
  }
  void rearm() { base.rearm(), side.rearm(); }
};

}  // namespace fifo_late
