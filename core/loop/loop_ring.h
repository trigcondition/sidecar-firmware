// core/loop/loop_ring.h - the looper's MAIN ring (spec-050 3.3): the Rytm's MAIN L / R as it arrives, always, int32 (24-bit
// full scale << 8) L R interleaved, `cap` frames (the Pi: 4,194,304 = 87.38 s). Header-only, lock-free, one writer (core 0,
// the Overbridge IN completion) and readers on core 3 (the grabs' copies, the playback's frames): the write count is
// published with release, read with acquire. A frame has an absolute index (frames written before it); the map host render
// frame <-> index is the minimum arrival offset over the last 24 blocks (seq::Follower's way: the USB path's jitter only
// ever delays), so `frame_at(h)` gives the frame that arrived at host frame h, jitter-free. A lost block is written as
// silence so the map stays linear: not written (the xHCI completion has no time for megabytes of zeros) but recorded in a short
// list of gap ranges, which every reader (copy, frame, gap_in) reads as zeros; a seqlock keeps the list whole for readers.
#pragma once
#include <stdint.h>
#include <string.h>

namespace core {
namespace loop {

class Ring {
 public:
  static constexpr int kWindow = 24;
  static constexpr uint32_t kInFlight = 64;   // frames a write has in flight (a block is 7): more than any block
  static constexpr int64_t kRestart = 48000;   // frames: a jump of the arrival offset past this is a pause, not jitter
  void init(int32_t* mem, uint32_t frames) {
    mem_ = mem, cap_ = frames, w_ = 0, n_ = 0, k_ = 0, have_ = false, gn_ = 0, gseq_ = 0;
    off_ = 0;
  }
  bool ready() const { return mem_ && cap_; }
  uint32_t capacity() const { return cap_; }
  int32_t* data() const { return mem_; }
  // the writer: n frames (L R interleaved) that arrived with the host's render frame `arrival` for the last of them,
  // preceded by `lost` frames that never came (written as silence)
  void write(const int32_t* lr, int n, uint64_t arrival, unsigned lost) {
    uint64_t w = w_;
    if (lost) {
      gap_add(w, w + lost);
      w += lost;
    }
    // the block's offset: host frame - index, the same for each of its frames (arrival of frame i = arrival - (n - 1 - i))
    int64_t d = (int64_t)arrival - (int64_t)(n - 1) - (int64_t)w;
    // a stream that stopped and came back (the Rytm re-plugged, a re-enumeration): the host's frames went on while nothing
    // was written. Its pause is written as silence too (past kRestart: a late burst is never that long), so the map stays
    // one line and a grab across it carries the gap
    if (have_ && d > off_ + kRestart) {
      uint64_t extra = (uint64_t)(d - off_);
      gap_add(w, w + extra);
      w += extra;
      d -= (int64_t)extra;
    }
    win_[k_] = d;
    k_ = (k_ + 1) % kWindow;
    if (n_ < kWindow) ++n_;
    int64_t m = win_[0];
    for (int i = 1; i < n_; ++i)
      if (win_[i] < m) m = win_[i];
    // a step back of the whole window (a new link) is taken at once; the minimum follows a faster path down only
    __atomic_store_n(&off_, m, __ATOMIC_RELAXED);
    have_ = true;
    for (int i = 0; i < n; ++i) put(w + i, lr[2 * i], lr[2 * i + 1]);
    __atomic_store_n(&w_, w + n, __ATOMIC_RELEASE);
  }
  uint64_t written() const { return __atomic_load_n(&w_, __ATOMIC_ACQUIRE); }
  // the offset a reader uses (host = index + offset); false until a block came
  bool offset(int64_t* o) const {
    *o = __atomic_load_n(&off_, __ATOMIC_RELAXED);
    return have_;
  }
  // the ring index of the frame that arrived at host frame h; false when it is not (yet) in the ring
  bool frame_at(uint64_t h, uint32_t* idx) const {
    uint64_t k;
    if (!index_at(h, &k)) return false;
    *idx = (uint32_t)(k % cap_);
    return true;
  }
  // the absolute index (frames written before it) of host frame h's frame, if it is in the ring
  bool index_at(uint64_t h, uint64_t* k) const {
    if (!have_) return false;
    int64_t i = (int64_t)h - __atomic_load_n(&off_, __ATOMIC_RELAXED);
    uint64_t w = written();
    if (i < 0 || (uint64_t)i >= w || w - (uint64_t)i > cap_) return false;
    *k = (uint64_t)i;
    return true;
  }
  uint64_t host_of(uint64_t k) const { return (uint64_t)((int64_t)k + __atomic_load_n(&off_, __ATOMIC_RELAXED)); }
  // n frames from the frame at host frame `from`, into out (L R interleaved); the count copied (0: not in the ring, or
  // overwritten while copying)
  uint32_t copy(uint64_t from, uint32_t n, int32_t* out) const {
    uint64_t k;
    if (!index_at(from, &k)) return 0;
    return copy_idx(k, n, out);
  }
  uint32_t copy_idx(uint64_t k, uint32_t n, int32_t* out) const {
    uint64_t w = written();
    if (k >= w || w - k + kInFlight > cap_ || n == 0) return 0;
    if (k + n > w) n = (uint32_t)(w - k);
    uint32_t i = (uint32_t)(k % cap_), first = cap_ - i < n ? cap_ - i : n;
    memcpy(out, mem_ + 2 * (size_t)i, 8u * first);
    if (first < n) memcpy(out + 2 * (size_t)first, mem_, 8u * (n - first));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);   // the copy's loads before the lap check (the seqlock's read order)
    // the writer puts a block's frames before it publishes the count: up to kInFlight frames past written() are already
    // overwritten, so the lap check keeps that much room (and the first test above too)
    if (written() - k + kInFlight > cap_) return 0;   // the writer lapped (or is lapping) the first frame meanwhile: not a copy
    Gap g[kGaps];
    int ng = gaps(g);
    for (int j = 0; j < ng; ++j) {   // the stretches that never arrived read as silence
      uint64_t a = g[j].a > k ? g[j].a : k, b = g[j].b < k + n ? g[j].b : k + n;
      for (uint64_t x = a; x < b; ++x) out[2 * (x - k)] = out[2 * (x - k) + 1] = 0;
    }
    return n;
  }
  // a gap (frames that never arrived) in the n frames from host frame `from`
  bool gap_in(uint64_t from, uint32_t n) const {
    int64_t o = __atomic_load_n(&off_, __ATOMIC_RELAXED);
    uint64_t k0 = (int64_t)from - o < 0 ? 0 : (uint64_t)((int64_t)from - o);
    Gap g[kGaps];
    int ng = gaps(g);
    for (int j = 0; j < ng; ++j)
      if (k0 < g[j].b && k0 + n > g[j].a) return true;
    return false;
  }
  void frame(uint64_t k, int32_t* l, int32_t* r) const {   // frame k (it must be in the ring); core 0's playback calls it per frame
    uint32_t i = (uint32_t)(k % cap_);
    *l = mem_[2 * (size_t)i], *r = mem_[2 * (size_t)i + 1];
    uint32_t n = __atomic_load_n(&gn_, __ATOMIC_ACQUIRE);   // (scanned in place, no copy: a torn entry misreads one frame at worst)
    if (n > (uint32_t)kGaps) n = kGaps;
    for (uint32_t j = 0; j < n; ++j)
      if (k >= gl_[j].a && k < gl_[j].b) {
        *l = *r = 0;
        break;
      }
  }


 private:
  struct Gap { uint64_t a, b; };
  static constexpr int kGaps = 64;
  void gap_add(uint64_t a, uint64_t b) {   // the writer: one more gap (the oldest forgotten past kGaps)
    uint32_t sq = __atomic_load_n(&gseq_, __ATOMIC_RELAXED);
    __atomic_store_n(&gseq_, sq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    gl_[gn_ % kGaps] = {a, b};
    ++gn_;
    __atomic_store_n(&gseq_, sq + 2, __ATOMIC_RELEASE);
  }
  int gaps(Gap* out) const {   // a whole copy of the list (a seqlock read: it waits out the writer's short odd window, however long a
    for (;;) {                  // preempted test thread sits in it; on the Pi the writer is core 0 and never waits)
      uint32_t s0 = __atomic_load_n(&gseq_, __ATOMIC_ACQUIRE);
      if (s0 & 1) continue;
      uint32_t n = gn_;
      int c = n < (uint32_t)kGaps ? (int)n : kGaps;
      for (int j = 0; j < c; ++j) out[j] = gl_[j];
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (__atomic_load_n(&gseq_, __ATOMIC_RELAXED) == s0) return c;
    }
  }
  void put(uint64_t k, int32_t l, int32_t r) {
    uint32_t i = (uint32_t)(k % cap_);
    mem_[2 * (size_t)i] = l, mem_[2 * (size_t)i + 1] = r;
  }
  int32_t* mem_ = nullptr;
  uint32_t cap_ = 0;
  uint64_t w_ = 0;
  int64_t win_[kWindow];
  int n_ = 0, k_ = 0;
  int64_t off_ = 0;
  bool have_ = false;
  Gap gl_[kGaps];
  uint32_t gn_ = 0, gseq_ = 0;
};

}  // namespace loop
}  // namespace core
