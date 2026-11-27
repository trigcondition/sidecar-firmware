// core/tests/test_morph_sweep.cpp - MORPH check (diagnosis round, fix/morph-ux): "a morph is a smooth interpolation over the
// whole fader travel, and sweeping back returns the values". For every kind of target, from every fader source (page /
// console, MIDI learn 7-bit and 14-bit, the QPERF knob's report), the fader goes 0 -> 100% -> 0 in 64 steps with the
// control loop running between steps; after each step the target's played value is compared with the morph's own table
//   a + (b - a) x f   (f = the played fader, read back)         stepped targets: A below halfway, B above, one switch
// and with the direction (monotone), the end values (B at the top, A back at 0) and the largest step between two samples
// (a continuous target must not jump at 50%).
//
// Soft checks: every case prints one line (ok / FAIL with the numbers), the test exits 1 when any failed. A case with a
// known failure is listed under KNOWN_FAILING below: it is reported, and does not fail the default run (make test); run
//   KNOWN=1 core/build/test_morph_sweep      to make the known-failing cases fail the run too (they are the findings).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "hooks.h"
#include "machines.h"
#include "mod/learn.h"
#include "mod/mod.h"
#include "mod/rytm_param.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "ui/page.h"
#include "ui/remote.h"
#if __has_include("loop/loop.h")   // the looper (feature/looper-050): LOOP:BAL is checked at the looper itself
#include "loop/loop.h"
#define HAVE_LOOPER 1
#endif

using namespace core;
using mod::Tid;

static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void run_ms(int ms) {
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), mod::tick(now_us());
}
static Tid T(const char* n) {
  Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
static void reset_all(float base) {
  mod::reset();
  scene::reset();
  learn::reset();
  scene::set_qp_mode(scene::kQpAlways);   // (the sweeps drive the knob as the fader: v2 Task 6's ALWAYS)
  v[0].p[0] = base;
  at(g_us + 1000), mod::tick(now_us());
}

// ---- the fader's sources
enum Src { kSrcConsole, kSrcLearn7, kSrcLearn14, kSrcQperf, kSrcN };
static const char* const kSrcName[kSrcN] = {"console", "learn7", "learn14", "qperf"};
static void qp(int pos, int mask, int flags) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x0E, (uint8_t)(pos >> 7), (uint8_t)(pos & 0x7F),
                       (uint8_t)(mask & 0x7F), (uint8_t)((mask >> 7) & 0x1F), (uint8_t)flags, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static int g_qmask = 0;
static void src_arm(Src s) {
  if (s == kSrcLearn7 || s == kSrcLearn14) {
    CHECK(learn::arm("fader"));
    learn::midi(1, 0xB0, 20, 0);   // the route (CC 20 on port 1 ch 1), value 0
    if (s == kSrcLearn14) learn::midi(1, 0xB0, 52, 0);   // its LSB (CC 52) heard: 14 bits
  } else if (s == kSrcQperf) {
    qp(0, g_qmask, 2);   // the first report since the link: a baseline
  }
}
static void src_init(Src s) {
  src_arm(s);
  scene::set_fader(0, scene::kConsole);
  run_ms(40);
}
// move the source to p (0..1); the control loop then runs 40 ms (the QPERF smoothing follows the interval)
static void src_move(Src s, double p) {
  switch (s) {
    case kSrcConsole: scene::set_fader((uint16_t)lround(p * 16383), scene::kConsole); break;
    case kSrcLearn7: learn::midi(1, 0xB0, 20, (uint8_t)lround(p * 127)); break;
    case kSrcLearn14: {
      int x = (int)lround(p * 16383);
      learn::midi(1, 0xB0, 20, (uint8_t)(x >> 7)), learn::midi(1, 0xB0, 52, (uint8_t)(x & 127));
      break;
    }
    case kSrcQperf: qp((int)lround(p * 255), g_qmask, 2); break;
    default: break;
  }
  run_ms(40);
}

// ---- what a target plays
static double obs(Tid t) {
  mod::Kind k = mod::kind_of(t);
  int i = mod::index_of(t), p = mod::param_of(t);
  const mod::Entry* e = mod::find(t);
  if (k == mod::kV) return v[i].p[p];
  if (k == mod::kFx) return fx::control.knob[i & 1][2 + (p & 3)];
  if (k == mod::kSnd) return fx::control.send[i][p & 1];
#if HAVE_LOOPER
  // what the looper plays (its BAL / LEV with the layer's value in), not the layer's e->applied: a looper that ignored
  // the scene's value would still show the morph there
  if (k == mod::kLoop) return p ? loop::lev_eff() : loop::bal_eff();
#endif
  if (!e) return NAN;
  if (mod::is_offset(k)) return mod::knob_now(t) + e->applied;
  return e->applied;
}
static int g_parts_seen = 0;   // P3: samples read from a 09 part (the sweeps below must run on that path)
static double obs_remote(Tid t) {
  int tr, w;
  if (!mod::rytm_word(t, &tr, &w)) return NAN;
  mod::RpView r;
  if (!mod::rp_view(tr, w, &r) || !r.morph) return NAN;
  if (r.pfl == 3) {   // P3: a 09 part - the Rytm's own arithmetic (pm_fake.h / rytm-mods "pm morph") on the 0A fader
    double f = (double)scene::fader_played() / 16383.0;
    ++g_parts_seen;
    return (r.pa + (r.pb - r.pa) * f) / 128.0;
  }
  return r.pfl ? NAN : r.ovr / 128.0;   // (a one-sided part: not a value alone)
}
static double observe(Tid t) { return mod::is_remote(mod::kind_of(t)) ? obs_remote(t) : obs(t); }

// ---- the harness
static int g_fail = 0, g_known = 0, g_run = 0;
static bool g_known_run = false;   // KNOWN=1: the known-failing cases fail the run
static std::vector<std::string> g_known_tags;
static bool is_known(const std::string& tag) {
  for (const std::string& k : g_known_tags)
    if (tag.compare(0, k.size(), k) == 0) return true;
  return false;
}
static void verdict(const std::string& tag, bool ok, const char* detail) {
  ++g_run;
  if (ok) {
    printf("  ok    %s%s\n", tag.c_str(), detail);
    return;
  }
  bool kn = is_known(tag);
  printf("  FAIL%s %s%s\n", kn ? " (known)" : "      ", tag.c_str(), detail);
  if (kn && !g_known_run) ++g_known;
  else ++g_fail;
}

struct Case {
  const char* name;
  const char* a_b;        // "A:B" values as numbers (A-side lock, B-side lock; "-" none) e.g. "30:90"
  bool stepped;           // expects one switch per direction (a step kind / list knob)
  double tol;             // the interpolation's tolerance in the target's unit
  double base;            // one-sided: the value the target plays unlocked (NAN: not needed)
};

// sweep 0 -> 1 -> 0 in n steps on source s for target t locked (a, b); a < 0: no A lock, b < 0: no B lock (values in
// the target's unit, x 128 in the lock)
static void sweep(const std::string& tag, Tid t, double a, double b, bool stepped, double tol, double base, Src s,
                  int n = 64) {
  const bool has_a = a >= 0, has_b = b >= 0;
  CHECK(scene::lock(0, t, (uint16_t)(has_a ? a * 128 : 0)) || !has_a);
  if (!has_a) scene::unlock(0, t);
  CHECK(scene::lock(1, t, (uint16_t)(has_b ? b * 128 : 0)) || !has_b);
  if (!has_b) scene::unlock(1, t);
  // a scene that has no lock on t still needs A / B to exist: lock a dummy there
  if (!has_a) scene::lock(0, T("FX2:P5"), 5 * 128);
  if (!has_b) scene::lock(1, T("FX2:P5"), 5 * 128);
  scene::set_ab(0, 1);
  src_init(s);
  double A = has_a ? a : base, B = has_b ? b : base;
  std::vector<double> val(2 * n + 1), fp(2 * n + 1);
  for (int i = 0; i <= 2 * n; ++i) {
    double p = i <= n ? (double)i / n : (double)(2 * n - i) / n;
    src_move(s, p);
    val[i] = observe(t), fp[i] = scene::fader_played() / 16383.0;
  }
  char d[200];
  bool ok = true;
  const char* why = "";
  double worst = 0, maxjump = 0, mono_bad = 0;
  int sw_up = 0, sw_dn = 0;
  double sw_up_at = -1, sw_dn_at = -1;
  const double dir = B >= A ? 1 : -1;
  for (int i = 0; i <= 2 * n && ok; ++i) {
    if (std::isnan(val[i])) { ok = false, why = " target not played (no entry / value)"; break; }
    if (!stepped) {
      double want = A + (B - A) * fp[i];
      double dev = std::fabs(val[i] - want);
      worst = dev > worst ? dev : worst;
      if (dev > tol) ok = false, why = " value off the interpolation";
    } else {
      double f = fp[i];
      if (f < 0.48 && std::fabs(val[i] - A) > tol) ok = false, why = " not A below 0.48";
      if (f > 0.52 && std::fabs(val[i] - B) > tol) ok = false, why = " not B above 0.52";
      if (std::fabs(val[i] - A) > tol && std::fabs(val[i] - B) > tol) ok = false, why = " neither A nor B";
    }
    if (i > 0) {
      double j = std::fabs(val[i] - val[i - 1]);
      maxjump = j > maxjump ? j : maxjump;
      bool up = i <= n;
      double mv = (val[i] - val[i - 1]) * dir * (up ? 1 : -1);   // toward B going up, toward A coming down
      if (mv < -tol) mono_bad = mv < mono_bad ? mv : mono_bad;
      if (stepped && std::fabs(val[i] - val[i - 1]) > tol) {
        if (up) ++sw_up, sw_up_at = fp[i];
        else ++sw_dn, sw_dn_at = fp[i];
      }
    }
  }
  if (ok && std::fabs(val[0] - A) > tol) ok = false, why = " not A at the start";
  if (ok && std::fabs(val[n] - B) > tol) ok = false, why = " not B at the top";
  if (ok && std::fabs(val[2 * n] - A) > tol) ok = false, why = " DID NOT RETURN to A at 0";
  if (ok && mono_bad < -tol) ok = false, why = " not monotone";
  if (ok && stepped && (sw_up != 1 || sw_dn != 1)) ok = false, why = " switches != 1 per direction";
  if (ok && !stepped && maxjump > std::fabs(B - A) * 3.0 / n + tol * 2) ok = false, why = " jump between two steps";
  if (stepped)
    snprintf(d, sizeof d, "  [A %.1f B %.1f: up switch at f %.3f, down at f %.3f%s]", A, B, sw_up_at, sw_dn_at, why);
  else
    snprintf(d, sizeof d, "  [A %.1f B %.1f: worst dev %.3f, max step %.2f of %.2f%s]", A, B, worst, maxjump,
             std::fabs(B - A) / n, why);
  verdict(tag, ok, d);
  // leave nothing behind
  scene::clear(0), scene::clear(1);
  run_ms(5);
}

static void for_sources(const char* name, const char* tgt, double a, double b, bool stepped, double tol, double base,
                        void (*prep)(void) = nullptr) {
  for (int s = 0; s < kSrcN; ++s) {
    reset_all(40);
    if (prep) prep();
    std::string tag = std::string(name) + "/" + kSrcName[s] + (a < 0 ? "/Bonly" : b < 0 ? "/Aonly" : "");
    sweep(tag, T(tgt), a, b, stepped, tol, base, (Src)s);
  }
}

static int list_knob() {
  for (int i = 0; i < 16; ++i)
    if (ui::remote_is_list(0, i)) return i;
  return -1;
}
static void plaits_report() {   // V1 plays PLAITS: its knobs and which are lists
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kPlaits | 0)};
  for (int k = 0; k < 16; ++k) m.push_back(10), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}

static void prep_v1() { plaits_report(), v[0].p[2] = 40; }
static void prep_ins() { ins::set_param(0, 0, 0, 40); }
static void prep_mfx() { master::set_lev(100); }

static void kinds_two_sided() {
  printf("-- every kind, A and B locked, 64 steps up and 64 down\n");
  for_sources("kV V1:P3", "V1:P3", 30, 100, false, 0.05, NAN, prep_v1);
  for_sources("kV V1:P3 (A>B)", "V1:P3", 100, 30, false, 0.05, NAN, prep_v1);
  for_sources("kFx FX1:P3", "FX1:P3", 20, 110, false, 0.8, NAN);
  for_sources("kSnd S1:F1", "S1:F1", 10, 120, false, 0.8, NAN);
  for_sources("kIns I1:A1", "I1:A1", 30, 100, false, 1.2, NAN, prep_ins);
  for_sources("kIsnd X1:B1", "X1:B1", 20, 100, false, 1.2, NAN);
  for_sources("kMfx MFX:LEV", "MFX:LEV", 60, 120, false, 1.2, NAN, prep_mfx);
  for_sources("kOut O1:LEV", "O1:LEV", 40, 110, false, 1.2, NAN);
  for_sources("kSeq T1:SWG (cont)", "T1:SWG", 52, 78, false, 0.8, NAN);
#if HAVE_LOOPER
  for_sources("kLoop LOOP:BAL", "LOOP:BAL", 10, 100, false, 0.8, NAN);
#else
  printf("kLoop LOOP:BAL: skipped (no looper in this build: core/loop)\n");
#endif
  for_sources("kRa RT1:FRQ (Rytm own)", "RT1:FRQ", 100, 20, false, 0.2, NAN);
  for_sources("kRs RT1:P1 (Rytm own)", "RT1:P1", 10, 90, false, 0.2, NAN);
  for_sources("kRfx RFX:TIM", "RFX:TIM", 10, 90, false, 0.2, NAN);
  // P3: a mod with the Rytm-side morph (caps2 kCaps2Morph): the same sweeps as 09 parts on the 0A fader, the values the
  // 05 path's within 1/128 (the tolerance above); a hello without it after
  const uint8_t h8[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, mod::kCapsParamMod, (uint8_t)ui::kCaps2Morph, 0xF7};
  engine_sysex(h8, (int)sizeof h8);
  CHECK(g_parts_seen == 0);
  for_sources("kRa RT1:FRQ (09/0A)", "RT1:FRQ", 100, 20, false, 0.2, NAN);
  for_sources("kRs RT1:P1 (09/0A)", "RT1:P1", 10, 90, false, 0.2, NAN);
  for_sources("kRfx RFX:TIM (09/0A)", "RFX:TIM", 10, 90, false, 0.2, NAN);
  CHECK(g_parts_seen > 0);   // (no kit from the Rytm: nothing goes; the parts are what the sender holds)
  const uint8_t h0[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, 0, 0xF7};
  engine_sysex(h0, (int)sizeof h0);
}
static void kinds_stepped() {
  printf("-- the stepped kinds: one switch per direction, at halfway (by design: scene.h)\n");
  static int lk;
  lk = -1;
  for (int s = 0; s < kSrcN; ++s) {   // a list knob of V1
    reset_all(40);
    plaits_report();
    int k = list_knob();
    CHECK(k >= 0);
    char nm[16];
    snprintf(nm, sizeof nm, "V1:P%d", k + 1);
    sweep(std::string("kV list knob ") + nm + "/" + kSrcName[s], T(nm), 3, 7, true, 0.01, NAN, (Src)s);
  }
  for_sources("kSeq T1:SPD (step)", "T1:SPD", 1, 6, true, 0.01, NAN);
  for_sources("kSeq T1:DIR (step)", "T1:DIR", 0, 4, true, 0.01, NAN);
  for_sources("kArp R1:MODE (step)", "R1:MODE", 0, 5, true, 0.01, NAN);
  for_sources("kRa RT1:FTYP (Rytm list word)", "RT1:FTYP", 1, 5, true, 0.01, NAN);
  for_sources("kRs RT1:WAV (Rytm list word)", "RT1:WAV", 2, 6, true, 0.01, NAN);
}
static void kinds_one_sided() {
  printf("-- one side locked: toward the target's base on the other half\n");
  for_sources("kV V1:P3", "V1:P3", 30, -1, false, 0.05, 40, prep_v1);
  for_sources("kV V1:P3", "V1:P3", -1, 100, false, 0.05, 40, prep_v1);
  for_sources("kFx FX1:P3", "FX1:P3", 20, -1, false, 0.8, 0);
  for_sources("kMfx MFX:LEV", "MFX:LEV", 60, -1, false, 1.2, 100, prep_mfx);
}


// ---- the things that could break a sweep half-way
static void report03_p3(float p3) {   // the Rytm's 0x03 for V1 (PLAITS): P3 = p3, the rest 10
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kPlaits | 0)};
  for (int k = 0; k < 16; ++k) {
    float x = k == 2 ? p3 : 10;
    int iv = (int)x;
    m.push_back((uint8_t)iv), m.push_back((uint8_t)((x - iv) * 128));
  }
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void check_line(const std::string& tag, bool ok, const char* fmt_s, double x, double y) {
  char d[160];
  snprintf(d, sizeof d, fmt_s, x, y);
  verdict(tag, ok, d);
}
static void midway_events() {
  printf("-- events in the middle of a sweep\n");
  const Tid t = T("V1:P3");
  // 1. the Rytm reports the parameter back (0x03, each step a different value, once also the played value itself)
  for (int s = 0; s < kSrcN; ++s) {
    reset_all(40);
    plaits_report(), v[0].p[2] = 40;
    scene::lock(0, t, 30 * 128), scene::lock(1, t, 100 * 128), scene::set_ab(0, 1);
    src_init((Src)s);
    double worst = 0, back = 0;
    for (int i = 0; i <= 128; ++i) {
      double p = i <= 64 ? i / 64.0 : (128 - i) / 64.0;
      src_move((Src)s, p);
      report03_p3(i % 7 * 9 + (i == 50 ? v[0].p[2] : 0));   // (at i 50 the Rytm echoes the played value + a bit)
      run_ms(3);
      double want = 30 + 70 * scene::fader_played() / 16383.0;
      double dev = std::fabs(v[0].p[2] - want);
      worst = dev > worst ? dev : worst;
    }
    back = std::fabs(v[0].p[2] - 30);
    check_line(std::string("a 0x03 echo every step / ") + kSrcName[s], worst < 0.1 && back < 0.1,
               "  [worst dev %.3f, at 0: off A by %.3f]", worst, back);
    scene::clear(0), scene::clear(1), run_ms(5);
  }
  // 2. A / B changed in the middle: the new pair takes over at once, and 0 is the new A
  for (int s = 0; s < kSrcN; ++s) {
    reset_all(40);
    plaits_report(), v[0].p[2] = 40;
    scene::lock(0, t, 30 * 128), scene::lock(1, t, 100 * 128), scene::lock(2, t, 60 * 128);
    scene::lock(0, T("FX1:P3"), 20 * 128), scene::lock(1, T("FX1:P3"), 110 * 128);   // (FX1:P3 in A and B only)
    scene::set_ab(0, 1);
    src_init((Src)s);
    for (int i = 0; i <= 45; ++i) src_move((Src)s, i / 64.0);   // 0.7
    scene::set_ab(0, 2);
    run_ms(5);
    double f = scene::fader_played() / 16383.0, e1 = 30 + 30 * f, e_fx = 20 + 0 * f;   // FX1:P3 only in A now: toward base
    bool ok1 = std::fabs(v[0].p[2] - e1) < 0.1;
    (void)e_fx;
    for (int i = 45; i <= 64; ++i) src_move((Src)s, i / 64.0);
    bool ok2 = std::fabs(v[0].p[2] - 60) < 0.1;
    for (int i = 64; i >= 0; --i) src_move((Src)s, i / 64.0);
    bool ok3 = std::fabs(v[0].p[2] - 30) < 0.1;
    scene::set_ab(0, 1);
    src_move((Src)s, 1.0);
    bool ok4 = std::fabs(v[0].p[2] - 100) < 0.1;
    char d[120];
    snprintf(d, sizeof d, "  [at 0.7 after the change %.2f (want %.2f), top %.2f, back %.2f, B again %.2f]", v[0].p[2], e1, 0.0, v[0].p[2], 0.0);
    verdict(std::string("A / B change at 0.7 / ") + kSrcName[s], ok1 && ok2 && ok3 && ok4, ok1 && ok2 && ok3 && ok4 ? "" : d);
    scene::clear(0), scene::clear(1), scene::clear(2), run_ms(5);
  }
  // 3. the fader's source changes in the middle (every order of the sources): the end is always A at 0
  {
    bool ok = true;
    char why[200] = "";
    for (int a = 0; a < kSrcN && ok; ++a)
      for (int b = 0; b < kSrcN && ok; ++b) {
        if (a == b || (a == kSrcLearn7 && b == kSrcLearn14) || (a == kSrcLearn14 && b == kSrcLearn7)) continue;   // (one controller: one route)
        reset_all(40);
        plaits_report(), v[0].p[2] = 40;
        scene::lock(0, t, 30 * 128), scene::lock(1, t, 100 * 128), scene::set_ab(0, 1);
        src_init((Src)a);
        src_arm((Src)b);   // (b's route / baseline: both sources are live from the start)
        for (int i = 0; i <= 40; ++i) src_move((Src)a, i / 64.0);   // a to 0.62
        for (int i = 0; i <= 64; ++i) src_move((Src)b, i / 64.0);   // b takes over, up to the top
        double top = v[0].p[2];
        for (int i = 64; i >= 0; --i) src_move((Src)b, i / 64.0);
        double bot = v[0].p[2];
        if (std::fabs(top - 100) > 0.1 || std::fabs(bot - 30) > 0.1) {
          ok = false;
          snprintf(why, sizeof why, "  [%s then %s: top %.2f (100), back %.2f (30)]", kSrcName[a], kSrcName[b], top, bot);
        }
        scene::clear(0), scene::clear(1), run_ms(5);
      }
    verdict("source change in the middle (every pair)", ok, why);
  }
}
static void qperf_specifics() {
  printf("-- the QPERF knob\n");
  // a slow turn from rest, one position per report: the take-over (2 positions, the jitter guard) glides over 200 ms
  // (v2 Task 6), so no 1 ms step of the played fader is above 1/32 of the travel
  reset_all(40);
  scene::lock(0, T("FX1:P3"), 0), scene::lock(1, T("FX1:P3"), 127 * 128), scene::set_ab(0, 1);
  scene::set_fader(4000, scene::kConsole), run_ms(40);   // another source has it
  qp(0, 0, 2), run_ms(40);
  int first_move = -1, last = scene::fader_played(), max_step = 0;
  for (int pos = 1; pos <= 20; ++pos) {
    qp(pos, 0, 2);
    for (int ms = 0; ms < 30; ++ms) {
      run_ms(1);
      int p = scene::fader_played(), d = p > last ? p - last : last - p;
      max_step = d > max_step ? d : max_step, last = p;
    }
    if (first_move < 0 && scene::fader_source() == scene::kQperf) first_move = pos;
  }
  check_line("a slow turn from rest: no 1 ms step above 1/32 of the travel at the take-over",
             first_move == 2 && max_step <= 16383 / 32, "  [the knob took at position %.0f; largest 1 ms step %.0f]",
             first_move, max_step);
  // flags bit 2 (macros muted) and FUNC (bit 0): ALWAYS takes the position all the same
  reset_all(40);
  qp(0, 0, 2), qp(100, 0, 2), run_ms(40);
  double f1 = scene::state().fader;
  qp(180, 0, 2 | 1), run_ms(40);
  check_line("ALWAYS: FUNC held still moves the fader", f1 == 100 * 16383 / 255 &&
             scene::state().fader == 180 * 16383 / 255, "  [fader %.0f then %.0f]", f1, scene::state().fader);
  // MUTED ONLY with the new mod (caps2 kCaps2QpMute): the macros playing, no move; muted, the knob is the fader
  reset_all(40);
  scene::set_qp_mode(scene::kQpMutedOnly);
  const uint8_t hello[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, (uint8_t)ui::kCaps2QpMute, 0xF7};
  engine_sysex(hello, (int)sizeof hello);
  qp(0, 0, 2), qp(100, 0, 2), qp(140, 0, 2), run_ms(40);
  double u = scene::state().fader;
  qp(140, 0, 2 | 4), qp(180, 0, 2 | 4), run_ms(40);   // the mute edge a baseline, then the knob
  check_line("MUTED ONLY: the knob moves the fader only with the macros muted", u == 0 &&
             scene::state().fader == 180 * 16383 / 255, "  [unmuted %.0f, muted %.0f]", u, scene::state().fader);
  const uint8_t hello0[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, 0, 0xF7};
  engine_sysex(hello0, (int)sizeof hello0);
}
// 0.5.0 v2 (S7): the Rytm's own scene and PERF macros are no scene targets: nothing to sweep, the lock is refused
static void rytm_refused() {
  printf("-- kRytm (RYTM:SCN, RYTM:PERFn): no scene target\n");
  reset_all(40);
  bool ok = !scene::lock(0, T("RYTM:PERF3"), 10 * 128) && !scene::lock(1, T("RYTM:SCN"), 2 * 128) &&
            !scene::state().s[0].n && !scene::state().s[1].n;
  verdict("kRytm RYTM:PERF3 / RYTM:SCN refused as scene locks", ok, "");
}

int main(int argc, char** argv) {
  (void)argc, (void)argv;
  g_known_run = getenv("KNOWN") != nullptr;
  // the known failures (findings of the diagnosis round); see notes/reports/morph-check-report.md
  g_known_tags.clear();
  // (case tag prefixes, "<target>/<source>[/Aonly|/Bonly]")
  char tmpl[] = "/tmp/core_morphsweep_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'M', 'S', 'W', 0xF7};
  engine_sysex(proj, sizeof proj);
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  kinds_two_sided();
  kinds_stepped();
  kinds_one_sided();
  midway_events();
  qperf_specifics();
  rytm_refused();
  printf("test_morph_sweep: %d cases, %d failing, %d known-failing (KNOWN=1 counts those too)\n", g_run, g_fail, g_known);
  if (g_fail) return 1;
  printf("test_morph_sweep: PASS\n");
  return 0;
}
