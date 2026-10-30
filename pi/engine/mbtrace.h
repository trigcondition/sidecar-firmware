// pi/engine/mbtrace.h - a diagnostic: when core 0 made a call that goes to the VideoCore firmware (a mailbox property
// tag) or ran a main-loop section for a long time, and how long it took, on the same frame scale as the control loop's
// gaps (cores.h Gaps: frames pulled since the last Output::Reset), so /status can line the two up. Scope(id) around a
// call records it. Two records, because /status is only readable 20-40 s after boot:
//   live  ring (kN): the last calls of at least kLiveUs, whenever - the periodic reads never enter it;
//   boot  ring (kB): every call of at least min_us in the first kBootFrames of the current stream (10 s of audio),
//                    then FROZEN (not overwritten) until the next stream (a Reset) clears it.
// ids: 1 ReadPower, 2 CPUThrottle::Update, 3 temperature (the stats line), 4 the ARM clock read back (the
// stats line: the voice budget follows it), 6 gadget throttled flags; 10.. core 0's main
// loop sections: 10 plug-and-play, 11 gadget, 12 sidecar, 13 MIDI, 14 transport Poll, 15 console, 16 card service,
// 17 recorder, 19 storage, 20 sampler; 30 a whole core-0 loop iteration (top to top) of at least 1 ms.
#pragma once
#include "shared.h"
#include "crumb.h"   // (2026-10-29: each Scope is also the core's breadcrumb section, crumb.h)

namespace mbtrace {
const unsigned kN = 16, kB = 64, kLiveUs = 200, kBootFrames = 480000;
struct Rec { volatile unsigned id, at, dur_us; };
inline Rec *ring() { static Rec r[kN]; return r; }
inline Rec *boot() { static Rec r[kB]; return r; }
inline volatile unsigned &count() { static volatile unsigned n; return n; }
inline volatile unsigned &bootCount() { static volatile unsigned n; return n; }
inline volatile unsigned &bootStream() { static volatile unsigned n; return n; }
inline void Note(unsigned id, unsigned at, unsigned dur, unsigned min_us) {
  unsigned st = g_Output.Streams();
  if (st && at < kBootFrames && dur >= min_us) {
    if (bootStream() != st) bootStream() = st, bootCount() = 0;   // a new stream: record its start-up afresh
    if (bootCount() < kB) {
      Rec &b = boot()[bootCount()];
      b.id = id, b.at = at, b.dur_us = dur;
      bootCount() = bootCount() + 1;
    }
  }
  if (dur < kLiveUs) return;
  Rec &r = ring()[count() % kN];
  r.id = id, r.at = at, r.dur_us = dur;
  count() = count() + 1;
}
class Scope {
 public:
  // min_us: the boot ring's threshold (0: every call)
  explicit Scope(unsigned id, unsigned min_us = 0) : id_(id), min_(min_us), was_(crumb::Enter(id)) {
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t0_)::"memory");
    at_ = g_Output.FramesSinceReset();
  }
  ~Scope() {
    unsigned long long t1, f;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t1)::"memory");
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    unsigned dur = (unsigned)((t1 - t0_) * 1000000 / f);
    Note(id_, at_, dur, min_);
    crumb::Leave(was_);
  }
 private:
  unsigned id_, at_, min_;
  uint32_t was_;
  unsigned long long t0_;
};
}  // namespace mbtrace
