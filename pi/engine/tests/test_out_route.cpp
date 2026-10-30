// pi/engine/tests/test_out_route.cpp - the OUT route in the output stage (core/fx/out.h, docs/STEREO.md): every voice on
// TRK = the old output bit for bit (tracks and main); TRK -> its track channel only, PRE -> 2/3 only, POST -> 0/1 only,
// out=main: the TRK sum on 0/1 plus the pairs; a stereo pair on POST at unity (LEV 100, PAN C, WID 64) sample-exact on
// 0/1; OUT switched every 100 ms under a sustained sine: no step beyond the crossfade's bound; the mute gate fades in
// 5 ms, a TRK voice untouched by it; the send FX feed = the voice's mono sum on PRE / POST, 0 on TRK; WID 0 = mono;
// PAN hard left; the pairs' limit; the cost of the mixer a frame.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "check.h"
#include "fx/out_measure.h"
#include "output.h"

using core::out::kPost;
using core::out::kPre;
using core::out::kTrk;

static const int kB = 7;   // a transport block's frames (ob::kFramesPerBlock)

// the output stage before the OUT route (output.cpp at 814bdb5), for the bit-for-bit comparison
struct OldOut {
  core::MainOut main;
  bool tracks = true;
  void frame(const int16_t* v, int32_t* out) {
    for (int c = 0; c < 12; ++c) out[c] = 0;
    float g = main.Gain();
    if (!tracks) {
      out[0] = out[1] = (int32_t)(main.Sum(v, g) * 2147483648.0f);
      return;
    }
    for (int k = 0; k < 8; ++k) {
      float x = v[k] / 32768.0f * g;
      x = x > 0.25f ? 0.25f : x < -0.25f ? -0.25f : x;
      out[Output::kVoiceToChannel[k]] = (int32_t)(x * 2147483648.0f);
    }
  }
};

static int16_t sine(double hz, double a, long i) { return (int16_t)std::lround(a * 32767.0 * std::sin(2 * M_PI * hz * i / 48000.0)); }

// feeds frames [t, t + n) of a generator into the stage and pulls them back
template <class G>
static void run(Output& o, long& t, int n, G gen, std::vector<std::vector<int32_t>>* got, std::vector<std::vector<float>>* feed = nullptr,
                std::vector<std::vector<int16_t>>* mono = nullptr) {
  for (int done = 0; done < n; done += kB) {
    int16_t l[kB][8], r[kB][8];
    for (int f = 0; f < kB; ++f) gen(t + f, l[f], r[f]);
    CHECK(o.Push(l, kB, r, 0xFF) == (uint32_t)kB);
    int32_t out[kB][12];
    float fd[kB][8];
    int16_t vm[kB][8];
    Output::Taps tp;
    tp.feed = fd;
    o.Pull(out, kB, vm, &tp);
    for (int f = 0; f < kB; ++f) {
      if (got) got->push_back(std::vector<int32_t>(out[f], out[f] + 12));
      if (feed) feed->push_back(std::vector<float>(fd[f], fd[f] + 8));
      if (mono) mono->push_back(std::vector<int16_t>(vm[f], vm[f] + 8));
    }
    t += kB;
  }
}

int main() {
  // ---- every voice TRK: the old stage, bit for bit, both modes, with and without an R plane
  for (int mode = 0; mode < 2; ++mode) {
    static Output o;
    o.Reset();
    o.SetMode(mode ? Output::kTracks : Output::kMain);
    OldOut ref;
    ref.tracks = mode == 1;
    srand(7);
    for (int k = 0; k < 4000; ++k) {
      int16_t blk[kB][8];
      for (int f = 0; f < kB; ++f)
        for (int v = 0; v < 8; ++v) blk[f][v] = (int16_t)((rand() % 65536) - 32768);
      if (k & 1) o.Push(blk, kB);
      else o.Push(blk, kB, blk, 0);
      int32_t out[kB][12];
      int16_t vm[kB][8];
      o.Pull(out, kB, vm);
      for (int f = 0; f < kB; ++f) {
        int32_t want[12];
        ref.frame(blk[f], want);
        for (int c = 0; c < 12; ++c) CHECK(out[f][c] == want[c]);
        for (int v = 0; v < 8; ++v) CHECK(vm[f][v] == blk[f][v]);
      }
    }
  }
  printf("test_out_route: TRK bit for bit (tracks, main)\n");

  static Output o;
  core::out::Control& c = o.OwnControl();
  o.SetMode(Output::kTracks);
  long t = 0;
  // past the 200 ms fade-in first
  auto st = [](long i, int16_t* l, int16_t* r) {
    for (int v = 0; v < 8; ++v) l[v] = r[v] = 0;
    l[3] = sine(440, 0.1, i), r[3] = sine(660, 0.1, i);   // voice 3 (BT): a stereo pair
    l[0] = r[0] = sine(220, 0.1, i);                      // voice 0 (BD): mono
  };
  run(o, t, 48000 / 5 + 70, st, nullptr);

  // ---- routing: voice 3 POST -> 0/1 only (sample-exact L / R at unity), PRE -> 2/3 only; voice 0 stays on its channel
  c.route[3] = kPost;
  run(o, t, 2 * core::out::kXfadeFrames, st, nullptr);   // past the crossfade
  std::vector<std::vector<int32_t>> got;
  std::vector<std::vector<float>> feed;
  std::vector<std::vector<int16_t>> mono;
  long t0 = t;
  run(o, t, 4800, st, &got, &feed, &mono);
  for (size_t i = 0; i < got.size(); ++i) {
    long fr = t0 + (long)i;
    CHECK(got[i][0] == (int32_t)sine(440, 0.1, fr) * 65536 && got[i][1] == (int32_t)sine(660, 0.1, fr) * 65536);
    CHECK(got[i][2] == 0 && got[i][3] == 0 && got[i][Output::kVoiceToChannel[3]] == 0);
    CHECK(got[i][Output::kVoiceToChannel[0]] == (int32_t)sine(220, 0.1, fr) * 65536);   // TRK: its channel, as before
    float want = ((float)sine(440, 0.1, fr) / 32768.0f + (float)sine(660, 0.1, fr) / 32768.0f) * 0.5f;
    CHECK(std::fabs(feed[i][3] - want) < 1e-6f && feed[i][0] == 0.0f);   // the send FX's stand-in for the BT stem
    CHECK(mono[i][3] == (int16_t)(((int)sine(440, 0.1, fr) + sine(660, 0.1, fr)) / 2));
  }
  c.route[3] = kPre;
  run(o, t, 2 * core::out::kXfadeFrames, st, nullptr);
  got.clear();
  t0 = t;
  run(o, t, 4800, st, &got);
  for (size_t i = 0; i < got.size(); ++i) {
    long fr = t0 + (long)i;
    CHECK(got[i][2] == (int32_t)sine(440, 0.1, fr) * 65536 && got[i][3] == (int32_t)sine(660, 0.1, fr) * 65536);
    CHECK(got[i][0] == 0 && got[i][1] == 0 && got[i][Output::kVoiceToChannel[3]] == 0);
  }
  // out=main: the TRK voices' sum on 0/1 (x 0.5), the PRE pair on 2/3
  o.SetMode(Output::kMain);
  got.clear();
  t0 = t;
  run(o, t, 480, st, &got);
  for (size_t i = 0; i < got.size(); ++i) {
    long fr = t0 + (long)i;
    int32_t m = (int32_t)((float)sine(220, 0.1, fr) * (0.5f / 32768.0f) * 2147483648.0f);
    CHECK(got[i][0] == m && got[i][1] == m && got[i][2] == (int32_t)sine(440, 0.1, fr) * 65536);
  }
  o.SetMode(Output::kTracks);
  printf("test_out_route: TRK / PRE / POST on their channels, out=main, POST sample-exact, the feed\n");

  // ---- no clicks: a full-scale-ish sustained sine on voice 3 (mono), OUT changed every 100 ms
  auto loud = [](long i, int16_t* l, int16_t* r) {
    for (int v = 0; v < 8; ++v) l[v] = r[v] = 0;
    l[3] = r[3] = sine(100, 0.2, i);
  };
  const int seq[] = {kTrk, kPost, kPre, kTrk, kPre, kPost, kTrk};
  c.route[3] = kTrk;
  run(o, t, 4800, loud, nullptr);
  got.clear();
  for (int r : seq) {
    c.route[3] = (uint8_t)r;
    run(o, t, 4802, loud, &got);
  }
  // the sine's own largest step: 0.2 x 2 pi 100 / 48000 of full scale; the crossfade adds at most the gain's step
  // (pi / 2 / 384 a frame) x the level
  double bound = (0.2 * 2 * M_PI * 100 / 48000 + 0.2 * M_PI / 2 / core::out::kXfadeFrames) * 2147483648.0 * 1.05;
  double worst = 0;
  for (size_t i = 1; i < got.size(); ++i)
    for (int ch : {0, 1, 2, 3, (int)Output::kVoiceToChannel[3]}) {
      double d = std::fabs((double)got[i][ch] - got[i - 1][ch]);
      if (d > worst) worst = d;
    }
  CHECK(worst <= bound);
  printf("test_out_route: OUT switched every 100 ms: worst step %.5f of full scale (bound %.5f)\n", worst / 2147483648.0,
         bound / 2147483648.0);

  // ---- the mute gate: voice 3 on POST, its track (BT, track 4) muted: out in 5 ms, monotone; TRK voice 0 untouched
  c.route[3] = kPost;
  auto both = [](long i, int16_t* l, int16_t* r) {
    for (int v = 0; v < 8; ++v) l[v] = r[v] = 0;
    l[3] = r[3] = 4000;   // DC: the gain is the level (below the limit's knee)
    l[0] = r[0] = 4000;
  };
  run(o, t, 4800, both, nullptr);
  c.open[3] = 0;
  got.clear();
  run(o, t, 700, both, &got);
  int at = -1;
  for (size_t i = 0; i < got.size(); ++i) {
    if (i) CHECK(got[i][0] <= got[i - 1][0]);
    if (got[i][0] == 0 && at < 0) at = (int)i;
    CHECK(got[i][Output::kVoiceToChannel[0]] == 4000 * 65536);
  }
  CHECK(at > 200 && at <= core::out::kGateFrames + 1);
  c.open[3] = 1;
  got.clear();
  run(o, t, 700, both, &got);
  for (size_t i = 1; i < got.size(); ++i) CHECK(got[i][0] >= got[i - 1][0]);
  CHECK(got.back()[0] == 4000 * 65536);
  printf("test_out_route: the mute gate: silent after %d frames (5 ms = %d), back in 5 ms\n", at, core::out::kGateFrames);

  // ---- WID 0: mono (L = R = the mid); PAN hard left on a stereo pair: R silent, L as is; LEV 50: half
  auto lr = [](long i, int16_t* l, int16_t* r) {
    for (int v = 0; v < 8; ++v) l[v] = r[v] = 0;
    l[3] = 3000, r[3] = 1000;
  };
  c.wid[3] = 0;
  run(o, t, 4800, lr, nullptr);
  got.clear();
  run(o, t, 7, lr, &got);
  CHECK(got.back()[0] == got.back()[1] && std::abs(got.back()[0] - 2000 * 65536) < 4);
  c.wid[3] = 64, c.pan[3] = 1;
  run(o, t, 4800, lr, nullptr);
  got.clear();
  run(o, t, 7, lr, &got);
  CHECK(std::abs(got.back()[0] - 3000 * 65536) < 4 && std::abs(got.back()[1]) < 4);
  c.pan[3] = 64, c.lev[3] = 50;
  run(o, t, 4800, lr, nullptr);
  got.clear();
  run(o, t, 7, lr, &got);
  CHECK(std::abs(got.back()[0] - 1500 * 65536) < 4 && std::abs(got.back()[1] - 500 * 65536) < 4);
  c.lev[3] = 100;
  // ---- the limit: POST at full scale never past -12 dBFS
  auto full = [](long i, int16_t* l, int16_t* r) {
    for (int v = 0; v < 8; ++v) l[v] = r[v] = 32767;
  };
  for (int v = 0; v < 8; ++v) c.route[v] = kPost;
  run(o, t, 4800, full, nullptr);
  got.clear();
  run(o, t, 70, full, &got);
  CHECK(got.back()[0] <= (int32_t)(core::out::kLimitPost * 2147483648.0f) && got.back()[0] > (int32_t)(0.24f * 2147483648.0f));
  printf("test_out_route: WID 0 mono, PAN hard left, LEV 50, the POST limit OK\n");

  // ---- stereo=measure (core/fx/out_measure.h) against a model of the Rytm: MAIN IN = 0.5 x POST (40 frames late) +
  // 0.16 x PRE (40 late) + 0.8 x voice 0's track channel (60 late: the analog chain's block) + a little noise; then
  // a Rytm whose MAIN IN leaves POST out
  for (int variant = 0; variant < 2; ++variant) {
    static Output om;
    core::out::Control& cm = om.OwnControl();
    for (int v = 0; v < 8; ++v) cm.route[v] = kTrk;
    om.SetMode(Output::kTracks);
    core::out::meas::reset();
    const double gpost = variant ? 0.0 : 0.5, gpre = 0.16, gtrk = 0.8;
    const int dpost = 40, dpre = 40, dtrk = 60;
    std::vector<double> hp(4096, 0), hr(4096, 0), ht(4096, 0);
    uint32_t w = 0, rnd = 1;
    auto rytm = [&](int32_t (*o)[12], int n) {
      for (int f = 0; f < n; ++f, ++w) {
        hp[w & 4095] = (o[f][0] + (double)o[f][1]) * 0.5 / 2147483648.0;
        hr[w & 4095] = (o[f][2] + (double)o[f][3]) * 0.5 / 2147483648.0;
        ht[w & 4095] = o[f][Output::kVoiceToChannel[0]] / 2147483648.0;
        rnd = rnd * 1664525u + 1013904223u;
        double x = gpost * hp[(w - dpost) & 4095] + gpre * hr[(w - dpre) & 4095] + gtrk * ht[(w - dtrk) & 4095] +
                   1e-4 * ((double)(rnd >> 8) / 16777216.0 - 0.5);
        int32_t s = (int32_t)(x * 2147483647.0);
        if (core::out::meas::active()) core::out::meas::in_frame(s, s);   // (gated as the Pi's IN completion)
      }
    };
    int16_t z[kB][8] = {};
    for (int k = 0; k < 2000; ++k) {   // past the fade-in
      om.Push(z, kB);
      int32_t o[kB][12];
      om.Pull(o, kB);
      rytm(o, kB);
    }
    core::out::meas::start(true, 0);
    for (int k = 0; k < 48000 * 8 / kB && core::out::meas::phase() != core::out::meas::kDone; ++k) {
      om.Push(z, kB);
      int32_t o[kB][12];
      om.Pull(o, kB);
      rytm(o, kB);
    }
    for (int k = 0; k < 10; ++k) {   // the IN side closes the last phase
      om.Push(z, kB);
      int32_t o[kB][12];
      om.Pull(o, kB);
      rytm(o, kB);
    }
    const core::out::meas::Result& r = core::out::meas::result();
    printf("test_out_route: measure (model %s): floor %.1f dB, POST %.2f dB%s, PRE %.2f dB, TRK %.2f dB, delays %d / %d\n",
           variant ? "without POST in MAIN" : "as inferred", r.floor_db, r.gain_db[0], r.heard[0] ? "" : " (not heard)",
           r.gain_db[1], r.gain_db[2], (int)r.delay[0], (int)r.delay[1]);
    CHECK(r.done && r.trk && r.heard[1] && r.heard[2]);
    CHECK(std::fabs(r.gain_db[1] - 20 * std::log10(gpre)) < 0.3 && std::fabs(r.gain_db[2] - 20 * std::log10(gtrk)) < 0.3);
    if (!variant) {
      CHECK(r.heard[0] && std::fabs(r.gain_db[0] - 20 * std::log10(gpost)) < 0.3);
      CHECK(r.delay[0] >= 0 && r.delay[1] >= 0 && r.delay[1] - r.delay[0] == dtrk - dpost);
    } else {
      CHECK(!r.heard[0] && r.delay[0] < 0);
    }
  }

  // ---- the cost: every voice on POST, a 100 ms crossfade window included
  {
    long tt = 0;
    std::vector<std::vector<int32_t>>* none = nullptr;
    auto t1 = std::chrono::steady_clock::now();
    const int frames = 480000;
    for (int k = 0; k < frames / 4800; ++k) {
      for (int v = 0; v < 8; ++v) c.route[v] = (uint8_t)(k & 1 ? kPost : kPre);
      run(o, tt, 4802, st, none);
    }
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count();
    printf("test_out_route: Output::Pull with 8 stereo voices crossfading: %.1f ns a frame on this Mac (%.2f %% of a "
           "core at 48 kHz)\n", us * 1000 / frames, us / frames * 48000 / 1e4);
  }
  printf("test_out_route: PASS\n");
  return 0;
}
