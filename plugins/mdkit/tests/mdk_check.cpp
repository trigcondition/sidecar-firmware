// plugins/mdkit/tests/mdk_check.cpp - the host checks of the three mdkit machines (docs/superpowers/specs/
// 2026-10-26-mdkit-machines-design.md "Checks"): the RMPA packages (make packages) loaded and run on this arm64 Mac as
// the Pi runs them (rmp_loader: verify, the two parts placed, relocated; MAP_JIT), against mdkit's own voices built
// natively with mdkit's own flags (cc -std=c99 -O2, unwindowed):
//   load      each package verifies, places and init()s; its state size and TYPE count
//   render    every TYPE, triggered, sounds (peak > 1e-3, finite) - at 48 kHz, through the resampler
//   exact     the core-rate samples before the resampler (the state's tap) equal mdkit's render bit for bit: the
//             knobs at their defaults, at 0 and at 127, a control moved mid-note (at the tap's sample count)
//   controls  P1..P8 reach set_control 1:1 (each control's 0 and 127 bit-exact against mdkit's set_control(k, v));
//             the Ps past a voice's control count are inert (bit-identical output)
//   resample  48 kHz output against a 512-tap reference of the same design: SNR over the first second
//   type      a TYPE change mid-note: the output fades to 0 within 2 ms, no step larger than the signal's own, the
//             next trig plays the new voice bit-exact; nothing written outside the state (guard bytes); back during
//             the fade: the old voice plays on
//   cpu       ns per 12-frame render call (mean, worst) per TYPE and the set-up of a TYPE change, on this Mac
//   engine    MD (mdk.rmp, the three in one: P9 ENGINE, P10 TYPE, P11 LEV) runs every check above over its 30 voices,
//             and: P10's raw value read in each ENGINE's zones (0 BD, 127 the last), an ENGINE change mid-note (the
//             largest state, PI-CC, ringing) fades without a step, the new ENGINE's voice bit-exact from a clean state,
//             nothing written outside the state; back to the playing ENGINE during the fade plays on
// MIT, Copyright 2026 trigcondition and contributors.
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rmp_api.h"
#include "rmp_loader.h"
extern "C" {
#include "mdkit.h"
#include "../mdk_tap.h"
}

#define VOICE(v) extern "C" const mdkit_voice mdkit_voice_##v;
VOICE(pi_bd) VOICE(pi_sd) VOICE(pi_mt) VOICE(pi_ml) VOICE(pi_ma) VOICE(pi_rs) VOICE(pi_rc) VOICE(pi_cc) VOICE(pi_hh)
VOICE(efm_bd) VOICE(efm_sd) VOICE(efm_xt) VOICE(efm_cp) VOICE(efm_rs) VOICE(efm_cb) VOICE(efm_hh) VOICE(efm_cy)
VOICE(trx_bd) VOICE(trx_sd) VOICE(trx_xt) VOICE(trx_cp) VOICE(trx_rs) VOICE(trx_cb) VOICE(trx_ch) VOICE(trx_oh)
VOICE(trx_cy) VOICE(trx_ma) VOICE(trx_cl) VOICE(trx_xc) VOICE(trx_b2)

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } } while (0)

struct Family { const char *name, *file; std::vector<const mdkit_voice *> ref; std::vector<int> engines; };
static const Family *g_fam;   // the family checked (MD: engines holds each ENGINE's TYPE count)

static double now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

// ---- the package, placed as the Pi's loader places it (core/host/um_host.cpp's layout: RW data right after the code)
struct Pkg {
  std::vector<uint8_t> bytes;
  const rmp_plugin *p = nullptr;
};
static void hlog(const char *m) { printf("  plugin log: %s\n", m); }
static uint32_t hrand(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static rmp_host g_host = {RMP_API, 48000.0f, 12, hlog, hrand, {}};

static bool load(const char *path, Pkg &k) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint8_t b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) k.bytes.insert(k.bytes.end(), b, b + n);
  fclose(f);
  rmp_host_target = RMP_TARGET_A64;
  rmp_info info;
  rmp_status vs = rmp_verify(k.bytes.data(), (uint32_t)k.bytes.size(), &info);
  if (vs != RMP_OK) { printf("  verify: %d\n", (int)vs); return false; }
  uint32_t code = (info.data_start + 4095u) & ~4095u;
  uint32_t data = (info.image_size - info.data_start + info.bss_size + 4095u) & ~4095u;
  if (!data) data = 4096;   // (no writable part: mmap wants a length)
  uint8_t *c = (uint8_t *)mmap(nullptr, code, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (c == MAP_FAILED) { perror("  mmap code"); return false; }
  uint8_t *d = (uint8_t *)mmap(c + code, data, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);   // (a hint: ADRP's +-4 GB)
  if (d == MAP_FAILED) { perror("  mmap data"); return false; }
  pthread_jit_write_protect_np(0);
  rmp_status st = rmp_place_a64(&info, c, (uintptr_t)c, d, (uintptr_t)d);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(c, code);
  if (st != RMP_OK) { printf("  place: %d\n", (int)st); return false; }
  k.p = (const rmp_plugin *)(c + info.entry_offset);
  return k.p->magic == RMP_PLUGIN_MAGIC && k.p->init(&g_host) == 0;
}

// ---- one voice of the package, its state between guard bytes
struct Voice {
  const rmp_plugin *p;
  std::vector<uint8_t> mem;
  uint8_t *st;
  enum { kGuard = 256 };
  explicit Voice(const rmp_plugin *pl) : p(pl), mem(pl->state_bytes + 2 * kGuard + 8, 0xA5) {
    st = (uint8_t *)(((uintptr_t)mem.data() + kGuard + 7) & ~(uintptr_t)7);
    memset(st, 0, p->state_bytes);   // the host zeroes it
    p->voice_init(st);
  }
  bool guards_ok() const {
    for (const uint8_t *q = mem.data(); q < st; ++q) if (*q != 0xA5) return false;
    for (const uint8_t *q = st + p->state_bytes; q < mem.data() + mem.size(); ++q) if (*q != 0xA5) return false;
    return true;
  }
  const mdk_tap *tap() const { return (const mdk_tap *)st; }
};

static float type_value(int t, int n) { return (float)((t * 128 + 64) / n); }   // the middle of TYPE t's zone

// the knobs: P1..P8 = v[0..7], P9 TYPE, P10 LEV 100, the rest 64. MD: voice t (0..29) as P9 ENGINE and P10 TYPE (the
// middles of their zones), P11 LEV
static void params(Voice &v, const int *k8, int t, int n, float lev = 100.0f) {
  float p[16];
  for (int i = 0; i < 16; ++i) p[i] = 64.0f;
  for (int i = 0; i < 8; ++i) p[i] = (float)k8[i];
  const std::vector<int> &en = g_fam->engines;
  if (en.empty()) {
    p[8] = type_value(t, n);
    p[9] = lev;
  } else {
    int e = 0;
    while (t >= en[e]) t -= en[e++];
    p[8] = type_value(e, (int)en.size());
    p[9] = type_value(t, en[e]);
    p[10] = lev;
  }
  v.p->params(v.st, p);
}
// MD: the raw knobs P9 ENGINE, P10 TYPE (P1..P8 at k8, LEV 100)
static void params_md(Voice &v, const int *k8, float eng, float type) {
  float p[16];
  for (int i = 0; i < 16; ++i) p[i] = 64.0f;
  for (int i = 0; i < 8; ++i) p[i] = (float)k8[i];
  p[8] = eng, p[9] = type, p[10] = 100.0f;
  v.p->params(v.st, p);
}
static void defaults(const mdkit_voice *mv, int *d) {
  for (int c = 0; c < 8; ++c) d[c] = c < (int)mv->control_count ? (mv->control_defaults ? mv->control_defaults[c] : 64) : 64;
}

struct Run { std::vector<float> out, core; };
// blocks of 12 frames; the core samples gathered from the tap after each call (the ring holds 64, a call adds ~12)
static void render(Voice &v, int blocks, Run &r, int change_at = -1, const int *k8b = nullptr, int t = 0, int n = 1) {
  float b[12];
  for (int i = 0; i < blocks; ++i) {
    if (i == change_at) params(v, k8b, t, n);
    uint64_t was = v.tap()->produced;
    v.p->render(v.st, b, 12);
    r.out.insert(r.out.end(), b, b + 12);
    for (uint64_t s = was; s < v.tap()->produced; ++s) r.core.push_back(v.tap()->ring[s & v.tap()->ring_mask]);
  }
}

// mdkit's own: calloc, init, set_control(k, v[k]) for its controls, trigger, render; a control change after
// `at` samples (k8b at that sample, the plugin's tap count) when at >= 0, a second trigger after `retrig` when >= 0
static std::vector<float> reference(const mdkit_voice *mv, const int *k8, size_t len, long at = -1, const int *k8b = nullptr,
                                    long retrig = -1) {
  void *s = calloc(1, mv->state_bytes);
  mv->init(s);
  for (unsigned c = 0; c < mv->control_count; ++c) mv->set_control(s, c, (unsigned)k8[c]);
  mv->trigger(s);
  std::vector<float> o(len);
  size_t pos = 0;
  while (pos < len) {
    size_t m = len - pos > 257 ? 257 : len - pos;
    if (at >= 0 && pos < (size_t)at && pos + m > (size_t)at) m = (size_t)at - pos;
    if (retrig >= 0 && pos < (size_t)retrig && pos + m > (size_t)retrig) m = (size_t)retrig - pos;
    if (retrig >= 0 && pos == (size_t)retrig) mv->trigger(s);
    if (at >= 0 && pos == (size_t)at)
      for (unsigned c = 0; c < mv->control_count; ++c)
        if (k8b[c] != k8[c]) mv->set_control(s, c, (unsigned)k8b[c]);
    mv->render(s, o.data() + pos, (unsigned)m);
    pos += m;
  }
  free(s);
  return o;
}

static bool same(const std::vector<float> &a, const std::vector<float> &b, size_t n, size_t *first = nullptr) {
  if (a.size() < n || b.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (memcmp(&a[i], &b[i], 4)) { if (first) *first = i; return false; }
  return true;
}
static double peak(const std::vector<float> &a, size_t from = 0, size_t to = SIZE_MAX) {
  double p = 0;
  for (size_t i = from; i < a.size() && i < to; ++i) p = fmax(p, fabs((double)a[i]));
  return p;
}
static bool finite(const std::vector<float> &a) {
  for (float x : a) if (!std::isfinite(x)) return false;
  return true;
}

// the 48 kHz reference of core samples x: a 512-tap Kaiser (beta 12) windowed sinc of the plugin's cutoff (0.92 x
// 22050 Hz) at the plugin's output times t_j = j * 147 / 160
static std::vector<double> ideal(const std::vector<float> &x, size_t nout) {
  std::vector<double> y(nout);
  const int H = 256;
  auto i0 = [](double v) { double s = 1, t = 1; for (int k = 1; k < 60; ++k) { t *= (v / (2 * k)) * (v / (2 * k)); s += t; } return s; };
  const double beta = 12, cut = 0.92, ib = i0(beta);
  for (size_t j = 0; j < nout; ++j) {
    double t = j * 147.0 / 160.0;
    long c = (long)floor(t);
    double acc = 0, g = 0;
    for (long m = c - H + 1; m <= c + H; ++m) {
      double d = m - t, xx = M_PI * cut * d;
      double s = cut * (fabs(xx) < 1e-12 ? 1.0 : sin(xx) / xx);
      double r = d / H, w = fabs(r) < 1 ? i0(beta * sqrt(1 - r * r)) / ib : 0;
      g += s * w;
      if (m >= 0 && m < (long)x.size()) acc += x[(size_t)m] * s * w;
    }
    y[j] = acc / g;
  }
  return y;
}

// signal y against error e: 10 log10 of their powers, below 16 kHz only when inband (both through a 511-tap
// windowed-sinc lowpass at 48 kHz): the band the resampler keeps flat (mdk_fir.h: -0.24 dB at 18 kHz, -4.6 at 20)
static double snr(const std::vector<double> &y, const std::vector<double> &e, bool inband) {
  static std::vector<double> lp;
  if (lp.empty()) {
    const int H = 255;
    const double fc = 16000.0 / 24000.0;
    for (int k = -H; k <= H; ++k) {
      double x = M_PI * fc * k, s = fc * (k ? sin(x) / x : 1.0);
      double w = 0.42 + 0.5 * cos(M_PI * k / (H + 1)) + 0.08 * cos(2 * M_PI * k / (H + 1));   // Blackman
      lp.push_back(s * w);
    }
  }
  double sy = 0, se = 0;
  int H = (int)lp.size() / 2;
  for (size_t j = 0; j < y.size(); ++j) {
    double a = y[j], b = e[j];
    if (inband) {
      a = b = 0;
      for (int k = -H; k <= H; ++k) {
        long i = (long)j + k;
        if (i >= 0 && i < (long)y.size()) a += y[(size_t)i] * lp[(size_t)(k + H)], b += e[(size_t)i] * lp[(size_t)(k + H)];
      }
    }
    sy += a * a, se += b * b;
  }
  return 10 * log10(sy / (se > 0 ? se : 1e-300));
}

// review 2: voice `from` rings, then a params() moving to voice `to` and a trig in the same tick (a p-locked step).
// The output less a fresh voice's `to` hit (the same params and trig: bit-identical but for what the old voice adds)
// is the old voice's tail: it must start where the output was (no step larger than the signal's own, or than a
// 20th of its level) and be gone after the 2 ms fade. Returns false with the numbers in *step, *own, *last.
static bool trig_switch(const rmp_plugin *pl, const Family &F, int from, int to, double *step, double *own, double *last,
                        double *setup_us) {
  int n = (int)F.ref.size(), df[8], dt[8];
  defaults(F.ref[from], df);
  defaults(F.ref[to], dt);
  Voice v(pl);
  params(v, df, from, n);
  v.p->note_on(v.st, 60.0f, 1.0f);
  Run r;
  render(v, 400, r);
  *own = 0;
  for (size_t i = r.out.size() - 480; i < r.out.size(); ++i) *own = fmax(*own, fabs(r.out[i] - r.out[i - 1]));
  *last = r.out.back();
  params(v, dt, to, n);
  double a = now_ns();
  v.p->note_on(v.st, 60.0f, 1.0f);
  *setup_us = (now_ns() - a) / 1000;
  Run q;
  render(v, 200, q);
  Voice w(pl);
  params(w, dt, to, n);
  w.p->note_on(w.st, 60.0f, 1.0f);
  Run e;
  render(w, 200, e);
  bool ok = v.tap()->type == (uint32_t)to && same(q.core, e.core, q.core.size()) && v.guards_ok();
  double prev = *last, tail_after = 0;
  *step = 0;
  for (size_t i = 0; i < q.out.size(); ++i) {
    double t = (double)q.out[i] - (double)e.out[i];
    if (i < 96) *step = fmax(*step, fabs(t - prev));
    else tail_after = fmax(tail_after, fabs(t));
    prev = t;
  }
  return ok && tail_after == 0.0 && *step <= fmax(*own * 1.5, fabs(*last) / 20);
}

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "../out";   // mdk_check DIR [md|pi|efm|trx ...]
  std::vector<Family> fams = {
      {"md", "mdk.rmp", {&mdkit_voice_efm_bd, &mdkit_voice_efm_sd, &mdkit_voice_efm_xt, &mdkit_voice_efm_cp,
                         &mdkit_voice_efm_rs, &mdkit_voice_efm_cb, &mdkit_voice_efm_hh, &mdkit_voice_efm_cy,
                         &mdkit_voice_pi_bd, &mdkit_voice_pi_sd, &mdkit_voice_pi_mt, &mdkit_voice_pi_ml, &mdkit_voice_pi_ma,
                         &mdkit_voice_pi_rs, &mdkit_voice_pi_rc, &mdkit_voice_pi_cc, &mdkit_voice_pi_hh,
                         &mdkit_voice_trx_bd, &mdkit_voice_trx_sd, &mdkit_voice_trx_xt, &mdkit_voice_trx_cp,
                         &mdkit_voice_trx_rs, &mdkit_voice_trx_cb, &mdkit_voice_trx_ch, &mdkit_voice_trx_oh,
                         &mdkit_voice_trx_cy, &mdkit_voice_trx_ma, &mdkit_voice_trx_cl, &mdkit_voice_trx_xc,
                         &mdkit_voice_trx_b2}, {8, 9, 13}},
      {"pi", "mdk_pi.rmp", {&mdkit_voice_pi_bd, &mdkit_voice_pi_sd, &mdkit_voice_pi_mt, &mdkit_voice_pi_ml, &mdkit_voice_pi_ma,
                            &mdkit_voice_pi_rs, &mdkit_voice_pi_rc, &mdkit_voice_pi_cc, &mdkit_voice_pi_hh}},
      {"efm", "mdk_efm.rmp", {&mdkit_voice_efm_bd, &mdkit_voice_efm_sd, &mdkit_voice_efm_xt, &mdkit_voice_efm_cp,
                              &mdkit_voice_efm_rs, &mdkit_voice_efm_cb, &mdkit_voice_efm_hh, &mdkit_voice_efm_cy}},
      {"trx", "mdk_trx.rmp", {&mdkit_voice_trx_bd, &mdkit_voice_trx_sd, &mdkit_voice_trx_xt, &mdkit_voice_trx_cp,
                              &mdkit_voice_trx_rs, &mdkit_voice_trx_cb, &mdkit_voice_trx_ch, &mdkit_voice_trx_oh,
                              &mdkit_voice_trx_cy, &mdkit_voice_trx_ma, &mdkit_voice_trx_cl, &mdkit_voice_trx_xc,
                              &mdkit_voice_trx_b2}}};
  const int kBlocks = 4000;   // 1 s at 48 kHz
  for (Family &F : fams) {
    bool want = argc <= 2;   // argv[2..]: the families to check (default all)
    for (int i = 2; i < argc; ++i) want = want || !strcmp(argv[i], F.name);
    if (!want) continue;
    g_fam = &F;
    Pkg k;
    std::string path = dir + "/" + F.file;
    bool ok = load(path.c_str(), k);
    CHECK(ok, "%s does not load", path.c_str());
    if (!ok) continue;
    const int n = (int)F.ref.size();
    printf("%s: %s loads: state %u B a voice, %d TYPEs, cpu_permille %u\n", F.name, F.file, k.p->state_bytes, n,
           k.p->cpu_permille);
    CHECK(k.p->state_bytes <= 65536, "state over the host's 64 KB");
    {   // the format (review 3): MD (an RLB2 block) 4, so a host from before RLB2 refuses it; MD-* stay 3. A 5 is refused
      rmp_header h;
      memcpy(&h, k.bytes.data(), sizeof h);
      CHECK(h.format == (F.engines.empty() ? 3 : 4), "%s: format %u", F.name, (unsigned)h.format);
      std::vector<uint8_t> t = k.bytes;
      t[4] = 5;
      rmp_info ti;
      CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &ti) == RMP_ERR_FORMAT, "%s: format 5 not refused", F.name);
    }
    double worst_ns = 0, worst_setup = 0;
    for (int t = 0; t < n; ++t) {
      const mdkit_voice *mv = F.ref[t];
      int def[8];
      for (int c = 0; c < 8; ++c) def[c] = c < (int)mv->control_count ? (mv->control_defaults ? mv->control_defaults[c] : 64) : 64;
      // render + exact (the voice's defaults)
      Voice v(k.p);
      params(v, def, t, n);
      CHECK(v.tap()->type == (uint32_t)t, "%s TYPE %d: the tap says %u", F.name, t, v.tap()->type);
      v.p->note_on(v.st, 60.0f, 1.0f);
      Run r;
      double t0 = now_ns();
      render(v, kBlocks, r);
      double dt = (now_ns() - t0) / kBlocks;
      std::vector<float> ref = reference(mv, def, r.core.size());
      size_t first = 0;
      bool ex = same(r.core, ref, r.core.size(), &first);
      CHECK(ex, "%s %s defaults: core sample %zu differs (%.9g vs %.9g)", F.name, mv->name, first,
            r.core.empty() ? 0.0 : r.core[first], ref.empty() ? 0.0 : ref[first]);
      double pk = peak(r.out);
      CHECK(pk > 1e-3 && finite(r.out), "%s %s: peak %g", F.name, mv->name, pk);
      CHECK(v.guards_ok(), "%s %s: wrote outside its state", F.name, mv->name);
      // the worst render call (a second run, timed per call)
      double wc = 0;   // the 99.9th percentile call (the Mac's scheduler preempts now and then)
      {
        std::vector<double> calls;
        Voice w(k.p);
        params(w, def, t, n);
        w.p->note_on(w.st, 60.0f, 1.0f);
        float b[12];
        for (int i = 0; i < kBlocks; ++i) {
          double a = now_ns();
          w.p->render(w.st, b, 12);
          calls.push_back(now_ns() - a);
        }
        std::sort(calls.begin(), calls.end());
        wc = calls[calls.size() * 999 / 1000];
      }
      // resample: 48 kHz against the ideal of the same core samples, the gain (LEV 100, velocity 1) taken out
      std::vector<double> y = ideal(r.core, 48000 - 64), e(y.size());
      double g = 100.0 / 127.0;
      for (size_t j = 0; j < y.size(); ++j) e[j] = r.out[j] / g - y[j];
      double snr_all = snr(y, e, false), snr = ::snr(y, e, true);
      CHECK(snr > 50.0, "%s %s: resampled SNR below 16 kHz %.1f dB", F.name, mv->name, snr);
      // controls: each of the voice's controls at 0 and 127, bit-exact against mdkit's set_control; the rest inert.
      // Two hits (0.15 s apart): TRX-CP, TRX-MA and TRX-CL latch their controls at a trigger from mdkit's control
      // clock, so a value set just before the first hit is heard from the second (mdkit: "take effect on the next
      // trigger")
      auto two_hits = [&](const int *kk, Run &q, long *retrig) {
        Voice u(k.p);
        params(u, kk, t, n);
        u.p->note_on(u.st, 60.0f, 1.0f);
        render(u, 600, q);
        *retrig = (long)u.tap()->produced;
        u.p->note_on(u.st, 60.0f, 1.0f);
        render(u, 600, q);
        return u.guards_ok();
      };
      Run base2;
      long rt0 = 0;
      two_hits(def, base2, &rt0);
      int moved = 0, inert_ok = 0, nc = (int)mv->control_count;
      for (int c = 0; c < 8; ++c) {
        int vals[2] = {0, 127};
        for (int x : vals) {
          int kk[8];
          memcpy(kk, def, sizeof kk);
          kk[c] = x;
          Run q;
          long rt = 0;
          two_hits(kk, q, &rt);
          if (c < nc) {
            std::vector<float> rr = reference(mv, kk, q.core.size(), -1, nullptr, rt);
            CHECK(same(q.core, rr, q.core.size()), "%s %s P%d=%d: not mdkit's set_control(%d, %d)", F.name, mv->name, c + 1, x, c, x);
            if (x != def[c] && !same(q.core, base2.core, q.core.size())) ++moved;
          } else {
            bool in = same(q.core, base2.core, q.core.size());
            CHECK(in, "%s %s P%d=%d: not inert", F.name, mv->name, c + 1, x);
            inert_ok += in;
          }
        }
      }
      // a control moved mid-note (P2 to 0 or 127 after 0.25 s), bit-exact at the tap's sample count
      {
        int kb[8];
        memcpy(kb, def, sizeof kb);
        kb[1] = def[1] > 63 ? 0 : 127;
        Voice u(k.p);
        params(u, def, t, n);
        u.p->note_on(u.st, 60.0f, 1.0f);
        Run q;
        render(u, 1000, q);
        long at = (long)q.core.size();
        render(u, 1000, q, 0, kb, t, n);
        std::vector<float> rr = reference(mv, def, q.core.size(), at, kb);
        CHECK(same(q.core, rr, q.core.size()), "%s %s: P2 moved mid-note not as mdkit", F.name, mv->name);
      }
      // the set-up of this TYPE (params() of a TYPE change on a voice that has not sounded): the control core's cost
      double su = 0;
      {
        Voice u(k.p);
        params(u, def, (t + 1) % n, n);
        double a = now_ns();
        params(u, def, t, n);
        su = now_ns() - a;
      }
      worst_ns = fmax(worst_ns, dt), worst_setup = fmax(worst_setup, su);
      printf("  %-7s %d controls (%d move the sound at 0/127, %d inert Ps checked): exact, peak %.3f, SNR <16 kHz %.1f dB "
             "(full band %.1f), %.0f ns a block (99.9 %% %.0f), set-up %.1f us\n",
             mv->name, nc, moved, inert_ok, ex ? pk : 0.0, snr, snr_all, dt, wc, su / 1000);
    }
    // a TYPE change mid-note: BD rings, TYPE 1 at 0.1 s: faded out in 2 ms, no step; the next trig plays TYPE 1
    {
      const mdkit_voice *m0 = F.ref[0], *m1 = F.ref[1];
      int def[8];
      for (int c = 0; c < 8; ++c) def[c] = c < (int)m0->control_count ? (m0->control_defaults ? m0->control_defaults[c] : 64) : 64;
      Voice v(k.p);
      params(v, def, 0, n);
      v.p->note_on(v.st, 60.0f, 1.0f);
      Run r;
      render(v, 400, r);
      double step_before = 0;
      for (size_t i = r.out.size() - 480; i < r.out.size(); ++i) step_before = fmax(step_before, fabs(r.out[i] - r.out[i - 1]));
      double last = fabs(r.out.back());
      params(v, def, 1, n);
      CHECK(v.tap()->type == 0, "%s: the TYPE changed at once while BD rang", F.name);
      size_t at = r.out.size();
      render(v, 40, r);   // 10 ms
      double step = 0;
      for (size_t i = at; i < r.out.size(); ++i) step = fmax(step, fabs(r.out[i] - r.out[i - 1]));
      double after = peak(r.out, at + 100);
      CHECK(after == 0.0, "%s: not silent 2 ms after the TYPE change (peak %g)", F.name, after);
      CHECK(step <= fmax(step_before * 1.5, last / 20), "%s: a step of %g at the change (the signal's own %g, a cut %g)", F.name, step,
            step_before, last);
      int d1[8];
      for (int c = 0; c < 8; ++c) d1[c] = c < (int)m1->control_count ? (m1->control_defaults ? m1->control_defaults[c] : 64) : 64;
      params(v, d1, 1, n);   // faded: this params() sets TYPE 1 up
      CHECK(v.tap()->type == 1, "%s: TYPE 1 not set up after the fade", F.name);
      v.p->note_on(v.st, 60.0f, 1.0f);
      Run q;
      render(v, 1000, q);
      std::vector<float> rr = reference(m1, d1, q.core.size());
      CHECK(same(q.core, rr, q.core.size()), "%s: TYPE 1 after the change not bit-exact", F.name);
      CHECK(v.guards_ok(), "%s: the TYPE change wrote outside the state", F.name);
      // back to TYPE 1 -> 0 -> 1 within the fade: TYPE 1 plays on, faded back in
      v.p->note_on(v.st, 60.0f, 1.0f);
      render(v, 20, q);
      params(v, d1, 0, n);
      render(v, 2, q);
      params(v, d1, 1, n);
      size_t b0 = q.out.size();
      render(v, 40, q);
      CHECK(v.tap()->type == 1 && peak(q.out, b0 + 200) > 0,
            "%s: back to the playing TYPE during the fade", F.name);
      // a trig during the fade: the new TYPE at once
      params(v, def, 0, n);
      v.p->note_on(v.st, 60.0f, 1.0f);
      CHECK(v.tap()->type == 0, "%s: a trig during the fade did not set the new TYPE up", F.name);
      // a TYPE change with a trig while the old voice rings (review 2): its tail fades out under the new hit
      double st2, own2, last2, su2;
      bool ok2 = trig_switch(k.p, F, 0, 1, &st2, &own2, &last2, &su2);
      if (!F.engines.empty()) {   // MD; MD-PI/EFM/TRX are kept byte for byte (a hard cut there, as before)
        CHECK(ok2, "%s: a TYPE change with a trig: the old voice's tail steps %g (the signal's own %g, its level %g)", F.name,
              st2, own2, last2);
        printf("  TYPE change with a trig while BD rings: the old voice faded under the new hit (largest step %.4f, the "
               "signal's own %.4f, a hard cut %.4f), the new hit bit-exact; the trig's call %.1f us\n", st2, own2,
               fabs(last2), su2);
      } else {
        printf("  TYPE change with a trig while BD rings: a hard cut of %.4f (MD-* unchanged; MD fades it)\n", fabs(last2));
      }
      printf("  TYPE change mid-note: faded to 0 within 2 ms (largest step %.4f, the signal's own %.4f, a hard cut %.4f), "
             "the next trig bit-exact, no writes outside the state\n", step, step_before, last);
    }
    if (!F.engines.empty()) {   // MD: the ENGINE knob
      // P10's raw value in each ENGINE's zones: 0 its BD, 127 its last, 64 the middle one
      int base = 0;
      for (int e = 0; e < (int)F.engines.size(); ++e) {
        int ne = F.engines[e], d0[8];
        defaults(F.ref[0], d0);
        float ev = type_value(e, (int)F.engines.size());
        struct { float v; int want; } cases[] = {{0.0f, 0}, {127.0f, ne - 1}, {64.0f, (64 * ne) >> 7}, {200.0f, ne - 1}, {-3.0f, 0}};
        for (auto &c : cases) {
          Voice u(k.p);
          params_md(u, d0, ev, c.v);
          CHECK(u.tap()->type == (uint32_t)(base + c.want), "md ENGINE %d TYPE value %g: voice %u, want %d", e, c.v,
                u.tap()->type, base + c.want);
        }
        base += ne;
      }
      CHECK(base == n, "md: the ENGINEs' counts add up to %d, not %d", base, n);
      // an ENGINE change mid-note: PI-CC (voice 15, the largest state) rings, then TRX (P9 127) at the same P10: faded
      // out in 2 ms without a step; the next params() sets TRX's voice up from a clean state, bit-exact
      const int cc = 15;
      const mdkit_voice *mc = F.ref[cc];
      int dc[8];
      defaults(mc, dc);
      Voice v(k.p);
      params(v, dc, cc, n);
      v.p->note_on(v.st, 60.0f, 1.0f);
      Run r;
      render(v, 400, r);
      double step_before = 0;
      for (size_t i = r.out.size() - 480; i < r.out.size(); ++i) step_before = fmax(step_before, fabs(r.out[i] - r.out[i - 1]));
      double last = fabs(r.out.back());
      float p10 = type_value(cc - 8, 9);   // PI-CC's P10 (7 of 9): in TRX's 13 zones (p10 * 13) >> 7
      int tt = 17 + (((int)p10 * 13) >> 7);
      params_md(v, dc, 127.0f, p10);
      CHECK(v.tap()->type == (uint32_t)cc, "md: the ENGINE changed at once while PI-CC rang");
      size_t at = r.out.size();
      render(v, 40, r);
      double step = 0;
      for (size_t i = at; i < r.out.size(); ++i) step = fmax(step, fabs(r.out[i] - r.out[i - 1]));
      double after = peak(r.out, at + 100);
      CHECK(after == 0.0, "md: not silent 2 ms after the ENGINE change (peak %g)", after);
      CHECK(step <= fmax(step_before * 1.5, last / 20), "md: a step of %g at the ENGINE change (the signal's own %g, a cut %g)",
            step, step_before, last);
      const mdkit_voice *mt = F.ref[tt];
      int dt[8];
      defaults(mt, dt);
      params_md(v, dt, 127.0f, p10);   // faded: this sets TRX's voice up
      CHECK(v.tap()->type == (uint32_t)tt, "md: TRX voice %d not set up after the fade (%u)", tt, v.tap()->type);
      v.p->note_on(v.st, 60.0f, 1.0f);
      Run q;
      render(v, 1000, q);
      std::vector<float> rr = reference(mt, dt, q.core.size());
      CHECK(same(q.core, rr, q.core.size()), "md: %s after the ENGINE change not bit-exact (stale state?)", mt->name);
      CHECK(v.guards_ok(), "md: the ENGINE change wrote outside the state");
      // back to the playing ENGINE during the fade: it plays on
      v.p->note_on(v.st, 60.0f, 1.0f);
      render(v, 20, q);
      params_md(v, dt, 0.0f, p10);
      render(v, 2, q);
      params_md(v, dt, 127.0f, p10);
      size_t b0 = q.out.size();
      render(v, 40, q);
      CHECK(v.tap()->type == (uint32_t)tt && peak(q.out, b0 + 200) > 0, "md: back to the playing ENGINE during the fade");
      // every ENGINE pair: a change mid-note, then the new voice bit-exact (BD of each ENGINE to the next one's BD)
      int bds[3] = {0, 8, 17};
      for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) {
          if (a == b) continue;
          Voice w(k.p);
          int da[8], db[8];
          defaults(F.ref[bds[a]], da);
          defaults(F.ref[bds[b]], db);
          params(w, da, bds[a], n);
          w.p->note_on(w.st, 60.0f, 1.0f);
          Run x;
          render(w, 200, x);
          params(w, db, bds[b], n);
          render(w, 40, x);
          params(w, db, bds[b], n);
          w.p->note_on(w.st, 60.0f, 1.0f);
          Run y;
          render(w, 600, y);
          std::vector<float> ry = reference(F.ref[bds[b]], db, y.core.size());
          CHECK(w.tap()->type == (uint32_t)bds[b] && same(y.core, ry, y.core.size()) && w.guards_ok(),
                "md: ENGINE %d -> %d: the new BD not bit-exact", a, b);
        }
      // an ENGINE change with a trig while PI-CC rings, to TRX-CL and to FM-BD, and every ENGINE pair's BDs (review 2)
      int pairs[][2] = {{cc, tt}, {cc, 0}, {0, 8}, {0, 17}, {8, 0}, {8, 17}, {17, 0}, {17, 8}};
      double worst = 0, worst_su = 0;
      for (auto &pr : pairs) {
        double st3, own3, last3, su3;
        bool ok3 = trig_switch(k.p, F, pr[0], pr[1], &st3, &own3, &last3, &su3);
        CHECK(ok3, "md: %s -> %s with a trig: the tail steps %g (the signal's own %g, its level %g)", F.ref[pr[0]]->name,
              F.ref[pr[1]]->name, st3, own3, last3);
        worst = fmax(worst, st3), worst_su = fmax(worst_su, su3);
      }
      printf("  ENGINE change with a trig (8 pairs, PI-CC ringing among them): the old voice faded under the new hit, "
             "largest step %.4f; the trig's call at most %.1f us\n", worst, worst_su);
      printf("  ENGINE: P10 read in each ENGINE's zones; a change mid-note (PI-CC ringing -> %s) faded within 2 ms "
             "(largest step %.4f, the signal's own %.4f), the new voice bit-exact; every ENGINE pair clean\n",
             mt->name, step, step_before);
    }
    printf("  cpu: worst TYPE %.0f ns a 12-frame block on this Mac (%.2f %% of a 250 us block), set-up at most %.1f us\n",
           worst_ns, worst_ns / 2500.0, worst_setup / 1000);
  }
  printf(g_fail ? "MDKIT CHECKS: %d FAILED\n" : "MDKIT CHECKS PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
