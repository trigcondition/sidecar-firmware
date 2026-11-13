// core/tests/test_smpsnap.cpp - SAMPLER's zero-crossing snap (0.5.0): its STRT and END (and LOOP's point) land on the
// nearest zero crossing of the sample within a small window, at playback only (the stored values untouched); no
// crossing in the window keeps the raw point; STRT at the very start and END at the very end stay; DSP SAMPLE (and
// the sample editor's / a preview's frame region) never snap. Voice 0; SMP A001 = a sine (period 97 frames, phase 1.0,
// no DC), A002 = the same on a DC offset (never crosses 0), A003 = stereo (R half of L).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "core_port.h"   // host_fake_time
#include "engine.h"
#include "machines.h"
#include "pool/pool.h"
#include "pool/sample_ram_pool.h"
#include "voices.h"
#include "wav.h"

using namespace core;

static constexpr int kN = 48000;
static constexpr double kPer = 97.0;
static double sine(double f) { return 0.5 * std::sin(2.0 * M_PI * f / kPer + 1.0); }
static double dcsine(double f) { return 0.7 + 0.1 * std::sin(2.0 * M_PI * f / kPer + 1.0); }
static double frame_of(double p120) { return p120 / 120.0 * (kN - 1); }

static float P[16];
static void reset_p() {
  const float d[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100};
  memcpy(P, d, sizeof P);
}
static int g_tag = kSmplTag;
static void send(int cmd) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(kDspSample | 0)};
  if (cmd == 0x04) m.push_back(60), m.push_back(100);
  for (float x : P) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  if (g_tag >= 0) m.push_back((uint8_t)(g_tag >> 7 & 0x7f)), m.push_back((uint8_t)(g_tag & 0x7f));
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static uint64_t g_us = 1000000;
static int16_t ring[kBlock][kVoices];
static std::vector<double> render(int frames) {
  std::vector<double> o;
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < frames / kBlock; ++b) {
    idle_stream();
    render_voices(ring, kBlock, 0, 0, kVoices, mach);
    for (int i = 0; i < kBlock; ++i) o.push_back(atanh(ring[i][0] / (0.7 * 32767.0)));
  }
  return o;
}
static void trig() { send(0x04); }
// the current SMP in the sample RAM, voice 0 playing from it
static void warm() {
  send(0x03);
  bool ok = false;
  for (int i = 0; i < 20000 && !ok; ++i) {
    trig();
    idle_prepare();
    for (int k = 0; k < 8; ++k) idle_stream();
    g_us += 1000, host_fake_time(true, g_us);
    ok = from_ram[0];
  }
  CHECK(ok);
}
static int last_sounding(const std::vector<double>& o) {
  int n = -1;
  for (size_t i = 0; i < o.size(); ++i)
    if (o[i] != 0.0) n = (int)i;
  return n;
}
static bool near(double a, double b, double e) { return std::fabs(a - b) < e; }
static constexpr double kZero = 0.04;   // a sample this near 0 is at a crossing (the sine's step is 0.032)

// a P (0..120), from `from` up, whose raw frame has a loud sine sample (|x| > 0.4: mid-cycle, far from a crossing)
static float mid_cycle(double from) {
  for (float p = (float)from; p < 119; p += 0.25f)
    if (std::fabs(sine(frame_of(p))) > 0.4) return p;
  return -1;
}

static void t_start() {
  reset_p();
  P[10] = 1;
  float p = mid_cycle(20);
  CHECK(p > 0);
  P[12] = p;
  warm();
  trig();
  std::vector<double> o = render(256);
  CHECK(std::fabs(o[0]) < kZero);   // the snapped start: a crossing (the raw point is loud)
  CHECK(std::fabs(o[1]) < 0.08);    // and the sine goes on from there
  trig();                           // the same again: the same start (nothing stored changed)
  std::vector<double> o2 = render(256);
  CHECK(o2[0] == o[0]);
}

static void t_end_once() {
  reset_p();
  float p = mid_cycle(60);
  P[13] = p;   // END mid-cycle: FWD plays STRT..END once
  trig();
  std::vector<double> o = render(kN);
  int n = last_sounding(o);
  CHECK(n > 0 && n < kN - 100);
  CHECK(std::fabs(o[n]) < kZero);                 // it ends on a crossing
  CHECK(std::fabs(n - frame_of(p)) <= 96 + 2);   // not far from the asked point
  reset_p();
}

static void t_loop_seam() {
  reset_p();
  P[11] = 29;   // FLP: loops STRT..END whole (LOOP 0)
  // STRT on a positive phase, END on a negative one: the raw seam jumps by about 0.9
  float a = -1, b = -1;
  for (float p = 20; p < 100; p += 0.25f) {
    double s = sine(frame_of(p));
    if (a < 0 && s > 0.45) a = p;
    if (a > 0 && p > a + 5 && s < -0.45) {
      b = p;
      break;
    }
  }
  CHECK(a > 0 && b > 0);
  P[12] = a, P[13] = b;
  trig();
  std::vector<double> o = render((int)(frame_of(b) - frame_of(a)) * 3 + 2048);
  double worst = 0;
  for (size_t i = 1; i < o.size(); ++i) worst = std::fmax(worst, std::fabs(o[i] - o[i - 1]));
  CHECK(worst < 0.05);   // no click at the seam
  CHECK(engines[0].playing());
  reset_p();
}

// the first P (0..120) from `from` up whose raw frame's sine is past 0.45 in `sign`'s direction (mid-cycle)
static float find_mid(double from, int sign) {
  for (float p = (float)from; p < 119; p += 0.25f)
    if (sign * sine(frame_of(p)) > 0.45) return p;
  return -1;
}

static void t_loop_point() {
  reset_p();
  P[11] = 29;   // FLP: STRT..END, then [LOOP, END]
  // LOOP positive, END negative: the raw END -> LOOP seam jumps by about 0.9 (so the guard fails without the snap)
  float s = find_mid(10, +1), l = find_mid(s + 8, +1), e = find_mid(l + 8, -1);
  CHECK(s > 0 && l > s && e > l);
  CHECK(sine(frame_of(l)) - sine(frame_of(e)) > 0.8);
  P[12] = s, P[13] = e, P[14] = l;
  trig();
  std::vector<double> o = render((int)(frame_of(e) - frame_of(s)) * 3 + 4096);
  double worst = 0;
  for (size_t i = 1; i < o.size(); ++i) worst = std::fmax(worst, std::fabs(o[i] - o[i - 1]));
  CHECK(worst < 0.05);   // the seam END -> LOOP too
  reset_p();
}

// SampleModes on its own (a sample of the test's own, the engine's snap flag set here): the loop seams with the snap off
// (a click) and on (none), M1's collapse, a stereo sample out of phase, the corner cases
static rytm::SampleModes* new_sm() {
  rytm::SampleModes* sm = new rytm::SampleModes;
  sm->Init();
  return sm;
}
// frames (+ the guard frame) of a sine as int16, `ch` channels (R = -L when ch is 2)
static std::vector<int16_t> make_pcm(int frames, int ch, double phase) {
  std::vector<int16_t> v((size_t)(frames + 1) * ch);
  for (int i = 0; i <= frames; ++i) {
    int16_t x = (int16_t)std::lround(32767.0 * 0.5 * std::sin(2.0 * M_PI * i / kPer + phase));
    if (ch == 2) v[2 * i] = x, v[2 * i + 1] = (int16_t)-x;
    else v[i] = x;
  }
  return v;
}
static std::vector<float> run(rytm::SampleModes* sm, const float* sp, int n, std::vector<float>* r = nullptr) {
  std::vector<float> l(n), rr(n);
  sm->Render(sp, l.data(), n, r ? rr.data() : nullptr);
  if (r) *r = rr;
  return l;
}
static double worst_jump(const std::vector<float>& o) {
  double w = 0;
  for (size_t i = 1; i < o.size(); ++i) w = std::fmax(w, std::fabs(o[i] - o[i - 1]));
  return w;
}
// the sp (TUN FIN SMP MOD STA END LOP LEV); MOD 32: LOOP; 0: ONE
static void make_sp(float* sp, float mod, double sa, double sb) {
  const float d[8] = {64, 64, 0, mod, (float)sa, (float)sb, 0, 100};
  memcpy(sp, d, sizeof d);
}

static void loop_seam(bool reversed) {
  rytm::SampleModes* sm = new_sm();
  std::vector<int16_t> pcm = make_pcm(kN, 1, 1.0);
  float s = find_mid(10, +1), l, e;
  if (!reversed) l = find_mid(s + 8, +1), e = find_mid(l + 8, -1);   // FLP: END -> LOOP jumps
  else l = find_mid(s + 8, -1), e = find_mid(l + 8, +1);             // RLP: STRT <-> LOOP jumps
  CHECK(s > 0 && l > s && e > l);
  for (int on = 0; on < 2; ++on) {
    float sp[8];
    if (!reversed) make_sp(sp, 32, s, e);
    else make_sp(sp, 32, e, s);   // RLP: END..STRT swapped, plays [STRT, LOOP]
    sm->Set(pcm.data(), kN, 48000.0f);
    sm->SetSnap(on);
    sm->SetLoop(l);
    sm->Trigger(60.0f, sp);
    std::vector<float> o = run(sm, sp, (int)(frame_of(e) - frame_of(s)) * 3 + 4096);
    double w = worst_jump(o);
    if (on) CHECK(w < 0.05);   // snapped: no click at the seam
    else CHECK(w > 0.5);       // raw: the seam is a big jump (this guard bites without the snap)
  }
  delete sm;
}
static void t_loop_seam_flp() { loop_seam(false); }
static void t_loop_seam_rlp() { loop_seam(true); }

// M1: STRT 0, END and LOOP 6 frames apart, both next to the same crossing: the snaps would put them together
static void t_loop_collapse() {
  reset_p();
  P[11] = 36;   // FLP
  auto q = [](double x) { return std::floor(x * 128.0) / 128.0; };   // the sysex's 1/128 steps
  auto cross = [](double f) { return std::lround((f * 2.0 * M_PI / kPer + 1.0) / M_PI); };
  double e = -1, l = -1;
  for (double x = 2.0; x < 6.0 && e < 0; x += 1.0 / 128) {
    double xe = q(x), xl = q(x - 2.0 / 128);
    double fe = frame_of(xe), fl = frame_of(xl);
    double c = (cross(fe) * M_PI - 1.0) * kPer / (2.0 * M_PI);
    if (cross(fe) == cross(fl) && std::fabs(fe - c) < 20 && fe - fl > 5 && xl >= 1.0) e = xe, l = xl;
  }
  CHECK(e > 0);
  P[12] = 0, P[13] = (float)e, P[14] = (float)l;
  warm();
  trig();
  int n0 = (int)frame_of(e);
  std::vector<double> o = render(n0 + 4000);
  double mn = 1, mx = -1;
  for (size_t i = n0 + 800; i < o.size(); ++i) mn = std::fmin(mn, o[i]), mx = std::fmax(mx, o[i]);
  CHECK(mx - mn > 0.1 && mx - mn < 0.4);   // the raw ~6-frame ramp loops (not a 1-frame buzz, not the whole region)
  reset_p();
}

// L1: STRT / END 1.5 frames apart (forward and reversed, on a short sample so the knob is fine enough): the region keeps
// its direction and does not collapse (it plays its 1-2 frames from the raw start)
static void tiny(bool reversed) {
  const int N = 4000;
  rytm::SampleModes* sm = new_sm();
  std::vector<int16_t> pcm = make_pcm(N, 1, 1.0);
  const double fa = 32.2, fb = 33.7;   // the sine crosses at 33.06
  float sp[8];
  if (!reversed) make_sp(sp, 0, fa / (N - 1) * 120, fb / (N - 1) * 120);
  else make_sp(sp, 0, fb / (N - 1) * 120, fa / (N - 1) * 120);
  sm->Set(pcm.data(), N, 48000.0f);
  sm->SetSnap(true);
  sm->Trigger(60.0f, sp);
  std::vector<float> o = run(sm, sp, 16);
  double f0 = reversed ? fb : fa;
  double expect = sine(std::floor(f0)) + (sine(std::floor(f0) + 1) - sine(std::floor(f0))) * (f0 - std::floor(f0));
  CHECK(near(o[0], expect, 0.004));   // the raw start
  int n = -1;
  for (int i = 0; i < 16; ++i)
    if (o[i] != 0.0f) n = i;
  CHECK(n >= 0 && n < 6);             // a region of 1-2 frames, not run away
  delete sm;
}
static void t_tiny_fwd() { tiny(false); }
static void t_tiny_rev() { tiny(true); }

// Stereo snaps on the L+R sum (the engine plays the (L+R)/2 mix: Render without outr). L and R cross zero elsewhere than
// their sum does; STRT mid-cycle starts on the mix's crossing. (Separate L / R outputs, Render with outr, would need a
// per-channel rule.)
static void t_stereo_mix() {
  const int N = 4000;
  rytm::SampleModes* sm = new_sm();
  std::vector<int16_t> pcm((size_t)(N + 1) * 2);
  for (int i = 0; i <= N; ++i) {
    pcm[2 * i] = (int16_t)std::lround(32767.0 * 0.5 * std::sin(2.0 * M_PI * i / kPer + 1.0));
    pcm[2 * i + 1] = (int16_t)std::lround(32767.0 * 0.2 * std::sin(2.0 * M_PI * i / kPer + 3.0));
  }
  float sp[8];
  make_sp(sp, 0, 10.0 / (N - 1) * 120, 120);   // frame 10: the mix is loud
  sm->Set(pcm.data(), N, 48000.0f, 2);
  sm->SetSnap(true);
  sm->Trigger(60.0f, sp);
  std::vector<float> o = run(sm, sp, 64);
  CHECK(std::fabs(o[0]) < 0.04);   // the mix at a crossing
  double raw = 0.5 * (0.5 * std::sin(2.0 * M_PI * 10 / kPer + 1.0) + 0.2 * std::sin(2.0 * M_PI * 10 / kPer + 3.0));
  CHECK(std::fabs(raw) > 0.1);     // (the raw point is loud)
  delete sm;
}

// N1: an explicit frame region (SetFrames, the sample editor's / a preview's path) keeps its loop point, snap off
static void t_frames_loop() {
  const int N = 4000;
  rytm::SampleModes* sm = new_sm();
  std::vector<int16_t> pcm = make_pcm(N, 1, 1.0);
  float sp[8];
  make_sp(sp, 32, 0, 120);   // LOOP
  sm->Set(pcm.data(), N, 48000.0f);
  sm->SetFrames(100, 2000);
  sm->SetSnap(false);
  sm->SetLoop(1500.0f / (N - 1) * 120.0f);
  sm->Trigger(60.0f, sp);
  std::vector<float> o = run(sm, sp, 4200);
  // past the first pass (frame 2000) it loops [1500, 1999]: a period of 499 frames
  for (int i = 2200; i < 2600; ++i) CHECK(near(o[i], o[i + 499], 0.005));
  delete sm;
}

// M3: samples of 3 and 2 frames: no crash, they play as they are
static void t_short() {
  for (int fr = 2; fr <= 3; ++fr) {
    rytm::SampleModes* sm = new_sm();
    std::vector<int16_t> pcm = {8000, (int16_t)-8000, 12000, 12000};
    if (fr == 2) pcm = {8000, 20000, 20000};
    for (int mid = 0; mid < 2; ++mid) {
      float sp[8];
      make_sp(sp, 0, mid ? 60 : 0, 120);
      sm->Set(pcm.data(), fr, 48000.0f);
      sm->SetSnap(true);
      sm->Trigger(60.0f, sp);
      std::vector<float> o = run(sm, sp, 8);
      for (float x : o) CHECK(std::fabs(x) <= 20000.0 / 32768 + 1e-3);
      if (!mid) CHECK(near(o[0], 8000.0 / 32768, 0.002));
    }
    delete sm;
  }
}

// M3: a STRT change while playing, and a sample swap (or an in-place change) then a trig: the new point snaps
static void t_strt_change() {
  rytm::SampleModes* sm = new_sm();
  std::vector<int16_t> pcm = make_pcm(kN, 1, 1.0);
  sm->Set(pcm.data(), kN, 48000.0f);
  sm->SetSnap(true);
  float a1 = mid_cycle(15), a2 = a1 - 0.4f;
  float sp[8];
  make_sp(sp, 0, a1, a1 + 1.0);
  sp[6] = 1;   // ONE loops
  sm->Trigger(60.0f, sp);
  std::vector<float> o = run(sm, sp, 128);
  make_sp(sp, 0, a2, a1 + 1.0);
  sp[6] = 1;
  std::vector<float> o2 = run(sm, sp, 1500);   // wraps once, to the new STRT's crossing
  CHECK(worst_jump(o2) < 0.05);
  delete sm;
}
static void t_swap() {
  rytm::SampleModes* sm = new_sm();
  std::vector<int16_t> a = make_pcm(kN, 1, 1.0), b = make_pcm(kN, 1, 2.0);
  float sp[8];
  make_sp(sp, 0, 5.0, 120);   // 5 P = frame 1999: mid-cycle in A or B or neither; the snap makes either quiet
  sm->SetSnap(true);
  sm->Set(a.data(), kN, 48000.0f);
  sm->Trigger(60.0f, sp);
  CHECK(std::fabs(run(sm, sp, 4)[0]) < kZero);
  sm->Set(b.data(), kN, 48000.0f);   // another sample, the same knobs
  sm->Trigger(60.0f, sp);
  CHECK(std::fabs(run(sm, sp, 4)[0]) < kZero);
  a = make_pcm(kN, 1, 0.3);          // the same pointer, new content: Set returns early, the trig must search again
  sm->Set(a.data(), kN, 48000.0f);
  sm->Trigger(60.0f, sp);
  CHECK(std::fabs(run(sm, sp, 4)[0]) < kZero);
  delete sm;
}

// reverse: END < STRT (FWD), and the REV zone: the first and last samples sound at a crossing
static void t_reverse() {
  reset_p();
  P[12] = mid_cycle(60), P[13] = mid_cycle(20);
  CHECK(P[12] > P[13] && P[13] > 0);
  warm();
  trig();
  std::vector<double> o = render((int)(frame_of(P[12]) - frame_of(P[13])) + 1024);
  int n = last_sounding(o);
  CHECK(n > 0 && n < (int)o.size() - 100);
  CHECK(std::fabs(o[0]) < kZero);
  CHECK(std::fabs(o[n]) < kZero);
  reset_p();
}
static void t_rev_zone() {
  reset_p();
  P[11] = 21;   // REV: END down to STRT
  P[12] = mid_cycle(20), P[13] = mid_cycle(60);
  warm();
  trig();
  std::vector<double> o = render((int)(frame_of(P[13]) - frame_of(P[12])) + 1024);
  int n = last_sounding(o);
  CHECK(n > 0 && n < (int)o.size() - 100);
  CHECK(std::fabs(o[0]) < kZero);
  CHECK(std::fabs(o[n]) < kZero);
  reset_p();
}

static void t_edges() {
  reset_p();
  warm();
  trig();   // STRT 0, END 120 (the whole sample, FWD once): both ends stay
  std::vector<double> o = render(kN + 256);
  CHECK(std::fabs(sine(0)) > 0.3);
  CHECK(near(o[0], sine(0), 0.003));   // frame 0 as it is (a loud one), not moved to a crossing
  int n = last_sounding(o);
  CHECK(n > kN - 4);                   // plays to the very end (not cut short by a snap)
  reset_p();
}

static void t_no_crossing() {
  reset_p();
  P[10] = 2;   // the DC-offset sine: no sign change anywhere
  float p = mid_cycle(20);
  P[12] = p;
  warm();
  trig();
  std::vector<double> o = render(256);
  double raw = frame_of(p);
  uint32_t k = (uint32_t)raw;
  double f = raw - k, expect = dcsine(k) + (dcsine(k + 1) - dcsine(k)) * f;
  CHECK(near(o[0], expect, 0.003));   // the raw point kept
  reset_p();
}

static void t_stereo() {
  reset_p();
  P[10] = 3;
  float p = mid_cycle(20);
  P[12] = p;
  warm();
  trig();
  std::vector<double> o = render(256);
  CHECK(std::fabs(o[0]) < kZero);   // the channels' sum crosses: a crossing
  reset_p();
}

static void t_dsps_unchanged() {
  reset_p();
  g_tag = 0;   // DSP SAMPLE's tag: no snap
  P[10] = 1;
  float p = mid_cycle(20);
  P[12] = p;
  warm();
  trig();
  std::vector<double> o = render(256);
  double raw = frame_of(p);
  uint32_t k = (uint32_t)raw;
  double f = raw - k, expect = sine(k) + (sine(k + 1) - sine(k)) * f;
  CHECK(std::fabs(o[0]) > 0.3 && near(o[0], expect, 0.01));   // the raw point
  g_tag = kSmplTag;
  reset_p();
}

int main(int argc, char** argv) {
  char tmpl[] = "/tmp/core-smpsnap-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/S") && fs.mkdir("/.rytm/projects/SMP"));
  write_wav("/Samples/S/sine.wav", kN, 1, 16, 48000, [](int i, int) { return (int32_t)std::lround(32767.0 * sine(i)); });
  write_wav("/Samples/S/dc.wav", kN, 1, 16, 48000, [](int i, int) { return (int32_t)std::lround(32767.0 * dcsine(i)); });
  write_wav("/Samples/S/st.wav", kN, 2, 16, 48000,
            [](int i, int c) { return (int32_t)std::lround(32767.0 * sine(i) * (c ? 0.5 : 1.0)); });
  FILE* f = fopen(host_path("/.rytm/project").c_str(), "wb");
  fputs("SMP", f);
  fclose(f);
  f = fopen(host_path("/.rytm/projects/SMP/samples.pool.txt").c_str(), "wb");
  fprintf(f,
          "#sidecar samplepool 1\nA001 sample - %d /Samples/S/sine.wav\tsine\nA002 sample - %d /Samples/S/dc.wav\tdc\n"
          "A003 sample - %d /Samples/S/st.wav\tst\n",
          44 + 2 * kN, 44 + 2 * kN, 44 + 4 * kN);
  fclose(f);
  setenv("CORE_POOL_RAM_MB", "64", 1);
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);
  CHECK(engine_begin() && card_ok && psram && engines);
  for (int i = 0; i < 2000 && npool < 3; ++i) idle_stream();
  CHECK(npool == 3);

  struct T {
    const char* name;
    void (*fn)();
  } tests[] = {{"start", t_start},   {"end_once", t_end_once},   {"loop_seam", t_loop_seam},   {"loop_point", t_loop_point},
               {"edges", t_edges},   {"no_crossing", t_no_crossing}, {"stereo", t_stereo},     {"dsps_unchanged", t_dsps_unchanged},
               {"loop_seam_flp", t_loop_seam_flp}, {"loop_seam_rlp", t_loop_seam_rlp}, {"loop_collapse", t_loop_collapse},
               {"tiny_fwd", t_tiny_fwd}, {"tiny_rev", t_tiny_rev}, {"stereo_mix", t_stereo_mix}, {"frames_loop", t_frames_loop}, {"short", t_short},
               {"strt_change", t_strt_change}, {"swap", t_swap}, {"reverse", t_reverse}, {"rev_zone", t_rev_zone}};
  for (const T& t : tests)   // (one name on the command line: just that test, so a failing CHECK hides no other)
    if (argc < 2 || !strcmp(argv[1], t.name)) t.fn(), printf("test_smpsnap: %s ok\n", t.name);
  CHECK(system((std::string("rm -rf '") + tmpl + "'").c_str()) == 0);
  printf("test_smpsnap: PASS\n");
  return 0;
}
