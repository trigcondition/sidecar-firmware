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
// PCM 16/24/32-bit, 1..2 channels (more: the first two), any rate; render()
// plays mono (a stereo file summed), or with outr its two channels (a voice on
// OUT PRE / POST, docs/STEREO.md; a mono file on both sides). Not here yet:
// reverse playback (END before STA) of files longer than the head - played
// forward instead.
#pragma once
#include "../platform.h"
#include <math.h>

#include "../pool/card_index.h"

namespace rytm {

class StreamVoice {
  static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

 public:
  static const uint32_t kHead = 8192;    // bytes from STA, in RAM
  static const uint32_t kRing = 16384;   // what follows, refilled from the card
  static const uint32_t kChunk = 8192;   // a read: fewer, bigger reads are far
                                         // cheaper on the card than many small

  void attach(uint8_t* head, uint8_t* ring) {
    head_ = head;
    ring_ = ring;
  }

  // The file (from the card index) and STA (0..1 of the data): open, read the
  // head. Returns false if the file cannot be read.
  bool prepare(card::Index& cards, int folder, int smp, float start) {
    card::FileRec r;
    core::File f;
    if (!cards.file(folder, smp, &r) || !cards.open(folder, r, &f)) return false;
    return prepare(r, f, folder, smp, start);
  }

  // The same with the file's record and an open copy of it at hand (the
  // sketch keeps one per pool slot): no index lookup or open on the card,
  // only the head's read (8 KB, ~0.65 ms).
  bool prepare(const card::FileRec& r, const core::File& file, int folder, int smp, float start) {
    uint32_t bpf = frame_bytes(r);
    uint32_t frames = bpf ? r.data_len / bpf : 0;
    uint32_t h0 = (uint32_t)(clamp01(start) * (frames ? frames - 1 : 0)) * bpf;
    if (ready_ && folder == folder_ && smp == smp_ && h0 == h0_) return true;
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
    uint32_t want = umin(kHead, r.data_len - umin(h0, r.data_len));
    want -= want % bpf_;
    if (!f_.seekSet(r.data_offset + h0) || (uint32_t)f_.read(head_, want) != want) return false;
    hlen_ = want;
    ready_ = true;
    return true;
  }

  // No sample (SMP off, an empty pool slot): silent until the next prepare.
  void clear() {
    playing_ = false;
    ready_ = false;
    if (f_.isOpen()) f_.close();
  }

  // The note stops, the sample stays prepared (the voice plays from RAM now).
  void stop() { playing_ = false; }

  void trigger() {
    if (!ready_) return;
    ip_ = h0_ / bpf_;
    fr_ = 0.0f;
    rs_ = h0_ + hlen_;     // the ring starts where the head ends
    fill_ = 0;
    playing_ = true;
    first_ = true;
  }

  // Between render blocks: top the ring up by at most one chunk. Returns bytes
  // read.
  uint32_t refill() {
    if (!playing_ || !f_.isOpen()) return 0;
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
    float lo = a < b ? a : b, hi = a < b ? b : a;   // reverse: forward for now
    if (first_) {
      ip_ = h0_ / bpf_;
      fr_ = 0.0f;
      first_ = false;
    }
    (void)lo;
    float step = rec_.rate / 48000.0f * powf(2.0f, semitones / 12.0f);
    step_ = step;
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
        ip_ = h0_ / bpf_;   // back to the head (STA); the ring restarts
        fr_ = 0.0f;
        rs_ = h0_ + hlen_;
        fill_ = 0;
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
    uint32_t end = rs_ + fill_;   // bytes in RAM from the head on, contiguously
    if (rs_ > h0_ + hlen_ && ip_ * bpf_ < rs_) end = h0_ + hlen_;   // looped back into the head
    if (end < h0_ + hlen_) end = h0_ + hlen_;
    if (end >= rec_.data_len) return cap;
    uint32_t have = end / bpf_;
    if (have <= ip_ + 1) return 0;
    float o = (have - ip_ - 1) / (step_ > 0.01f ? step_ : 0.01f);
    return o >= cap ? cap : (uint32_t)o;
  }

  bool playing() const { return playing_; }
  uint32_t underruns() const { return underruns_; }
  const char* name() const { return rec_.name; }

 private:
  static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
  static uint32_t frame_bytes(const card::FileRec& r) {
    return (uint32_t)r.channels * (r.bits / 8);
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
    if (b >= h0_ && b + bpf_ <= h0_ + hlen_) {
      p = head_ + (b - h0_);
    } else if (b >= rs_ && b + bpf_ <= rs_ + fill_) {
      uint32_t w = b % kRing;
      if (w + bpf_ > kRing) {        // a frame across the ring's end
        static uint8_t tmp[8];
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
  uint32_t underruns_ = 0;
};

}  // namespace rytm
