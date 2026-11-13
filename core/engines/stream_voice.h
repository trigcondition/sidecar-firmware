// A sample voice streamed from the SD card (docs/sampler-plan.md), the Teensy's
// counterpart of sim/sampler/sample_voice.h (same controls, same sound for
// forward playback).
//
// prepare() - called when the sound's FLD/SMP/STA change, between notes - opens
// the file and reads its first kHead bytes from STA into RAM. trigger() starts
// from that head at once; refill() keeps a kRing ring of what follows topped
// up from the card, 8 KB at a time (about 0.75 ms a read on the exFAT card,
// 2026-10-09). The sketch calls it while it waits for the next request, the
// stream with the least ahead() first, and between render blocks only for a
// stream that would run dry within the request. render() never touches the
// card.
//
// With a card core (core/msamp/cardq.h: the Pi's core 0; 7z, 2026-10-31) the head's read, every refill and the file's
// close are requests of the stream's own, one at a time: step() takes a finished one in, refill() posts the next, the
// control core waits for none of them. What a request brought counts only if it is still what follows the ring (a loop
// back or a new trig moved it on: dropped, the bytes were never live). The ring is twice the size for it: core 0's
// latency comes out of the read-ahead, which stays above what a read on the control core had (~170 ms of 16-bit stereo
// at unity, 85 ms before). When core 0 is held up (a blocking connect, a long upload): a request that has not started,
// with under kRescueFrames of audio left, is taken back and read here, the neediest stream's only, one a pass (voices.cpp) - the
// one card read left on the control core, the read every refill did before, only when the stream would go silent otherwise. MULTISAMPLE's take_back at kTakeBackUs
// (200 ms) is no use here: this ring is empty well before that.
//
// PCM 16/24/32-bit, 1..2 channels (more: the first two), any rate; render()
// plays mono (a stereo file summed), or with outr its two channels (a voice on
// OUT PRE / POST, docs/STEREO.md; a mono file on both sides).
//
// Reverse (prepare's rev: SAMPLER's REV / RLP, or END before STRT): the head is the kHead bytes that end just past
// the start frame (END's), the ring holds what precedes the ring's low end and is refilled downwards, a chunk at a
// time below it (the same ring, the same requests); playback steps down, sample by sample, from the start frame to
// the region's low end (STA), a loop back to the head; nothing read below that low end. DSP SAMPLE prepares no rev:
// its END before STA plays forward from the head, as before 0.5.0.
#pragma once
#include "../platform.h"
#include <math.h>
#include <string.h>

#include "../pool/card_index.h"
#if CORE_MSAMP
#include "../msamp/cardq.h"
#endif

namespace rytm {

class StreamVoice {
  static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

 public:
  static const uint32_t kHead = 8192;    // bytes from STA, in RAM
  static const uint32_t kChunk = 8192;   // a read: fewer, bigger reads are far
                                         // cheaper on the card than many small
  // A reverse read (SAMPLER's REV / RLP). Each is a backwards seek, which on FAT32 (FF_USE_FASTSEEK 0) walks the
  // cluster chain from the file's start: bigger reads mean fewer seeks (review M3, to be measured on the Pi with a FAT32
  // card). At most kChunk: the card queue's read runs on past the ring's end into kRingBytes' spare kChunk.
  static const uint32_t kRevChunk = kChunk;
#if CORE_MSAMP
  static const uint32_t kRing = 32768;   // what follows, refilled from the card
  static const uint32_t kRingBytes = kRing + kChunk;   // a read runs on past the end, copied round when it is in
  enum Prep : uint8_t { kPrepWait, kPrepOk, kPrepFail };
  static const uint32_t kRescueFrames = 480;           // 10 ms of output left: a request not started is taken back
  static const uint32_t kRescueWaitUs = 3000;          // ... that has waited this long (core 0's usual pass is less)
  static const uint32_t kHeadWaitUs = 10000;           // a head (or close) a trig waits for
#else
  static const uint32_t kRing = 16384;   // what follows, refilled from the card
  static const uint32_t kRingBytes = kRing;
#endif

  void attach(uint8_t* head, uint8_t* ring) {
    head_ = head;
    ring_ = ring;
  }

  // The file (from the card index) and STA (0..1 of the data): open, read the
  // head. Returns false if the file cannot be read.
  bool prepare(card::Index& cards, int folder, int smp, float start, bool rev = false) {
    card::FileRec r;
    core::File f;
    if (!cards.file(folder, smp, &r) || !cards.open(folder, r, &f)) return false;
    return prepare(r, f, folder, smp, start, rev);
  }

  // The same with the file's record and an open copy of it at hand (the
  // sketch keeps one per pool slot): no index lookup or open on the card,
  // only the head's read (8 KB, ~0.65 ms). rev: played backwards from start (see the top).
  bool prepare(const card::FileRec& r, const core::File& file, int folder, int smp, float start, bool rev = false) {
    uint32_t bpf = frame_bytes(r);
    uint32_t frames = bpf ? r.data_len / bpf : 0;
    uint32_t h0, want, s0;
    head_of(r, bpf, frames, start, rev, &h0, &want, &s0);
    if (ready_ && folder == folder_ && smp == smp_ && h0 == h0_ && rev == rev_ && s0 == s0_) return true;
    playing_ = false;
    ready_ = false;
    if (f_.isOpen()) f_.close();
    if (!file.isOpen()) return false;
    f_ = file;
    rec_ = r;
    bpf_ = bpf;
    fmt_ = r.bits == 16 && r.channels == 1 ? 1 : r.bits == 16 && r.channels == 2 ? 2
           : r.bits == 24 && r.channels == 1 ? 3 : r.bits == 24 && r.channels == 2 ? 4 : 0;
    frames_ = frames;
    folder_ = folder;
    smp_ = smp;
    h0_ = h0;
    rev_ = rev, s0_ = s0;
    if (!f_.seekSet(r.data_offset + h0) || (uint32_t)f_.read(head_, want) != want) return false;
    hlen_ = want;
    ready_ = true;
    return true;
  }

#if CORE_MSAMP
  // prepare() with a card core, called again until it says: the request under way ends, the file before closes, the
  // head is read, each by the card core. urgent: a trig waits for it.
  Prep prepare_q(const card::FileRec& r, const core::File& file, int folder, int smp, float start, bool urgent,
                 bool rev = false) {
    uint32_t bpf = frame_bytes(r);
    uint32_t frames = bpf ? r.data_len / bpf : 0;
    uint32_t h0, want, s0;
    head_of(r, bpf, frames, start, rev, &h0, &want, &s0);
    bool same = folder == folder_ && smp == smp_ && h0 == h0_ && rev == rev_ && s0 == s0_;
    urgent_ = urgent;
    if (!rq_kind_ && same && head_res_ != kPrepWait) {   // its head's read ended
      Prep res = head_res_;
      head_res_ = kPrepWait;
      return res;
    }
    bool head = rq_kind_ == kRqHead || (rq_kind_ == kRqPost && rq_want_ == kRqHead);   // (kRqPost: the queue refused it so far)
    if (same && (ready_ || head)) return ready_ ? kPrepOk : kPrepWait;
    // another sample: what is under way no longer counts (its key gone: asked for again it starts over)
    playing_ = false;
    ready_ = false;
    head_res_ = kPrepWait;
    folder_ = -1;
    ++seq_;
    if (f_.isOpen()) pclose_ = true;
    step(core::now_us(), false);
    if (rq_kind_ || pclose_) return kPrepWait;
    if (!file.isOpen()) return kPrepFail;
    f_ = file;
    rec_ = r;
    bpf_ = bpf;
    fmt_ = r.bits == 16 && r.channels == 1 ? 1 : r.bits == 16 && r.channels == 2 ? 2
           : r.bits == 24 && r.channels == 1 ? 3 : r.bits == 24 && r.channels == 2 ? 4 : 0;
    frames_ = frames;
    folder_ = folder;
    smp_ = smp;
    h0_ = h0;
    rev_ = rev, s0_ = s0;
    ++seq_;
    post(kRqHead, core::cardq::kReadReq, r.data_offset + h0, head_, want);   // (the queue full: step posts it)
    hwant_ = want;
    return kPrepWait;
  }

  // A request waiting too long, still of use: a read of where the stream is now, a head of the sample wanted.
  bool overdue(uint32_t now) const {
    namespace q = core::cardq;
    uint8_t k = rq_kind_ == kRqPost ? rq_want_ : rq_kind_;
    if (k == kRqNone || (rq_kind_ != kRqPost && q::idle(&rq_))) return false;
    if (k == kRqRing && (rq_seq_ != seq_ || !ring_next(rq_off_, rq_n_) || !playing_ || ahead(kRescueFrames) >= kRescueFrames)) return false;
    if (k == kRqHead && rq_seq_ != seq_) return false;
    if (rq_kind_ == kRqPost) return k == kRqRing || urgent_;
    uint32_t w = now - rq_.posted_us;
    return w > (k == kRqRing ? kRescueWaitUs : urgent_ ? kHeadWaitUs : q::kTakeBackUs);
  }
  // Each pass: a finished request taken in, a close owed posted; rescue: an overdue one run here (voices.cpp: one a pass).
  // Invariant: step / refill_q never run while a render core renders this voice (the Pi: core 3 between RenderBlock's
  // blocks; the Teensy and the host: one loop). render / render_rev move rs_ / fill_ at a wrap; a completion checked
  // with ring_next and applied after such a wrap would leave the ring a gap and a reverse voice silent to its next trig.
  void step(uint32_t now, bool rescue = true) {
    namespace q = core::cardq;
    bool late = rescue && overdue(now);
    bool stale = rq_kind_ == kRqPost ? rq_want_ != kRqClose && rq_seq_ != seq_ : false;
    if (rq_kind_ == kRqRing || (rq_kind_ == kRqPost && rq_want_ == kRqRing)) stale = rq_seq_ != seq_ || !ring_next(rq_off_, rq_n_);
    else if (rq_kind_ == kRqHead) stale = rq_seq_ != seq_;
    if (stale) {   // of no use any more: not started, it never runs (core 0 held up, it would block this stream's next one)
      if (rq_kind_ == kRqPost) rq_kind_ = 0;
      else q::cancel(&rq_);
    }
    if (rq_kind_ == kRqPost) {   // the queue was full (held up for long: taken-back requests fill it too)
      if (q::post(&rq_)) {
        rq_kind_ = rq_want_;
        return;
      }
      if (!late) return;
      q::run_here(&rq_);
      rq_kind_ = rq_want_;
    } else if (rq_kind_ && !q::idle(&rq_)) {
      if (!late || !q::take_back(&rq_)) return;
    }
    if (rq_kind_ == kRqRing) {
      if (rq_.got == (int32_t)rq_n_ && rq_seq_ == seq_ && ring_next(rq_off_, rq_n_)) {   // still what follows the ring
        uint32_t w = rq_off_ % kRing;                                                    // (reverse: what precedes it)
        if (w + rq_n_ > kRing) memcpy(ring_, ring_ + kRing, w + rq_n_ - kRing);
        if (rev_) rs_ = rq_off_;
        fill_ += rq_n_;
      }
    } else if (rq_kind_ == kRqHead) {
      if (rq_seq_ == seq_) {   // (else the sample moved on: nobody waits for this head)
        bool ok = rq_.got == (int32_t)hwant_;
        if (ok) hlen_ = hwant_, ready_ = true;
        head_res_ = ok ? kPrepOk : kPrepFail;
      }
    }
    rq_kind_ = 0;
    rq_.state = q::kFree;
    if (pclose_) {
      pclose_ = false;
      if (f_.isOpen()) post(kRqClose, q::kCloseReq, 0, nullptr, 0);
    }
  }
#endif

  // No sample (SMP off, an empty pool slot): silent until the next prepare.
  void clear() {
    playing_ = false;
    ready_ = false;
#if CORE_MSAMP
    if (core::cardq::worker()) {
      pclose_ = true;   // (by step: the request under way first)
      head_res_ = kPrepWait;
      folder_ = -1;
      ++seq_;
      return;
    }
#endif
    if (f_.isOpen()) f_.close();
  }

  // The note stops, the sample stays prepared (the voice plays from RAM now).
  void stop() { playing_ = false; }

  void trigger() {
    if (!ready_) return;
    ip_ = s0_;
    fr_ = 0.0f;
    rs_ = rev_ ? h0_ : h0_ + hlen_;   // the ring starts where the head ends (reverse: below it)
    fill_ = 0;
    under_ = false;
    lo_ = 0;   // (the region's: from the first render on)
    playing_ = true;
    first_ = true;
  }

  // Between render blocks: top the ring up by at most one chunk. Returns bytes
  // read.
  uint32_t refill() {
#if CORE_MSAMP
    if (core::cardq::worker()) return refill_q();
#endif
    if (!playing_ || !f_.isOpen()) return 0;
    if (rev_) {
      uint32_t off, n;
      if (!next_rev(&off, &n)) return 0;
      if (!f_.seekSet(rec_.data_offset + off)) return 0;
      uint32_t w = off % kRing, a = umin(n, kRing - w);
      if ((uint32_t)f_.read(ring_ + w, a) != a) return 0;
      if (n > a && (uint32_t)f_.read(ring_, n - a) != n - a) return 0;
      rs_ = off, fill_ += n;
      return n;
    }
    uint32_t got = 0;
    // drop what playback has passed (keep a frame behind for interpolation)
    uint32_t at = ip_ * bpf_;
    if (at > rs_ + bpf_ && at - rs_ - bpf_ <= fill_) {
      uint32_t drop = at - rs_ - bpf_;
      rs_ += drop;
      fill_ -= drop;
    }
    while (fill_ + kChunk <= kRing && rs_ + fill_ < rec_.data_len) {
      uint32_t off = rs_ + fill_, n = umin(kChunk, rec_.data_len - off);
      if (n == kChunk) n -= (rec_.data_offset + off + n) % 512;   // end on a sector: the next read is whole sectors
      if (!f_.seekSet(rec_.data_offset + off)) break;
      uint32_t w = (rs_ + fill_) % kRing;
      uint32_t a = umin(n, kRing - w);
      if ((uint32_t)f_.read(ring_ + w, a) != a) break;
      if (n > a && (uint32_t)f_.read(ring_, n - a) != n - a) break;
      fill_ += n;
      got += n;
      break;                           // one chunk a call: the voices take turns
    }
    return got;
  }

  // semitones: TUN+FIN+note (0 = the file's pitch); start/end 0..1; loop
  // between them; writes n floats, -1..1. Runs of frames that lie in one
  // piece of RAM (the head, or the ring up to its end) take a loop per sample
  // format; the rest (a frame across the ring's end) goes sample by sample.
  void render(float semitones, float start, float end, bool loop, float level, float* out, int n,
              float* outr = nullptr) {
    if (!playing_) {
      for (int i = 0; i < n; ++i) out[i] = 0.0f;
      if (outr)
        for (int i = 0; i < n; ++i) outr[i] = 0.0f;
      return;
    }
    if (outr && rec_.channels < 2) outr_copy_ = outr, outr = nullptr;   // a mono file: R = L (below)
    else outr_copy_ = nullptr;
    float a = clamp01(start) * (frames_ - 1), b = clamp01(end) * (frames_ - 1);
    float lo = a < b ? a : b, hi = a < b ? b : a;
    if (first_) {
      ip_ = s0_;
      fr_ = 0.0f;
      first_ = false;
    }
    float step = rec_.rate / 48000.0f * powf(2.0f, semitones / 12.0f);
    step_ = step;
    if (rev_) {
      render_rev(lo, hi, loop, step, level, out, n, outr);
      return;
    }
    uint32_t ihi = (uint32_t)hi;
    for (int i = 0; i < n;) {
      if (ip_ > ihi || (ip_ == ihi && fr_ > hi - ihi)) {
        if (!loop || hi - lo < 1.0f) {
          playing_ = false;
          for (; i < n; ++i) {
            out[i] = 0.0f;
            if (outr) outr[i] = 0.0f;
          }
          if (outr_copy_)
            for (int j = 0; j < n; ++j) outr_copy_[j] = out[j];
          return;
        }
        ip_ = s0_;   // back to the head (STA); the ring restarts
        fr_ = 0.0f;
        rs_ = h0_ + hlen_;
        fill_ = 0;   // (never while step runs: see step's invariant)
      }
      // the run of frames around ip_ that are in one piece of RAM, ending
      // before hi + 1 (the last frame interpolated towards)
      const uint8_t* base;
      uint32_t f0, f1;
      if (span(ip_, &base, &f0, &f1)) {
        if (f1 > ihi + 2) f1 = ihi + 2;
        int done;
        if (outr) {
          switch (fmt_) {
            case 2: done = run2<2>(base, f0, f1, ihi, hi, step, level, out + i, outr + i, n - i); break;
            case 4: done = run2<4>(base, f0, f1, ihi, hi, step, level, out + i, outr + i, n - i); break;
            default: done = 0;
          }
        } else {
          switch (fmt_) {
            case 1: done = run<1>(base, f0, f1, ihi, hi, step, level, out + i, n - i); break;
            case 2: done = run<2>(base, f0, f1, ihi, hi, step, level, out + i, n - i); break;
            case 3: done = run<3>(base, f0, f1, ihi, hi, step, level, out + i, n - i); break;
            case 4: done = run<4>(base, f0, f1, ihi, hi, step, level, out + i, n - i); break;
            default: done = 0;
          }
        }
        if (done > 0) {
          i += done;
          continue;
        }
      }
      float s0, s1;
      if (outr) {
        float r0, r1;
        if (!sample(ip_, &s0, &r0) || !sample(ip_ + 1, &s1, &r1)) {
          ++underruns_;
          outr[i] = 0.0f;
          out[i++] = 0.0f;
          continue;
        }
        outr[i] = (r0 + (r1 - r0) * fr_) * level;
        out[i++] = (s0 + (s1 - s0) * fr_) * level;
        advance(step);
        continue;
      }
      if (!sample(ip_, &s0) || !sample(ip_ + 1, &s1)) {
        ++underruns_;
        out[i++] = 0.0f;   // the card is behind: silence rather than garbage
        continue;
      }
      out[i++] = (s0 + (s1 - s0) * fr_) * level;
      advance(step);
    }
    if (outr_copy_)
      for (int j = 0; j < n; ++j) outr_copy_[j] = out[j];
  }

  // Output frames buffered ahead of playback (at the last render's pitch), at
  // most `cap`; cap when the file's end is in RAM. The main loop tops up the
  // stream with the least first.
  uint32_t ahead(uint32_t cap) const {
    if (!playing_) return cap;
    if (rev_) return ahead_rev(cap);
    uint32_t end = rs_ + fill_;   // bytes in RAM from the head on, contiguously
    if (rs_ > h0_ + hlen_ && ip_ * bpf_ < rs_) end = h0_ + hlen_;   // looped back into the head
    if (end < h0_ + hlen_) end = h0_ + hlen_;
    if (end >= rec_.data_len) return cap;
    uint32_t have = end / bpf_;
    if (have <= ip_ + 1) return 0;
    float o = (have - ip_ - 1) / (step_ > 0.01f ? step_ : 0.01f);
    return o >= cap ? cap : (uint32_t)o;
  }

  // The ring's output ms when full, at the last render's pitch
  uint32_t ring_ms() const { return (uint32_t)((float)(kRing / bpf_) / (step_ > 0.01f ? step_ : 0.01f)) / 48; }
  bool playing() const { return playing_; }
  // where it reads, 0..1 of the file; -1 not playing (render's core, between its blocks)
  float progress() const { return playing_ && frames_ && !first_ ? (float)ip_ / (float)frames_ : -1.0f; }
  uint32_t underruns() const { return underruns_; }
  const char* name() const { return rec_.name; }

 private:
  static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
  static uint32_t frame_bytes(const card::FileRec& r) {
    return (uint32_t)r.channels * (r.bits / 8);
  }

#if CORE_MSAMP
  enum : uint8_t { kRqNone, kRqRing, kRqHead, kRqClose, kRqPost };
  void post(uint8_t kind, core::cardq::Kind k, uint64_t off, uint8_t* buf, uint32_t len) {
    rq_.kind = k, rq_.f = &f_, rq_.off = off, rq_.buf = buf, rq_.len = len, rq_.got = -1, rq_.path[0] = 0;
    rq_want_ = kind, rq_seq_ = seq_;
    rq_kind_ = core::cardq::post(&rq_) ? kind : (uint8_t)kRqPost;
  }
  // The card core's refill: the same read as refill()'s (one contiguous one past the ring's end), posted. Returns the
  // bytes asked for, 0 when there is nothing to post.
  uint32_t refill_q() {
    step(core::now_us(), false);
    if (rq_kind_ || pclose_ || !playing_ || !f_.isOpen()) return 0;
    if (rev_) {
      uint32_t off, n;
      if (!next_rev(&off, &n)) return 0;
      rq_off_ = off, rq_n_ = n;
      post(kRqRing, core::cardq::kReadReq, rec_.data_offset + off, ring_ + off % kRing, n);
      return n;
    }
    uint32_t at = ip_ * bpf_;
    if (at > rs_ + bpf_ && at - rs_ - bpf_ <= fill_) {
      uint32_t drop = at - rs_ - bpf_;
      rs_ += drop;
      fill_ -= drop;
    }
    if (fill_ + kChunk > kRing || rs_ + fill_ >= rec_.data_len) return 0;
    uint32_t off = rs_ + fill_, n = umin(kChunk, rec_.data_len - off);
    if (n == kChunk) n -= (rec_.data_offset + off + n) % 512;   // end on a sector: the next read is whole sectors
    rq_off_ = off, rq_n_ = n;
    post(kRqRing, core::cardq::kReadReq, rec_.data_offset + off, ring_ + off % kRing, n);
    return n;
  }
#endif

  // The head for a prepare from start (0..1): forward the kHead bytes from its frame; reverse the kHead bytes that end
  // two frames past it (its frame and the one it interpolates towards). s0: the frame playback starts on.
  static void head_of(const card::FileRec& r, uint32_t bpf, uint32_t frames, float start, bool rev, uint32_t* h0,
                      uint32_t* want, uint32_t* s0) {
    uint32_t f = (uint32_t)(clamp01(start) * (frames ? frames - 1 : 0)), at = f * bpf;
    *s0 = f;
    if (!rev) {
      uint32_t w = umin(kHead, r.data_len - umin(at, r.data_len));
      *h0 = at, *want = w - (bpf ? w % bpf : 0);
      return;
    }
    uint32_t top = umin(frames * bpf, at + 2 * bpf), w = umin(kHead, top);
    w -= bpf ? w % bpf : 0;
    *h0 = top - w, *want = w;
  }
  // A ring read of n bytes at off is the one the ring wants next: what follows it (reverse: what precedes it)
  bool ring_next(uint32_t off, uint32_t n) const { return rev_ ? off + n == rs_ : off == rs_ + fill_; }
  // Reverse: what playback has passed (above its frame + 1) dropped; the next read below the ring (off, n), false
  // when none (the ring full, the file's start reached). A read's start on a sector, as forward's end.
  bool next_rev(uint32_t* off, uint32_t* n) {
    uint32_t keep = (ip_ + 2) * bpf_;
    if (fill_ && keep >= rs_ && keep < rs_ + fill_) fill_ = keep - rs_;
    if (fill_ + kRevChunk > kRing || rs_ <= lo_ * bpf_) return false;   // (nothing below the region's low end: review L3)
    uint32_t k = umin(kRevChunk, rs_), o = rs_ - k, mis = (uint32_t)((rec_.data_offset + o) % 512);
    if (k == kRevChunk && mis) k -= 512 - mis, o = rs_ - k;
    *off = o, *n = k;
    return true;
  }
  // Reverse: frames buffered below the play position (contiguously, the head and the ring under it), at most cap
  uint32_t ahead_rev(uint32_t cap) const {
    uint32_t at = ip_ * bpf_, low;
    if (at >= h0_ && at < h0_ + hlen_) low = fill_ && rs_ + fill_ >= h0_ ? rs_ : h0_;
    else if (at >= rs_ && at < rs_ + fill_) low = rs_;
    else return 0;
    if (low <= lo_ * bpf_) return cap;   // the region's low end (the file's start) in RAM
    uint32_t lf = (low + bpf_ - 1) / bpf_;
    if (ip_ <= lf) return 0;
    float o = (ip_ - lf) / (step_ > 0.01f ? step_ : 0.01f);
    return o >= cap ? cap : (uint32_t)o;
  }
  // Reverse playback: from the start frame down to lo, sample by sample; a loop back to the head
  void render_rev(float lo, float hi, bool loop, float step, float level, float* out, int n, float* outr) {
    lo_ = (uint32_t)lo;   // the lowest frame playback reads: the refills stop there
    for (int i = 0; i < n; ++i) {
      if (under_ || (float)ip_ + fr_ < lo) {
        if (!loop || hi - lo < 1.0f) {
          playing_ = false;
          for (; i < n; ++i) {
            out[i] = 0.0f;
            if (outr) outr[i] = 0.0f;
          }
          break;
        }
        ip_ = s0_, fr_ = 0.0f, under_ = false;   // back to the head (the start); the ring restarts below it
        rs_ = h0_, fill_ = 0;   // (never while step runs: see step's invariant)
      }
      float s0, s1, r0 = 0.0f, r1 = 0.0f;
      if (!sample(ip_, &s0, outr ? &r0 : nullptr) || !sample(ip_ + 1, &s1, outr ? &r1 : nullptr)) {
        ++underruns_;   // the card is behind: silence rather than garbage
        out[i] = 0.0f;
        if (outr) outr[i] = 0.0f;
        continue;
      }
      out[i] = (s0 + (s1 - s0) * fr_) * level;
      if (outr) outr[i] = (r0 + (r1 - r0) * fr_) * level;
      float f = fr_ - step;
      if (f >= 0.0f) {
        fr_ = f;
        continue;
      }
      uint32_t w = (uint32_t)ceilf(-f);
      if (w > ip_) under_ = true, ip_ = 0, fr_ = 0.0f;
      else ip_ -= w, fr_ = f + (float)w;
    }
    if (outr_copy_)
      for (int j = 0; j < n; ++j) outr_copy_[j] = out[j];
  }

  void advance(float step) {
    fr_ += step;
    uint32_t w = (uint32_t)fr_;
    ip_ += w;
    fr_ -= w;
  }

  // Frames [f0, f1) containing k, contiguous in RAM at base (frame f0).
  bool span(uint32_t k, const uint8_t** base, uint32_t* f0, uint32_t* f1) const {
    uint32_t b = k * bpf_;
    if (b >= h0_ && b + bpf_ <= h0_ + hlen_) {
      *f0 = h0_ / bpf_;
      *f1 = (h0_ + hlen_) / bpf_;
      *base = head_;   // h0_ is a whole number of frames
      return true;
    }
    if (b >= rs_ && b + bpf_ <= rs_ + fill_) {
      uint32_t wrap0 = b - b % kRing, lo = rs_ > wrap0 ? rs_ : wrap0;
      uint32_t hiB = rs_ + fill_ < wrap0 + kRing ? rs_ + fill_ : wrap0 + kRing;
      uint32_t a = (lo + bpf_ - 1) / bpf_, z = hiB / bpf_;
      if (k < a || k + 1 > z) return false;
      *f0 = a;
      *f1 = z;
      *base = ring_ + (a * bpf_ - wrap0);
      return true;
    }
    return false;
  }

  // Samples while ip_ and ip_ + 1 are in [f0, f1) and ip_ <= hi; how many.
  // FMT 1: 16-bit mono, 2: 16-bit stereo, 3: 24-bit mono, 4: 24-bit stereo.
  template <int FMT>
  int run(const uint8_t* base, uint32_t f0, uint32_t f1, uint32_t ihi, float hi, float step, float level,
          float* out, int n) {
    const uint32_t bpf = FMT == 1 ? 2 : FMT == 2 ? 4 : FMT == 3 ? 3 : 6;
    float hf = hi - ihi;
    uint32_t ip = ip_;
    float fr = fr_;
    int i = 0;
    for (; i < n; ++i) {
      if (ip < f0 || ip + 1 >= f1 || ip > ihi || (ip == ihi && fr > hf)) break;
      const uint8_t* p = base + (ip - f0) * bpf;
      float s0 = dec<FMT>(p), s1 = dec<FMT>(p + bpf);
      out[i] = (s0 + (s1 - s0) * fr) * level;
      fr += step;
      uint32_t w = (uint32_t)fr;
      ip += w;
      fr -= w;
    }
    ip_ = ip;
    fr_ = fr;
    return i;
  }
  // run's stereo form (FMT 2 / 4): L into out, R into outr
  template <int FMT>
  int run2(const uint8_t* base, uint32_t f0, uint32_t f1, uint32_t ihi, float hi, float step, float level, float* out,
           float* outr, int n) {
    const uint32_t bpf = FMT == 2 ? 4 : 6, bs = FMT == 2 ? 2 : 3;
    float hf = hi - ihi;
    uint32_t ip = ip_;
    float fr = fr_;
    int i = 0;
    for (; i < n; ++i) {
      if (ip < f0 || ip + 1 >= f1 || ip > ihi || (ip == ihi && fr > hf)) break;
      const uint8_t* p = base + (ip - f0) * bpf;
      float l0 = one<FMT>(p), l1 = one<FMT>(p + bpf), r0 = one<FMT>(p + bs), r1 = one<FMT>(p + bpf + bs);
      out[i] = (l0 + (l1 - l0) * fr) * level;
      outr[i] = (r0 + (r1 - r0) * fr) * level;
      fr += step;
      uint32_t w = (uint32_t)fr;
      ip += w;
      fr -= w;
    }
    ip_ = ip;
    fr_ = fr;
    return i;
  }
  template <int FMT>
  static inline float one(const uint8_t* q) {   // one channel's sample (FMT 2: 16-bit, 4: 24-bit)
    if (FMT == 2) return (int16_t)(q[0] | q[1] << 8) * (1.0f / 32768.0f);
    return (int32_t)(q[0] << 8 | q[1] << 16 | (uint32_t)q[2] << 24) * (1.0f / 2147483648.0f);
  }
  template <int FMT>
  static inline float dec(const uint8_t* q) {
    if (FMT == 1) return (int16_t)(q[0] | q[1] << 8) * (1.0f / 32768.0f);
    if (FMT == 2)
      return ((int16_t)(q[0] | q[1] << 8) + (int16_t)(q[2] | q[3] << 8)) * (0.5f / 32768.0f);
    if (FMT == 3) return (int32_t)(q[0] << 8 | q[1] << 16 | (uint32_t)q[2] << 24) * (1.0f / 2147483648.0f);
    return ((int32_t)(q[0] << 8 | q[1] << 16 | (uint32_t)q[2] << 24) +
            (int32_t)(q[3] << 8 | q[4] << 16 | (uint32_t)q[5] << 24)) *
           (0.5f / 2147483648.0f);
  }

  // Frame k (mono, -1..1) from the head or the ring; false if not loaded. r: its two channels instead (L in *out)
  bool sample(uint32_t k, float* out, float* r = nullptr) {
    uint32_t b = k * bpf_;
    const uint8_t* p;
    uint8_t tmp[8];   // a frame across the ring's end (on the stack: two render cores read at once)
    if (b >= h0_ && b + bpf_ <= h0_ + hlen_) {
      p = head_ + (b - h0_);
    } else if (b >= rs_ && b + bpf_ <= rs_ + fill_) {
      uint32_t w = b % kRing;
      if (w + bpf_ > kRing) {        // a frame across the ring's end
        for (uint32_t i = 0; i < bpf_; ++i) tmp[i] = ring_[(b + i) % kRing];
        p = tmp;
      } else {
        p = ring_ + w;
      }
    } else if (b >= rec_.data_len) {
      *out = 0.0f;
      if (r) *r = 0.0f;
      return true;
    } else {
      return false;
    }
    int ch = rec_.channels > 2 ? 2 : rec_.channels;
    int bytes = rec_.bits / 8;
    float s = 0.0f, c2[2] = {0.0f, 0.0f};
    for (int c = 0; c < ch; ++c) {
      const uint8_t* q = p + c * bytes;
      int32_t v;
      if (bytes == 2) v = (int16_t)(q[0] | q[1] << 8) << 16;
      else if (bytes == 3) v = (int32_t)(q[0] << 8 | q[1] << 16 | (uint32_t)q[2] << 24);
      else v = (int32_t)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24);
      c2[c] = v * (1.0f / 2147483648.0f);
      s += c2[c];
    }
    if (r) {
      *out = c2[0], *r = ch > 1 ? c2[1] : c2[0];
      return true;
    }
    *out = s / ch;
    return true;
  }

  core::File f_;
  card::FileRec rec_;
  uint8_t* head_ = nullptr;
  uint8_t* ring_ = nullptr;
  uint32_t bpf_ = 2, frames_ = 0, h0_ = 0, hlen_ = 0;
  uint32_t rs_ = 0, fill_ = 0;
  int folder_ = -1, smp_ = -1;
  uint32_t ip_ = 0;   // playback position: frame ip_ + fr_ (a float alone loses
  float fr_ = 0.0f;   // the fraction past 2^24 frames, ~6 min at 48 kHz)
  float step_ = 1.0f;
  uint8_t fmt_ = 0;   // run<>'s format, 0: another (sample by sample)
  float* outr_copy_ = nullptr;   // render's R for a mono file: L copied at the end
  bool ready_ = false, playing_ = false, first_ = false;
  bool rev_ = false, under_ = false;   // prepared to play backwards; reverse playback went below frame 0
  uint32_t s0_ = 0;                    // the frame playback starts on (forward: the head's first)
  uint32_t lo_ = 0;                    // reverse: the region's low frame (render's; 0 before the first render)
  uint32_t underruns_ = 0;
#if CORE_MSAMP
  core::cardq::Req rq_ = {};
  uint8_t rq_kind_ = kRqNone, rq_want_ = kRqNone;   // what rq_ is doing; kRqPost: rq_want_ still to be posted
  uint32_t rq_off_ = 0, rq_n_ = 0, rq_seq_ = 0, seq_ = 0, hwant_ = 0;
  Prep head_res_ = kPrepWait;   // the head's read ended (prepare_q's answer)
  bool pclose_ = false, urgent_ = false;
#endif
};

}  // namespace rytm
