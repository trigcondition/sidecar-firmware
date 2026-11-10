// plugins/cloud/tests/cloud_check.cpp - the host checks of CLOUD: the RMPA package (make package: out/cloud.a64.rmp)
// loaded and run on this arm64 Mac as the Pi runs it (rmp_loader: verify, the two parts placed, relocated; MAP_JIT),
// through Plugin API 3 as the engine calls it (an rmp3_host, voice_init, voice_attach with an arena part, rmp3_block
// renders, stereo and mono routes). Each scenario writes a WAV to plugins/build-wav/cloud-<name>.wav:
//   load      the table: API 3 only, needs the arena, wants stereo, 8 states, the arena part per voice, state size
//   held      a held note at the defaults: sounds, stereo, finite, within +-1, no denormals; the arena was full of
//             junk at attach and none of it is heard (wet only: silence until the clear, then a fade in)
//   freeze    FRZ on and the note released: the cloud keeps playing; without FRZ it dies away to exact zeros
//   feedback  FDBK at 127 for 20 s in every quality and with the delay mode: bounded, no runaway, no DC, finite
//   note      a note change: the dry pitch moves at once, the cloud's pitch follows within seconds
//   synth     the SYNTH engine: sounds, the grains at the note's pitch
//   quality   a QUAL change mid-note: no burst (the old bytes are never read as the new format), no click
//   knobs     each P alone at 0 and 127, every P at 0, every P at 127, random knobs and notes each block
//   routes    a mono route (out[1] null) gets the mix; n 1..16; no arena / too small an arena: silence, no crash
//   calls     voice_init + voice_attach, params, note_on, note_off: their time (the API 3 limits: 0.5 / 2 ms)
//   cpu       ns per 12-frame render call, mean and worst, light to heaviest settings; the native build against the
//             starter machines' declared permille
// MIT, Copyright 2026 trigcondition and contributors.
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rmp_api.h"
#include "rmp_loader.h"
#include "rmp/rmp_api3.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } } while (0)

static const int kFs = 48000, kBlock = 12;
static const char* g_wavdir = "../../build-wav";
static double now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
static void hlog(const char* m) { printf("  plugin log: %s\n", m); }
static uint32_t hrand(uint32_t* s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static rmp3_host g_h3 = {RMP3_MAGIC, sizeof(rmp3_host), 0,
                         RMP3_CAP_TRANSPORT | RMP3_CAP_ARENA | RMP3_CAP_GUARD | RMP3_CAP_STEREO, 0, 0, 16u << 20, 0, {}};
static rmp_host g_host = {3u, (float)kFs, kBlock, hlog, hrand, {}};
static rmp_host g_host2 = {RMP_API, (float)kFs, kBlock, hlog, hrand, {}};
static rmp_host g_host1 = {1u, (float)kFs, kBlock, hlog, hrand, {}};
static rmp3_transport g_tr = {0, 120.0f, 20833.0f, 0.0f, -1, 0.0f, 0, 385};

// the package, placed as the Pi's loader places it (RW data right after the code)
static const rmp_plugin* load(const char* path, rmp_info& info) {
  FILE* f = fopen(path, "rb");
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
  uint8_t* c = (uint8_t*)mmap(nullptr, code, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (c == MAP_FAILED) return nullptr;
  uint8_t* d = (uint8_t*)mmap(c + code, data, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (d == MAP_FAILED) return nullptr;
  pthread_jit_write_protect_np(0);
  rmp_status st = rmp_place_a64(&info, c, (uintptr_t)c, d, (uintptr_t)d);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(c, code);
  if (st != RMP_OK) { printf("  place: %d\n", (int)st); return nullptr; }
  const rmp_plugin* p = (const rmp_plugin*)(c + info.entry_offset);
  return p->magic == RMP_PLUGIN_MAGIC && p->init(&g_host) == 0 ? p : nullptr;
}

static const rmp_plugin* g_p;
static const rmp3_plugin* g_p3;

// the manifest's defaults
static const float kDef[16] = {24, 72, 92, 64, 64, 40, 64, 96, 64, 0, 127, 0, 40, 76, 64, 0};
enum { TIME, SIZE, DENS, PTCH, SHAP, FDBK, SPRD, MIX, RVB, FRZ, QUAL, BANK, WAVE, TRND, PRND, ENG };
static const char* kShort[16] = {"TIME", "SIZE", "DENS", "PTCH", "SHAP", "FDBK", "SPRD", "MIX",
                                 "RVB", "FRZ", "QUAL", "BANK", "WAVE", "TRND", "PRND", "ENG"};
static float qual_value(int q) { return (float)(q * 32 + 16); }   // QUAL's zones: LOFI TAPE VINTAGE DIGITAL

// One voice: its state between guard bytes, its arena part (junk at attach, as a part a previous track left) with a
// guard page after it.
struct Voice {
  std::vector<uint8_t> mem;
  uint8_t* st;
  uint8_t* arena = nullptr;
  size_t arena_bytes = 0;
  double attach_ns = 0;
  enum { kGuard = 256, kArenaGuard = 4096 };
  explicit Voice(int arena_mode = 1, uint8_t junk = 0xA5, int vi = 0)   // arena_mode 0 none, 1 voice_want, 2 too small
      : mem(g_p->state_bytes + 2 * kGuard + 8, 0xA5) {
    st = (uint8_t*)(((uintptr_t)mem.data() + kGuard + 7) & ~(uintptr_t)7);
    memset(st, 0, g_p->state_bytes);
    if (arena_mode) {
      arena_bytes = arena_mode == 1 ? g_p3->voice_want : g_p3->voice_want / 2;
      void* a = nullptr;
      if (posix_memalign(&a, 4096, arena_bytes + kArenaGuard) == 0) arena = (uint8_t*)a;
      memset(arena, junk, arena_bytes);
      memset(arena + arena_bytes, 0xA5, kArenaGuard);
    }
    rmp3_voice v = {nullptr, arena, 0, (uint32_t)arena_bytes, 1, vi, -1, RMP3_AT_VOICE};
    double t0 = now_ns();
    g_p->voice_init(st);
    g_p3->voice_attach(st, &v);
    attach_ns = now_ns() - t0;
  }
  ~Voice() { free(arena); }
  bool guards_ok() const {
    for (const uint8_t* q = mem.data(); q < st; ++q) if (*q != 0xA5) return false;
    for (const uint8_t* q = st + g_p->state_bytes; q < mem.data() + mem.size(); ++q) if (*q != 0xA5) return false;
    if (arena)
      for (size_t i = 0; i < kArenaGuard; ++i) if (arena[arena_bytes + i] != 0xA5) return false;
    return true;
  }
  void params(const float* p16) { g_p->params(st, p16); }
  void note_on(float note, float vel = 0.9f) { g_p->note_on(st, note, vel); }
  void note_off() { g_p->note_off(st); }
  // n frames; r null: a mono route (TRK)
  void block(float* l, float* r, int n = kBlock) {
    static uint32_t gen = 0;
    rmp3_block b;
    memset(&b, 0, sizeof b);
    b.size = sizeof b, b.n = n, b.flags = r ? 0u : RMP3_B_MONO_OUT, b.gen = gen++;
    b.out[0] = l, b.out[1] = r, b.place = RMP3_AT_VOICE, b.transport = &g_tr;
    g_p3->render(st, &b);
  }
};

struct Stereo {
  std::vector<float> l, r;
  void resize(size_t n) { l.assign(n, 0.0f), r.assign(n, 0.0f); }
  size_t size() const { return l.size(); }
};

// render `secs` more into s from frame `at`, calling `each(frame)` before each block
template <typename F>
static void run(Voice& v, Stereo& s, size_t at, double secs, F each) {
  size_t total = (size_t)(secs * kFs) / kBlock * kBlock;
  if (s.size() < at + total) { s.l.resize(at + total, 0.0f); s.r.resize(at + total, 0.0f); }
  for (size_t i = at; i < at + total; i += kBlock) {
    each(i);
    v.block(&s.l[i], &s.r[i]);
  }
}
static void run(Voice& v, Stereo& s, size_t at, double secs) { run(v, s, at, secs, [](size_t) {}); }

static double rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, x.size());
  for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
  return b > a ? std::sqrt(s / (double)(b - a)) : 0.0;
}
static double rms2(const Stereo& s, size_t a, size_t b) { return std::sqrt(0.5 * (std::pow(rms(s.l, a, b), 2) + std::pow(rms(s.r, a, b), 2))); }
static float peak(const Stereo& s, size_t a, size_t b) {
  float p = 0;
  b = std::min(b, s.size());
  for (size_t i = a; i < b; ++i) p = std::max(p, std::max(std::fabs(s.l[i]), std::fabs(s.r[i])));
  return p;
}
static double mean(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, x.size());
  for (size_t i = a; i < b; ++i) s += x[i];
  return b > a ? s / (double)(b - a) : 0.0;
}
static float max_step(const Stereo& s, size_t a, size_t b) {
  float m = 0;
  b = std::min(b, s.size());
  for (size_t i = a + 1; i < b; ++i) m = std::max(m, std::max(std::fabs(s.l[i] - s.l[i - 1]), std::fabs(s.r[i] - s.r[i - 1])));
  return m;
}
static bool finite_in(const Stereo& s) {
  for (size_t i = 0; i < s.size(); ++i)
    if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i]) || std::fabs(s.l[i]) > 1.0f || std::fabs(s.r[i]) > 1.0f) return false;
  return true;
}
static int subnormals(const Stereo& s) {
  int n = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    n += s.l[i] != 0.0f && std::fpclassify(s.l[i]) == FP_SUBNORMAL;
    n += s.r[i] != 0.0f && std::fpclassify(s.r[i]) == FP_SUBNORMAL;
  }
  return n;
}
// L-R difference against the mix: 0 mono, ~1 wide
static double width(const Stereo& s, size_t a, size_t b) {
  double side = 0, mid = 0;
  for (size_t i = a; i < std::min(b, s.size()); ++i) {
    double m = 0.5 * (s.l[i] + s.r[i]), d = 0.5 * (s.l[i] - s.r[i]);
    mid += m * m, side += d * d;
  }
  return mid > 0 ? std::sqrt(side / mid) : 0.0;
}
// the fundamental (Hz) of the mix over [a, a + 8192): the normalised autocorrelation's first peak near its best
static double pitch_hz(const Stereo& s, size_t a) {
  const int n = 8192, lo = 40, hi = 1200;
  std::vector<double> x(n);
  for (int i = 0; i < n; ++i) x[i] = 0.5 * (s.l[a + i] + s.r[a + i]);
  double m = 0;
  for (double v : x) m += v;
  m /= n;
  for (double& v : x) v -= m;
  std::vector<double> r(hi + 1, 0.0);
  double e0 = 0;
  for (int i = 0; i < n - hi; ++i) e0 += x[i] * x[i];
  if (e0 <= 0) return 0;
  double best = 0;
  for (int lag = lo; lag <= hi; ++lag) {
    double c = 0, e1 = 0;
    for (int i = 0; i < n - hi; ++i) c += x[i] * x[i + lag], e1 += x[i + lag] * x[i + lag];
    r[lag] = e1 > 0 ? c / std::sqrt(e0 * e1) : 0;
    best = std::max(best, r[lag]);
  }
  for (int lag = lo + 1; lag < hi; ++lag)
    if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
      // parabolic interpolation of the peak
      double y0 = r[lag - 1], y1 = r[lag], y2 = r[lag + 1], d = y0 - 2 * y1 + y2;
      double off = d != 0 ? 0.5 * (y0 - y2) / d : 0;
      return kFs / (lag + off);
    }
  return 0;
}
// the pitch of the loudest 8192-frame window in [a, a + 2 s): a cloud has gaps between its grains
static double pitch_loud(const Stereo& s, size_t a) {
  size_t best = a;
  double be = -1;
  for (size_t w = a; w + 8192 <= a + 2 * (size_t)kFs && w + 8192 <= s.size(); w += 1024) {
    double e = 0;
    for (size_t i = w; i < w + 8192; ++i) e += s.l[i] * s.l[i] + s.r[i] * s.r[i];
    if (e > be) be = e, best = w;
  }
  return pitch_hz(s, best);
}
static double note_hz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }
static double cents(double hz, double ref) { return hz > 0 ? 1200.0 * std::log2(hz / ref) : 9999.0; }

static void write_wav(const char* name, const Stereo& s) {
  std::string path = std::string(g_wavdir) + "/cloud-" + name + ".wav";
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { printf("  (no %s)\n", path.c_str()); return; }
  uint32_t frames = (uint32_t)s.size(), data = frames * 4, riff = 36 + data, fmt = 16, rate = kFs, bps = kFs * 4;
  uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
  fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  for (uint32_t i = 0; i < frames; ++i) {
    int16_t a = (int16_t)std::lrint(std::max(-1.0f, std::min(1.0f, s.l[i])) * 32767.0f);
    int16_t b = (int16_t)std::lrint(std::max(-1.0f, std::min(1.0f, s.r[i])) * 32767.0f);
    fwrite(&a, 2, 1, f); fwrite(&b, 2, 1, f);
  }
  fclose(f);
}

// the common checks of a run
static void check_clean(const char* what, const Stereo& s, const Voice& v) {
  CHECK(finite_in(s), "%s: not finite or outside +-1", what);
  CHECK(subnormals(s) == 0, "%s: %d denormal samples", what, subnormals(s));
  CHECK(v.guards_ok(), "%s: wrote outside its state or its arena part", what);
}

static size_t sec(double s) { return (size_t)(s * kFs) / kBlock * kBlock; }

int main(int argc, char** argv) {
  const char* pkg = argc > 1 ? argv[1] : "../out/cloud.a64.rmp";
  const char* native = argc > 2 ? argv[2] : "../libcloud.dylib";
  if (const char* w = getenv("WAVDIR")) g_wavdir = w;
  g_host.reserved[RMP_H_V3] = &g_h3;
  g_host2.reserved[RMP_H_V3] = &g_h3;
  rmp_info info;
  g_p = load(pkg, info);
  CHECK(g_p, "%s does not load", pkg);
  if (!g_p) return 1;
  g_p3 = (const rmp3_plugin*)g_p->reserved[RMP_P_V3];

  // ---- load
  printf("load      %s: id %s, image %u B, bss %u B, state %u B, arena %u B a voice, cpu %u permille, flags 0x%x\n", pkg,
         info.id, info.image_size, info.bss_size, g_p->state_bytes, g_p3 ? g_p3->voice_want : 0, g_p->cpu_permille, info.flags);
  CHECK(strcmp(info.id, "cloud.texture") == 0, "id %s", info.id);
  CHECK(g_p->api == 3u && g_p->render == nullptr, "API 3 only: api %u", g_p->api);
  CHECK(g_p3 && g_p3->magic == RMP3_MAGIC && g_p3->kind == RMP3_KIND_MACHINE, "rmp3_plugin");
  if (!g_p3) return 1;
  CHECK(g_p3->needs == RMP3_CAP_ARENA, "needs 0x%x", g_p3->needs);
  CHECK(g_p3->out_channels == 2 && g_p3->in_channels == 0, "channels %u/%u", g_p3->out_channels, g_p3->in_channels);
  CHECK(g_p3->states == 8 && g_p3->voice_min == g_p3->voice_want, "states %u", g_p3->states);
  CHECK(g_p3->voice_want < 1100000u, "the arena part %u B", g_p3->voice_want);
  CHECK((uint64_t)g_p3->states * g_p3->voice_want <= 16u << 20, "8 states over a quarter of a 64 MB pool");
  CHECK(g_p->state_bytes <= 65536u, "state %u over the Pi's 64 KB", g_p->state_bytes);
  CHECK(rmp_hf_pitch(info.flags) != RMP_PITCH_NONE && rmp_hf_max_voices(info.flags) <= 1, "flags 0x%x", info.flags);
  CHECK(info.bss_size <= 64, "globals %u B (Beads' random state only)", info.bss_size);

  // ---- held: the defaults, a held note (C3), the arena junk at attach never heard
  {
    Voice v;
    Stereo s;
    v.params(kDef);
    v.note_on(48.0f);
    run(v, s, 0, 6.0);
    v.note_off();
    run(v, s, s.size(), 3.0);
    write_wav("held", s);
    check_clean("held", s, v);
    double r = rms2(s, sec(1), sec(6)), w = width(s, sec(2), sec(6));
    printf("held      defaults, C3 6 s: rms %.3f, peak %.3f, width (side/mid) %.2f, after the release %.4f\n", r,
           peak(s, 0, s.size()), w, rms2(s, sec(8), sec(9)));
    CHECK(r > 0.05 && r < 0.7, "held: rms %.3f", r);
    CHECK(w > 0.1, "held: no stereo spread (%.3f)", w);
    // wet only, a fresh voice: the junk must not come out while the buffer is cleared
    Voice j(1, 0x7F);
    Stereo t;
    float wet[16];
    memcpy(wet, kDef, sizeof wet);
    wet[MIX] = 127, wet[RVB] = 0, wet[DENS] = 127;
    j.params(wet);
    j.note_on(48.0f);
    run(j, t, 0, 0.5);
    float early = peak(t, 0, sec(0.006));
    printf("held      wet only on an arena of junk: peak %.5f in the first 6 ms (the clear), %.3f by 0.5 s\n", early,
           peak(t, 0, t.size()));
    CHECK(early < 1e-3f, "junk heard while the buffer clears: peak %.4f", early);
    CHECK(rms2(t, sec(0.2), sec(0.5)) > 0.01, "the wet cloud does not start after the clear");
    check_clean("held wet", t, j);
  }

  // ---- freeze: a note, then FRZ on and the note released: the cloud plays on; without FRZ it dies to zeros
  {
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[FDBK] = 0, prm[RVB] = 0, prm[MIX] = 127;
    Voice v;
    Stereo s;
    v.params(prm);
    v.note_on(55.0f);
    run(v, s, 0, 3.0);
    prm[FRZ] = 127;
    v.params(prm);
    v.note_off();
    run(v, s, s.size(), 9.0);
    write_wav("freeze", s);
    check_clean("freeze", s, v);
    double before = rms2(s, sec(2), sec(3)), late = rms2(s, sec(10), sec(12));
    printf("freeze    FRZ on at 3 s, the note off: rms %.3f before, %.3f at 10-12 s\n", before, late);
    CHECK(late > 0.3 * before, "frozen cloud died: %.4f vs %.4f", late, before);

    Voice u;
    Stereo q;
    prm[FRZ] = 0;
    u.params(prm);
    u.note_on(55.0f);
    run(u, q, 0, 3.0);
    u.note_off();
    run(u, q, q.size(), 14.0);
    check_clean("unfrozen", q, u);
    double tail = rms2(q, sec(15), sec(17));
    int zeros = 0;
    for (size_t i = sec(16); i < q.size(); ++i) zeros += q.l[i] == 0.0f && q.r[i] == 0.0f;
    printf("freeze    without FRZ: rms %.6f at 15-17 s, %.0f %% exact zeros in the last second\n", tail,
           100.0 * zeros / (double)(q.size() - sec(16)));
    CHECK(tail < 1e-4, "without FRZ the cloud does not die away: %.6f", tail);
  }

  // ---- feedback: FDBK 127, dense, each quality, the delay mode, 20 s each
  {
    Stereo all;
    for (int mode = 0; mode < 6; ++mode) {
      float prm[16];
      memcpy(prm, kDef, sizeof prm);
      prm[FDBK] = 127, prm[DENS] = 127, prm[SIZE] = 90, prm[MIX] = 127, prm[RVB] = 127;
      if (mode < 4) prm[QUAL] = qual_value(mode);
      if (mode == 4) prm[SIZE] = 127;                 // the delay mode
      if (mode == 5) prm[SIZE] = 20, prm[PTCH] = 127;  // short reversed grains two octaves up
      Voice v;
      Stereo s;
      v.params(prm);
      v.note_on(43.0f, 1.0f);
      run(v, s, 0, 2.0);
      v.note_off();
      run(v, s, s.size(), 18.0);
      char what[48];
      snprintf(what, sizeof what, "feedback mode %d", mode);
      check_clean(what, s, v);
      double r = rms2(s, sec(16), sec(20)), dc = std::fabs(0.5 * (mean(s.l, sec(16), sec(20)) + mean(s.r, sec(16), sec(20))));
      float pk = peak(s, sec(10), sec(20));
      static const char* kMode[6] = {"LOFI", "TAPE", "VINTAGE", "DIGITAL", "DELAY", "REVERSED +24"};
      printf("feedback  FDBK 127 %-12s: rms %.3f at 16-20 s, peak %.3f, DC %.4f\n", kMode[mode], r, pk, dc);
      CHECK(r < 0.75, "%s: runaway rms %.3f", what, r);
      CHECK(dc < 0.02, "%s: DC %.4f", what, dc);
      all.l.insert(all.l.end(), s.l.begin() + sec(14), s.l.end());
      all.r.insert(all.r.end(), s.r.begin() + sec(14), s.r.end());
    }
    write_wav("feedback", all);
  }

  // ---- note: C3 then C4: the dry pitch at once (MIX 0), the cloud's pitch within seconds (MIX 127)
  {
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[WAVE] = 0, prm[BANK] = 0, prm[TRND] = 64, prm[PRND] = 64, prm[SPRD] = 0, prm[RVB] = 0, prm[FDBK] = 0;
    prm[MIX] = 0;
    Voice d;
    Stereo s;
    d.params(prm);
    d.note_on(48.0f);
    run(d, s, 0, 1.0);
    d.note_on(60.0f);
    run(d, s, s.size(), 1.0);
    double f1 = pitch_hz(s, sec(0.5)), f2 = pitch_hz(s, sec(1.3));
    printf("note      dry: C3 %.2f Hz (%.1f cents), C4 %.2f Hz (%.1f cents)\n", f1, cents(f1, note_hz(48)), f2,
           cents(f2, note_hz(60)));
    CHECK(std::fabs(cents(f1, note_hz(48))) < 10, "dry C3 at %.2f Hz", f1);
    CHECK(std::fabs(cents(f2, note_hz(60))) < 10, "dry C4 at %.2f Hz", f2);
    check_clean("note dry", s, d);

    prm[MIX] = 127, prm[TIME] = 0, prm[SIZE] = 90, prm[DENS] = 105;   // a dense cloud of long grains
    Voice w;
    Stereo t;
    w.params(prm);
    w.note_on(48.0f);
    run(w, t, 0, 4.0);
    w.note_on(60.0f);
    run(w, t, t.size(), 4.0);
    write_wav("note", t);
    double g1 = pitch_loud(t, sec(2.0)), g2 = pitch_loud(t, sec(6.0));
    printf("note      cloud: C3 %.2f Hz (%.1f cents), after the change to C4 %.2f Hz (%.1f cents)\n", g1,
           cents(g1, note_hz(48)), g2, cents(g2, note_hz(60)));
    CHECK(std::fabs(cents(g1, note_hz(48))) < 25, "cloud C3 at %.2f Hz", g1);
    CHECK(std::fabs(cents(g2, note_hz(60))) < 25, "cloud C4 at %.2f Hz", g2);
    check_clean("note cloud", t, w);
    // PTCH +7 (a fifth): the cloud a fifth up
    prm[PTCH] = 107;   // inside the 7-semitone plateau of Beads' pitch law (105.8..109.4)
    Voice x;
    Stereo u;
    x.params(prm);
    x.note_on(48.0f);
    run(x, u, 0, 4.0);
    double g3 = pitch_loud(u, sec(2.0));
    printf("note      cloud, PTCH on +7: %.2f Hz (%.1f cents from G3)\n", g3, cents(g3, note_hz(55)));
    CHECK(std::fabs(cents(g3, note_hz(55))) < 25, "PTCH +7 gives %.2f Hz", g3);
  }

  // ---- synth: the SYNTH engine, the grains at the note
  {
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[ENG] = 127;
    Voice v;
    Stereo s;
    v.params(prm);
    v.note_on(48.0f);
    run(v, s, 0, 4.0);
    v.note_off();
    run(v, s, s.size(), 2.0);
    write_wav("synth", s);
    check_clean("synth", s, v);
    double r = rms2(s, sec(1), sec(4));
    printf("synth     defaults: rms %.3f, width %.2f, after the release %.4f\n", r, width(s, sec(1), sec(4)),
           rms2(s, sec(5.5), sec(6)));
    CHECK(r > 0.03, "synth: rms %.4f", r);
    prm[WAVE] = 0, prm[TIME] = 0, prm[BANK] = 0, prm[TRND] = 64, prm[PRND] = 64, prm[MIX] = 127, prm[RVB] = 0;
    prm[SPRD] = 0;
    Voice w;
    Stereo t;
    w.params(prm);
    w.note_on(57.0f);
    run(w, t, 0, 2.0);
    double f = pitch_loud(t, sec(1.0));
    printf("synth     grains only, A3: %.2f Hz (%.1f cents)\n", f, cents(f, note_hz(57)));
    CHECK(std::fabs(cents(f, note_hz(57))) < 25, "synth A3 at %.2f Hz", f);
  }

  // ---- quality: QUAL changed mid-note, each to each: no burst of the old bytes, no click past the playing's own
  {
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[MIX] = 127, prm[DENS] = 110;
    Voice v;
    Stereo s;
    v.params(prm);
    v.note_on(50.0f);
    run(v, s, 0, 2.0);
    const int order[] = {0, 2, 1, 3, 1, 0, 3, 2, 3};
    for (int q : order) {
      size_t at = s.size();
      float before = peak(s, at - sec(0.5), at), step_before = max_step(s, at - sec(0.5), at);
      prm[QUAL] = qual_value(q);
      v.params(prm);
      run(v, s, at, 1.5);
      float after = peak(s, at, at + sec(0.1)), step_after = max_step(s, at, at + sec(0.1));
      printf("quality   -> %d: peak %.3f (before %.3f), largest step %.3f (before %.3f)\n", q, after, before,
             step_after, step_before);
      CHECK(after <= 1.25f * before + 0.02f, "a burst at the change to %d: %.3f vs %.3f", q, after, before);
      CHECK(step_after <= 1.5f * step_before + 0.02f, "a click at the change to %d: %.3f vs %.3f", q, step_after, step_before);
    }
    write_wav("quality", s);
    check_clean("quality", s, v);
    // the engine switch both ways, mid-note
    prm[ENG] = 127;
    v.params(prm);
    size_t at = s.size();
    run(v, s, at, 1.0);
    prm[ENG] = 0;
    v.params(prm);
    run(v, s, s.size(), 1.0);
    printf("quality   ENG CLOUD -> SYNTH -> CLOUD: largest step %.3f\n", max_step(s, at, s.size()));
    check_clean("engine switch", s, v);
  }

  // ---- knobs: each P alone at 0 and 127, every P at 0 / 127, random each block
  {
    int runs = 0;
    for (int k = 0; k < 16; ++k)
      for (float val : {0.0f, 127.0f}) {
        float prm[16];
        memcpy(prm, kDef, sizeof prm);
        prm[k] = val;
        Voice v;
        Stereo s;
        v.params(prm);
        v.note_on(52.0f);
        run(v, s, 0, 3.0);
        v.note_off();
        run(v, s, s.size(), 1.0);
        char what[32];
        snprintf(what, sizeof what, "%s at %.0f", kShort[k], val);
        check_clean(what, s, v);
        // MIX 0 is the dry oscillator; FRZ ON from the start freezes an empty buffer (only the dry signal)
        CHECK(rms2(s, sec(1), sec(3)) > 0.005, "%s: silent (rms %.5f)", what, rms2(s, sec(1), sec(3)));
        ++runs;
      }
    for (float val : {0.0f, 127.0f}) {
      float prm[16];
      for (float& x : prm) x = val;
      Voice v;
      Stereo s;
      v.params(prm);
      v.note_on(52.0f);
      run(v, s, 0, 3.0);
      check_clean(val == 0.0f ? "every P at 0" : "every P at 127", s, v);
      printf("knobs     every P at %3.0f: rms %.4f\n", val, rms2(s, sec(1), sec(3)));
      ++runs;
    }
    {
      Voice v;
      Stereo s;
      float prm[16];
      uint32_t rs = 12345;
      run(v, s, 0, 6.0, [&](size_t i) {
        for (int j = 0; j < 16; ++j) prm[j] = (float)(hrand(&rs) % 128) + (float)(hrand(&rs) % 128) / 128.0f;
        v.params(prm);
        if (i % 4800 == 0) v.note_on((float)(hrand(&rs) % 128), (float)(hrand(&rs) % 128) / 127.0f);
        if (i % 4800 == 2400) v.note_off();
      });
      check_clean("random", s, v);
      printf("knobs     random each block, a note every 100 ms: rms %.3f\n", rms2(s, 0, s.size()));
      write_wav("random", s);
    }
    printf("knobs     %d runs: each P alone at 0 and 127, all at 0, all at 127\n", runs);
  }

  // ---- routes: mono, n 1..16, no arena, too small an arena
  {
    Voice v;
    v.params(kDef);
    v.note_on(48.0f);
    std::vector<float> m(sec(2.0));
    for (size_t i = 0; i < m.size(); i += kBlock) v.block(&m[i], nullptr);
    double r = rms(m, sec(1), sec(2));
    bool ok = true;
    for (float x : m) ok &= std::isfinite(x) && std::fabs(x) <= 1.0f;
    printf("routes    mono route (TRK): rms %.3f\n", r);
    CHECK(ok && r > 0.03, "mono route: rms %.4f", r);
    // n 1..16 in every quality, against the same voice at n 12: the output's sanitiser hides a NaN inside (it
    // writes 0 and resets the state), so compare the level and count exact zeros instead (the tape qualities'
    // 2:1 converters need even counts: an odd n used to leave them reading the stack)
    for (int q = 0; q < 4; ++q) {
      float prm[16];
      memcpy(prm, kDef, sizeof prm);
      prm[QUAL] = qual_value(q), prm[MIX] = 127, prm[DENS] = 110, prm[TIME] = 10;
      // n: 12 (the reference), 1, 11 and 16 throughout, then every count in turn (1, 8, 15, 6, 13, ...)
      const int kModes[5] = {12, 1, 11, 16, 0};
      double ref = 0;
      for (int mi = 0; mi < 5; ++mi) {
        Voice w(1, (uint8_t)(0x30 + q));
        w.params(prm);
        w.note_on(48.0f);
        std::vector<float> L, R;
        float l[16], rr[16];
        int k = 0, bad = 0;
        while (L.size() < sec(4.0)) {
          const int n = kModes[mi] ? kModes[mi] : 1 + (k++ * 7) % 16;
          w.block(l, rr, n);
          for (int i = 0; i < n; ++i) {
            bad += !std::isfinite(l[i]) || !std::isfinite(rr[i]);
            L.push_back(l[i]), R.push_back(rr[i]);
          }
        }
        const double a = rms(L, sec(1), sec(4)), b = rms(R, sec(1), sec(4)), lev = std::sqrt(0.5 * (a * a + b * b));
        size_t z = 0;
        for (size_t i = sec(1); i < sec(4); ++i) z += L[i] == 0.0f && R[i] == 0.0f;
        const double zeros = double(z) / double(sec(3));
        if (mi == 0) ref = lev;
        printf("routes    QUAL %d, n %-5s: rms %.3f, exact zeros %.1f %%\n", q,
               kModes[mi] ? std::to_string(kModes[mi]).c_str() : "1..16", lev, zeros * 100);
        CHECK(bad == 0, "QUAL %d n %d: %d bad samples", q, kModes[mi], bad);
        CHECK(lev > 0.6 * ref && lev < 1.6 * ref, "QUAL %d n %d: rms %.3f against %.3f at n 12", q, kModes[mi], lev, ref);
        CHECK(zeros < 0.05, "QUAL %d n %d: %.0f %% exact zeros", q, kModes[mi], zeros * 100);
        CHECK(w.guards_ok(), "QUAL %d n %d: wrote outside", q, kModes[mi]);
      }
    }
    for (int mode : {0, 2}) {
      Voice z(mode);
      Stereo s;
      z.params(kDef);
      z.note_on(48.0f);
      run(z, s, 0, 0.5);
      CHECK(peak(s, 0, s.size()) == 0.0f, "arena mode %d: not silent", mode);
      CHECK(z.guards_ok(), "arena mode %d: wrote outside", mode);
    }
    printf("routes    no arena and half an arena: silent, nothing written outside\n");
  }

  // ---- calls: the control calls' time (API 3: three over 500 us in a row, or one over 2 ms, mute the slot)
  {
    double attach = 0, prm = 0, on = 0, off = 0;
    for (int rep = 0; rep < 20; ++rep) {
      Voice v(1, (uint8_t)rep);
      attach = std::max(attach, v.attach_ns);
      float p[16];
      memcpy(p, kDef, sizeof p);
      for (int i = 0; i < 200; ++i) {
        p[i % 16] = (float)(i % 128);
        double t0 = now_ns();
        v.params(p);
        prm = std::max(prm, now_ns() - t0);
        t0 = now_ns();
        v.note_on((float)(36 + i % 48), 0.8f);
        on = std::max(on, now_ns() - t0);
        t0 = now_ns();
        v.note_off();
        off = std::max(off, now_ns() - t0);
        float l[kBlock], r[kBlock];
        v.block(l, r);
      }
    }
    printf("calls     slowest of 20 voices x 200: voice_init + voice_attach %.1f us, params %.2f us, note_on %.2f us, "
           "note_off %.2f us\n", attach / 1e3, prm / 1e3, on / 1e3, off / 1e3);
    // the Pi's limits over the slowest Mac-to-Pi 4 ratio seen (x12, the AMP insert): an attach (once a voice) under
    // 2 ms there, the calls a step makes under 500 us (the slowest of 4000 includes the Mac's own scheduling jitter)
    CHECK(attach < 2000e3 / 12, "voice_init + voice_attach %.0f us on the Mac", attach / 1e3);
    CHECK(prm < 500e3 / 12 && on < 500e3 / 12 && off < 500e3 / 12, "a control call over %.0f us on the Mac", 500.0 / 12);
  }

  // ---- cpu: ns per 12-frame render call, after the clear; mean and worst of 20000 calls
  struct Load { const char* what; float p[16]; };
  std::vector<Load> loads;
  auto add = [&](const char* what, std::initializer_list<std::pair<int, float>> kv) {
    Load l;
    l.what = what;
    memcpy(l.p, kDef, sizeof l.p);
    for (auto& e : kv) l.p[e.first] = e.second;
    loads.push_back(l);
  };
  add("defaults (DIGITAL)", {});
  add("dense, long grains DIGITAL", {{DENS, 127}, {SIZE, 100}, {FDBK, 100}, {RVB, 127}});
  add("dense, long grains VINTAGE", {{DENS, 127}, {SIZE, 100}, {FDBK, 100}, {RVB, 127}, {QUAL, qual_value(2)}});
  add("dense, long grains TAPE", {{DENS, 127}, {SIZE, 100}, {FDBK, 100}, {RVB, 127}, {QUAL, qual_value(1)}});
  add("dense, long grains LOFI", {{DENS, 127}, {SIZE, 100}, {FDBK, 100}, {RVB, 127}, {QUAL, qual_value(0)}});
  add("delay mode", {{SIZE, 127}, {FDBK, 100}, {RVB, 127}});
  add("SYNTH defaults", {{ENG, 127}});
  add("SYNTH dense, long grains", {{ENG, 127}, {DENS, 127}, {SIZE, 100}, {RVB, 127}});
  double worst_mean = 0, worst_call = 0;
  for (const Load& l : loads) {
    Voice v;
    v.params(l.p);
    v.note_on(48.0f);
    float a[kBlock], b[kBlock];
    for (int i = 0; i < 4000; ++i) v.block(a, b);   // past the clear, the cloud full
    double best_mean = 1e30, max_call = 0;
    for (int rep = 0; rep < 5; ++rep) {
      double t0 = now_ns();
      for (int i = 0; i < 20000; ++i) {
        double c0 = rep == 4 ? now_ns() : 0;
        v.block(a, b);
        if (rep == 4) max_call = std::max(max_call, now_ns() - c0);
      }
      best_mean = std::min(best_mean, (now_ns() - t0) / 20000);
    }
    worst_mean = std::max(worst_mean, best_mean), worst_call = std::max(worst_call, max_call);
    printf("cpu       %-28s %6.0f ns mean a call (%.2f %% of a Mac core), slowest call %6.0f ns\n", l.what, best_mean,
           best_mean / 250000.0 * 100.0, max_call);
  }
  // the blocks a clear spans (a voice's first 16 blocks, and a QUAL change's fade, clear and fade-in): the slowest
  // of each run, the median over 21 runs (one run's slowest is mostly the Mac's own scheduling)
  {
    std::vector<double> attach_max, qual_max;
    for (int rep = 0; rep < 21; ++rep) {
      Voice v(1, (uint8_t)rep);
      float prm[16];
      memcpy(prm, kDef, sizeof prm);
      prm[DENS] = 127, prm[SIZE] = 100, prm[QUAL] = qual_value(2);
      v.params(prm);
      v.note_on(48.0f);
      float a[kBlock], b[kBlock];
      double mx = 0;
      for (int i = 0; i < 16; ++i) {
        double t0 = now_ns();
        v.block(a, b);
        mx = std::max(mx, now_ns() - t0);
      }
      attach_max.push_back(mx);
      for (int i = 0; i < 2000; ++i) v.block(a, b);
      prm[QUAL] = qual_value(rep % 2 ? 0 : 1);
      v.params(prm);
      mx = 0;
      for (int i = 0; i < 60; ++i) {
        double t0 = now_ns();
        v.block(a, b);
        mx = std::max(mx, now_ns() - t0);
      }
      qual_max.push_back(mx);
    }
    std::sort(attach_max.begin(), attach_max.end());
    std::sort(qual_max.begin(), qual_max.end());
    const double am = attach_max[10], qm = qual_max[10];
    printf("cpu       the clear: slowest block after an attach %.0f ns, around a QUAL change %.0f ns (medians of 21)\n",
           am, qm);
    CHECK(am < 125e3 / 12 && qm < 125e3 / 12, "a clear block over %.1f us on the Mac (the voice budget / 12)",
          125.0 / 12);
  }
  printf("cpu       package worst mean %.0f ns = %.2f %% of a core; Pi 4 est. x4.4..x12 -> %.1f..%.1f us a block "
         "(%.0f..%.0f %% of the 125 us voice budget); Pi 5 est. Pi 4 / 2.5\n", worst_mean, worst_mean / 2500.0,
         worst_mean * 4.4 / 1e3, worst_mean * 12 / 1e3, worst_mean * 4.4 / 1250.0, worst_mean * 12 / 1250.0);
  // the native build against the starter machines' declared figures (as sophie_check)
  void* hn = dlopen(native, RTLD_NOW);
  const rmp_plugin* pn = hn ? (const rmp_plugin*)dlsym(hn, "rmp_entry") : nullptr;
  if (pn && pn->init(&g_host) == 0) {
    const rmp3_plugin* qn = (const rmp3_plugin*)pn->reserved[RMP_P_V3];
    std::vector<uint8_t> st(pn->state_bytes, 0);
    void* ar = nullptr;
    posix_memalign(&ar, 4096, qn->voice_want);
    rmp3_voice vv = {nullptr, ar, 0, qn->voice_want, 1, 0, -1, RMP3_AT_VOICE};
    pn->voice_init(st.data());
    qn->voice_attach(st.data(), &vv);
    pn->params(st.data(), loads[1].p);
    pn->note_on(st.data(), 48.0f, 0.9f);
    float a[kBlock], b[kBlock];
    rmp3_block blk;
    memset(&blk, 0, sizeof blk);
    blk.size = sizeof blk, blk.n = kBlock, blk.out[0] = a, blk.out[1] = b, blk.place = RMP3_AT_VOICE, blk.transport = &g_tr;
    for (int i = 0; i < 4000; ++i) qn->render(st.data(), &blk);
    double mn = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
      double t0 = now_ns();
      for (int i = 0; i < 20000; ++i) qn->render(st.data(), &blk);
      mn = std::min(mn, (now_ns() - t0) / 20000);
    }
    std::vector<double> est;
    for (int r = 3; r < argc; ++r) {
      void* h = dlopen(argv[r], RTLD_NOW);
      const rmp_plugin* pr = h ? (const rmp_plugin*)dlsym(h, "rmp_entry") : nullptr;
      if (!pr || pr->init(pr->api == 1u ? &g_host1 : &g_host2) != 0) { printf("cpu       (%s: not loaded)\n", argv[r]); continue; }
      float rp[16];
      for (int i = 0; i < 16; ++i) rp[i] = 64.0f;
      rp[1] = 100.0f;
      std::vector<uint8_t> rs(pr->state_bytes, 0);
      pr->voice_init(rs.data());
      pr->params(rs.data(), rp);
      pr->note_on(rs.data(), 60.0f, 0.8f);
      float o[kBlock];
      for (int i = 0; i < 2000; ++i) pr->render(rs.data(), o, kBlock);
      double mr = 1e30;
      for (int rep = 0; rep < 5; ++rep) {
        double t0 = now_ns();
        for (int i = 0; i < 20000; ++i) pr->render(rs.data(), o, kBlock);
        mr = std::min(mr, (now_ns() - t0) / 20000);
      }
      est.push_back(pr->cpu_permille * mn / mr);
      printf("cpu       native: cloud %.0f ns (dense), %s %.0f ns (declared %u): ~%.0f permille\n", mn,
             strrchr(argv[r], '/') + 1, mr, pr->cpu_permille, est.back());
    }
    if (!est.empty()) {
      std::sort(est.begin(), est.end());
      double e = est[est.size() / 2];
      printf("cpu       estimate %.0f permille (median, dense), declared %u\n", e, g_p->cpu_permille);
      CHECK(e <= 2.0 * g_p->cpu_permille, "the estimate %.0f is over 2x the declared %u", e, g_p->cpu_permille);
    }
    free(ar);
  } else {
    printf("cpu       (%s: no native build)\n", native);
  }
  CHECK(worst_mean < 25000, "the worst mean %.0f ns a block on the Mac (x12 = %.0f us, over the Pi 4's 125 us voice budget "
        "with no room)", worst_mean, worst_mean * 12 / 1e3);
  printf(g_fail ? "cloud_check: %d FAILED\n" : "cloud_check: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
