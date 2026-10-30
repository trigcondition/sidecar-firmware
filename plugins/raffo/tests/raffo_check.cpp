// plugins/raffo/tests/raffo_check.cpp - the host checks of RAFFO: the RMPA package (make package: out/raffo.a64.rmp)
// loaded and run on this arm64 Mac as the Pi runs it (rmp_loader: verify, the two parts placed, relocated; MAP_JIT):
//   load      verifies, places, init()s; the header's flags opt into instancing (max_voices 8, pitch note); the
//             state fits the Pi's 64 KB (and the Teensy's 16 KB)
//   chord     C3 E3 G3 B3 (one state a note, as a poly track's instances): each sounds while held (rms), finite, within
//             +-1, nothing written outside its state (guard bytes), decays after note_off to exact silence
//   knobs     every P at 0, at 127, and random values each block mid-note: finite, within +-1
//   retrig    a note_on on a held voice (legato C2 -> G2: no retrigger) and on a releasing one (C2 again) (the envelopes go on from where
//             they are): no step larger than 1.5x the first attack's (no click)
//   glide     legato C3 -> C4 on a bare triangle (GLID 40): the pitch right after is between the two, then C4 (zero
//             crossings); GLID 0: C4 at once
//   cpu       ns per 12-frame render call (the best of 5 means) a voice, held, at the defaults and at full MIX / SUB /
//             RES / ENV; the native build against sine_test's, sc_acid's and sc_wavepad's (their declared permille)
// MIT, Copyright 2026 trigcondition and contributors.
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rmp_api.h"
#include "rmp_loader.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } } while (0)

static const int kFs = 48000, kBlock = 12;
static double now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
static void hlog(const char *m) { printf("  plugin log: %s\n", m); }
static uint32_t hrand(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static rmp_host g_host = {RMP_API, (float)kFs, kBlock, hlog, hrand, {}};
static rmp_host g_host1 = {1u, (float)kFs, kBlock, hlog, hrand, {}};   // sine_test is an API-1 plugin

// the package, placed as the Pi's loader places it (mdkit's mdk_check.cpp: RW data right after the code)
static const rmp_plugin *load(const char *path, rmp_info &info) {
  FILE *f = fopen(path, "rb");
  if (!f) return nullptr;
  static std::vector<uint8_t> bytes;
  uint8_t b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) bytes.insert(bytes.end(), b, b + n);
  fclose(f);
  rmp_host_target = RMP_TARGET_A64;
  rmp_status vs = rmp_verify(bytes.data(), (uint32_t)bytes.size(), &info);
  if (vs != RMP_OK) { printf("  verify: %d\n", (int)vs); return nullptr; }
  uint32_t code = (info.data_start + 4095u) & ~4095u;
  uint32_t data = (info.image_size - info.data_start + info.bss_size + 4095u) & ~4095u;
  if (!data) data = 4096;
  uint8_t *c = (uint8_t *)mmap(nullptr, code, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (c == MAP_FAILED) return nullptr;
  uint8_t *d = (uint8_t *)mmap(c + code, data, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (d == MAP_FAILED) return nullptr;
  pthread_jit_write_protect_np(0);
  rmp_status st = rmp_place_a64(&info, c, (uintptr_t)c, d, (uintptr_t)d);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(c, code);
  if (st != RMP_OK) { printf("  place: %d\n", (int)st); return nullptr; }
  const rmp_plugin *p = (const rmp_plugin *)(c + info.entry_offset);
  return p->magic == RMP_PLUGIN_MAGIC && p->init(&g_host) == 0 ? p : nullptr;
}

// one voice, its state between guard bytes
struct Voice {
  const rmp_plugin *p;
  std::vector<uint8_t> mem;
  uint8_t *st;
  enum { kGuard = 256 };
  explicit Voice(const rmp_plugin *pl) : p(pl), mem(pl->state_bytes + 2 * kGuard + 8, 0xA5) {
    st = (uint8_t *)(((uintptr_t)mem.data() + kGuard + 7) & ~(uintptr_t)7);
    memset(st, 0, p->state_bytes);
    p->voice_init(st);
  }
  bool guards_ok() const {
    for (const uint8_t *q = mem.data(); q < st; ++q) if (*q != 0xA5) return false;
    for (const uint8_t *q = st + p->state_bytes; q < mem.data() + mem.size(); ++q) if (*q != 0xA5) return false;
    return true;
  }
  void params(const float *p16) { p->params(st, p16); }
  void block(float *o) { p->render(st, o, kBlock); }
};

// the manifest's defaults (manifest.json; P16, without a knob, 64)
static const float kDef[16] = {48, 80, 72, 85, 80, 44, 46, 64, 0, 60, 110, 30, 30, 24, 30, 100};

static double rms(const std::vector<float> &x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) s += (double)x[i] * x[i];
  return b > a ? std::sqrt(s / (double)(b - a)) : 0.0;
}
static bool finite_in(const std::vector<float> &x) {
  for (float v : x) if (!std::isfinite(v) || v < -1.0f || v > 1.0f) return false;
  return true;
}

// ns per render call, held: the best mean of five runs of 20000 calls after a warm-up
static double time_voice(const rmp_plugin *p, const float *prm) {
  Voice v(p);
  v.params(prm);
  p->note_on(v.st, 48.0f, 0.8f);
  float o[kBlock];
  for (int i = 0; i < 2000; ++i) v.block(o);
  const int blocks = 20000;
  double mean = 1e30;
  for (int run = 0; run < 5; ++run) {   // the least disturbed of five runs
    double t0 = now_ns();
    for (int i = 0; i < blocks; ++i) v.block(o);
    mean = std::min(mean, (now_ns() - t0) / blocks);
  }
  return mean;
}

int main(int argc, char **argv) {
  const char *pkg = argc > 1 ? argv[1] : "../out/raffo.a64.rmp";
  const char *native = argc > 2 ? argv[2] : "../libraffo.dylib";
  rmp_info info;
  const rmp_plugin *p = load(pkg, info);
  CHECK(p, "%s does not load", pkg);
  if (!p) return 1;
  printf("load      %s: id %s, image %u B, bss %u B, state %u B, cpu %u permille, flags 0x%x\n", pkg, info.id,
         info.image_size, info.bss_size, p->state_bytes, p->cpu_permille, info.flags);
  CHECK(strcmp(info.id, "raffo.mono4") == 0, "id %s", info.id);
  CHECK(rmp_hf_max_voices(info.flags) == 8 && (info.flags & RMP_HF_INSTANCES), "max_voices: flags 0x%x", info.flags);
  CHECK(rmp_hf_pitch(info.flags) != RMP_PITCH_NONE, "pitch: flags 0x%x", info.flags);
  CHECK(p->state_bytes <= 16384u, "state %u over the Teensy's 16 KB (and the Pi's 64 KB)", p->state_bytes);

  // chord: one state a note
  const float notes[4] = {48, 52, 55, 59};
  const int held = kFs, rel = 3 * kFs / 2, total = (held + rel) / kBlock * kBlock;
  std::vector<float> mix(total, 0.0f);
  for (int k = 0; k < 4; ++k) {
    Voice v(p);
    v.params(kDef);
    p->note_on(v.st, notes[k], 0.8f);
    std::vector<float> x(total);
    for (int i = 0; i < total; i += kBlock) {
      if (i == held / kBlock * kBlock) p->note_off(v.st);
      v.block(&x[i]);
    }
    double r = rms(x, kFs / 10, held - kFs / 10), tail = rms(x, total - kFs / 5, total);
    float pk = 0;
    for (float s : x) pk = std::max(pk, std::fabs(s));
    printf("chord     note %.0f: rms held %.3f, peak %.3f, tail %.2g\n", notes[k], r, pk, tail);
    CHECK(finite_in(x), "note %.0f: not finite or outside +-1", notes[k]);
    CHECK(r > 0.05, "note %.0f: rms %.4f while held", notes[k], r);
    CHECK(tail == 0.0, "note %.0f: not silent after the release (%.3g)", notes[k], tail);
    CHECK(v.guards_ok(), "note %.0f: wrote outside its state", notes[k]);
    for (int i = 0; i < total; ++i) mix[i] += x[i];
  }
  printf("chord     the four summed: rms held %.3f\n", rms(mix, kFs / 10, held - kFs / 10));

  // knobs: all 0, all 127, random each block
  for (int mode = 0; mode < 3; ++mode) {
    Voice v(p);
    float prm[16];
    for (int i = 0; i < 16; ++i) prm[i] = mode == 0 ? 0.0f : 127.0f;
    v.params(mode == 2 ? kDef : prm);
    uint32_t rs = 12345;
    std::vector<float> x(kFs);
    p->note_on(v.st, mode == 1 ? 127.0f : 24.0f, 1.0f);
    for (int i = 0; i < kFs; i += kBlock) {
      if (mode == 2) {
        for (int j = 0; j < 16; ++j) prm[j] = (float)(hrand(&rs) % 128) + (float)(hrand(&rs) % 128) / 128.0f;
        v.params(prm);
        if (i % 4800 == 0) p->note_on(v.st, (float)(hrand(&rs) % 128), 1.0f);
      }
      v.block(&x[i]);
    }
    CHECK(finite_in(x), "knobs mode %d: not finite or outside +-1", mode);
    CHECK(v.guards_ok(), "knobs mode %d: wrote outside its state", mode);
    printf("knobs     %s: rms %.3f, finite\n", mode == 0 ? "all 0" : mode == 1 ? "all 127" : "random", rms(x, 0, x.size()));
  }

  // retrig: a legato note_on on a held voice, then note_off and a note_on in the release
  for (int mode = 0; mode < 2; ++mode) {
    Voice v(p);
    v.params(kDef);
    p->note_on(v.st, 36.0f, 0.8f);
    std::vector<float> x(kFs);
    const int at = kFs / 2 / kBlock * kBlock, off = at - kFs / 20 / kBlock * kBlock;
    for (int i = 0; i < kFs; i += kBlock) {
      if (mode == 1 && i == off) p->note_off(v.st);
      if (i == at) p->note_on(v.st, mode ? 36.0f : 43.0f, 0.8f);
      v.block(&x[i]);
    }
    float first = 0, around = 0;
    for (int i = 0; i < kFs / 10; ++i) first = std::max(first, std::fabs(x[i + 1] - x[i]));
    for (int i = at - 48; i < at + kFs / 10; ++i) around = std::max(around, std::fabs(x[i + 1] - x[i]));
    printf("retrig    %s: largest step %.3f (the first attack's %.3f), rms after %.3f\n", mode ? "in the release" : "legato",
           around, first, rms(x, at + kFs / 10, kFs));
    CHECK(around <= 1.5f * first + 1e-3f, "%s: a step of %.3f (the first attack's %.3f)", mode ? "release" : "legato", around, first);
    CHECK(rms(x, at + kFs / 10, kFs) > 0.05, "mode %d: silent after the second note", mode);
  }

  // glide: a bare triangle (WAV1 TRI, MIX SUB 0, CUT 127, RES ENV 0), C3 then legato C4; zero crossings a window
  for (int gl = 0; gl < 2; ++gl) {
    Voice v(p);
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[0] = 0; prm[3] = 0; prm[4] = 0; prm[5] = 127; prm[6] = 0; prm[7] = 0; prm[14] = gl ? 40.0f : 0.0f;
    v.params(prm);
    p->note_on(v.st, 48.0f, 0.8f);
    const int at = kFs / 4 / kBlock * kBlock, total = kFs;
    std::vector<float> x(total);
    for (int i = 0; i < total; i += kBlock) {
      if (i == at) p->note_on(v.st, 60.0f, 0.8f);
      v.block(&x[i]);
    }
    auto hz = [&](int a, int n) { int z = 0; for (int i = a + 1; i < a + n; ++i) z += (x[i - 1] < 0) != (x[i] < 0); return z * 0.5 * kFs / n; };
    double before = hz(at - kFs / 10, kFs / 10), early = hz(at, kFs / 50), late = hz(total - kFs / 5, kFs / 5);
    printf("glide     GLID %d: %.0f Hz before, %.0f Hz the first 20 ms, %.0f Hz at the end\n", gl ? 40 : 0, before, early, late);
    CHECK(std::fabs(before - 130.8) < 8 && std::fabs(late - 261.6) < 8, "GLID %d: %.0f / %.0f Hz", gl ? 40 : 0, before, late);
    if (gl) CHECK(early < 230, "GLID 40: %.0f Hz the first 20 ms (no glide)", early);
    else CHECK(early > 240, "GLID 0: %.0f Hz the first 20 ms (a glide)", early);
  }

  // cpu: the package (the Pi's code), then the native builds against the starter machines' declared figures (each an
  // estimate that "a Mac timing against sine_test agrees within the guard's 2x"): the median ratio
  double m = time_voice(p, kDef);
  float hard[16];
  memcpy(hard, kDef, sizeof hard);
  hard[3] = 127; hard[4] = 127; hard[6] = 127; hard[7] = 127;
  double m2 = time_voice(p, hard);
  printf("cpu       package: %.0f ns mean a 12-frame call (defaults), %.0f (MIX SUB RES ENV 127)\n", m, m2);
  void *hn = dlopen(native, RTLD_NOW);
  const rmp_plugin *pn = hn ? (const rmp_plugin *)dlsym(hn, "rmp_entry") : nullptr;
  CHECK(pn && pn->init(&g_host) == 0, "%s: no native build", native);
  if (pn) {
    double mn = time_voice(pn, hard);
    std::vector<double> est;
    for (int r = 3; r < argc; ++r) {
      void *h = dlopen(argv[r], RTLD_NOW);
      const rmp_plugin *pr = h ? (const rmp_plugin *)dlsym(h, "rmp_entry") : nullptr;
      if (!pr || pr->init(pr->api == 1u ? &g_host1 : &g_host) != 0) { printf("cpu       (%s: not loaded)\n", argv[r]); continue; }
      float rp[16];
      for (int i = 0; i < 16; ++i) rp[i] = 64.0f;
      rp[1] = 100.0f;
      double mr = time_voice(pr, rp);
      est.push_back(pr->cpu_permille * mn / mr);
      printf("cpu       native: raffo %.0f ns, %s %.0f ns (declared %u): ~%.0f permille\n", mn, strrchr(argv[r], '/') + 1,
             mr, pr->cpu_permille, est.back());
    }
    if (!est.empty()) {
      std::sort(est.begin(), est.end());
      double e = est[est.size() / 2];
      printf("cpu       estimate %.0f permille (median), declared %u\n", e, p->cpu_permille);
      CHECK(e <= 2.0 * p->cpu_permille, "the estimate %.0f is over the guard's 2x of the declared %u", e, p->cpu_permille);
    }
  }
  printf(g_fail ? "raffo_check: %d FAILED\n" : "raffo_check: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
