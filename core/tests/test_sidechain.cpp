// core/tests/test_sidechain.cpp - the SIDECHAIN stage of the INSERT chains (core/fx/sidechain.h, insert.h RunSc,
// docs/INSERT-FX.md): PUMP's envelope shape and timing sample for sample, its trig sources (Rytm track, voice, AUDIO IN's
// level onset), DUCK as the INSERT COMP keyed from the source (gain reduction against the key's level, the key HPF, and
// the same answer as a COMP slot when the key is the signal itself), the key band-pass KBAS / KWID (its response through
// MON, OFF bit for bit) and MON itself, off = untouched, a missing source = no effect, the
// control side (modes, defaults, wanted sources, status, console, json), the mod targets, and the stage's cost.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "core.h"
#include "rui_png.h"
#include "fx/insert.h"
#include "lfo.h"
#include "mod/target.h"
#include "ui/extra.h"
#include "ui/page.h"
#include "ui/remote.h"
#include "ui/ui.h"

using namespace core;
using ins::Engine;

static const float kSr = 48000.0f;
static std::vector<float> g_mem((size_t)ins::kLines * ins::kLine);
static Engine fresh() {
  Engine e(kSr);
  std::fill(g_mem.begin(), g_mem.end(), 0.0f);
  e.Place((float(*)[ins::kLine])g_mem.data());
  e.Reset();
  ins::reset();
  memset((void*)&sc::bus(), 0, sizeof(sc::Bus));
  sc::bus().wanted_trig = ~0u;
  return e;
}
static void setsc(int c, int mode, int src, std::vector<int> p) {
  ins::control.scmode[c] = (uint8_t)mode;
  ins::control.scsrc[c] = (uint8_t)src;
  for (int k = 0; k < sc::kParams; ++k) ins::control.scp[c][k] = (uint8_t)(k < (int)p.size() ? p[k] : 0);
}
// PUMP's numbers, as the design says them (sidechain.h / the note): DPTH linear, ATT / HOLD / REL squared knobs
static int att_n(int v) { float p = v / 127.0f; return (int)(0.03f * p * p * kSr + 0.5f); }
static int hold_n(int v) { float p = v / 127.0f; return (int)(0.25f * p * p * kSr + 0.5f); }
static int rel_n(int v) { float p = v / 127.0f; return (int)((0.01f + 1.49f * p * p) * kSr + 0.5f); }

static const int kB = 16;   // a block = a chunk
// constant `level` through chain 0, `total` frames in blocks of 16; the stems alive (zeros written) unless !alive;
// trigs at the frames in `trigs` (a trig lands at the start of its block)
static std::vector<float> pump(Engine& e, int total, const std::vector<int>& trigs, bool alive = true, float level = 0.5f,
                               int track = 0) {
  std::vector<float> y(total, level);
  float z[kB] = {};
  for (int i = 0; i < total; i += kB) {
    if (alive) sc::write(sc::kTrackStem[track], z, kB);
    for (int t : trigs)
      if (t >= i && t < i + kB) sc::trig_track(track);
    e.ProcessMono(0, &y[i], kB, ins::control);
  }
  return y;
}
static void __attribute__((noinline)) poison_stack() {   // what an uninitialised local in the next calls would read
  volatile float a[8192];
  for (int i = 0; i < 8192; ++i) a[i] = 3.0e30f;
}
static float db(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

int main() {
  // ---- PUMP: instant attack, hold, linear release, sample for sample from the trig's frame
  {
    Engine e = fresh();
    const int D = 127, A = 0, H = 40, R = 30, S = 64, T0 = 480;
    setsc(0, sc::kPump, 0, {D, A, H, R, S, 50});
    CHECK(att_n(A) == 0);
    const int hn = hold_n(H), rn = rel_n(R);
    auto y = pump(e, 12000, {T0});
    for (int i = 0; i < T0; ++i) CHECK(y[i] == 0.5f);                       // nothing before the trig, bit for bit
    for (int j = 0; j < hn; ++j) CHECK(std::fabs(y[T0 + j]) < 1e-6f);        // gmin = 0 from the trig's own frame
    for (int i = 1; i <= rn; ++i) CHECK(std::fabs(y[T0 + hn + i - 1] - 0.5f * (float)i / (float)rn) < 2e-6f);
    for (int i = T0 + hn + rn; i < 12000; ++i) CHECK(y[i] == 0.5f);        // back at unity on the last release frame
    printf("PUMP: instant attack at the trig frame %d, hold %d, linear release %d frames, then unity: exact\n", T0, hn, rn);
    // a depth of 64 (gmin = 1 - 64/127) and a linear attack of attN frames
    e = fresh();
    setsc(0, sc::kPump, 0, {64, 64, 20, 20, 64, 50});
    const int an = att_n(64), h2 = hold_n(20), r2 = rel_n(20);
    const float gmin = 1.0f - 64.0f / 127.0f;
    y = pump(e, 12000, {T0});
    for (int j = 0; j < an; ++j) CHECK(std::fabs(y[T0 + j] - 0.5f * (1.0f - (1.0f - gmin) * (float)(j + 1) / (float)an)) < 2e-6f);
    for (int j = an; j < an + h2; ++j) CHECK(std::fabs(y[T0 + j] - 0.5f * gmin) < 2e-6f);
    CHECK(y[T0 + an + h2 + r2 - 1] == 0.5f);
    printf("PUMP: attack %d frames linear to %.3f, hold %d, release %d: exact\n", an, gmin, h2, r2);
    // SHAP: the release curve u^e (e = 2^((v-64)/32)): below 64 the gain is back sooner than linear, above later
    e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 0, 60, 20, 50});
    auto fast = pump(e, 20000, {T0});
    e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 0, 60, 120, 50});
    auto slow = pump(e, 20000, {T0});
    const int rm = rel_n(60);
    CHECK(fast[T0 + rm / 2] > 0.5f * 0.5f + 0.02f && slow[T0 + rm / 2] < 0.5f * 0.5f - 0.02f);
    CHECK(std::fabs(fast[T0 + rm - 1] - 0.5f) < 1e-3f && std::fabs(slow[T0 + rm - 1] - 0.5f) < 1e-3f);
    printf("PUMP SHAP: halfway through the release 0.5 x %.2f (SHAP 20), 0.5 x %.2f (SHAP 120); both at unity at the end\n",
           fast[T0 + rm / 2] / 0.5f, slow[T0 + rm / 2] / 0.5f);
    // a retrigger in the middle of the release goes down from where the gain is (no jump up)
    e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 20, 60, 64, 50});
    const int t2 = T0 + hold_n(20) + rm / 2 + 3;
    y = pump(e, 20000, {T0, t2});   // the second lands half way through the release
    int t1 = t2 / kB * kB;
    CHECK(y[t1 - 1] > 0.1f && y[t1] <= y[t1 - 1] + 1e-6f);   // it was coming back up
    CHECK(std::fabs(y[t1]) < 1e-6f);   // instant attack: down to gmin again
    printf("PUMP retrigger: restarts the envelope at the next trig\n");
  }

  // ---- PUMP's trig sources: a Rytm track's trig (through its stem being there), a voice's trig, AUDIO IN's level onset
  {
    Engine e = fresh();
    setsc(0, sc::kPump, 3, {127, 0, 20, 20, 64, 50});   // CP: stem 2 (shared with RS), its own trig
    auto y = pump(e, 3000, {960}, true, 0.5f, 3);
    CHECK(y[959] == 0.5f && std::fabs(y[960]) < 1e-6f);
    e = fresh();
    setsc(0, sc::kPump, 2, {127, 0, 20, 20, 64, 50});   // RS shares CP's stem: CP's trig does not fire RS
    y = pump(e, 3000, {960}, true, 0.5f, 3);
    for (float v : y) CHECK(v == 0.5f);
    printf("PUMP: a track's trig fires its own envelope only (RS and CP share a stem, not a trig)\n");
    // a voice's trig
    e = fresh();
    setsc(0, sc::kPump, sc::kV0 + 2, {127, 0, 20, 20, 64, 50});
    std::vector<float> v(3000, 0.5f);
    float z[kB] = {};
    for (int i = 0; i < 3000; i += kB) {
      sc::write(sc::kStems + 2 + 2, z, kB);
      if (i == 960) sc::trig_voice(2);
      e.ProcessMono(0, &v[i], kB, ins::control);
    }
    CHECK(v[959] == 0.5f && std::fabs(v[960]) < 1e-6f);
    printf("PUMP: a voice's trig (V3) fires it\n");
    // AUDIO IN: no trig, a level onset (the key's 80 Hz high pass, a peak follower, THR, a re-arm below half of it)
    e = fresh();
    const int THR = 64, B0 = 4800;
    setsc(0, sc::kPump, sc::kAinL, {127, 0, 20, 20, 64, THR});
    std::vector<float> w(14400, 0.5f), key(kB);
    int first = -1;
    for (int i = 0; i < 14400; i += kB) {
      for (int k = 0; k < kB; ++k) key[k] = i + k >= B0 ? 0.3f * std::sin(2.0f * (float)M_PI * 100.0f * (float)(i + k - B0) / kSr) : 0.0f;
      sc::write(sc::kStems, key.data(), kB);
      e.ProcessMono(0, &w[i], kB, ins::control);
      for (int k = 0; k < kB; ++k)
        if (first < 0 && w[i + k] < 0.49f) first = i + k;
    }
    printf("PUMP from AUDIO IN: the onset at frame %d (the signal starts at %d)\n", first, B0);
    CHECK(first >= B0 && first < B0 + 96);
    for (int i = 0; i < B0; ++i) CHECK(w[i] == 0.5f);
    // one trig for the sustained tone, not one per cycle: back at unity once the envelope has run out, and it stays
    const int run = hold_n(20) + rel_n(20);
    for (int i = first + run + 480; i < 14400; ++i) CHECK(w[i] == 0.5f);
    printf("PUMP from AUDIO IN: one envelope for a sustained tone, none before it\n");
  }

  // ---- DUCK: the INSERT COMP keyed by the source
  {
    // the key is the signal itself (a voice's own pre-chain sound): the same answer as a COMP slot with the same knobs
    const int THR = 70, RAT = 100, SPD = 40;
    Engine e = fresh(), ref = fresh();
    setsc(0, sc::kDuck, sc::kV0, {THR, RAT, SPD, SPD, 0, 0});   // V1's own signal as the key
    ins::control.type[1][0] = ins::kComp;
    for (int k = 0; k < 4; ++k) ins::control.play[1][0][k] = (uint8_t)(k == 0 ? THR : k == 1 ? RAT : k == 2 ? SPD : 0);
    const int N = 24000;
    std::vector<float> a(N), b(N), x(N);
    for (int i = 0; i < N; ++i) {   // a 4 Hz square of loud / quiet bursts of a 220 Hz tone
      float amp = ((i / 6000) & 1) ? 0.04f : 0.7f;
      x[i] = a[i] = b[i] = amp * std::sin(2.0f * (float)M_PI * 220.0f * (float)i / kSr);
    }
    for (int i = 0; i < N; i += kB) {
      sc::write(sc::kStems + 2, &x[i], kB);
      e.ProcessMono(0, &a[i], kB, ins::control);
      ref.ProcessMono(1, &b[i], kB, ins::control);
    }
    float worst = 0.0f, ref_pk = 0.0f;
    for (int i = 2000; i < N; ++i) worst = std::max(worst, std::fabs(a[i] - b[i])), ref_pk = std::max(ref_pk, std::fabs(b[i]));
    printf("DUCK keyed by the signal itself vs a COMP slot: worst difference %.2e (peak %.2f)\n", worst, ref_pk);
    CHECK(worst < 3e-4f);
    // and it really reduced the loud bursts (COMP's own gain reduction)
    float gl = 0.0f, gq = 0.0f;
    for (int i = 5000; i < 6000; ++i) gl = std::max(gl, std::fabs(a[i]));
    for (int i = 11000; i < 12000; ++i) gq = std::max(gq, std::fabs(a[i]));
    CHECK(gl < 0.7f * 0.5f && gq <= 0.04f * 1.001f);   // the loud burst came down a lot, the quiet one is not louder
  }
  {
    // gain reduction against the key's level: a quiet key under the threshold leaves the signal alone, a loud one ducks it
    auto gr_db = [](float key_amp, float key_hz, int kbas) {
      Engine e = fresh();
      setsc(0, sc::kDuck, 0, {64, 100, 10, 10, kbas, 0});   // BD's stem, THR -20 dB
      std::vector<float> y(9600), k(kB);
      for (int i = 0; i < 9600; ++i) y[i] = 0.2f * std::sin(2.0f * (float)M_PI * 3000.0f * (float)i / kSr);
      std::vector<float> x = y;
      for (int i = 0; i < 9600; i += kB) {
        for (int j = 0; j < kB; ++j) k[j] = key_amp * std::sin(2.0f * (float)M_PI * key_hz * (float)(i + j) / kSr);
        sc::write(0, k.data(), kB);
        e.ProcessMono(0, &y[i], kB, ins::control);
      }
      double sx = 0, sy = 0;
      for (int i = 4800; i < 9600; ++i) sx += x[i] * x[i], sy += y[i] * y[i];
      return db((float)std::sqrt(sy / sx));
    };
    float quiet = gr_db(0.02f, 1000, 0), mid = gr_db(0.2f, 1000, 0), loud = gr_db(0.8f, 1000, 0);
    printf("DUCK gain against the key's level (THR -20 dB, RAT 100): key -34 dB %.2f dB, -14 dB %.2f dB, -2 dB %.2f dB\n", quiet, mid, loud);
    CHECK(quiet > -0.1f && mid < -2.0f && loud < mid - 4.0f);
    // the key's high pass (KBAS at about 1 kHz): a 50 Hz key stops driving it, a 5 kHz key still does
    const int kb = sc::kbas_from_old_hpf(100);
    float lo_off = gr_db(0.8f, 50, 0), lo_on = gr_db(0.8f, 50, kb), hi_on = gr_db(0.8f, 5000, kb);
    printf("DUCK key high pass: 50 Hz key %.2f dB off, %.2f dB with KBAS %.0f Hz; 5 kHz key %.2f dB\n", lo_off, lo_on, sc::kbas_hz(kb), hi_on);
    CHECK(lo_off < -6.0f && lo_on > -0.5f && hi_on < -6.0f);
  }


  // ---- the key band-pass KBAS / KWID and MON (0.5.2): the response is measured through MON, which plays the filtered key
  {
    // MON on, the chain's own signal a constant 0.3 (not in the output), THR / RAT such that nothing is reduced; a sine key
    // of `hz` on BD's stem; the gain (dB) of the monitored key over the last 4800 frames
    auto kf_db = [](int kbas, int kwid, float hz, float sr_scale = 1.0f) {
      Engine e = fresh();
      setsc(0, sc::kDuck, 0, {0, 0, 10, 10, kbas, kwid});
      ins::control.scmon[0] = 1;
      const int N = 28800;
      std::vector<float> y(N, 0.3f), k(kB);
      double sx = 0, sy = 0;
      for (int i = 0; i < N; i += kB) {
        for (int j = 0; j < kB; ++j) k[j] = 0.5f * std::sin(2.0f * (float)M_PI * hz * sr_scale * (float)(i + j) / kSr);
        sc::write(0, k.data(), kB);
        e.ProcessMono(0, &y[i], kB, ins::control);
        if (i >= N - 4800)
          for (int j = 0; j < kB; ++j) sx += k[j] * k[j], sy += y[i + j] * y[i + j];
      }
      return 10.0f * std::log10((float)(sy / sx) + 1e-20f);
    };
    // the knobs' scales: KBAS OFF or 20 Hz .. 19.6 kHz (log), KWID OFF or 0.5 .. 9.9 octaves
    CHECK(sc::kbas_hz(0) == 0.0f && std::fabs(sc::kbas_hz(1) - 20.0f) < 0.01f && std::fabs(sc::kbas_hz(127) - 19600.0f) < 20.0f);
    CHECK(sc::kwid_oct(0) == 0.0f && std::fabs(sc::kwid_oct(1) - 0.5f) < 1e-4f && std::fabs(sc::kwid_oct(127) - 9.9f) < 1e-3f);
    // KBAS: a 3rd order high pass: -3 dB at the corner, 18 dB per octave below it, flat above
    {
      const int KB = 70;
      const float f0 = sc::kbas_hz(KB);
      const float a = kf_db(KB, 0, f0), b = kf_db(KB, 0, f0 / 4.0f), c = kf_db(KB, 0, f0 / 8.0f), d = kf_db(KB, 0, f0 * 8.0f);
      printf("KBAS %.0f Hz: corner %.2f dB, f/4 %.1f dB, f/8 %.1f dB (slope %.1f dB/oct), 8f %.2f dB\n", f0, a, b, c, (c - b), d);
      CHECK(std::fabs(a + 3.01f) < 0.5f);
      CHECK(std::fabs((c - b) + 18.06f) < 1.2f);
      CHECK(std::fabs(d) < 0.15f);
    }
    // KWID: a 2nd order low pass KWID octaves above KBAS: -3 dB there, 12 dB per octave above it
    {
      const int KB = 20, KW = 50;
      const float f1 = sc::kbas_hz(KB) * std::exp2(sc::kwid_oct(KW));
      const float a = kf_db(KB, KW, f1), b = kf_db(KB, KW, f1 * 2.0f), c = kf_db(KB, KW, f1 * 4.0f), d = kf_db(KB, KW, f1 / 8.0f);
      printf("KWID: low pass at %.0f Hz: corner %.2f dB, 2f %.1f dB, 4f %.1f dB (slope %.1f dB/oct)\n", f1, a, b, c, c - b);
      CHECK(std::fabs(a + 3.01f) < 0.5f);
      CHECK(std::fabs((c - b) + 11.8f) < 1.5f);
      CHECK(std::fabs(d) < 0.3f);   // (the high pass at 20 Hz x 2^KWID below)
    }
    // KBAS OFF, KWID on: a plain low pass from 20 Hz x 2^KWID
    {
      const int KW = 70;
      const float f1 = 20.0f * std::exp2(sc::kwid_oct(KW));
      const float a = kf_db(0, KW, f1), lo = kf_db(0, KW, 40.0f);
      printf("KBAS OFF, KWID %.2f oct: low pass at %.0f Hz: corner %.2f dB, 40 Hz %.2f dB\n", sc::kwid_oct(KW), f1, a, lo);
      CHECK(std::fabs(a + 3.01f) < 0.5f && std::fabs(lo) < 0.05f);
    }
    // both OFF: MON plays the key exactly (bit for bit), and with the chain's own signal not in it
    {
      Engine e = fresh();
      setsc(0, sc::kDuck, 0, {0, 0, 10, 10, 0, 0});
      ins::control.scmon[0] = 1;
      const int N = 4800;
      std::vector<float> y(N, 0.3f), key(N);
      bool same = true;
      for (int i = 0; i < N; ++i) key[i] = 0.5f * std::sin(0.031f * (float)i) + 0.2f * std::sin(0.7f * (float)i);
      for (int i = 0; i < N; i += kB) {
        sc::write(0, &key[i], kB);
        e.ProcessMono(0, &y[i], kB, ins::control);
      }
      for (int i = 1000; i < N; ++i) same = same && y[i] == key[i];
      CHECK(same);
      printf("MON with KBAS and KWID OFF: the key, bit for bit\n");
    }
    // MON in stereo: both sides the key; with the key filtered it is the same filtered mono key on L and R
    {
      Engine e = fresh();
      setsc(8, sc::kDuck, 0, {0, 0, 10, 10, 50, 60});
      ins::control.scmon[8] = 1;
      float y[kB][2], k[kB];
      float worst = 0.0f;
      for (int it = 0; it < 600; ++it) {
        for (int j = 0; j < kB; ++j) k[j] = 0.5f * std::sin(0.05f * (float)(it * kB + j)), y[j][0] = 0.3f, y[j][1] = -0.2f;
        sc::write(0, k, kB);
        e.ProcessStereo(8, y, kB, ins::control);
        if (it > 400)
          for (int j = 0; j < kB; ++j) worst = std::max(worst, std::fabs(y[j][0] - y[j][1]));
      }
      CHECK(worst == 0.0f);
    }
    // MON ramps (5 ms): switched on and off in the middle of a signal, no step
    {
      Engine e = fresh();
      setsc(0, sc::kDuck, 0, {0, 0, 10, 10, 40, 80});
      const int N = 9600, on_at = 3200, off_at = 6400;
      std::vector<float> y(N, 0.3f), key(N), ref(N, 0.3f);
      for (int i = 0; i < N; ++i) key[i] = 0.5f * std::sin(2.0f * (float)M_PI * 1000.0f * (float)i / kSr);
      Engine e2 = fresh();
      setsc(0, sc::kDuck, 0, {0, 0, 10, 10, 40, 80});
      std::vector<float> mon(N, 0.3f);
      ins::control.scmon[0] = 0;
      for (int i = 0; i < N; i += kB) {
        ins::control.scmon[0] = i >= on_at && i < off_at;
        sc::write(0, &key[i], kB);
        e.ProcessMono(0, &y[i], kB, ins::control);
      }
      float step = 0.0f, offdiff = 0.0f;
      for (int i = 1; i < N; ++i) step = std::max(step, std::fabs(y[i] - y[i - 1]));
      for (int i = off_at + 256; i < N; ++i) offdiff = std::max(offdiff, std::fabs(y[i] - 0.3f));
      printf("MON on / off: the largest sample step %.4f (the key alone steps up to %.4f), back to the chain within 5 ms: %.1e\n", step,
             0.5f * 2.0f * (float)M_PI * 1000.0f / kSr, offdiff);
      CHECK(step < 0.1f);
      CHECK(offdiff < 1e-6f);
      CHECK(y[on_at - 1] == 0.3f && y[on_at + 300] != 0.3f);
      (void)e2;
    }
    // MON is DUCK's: PUMP plays its own gain whatever scmon says
    {
      Engine e = fresh();
      setsc(0, sc::kPump, 0, {127, 0, 20, 20, 64, 50});
      ins::control.scmon[0] = 1;
      auto y = pump(e, 3000, {960});
      CHECK(y[959] == 0.5f && std::fabs(y[960]) < 1e-6f);
    }
    // a sweep of both knobs, with and without MON, a loud key and a chirp: nothing but finite numbers (and no fault)
    {
      for (int sr_i = 0; sr_i < 2; ++sr_i) {
        const float sr = sr_i ? 22050.0f : kSr;
        std::vector<float> mem((size_t)ins::kLines * ins::kLine);
        Engine e(sr);
        e.Place((float(*)[ins::kLine])mem.data());
        e.Reset();
        ins::reset();
        memset((void*)&sc::bus(), 0, sizeof(sc::Bus));
        bool fin = true;
        float peak = 0.0f;
        for (int kb = 0; kb < 128; kb += 9)
          for (int kw = 0; kw < 128; kw += 9) {
            setsc(0, sc::kDuck, 0, {64, 60, 10, 10, kb, kw});
            ins::control.scmon[0] = (kb + kw) & 1;
            float k[kB], y[kB];
            for (int it = 0; it < 200; ++it) {
              for (int j = 0; j < kB; ++j) {
                const float t = (float)(it * kB + j);
                k[j] = 0.9f * std::sin(0.00004f * t * t) + ((it * 7 + j) % 3 == 0 ? 0.3f : -0.2f);
                y[j] = 0.4f * std::sin(0.02f * t);
              }
              sc::write(0, k, kB);
              e.ProcessMono(0, y, kB, ins::control);
              for (int j = 0; j < kB; ++j) fin = fin && std::isfinite(y[j]), peak = std::max(peak, std::fabs(y[j]));
            }
          }
        printf("KBAS x KWID sweep at %.0f Hz: finite %d, peak %.2f, faults %d\n", sr, (int)fin, peak, (int)e.Faults());
        CHECK(fin && peak < 4.0f && e.Faults() == 0);
      }
      ins::control.scmon[0] = 0;
    }
  }

  // ---- MON's control side: a per chain flag, off by default, off after a mode or source change, console and /status
  {
    ins::reset();
    char o[400];
    ins::set_sc_mode(1, sc::kDuck);
    CHECK(!ins::sc_mon(1));
    CHECK(ins::command("ins=V2:sc:mon:on", o, sizeof o) && ins::sc_mon(1) && ins::control.scmon[1] == 1);
    static char js[16384];
    ins::json(js, sizeof js);
    CHECK(strstr(js, "\"mon\":1") != nullptr);
    CHECK(ins::command("ins=V2:sc:mon:off", o, sizeof o) && !ins::sc_mon(1));
    ins::json(js, sizeof js);
    CHECK(!strstr(js, "\"mon\":1") && strstr(js, "\"mon\":0"));
    ins::set_sc_mon(1, true);
    ins::set_sc_src(1, 3);                    // a source change: MON off
    CHECK(!ins::sc_mon(1));
    ins::set_sc_mon(1, true);
    ins::set_sc_mode(1, sc::kPump);           // a mode change: MON off
    CHECK(!ins::sc_mon(1));
    ins::set_sc_mon(1, true);                 // PUMP has no key to hear: refused
    CHECK(!ins::sc_mon(1));
    ins::set_sc_mon(5, true);                 // a chain whose stage is off: refused
    CHECK(!ins::sc_mon(5));
    ins::set_sc_mode(1, sc::kDuck);
    ins::set_sc_mon(1, true);
    ins::set_sc_mode(1, sc::kOff);
    CHECK(!ins::sc_mon(1));
    ins::set_sc_mode(1, sc::kDuck);
    CHECK(!ins::sc_mon(1));
    ins::set_sc_mon(1, true);
    ins::set_sc_param(1, 4, 50);              // a knob does not turn it off
    CHECK(ins::sc_mon(1));
    ins::sc_mon_off_all();
    CHECK(!ins::sc_mon(1));
    ins::set_sc_mon(1, true);
    ins::reset();                             // a kit change / reset: off
    CHECK(!ins::sc_mon(1) && ins::control.scmon[1] == 0);
    printf("MON control: off by default, console + /status, off on a mode / source change, PUMP / OFF refuse it\n");
  }

  // ---- fix round 1: MON across a DUCK -> PUMP mode change (H1), a runaway key (M3), the web's lease (M1)
  {
    auto run = [&](bool mon) {
      Engine e = fresh();
      setsc(0, sc::kDuck, 0, {0, 0, 10, 10, 40, 60});
      ins::control.scmon[0] = mon;
      std::vector<float> y(2400, 0.5f), k(kB);
      for (int i = 0; i < 2400; i += kB) {
        for (int j = 0; j < kB; ++j) k[j] = 0.5f * std::sin(0.05f * (float)(i + j));
        sc::write(0, k.data(), kB);
        e.ProcessMono(0, &y[i], kB, ins::control);
      }
      setsc(0, sc::kPump, 0, {127, 0, 20, 20, 64, 50});   // the mode changes, MON gone (set_sc_mode clears it)
      ins::control.scmon[0] = 0;
      std::vector<float> o(960, 0.5f);
      for (int i = 0; i < 960; i += kB) {
        poison_stack();
        sc::write(0, k.data(), kB);
        e.ProcessMono(0, &o[i], kB, ins::control);
      }
      return o;
    };
    auto a = run(true), b = run(false);
    bool fin = true;
    for (float v : a) fin = fin && std::isfinite(v) && std::fabs(v) < 2.0f;
    CHECK(fin);
    CHECK(memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);   // the same as PUMP that never had MON
    printf("MON then PUMP: finite, and the PUMP result without MON, bit for bit\n");
  }
  {
    // a runaway key (full-scale x 1e6, then a NaN) does not get through MON
    Engine e = fresh();
    setsc(0, sc::kDuck, 0, {0, 0, 10, 10, 0, 0});
    ins::control.scmon[0] = 1;
    float peak = 0.0f;
    bool fin = true;
    for (int it = 0; it < 400; ++it) {
      float k[kB], y[kB];
      for (int j = 0; j < kB; ++j) k[j] = it < 200 ? 1.0e6f * std::sin(0.05f * (float)(it * kB + j)) : (j == 3 ? NAN : 0.1f), y[j] = 0.3f;
      sc::write(0, k, kB);
      e.ProcessMono(0, y, kB, ins::control);
      for (int j = 0; j < kB; ++j) fin = fin && std::isfinite(y[j]), peak = std::max(peak, std::fabs(y[j]));
    }
    printf("MON with a runaway key: peak %.2f, finite %d\n", peak, (int)fin);
    CHECK(fin && peak <= 2.0f);
  }
  {
    // the web's MON (console) lapses unless refreshed (3 s); the Rytm's T1 path does not
    ins::reset();
    char o[400];
    uint64_t us = 5000000000ull;
    host_fake_time(true, us);
    ins::set_sc_mode(1, sc::kDuck);
    CHECK(ins::command("ins=V2:sc:mon:on", o, sizeof o) && ins::sc_mon(1));
    host_fake_time(true, us += 2000000);
    ins::save();
    CHECK(ins::sc_mon(1));                                  // 2 s: still on
    CHECK(ins::command("ins=V2:sc:mon:on", o, sizeof o));   // the web refreshes it
    host_fake_time(true, us += 2000000);
    ins::save();
    CHECK(ins::sc_mon(1));                                  // 2 s after the refresh: still on
    host_fake_time(true, us += 2000000);
    ins::save();
    CHECK(!ins::sc_mon(1) && ins::control.scmon[1] == 0);   // 4 s without one: lapsed
    ins::set_sc_mon(1, true);                               // the Rytm's T1: no lease
    host_fake_time(true, us += 60000000);
    ins::save();
    CHECK(ins::sc_mon(1));
    ins::set_sc_mon(1, false);
    CHECK(ins::command("ins=V2:sc:mon:on", o, sizeof o) && ins::command("ins=V2:sc:mon:off", o, sizeof o) && !ins::sc_mon(1));
    // renew-only: never switches MON on, only extends the web's lease
    ins::set_sc_mode(1, sc::kDuck);
    CHECK(ins::command("ins=V2:sc:mon:renew", o, sizeof o) && !ins::sc_mon(1));   // off: stays off
    CHECK(ins::command("ins=V2:sc:mon:on", o, sizeof o) && ins::sc_mon(1));
    for (int i = 0; i < 4; ++i) {   // renewed every 2 s: still on after 8 s
      host_fake_time(true, us += 2000000);
      CHECK(ins::command("ins=V2:sc:mon:renew", o, sizeof o));
      ins::save();
    }
    CHECK(ins::sc_mon(1));
    ins::set_sc_mon(1, false);                                                      // a T1 off on the Rytm
    CHECK(ins::command("ins=V2:sc:mon:renew", o, sizeof o) && !ins::sc_mon(1));    // a late beat: stays off
    ins::set_sc_mon(1, true);                                                       // T1 on: no lease, renew does not give it one
    CHECK(ins::command("ins=V2:sc:mon:renew", o, sizeof o));
    host_fake_time(true, us += 60000000);
    ins::save();
    CHECK(ins::sc_mon(1));
    ins::set_sc_mon(1, false);
    host_fake_time(false, 0);
    ins::reset();
    printf("MON from the web lapses after 3 s without a refresh; T1's does not\n");
  }

  // ---- the Rytm's SIDECHAIN view: T1 toggles MON (a trig key the page takes in that view only, its LED shows it), the knobs
  // are KBAS / KWID, leaving the view or the page turns it off
  {
    ins::reset();
    int ki = -1, km = -1;
    for (int i = 0; i < ui::extra_pages(); ++i) {
      if (!strcmp(ui::extra_page(i).title, "INSERT")) ki = i;
      if (!strcmp(ui::extra_page(i).title, "MASTER")) km = i;
    }
    const ui::ExtraPage& pg = ui::extra_page(ki);
    ui::ins_page_set(false, 0, 0, 0);
    CHECK(!pg.trigs_now || pg.trigs_now() == 0);           // not in the SIDECHAIN view: no trig key is the page's
    CHECK(pg.key(0x41) && pg.key(0x43));                  // RIGHT, DOWN: the SIDECHAIN view
    CHECK(pg.trigs_now && pg.trigs_now() == 0);            // the stage is off: no MON key
    pg.enc(0, 2, false);                                    // DUCK
    CHECK(ins::sc_mode(0) == sc::kDuck && pg.trigs_now() == 1u);
    uint8_t leds[16];
    CHECK(pg.leds_now && pg.leds_now(leds));
    const uint8_t led_off = leds[0];
    CHECK(pg.key(0));                                       // T1: MON on
    CHECK(ins::sc_mon(0));
    CHECK(pg.leds_now(leds) && leds[0] != led_off);
    CHECK(pg.key(0) && !ins::sc_mon(0));                    // again: off
    CHECK(pg.key(0) && ins::sc_mon(0));
    {   // the shots of the view (build/rui_sidechain): DUCK with KBAS / KWID set, MON off and on, and a source that is not there
      const std::string dir = rui_png_dir("RUI_SIDECHAIN", "build/rui_sidechain");
      uint8_t fb[ui::kBytes];
      ui::Canvas sc_cv;
      ins::set_sc_param(0, 4, 60), ins::set_sc_param(0, 5, 40);
      ins::set_sc_mon(0, false);
      CHECK(ui::remote_render(6 + ki, 0, nullptr, sc_cv));
      ui::preview(sc_cv, fb);
      rui_png(dir, "duck_kbas_kwid.png", fb);
      ins::set_sc_mon(0, true);
      CHECK(ui::remote_render(6 + ki, 0, nullptr, sc_cv));
      ui::preview(sc_cv, fb);
      rui_png(dir, "duck_mon_on.png", fb);
      ins::set_sc_mon(0, false);
    }
    CHECK(pg.key(0));
    CHECK(pg.key(0x44) && !ins::sc_mon(0));                // YES: another chain (the bus): MON off
    CHECK(pg.key(0x44) && pg.key(0x44) && pg.key(0x44));   // bus 2, 3, then the voice again
    CHECK(pg.key(0) && ins::sc_mon(0));
    CHECK(pg.key(0x43) && !ins::sc_mon(0));                // DOWN, back to the EQ view: MON off
    CHECK(pg.key(0x43));                                    // again to the SIDECHAIN view
    CHECK(pg.key(0) && ins::sc_mon(0));
    ui::sc_mon_page_left();                                 // the page left (remote.cpp): MON off on every chain
    CHECK(!ins::sc_mon(0));
    // the MASTER page's view: the same T1, M1 / M2 by YES
    ins::reset();
    CHECK(km >= 0);
    const ui::ExtraPage& mg = ui::extra_page(km);
    CHECK(mg.key(0x41) && mg.key(0x43));                  // RIGHT, DOWN
    CHECK(mg.trigs_now && mg.trigs_now() == 0);
    mg.enc(0, 2, false);                                    // DUCK on M1
    CHECK(ins::sc_mode(ins::kM0) == sc::kDuck && mg.trigs_now() == 1u);
    CHECK(mg.key(0) && ins::sc_mon(ins::kM0));
    CHECK(mg.key(0x44) && !ins::sc_mon(ins::kM0));          // YES: M2 (another chain: MON off)
    CHECK(mg.trigs_now() == 0);                              // M2's stage is off: no MON key
    CHECK(mg.key(0x44) && mg.key(0) && ins::sc_mon(ins::kM0) && mg.key(0x43) && !ins::sc_mon(ins::kM0));   // M1 again, MON, out of the view: off
    ins::reset();
  }

  // ---- off: untouched, and no work
  {
    Engine e = fresh();
    ins::control.type[0][0] = ins::kDrive;
    for (int k = 0; k < 4; ++k) ins::control.play[0][0][k] = (uint8_t)(k == 0 ? 60 : k == 3 ? 127 : 20);
    std::vector<float> x(4800), a, b;
    for (int i = 0; i < 4800; ++i) x[i] = 0.3f * std::sin(2.0f * (float)M_PI * 330.0f * (float)i / kSr);
    a = x;
    for (int i = 0; i < 4800; i += 12) e.ProcessMono(0, &a[i], 12, ins::control);
    // the stage's settings are in the control block but the mode is OFF: the same bits
    for (int k = 0; k < sc::kParams; ++k) ins::control.scp[0][k] = 127;
    ins::control.scsrc[0] = 5;
    Engine e2 = fresh();
    ins::control.type[0][0] = ins::kDrive;
    for (int k = 0; k < 4; ++k) ins::control.play[0][0][k] = (uint8_t)(k == 0 ? 60 : k == 3 ? 127 : 20);
    for (int k = 0; k < sc::kParams; ++k) ins::control.scp[0][k] = 127;
    b = x;
    for (int i = 0; i < 4800; i += 12) e2.ProcessMono(0, &b[i], 12, ins::control);
    CHECK(memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    Engine e3 = fresh();
    CHECK(!e3.Active(0, ins::control));
    setsc(0, sc::kDuck, 0, {50, 70, 10, 10, 0, 0});
    CHECK(e3.Active(0, ins::control));
    printf("off: a chain with the stage off is bit for bit what it was, and not active\n");
    // engaged and released: the fade ends idle (no work, untouched again)
    std::vector<float> z(2400, 0.4f);
    for (int i = 0; i < 2400; i += kB) e3.ProcessMono(0, &z[i], kB, ins::control);
    ins::control.scmode[0] = sc::kOff;
    for (int i = 0; i < 2400; i += kB) e3.ProcessMono(0, &z[i], kB, ins::control);
    CHECK(!e3.Active(0, ins::control));
  }

  // ---- a missing source does nothing (stems exist only in Overbridge mode)
  {
    Engine e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 20, 20, 64, 50});
    // M1: PUMP keyed by a Rytm track fires from the track's MIDI trigs with no stem ring live (MIDI-only, no Overbridge)
    auto y = pump(e, 6000, {960}, false);
    CHECK(y[959] == 0.5f && std::fabs(y[960]) < 1e-6f);
    // PUMP from AUDIO IN has no trigs: a missing ring is silence, nothing happens
    e = fresh();
    setsc(0, sc::kPump, sc::kAinL, {127, 0, 20, 20, 64, 50});
    y = pump(e, 6000, {960, 2000, 3000}, false);
    for (float v : y) CHECK(v == 0.5f);
    e = fresh();
    setsc(0, sc::kDuck, 0, {0, 127, 0, 0, 0, 0});   // THR 0 dB.. a key never written
    y = pump(e, 6000, {});
    for (float v : y) CHECK(v == 0.5f);
    // the stem stops mid-way: a stale ring is a missing one (its last frames do not keep ducking)
    e = fresh();
    setsc(0, sc::kDuck, 0, {127, 127, 0, 0, 0, 0});   // THR -40 dB
    std::vector<float> loud(kB, 0.9f), s(30000, 0.3f);
    for (int i = 0; i < 30000; i += kB) {
      if (i < 3000) sc::write(0, loud.data(), kB);
      e.ProcessMono(0, &s[i], kB, ins::control);
    }
    CHECK(s[2900] < 0.3f * 0.9f);                                  // ducked while it played
    CHECK(std::fabs(s[29999] - 0.3f) < 1e-5f);                     // and released once it stopped
    printf("a missing source: no effect (PUMP with no stem, DUCK with no key, a stem that stops)\n");
  }

  // ---- the control side
  {
    Engine e = fresh();
    ins::set_sc_mode(3, sc::kPump);
    CHECK(ins::sc_mode(3) == sc::kPump && ins::sc_src(3) == 0 && ins::sc_param(3, 0) == sc::def(sc::kPump, 0));
    CHECK(sc::bus().wanted == 1u && sc::bus().wanted_trig == 1u);   // BD: its stem and its trig
    ins::set_sc_src(3, sc::kAinLR);
    CHECK(sc::bus().wanted == (3u << sc::kStems) && sc::bus().wanted_trig == 0u);   // AUDIO IN L + R, no trig
    ins::set_sc_src(3, sc::kV0 + 4);
    CHECK(sc::bus().wanted == 1u << (sc::kStems + 2 + 4) && sc::bus().wanted_trig == 1u << (sc::kTracks + 4));
    ins::set_sc_param(3, 0, 70);
    ins::set_sc_mode(3, sc::kDuck, true);   // keep: the knobs stay
    CHECK(ins::sc_param(3, 0) == 70 && ins::sc_mode(3) == sc::kDuck);
    ins::set_sc_mode(3, sc::kDuck);          // H1: the mode it is already in: nothing reloaded, the knobs stay
    CHECK(ins::sc_param(3, 0) == 70 && ins::sc_mode(3) == sc::kDuck);
    ins::set_sc_mode(3, sc::kPump);          // a real change loads the defaults
    CHECK(ins::sc_param(3, 0) == sc::def(sc::kPump, 0));
    ins::set_sc_mode(3, sc::kOff);
    CHECK(sc::bus().wanted == 0u && !ins::sc_on(3) && !ins::sc_any_on());
    // the status: why it does nothing
    char why[16];
    ins::set_sc_mode(2, sc::kPump);   // M1: PUMP on a track runs on its MIDI trigs: no stems needed
    CHECK(ins::sc_status(2, why, sizeof why) && !strcmp(why, "OK"));
    ins::set_sc_mode(2, sc::kDuck);
    CHECK(!ins::sc_status(2, why, sizeof why) && !strcmp(why, "NO STEMS"));
    ins::set_sc_src(2, sc::kAinL);
    CHECK(!ins::sc_status(2, why, sizeof why) && !strcmp(why, "NO AUDIO IN"));
    float z[kB] = {};
    sc::write(sc::kStems, z, kB);
    CHECK(ins::sc_status(2, why, sizeof why) && !strcmp(why, "OK"));
    ins::set_sc_src(2, sc::kV0 + 1);
    CHECK(ins::sc_status(2, why, sizeof why));
    // B1 / B2 run inside send FX 1 / 2's return: with that send FX OFF the stage does nothing, and says so
    fx::control.knob[0][0] = fx::kOff;
    ins::set_sc_mode(ins::kBus0, sc::kDuck);
    CHECK(!ins::sc_status(ins::kBus0, why, sizeof why) && !strcmp(why, "SEND FX OFF"));
    fx::control.knob[0][0] = fx::kChorus;
    ins::set_sc_src(ins::kBus0, sc::kV0);
    CHECK(ins::sc_status(ins::kBus0, why, sizeof why));
    ins::set_sc_mode(ins::kBus0, sc::kOff);
    ins::set_sc_mode(2, sc::kOff);
    CHECK(ins::sc_status(2, why, sizeof why) && !strcmp(why, "OFF"));
    // names and texts
    CHECK(sc::src_from_name("cb") == 11 && sc::src_from_name("ain") == sc::kAinLR && sc::src_from_name("v8") == sc::kV0 + 7 &&
          sc::src_from_name("nope") < 0);
    CHECK(sc::mode_from_name("Pump") == sc::kPump && sc::mode_from_name("duck") == sc::kDuck && sc::mode_from_name("x") < 0);
    char t[16];
    ins::sc_value_text(sc::kDuck, 0, 127, t, sizeof t);
    CHECK(!strcmp(t, "-40DB"));
    ins::sc_value_text(sc::kPump, 1, 0, t, sizeof t);
    CHECK(!strcmp(t, "INST"));
    ins::sc_value_text(sc::kDuck, 4, 0, t, sizeof t);
    CHECK(!strcmp(t, "OFF"));
    ins::sc_value_text(sc::kDuck, 5, 0, t, sizeof t);
    CHECK(!strcmp(t, "OFF"));
    ins::sc_value_text(sc::kDuck, 4, 1, t, sizeof t);
    CHECK(!strcmp(t, "20HZ"));
    ins::sc_value_text(sc::kDuck, 4, 127, t, sizeof t);
    CHECK(!strcmp(t, "19.6K"));
    ins::sc_value_text(sc::kDuck, 5, 1, t, sizeof t);
    CHECK(!strcmp(t, "0.5 OCT"));
    ins::sc_value_text(sc::kDuck, 5, 127, t, sizeof t);
    CHECK(!strcmp(t, "9.9 OCT"));
    CHECK(!strcmp(sc::label(sc::kDuck, 4), "KBAS") && !strcmp(sc::label(sc::kDuck, 5), "KWID"));
    // the console and the json
    char o[400];
    CHECK(ins::command("ins=V2:sc:mode:pump", o, sizeof o) && ins::sc_mode(1) == sc::kPump);
    CHECK(ins::command("ins=V2:sc:src:ch", o, sizeof o) && ins::sc_src(1) == 8);
    CHECK(ins::command("ins=V2:sc:p1:77", o, sizeof o) && ins::sc_param(1, 0) == 77);
    CHECK(ins::command("ins=V2:sc:mode:pump", o, sizeof o) && ins::sc_param(1, 0) == 77);   // H1: the same mode again
    CHECK(ins::command("ins=b3:sc:mode:duck", o, sizeof o) && ins::sc_mode(ins::kBus0 + 2) == sc::kDuck);
    CHECK(ins::command("ins=M2:sc:src:ain", o, sizeof o) && ins::sc_src(ins::kM0 + 1) == sc::kAinLR);
    static char js[16384];
    int n = ins::json(js, sizeof js);
    CHECK(n > 0 && strstr(js, "\"sc\":[") && strstr(js, "\"m\":\"PUMP\"") && strstr(js, "\"s\":\"CH\""));
    CHECK(ins::command("ins=V2:sc:off", o, sizeof o) && ins::sc_mode(1) == sc::kOff);
    printf("control: modes, defaults, wanted sources, status, console, json\n");
    ins::reset();
    CHECK(!ins::sc_any_on() && sc::bus().wanted == 0u);
  }

  // ---- the mod targets: kIns params 12..17 (I3:S1 .. S6), an offset on the knob, LFO names by mode
  {
    Engine e = fresh();
    ins::set_sc_mode(2, sc::kPump);
    const mod::Tid t = mod::tid(mod::kIns, 2, 12);
    CHECK(mod::valid(t) && mod::valid(mod::tid(mod::kIns, 12, 17)) && !mod::valid(mod::tid(mod::kIns, 2, 18)));
    char nm[16];
    mod::name(t, nm, sizeof nm);
    CHECK(!strcmp(nm, "I3:S1"));
    CHECK(!strcmp(mod::name(mod::tid(mod::kIns, 12, 17), nm, sizeof nm), "M2:S6"));
    mod::Tid back = 0;
    CHECK(mod::parse("I3:S4", &back) && back == mod::tid(mod::kIns, 2, 15));
    const mod::Range r = mod::range(t);
    CHECK(r.lo == -127 && r.hi == 127);
    const int base = ins::sc_param(2, 0);
    mod::put(t, 20.0f);
    CHECK(ins::sc_param(2, 0) == base && ins::sc_playing(2, 0) == std::min(127, base + 20));
    CHECK(ins::control.scp[2][0] == std::min(127, base + 20));
    mod::put(t, -127.0f);
    CHECK(ins::sc_playing(2, 0) == 0);
    mod::put(t, 0.0f);
    CHECK(mod::knob_now(t) == (float)base);
    char pl[8];
    mod::param_label(t, pl, sizeof pl);
    CHECK(!strcmp(pl, "SDPT") || !strcmp(pl, "SDPTH"));
    CHECK(lfo::params_of(lfo::kTIns + 2) == 18);
    ins::set_sc_mode(2, sc::kDuck);
    mod::param_label(t, pl, sizeof pl);
    CHECK(!strcmp(pl, "STHR"));
    mod::param_label(mod::tid(mod::kIns, 2, 16), pl, sizeof pl);   // S5 / S6 in DUCK are KBAS / KWID
    CHECK(!strcmp(pl, "SKBAS") || !strcmp(pl, "SKBA"));
    mod::param_label(mod::tid(mod::kIns, 2, 17), pl, sizeof pl);
    CHECK(!strcmp(pl, "SKWID") || !strcmp(pl, "SKWI"));
    printf("mod: I3:S1..S6 (kIns 12..17) are targets, offsets on the knob, labels follow the mode\n");
    ins::reset();
  }

  // ---- the Rytm's pages: DOWN on the INSERT page's EQ view (and the MASTER FX page's) opens the SIDECHAIN view
  {
    ins::reset();
    int ki = -1, km = -1;
    for (int i = 0; i < ui::extra_pages(); ++i) {
      if (!strcmp(ui::extra_page(i).title, "INSERT")) ki = i;
      if (!strcmp(ui::extra_page(i).title, "MASTER")) km = i;
    }
    CHECK(ki >= 0 && km >= 0);
    ui::ins_page_set(false, 0, 0, 0);
    ui::Canvas c;
    CHECK(ui::extra_page(ki).key(0x41));          // RIGHT: the EQ view
    CHECK(ui::extra_page(ki).key(0x43));          // DOWN: the SIDECHAIN view
    ui::extra_page(ki).enc(0, 1, false);          // A MODE: OFF -> PUMP
    CHECK(ins::sc_mode(0) == sc::kPump);
    ui::extra_page(ki).enc(1, 3, false);          // B SRC: BD -> a few along
    CHECK(ins::sc_src(0) > 0);
    ui::extra_page(ki).enc(2, 10, false);         // C DPTH
    CHECK(ins::sc_param(0, 0) == sc::def(sc::kPump, 0) + 10);
    ui::extra_page(ki).enc(7, 5, true);           // H THR, FUNC: by 8
    CHECK(ins::sc_param(0, 5) == sc::def(sc::kPump, 5) + 40);
    CHECK(ui::remote_render(6 + ki, 0, nullptr, c));   // renders; the text-fit check (check.h) covers the strings
    CHECK(ui::extra_page(ki).key(0x44));          // YES: bus 1
    ui::extra_page(ki).enc(0, 2, false);          // DUCK on B1
    CHECK(ins::sc_mode(ins::kBus0) == sc::kDuck);
    CHECK(ui::remote_render(6 + ki, 0, nullptr, c));
    CHECK(ui::extra_page(ki).key(0x40));          // LEFT: the EQ view again
    CHECK(ui::extra_page(ki).key(0x45));          // NO: the track view
    ui::mfx_page_set(0, true);
    CHECK(ui::extra_page(km).key(0x43));
    ui::extra_page(km).enc(0, 1, false);
    CHECK(ins::sc_mode(ins::kM0) == sc::kPump);
    CHECK(ui::extra_page(km).key(0x44));          // YES: M2
    ui::extra_page(km).enc(0, 2, false);
    CHECK(ins::sc_mode(ins::kM0 + 1) == sc::kDuck);
    CHECK(ui::remote_render(6 + km, 0, nullptr, c));
    CHECK(ui::extra_page(km).key(0x45));
    ins::reset();
    printf("pages: the SIDECHAIN view on the INSERT and MASTER pages (DOWN on the EQ view)\n");
  }

  // ---- M2: a mode or source change is a gain ramp, not a step; a source change fires no spurious trig
  {
    // PUMP in its hold at gmin = 0 (0.5 -> 0), then DUCK with no key (gain 1): the gain comes back over a few ms, no step
    Engine e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 127, 20, 64, 50});
    std::vector<float> y(4800, 0.5f);
    float z[kB] = {};
    for (int i = 0; i < 4800; i += kB) {
      sc::write(0, z, kB);
      if (i == 480) sc::trig_track(0);
      if (i == 2400) ins::control.scmode[0] = sc::kDuck, ins::control.scp[0][0] = 0;
      e.ProcessMono(0, &y[i], kB, ins::control);
    }
    CHECK(std::fabs(y[2399]) < 1e-6f);   // ducked by the PUMP when the mode changed
    float step = 0.0f;
    for (int i = 2400; i < 2700; ++i) step = std::max(step, std::fabs(y[i] - y[i - 1]));
    printf("PUMP -> DUCK: the largest step %.4f (a 0.5 jump without the ramp)\n", step);
    CHECK(step < 0.02f);
    CHECK(std::fabs(y[4799] - 0.5f) < 1e-5f);
    // a source change: the new track's counter was already ahead; no trig fires
    e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 127, 20, 64, 50});
    for (int i = 0; i < 9; ++i) sc::trig_track(1);
    std::vector<float> w(4800, 0.5f);
    for (int i = 0; i < 4800; i += kB) {
      sc::write(0, z, kB);
      if (i == 960) ins::control.scsrc[0] = 1;
      e.ProcessMono(0, &w[i], kB, ins::control);
    }
    for (float v : w) CHECK(v == 0.5f);
    printf("source change: no spurious trig from the new source's counter\n");
  }

  // ---- H1 on the Rytm view / L4: a mode knob nudge that does not change the mode keeps the knobs; DUCK's H is KWID
  {
    ins::reset();
    int ki = -1;
    for (int i = 0; i < ui::extra_pages(); ++i)
      if (!strcmp(ui::extra_page(i).title, "INSERT")) ki = i;
    ui::ins_page_set(false, 0, 0, 0);
    CHECK(ui::extra_page(ki).key(0x41) && ui::extra_page(ki).key(0x43));
    ui::extra_page(ki).enc(0, 2, false);          // DUCK
    CHECK(ins::sc_mode(0) == sc::kDuck);
    ui::extra_page(ki).enc(2, 9, false);          // C THR
    const int thr = ins::sc_param(0, 0);
    ui::extra_page(ki).enc(0, 1, false);          // A: at the end of the list: still DUCK
    ui::extra_page(ki).enc(0, 0, false);
    CHECK(ins::sc_mode(0) == sc::kDuck && ins::sc_param(0, 0) == thr);
    const int h = ins::sc_param(0, 5);
    ui::extra_page(ki).enc(7, 5, false);          // H: DUCK's sixth knob is KWID now
    CHECK(ins::sc_param(0, 5) != h);
    ins::reset();
  }

  // ---- M3: the /status "ins" json at its worst: all 13 chains, every slot, every field as long as it gets
  {
    ins::reset();
    for (int c = 0; c < ins::kChains; ++c) {
      for (int s = 0; s < ins::kSlots; ++s)   // the longest a chain takes (a send TYPE needs room: the first that is taken)
        for (int t = ins::kAllTypes - 1; t > 0 && ins::control.type[c][s] != t; --t) ins::set_type(c, s, t);
      ins::set_sc_mode(c, sc::kDuck);
      ins::set_sc_src(c, sc::kAinLR);
      for (int k = 0; k < sc::kParams; ++k) ins::set_sc_param(c, k, 127);
      for (int k = 0; k < ins::kEqParams; ++k) ins::set_eq(c, k, 127);
      for (int k = 0; k < sc::kParams; ++k) ins::control.scp[c][k] = 127;
    }
    static char big[65536], small[ins::kInsJsonCap];
    const int n = ins::json(big, sizeof big);
    printf("ins json worst case: %d bytes of %d (the /status buffer; the stream budget is added after it)\n", n, ins::kInsJsonCap);
    CHECK(n > 0 && n < (int)sizeof big);
    int nduck = 0, nslot = 0, noff = 0;
    for (const char* q = big; (q = strstr(q, "\"m\":\"DUCK\"")); ++q) ++nduck;
    for (const char* q = big; (q = strstr(q, "{\"t\":\"")); ++q) ++nslot;
    for (const char* q = big; (q = strstr(q, "{\"t\":\"OFF\"")); ++q) ++noff;
    printf("worst case: %d SC stages on, %d slots, %d of them OFF\n", nduck, nslot, noff);
    CHECK(nduck == ins::kChains && noff == 0);
    CHECK(n + 700 <= ins::kInsJsonCap);   // room for bridge's "stream":{...}
    const int m = ins::json(small, sizeof small);
    CHECK(m == n && !strcmp(small, big));
    // valid: balanced braces / brackets outside strings
    int depth = 0;
    bool str = false;
    for (int i = 0; i < n; ++i) {
      char ch = big[i];
      if (ch == '"') str = !str;
      else if (!str && (ch == '{' || ch == '[')) ++depth;
      else if (!str && (ch == '}' || ch == ']')) --depth;
      CHECK(depth >= 0);
    }
    CHECK(depth == 0 && !str);
    ins::reset();
  }

  // ---- L1: a ring written once and then stale does not count as live on the first read after an engage
  {
    Engine e = fresh();
    setsc(0, sc::kDuck, 0, {127, 127, 0, 0, 0, 0});
    std::vector<float> loud(kB, 0.9f), y(kB, 0.3f);
    sc::write(0, loud.data(), kB);   // an old write; nothing moves it any more
    e.ProcessMono(0, y.data(), kB, ins::control);
    for (float v : y) CHECK(v == 0.3f);
  }

  // ---- L2: a call longer than one chunk reads each chunk's own key frames
  {
    Engine e = fresh();
    setsc(0, sc::kDuck, 0, {127, 127, 0, 0, 0, 0});
    std::vector<float> key(48, 0.0f), y(48, 0.3f), w2(kB, 0.3f);
    sc::write(0, key.data(), kB);   // the ring is moving
    e.ProcessMono(0, w2.data(), kB, ins::control);
    for (int i = 0; i < kB; ++i) key[i] = 0.9f;   // loud in the first chunk of the call only
    sc::write(0, key.data(), 48);
    e.ProcessMono(0, y.data(), 48, ins::control);
    CHECK(y[10] < 0.3f);
  }

  // ---- the cost: a stereo bus chain, 16-frame chunks (what the bus and master chains see), and a mono voice chain
  {
    auto cost = [](int mode, bool stereo, bool keyfilter = true) {
      Engine e = fresh();
      setsc(8, mode, 0, mode == sc::kPump ? std::vector<int>{100, 5, 30, 60, 40, 50}
                                          : std::vector<int>{60, 80, 30, 50, keyfilter ? 60 : 0, keyfilter ? 60 : 0});
      std::vector<float> key(kB), l(kB), r(kB);
      const int N = 60000;
      uint64_t ns = 0;
      for (int it = 0; it < N; ++it) {
        for (int j = 0; j < kB; ++j) key[j] = 0.5f * std::sin(0.05f * (float)(it * kB + j)), l[j] = r[j] = 0.3f * key[j];
        sc::write(0, key.data(), kB);
        if (mode == sc::kPump && it % 4000 == 0) sc::trig_track(0);
        float y[kB][2];
        for (int j = 0; j < kB; ++j) y[j][0] = l[j], y[j][1] = r[j];
        auto t0 = std::chrono::steady_clock::now();
        if (stereo) e.ProcessStereo(8, y, kB, ins::control);
        else e.ProcessMono(8, l.data(), kB, ins::control);
        ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
      }
      return (double)ns / N;
    };
    for (int m = sc::kPump; m <= sc::kDuck; ++m)
      for (int st = 0; st < 2; ++st) {
        double ns = cost(m, st);
        printf("CPU: %s %s%s, %.0f ns per 16-frame chunk = %.3f %% of a core at 48 kHz (this Mac, -O2; the Pi is ~3-5x slower)\n",
               sc::mode_name(m), st ? "stereo" : "mono", m == sc::kDuck ? " (KBAS + KWID on)" : "", ns, ns * 3000.0 / 1e9 * 100.0);
      }
    for (int st = 0; st < 2; ++st) {
      double ns = cost(sc::kDuck, st, false);
      printf("CPU: DUCK %s (KBAS, KWID OFF), %.0f ns per 16-frame chunk = %.3f %% of a core at 48 kHz\n", st ? "stereo" : "mono", ns,
             ns * 3000.0 / 1e9 * 100.0);
    }
  }
  printf("test_sidechain: ALL OK\n");
  return 0;
}
