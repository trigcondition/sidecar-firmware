// core/loop/loop.cpp - see loop.h. Control side (core 3): the grabs, the store, the state machine, one tick a control pass.
// Core 0: in_block (the ring) and out_mix (the playback), which take what to play from one published snapshot.
// One looper per box (kSlots): every piece of state below is a Looper's, so slot 0's and slot 1's never meet. The public
// functions at the end pick the Looper: the core 0 hooks, init and KEEP by the slot they are given, the controls by the
// slot in hand (set_slot_fn: the engine's fx_slot), tick every slot in its own Scope (its clock, its mutes, its box).
// Settings: loop.txt is a box setting, read and written in the slot's Scope (state_path: its own folder).
#include "loop/loop.h"

#include <math.h>
#include <string.h>

#include "device/device.h"
#include "fmt/fmt.h"
#include "lfo.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "platform.h"
#include "seq/seq.h"
#include "sidecar_features.h"

namespace core {
namespace loop {

namespace {

struct Looper {
  int slot_ = 0;
  // the box's returns the loop plays onto: [0] POST (after its master FX), [1] PRE (before them); L / R channel of F, kNone: the
  // box has none (set_returns; the Rytm's 0/1 and 2/3 by default)
  static constexpr uint8_t kNone = kNoReturn;
  uint8_t ret_[2][2] = {{0, 1}, {2, 3}};
  static constexpr const char* kFile = "/.rytm/loop.txt";   // (in the slot's Scope: its box's folder)



  // ------------------------------------------------------------------------------------------------ the environment
  // (each its own box's: its clock, its pass's frames; muted: the slot in hand's, tick runs in the slot's Scope)
  CORE_COLD bool def_beat_map(double* o, double* p, int* l) { return seq::beat_map(slot_, o, p, l); }
  CORE_COLD uint64_t def_frames() { return seq::frames(slot_); }
  CORE_COLD uint16_t def_muted() { return seq::muted(); }
  CORE_COLD bool def_clock(float* t, float* us, bool* run) {
    lfo::ClockState c;
    lfo::clock_state(slot_, &c);
    *run = c.run && c.ticking;
    return lfo::clock_now(slot_, t, us);
  }
  Env env_;
  CORE_COLD bool beat_map(double* o, double* p, int* l) { return env_.beat_map ? env_.beat_map(o, p, l) : def_beat_map(o, p, l); }
  CORE_COLD uint64_t frames_now() { return env_.frames ? env_.frames() : def_frames(); }
  CORE_COLD uint16_t muted_now() { return env_.muted ? env_.muted() : def_muted(); }

  Ring ring_;
  uint8_t* store_ = nullptr;
  uint64_t store_bytes_ = 0;
  bool on_ = false;
  Stats stats_;

  // ------------------------------------------------------------------------------------------------ the grabs
  Grab g_[kMaxGrabs];   // 0 newest
  uint64_t g_off_[kMaxGrabs];   // each one's place in the store (bytes)
  uint64_t g_k0_[kMaxGrabs];    // its first frame's absolute ring index (the ring's source until copied)
  int ng_ = 0;
  uint32_t list_seq_ = 0;   // odd while the grab list is being shifted (json, read on core 0, retries around it)
  uint32_t next_id_ = 1;
  // a grab's extent in the store: its frames (kPre before the first), then the limiter's gain envelope (one uint16 a kEnvStep frames,
  // 32768 = unity; the audio is never written after the copy: KEEP saves it as it was grabbed). Memory: 8 bytes a frame + 2 bytes a 16
  // frames = 1.6 % over the audio (12 MB of audio: +190 KB)
  static constexpr uint32_t kEnvStep = 16;
  static constexpr float kEnvOne = 32768.0f;
  inline uint64_t env_off(uint32_t frames) { return ((uint64_t)(frames + kPre) * 8 + 15) & ~15ull; }
  inline uint32_t env_pts(uint32_t frames) { return (frames + kPre + kEnvStep - 1) / kEnvStep + 2; }
  inline uint64_t extent(uint32_t frames) { return (env_off(frames) + (uint64_t)env_pts(frames) * 2 + 15) & ~15ull; }

  struct Pending {
    bool on;
    int half_bars;
    double E, grid;   // the grid line (index, frames) the grab ends on
    uint64_t press;
  } pend_;
  struct Copy {
    bool on;
    uint32_t id;
    uint64_t k;       // next ring index to copy
    uint32_t done, total;   // frames
    uint32_t peak;    // the largest |sample| of what is copied
  } cp_;
  int need_bpm_ = 47;

  // a level that moves linearly over kRampFrames from where it was when core 3 set it (core 0 evaluates it at its frame)
  static constexpr int kRampFrames = 480;
  struct Ramp {
    float from = 1, to = 1;
    uint64_t at = 0;
    float at_h(uint64_t h) const {
      if (h <= at) return from;
      uint64_t d = h - at;
      return d >= (uint64_t)kRampFrames ? to : from + (to - from) * (float)d / (float)kRampFrames;
    }
  };
  // what core 0 plays: a snapshot, rewritten whole (core 3), taken under a seqlock (core 0)
  struct LpSnap {
    uint32_t id;          // a new id is a new loop
    bool active, from_ring;
    const int32_t* pcm;   // frame 0 of the store's copy (kPre frames before it)
    uint64_t idx0;        // from_ring: frame 0's absolute ring index
    uint32_t len;
    uint64_t start;       // the host render frame of sample 0
    uint64_t stop_at;     // 0: none
    uint32_t stop_len;    // frames of the fade
    uint32_t peak;        // the loop's largest |sample| (the make-up is capped from it; while it is still copied: so far)
    const uint16_t* env;  // the limiter's gain envelope (null: none, gain 1)
    uint32_t peak_unl;    // the loop's peak without the envelope (what it was playing from the ring with), while it fades in
  };
  struct Snap {
    LpSnap cur;           // the loop that starts (or plays) now
    LpSnap prev;          // the loop it replaces while that is still heard (a resample's wait: until cur.start + kSeam)
    Ramp own, lp;         // the Sidecar's own output's gain (BAL), the loop's (LEV x BAL)
  };
  Snap snap_;             // core 3's working copy
  Snap pub_;
  uint32_t pub_seq_ = 0;  // odd while pub_ is written
  uint32_t loop_ids_ = 0;
  struct LoopInfo {
    bool on;
    uint32_t grab_id;
    uint64_t start;
    uint32_t len;
    uint64_t stop_at, stop_end;
    uint8_t half_bars;
  } lp_, lpp_;   // lpp_: the loop still heard under lp_ until lp_ starts (spec 3.6's swap)

  CORE_COLD void publish() {
    __atomic_store_n(&pub_seq_, pub_seq_ + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    pub_ = snap_;
    __atomic_store_n(&pub_seq_, pub_seq_ + 1, __ATOMIC_RELEASE);
  }

  CORE_COLD int find_id(uint32_t id) {
    for (int i = 0; i < ng_; ++i)
      if (g_[i].id == id) return i;
    return -1;
  }
  CORE_COLD int64_t bar_frames(double per16) { return (int64_t)(16.0 * per16 + 0.5); }

  // the bars: the sequencer's beat map; without its 2C ticks the MIDI clock's (96 ticks a bar, less exact: spec 3.4)
  struct Map {
    double origin, per16;   // origin: render frame of bar line 0, `lat` early; per16: frames of a 1/16
    int lat;
    bool midi;
  };
  CORE_COLD bool get_map(Map* m, uint64_t now) {
    m->midi = false;
    if (beat_map(&m->origin, &m->per16, &m->lat)) return true;
    float ticks, tick_us;
    bool run = true;
    if (!env_.clock && !env_.beat_map) {   // (the defaults: the lfo clock)
      if (!def_clock(&ticks, &tick_us, &run)) return false;
    } else if (env_.clock) {
      if (!env_.clock(&ticks, &tick_us, &run)) return false;
    } else {
      return false;
    }
    if (!run || tick_us < 1000.0f) return false;
    double per_tick = (double)tick_us * 0.048;   // frames of one 24 ppqn tick
    m->per16 = 6.0 * per_tick;
    m->origin = (double)now - (double)ticks * per_tick;   // bar line 0 at the clock's position 0 (the MIDI clock's own time, no `lat` early)
    m->midi = true;
    return true;
  }
  // when bar line / grid line k's audio arrives in the ring
  CORE_COLD double arrival(const Map& m, double k, double grid) { return m.origin + m.lat + k * grid; }

  // ------------------------------------------------------------------------------------------------ the settings
  int bal_ = 64, lev_ = 100, fade_ = kFade1, live_via_ = 0, mod_bal_ = -1, mod_lev_ = -1;
  bool auto_mute_ = false;
  // OUT and the make-ups: core 3 / the Pi's init write, core 0's out_mix reads once a block (atomics; floats as their bits)
  int out_ = kOutPost;
  uint32_t pre_gain_bits_ = 0x3F800000u;   // 1.0f
  uint32_t postgain_bits_ = 0;             // 0: none measured
  CORE_COLD int bal_now() { return mod_bal_ >= 0 ? mod_bal_ : bal_; }
  CORE_COLD int lev_now() { return mod_lev_ >= 0 ? mod_lev_ : lev_; }
  // the levels as they play: core 3 moves each from where it is to the new value over 10 ms
  CORE_COLD void retarget() {
    uint64_t now = frames_now();
    float og = own_gain(bal_now()), lg = loop_gain(bal_now()) * (float)lev_now() / 100.0f;
    snap_.own = {snap_.own.at_h(now), og, now};
    snap_.lp = {snap_.lp.at_h(now), lg, now};
    if (on_) publish();
  }

  // auto-mute (spec 3.7): the tracks the looper muted at the loop's start; `seen`: those the Rytm has reported muted since, so
  // a later report without one is the user's doing and the track is theirs
  struct AutoMute {
    bool on;
    uint16_t set, seen;
  } am_;
  bool dirty_ = false;     // a setting changed (loop.txt). Every write (the setters, the cal, load / save) is on core 3: the page, the console and
                           // the control loop run there; core 0 only reads out_ / postgain_bits_ / pre_gain_bits_ (atomics), never dirty_
  int post_lat_ = 0;
  fmt::KvKeep keep_lines_;   // the lines of loop.txt this build does not know, written back as they were
  char msg_[40];
  uint32_t msg_gen_ = 0;
  CORE_COLD void say(const char* t) { sniprintf(msg_, sizeof msg_, "%s", t), ++msg_gen_; }
  // KEEP: 0 none, 1 asked, 2 core 0 is writing it
  uint8_t keep_state_ = 0;
  uint32_t keep_id_ = 0;
  KeepJob keep_job_;   // what core 0 is told (written before keep_state_ goes to 1: the grab list may move under core 3 meanwhile)
  // calibration (spec 3.4): kCalN clicks kCalGap frames apart from kCalAt; core 0 renders them and listens
  static constexpr int kCalN = 3, kCalGap = 9600, kCalClick = 24, kCalWindow = 4800;
  uint64_t cal_at_ = 0;      // the first click's render frame (0: none)
  int32_t cal_found_[kCalN];   // the round trip found for each click (frames), -1 not yet
  uint32_t cal_peak_[kCalN];   // the largest |sample| back in the kCalPeakWin frames from each click's onset (what the Rytm's POST return made of it)
  static constexpr int kCalPeakWin = 96;
  int32_t cal_floor_ = 0;
  uint8_t cal_state_ = 0;    // 0 idle, 1 clicks on their way
  bool via_on_ = false;   // LIVE VIA has set a macro that is not back at full yet
  int via_macro_ = 0;

  // ------------------------------------------------------------------------------------------------ the store
  CORE_COLD bool protected_id(uint32_t id) { return (lp_.on && lp_.grab_id == id) || (lpp_.on && lpp_.grab_id == id) || (keep_state_ && keep_id_ == id); }
  CORE_COLD void list_edit(bool on) { __atomic_store_n(&list_seq_, list_seq_ + 1, on ? __ATOMIC_RELAXED : __ATOMIC_RELEASE), __atomic_thread_fence(__ATOMIC_SEQ_CST); }
  CORE_COLD void drop_at(int i) {
    list_edit(true);
    if (cp_.on && cp_.id == g_[i].id) cp_.on = false;
    for (int j = i; j + 1 < ng_; ++j) g_[j] = g_[j + 1], g_off_[j] = g_off_[j + 1], g_k0_[j] = g_k0_[j + 1];
    --ng_;
    ++stats_.dropped;
    list_edit(false);
  }
  CORE_COLD bool drop_oldest() {   // the oldest that is not playing or copying
    for (int i = ng_ - 1; i >= 0; --i)
      if (!protected_id(g_[i].id) && !(cp_.on && cp_.id == g_[i].id)) {
        drop_at(i);
        return true;
      }
    return false;
  }
  CORE_COLD bool keeps_extent(int i) { return protected_id(g_[i].id) || (cp_.on && cp_.id == g_[i].id); }
  // first fit among the grabs' extents (only those that cannot be dropped when `fixed`); the place in *off
  CORE_COLD bool first_fit(uint64_t bytes, uint64_t* off, bool fixed) {
    uint64_t pos = 0;
    for (;;) {
      uint64_t end_of_overlap = pos;
      bool clash = false;
      for (int i = 0; i < ng_; ++i) {
        if (fixed && !keeps_extent(i)) continue;
        uint64_t a = g_off_[i], b = a + extent(g_[i].frames);
        if (a < pos + bytes && b > pos) clash = true, end_of_overlap = b > end_of_overlap ? b : end_of_overlap;
      }
      if (!clash) {
        if (pos + bytes > store_bytes_) return false;
        *off = pos;
        return true;
      }
      pos = (end_of_overlap + 15) & ~15ull;
    }
  }
  // whether the grab fits once every droppable grab is gone (checked before dropping any)
  CORE_COLD bool room_possible(uint64_t bytes) {
    uint64_t o;
    return first_fit(bytes, &o, true);
  }
  CORE_COLD bool find_room(uint64_t bytes, uint64_t* off) {
    // first fit among the grabs' extents (by offset); false: no gap
    for (;;) {
      uint64_t pos = 0;
      for (;;) {
        uint64_t end_of_overlap = pos;
        bool clash = false;
        for (int i = 0; i < ng_; ++i) {
          uint64_t a = g_off_[i], b = a + extent(g_[i].frames);
          if (a < pos + bytes && b > pos) clash = true, end_of_overlap = b > end_of_overlap ? b : end_of_overlap;
        }
        if (!clash) {
          if (pos + bytes <= store_bytes_) {
            *off = pos;
            return true;
          }
          break;
        }
        pos = (end_of_overlap + 15) & ~15ull;
      }
      if (!drop_oldest()) return false;
    }
  }

  // ------------------------------------------------------------------------------------------------ the grabs
  // ------------------------------------------------------------------------------------------------ the grab's limiter
  // Run once on each grab as it is copied to the store (core 3, never in the audio thread): a stereo-linked look-ahead peak limiter that
  // takes the loop's peaks down by at most 6 dB toward kThr, the level at which the loop, played with the make-up OUT has now at LEV 100,
  // reaches kLoopCeil. It does not touch the audio: it writes a gain envelope next to it (one uint16 every kEnvStep frames, linearly
  // interpolated by the audio thread), so the store keeps the grab as it was (KEEP saves the original). The loop's peak (what the cap uses)
  // is measured after the envelope, as it is played. Look-ahead kLimL frames (1.5 ms): the gain is the boxcar (kLimL) of the sliding minimum
  // (kLimL + 1) of what each frame needs, so it reaches its target before the peak and moves at most 0.5 / kLimL a frame; the release is
  // exponential (tau 20 ms, ~60 ms to settle). Each envelope point is the lowest gain within kEnvStep - 1 frames around it, so the
  // interpolation never gives a frame more gain than the limiter did. Under the threshold the envelope is exactly 1.
  static constexpr int kLimL = 72;
  static constexpr float kLimRel = 1.0412e-3f;   // 1 - exp(-1 / (0.020 s x 48000))
  struct Limiter {
    float thr;                  // store units
    uint32_t t;                 // frames fed
    float dv[kLimL + 2];        // the sliding minimum's deque: values and frame numbers (a ring)
    uint32_t di[kLimL + 2];
    int dh, dn;
    float ring[kLimL];          // the last kLimL window minima (the boxcar)
    double sum;
    float gf;                   // the gain now
    float gbuf[32];             // the gain of the last 32 frames out (for the envelope's points)
    const int32_t* base;        // the copy (frame 0 = its first pre-roll frame), read only
    uint16_t* env;
    uint32_t total, next_pt, pko;   // frames; the next envelope point to set; the first frame whose peak is not measured yet
    uint32_t peak;              // the largest |sample| as played (after the envelope) of the frames measured
  } lim_;
  // the envelope's gain at store frame i (also what the audio thread does)
  inline float env_gain(const uint16_t* e, uint32_t i) {
    const uint32_t j = i / kEnvStep;
    const float a = (float)e[j] * (1.0f / kEnvOne), b = (float)e[j + 1] * (1.0f / kEnvOne);
    return a + (b - a) * ((float)(i % kEnvStep) * (1.0f / kEnvStep));
  }
  CORE_COLD void lim_begin(const int32_t* base, uint16_t* env, uint32_t total) {
    float mk = makeup();
    lim_.thr = mk > 0.0f ? kLoopCeil * 2147483648.0f / mk : 3.0e9f;
    lim_.t = 0, lim_.dh = lim_.dn = 0, lim_.sum = (double)kLimL, lim_.gf = 1.0f, lim_.peak = 0;
    lim_.base = base, lim_.env = env, lim_.total = total, lim_.next_pt = 0, lim_.pko = 0;
    for (int i = 0; i < kLimL; ++i) lim_.ring[i] = 1.0f;
    for (int i = 0; i < 32; ++i) lim_.gbuf[i] = 1.0f;
  }
  CORE_COLD void lim_peaks(uint32_t to) {   // the played peak of frames pko .. to - 1 (their envelope points are set)
    Limiter& L = lim_;
    uint32_t pk = L.peak;
    while (L.pko < to) {
      const uint32_t j = L.pko / kEnvStep, e = (j + 1) * kEnvStep < to ? (j + 1) * kEnvStep : to;   // this segment's frames
      const uint16_t ea = L.env[j], eb = L.env[j + 1];
      const float a = (float)ea * (1.0f / kEnvOne), b = (float)eb * (1.0f / kEnvOne);
      for (uint32_t i = L.pko; i < e; ++i) {
        const int32_t* x = L.base + 2 * (size_t)i;
        const uint32_t ax = x[0] < 0 ? 0u - (uint32_t)x[0] : (uint32_t)x[0], ay = x[1] < 0 ? 0u - (uint32_t)x[1] : (uint32_t)x[1];
        uint32_t m = ax > ay ? ax : ay;
        if (ea != (uint16_t)kEnvOne || eb != (uint16_t)kEnvOne) {   // (unity: the sample itself)
          const float g = a + (b - a) * ((float)(i % kEnvStep) * (1.0f / kEnvStep));   // (as env_gain)
          const float p = (float)m * g;
          m = p >= 4294967040.0f ? 0xFFFFFF00u : (uint32_t)p;
        }
        pk = m > pk ? m : pk;
      }
      L.pko = e;
    }
    L.peak = pk;
  }
  CORE_COLD void lim_point(uint32_t j, uint32_t hi_cap) {   // envelope point j: the lowest gain of frames 16j - 15 .. 16j + 15 (those that exist)
    Limiter& L = lim_;
    const int64_t lo = (int64_t)j * kEnvStep - (kEnvStep - 1), hi = (int64_t)j * kEnvStep + (kEnvStep - 1);
    const int64_t a = lo < 0 ? 0 : lo, b = hi < (int64_t)hi_cap ? hi : (int64_t)hi_cap - 1;
    float m = L.gf;
    for (int64_t o = a; o <= b; ++o) m = L.gbuf[o & 31] < m ? L.gbuf[o & 31] : m;
    L.env[j] = (uint16_t)lrintf(m * kEnvOne);
  }
  // one frame in (amp: its larger |sample|); the gain of the frame kLimL behind it comes out
  CORE_COLD void lim_step(float amp) {
    Limiter& L = lim_;
    float r = 1.0f;
    if (amp > L.thr) {
      r = L.thr / amp;
      if (r < 0.5f) r = 0.5f;   // at most 6 dB
    }
    const uint32_t t = L.t++;
    while (L.dn && L.dv[(L.dh + L.dn - 1) % (kLimL + 2)] >= r) --L.dn;
    const int at = (L.dh + L.dn) % (kLimL + 2);
    L.dv[at] = r, L.di[at] = t, ++L.dn;
    while (L.di[L.dh] + (uint32_t)kLimL < t) L.dh = (L.dh + 1) % (kLimL + 2), --L.dn;
    const float gmin = L.dv[L.dh];
    float& slot = L.ring[t % kLimL];
    L.sum += (double)gmin - (double)slot;
    slot = gmin;
    float g = (float)(L.sum / kLimL);
    if (g > 1.0f) g = 1.0f;
    const float up = L.gf + (1.0f - L.gf) * kLimRel;
    L.gf = g < up ? g : up;
    if (L.gf > 1.0f - 1e-4f) L.gf = 1.0f;   // (back to exactly unity after a release: a loop with a quiet tail has an envelope of exactly 1 there)
    if (t < (uint32_t)kLimL) return;
    const uint32_t o = t - kLimL;   // the frame this gain is for
    L.gbuf[o & 31] = L.gf;
    if (o % kEnvStep == kEnvStep - 1) {   // 16j + 15: point j can be set (its window is complete); the segment before it can be measured
      lim_point(L.next_pt, o + 1);
      if (L.next_pt > 0) lim_peaks(L.next_pt * kEnvStep);
      ++L.next_pt;
    }
  }
  CORE_COLD void lim_feed(uint32_t from, uint32_t n) {   // frames from .. from + n - 1 of the copy
    for (uint32_t i = from; i < from + n; ++i) {
      int32_t a = lim_.base[2 * (size_t)i], b = lim_.base[2 * (size_t)i + 1];
      float ua = (float)(a < 0 ? 0u - (uint32_t)a : (uint32_t)a), ub = (float)(b < 0 ? 0u - (uint32_t)b : (uint32_t)b);
      lim_step(ua > ub ? ua : ub);
    }
  }
  CORE_COLD void lim_flush() {   // the last kLimL frames' gains, the points still to set, the rest of the peak
    Limiter& L = lim_;
    for (int i = 0; i < kLimL; ++i) lim_step(0.0f);
    const uint32_t pts = env_pts(L.total - kPre);
    for (; L.next_pt < pts; ++L.next_pt) lim_point(L.next_pt, L.total);
    lim_peaks(L.total);
  }

  CORE_COLD void make_loop(int gi, uint64_t start, bool replace) {
    const Grab& g = g_[gi];
    uint64_t now = frames_now();
    // what keeps sounding until the new loop starts: the loop playing now, or - the new one has not started yet (a second
    // grab or a tap in the wait) - the one that has been playing under it all along
    if (replace && lp_.on && lp_.start > now && lpp_.on) {
      // lpp_ and snap_.prev stay
    } else if (replace && lp_.on && lp_.start <= now) {   // (a loop that has not started is not heard: nothing to keep under the new one)
      lpp_ = lp_, snap_.prev = snap_.cur;
    } else {
      lpp_ = LoopInfo(), snap_.prev = LpSnap();
    }
    snap_.cur = LpSnap{++loop_ids_, true, !g.copied, g.copied ? g.pcm : nullptr, g_k0_[gi], g.frames, start, 0, 0, g.peak, g.copied ? g.env : nullptr, g.peak};
    lp_ = {true, g.id, start, g.frames, 0, 0, g.half_bars};
    publish();
  }

  // the render frame of the first grid line after `now` that is far enough ahead for the audio thread to take the command
  CORE_COLD uint64_t next_line(const Map& m, double grid, uint64_t now) {
    // (rendered post_lat frames early: a POST path slower than the voice path's still reaches the Rytm's output on the beat)
    double k = floor(((double)now + 96.0 + (double)post_lat_ - m.origin) / grid) + 1.0;
    return (uint64_t)(m.origin + k * grid - (double)post_lat_);
  }

  CORE_COLD void finalize(uint64_t now) {
    Map m;
    if (!get_map(&m, now)) {
      pend_.on = false;
      ++stats_.refused;
      return;
    }
    int hb = pend_.half_bars;
    double g = pend_.grid;
    int64_t len = (int64_t)(hb * 8 * m.per16 + 0.5);
    double a_e = arrival(m, pend_.E, g);
    uint64_t start_h = (uint64_t)(a_e - (double)len);
    uint64_t k0;
    if (!ring_.index_at(start_h, &k0) || k0 < (uint64_t)kPre) {
      pend_.on = false;
      ++stats_.refused;
      return;
    }
    uint64_t bytes = extent((uint32_t)len);
    uint64_t off;
    if (!room_possible(bytes)) {   // not even with the others gone: none is dropped for it
      pend_.on = false;
      ++stats_.refused;
      say("NO ROOM");
      return;
    }
    if (ng_ >= kMaxGrabs && !drop_oldest()) {
      pend_.on = false;
      ++stats_.refused;
      return;
    }
    if (!find_room(bytes, &off)) {
      pend_.on = false;
      ++stats_.refused;
      return;
    }
    list_edit(true);
    for (int j = ng_; j > 0; --j) g_[j] = g_[j - 1], g_off_[j] = g_off_[j - 1], g_k0_[j] = g_k0_[j - 1];
    ++ng_;
    Grab& n = g_[0];
    n = Grab{};
    n.id = next_id_++;
    n.frames = (uint32_t)len;
    n.half_bars = (uint8_t)hb;
    n.bpm = (float)(60.0 * 48000.0 / (4.0 * m.per16));
    n.start = start_h;
    n.gap = ring_.gap_in(start_h, (uint32_t)len) ? 1 : 0;
    g_off_[0] = off;
    g_k0_[0] = k0;
    list_edit(false);
    cp_ = {true, n.id, k0 - (uint64_t)kPre, 0, (uint32_t)(len + kPre), 0};
    lim_begin((const int32_t*)(store_ + g_off_[0]), (uint16_t*)(store_ + g_off_[0] + env_off(n.frames)), (uint32_t)(len + kPre));
    ++stats_.grabs;
    bool replace = lp_.on && (lp_.stop_at == 0 || now < lp_.stop_end);
    if (replace) ++stats_.resamples;
    // the loop starts at the next grid line (bar line for every length but the half bar's grid)
    make_loop(0, next_line(m, g, now), replace);
    pend_.on = false;
  }

  CORE_COLD void pump_pending(uint64_t now) {
    if (!pend_.on || cp_.on) return;
    Map m;
    if (!get_map(&m, now)) {
      pend_.on = false;
      ++stats_.refused;
      return;
    }
    double a_e = arrival(m, pend_.E, pend_.grid);
    uint64_t k;
    if ((double)now < a_e) return;
    // the ring has every frame up to that line's: its index is written
    int64_t offs;
    if (!ring_.offset(&offs)) return;
    k = (uint64_t)((int64_t)a_e - offs);
    if (ring_.written() < k) return;
    finalize(now);
  }

  CORE_COLD void pump_copy() {
    if (!cp_.on) return;
    int gi = find_id(cp_.id);
    if (gi < 0) {
      cp_.on = false;
      return;
    }
    uint32_t n = cp_.total - cp_.done;
    if (n > kPieceBytes / 8) n = kPieceBytes / 8;
    uint64_t w = ring_.written();
    bool lost = cp_.k >= w || w - cp_.k + 8 > ring_.capacity();   // the writer would be on the frames we copy (the press rule's margin leaves room for the rest)
    int32_t* dst = (int32_t*)(store_ + g_off_[gi]) + 2 * (size_t)cp_.done;
    uint32_t got = lost ? 0 : ring_.copy_idx(cp_.k, n, dst);
    if (got != n) {   // overtaken: abandoned, the grab refused; a loop playing from it goes (cut)
      ++stats_.abandoned;
      bool was_playing = lp_.on && lp_.grab_id == cp_.id;
      bool was_under = lpp_.on && lpp_.grab_id == cp_.id;   // the loop a waiting resample replaces
      cp_.on = false;
      if (was_under) {
        snap_.prev.active = false;
        lpp_ = LoopInfo();
        publish();
      }
      if (was_playing) {
        snap_.cur.active = false;
        publish();
        lp_.on = false;
      }
      int i = find_id(cp_.id);
      if (i >= 0) {
        list_edit(true);
        for (int j = i; j + 1 < ng_; ++j) g_[j] = g_[j + 1], g_off_[j] = g_off_[j + 1], g_k0_[j] = g_k0_[j + 1];
        --ng_;
        list_edit(false);
      }
      return;
    }
    if (n * 8 > stats_.worst_copy_bytes) stats_.worst_copy_bytes = n * 8;
    {   // the loop's peak (L or R) for the level cap: the piece is in the cache, one pass over it
      uint32_t pk = cp_.peak;
      for (uint32_t i = 0; i < 2 * n; ++i) {
        int32_t x = dst[i];
        uint32_t a = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
        pk = a > pk ? a : pk;
      }
      cp_.peak = pk;
      g_[gi].peak = pk;
      if (lp_.on && lp_.grab_id == cp_.id && snap_.cur.from_ring && snap_.cur.active && snap_.cur.peak != pk) {   // a loop playing from the ring: so far
        snap_.cur.peak = pk, snap_.cur.peak_unl = pk;
        publish();
      }
    }
    lim_feed(cp_.done, n);
    cp_.k += n, cp_.done += n;
    if (cp_.done >= cp_.total) {
      Grab& g = g_[gi];
      lim_flush();
      g.peak = lim_.peak;   // (the grab's own, as played through the envelope: the cap gives the limited loop more gain)
      g.env = (const uint16_t*)(store_ + g_off_[gi] + env_off(g.frames));
      g.pcm = (int32_t*)(store_ + g_off_[gi]) + 2 * kPre;
      g.copied = 1;
      cp_.on = false;
      ++stats_.copies;
      if (lp_.on && lp_.grab_id == g.id && snap_.cur.from_ring && snap_.cur.active) {   // the loop goes on from the store, the same frames
        snap_.cur.from_ring = false;
        snap_.cur.pcm = g.pcm;
        snap_.cur.peak = g.peak;
        snap_.cur.peak_unl = cp_.peak;   // (the unlimited one: the envelope fades in against it)
        snap_.cur.env = g.env;
        publish();
      }
    }
  }


  // ------------------------------------------------------------------------------------------------ the public side
  CORE_COLD void set_env(const Env* e) { env_ = e ? *e : Env(); }

  CORE_COLD void init(int32_t* ring, uint32_t ring_frames, uint8_t* store, uint64_t store_bytes) {
    if (!SIDECAR_LOOPER) ring = nullptr, store = nullptr, store_bytes = 0;   // 0.5.2: the looper is off (sidecar_features.h): no ring, no store, on_ stays false and nothing below acts
    on_ = ring && ring_frames && store && store_bytes;
    store_ = store, store_bytes_ = store_bytes;
    if (ring && ring_frames) ring_.init(ring, ring_frames);
    seq::set_clock_tracks(seq::kClockLoop, on_ ? 1 : 0);   // BD's ticks: the looper's bars (spec 3.4)
    reset();
  }

  CORE_COLD void reset() {
    ng_ = 0, next_id_ = 1, pend_ = Pending(), cp_ = Copy(), stats_ = Stats(), lp_ = LoopInfo(), lpp_ = LoopInfo(), am_ = AutoMute(), via_on_ = false;
    snap_ = Snap();
    bal_ = 64, lev_ = 100, fade_ = kFade1, live_via_ = 0, mod_bal_ = mod_lev_ = -1, auto_mute_ = false;
    __atomic_store_n(&out_, (int)kOutPost, __ATOMIC_RELAXED), __atomic_store_n(&postgain_bits_, 0u, __ATOMIC_RELAXED);
    dirty_ = false, post_lat_ = 0, keep_state_ = 0, cal_state_ = 0, cal_at_ = 0, msg_[0] = 0, msg_gen_ = 0, keep_lines_.n = 0;
    core0_reset();
    if (on_) publish();
    if (ring_.ready()) ring_.init(ring_.data(), ring_.capacity());
  }

  CORE_COLD bool on() { return on_; }
  bool wants_in() { return on_; }

  CORE_COLD int grab(int half_bars) {
    if (!on_) return kErrOff;
    if (half_bars != kHalf && half_bars != k1 && half_bars != k2 && half_bars != k4 && half_bars != k8 && half_bars != k16)
      return kErrBadArg;
    uint64_t now = frames_now();
    Map m;
    if (!get_map(&m, now)) {
      ++stats_.refused;
      return kErrStopped;
    }
    double grid = half_bars == kHalf ? 8 * m.per16 : 16 * m.per16;
    double E = floor(((double)now - m.origin) / grid);   // the last grid line (render-domain); its audio comes lat later
    if (E < 0) {
      ++stats_.refused;
      return kErrStopped;
    }
    double len = (double)half_bars * 8 * m.per16;
    double a_e = arrival(m, E, grid);
    double at = (double)now > a_e ? (double)now : a_e;   // a press that waits: measured from the line's arrival
    // spec 3.3: it must fit with what has been written since its first frame: the grab, the part of the grid in progress,
    // the round trip, the copy's time (a 256 KB piece a control pass, taken as 0.1 ms) and a 0.5 s margin
    double cap = (double)ring_.capacity();
    double copy = (len + kPre) * 8 / (double)kPieceBytes * kPieceFrames;   // frames of the copy (a piece a control pass: kPieceFrames)
    if (at - (a_e - len) + m.lat + copy + 24000.0 > cap) {
      // the BPM a grab of this many bars needs at the worst point of the bar (a whole bar in progress)
      double bars = (double)half_bars / 2.0, room = cap - m.lat - 24000.0 - copy;
      int need = (int)ceil(240.0 * 48000.0 * (bars + 1.0) / room);   // 4 beats a bar: bpm = 240 s x (bars + 1) / (room / 48000)
      need_bpm_ = need;
      ++stats_.refused;
      return kErrTooLong;
    }
    uint64_t k;
    if (!ring_.index_at((uint64_t)(a_e - len - kPre) > 0 ? (uint64_t)(a_e - len - kPre) : 0, &k)) {
      ++stats_.refused;
      return kErrNoRoom;   // (the ring has not got that far back: just started)
    }
    uint64_t bytes = extent((uint32_t)(len + 0.5));
    if (bytes > store_bytes_) {
      ++stats_.refused;
      return kErrNoRoom;
    }
    pend_ = {true, half_bars, E, grid, now};
    pump_pending(now);
    return kOk;
  }

  CORE_COLD const char* err_text(int err, int half_bars, char* b, int cap) {
    switch (err) {
      case kErrStopped: sniprintf(b, cap, "PLAY FIRST"); break;
      case kErrTooLong: sniprintf(b, cap, "%d BARS NEEDS %d BPM", half_bars / 2, need_bpm_); break;
      case kErrNoRoom: sniprintf(b, cap, "NO ROOM"); break;
      case kErrOff: sniprintf(b, cap, "LOOPER OFF"); break;
      default: sniprintf(b, cap, "BAD LENGTH"); break;
    }
    return b;
  }

  // auto-mute and LIVE VIA follow the loop: on at its start, off at the start of its fade back (or cut)
  CORE_COLD void live_and_mutes(uint64_t now) {
    if (!SIDECAR_LOOPER) return;
    if (!device::rytm_at(slot_)) return;   // the Rytm's track mutes and PERF macros: a Rytm's looper only (another box: a named gap)   // 0.5.2: off (sidecar_features.h): never a Rytm track mute, never a LIVE VIA move
    auto heard = [&](const LoopInfo& x) { return x.on && now >= x.start && !(x.stop_at && now >= x.stop_at); };
    bool audible = heard(lp_) || heard(lpp_);   // (a resample's wait: the old loop is still heard)
    if (audible && auto_mute_ && !am_.on) {
      uint16_t before = muted_now();
      am_ = {true, (uint16_t)(~before & 0xFFF), 0};
      if (am_.set) mod::rytm_mute(am_.set, 0);
    }
    if (am_.on) {
      uint16_t cur = muted_now();
      am_.seen |= am_.set & cur;
      uint16_t dropped = am_.seen & ~cur;   // a mute we made and the Rytm showed, now gone: the user's
      am_.set &= ~dropped, am_.seen &= ~dropped;
      if (!audible) {
        if (am_.set) mod::rytm_mute(0, am_.set);
        am_ = AutoMute();
      }
    }
    if (live_via_ && audible) {
      via_macro_ = live_via_, via_on_ = true;
      mod::rytm_perf(live_via_, live_perf(bal_now()));
    } else if (via_on_ && !audible) {   // the loop is leaving: the live comes back at once
      mod::rytm_perf(via_macro_, 127);
      via_on_ = false;
    }
  }

  CORE_COLD void tick(uint32_t us) {
    (void)us;
    if (!SIDECAR_LOOPER || !on_) return;
    seq::set_clock_tracks(seq::kClockLoop, 1);   // (seq::init in engine_begin resets the users: asked again each pass, a store)
    uint64_t now = frames_now();
    pump_pending(now);
    pump_copy();
    if (lp_.on && lp_.stop_at && now >= lp_.stop_end) lp_.on = false;   // faded out: the ring alone
    if (lpp_.on && (!lp_.on || now >= lp_.start + (uint64_t)kSeam)) lpp_ = LoopInfo(), snap_.prev = LpSnap();   // the swap is done
    live_and_mutes(now);
    cal_tick(now);
    keep_tick();
  }

  CORE_COLD bool play(int g) {
    if (!on_ || g < 0 || g >= ng_) return false;
    uint64_t now = frames_now();
    Map m;
    if (!get_map(&m, now)) return false;
    double grid = g_[g].half_bars == kHalf ? 8 * m.per16 : 16 * m.per16;
    bool replace = lp_.on && (lp_.stop_at == 0 || now < lp_.stop_end);
    make_loop(g, next_line(m, grid, now), replace);
    return true;
  }
  CORE_COLD int playing() {
    if (!lp_.on) return -1;
    int i = find_id(lp_.grab_id);
    return i;
  }
  CORE_COLD bool playing_info(uint64_t* start, uint32_t* len) {
    if (!lp_.on) return false;
    *start = lp_.start, *len = lp_.len;
    return true;
  }
  CORE_COLD bool bar_position(float* bar, float* of_bars) {
    uint64_t now = frames_now();
    if (!lp_.on || now < lp_.start || lp_.len == 0) return false;
    double b = lp_.half_bars / 2.0;
    *of_bars = (float)b;
    *bar = (float)(((now - lp_.start) % lp_.len) / (double)lp_.len * b);
    return true;
  }
  CORE_COLD int grabs() { return ng_; }
  CORE_COLD int grab_index(uint32_t id) { return find_id(id); }
  CORE_COLD const Grab* grab_at(int i) { return i >= 0 && i < ng_ ? &g_[i] : nullptr; }
  CORE_COLD bool pending() { return pend_.on; }
  CORE_COLD const Stats& stats() { return stats_; }

  CORE_COLD State state() {
    if (!lp_.on) return kRing;
    uint64_t now = frames_now();
    if (lp_.stop_at) return now >= lp_.stop_end ? kRing : kFading;
    return now < lp_.start ? kWait : kPlaying;
  }

  // ------------------------------------------------------------------------------------------------ settings, levels, stop
  CORE_COLD int clamp127(int v) { return v < 0 ? 0 : v > 127 ? 127 : v; }
  CORE_COLD void set_bal(int v) { bal_ = clamp127(v), dirty_ = true, retarget(); }
  CORE_COLD void set_lev(int v) { lev_ = clamp127(v), dirty_ = true, retarget(); }
  CORE_COLD void set_fade(int f) { fade_ = f < 0 ? 0 : f >= kFades ? kFades - 1 : f, dirty_ = true; }
  CORE_COLD void set_auto_mute(bool on) { if (SIDECAR_LOOPER) auto_mute_ = on, dirty_ = true; }
  CORE_COLD void set_live_via(int perf) { if (SIDECAR_LOOPER) live_via_ = perf < 0 ? 0 : perf > 12 ? 12 : perf, dirty_ = true; }
  CORE_COLD int bal() { return bal_; }
  CORE_COLD int lev() { return lev_; }
  CORE_COLD int fade() { return fade_; }
  CORE_COLD bool auto_mute() { return auto_mute_; }
  CORE_COLD int live_via() { return live_via_; }
  CORE_COLD void set_bal_mod(int v) { mod_bal_ = v < 0 ? -1 : clamp127(v), retarget(); }
  CORE_COLD void set_lev_mod(int v) { mod_lev_ = v < 0 ? -1 : clamp127(v), retarget(); }
  CORE_COLD int bal_eff() { return bal_now(); }
  CORE_COLD int lev_eff() { return lev_now(); }

  // spec 3.8: 0..64 the loop rises from 0 to full (equal power: sin), 64..127 the Sidecar's own output falls from full
  // to 0 (cos); the other side full
  static CORE_COLD float loop_gain(int b) { return b >= 64 ? 1.0f : sinf(1.5707963f * (float)b / 64.0f); }
  static CORE_COLD float own_gain(int b) { return b <= 64 ? 1.0f : b >= 127 ? 0.0f : cosf(1.5707963f * (float)(b - 64) / 63.0f); }
  static CORE_COLD int live_perf(int b) { return b <= 64 ? 127 : 127 - (b - 64) * 127 / 63; }

  // the FADE setting in bars: 1/4 1/2 1 2 4
  CORE_COLD double fade_bars() {
    static const double kB[kFades] = {0.25, 0.5, 1, 2, 4};
    return kB[fade_];
  }

  CORE_COLD void stop(bool cut, bool now) {
    if (!on_ || !lp_.on) return;
    uint64_t t = frames_now();
    // a resample waiting for its bar line: what is heard is the old loop; the new one is dropped and the stop is the old one's
    if (lp_.start > t && lpp_.on) {
      lp_ = lpp_, snap_.cur = snap_.prev;
      lpp_ = LoopInfo(), snap_.prev = LpSnap();
    }
    Map m;
    bool have = get_map(&m, t);
    uint64_t at = t + 96;   // (far enough ahead for the audio thread to take it)
    if (!now && have) at = next_line(m, 16.0 * m.per16, t);
    uint32_t len = cut || !have ? (uint32_t)kCutFrames : (uint32_t)(fade_bars() * 16.0 * m.per16 + 0.5);
    if (len < (uint32_t)kCutFrames) len = kCutFrames;
    if (at <= lp_.start) {   // the loop has not started (and nothing is under it): it never will
      snap_.cur.active = false;
      snap_.prev.active = false;
      publish();
      lp_.on = false;
      lpp_ = LoopInfo();
      return;
    }
    snap_.cur.stop_at = at, snap_.cur.stop_len = len;
    lp_.stop_at = at, lp_.stop_end = at + len;
    publish();
  }

  // ------------------------------------------------------------------------------------------------ calibration
  // the click's k-th frame (a half-sine burst, 8.0e8 at the top): out_mix renders it, cal_tick knows its peak
  inline int32_t click_at(uint64_t k) { return (int32_t)(8.0e8f * sinf(3.14159265f * (float)(k + 1) / (kCalClick + 1))); }
  CORE_COLD void cal_start() {
    if (!on_) return;
    for (int i = 0; i < kCalN; ++i) cal_found_[i] = -1, __atomic_store_n(&cal_peak_[i], 0u, __ATOMIC_RELAXED);
    cal_floor_ = 0;
    cal_state_ = 1;
    __atomic_store_n(&cal_at_, frames_now() + 14400, __ATOMIC_RELEASE);   // 300 ms: the floor is measured first
  }
  CORE_COLD bool cal_wanted() { return cal_state_ == 1; }
  CORE_COLD bool cal_info(uint64_t* first, int* n, uint32_t* gap) {
    uint64_t a = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
    if (!a) return false;
    *first = a, *n = kCalN, *gap = kCalGap;
    return true;
  }
  CORE_COLD int post_lat() { return post_lat_; }
  CORE_COLD void set_post_lat(int f) { post_lat_ = f, dirty_ = true; }
  CORE_COLD void cal_tick(uint64_t now) {
    if (cal_state_ != 1) return;
    uint64_t a = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
    if (now < a + (uint64_t)kCalN * kCalGap + kCalWindow) return;
    int v[kCalN], n = 0;
    for (int i = 0; i < kCalN; ++i) {
      int32_t f = __atomic_load_n(&cal_found_[i], __ATOMIC_ACQUIRE);
      if (f >= 0) v[n++] = f;
    }
    cal_state_ = 0;
    __atomic_store_n(&cal_at_, 0, __ATOMIC_RELEASE);
    if (n < 2) {
      ++stats_.cal_fail;
      say("CAL: NO CLICK HEARD");
      return;
    }
    for (int i = 1; i < n; ++i)   // the median
      for (int j = i; j > 0 && v[j] < v[j - 1]; --j) { int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    double o, p16;
    int lat = 0;
    if (!beat_map(&o, &p16, &lat)) {   // no transport: its latency is not the voice path's to compare with
      ++stats_.cal_fail;
      say("CAL: PLAY FIRST");
      return;
    }
    post_lat_ = v[n / 2] - lat;
    dirty_ = true;
    ++stats_.cal_ok;
    char b[40];
    sniprintf(b, sizeof b, "POST LAT %d", post_lat_);
    // the POST return's gain: the click's peak out / the peak back, each click that came back (the median); kept when it is sane
    int32_t cp = 0;
    for (int k = 0; k < kCalClick; ++k) cp = click_at((uint64_t)k) > cp ? click_at((uint64_t)k) : cp;
    float gv[kCalN];
    int ng = 0;
    for (int i = 0; i < kCalN; ++i) {
      uint32_t pk = __atomic_load_n(&cal_peak_[i], __ATOMIC_ACQUIRE);
      if (__atomic_load_n(&cal_found_[i], __ATOMIC_ACQUIRE) >= 0 && pk > 0) gv[ng++] = (float)cp / (float)pk;
    }
    if (ng > 0) {
      for (int i = 1; i < ng; ++i)
        for (int j = i; j > 0 && gv[j] < gv[j - 1]; --j) { float t = gv[j]; gv[j] = gv[j - 1]; gv[j - 1] = t; }
      float g = gv[ng / 2];
      if (g >= 1.5f && g <= 6.0f) {
        set_postgain(g);
        int g100 = (int)(g * 100.0f + 0.5f), db10 = (int)(20.0f * log10f(g) * 10.0f + 0.5f);
        sniprintf(b, sizeof b, "POST LAT %d X%d.%02d %d.%dDB", post_lat_, g100 / 100, g100 % 100, db10 / 10, db10 % 10);
      }
    }
    say(b);
  }

  // ------------------------------------------------------------------------------------------------ KEEP
  CORE_COLD bool keep(int g, char* msg, int cap) {
    const Grab* gr = grab_at(g);
    if (!on_ || !gr) {
      sniprintf(msg, cap, "NO GRAB");
      return false;
    }
    if (gr->kept) {
      sniprintf(msg, cap, "ALREADY KEPT");
      return false;
    }
    if (!gr->copied) {
      sniprintf(msg, cap, "COPYING: WAIT");
      return false;
    }
    if (keep_state_) {
      sniprintf(msg, cap, "KEEP BUSY");
      return false;
    }
    keep_id_ = gr->id;
    keep_job_ = KeepJob{gr->id, gr->frames, gr->pcm, gr->bpm, gr->half_bars, gr->start};
    __atomic_store_n(&keep_state_, 1, __ATOMIC_RELEASE);
    sniprintf(msg, cap, "KEEPING");
    return true;
  }
  CORE_COLD bool keep_take(KeepJob* j) {   // (core 0)
    if (__atomic_load_n(&keep_state_, __ATOMIC_ACQUIRE) != 1) return false;
    *j = keep_job_;
    __atomic_store_n(&keep_state_, 2, __ATOMIC_RELEASE);
    return true;
  }
  // core 0 reports through a slot; core 3's tick applies it (the grab list is core 3's)
  struct KeepRes {
    uint32_t id;
    bool ok;
    char name[32];
  } keep_res_;
  CORE_COLD void keep_finished(uint32_t id, bool ok, const char* name) {   // (core 0)
    keep_res_.id = id, keep_res_.ok = ok;
    sniprintf(keep_res_.name, sizeof keep_res_.name, "%s", name ? name : "");
    __atomic_store_n(&keep_state_, 3, __ATOMIC_RELEASE);
  }
  CORE_COLD void keep_tick() {
    if (__atomic_load_n(&keep_state_, __ATOMIC_ACQUIRE) != 3) return;
    int i = find_id(keep_res_.id);
    if (keep_res_.ok && i >= 0) g_[i].kept = 1, ++stats_.kept;
    char b[48];
    if (keep_res_.ok) sniprintf(b, sizeof b, "KEPT %s", keep_res_.name);
    else sniprintf(b, sizeof b, "KEEP FAILED");
    say(b);
    __atomic_store_n(&keep_state_, 0, __ATOMIC_RELEASE);
  }

  // ------------------------------------------------------------------------------------------------ the card (loop.txt)

  CORE_COLD bool dirty() { return dirty_; }
  CORE_COLD void load() {
    if (!SIDECAR_LOOPER) return;   // 0.5.2: off (sidecar_features.h): a loop.txt on the card is left alone and turns nothing on
    keep_lines_.n = 0;
    fmt::post_wait(kFile);
    fmt::recover(kFile);
    File f;
    if (!f.open(kFile, kRead)) return;
    static char b[512];
    memset(b, 0, sizeof b);
    int got = f.read(b, sizeof b - 1);
    f.close();
    take_text(b, got);
  }
  // loop.txt's text into this looper's settings (load; another slot's at its box's attach: core/pool/pool.cpp kSettings)
  CORE_COLD void take_text(const char* t, int got) {
    if (!SIDECAR_LOOPER) return;
    keep_lines_.n = 0;
    static char b[512];
    if (got < 0) got = 0;
    if (got > (int)sizeof b - 1) got = (int)sizeof b - 1;
    if (t != b) memcpy(b, t, (size_t)got);
    b[got] = 0;
    static const char* const kKnown[] = {"bal", "lev", "fade", "mute", "live", "postlat", "out", "postgain", nullptr};
    if (got > 0) fmt::kv_keep(b, (uint32_t)got, kKnown, &keep_lines_);
    auto num = [&](const char* key, int* out) {   // parsed by hand: the Pi's bare-metal build has no strtol
      size_t kl = strlen(key);
      for (const char* l = b; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : nullptr) {
        if (strncmp(l, key, kl) || l[kl] != '=') continue;
        const char* e = l + kl + 1;
        bool neg = *e == '-';
        if (neg) e++;
        const char* d = e;
        int v = 0;
        while (*e >= '0' && *e <= '9' && v < 100000) v = v * 10 + (*e++ - '0');
        if (e != d && (*e == 0 || *e == '\n' || *e == '\r')) {
          *out = neg ? -v : v;
          return true;
        }
      }
      return false;
    };
    int v;
    // out=post|pre (anything else: POST); postgain in thousandths (3508 = x3.508), only 1500..6000 is taken
    for (const char* l = b; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : nullptr) {
      if (strncmp(l, "out=", 4)) continue;
      if (!strncmp(l + 4, "pre", 3) && (l[7] == 0 || l[7] == '\n' || l[7] == '\r')) set_out(kOutPre);
      else if (!strncmp(l + 4, "post", 4) && (l[8] == 0 || l[8] == '\n' || l[8] == '\r')) set_out(kOutPost);
    }
    if (num("postgain", &v) && v >= 1500 && v <= 6000) set_postgain((float)v / 1000.0f);
    if (num("bal", &v)) bal_ = clamp127(v);
    if (num("lev", &v)) lev_ = clamp127(v);
    if (num("fade", &v)) fade_ = v < 0 ? 0 : v >= kFades ? kFades - 1 : v;
    if (num("mute", &v)) auto_mute_ = v != 0;
    if (num("live", &v)) live_via_ = v < 0 ? 0 : v > 12 ? 12 : v;
    if (num("postlat", &v)) post_lat_ = v < -9999 ? -9999 : v > 9999 ? 9999 : v;
    dirty_ = false;
    retarget();
  }
  CORE_COLD bool save() {
    if (!SIDECAR_LOOPER || !dirty_) return false;   // (off: nothing is ever dirty, and the card's loop.txt is never rewritten)
    char* t = fmt::post_begin();
    if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
    int pg = (int)(postgain() * 1000.0f + 0.5f);
    uint32_t n = (uint32_t)sniprintf(t, 160, CORE_STR("bal=%d\nlev=%d\nfade=%d\nmute=%d\nlive=%d\npostlat=%d\nout=%s\n"), bal_, lev_, fade_,
                                     auto_mute_ ? 1 : 0, live_via_, post_lat_, out() == kOutPre ? "pre" : "post");
    if (pg > 0) n += (uint32_t)sniprintf(t + n, 160 - n, CORE_STR("postgain=%d\n"), pg);
    n = fmt::kv_append(keep_lines_, t, n, fmt::kPostMax);
    if (!fmt::post_end(kFile, n)) return false;
    dirty_ = false;
    return true;
  }

  // ------------------------------------------------------------------------------------------------ console and status
  CORE_COLD const char* last_message() { return msg_; }
  CORE_COLD uint32_t message_gen() { return msg_gen_; }
  CORE_COLD int ring_seconds() {
    uint64_t w = ring_.written();
    uint64_t f = w < ring_.capacity() ? w : ring_.capacity();
    return (int)(f / 48000);
  }
  static constexpr const char* kStateName[] = {"ring", "wait", "playing", "fading"};
  static constexpr const char* kFadeName[kFades] = {"1/4", "1/2", "1", "2", "4"};
  CORE_COLD const char* len_name(int hb, char* b, int cap) {
    if (hb == 1) sniprintf(b, cap, "1/2");
    else sniprintf(b, cap, "%d", hb / 2);
    return b;
  }
  CORE_COLD int parse_len(const char* s) {   // "0.5" "1/2" "1" .. "16" -> half bars (0 bad)
    if (!strcmp(s, "0.5") || !strcmp(s, "1/2")) return kHalf;
    int v = 0;
    const char* e = s;
    while (*e >= '0' && *e <= '9') v = v * 10 + (*e++ - '0');
    if (*e || e == s) return 0;
    return v == 1 || v == 2 || v == 4 || v == 8 || v == 16 ? v * 2 : 0;
  }
  CORE_COLD bool parse_int(const char* s, int* v) {
    bool neg = *s == '-';
    if (neg) ++s;
    const char* d = s;
    int x = 0;
    while (*s >= '0' && *s <= '9' && x < 100000) x = x * 10 + (*s++ - '0');
    if (s == d || *s) return false;
    *v = neg ? -x : x;
    return true;
  }

  CORE_COLD bool command(const char* p, char* o, int cap) {
    if (strncmp(p, "loop", 4) != 0 || (p[4] && p[4] != '=')) return false;
    if (cap <= 0) return true;
    o[0] = 0;
    if (!SIDECAR_LOOPER) {
      sniprintf(o, cap, CORE_STR("loop: not in this release"));
      return true;
    }
    if (!on_) {
      sniprintf(o, cap, "loop: off (loopram=0, or no RAM for it)");
      return true;
    }
    if (!p[4]) {
      char gl[96] = "";
      int n = 0;
      for (int i = 0; i < ng_ && n < (int)sizeof gl - 16; ++i) {
        char l[8];
        n += sniprintf(gl + n, sizeof gl - n, "%s%s%s%s", i ? " " : "", len_name(g_[i].half_bars, l, sizeof l), g_[i].kept ? "k" : "", g_[i].copied ? "" : "~");
      }
      float bar, of;
      char pos[24] = "";
      if (bar_position(&bar, &of)) sniprintf(pos, sizeof pos, " pos %d.%02d/%d", (int)bar, (int)(bar * 100) % 100, (int)of < 1 ? 1 : (int)of);   // (integers: the Pi's sniprintf has no %f)
      char lv[10] = "off";
      if (live_via_) sniprintf(lv, sizeof lv, "perf%d", live_via_);
      sniprintf(o, cap, "loop: %s, ring %d s, %d grabs [%s]%s, bal %d lev %d fade %s mute %s live %s postlat %d out %s", kStateName[state()],
                ring_seconds(), ng_, gl, pos, bal_, lev_, kFadeName[fade_], auto_mute_ ? "on" : "off", lv, post_lat_, out() == kOutPre ? "pre" : "post");
      return true;
    }
    const char* a = p + 5;
    char b[48];
    int v;
    if (!strncmp(a, "grab:", 5)) {
      int hb = parse_len(a + 5);
      if (!hb) sniprintf(o, cap, "loop=grab:L (L 0.5 1 2 4 8 16 bars)");
      else {
        int e = grab(hb);
        if (e) say(err_text(e, hb, b, sizeof b)), sniprintf(o, cap, "loop: %s", b);
        else sniprintf(o, cap, "loop: grab %s bars%s", len_name(hb, b, sizeof b), pend_.on ? " (waiting for the bar line's audio)" : "");
      }
    } else if (!strncmp(a, "play:", 5)) {
      int g;
      if (a[5] == '#') {   // by the grab's id (a view that was drawn before a newer grab landed still means the same one)
        int id = 0;
        g = parse_int(a + 6, &id) ? find_id((uint32_t)id) : -1;
      } else if (!parse_int(a + 5, &g)) {
        g = -1;
      }
      if (g < 0 || !play(g)) say("NO GRAB / PLAY FIRST"), sniprintf(o, cap, "loop: no grab %s, or the transport is stopped", a + 5);
      else sniprintf(o, cap, "loop: play grab %d from the next bar line", g);
    } else if (!strncmp(a, "stop", 4)) {
      bool cut = false, now = false;
      for (const char* w = a + 4; *w == ':';) {
        ++w;
        if (!strncmp(w, "cut", 3)) cut = true, w += 3;
        else if (!strncmp(w, "fade", 4)) cut = false, w += 4;
        else if (!strncmp(w, "now", 3)) now = true, w += 3;
        else break;
      }
      stop(cut, now);
      sniprintf(o, cap, "loop: stop (%s, %s)", cut ? "cut" : "fade", now ? "at once" : "at the next bar line");
    } else if (!strncmp(a, "keep:", 5)) {
      int g;
      if (a[5] == '#') {
        int id = 0;
        g = parse_int(a + 6, &id) ? find_id((uint32_t)id) : -1;
        if (g < 0) g = 99;   // (no such grab: keep() says NO GRAB)
      } else if (!parse_int(a + 5, &g)) g = -1;
      if (g < 0) sniprintf(o, cap, "loop=keep:N (N 0 newest) or #ID");
      else {
        keep(g, b, sizeof b);
        say(b);
        sniprintf(o, cap, "loop: %s", b);
      }
    } else if (!strncmp(a, "bal:", 4) || !strncmp(a, "lev:", 4)) {
      if (!parse_int(a + 4, &v)) sniprintf(o, cap, "loop=%.3s:0..127", a);
      else {
        bool isbal = a[0] == 'b';
        v = clamp127(v);
        mod::manual(mod::tid(mod::kLoop, 0, isbal ? 0 : 1), (float)v);
        if (isbal) set_bal(v);
        else set_lev(v);
        sniprintf(o, cap, "loop: %s %d", isbal ? "bal" : "lev", v);
      }
    } else if (!strncmp(a, "fade:", 5)) {
      int f = -1;
      for (int i = 0; i < kFades; ++i)
        if (!strcmp(a + 5, kFadeName[i])) f = i;
      if (!strcmp(a + 5, "0.25")) f = 0;
      if (!strcmp(a + 5, "0.5")) f = 1;
      if (f < 0) sniprintf(o, cap, "loop=fade:1/4|1/2|1|2|4 (bars)");
      else set_fade(f), sniprintf(o, cap, "loop: fade %s bars", kFadeName[f]);
    } else if (!strncmp(a, "out:", 4)) {
      if (!strcmp(a + 4, "post") || !strcmp(a + 4, "pre")) set_out(a[4 + 2] == 's' ? kOutPost : kOutPre), sniprintf(o, cap, "loop: out %s", out() == kOutPre ? "pre" : "post");
      else sniprintf(o, cap, "loop=out:post|pre");
    } else if (!strncmp(a, "mute:", 5)) {
      if (!strcmp(a + 5, "on") || !strcmp(a + 5, "off")) set_auto_mute(a[5 + 1] == 'n'), sniprintf(o, cap, "loop: auto-mute %s", auto_mute_ ? "on" : "off");
      else sniprintf(o, cap, "loop=mute:on|off");
    } else if (!strncmp(a, "live:", 5)) {
      int m = -1;
      if (!strcmp(a + 5, "off")) m = 0;
      else if (!strncmp(a + 5, "perf", 4) && parse_int(a + 9, &v) && v >= 1 && v <= 12) m = v;
      if (m < 0) sniprintf(o, cap, "loop=live:off|perf1..perf12");
      else set_live_via(m), sniprintf(o, cap, "loop: live via %s", m ? "PERF" : "off");
      if (m > 0) sniprintf(o + strlen(o), cap - (int)strlen(o), " %d", m);
    } else if (!strcmp(a, "cal")) {
      cal_start();
      sniprintf(o, cap, "loop: calibrating POST latency (3 clicks on POST 0 / 1, the Rytm playing or not)");
    } else {
      sniprintf(o, cap, "loop: grab:L play:N stop[:cut|fade][:now] keep:N bal:V lev:V fade:B mute:on|off live:off|perfN out:post|pre cal");
    }
    return true;
  }

  CORE_COLD int json_once(char* o, int cap) {
    if (cap <= 0) return 0;
    int n = sniprintf(o, cap, "\"loop\":{\"on\":%d", on_ ? 1 : 0);
    if (!on_) return n + sniprintf(o + n, cap - n, "}");
    float bar, of;
    bool pos = bar_position(&bar, &of);
    n += sniprintf(o + n, cap - n,
                   ",\"state\":\"%s\",\"ring_s\":%d,\"bal\":%d,\"lev\":%d,\"fade\":%d,\"mute\":%d,\"live\":%d,\"post_lat\":%d,\"out\":\"%s\",\"postgain\":%d,\"pending\":%d,\"cal\":%d",
                   kStateName[state()], ring_seconds(), bal_, lev_, fade_, auto_mute_ ? 1 : 0, live_via_, post_lat_, out() == kOutPre ? "pre" : "post",
                   (int)(postgain() * 1000.0f + 0.5f), pend_.on ? 1 : 0, cal_state_ == 1 ? 1 : 0);
    if (pos) n += sniprintf(o + n, cap - n, ",\"bar\":%d.%03d,\"bars\":%d.%d", (int)bar, (int)(bar * 1000) % 1000, (int)of, (int)(of * 10) % 10);
    n += sniprintf(o + n, cap - n, ",\"msg\":\"%s\",\"msg_n\":%u,\"grabs\":[", msg_, (unsigned)msg_gen_);
    int pi = playing();
    for (int i = 0; i < ng_ && n < cap - 120; ++i)
      n += sniprintf(o + n, cap - n, "%s{\"id\":%u,\"hb\":%d,\"bpm\":%d.%d,\"kept\":%d,\"gap\":%d,\"copied\":%d,\"playing\":%d}", i ? "," : "",
                     (unsigned)g_[i].id, g_[i].half_bars, (int)(g_[i].bpm * 10.0f + 0.5f) / 10, (int)(g_[i].bpm * 10.0f + 0.5f) % 10, g_[i].kept, g_[i].gap, g_[i].copied, i == pi ? 1 : 0);
    n += sniprintf(o + n, cap - n, "]}");
    return n < cap ? n : 0;
  }
  CORE_COLD int json(char* o, int cap) {   // (core 0 reads core 3's list: again if it was shifted meanwhile, a few tries)
    int n = 0;
    for (int t = 0; t < 4; ++t) {
      uint32_t s0 = __atomic_load_n(&list_seq_, __ATOMIC_ACQUIRE);
      if (s0 & 1) continue;
      n = json_once(o, cap);
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (__atomic_load_n(&list_seq_, __ATOMIC_RELAXED) == s0) break;
    }
    return n;
  }

  // ------------------------------------------------------------------------------------------------ core 0
  void in_block(const int32_t* lr, int n, uint64_t arrival_frame, unsigned lost) {
    if (!ring_.ready()) return;
    ring_.write(lr, n, arrival_frame, lost);
    uint64_t c0 = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
    if (!c0) return;
    // the calibration's listening (spec 3.4): the stream's own level before the first click, then each click's window
    uint64_t first = arrival_frame - (uint64_t)(n - 1);
    int32_t fl = cal_floor_;
    if (first < c0) {
      for (int i = 0; i < n; ++i) {
        int32_t a = lr[2 * i] < 0 ? -lr[2 * i] : lr[2 * i];
        if (a > fl) fl = a;
      }
      cal_floor_ = fl;
      return;
    }
    int32_t thr = fl > 0x7FFFFFFF / 8 ? 0x7FFFFFFF / 2 : 4 * fl;
    if (thr < (1 << 21)) thr = 1 << 21;   // -60 dBFS at least
    for (int c = 0; c < kCalN; ++c) {
      uint64_t t = c0 + (uint64_t)c * kCalGap;
      int32_t found = cal_found_[c];   // (this thread is its only writer)
      if (arrival_frame < t || first >= t + kCalWindow + (found >= 0 ? (uint64_t)found + kCalPeakWin : 0)) continue;
      uint32_t pk = cal_peak_[c];
      for (int i = 0; i < n; ++i) {
        uint64_t h = first + (uint64_t)i;
        if (h < t) continue;
        int32_t a = lr[2 * i] < 0 ? -lr[2 * i] : lr[2 * i];
        if (found < 0) {
          if (h >= t + kCalWindow || a <= thr) continue;
          found = (int32_t)(h - t);
          __atomic_store_n(&cal_found_[c], found, __ATOMIC_RELEASE);
        } else if (h >= t + (uint64_t)found + kCalPeakWin) {
          break;
        }
        // the peak back (L or R, as heard) in kCalPeakWin frames from the onset
        uint32_t ua = (uint32_t)a, ub = lr[2 * i + 1] < 0 ? 0u - (uint32_t)lr[2 * i + 1] : (uint32_t)lr[2 * i + 1];
        uint32_t m = ua > ub ? ua : ub;
        pk = m > pk ? m : pk;
      }
      __atomic_store_n(&cal_peak_[c], pk, __ATOMIC_RELEASE);
    }
  }

  struct Voice {
    bool active;
    uint32_t id;
    bool from_ring;
    const int32_t* pcm;
    uint64_t idx0;
    uint32_t len;
    uint64_t start, stop_at;
    uint32_t stop_len;
    bool fade_in;
    uint32_t peak;   // the loop's largest |sample| (0: not known)
    const uint16_t* env;   // the limiter's gain envelope (null: gain 1)
    uint32_t peak_unl;     // the peak without the envelope: the cap blends from it to `peak` as the envelope fades in (ew)
    float ew;        // how much of the envelope is applied (0..1): a loop that moves from the ring to the store fades it in over 10 ms
    float trim[2];   // its level cap now on POST / PRE (<= 1): moves toward the target a block gives it, < 0 not set yet
  };
  Voice cur_, old_;
  float sw_[2] = {-1.0f, -1.0f};   // how much of the loop is on POST / PRE now (OUT crossfades over 10 ms; < 0: not set yet)
  Ramp ramp_own_, ramp_loop_;
  uint32_t seen_seq_ = 0;

  inline void vframe(const Voice& v, int64_t k, float* l, float* r) {
    if (v.from_ring) {
      int32_t a, b;
      ring_.frame(v.idx0 + (uint64_t)k, &a, &b);
      *l = (float)a, *r = (float)b;
    } else {
      float g = v.env ? env_gain(v.env, (uint32_t)(k + kPre)) : 1.0f;   // (the limiter's envelope; k < 0: the pre-roll)
      if (v.ew < 1.0f) g = 1.0f + (g - 1.0f) * v.ew;
      *l = (float)v.pcm[2 * k] * g, *r = (float)v.pcm[2 * k + 1] * g;
    }
  }
  // the voice's frame at host frame h (with its seam and its own stop's fade); false: silent
  inline bool vat(const Voice& v, uint64_t h, float* l, float* r) {
    if (!v.active || h < v.start) return false;
    float g = 1.0f;
    if (v.stop_at && h >= v.stop_at) {
      uint64_t d = h - v.stop_at;
      if (d >= v.stop_len) return false;
      g = 1.0f - (float)(d + 1) / (float)v.stop_len;
    }
    uint64_t p = (h - v.start) % v.len;
    float a, b;
    vframe(v, (int64_t)p, &a, &b);
    if (p + kSeam >= v.len) {   // the seam: the end fades into the frames that lead into the start
      int j = (int)(p + kSeam - v.len);
      float w = (float)(j + 1) / kSeam, c, d;
      vframe(v, -kSeam + j, &c, &d);
      a = a + (c - a) * w, b = b + (d - b) * w;
    }
    *l = a * g, *r = b * g;
    return true;
  }
  void core0_reset() {
    cur_ = Voice(), old_ = Voice(), ramp_own_ = Ramp(), ramp_loop_ = Ramp(), seen_seq_ = 0;
    sw_[0] = sw_[1] = -1.0f;
  }
  inline int32_t add(int32_t a, float x) {
    int64_t s = (int64_t)a + (int64_t)x;
    return s > 2147483647 ? 2147483647 : s < INT32_MIN ? INT32_MIN : (int32_t)s;
  }
  // the loop onto POST or PRE: each loop's level is capped from its peak (out_mix), so one loop stays under kKnee (= kLoopCeil); several summed
  // loops, or the Sidecar's own output under them, may ask for more than full scale.
  // Under kKnee of full scale the sum is exact (as add); above it bends smoothly toward full scale (a tanh knee) instead of a
  // hard clip that a resample would record again
  static constexpr float kKnee = kLoopCeil;   // a loop alone (capped to kLoopCeil) is never shaped; a sum with the base or other loops is
  inline int32_t add_soft(int32_t a, float x) {
    float s = ((float)a + x) * (1.0f / 2147483648.0f), m = s < 0 ? -s : s;
    if (!(m > kKnee)) return add(a, x);
    // what is already on the pair is not the loop's to bend: at or past the knee on its own, only the loop's share is shaped, into the
    // headroom that is left above it (a tiny or silent loop is added as it is; a loop in the same direction never reaches full scale)
    float ma = (float)a * (1.0f / 2147483648.0f);
    if (ma >= kKnee || -ma >= kKnee) {
      float hr = 2147483520.0f - fabsf((float)a);
      if (x * (float)a <= 0.0f) return add(a, x);   // (the loop pulls it back: as it is, also from a base on the rail)
      if (!(hr > 0.0f)) return a;                   // (no room)
      float y = hr * tanhf(fabsf(x) / hr);
      return add(a, x < 0 ? -y : y);
    }
    float o = kKnee + (1.0f - kKnee) * tanhf((m - kKnee) / (1.0f - kKnee));
    return (int32_t)((s < 0 ? -o : o) * 2147483520.0f);
  }

  Voice voice_of(const LpSnap& s, bool fade_in) {
    return Voice{s.active, s.id, s.from_ring, s.pcm, s.idx0, s.len, s.start, s.stop_at, s.stop_len, fade_in, s.peak, s.env, s.peak_unl, 1.0f, {-1.0f, -1.0f}};
  }
  // a loop's presence 0..1 at host frame h: its start's ramp times its stop's fade (BAL's own-output gain follows it)
  float env_of(const Voice& v, uint64_t h) {
    if (!v.active || h < v.start) return 0.0f;
    float e = v.fade_in ? (float)(h - v.start + 1) / kRampFrames : 1.0f;   // (a swap in: the loop it replaces was already down)
    if (e > 1.0f) e = 1.0f;
    if (v.stop_at && h >= v.stop_at) {
      float d = (float)(h - v.stop_at + 1) / (float)v.stop_len;
      e *= d >= 1.0f ? 0.0f : 1.0f - d;
    }
    return e;
  }
  void take_snapshot(uint64_t h0) {
    uint32_t s0 = __atomic_load_n(&pub_seq_, __ATOMIC_ACQUIRE);
    if (s0 == seen_seq_ || (s0 & 1)) return;
    Snap s = pub_;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&pub_seq_, __ATOMIC_RELAXED) != s0) return;   // rewritten meanwhile: the next block
    seen_seq_ = s0;
    ramp_own_ = s.own, ramp_loop_ = s.lp;
    if (!s.cur.active && !s.prev.active) {   // gone (an abandoned copy, a stop before the start): a cut from here
      if (cur_.active && !(cur_.stop_at && cur_.stop_at <= h0)) cur_.stop_at = h0, cur_.stop_len = kCutFrames;
      if (old_.active && !(old_.stop_at && old_.stop_at <= h0)) old_.stop_at = h0, old_.stop_len = kCutFrames;
      return;
    }
    // the snapshot is the whole truth: what plays, and what it replaces while that is heard
    const Voice was_cur = cur_, was_old = old_;
    // a loop keeps its level cap as it moves from cur to old; one that moves from the ring to the store fades its envelope in
    auto trim_of = [&](Voice* nv) {
      const Voice* v = was_cur.active && was_cur.id == nv->id ? &was_cur : was_old.active && was_old.id == nv->id ? &was_old : nullptr;
      nv->trim[0] = v ? v->trim[0] : -1.0f, nv->trim[1] = v ? v->trim[1] : -1.0f;
      if (v) nv->ew = v->from_ring && !nv->from_ring ? 0.0f : v->ew;
    };
    if (s.prev.active) old_ = voice_of(s.prev, false), trim_of(&old_);
    else if (s.cur.id != cur_.id) old_.active = false;
    Voice c = voice_of(s.cur, !s.prev.active);
    if (s.cur.id == cur_.id) c.fade_in = cur_.fade_in;
    trim_of(&c);
    cur_ = c;
  }

  uint32_t out_gain_bits_ = 0x3F800000u;   // 1.0f, as bits (the __atomic builtins take integers)
  void set_out_gain(float g) {
    if (!(g > 0.0f && g < 16.0f)) g = 1.0f;
    uint32_t b;
    memcpy(&b, &g, 4);
    __atomic_store_n(&out_gain_bits_, b, __ATOMIC_RELAXED);
  }
  float out_gain() {
    uint32_t b = __atomic_load_n(&out_gain_bits_, __ATOMIC_RELAXED);
    float g;
    memcpy(&g, &b, 4);
    return g;
  }

  uint32_t fbits(float g) {
    uint32_t b;
    memcpy(&b, &g, 4);
    return b;
  }
  float fof(uint32_t b) {
    float g;
    memcpy(&g, &b, 4);
    return g;
  }
  void set_pre_gain(float g) {
    if (!(g > 0.0f && g < 16.0f)) g = 1.0f;
    __atomic_store_n(&pre_gain_bits_, fbits(g), __ATOMIC_RELAXED);
  }
  float pre_gain() { return fof(__atomic_load_n(&pre_gain_bits_, __ATOMIC_RELAXED)); }
  void set_postgain(float g) {
    uint32_t b = g >= 1.5f && g <= 6.0f ? fbits(g) : 0u;
    if (b != __atomic_load_n(&postgain_bits_, __ATOMIC_RELAXED)) dirty_ = true;
    __atomic_store_n(&postgain_bits_, b, __ATOMIC_RELAXED);
  }
  float postgain() { return fof(__atomic_load_n(&postgain_bits_, __ATOMIC_RELAXED)); }
  void set_out(int o) {
    int v = o == kOutPre && ret_[1][0] != kNone && ret_[1][1] != kNone ? kOutPre : kOutPost;   // (a box without a PRE return: POST)
    if (v != __atomic_load_n(&out_, __ATOMIC_RELAXED)) dirty_ = true;
    __atomic_store_n(&out_, v, __ATOMIC_RELAXED);
  }
  int out() { return __atomic_load_n(&out_, __ATOMIC_RELAXED); }
  float makeup_of(int pair) {
    if (pair) return pre_gain();
    float pg = postgain();
    return pg > 0.0f ? pg : out_gain();
  }
  float makeup() { return makeup_of(out() == kOutPre ? 1 : 0); }

  bool wants_out() { return on_; }

  void out_mix(int32_t (*F)[12], int n, uint64_t render_frame) {
    if (!on_) return;
    take_snapshot(render_frame);
    uint64_t c0 = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
    if (c0) {   // loop=cal: a click (a short half-sine burst) onto POST 0 / 1, each kCalGap
      for (int f = 0; f < n; ++f) {
        uint64_t h = render_frame + (uint64_t)f;
        if (h < c0 || (h - c0) / kCalGap >= (uint64_t)kCalN || (h - c0) % kCalGap >= (uint64_t)kCalClick) continue;
        int32_t x = click_at((h - c0) % kCalGap);
        if (ret_[0][0] != kNone) F[f][ret_[0][0]] = add(F[f][ret_[0][0]], (float)x);
          if (ret_[0][1] != kNone) F[f][ret_[0][1]] = add(F[f][ret_[0][1]], (float)x);
      }
    }
    if (!cur_.active && !old_.active) {
      sw_[0] = -1.0f;   // (idle: the next loop starts on the pair OUT names, no fade)
      return;
    }
    const float mkp[2] = {makeup_of(0), makeup_of(1)};   // the returns' make-ups (POST: postgain or set_out_gain; PRE: set_pre_gain)
    const int want = out() == kOutPre && ret_[1][0] != kNone && ret_[1][1] != kNone ? 1 : 0;   // (a box without a PRE return: POST)
    if (sw_[0] < 0.0f) sw_[0] = want ? 0.0f : 1.0f, sw_[1] = want ? 1.0f : 0.0f;
    // each loop's level cap from its own peak: the loop alone stays under kLoopCeil of full scale, LEV and BAL (ramp_loop_) included
    // (a quiet one keeps all the make-up; LEV under unity just makes it quieter). The gain on a pair is min(make-up x LEV x BAL, cap / peak)
    const float rg = fmaxf(ramp_loop_.at_h(render_frame), ramp_loop_.at_h(render_frame + (uint64_t)n));
    auto cap = [&](const Voice& v, int p) {
      // the peak the loop reaches as played: original x (1 + (envelope - 1) x ew) is at most unl x (1 - ew) + lim x ew while the envelope
      // fades in (ring -> store); no peak yet while it plays from the ring (no piece scanned): as if full scale
      float pk = (float)v.peak;
      if (v.ew < 1.0f && v.peak_unl > v.peak) pk = (float)v.peak_unl + ((float)v.peak - (float)v.peak_unl) * v.ew;
      if (!v.peak && v.from_ring) pk = 2147483648.0f;
      if (!(pk > 0.0f)) return 1.0f;
      float t = kLoopCeil * 2147483648.0f / (pk * mkp[p] * (rg > 1.0f ? rg : 1.0f));
      return t < 1.0f ? t : 1.0f;
    };
    float tc[2] = {cap(cur_, 0), cap(cur_, 1)}, to[2] = {cap(old_, 0), cap(old_, 1)};
    for (int p = 0; p < 2; ++p) {
      if (cur_.trim[p] < 0.0f) cur_.trim[p] = tc[p];
      if (old_.trim[p] < 0.0f) old_.trim[p] = to[p];
      // the make-up is read raw, so a cap that falls (a make-up change, e.g. a cal result) is taken at once: trim x make-up stays as it was;
      // only a cap that rises is eased in over 10 ms (below)
      if (tc[p] < cur_.trim[p]) cur_.trim[p] = tc[p];
      if (to[p] < old_.trim[p]) old_.trim[p] = to[p];
    }
    constexpr float kTrimStep = 1.0f / kRampFrames;   // a cap that changes (a copy still coming in, LEV, the make-up) and OUT move over 10 ms
    for (int f = 0; f < n; ++f) {
      for (int p = 0; p < 2; ++p) {
        cur_.trim[p] += fminf(kTrimStep, fmaxf(-kTrimStep, tc[p] - cur_.trim[p]));
        old_.trim[p] += fminf(kTrimStep, fmaxf(-kTrimStep, to[p] - old_.trim[p]));
        const float t = p == want ? 1.0f : 0.0f;
        sw_[p] += fminf(kTrimStep, fmaxf(-kTrimStep, t - sw_[p]));
      }
      cur_.ew = fminf(1.0f, cur_.ew + kTrimStep), old_.ew = fminf(1.0f, old_.ew + kTrimStep);
      uint64_t h = render_frame + (uint64_t)f;
      float l[2] = {0, 0}, r[2] = {0, 0}, a, b;   // the loop's frame as it goes onto POST / PRE (each pair's own cap)
      bool any = false;
      if (vat(cur_, h, &a, &b)) {
        float w = h - cur_.start < (uint64_t)kSeam ? (float)(h - cur_.start + 1) / kSeam : 1.0f;   // a start fades in; a swap crossfades
        for (int p = 0; p < 2; ++p) l[p] += a * w * cur_.trim[p], r[p] += b * w * cur_.trim[p];
        any = true;
      }
      if (old_.active) {
        float c, d;
        bool xf = cur_.active && h >= cur_.start;   // past the new loop's start: the 64-frame crossfade, then it is gone
        if (xf && h >= cur_.start + kSeam) old_.active = false;
        else if (vat(old_, h, &c, &d)) {
          float w = xf ? 1.0f - (float)(h - cur_.start + 1) / kSeam : 1.0f;
          for (int p = 0; p < 2; ++p) l[p] += c * w * old_.trim[p], r[p] += d * w * old_.trim[p];
          any = true;
        }
      }
      if (!any) continue;
      // BAL: the Sidecar's own output (every channel of F so far) falls toward `own` as the loop is heard (spec 3.8)
      float og = ramp_own_.at_h(h);
      if (og < 0.99999f) {
        float ow = cur_.active && h >= cur_.start ? 1.0f - (float)(h - cur_.start + 1) / kSeam : 1.0f;   // the old loop's crossfade weight
        float e = fmaxf(env_of(cur_, h), env_of(old_, h) * (ow < 0 ? 0 : ow));   // whichever loop is heard: a resample's wait keeps the old one's
        float m = 1.0f + (og - 1.0f) * e;
        for (int c = 0; c < 12; ++c) F[f][c] = (int32_t)((float)F[f][c] * m);
      }
      const float lv = ramp_loop_.at_h(h);
      for (int p = 0; p < 2; ++p) {
        if (!(sw_[p] > 0.0f) || ret_[p][0] == kNone || ret_[p][1] == kNone) continue;
        const float lg = lv * mkp[p] * sw_[p];
        const uint8_t cl = ret_[p][0], cr = ret_[p][1];
          F[f][cl] = add_soft(F[f][cl], l[p] * lg), F[f][cr] = add_soft(F[f][cr], r[p] * lg);
      }
    }
    // a stopped voice that has faded out is gone (its stop's end passed)
    if (cur_.active && cur_.stop_at && render_frame + (uint64_t)n >= cur_.stop_at + cur_.stop_len) cur_.active = false;
  }
};

Looper box_[kSlots];
int (*slot_fn_)() = nullptr;
CORE_COLD Looper& hand() {
  int s = slot_fn_ ? slot_fn_() : 0;
  return box_[s >= 0 && s < kSlots ? s : 0];
}
inline Looper& at(int slot) { return box_[slot >= 0 && slot < kSlots ? slot : 0]; }
inline bool valid(int slot) { return slot >= 0 && slot < kSlots; }

}  // namespace

// ------------------------------------------------------------------------------------------------ the public side: which box
CORE_COLD void set_slot_fn(int (*fn)()) { slot_fn_ = fn; }
CORE_COLD int slot_in_hand() { return &hand() - box_; }
CORE_COLD void set_returns(int slot, const uint8_t post[2], const uint8_t pre[2]) {
  if (!valid(slot)) return;
  Looper& b = at(slot);
  for (int c = 0; c < 2; ++c) {
    b.ret_[0][c] = post && post[c] < 12 ? post[c] : kNoReturn;
    b.ret_[1][c] = pre && pre[c] < 12 ? pre[c] : kNoReturn;
  }
}
CORE_COLD bool has_pre(int slot) { return valid(slot) && at(slot).ret_[1][0] != kNoReturn && at(slot).ret_[1][1] != kNoReturn; }

CORE_COLD void set_env(const Env* e) { hand().set_env(e); }
CORE_COLD void init(int slot, int32_t* ring, uint32_t ring_frames, uint8_t* store, uint64_t store_bytes) {
  if (!valid(slot)) return;
  at(slot).slot_ = slot;
  device::Scope in(device::g_links[slot < device::kLinks ? slot : 0]);   // (its clock's users: the slot's)
  at(slot).init(ring, ring_frames, store, store_bytes);
}
CORE_COLD void init(int32_t* ring, uint32_t ring_frames, uint8_t* store, uint64_t store_bytes) { init(slot_in_hand(), ring, ring_frames, store, store_bytes); }
CORE_COLD void reset() {
  for (int s = 0; s < kSlots; ++s) box_[s].slot_ = s, box_[s].reset();
}
CORE_COLD bool on() { return hand().on(); }
CORE_COLD bool on(int slot) { return valid(slot) && at(slot).on(); }

CORE_COLD int grab(int half_bars) { return hand().grab(half_bars); }
CORE_COLD bool play(int g) { return hand().play(g); }
CORE_COLD void stop(bool cut, bool now) { hand().stop(cut, now); }
CORE_COLD bool keep(int g, char* msg, int cap) { return hand().keep(g, msg, cap); }
CORE_COLD void tick(uint32_t us) {   // each slot in its own Scope: its clock's users, its box's mutes and PERF
  for (int s = 0; s < kSlots && s < device::kLinks; ++s) {
    if (!box_[s].on()) continue;
    device::Scope in(device::g_links[s]);
    box_[s].tick(us);
  }
}
CORE_COLD const char* err_text(int err, int half_bars, char* b, int cap) { return hand().err_text(err, half_bars, b, cap); }

CORE_COLD int grabs() { return hand().grabs(); }
CORE_COLD const Grab* grab_at(int i) { return hand().grab_at(i); }
CORE_COLD int grab_index(uint32_t id) { return hand().grab_index(id); }
CORE_COLD State state() { return hand().state(); }
CORE_COLD int playing() { return hand().playing(); }
CORE_COLD bool playing_info(uint64_t* start, uint32_t* len) { return hand().playing_info(start, len); }
CORE_COLD bool pending() { return hand().pending(); }
CORE_COLD int ring_seconds() { return hand().ring_seconds(); }
CORE_COLD bool bar_position(float* bar, float* of_bars) { return hand().bar_position(bar, of_bars); }

CORE_COLD void set_bal(int v) { hand().set_bal(v); }
CORE_COLD void set_lev(int v) { hand().set_lev(v); }
CORE_COLD void set_fade(int f) { hand().set_fade(f); }
CORE_COLD void set_auto_mute(bool on) { hand().set_auto_mute(on); }
CORE_COLD void set_live_via(int perf) { hand().set_live_via(perf); }
CORE_COLD void set_out(int o) { hand().set_out(o); }
CORE_COLD int out() { return hand().out(); }
CORE_COLD int bal() { return hand().bal(); }
CORE_COLD int lev() { return hand().lev(); }
CORE_COLD int fade() { return hand().fade(); }
CORE_COLD bool auto_mute() { return hand().auto_mute(); }
CORE_COLD int live_via() { return hand().live_via(); }
CORE_COLD void set_bal_mod(int v) { hand().set_bal_mod(v); }
CORE_COLD void set_lev_mod(int v) { hand().set_lev_mod(v); }
CORE_COLD int bal_eff() { return hand().bal_eff(); }
CORE_COLD int lev_eff() { return hand().lev_eff(); }
CORE_COLD float own_gain(int b) { return Looper::own_gain(b); }
CORE_COLD float loop_gain(int b) { return Looper::loop_gain(b); }
CORE_COLD int live_perf(int b) { return Looper::live_perf(b); }

void in_block(int slot, const int32_t* lr, int n, uint64_t arrival, unsigned lost) {
  if (valid(slot)) at(slot).in_block(lr, n, arrival, lost);
}
void out_mix(int slot, int32_t (*F)[12], int n, uint64_t render_frame) {
  if (valid(slot)) at(slot).out_mix(F, n, render_frame);
}
void in_block(const int32_t* lr, int n, uint64_t arrival, unsigned lost) { hand().in_block(lr, n, arrival, lost); }
void out_mix(int32_t (*F)[12], int n, uint64_t render_frame) { hand().out_mix(F, n, render_frame); }
void set_out_gain(int slot, float g) { if (valid(slot)) at(slot).set_out_gain(g); }
void set_pre_gain(int slot, float g) { if (valid(slot)) at(slot).set_pre_gain(g); }
void set_out_gain(float g) { hand().set_out_gain(g); }
float out_gain() { return hand().out_gain(); }
void set_pre_gain(float g) { hand().set_pre_gain(g); }
float pre_gain() { return hand().pre_gain(); }
void set_postgain(float g) { hand().set_postgain(g); }
float postgain() { return hand().postgain(); }
float makeup() { return hand().makeup(); }
bool wants_in() { return hand().wants_in(); }
bool wants_out() { return hand().wants_out(); }
bool wants_in(int slot) { return valid(slot) && at(slot).wants_in(); }
bool wants_out(int slot) { return valid(slot) && at(slot).wants_out(); }

CORE_COLD void cal_start() { hand().cal_start(); }
CORE_COLD bool cal_wanted() { return hand().cal_wanted(); }
CORE_COLD bool cal_info(uint64_t* first_click, int* n, uint32_t* gap) { return hand().cal_info(first_click, n, gap); }
CORE_COLD int post_lat() { return hand().post_lat(); }
CORE_COLD void set_post_lat(int f) { hand().set_post_lat(f); }

CORE_COLD void load() { hand().load(); }
CORE_COLD void take_text(const char* t, int n) { hand().take_text(t, n); }
CORE_COLD bool save() { return hand().save(); }
CORE_COLD bool dirty() { return hand().dirty(); }
CORE_COLD bool command(const char* p, char* out, int cap) { return hand().command(p, out, cap); }
CORE_COLD int json(char* out, int cap) { return hand().json(out, cap); }
CORE_COLD int json(int slot, char* out, int cap) { return valid(slot) ? at(slot).json(out, cap) : 0; }
CORE_COLD const char* last_message() { return hand().last_message(); }
CORE_COLD uint32_t message_gen() { return hand().message_gen(); }

bool keep_take(int slot, KeepJob* j) { return valid(slot) && at(slot).keep_take(j); }
void keep_finished(int slot, uint32_t id, bool ok, const char* take_name) { if (valid(slot)) at(slot).keep_finished(id, ok, take_name); }
bool keep_take(KeepJob* j) { return hand().keep_take(j); }
void keep_finished(uint32_t id, bool ok, const char* take_name) { hand().keep_finished(id, ok, take_name); }
const Stats& stats() { return hand().stats(); }
const Stats& stats(int slot) { return at(slot).stats(); }

}  // namespace loop
}  // namespace core
