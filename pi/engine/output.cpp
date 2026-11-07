// pi/engine/output.cpp - see output.h.
#include "output.h"

#include <string.h>

#include <new>

#include "fx/out_measure.h"
#include "fx/out_mix.h"

// the device's voice channels (core/fx/out.h Device: the Rytm's, ob_block.h's kVoiceToRytm)
const uint8_t Output::kVoiceToChannel[Output::kVoices] = {
    core::out::kRytmMk2.voice[0], core::out::kRytmMk2.voice[1], core::out::kRytmMk2.voice[2], core::out::kRytmMk2.voice[3],
    core::out::kRytmMk2.voice[4], core::out::kRytmMk2.voice[5], core::out::kRytmMk2.voice[6], core::out::kRytmMk2.voice[7]};

Output::Output() {
  static_assert(sizeof(core::out::Mixer) <= sizeof mix_mem_, "Output: the OUT mixer's room");
  mix_ = new (mix_mem_) core::out::Mixer();
}

void Output::Reset() {
  // Pops what is queued now, as the FIFO's consumer: the producer may keep pushing meanwhile, bounded by the count.
  const uint32_t level = fifo_.size();
  Frame f;
  for (uint32_t n = level; n && fifo_.pop(&f); --n) {
  }
  if (streams_ == 0) d_.pre_reset_pulled = d_.since_reset;
  d_ = Diag{d_.pre_reset_pulled, level, kNone, kNone, 0, kNone, kNone, kNone, kNone, 0};
  main_.Reset();
  armed_ = false;   // the refill burst that follows is the restart bucket's
  ++streams_;
}

uint32_t Output::Push(const int16_t (*frames)[kVoices], uint32_t n, const int16_t (*r)[kVoices], uint8_t st) {
  uint32_t k = 0;
  for (; k < n; ++k) {
    Frame f;
    memcpy(f.v, frames[k], sizeof f.v);
    memcpy(f.r, r ? r[k] : frames[k], sizeof f.r);
    f.st = st;
    if (!fifo_.push(f)) break;
  }
  return k;
}

// x before any limit. No gain or down: the -12 dBFS limit, then the gain, the Rytm's path bit for bit. Up (a box whose insert point sits low): the gain first and one knee
// toward full scale, so a hot voice is not cut at the limit and clipped again.
float Output::Own(float x, float t) {
  if (t > 1.0f) return core::out::Mixer::Limit(x * t, 0.9999f);
  x = Limit(x) * t;
  return x > 0.9999f ? 0.9999f : x < -0.9999f ? -0.9999f : x;
}

void Output::Pull(int32_t (*out)[kChannels], uint32_t n, int16_t (*voices)[kVoices], const Taps* taps) {
  const core::out::Control& c = *ctl_;
  bool block_set = false;
  const uint32_t base = d_.since_reset;
  if (d_.first_pull_level == kNone) d_.first_pull_level = fifo_.size();
  // armed: a stream is up (a Reset has come: the Rytm's stream, the TCP link, the self-test) and the FIFO has reached its
  // target since. Before any stream nothing is filling the FIFO for a reason, and no pull is a glitch.
  if (!armed_ && streams_ && target_ && fifo_.size() + kArmSlack >= target_)
    armed_ = true, d_.arm_at = base, d_.arm_level = fifo_.size();
  const Mode mode = GetMode();
  const bool test = core::out::meas::active();   // stereo=measure: its test signals (core/fx/out_measure.h)
  const float trk_gain = c.trk10 ? powf(10.0f, (float)c.trk10 / 200.0f) : 1.0f;   // stereo=trim:trk (once a block: a change is a step, a test setting)
  int32_t tp[kVoices] = {};   // each voice's largest |sample| on its own track this call (the web's meters, TakeTrkPeaks)
  float tg = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    for (int ch = 0; ch < kChannels; ++ch) out[i][ch] = 0;
    if (taps && taps->feed) memset(taps->feed[i], 0, sizeof taps->feed[i]);
    if (taps && taps->pairs) memset(taps->pairs[i], 0, sizeof taps->pairs[i]);
    Frame f;
    if (!fifo_.pop(&f)) {
      uint32_t pos = base + i;
      // a glitch: armed, or a stream (with a target) that has had its refill grace and still never reached it - a
      // stream that never fills is starved, and calling all of it "restart" read as `under 0` (audit 2026-10-26)
      if (armed_ || (streams_ && target_ && pos >= kRestartGrace)) {
        ++under_;
        if (d_.under_first == kNone) d_.under_first = pos;
        d_.under_last = pos;
      } else {
        ++restart_under_;
        if (d_.restart_first == kNone) d_.restart_first = pos;
        d_.restart_last = pos;
      }
      if (voices) memset(voices[i], 0, sizeof voices[i]);
      if (taps && taps->lr) memset(taps->lr[i], 0, sizeof taps->lr[i]);
      continue;
    }
    if (taps && taps->lr) memcpy(taps->lr[i], f.v, sizeof f.v), memcpy(taps->lr[i] + kVoices, f.r, sizeof f.r);
    float g = main_.Gain();
    tg = g;
    if (!test && mix_->AllTrk(c)) {   // every voice on TRK: the old path, bit for bit
      if (voices) memcpy(voices[i], f.v, sizeof f.v);
      for (int v = 0; v < kVoices; ++v) {   // (an integer abs and max a voice: the meter, nothing in the output)
        const int32_t a = f.v[v] < 0 ? -(int32_t)f.v[v] : f.v[v];
        if (a > tp[v]) tp[v] = a;
      }
      if (mode == kMain) {
        out[i][0] = out[i][1] = To32(main_.Sum(f.v, g));
      } else {
        for (int v = 0; v < kVoices; ++v) {
          const float y = f.v[v] / 32768.0f * g, x = Limit(y);
          float a = x < 0 ? -x : x;
          if (a > peak_) peak_ = a;
          const uint8_t ch = core::out::device().voice[v];   // the Rytm's: kVoiceToChannel, the old path bit for bit
          if (ch < kChannels) out[i][ch] = To32(Own(y, trk_gain));
        }
      }
      continue;
    }
    if (!block_set) mix_->Block(c, f.st), block_set = true;
    float trk[kVoices], pre[2], post[2], feed[kVoices];
    mix_->Frame(c, f.v, f.r, trk, pre, post, feed, vpk_);
    if (test) {
      float tp, tq, tt;
      int tv;
      if (core::out::meas::out_frame(&tp, &tq, &tt, &tv)) {
        pre[0] += tp, pre[1] += tp, post[0] += tq, post[1] += tq;
        trk[tv & 7] += tt * 32768.0f;
        if (trk[tv & 7] > 32767.0f) trk[tv & 7] = 32767.0f;
        if (trk[tv & 7] < -32768.0f) trk[tv & 7] = -32768.0f;
      }
    }
    int16_t m[kVoices];   // the TRK shares (int16 scale: |trk| <= 32767)
    for (int v = 0; v < kVoices; ++v) {
      m[v] = (int16_t)trk[v];
      const int32_t a = m[v] < 0 ? -(int32_t)m[v] : m[v];   // the meter: a voice's share on its own track
      if (a > tp[v]) tp[v] = a;
      if (voices) voices[i][v] = (int16_t)(((int32_t)f.v[v] + (int32_t)f.r[v]) / 2);
    }
    if (mode == kMain) {
      out[i][core::out::device().post[0]] = out[i][core::out::device().post[1]] = To32(main_.Sum(m, g));
    } else {
      for (int v = 0; v < kVoices; ++v) {
        const float y = trk[v] / 32768.0f * g, x = Limit(y);
        float a = x < 0 ? -x : x;
        if (a > peak_) peak_ = a;
        const uint8_t ch = core::out::device().voice[v];
        if (ch < kChannels) out[i][ch] = To32(Own(y, trk_gain));   // (a box with fewer voices: the rest have no channel)
      }
    }
    pre[0] *= g, pre[1] *= g, post[0] *= g, post[1] *= g;
    core::out::Mixer::Pairs(c, pre, post);
    for (int k = 0; k < 2; ++k) {
      float a = pre[k] < 0 ? -pre[k] : pre[k], b = post[k] < 0 ? -post[k] : post[k];
      if (a > peak_) peak_ = a;
      if (b > peak_) peak_ = b;
    }
    const core::out::Device& dv = core::out::device();   // the returns' channels (the device's table)
    out[i][dv.pre[0]] = Add(out[i][dv.pre[0]], pre[0]), out[i][dv.pre[1]] = Add(out[i][dv.pre[1]], pre[1]);
    out[i][dv.post[0]] = Add(out[i][dv.post[0]], post[0]), out[i][dv.post[1]] = Add(out[i][dv.post[1]], post[1]);
    if (taps && taps->feed)
      for (int v = 0; v < kVoices; ++v) taps->feed[i][v] = feed[v] * g;
    if (taps && taps->pairs) taps->pairs[i][0] = pre[0], taps->pairs[i][1] = pre[1], taps->pairs[i][2] = post[0], taps->pairs[i][3] = post[1];
  }
  d_.since_reset += n;
  for (int v = 0; v < kVoices; ++v)
    if (tp[v] > tpk_[v]) tpk_[v] = tp[v];
  if (tg > 0.0f) tgain_ = tg;
}

void Output::TakeTrkPeaks(float* pk) {
  const float g = tgain_ / 32768.0f;
  for (int v = 0; v < kVoices; ++v) {
    float a = (float)tpk_[v] * g;
    pk[v] = a > 1.0f ? 1.0f : a;
    tpk_[v] = 0;
  }
}

void Output::TakeVoicePeaks(float (*pk)[2]) {
  for (int v = 0; v < kVoices; ++v)
    for (int c = 0; c < 2; ++c) pk[v][c] = vpk_[v][c], vpk_[v][c] = 0.0f;
}

float Output::TakePeak() {
  float p = main_.TakePeak();
  if (peak_ > p) p = peak_;
  peak_ = 0.0f;
  return p;
}
