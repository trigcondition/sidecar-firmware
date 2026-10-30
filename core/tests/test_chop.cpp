// core/tests/test_chop.cpp - CHOP (core/chop): the slice map's file (v2 written and read back, the sampler's v1 upgraded,
// newer / foreign-newer / damaged, other SFZ players read the regions), the edits (zero-crossing snap, GRID, add / delete
// / move / merge / split), TRANSIENT on synthetic drums, LAZY with simulated taps and a latency, the machine on a RAM
// buffer (which slice a trig / a CHRO pad / a sequencer note plays, the per-slice settings, CHOKE, SHUFFLE and NEIGHBOUR
// CHANCE repeatable, MORPH, STUTTER, TAPE STOP / SPIN-UP), a card sample (its trig waiting for the load, its map
// written, found again by the sample's hash in the INSTRUMENTS pool, the sampler's v1 map found and upgraded),
// RESAMPLE (the sampler writes the take, the slice plays it), no allocation and no card time on the render / control
// cores, the CPU, and the Rytm pages (PNGs to $RUI_OUT, default build/rui_chop; the text-fit check).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "chop/chop.h"
#include "fmt/fmt.h"
#include "fx/insert.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "msamp/cardq.h"
#include "msamp/sfz.h"
#include "nmach.h"
#include "pool/pool.h"
#include "pool_util.h"
#include "poly.h"
#include "rui_png.h"
#include "samp/sampler.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;

// ---- the allocation counter (the render path must not allocate)
static long g_news = 0;
void* operator new(size_t n) {
  ++g_news;
  void* p = malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) {
  ++g_news;
  void* p = malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

static constexpr int kC = kChopMachine;
static std::string g_card;
static uint64_t g_us = 1000000;
// P1..P16: SRC SLC TUNE MRPH SEED NBR VEL LEV | STUT SLEN TAPE TTIM ATK DEC DIR LOOP
static std::vector<float> P(float src, float slc = 0, float tune = 64, float mrph = 0, float seed = 0, float nbr = 0) {
  return {src, slc, tune, mrph, seed, nbr, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0};
}
static float slc_v(int k) { return (float)(2 * k + 1); }   // SLC's value for slice k (64 zones of 2)

// ---- the control core's passes with the card core's turn between them (1 ms each); card time charged by core
static void run(int ms) {
  for (int i = 0; i < ms; ++i) {
    host_card_core(3);
    for (int k = 0; k < 16 && (nm::step() || msamp::step()); ++k) {}
    samp::tick();
    host_card_core(0);
    for (int k = 0; k < 16 && cardq::service(); ++k) {}
    for (int k = 0; k < 4 && samp::poll(); ++k) {}
    for (int k = 0; k < 4 && lib::step(); ++k) {}
    host_card_core(3);
    g_us += 1000;
    host_fake_time(true, g_us);
  }
}
// the machine alone on voice vi: n frames (-1..1)
static std::vector<float> render(int vi, int n, bool steps = false) {
  std::vector<float> o((size_t)n, 0.0f);
  for (int s = 0; s < n; s += kBlock) {
    chop::render_add(vi, o.data() + s, n - s < kBlock ? n - s : kBlock);
    if (steps && (s / kBlock) % 4 == 0) run(1);
  }
  return o;
}
static void quiet(int vi) {   // the voice's notes gone (faded) before the next measured hit
  chop::silence(vi);
  render(vi, 480);
}
static double rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  if (b > x.size()) b = x.size();
  for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
  return b > a ? std::sqrt(s / (double)(b - a)) : 0;
}
static double zc(const std::vector<float>& x, size_t a, size_t b) {   // rising zero crossings a second
  return wg::zc_freq(x, a, b);
}

// ---- the test material: a synthetic break (kicks, snares, hats at known onsets) and eight tones
static constexpr int kBreakN = 96000;   // 2 s at 48 kHz
static const uint32_t kOnsets[] = {0, 12000, 24000, 36000, 48000, 60000, 66000, 72000, 84000};
static int16_t g_break[kBreakN];
static void make_break() {
  std::vector<double> x(kBreakN, 0.0);
  uint32_t rnd = 12345;
  auto noise = [&]() { rnd = rnd * 1664525u + 1013904223u; return (double)(rnd >> 8) / 8388608.0 - 1.0; };
  for (int k = 0; k < (int)(sizeof kOnsets / sizeof kOnsets[0]); ++k) {
    uint32_t a = kOnsets[k];
    int kind = k % 3;   // kick, snare, hat
    double ph = 0;
    for (uint32_t i = a; i < (uint32_t)kBreakN && i < a + 11000; ++i) {
      double t = (i - a) / 48000.0;
      double v;
      if (kind == 0) {
        ph += 2 * M_PI * (50 + 70 * std::exp(-t * 30)) / 48000.0;
        v = 0.8 * std::exp(-t * 9) * std::sin(ph);
      } else if (kind == 1) {
        v = 0.55 * std::exp(-t * 18) * noise() + 0.25 * std::exp(-t * 25) * std::sin(2 * M_PI * 200 * t);
      } else {
        v = 0.3 * std::exp(-t * 60) * noise();
      }
      uint32_t left = a + 11000 - i;   // (a fade over the last 1000 frames: no cut-off click to find)
      x[i] += left < 1000 ? v * left / 1000.0 : v;
    }
  }
  for (int i = 0; i < kBreakN; ++i) x[i] += 0.0005 * noise();   // a floor (-66 dBFS)
  for (int i = 0; i < kBreakN; ++i) {
    double v = x[i] > 0.99 ? 0.99 : x[i] < -0.99 ? -0.99 : x[i];
    g_break[i] = (int16_t)std::lround(v * 32767);
  }
}
// eight 0.25 s tones (slice k at 200 + 100 k Hz, 0.5 amplitude) and a ramp for REVERSE
static constexpr int kTonesN = 8 * 12000;
static int16_t g_tones[kTonesN];
static void make_tones() {
  for (int i = 0; i < kTonesN; ++i) {
    int k = i / 12000;
    g_tones[i] = (int16_t)std::lround(0.5 * 32767 * std::sin(2 * M_PI * (200.0 + 100.0 * k) * (i % 12000) / 48000.0));
  }
}
static double hz_of_slice(int k) { return 200.0 + 100.0 * k; }

// ---- 1. the file
static void test_format() {
  chop::Map m;
  chop::map_clear(&m);
  m.frames = 192000, m.rate = 48000, m.ch = 2, m.has_sha = true, m.size = 768044;
  for (int i = 0; i < 32; ++i) m.sha[i] = (uint8_t)(i * 7 + 1);
  snprintf(m.sample, sizeof m.sample, "/Samples/LIVE/amen break.wav");
  snprintf(m.name, sizeof m.name, "AMEN BREAK");
  m.n = 4;
  for (int k = 0; k < 4; ++k) chop::slice_defaults(&m.s[k], k * 48000, (k + 1) * 48000);
  chop::Slice& s1 = m.s[1];
  s1.pitch = -7, s1.fine = 33, s1.gain = -3, s1.rev = 1, s1.atk_ms = 12, s1.dec_ms = 1500, s1.loop = chop::kLoopPing;
  s1.choke = 3, s1.nudge = -240, s1.stut = 6, s1.stut_len = 12, s1.stut_dec = 40, s1.tape = chop::kTapeStop;
  s1.tape_ms = 750, s1.nbr = 25;
  m.nalt = 1;
  snprintf(m.alt[0], sizeof m.alt[0], "/Samples/LIVE/AMEN_BREAK-S3.wav");
  m.s[2].alt = 0;
  static char text[65536];
  int n = chop::write(m, text, sizeof text);
  CHECK(n > 0 && chop::version_of(text, (uint32_t)n) == 2);
  chop::Map r;
  char why[96];
  CHECK(chop::read(text, (uint32_t)n, &r, why, sizeof why, ""));
  CHECK(r.n == 4 && r.frames == 192000 && r.rate == 48000 && r.ch == 2 && r.has_sha && !memcmp(r.sha, m.sha, 32));
  CHECK(r.size == 768044 && !strcmp(r.sample, m.sample) && !strcmp(r.name, "AMEN BREAK") && r.nalt == 1);
  CHECK(!memcmp(&r.s[1], &m.s[1], sizeof(chop::Slice)) && r.s[2].alt == 0 && !strcmp(r.alt[0], m.alt[0]));
  CHECK(r.s[2].start == 96000 && r.s[2].end == 144000 && r.s[3].start == 144000 && r.s[3].end == 192000);
  // the same bytes again (the writer is canonical)
  static char text2[65536];
  CHECK(chop::write(r, text2, sizeof text2) == n && !memcmp(text, text2, (size_t)n));
  // other players read the regions: msamp's SFZ parser (MULTISAMPLE, the INSTRUMENTS pool's member scan)
  static msamp::Region regs[16];
  static char smp[8][msamp::kPath];
  msamp::Desc d = {};
  d.regions = regs, d.max_regions = 16, d.samples = smp, d.max_samples = 8;
  CHECK(msamp::parse(text, n, "/CHOP/LIVE", &d) && d.nregions == 4 && d.nsamples == 2);
  CHECK(!strcmp(smp[0], "/Samples/LIVE/amen break.wav") && !strcmp(smp[1], m.alt[0]));
  CHECK(regs[0].offset == 0 && regs[0].end == 47999 && regs[1].offset == 48000 && regs[1].end == 95999 && regs[1].transpose == -7);
  CHECK(regs[0].lokey == 36 && regs[3].lokey == 39 && regs[2].offset == 0);
  // the registry: v2 ok, the sampler's v1 upgraded (the golden files: test_fmt), newer refused, min=2 read, damaged
  const fmt::Format* f = fmt::find("slicemap");
  CHECK(f && f->current == 2 && f->oldest == 1 && f->nchain == 1);
  static uint8_t out[65536];
  fmt::Buf ob = {out, 0, sizeof out};
  fmt::Info info;
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)text, (uint32_t)n}, &ob, &info) == fmt::kOk && ob.n == (uint32_t)n);
  const char* v1 = "// Sidecar sample T: 3 slices (grid), 48 kHz; key 36 (C1) is the first\n<global> loop_mode=one_shot\n"
                   "<region> sample=T.wav key=36 pitch_keycenter=36 offset=0 end=999\n"
                   "<region> sample=T.wav key=37 pitch_keycenter=37 offset=1000 end=1999\n"
                   "<region> sample=T.wav key=38 pitch_keycenter=38 offset=2000 end=2999\n";
  ob = {out, 0, sizeof out};
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)v1, (uint32_t)strlen(v1)}, &ob, &info) == fmt::kUpgraded && info.version == 1);
  CHECK(chop::read((const char*)out, ob.n, &r, why, sizeof why, "/Samples/LIVE") && r.n == 3 && r.s[2].start == 2000 &&
        r.s[2].end == 3000 && !strcmp(r.sample, "/Samples/LIVE/T.wav"));
  std::string newer = std::string("// sidecar slicemap 3\n") + (text + strlen("// sidecar slicemap 2\n"));
  ob = {out, 0, sizeof out};
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)newer.data(), (uint32_t)newer.size()}, &ob, &info) == fmt::kNewer && info.locked);
  std::string foreign = std::string("// sidecar slicemap 3 min=2\n") + (text + strlen("// sidecar slicemap 2\n")) +
                        "<region> key=40 offset=150000 end=150999 sc_future=1\n";   // (out of order: the reader refuses)
  ob = {out, 0, sizeof out};
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)foreign.data(), (uint32_t)foreign.size()}, &ob, &info) != fmt::kOk);
  foreign = std::string("// sidecar slicemap 3 min=2\n") + (text + strlen("// sidecar slicemap 2\n"));
  foreign.insert(foreign.find("<region> key=37"), "<region> key=99 offset=50 sc_future=1 end=99\n");   // ignored opcode, but overlapping:
  ob = {out, 0, sizeof out};
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)foreign.data(), (uint32_t)foreign.size()}, &ob, &info) != fmt::kOk);
  foreign = std::string("// sidecar slicemap 3 min=2\n") + (text + strlen("// sidecar slicemap 2\n")) + "// a later opcode: sc_x=1\n";
  ob = {out, 0, sizeof out};
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)foreign.data(), (uint32_t)foreign.size()}, &ob, &info) == fmt::kForeignNewer &&
        info.read_only);
  std::string bad = text;
  bad[bad.find("offset=48000")] = '#';
  ob = {out, 0, sizeof out};
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)bad.data(), (uint32_t)bad.size()}, &ob, &info) == fmt::kUnreadable && info.locked);
  std::string nul(text, (size_t)n);
  nul[100] = 0;
  CHECK(fmt::load(*f, fmt::Bytes{(const uint8_t*)nul.data(), (uint32_t)nul.size()}, &ob, &info) == fmt::kUnreadable);
  CHECK(!chop::read("hello", 5, &r, why, sizeof why) && !chop::read("<region> end=-1\n", 16, &r, why, sizeof why));
  // a quick mutation pass (the registry's fuzz target covers every format at length): no crash, a verdict each
  uint32_t seed = 99;
  for (int it = 0; it < 4000; ++it) {
    std::string x(text, (size_t)n);
    int muts = 1 + it % 5;
    for (int k = 0; k < muts; ++k) {
      seed = seed * 1664525u + 1013904223u;
      size_t at = (seed >> 8) % x.size();
      int op = (seed >> 4) & 3;
      if (op == 0) x[at] = (char)(seed >> 16);
      else if (op == 1) x.erase(at, 1 + (seed >> 20) % 16);
      else if (op == 2) x.insert(at, std::string(1 + (seed >> 24) % 8, "0123456789=<> /\n"[(seed >> 12) & 15]));
      else x.resize(at);
      if (x.empty()) x = "x";
    }
    ob = {out, 0, sizeof out};
    fmt::Status s = fmt::load(*f, fmt::Bytes{(const uint8_t*)x.data(), (uint32_t)x.size()}, &ob, &info);
    CHECK(s <= fmt::kUnreadable);
    if (s == fmt::kOk) CHECK(chop::read((const char*)out, ob.n, &r, why, sizeof why) && r.n <= chop::kMaxSlices);
  }
  printf("test_chop: format PASS (v2 %d bytes, 4 slices)\n", n);
}

// ---- 2. the edits
static void test_edits() {
  // the snap: a sine's zero crossings every 120 frames (200 Hz); a point 30 frames off one goes to it, a point with none
  // within reach stays
  static int16_t sine[48000];
  for (int i = 0; i < 48000; ++i) sine[i] = (int16_t)std::lround(10000 * std::sin(2 * M_PI * 200 * (i + 0.5) / 48000.0));
  CHECK(chop::snap(sine, 1, 48000, 1230) == 1200 || chop::snap(sine, 1, 48000, 1230) == 1199);
  uint32_t z = chop::snap(sine, 1, 48000, 4855);
  CHECK(z >= 4799 && z <= 4800 + 1 || (z >= 4919 && z <= 4920));
  CHECK((sine[z] >= 0) != (sine[z - 1] >= 0) || std::abs(sine[z]) < 400);
  static int16_t dc[4800];
  for (int i = 0; i < 4800; ++i) dc[i] = 5000;
  CHECK(chop::snap(dc, 1, 4800, 2000) == 2000);   // no crossing: unmoved
  chop::Map m;
  chop::map_clear(&m);
  m.frames = 48000, m.rate = 48000, m.ch = 1;
  chop::grid(&m, 8, sine, 1);
  CHECK(m.n == 8 && m.s[0].start == 0 && m.s[7].end == 48000);
  for (int k = 1; k < 8; ++k) {
    CHECK(m.s[k - 1].end == m.s[k].start);
    CHECK(std::abs((int)m.s[k].start - k * 6000) <= 96);   // snapped within 2 ms
    int a = sine[m.s[k].start], b = sine[m.s[k].start - 1];
    CHECK((a >= 0) != (b >= 0) || std::abs(a) < 400);
  }
  // add / split / delete / merge / move
  chop::map_clear(&m);
  m.frames = 48000, m.rate = 48000, m.ch = 1;
  chop::grid(&m, 2, nullptr, 1);
  CHECK(m.n == 2 && m.s[1].start == 24000);
  CHECK(chop::add_marker(&m, 6000) && m.n == 3 && m.s[0].end == 6000 && m.s[1].start == 6000 && m.s[1].end == 24000);
  CHECK(!chop::add_marker(&m, 6010));   // a slice shorter than kMinSlice: refused
  m.s[1].pitch = 5;
  CHECK(chop::split(&m, 1, 15000) && m.n == 4 && m.s[2].start == 15000 && m.s[2].pitch == 5 && m.s[1].end == 15000);
  CHECK(chop::del(&m, 2) && m.n == 3 && m.s[1].end == 15000 && m.s[2].start == 24000);   // a gap 15000..24000
  CHECK(chop::slice_at(m, 20000) < 0 && chop::add_marker(&m, 20000) && m.n == 4 && m.s[2].start == 20000 && m.s[2].end == 24000);
  CHECK(chop::merge(&m, 1) && m.n == 3 && m.s[1].start == 6000 && m.s[1].end == 24000);
  CHECK(chop::move_start(&m, 1, 3000) && m.s[1].start == 3000 && m.s[0].end == 3000);   // the touching neighbour follows
  CHECK(chop::move_start(&m, 1, 10) && m.s[1].start == chop::kMinSlice && m.s[0].end == chop::kMinSlice);   // clamped
  CHECK(chop::move_end(&m, 2, 99999) && m.s[2].end == 48000);
  CHECK(chop::move_end(&m, 1, 30000) && m.s[1].end == 30000 && m.s[2].start == 30000);
  CHECK(!chop::merge(&m, 2) && !chop::del(&m, 7) && !chop::split(&m, 0, 10));
  for (int k = 0; k < chop::kMaxSlices + 4; ++k) chop::add_marker(&m, 31000 + k * 200);
  CHECK(m.n == chop::kMaxSlices && !chop::add_marker(&m, 47000));   // 64 at most
  // NUDGE: the start moved, the end kept, inside the sample
  chop::map_clear(&m);
  m.frames = 48000;
  chop::grid(&m, 4, nullptr, 1);
  m.s[1].nudge = -500;
  uint32_t a, b;
  chop::slice_span(m, 1, &a, &b);
  CHECK(a == 11500 && b == 24000);
  m.s[0].nudge = -500;
  chop::slice_span(m, 0, &a, &b);
  CHECK(a == 0);
  printf("test_chop: edits PASS\n");
}

// ---- 3. TRANSIENT on the synthetic break
static void test_onsets() {
  chop::Map m;
  chop::map_clear(&m);
  m.frames = kBreakN, m.rate = 48000, m.ch = 1;
  chop::Onsets o;
  chop::onsets_begin(&o, 64);
  for (uint32_t at = 0; at < (uint32_t)kBreakN; at += 7000) chop::onsets_feed(&o, g_break, 1, kBreakN, at + 7000);   // in pieces
  chop::onsets_apply(o, &m, g_break, 1);
  int want = (int)(sizeof kOnsets / sizeof kOnsets[0]);
  printf("test_chop: TRANSIENT (sens 64): %d slices, want %d:", m.n, want);
  for (int k = 0; k < m.n; ++k) printf(" %u", m.s[k].start);
  printf("\n");
  CHECK(m.n == want);
  for (int k = 0; k < want; ++k) CHECK(std::abs((int)m.s[k].start - (int)kOnsets[k]) <= 144);   // within 3 ms
  // the least sensitive: the loud hits only (kicks and snares, no hats); the most: no hit split twice
  chop::onsets_begin(&o, 0);
  chop::onsets_feed(&o, g_break, 1, kBreakN, kBreakN);
  chop::onsets_apply(o, &m, g_break, 1);
  int low = m.n;
  chop::onsets_begin(&o, 127);
  chop::onsets_feed(&o, g_break, 1, kBreakN, kBreakN);
  chop::onsets_apply(o, &m, g_break, 1);
  printf("test_chop: TRANSIENT sens 0: %d slices, sens 127: %d\n", low, m.n);
  CHECK(low >= 2 && low <= want && m.n >= want && m.n <= want + 3);
  // silence and a steady tone: nothing after the start
  static int16_t quiet[48000];
  memset(quiet, 0, sizeof quiet);
  chop::onsets_begin(&o, 127);
  chop::onsets_feed(&o, quiet, 1, 48000, 48000);
  CHECK(o.n == 1);
  printf("test_chop: onsets PASS\n");
}

// ---- 4. LAZY with simulated taps: each tap arrives `lat` frames after the onset the player heard, with jitter
static void test_lazy_pure() {
  chop::Lazy l;
  chop::lazy_begin(&l);
  const uint32_t lat = 337;
  int jit[] = {0, 40, -30, 70, -60, 25, 10, -45, 55};
  for (int k = 1; k < (int)(sizeof kOnsets / sizeof kOnsets[0]); ++k)
    CHECK(chop::lazy_tap(&l, kOnsets[k] + lat + jit[k], lat, g_break, 1, kBreakN) >= 0);
  CHECK(l.n == 8);
  for (int k = 1; k < 9; ++k) {   // each marker within the jitter + 2 ms of its onset, on a zero crossing or the onset
    int d = (int)l.at[k - 1] - (int)kOnsets[k];
    CHECK(std::abs(d) <= std::abs(jit[k]) + 96);
  }
  CHECK(chop::lazy_tap(&l, kOnsets[8] + lat + 20, lat, g_break, 1, kBreakN) < 0 && l.refused == 1);   // a double tap
  CHECK(chop::lazy_undo(&l) && l.n == 7);
  chop::Map m;
  chop::map_clear(&m);
  m.frames = kBreakN;
  chop::lazy_apply(l, &m);
  CHECK(m.n == 7 && m.s[0].start == l.at[0] && m.s[6].end == kBreakN);
  chop::lazy_begin(&l);   // 64 at most, then refused with a count
  for (int k = 0; k < 70; ++k) chop::lazy_tap(&l, 1000 + (uint32_t)k * 1000, 0, nullptr, 1, kBreakN);
  CHECK(l.n == chop::kMaxSlices && l.refused == 6);
  printf("test_chop: LAZY (pure) PASS\n");
}

// ---- 5. the machine on a RAM buffer
static void fresh_voice(int vi, const int16_t* pcm, uint32_t n, std::vector<float> p) {
  chop::silence(vi);
  render(vi, 960);
  wg::params(kC, vi, p);
  CHECK(v[vi].cp);
  CHECK(chop::use_buffer(vi, pcm, n, 1, 48000));
  run(30);   // (the overview)
}
static void test_engine_buffer() {
  const int vi = 1;
  fresh_voice(vi, g_tones, kTonesN, P(0));
  const chop::Map* m = chop::map_of(vi);
  CHECK(m && m->n == chop::kDefaultGrid && chop::sample_ready(vi));   // a new source: GRID 16
  CHECK(chop::edit_grid(vi, 8) && chop::slices(vi) == 8);
  for (int k = 1; k < 8; ++k) CHECK(std::abs((int)m->s[k].start - k * 12000) <= 96);
  // which slice: a Rytm trig SLC + (note - 60); a CHRO pad 60..71 from SLC; the sequencer's note is the slice
  wg::params(kC, vi, P(0, slc_v(2)));
  CHECK(chop::slice_of_hit(vi, 60, false) == 2 && chop::slice_of_hit(vi, 63, false) == 5 && chop::slice_of_hit(vi, 66, false) == 0);
  CHECK(chop::slice_of_hit(vi, 0, true) == 0 && chop::slice_of_hit(vi, 5, true) == 5 && chop::slice_of_hit(vi, 9, true) == 1);
  for (int k = 0; k < 8; ++k) {   // each slice its own tone (a trig with SLC k)
    quiet(vi);
    wg::trig(kC, vi, 60, 127, P(0, slc_v(k)));
    std::vector<float> o = render(vi, 9000);
    double f = zc(o, 1000, 9000);
    CHECK(chop::played_slice(vi) == k && std::fabs(f - hz_of_slice(k)) < 8);
  }
  quiet(vi);
  chop::live_on(vi, 3, 100, poly::kSeqCh, 3);   // the sequencer: note 3 = slice 4
  std::vector<float> o = render(vi, 9000);
  CHECK(chop::played_slice(vi) == 3 && std::fabs(zc(o, 1000, 9000) - hz_of_slice(3)) < 8);
  chop::live_off(vi, poly::kSeqCh, 3);
  wg::params(kC, vi, P(0, slc_v(0)));
  quiet(vi);
  chop::live_on(vi, 62, 100, 9, 62);   // a pad bound on channel 10 (the CHRO pads' 60..71)
  o = render(vi, 9000);
  CHECK(chop::played_slice(vi) == 2 && std::fabs(zc(o, 1000, 9000) - hz_of_slice(2)) < 8);
  chop::live_off(vi, 9, 62);
  render(vi, 12000);
  // per slice: PITCH +12 an octave up, GAIN -6 dB half, a one-shot ends at the slice's end, DECAY, ATTACK
  CHECK(chop::edit_field(vi, 1, chop::kFPitch, 12));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(1)));
  o = render(vi, 7000);
  CHECK(std::fabs(zc(o, 500, 5500) - 2 * hz_of_slice(1)) < 12);
  CHECK(rms(o, 6200, 7000) < 1e-4);   // at 2x it lasts 6000 frames
  CHECK(chop::edit_field(vi, 1, chop::kFPitch, 0));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(2)));
  double r0 = rms(render(vi, 9000), 2000, 8000);
  CHECK(chop::edit_field(vi, 2, chop::kFGain, -6));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(2)));
  double r1 = rms(render(vi, 9000), 2000, 8000);
  CHECK(std::fabs(20 * std::log10(r1 / r0) + 6) < 0.3);
  CHECK(chop::edit_field(vi, 2, chop::kFDec, 50));   // 50 ms to -60 dB
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(2)));
  o = render(vi, 9000);
  CHECK(rms(o, 2400, 2600) < 0.002 && rms(o, 0, 240) > 0.05);
  CHECK(chop::edit_field(vi, 2, chop::kFDec, 0) && chop::edit_field(vi, 2, chop::kFAtk, 100));   // 100 ms attack
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(2)));
  o = render(vi, 9000);
  CHECK(rms(o, 0, 480) < 0.3 * rms(o, 6000, 6480));
  CHECK(chop::edit_field(vi, 2, chop::kFAtk, 0) && chop::edit_field(vi, 2, chop::kFGain, 0));
  // REVERSE on a ramp buffer (slices of a rising ramp: reversed, the output falls)
  static int16_t ramp[48000];
  for (int i = 0; i < 48000; ++i) ramp[i] = (int16_t)((i % 6000) * 4);
  CHECK(chop::use_buffer(vi, ramp, 48000, 1, 48000) && chop::edit_grid(vi, 8));
  run(5);
  CHECK(chop::edit_field(vi, 0, chop::kFRev, 1));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(0)));
  o = render(vi, 6000);
  CHECK(o[1000] > o[4000] && o[4000] > o[5000]);
  CHECK(chop::edit_field(vi, 0, chop::kFRev, 0));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(0)));
  o = render(vi, 6000);
  CHECK(o[1000] < o[4000]);
  // LOOP: FWD keeps sounding past the slice's end while held, PING too; the note-off releases it (30 ms)
  CHECK(chop::use_buffer(vi, g_tones, kTonesN, 1, 48000) && chop::edit_grid(vi, 8));
  run(5);
  CHECK(chop::edit_field(vi, 4, chop::kFLoop, chop::kLoopFwd));
  chop::live_on(vi, 4, 100, poly::kSeqCh, 4);
  o = render(vi, 36000);
  CHECK(rms(o, 30000, 36000) > 0.1 && std::fabs(zc(o, 13000, 35000) - hz_of_slice(4)) < 8);
  chop::live_off(vi, poly::kSeqCh, 4);
  o = render(vi, 4800);
  CHECK(rms(o, 2400, 4800) < 1e-4);
  CHECK(chop::edit_field(vi, 4, chop::kFLoop, chop::kLoopPing));
  chop::live_on(vi, 4, 100, poly::kSeqCh, 4);
  o = render(vi, 30000);
  CHECK(rms(o, 24000, 30000) > 0.1);
  chop::live_off(vi, poly::kSeqCh, 4);
  render(vi, 4800);
  CHECK(chop::edit_field(vi, 4, chop::kFLoop, chop::kLoopOff));
  // CHOKE: slices 0 and 1 in group 1 - the second cuts the first within 2 ms; group 0 slices overlap
  CHECK(chop::edit_field(vi, 0, chop::kFChoke, 1) && chop::edit_field(vi, 1, chop::kFChoke, 1));
  uint32_t ch0 = chop::stats().chokes;
  wg::trig(kC, vi, 60, 127, P(0, slc_v(0)));
  render(vi, 1200);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(1)));
  render(vi, 240);
  CHECK(chop::stats().chokes == ch0 + 1 && chop::voice_notes(vi) == 1);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(2)));   // group 0: overlaps the group 1 note
  render(vi, 240);
  CHECK(chop::voice_notes(vi) == 2);
  render(vi, 12000);
  // SHUFFLE: the same seed the same order (a fresh map's too), seed 0 the map's own, a permutation; SEED moves it live
  uint8_t a[64], b[64], c[64];
  chop::shuffle(17, 8, a);
  chop::shuffle(17, 8, b);
  chop::shuffle(18, 8, c);
  CHECK(!memcmp(a, b, 8) && memcmp(a, c, 8));
  int seen = 0;
  for (int k = 0; k < 8; ++k) seen |= 1 << a[k];
  CHECK(seen == 0xFF);
  chop::shuffle(0, 8, a);
  for (int k = 0; k < 8; ++k) CHECK(a[k] == k);
  chop::shuffle(17, 8, a);
  CHECK(chop::edit_field(vi, 0, chop::kFChoke, 0) && chop::edit_field(vi, 1, chop::kFChoke, 0));
  for (int k = 0; k < 8; ++k) {
    quiet(vi);
    wg::trig(kC, vi, 60, 127, P(0, slc_v(k), 64, 0, 17));
    o = render(vi, 9000);
    CHECK(chop::played_slice(vi) == a[k] && std::fabs(zc(o, 1000, 9000) - hz_of_slice(a[k])) < 8);
  }
  // NEIGHBOUR CHANCE: NBR 50 %: about half the hits move to a neighbour, the same hits the same picks after a reset
  std::vector<int> run1, run2;
  for (int rep = 0; rep < 2; ++rep) {
    CHECK(chop::edit_grid(vi, 8));   // (a map edit starts the hit counts again)
    std::vector<int>& r = rep ? run2 : run1;
    for (int h = 0; h < 200; ++h) {
      wg::trig(kC, vi, 60, 127, P(0, slc_v(3), 64, 0, 0, 64));
      r.push_back(chop::played_slice(vi));
      chop::silence(vi);
      render(vi, 120);
    }
  }
  int moved = 0;
  for (int x : run1) {
    moved += x != 3;
    CHECK(x >= 2 && x <= 4);
  }
  printf("test_chop: NEIGHBOUR 50 %%: %d of 200 hits moved\n", moved);
  CHECK(run1 == run2 && moved > 70 && moved < 130);
  CHECK(chop::edit_field(vi, 0, chop::kFNbr, 100));   // the slice's own 100 %: always (slice 0 to 1)
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(0)));
  CHECK(chop::played_slice(vi) == 1);
  CHECK(chop::edit_field(vi, 0, chop::kFNbr, 0));
  render(vi, 12000);
  // MORPH: 0 the slice, fully up its right-hand neighbour, half both at equal power
  for (int mv : {0, 127, 64}) {
    quiet(vi);
    wg::trig(kC, vi, 60, 127, P(0, slc_v(2), 64, (float)mv));
    o = render(vi, 11000);
    int nfft;
    std::vector<float> seg(o.begin() + 1000, o.begin() + 9192);
    std::vector<double> sp = wg::spectrum(seg, &nfft);
    auto band = [&](double hz) {
      double e = 0;
      for (int k = 0; k < (int)sp.size(); ++k)
        if (std::fabs(k * 48000.0 / nfft - hz) < 30) e += sp[k];
      return e;
    };
    double e2 = band(hz_of_slice(2)), e3 = band(hz_of_slice(3));
    printf("test_chop: MORPH %d: slice 3 %.3g, slice 4 %.3g\n", mv, e2, e3);
    if (mv == 0) CHECK(e2 > 1000 * e3);
    else if (mv == 127) CHECK(e3 > 1000 * e2);
    else CHECK(std::fabs(10 * std::log10(e2 / e3)) < 1.0);
  }
  // STUTTER: x4 of the first 25 %: the first 3000 frames four times, then the rest (the slice lasts 21000 frames)
  CHECK(chop::use_buffer(vi, g_break, kBreakN, 1, 48000) && chop::edit_grid(vi, 8));
  run(5);
  CHECK(chop::edit_field(vi, 0, chop::kFStut, 4) && chop::edit_field(vi, 0, chop::kFSlen, 25) &&
        chop::edit_field(vi, 0, chop::kFSdec, 0));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(0)));
  o = render(vi, 24000);
  double cc = 0, n1 = 0, n2 = 0;   // repeat 2 (frames 3000..6000) against repeat 1 (0..3000), past the edge fades
  for (int i = 100; i < 2900; ++i) cc += (double)o[i] * o[3000 + i], n1 += (double)o[i] * o[i], n2 += (double)o[3000 + i] * o[3000 + i];
  double corr = cc / std::sqrt(n1 * n2 + 1e-30);
  printf("test_chop: STUTTER x4: repeat 2 against 1 correlates %.4f; length %d frames\n", corr, (int)(rms(o, 20900, 21000) > 0) );
  CHECK(corr > 0.999 && rms(o, 9000, 12000) > 0.01 && rms(o, 21100, 24000) < 1e-4);
  CHECK(chop::edit_field(vi, 0, chop::kFStut, 1));
  // TAPE STOP: the pitch falls over the end - the slice lasts longer and its last part is slower; SPIN-UP: slow first
  CHECK(chop::use_buffer(vi, g_tones, kTonesN, 1, 48000) && chop::edit_grid(vi, 8));
  run(5);
  CHECK(chop::edit_field(vi, 5, chop::kFTape, chop::kTapeStop) && chop::edit_field(vi, 5, chop::kFTms, 150));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(5)));
  o = render(vi, 24000);
  int last = 0;
  for (int i = 0; i < 24000; ++i) if (std::fabs(o[i]) > 1e-4) last = i;
  double f_early = zc(o, 1000, 6000), f_late = zc(o, last - 2500, last - 500);
  printf("test_chop: TAPE STOP: %d frames (12000 plain), %.0f Hz early, %.0f Hz at the end\n", last, f_early, f_late);
  CHECK(last > 12500 && last < 16000 && std::fabs(f_early - hz_of_slice(5)) < 8 && f_late < 0.6 * f_early);
  CHECK(chop::edit_field(vi, 5, chop::kFTape, chop::kTapeSpin));
  quiet(vi);
  wg::trig(kC, vi, 60, 127, P(0, slc_v(5)));
  o = render(vi, 24000);
  CHECK(zc(o, 200, 2400) < 0.6 * hz_of_slice(5) && std::fabs(zc(o, 9000, 11500) - hz_of_slice(5)) < 10);
  CHECK(chop::edit_field(vi, 5, chop::kFTape, chop::kTapeOff));
  // the knobs override a hit's settings (a p-lock): STUT x2 (P9), TAPE STOP (P11), DIR REV (P15) on a plain slice
  std::vector<float> pk = P(0, slc_v(6));
  pk[10] = 80;   // TAPE: STOP (4 zones: SLC OFF STOP SPIN)
  quiet(vi);
  wg::trig(kC, vi, 60, 127, pk);
  o = render(vi, 24000);
  last = 0;
  for (int i = 0; i < 24000; ++i) if (std::fabs(o[i]) > 1e-4) last = i;
  CHECK(last > 12500);
  render(vi, 6000);
  // UNDO: one level (again: the edit back)
  int n0 = chop::slices(vi);
  CHECK(chop::edit_grid(vi, 4) && chop::slices(vi) == 4 && chop::edit_undo(vi) && chop::slices(vi) == n0 && chop::edit_undo(vi) &&
        chop::slices(vi) == 4);
  chop::silence(vi);
  render(vi, 960);
  printf("test_chop: engine (RAM buffer) PASS\n");
}

// ---- 6. LAZY on the engine: the break plays, taps come at render times (the onsets + the latency + jitter)
static void test_engine_lazy() {
  const int vi = 2;
  fresh_voice(vi, g_break, kBreakN, P(0));
  chop::set_latency(400);
  CHECK(chop::lazy_arm(vi) && chop::lazy_on(vi));
  int ti = 1, jit[] = {0, 30, -20, 50, -40, 10, 0, -30, 20};
  const int nk = (int)(sizeof kOnsets / sizeof kOnsets[0]);
  for (int f = 0; f < kBreakN + 2400; f += kBlock) {
    std::vector<float> o(kBlock, 0.0f);
    chop::render_add(vi, o.data(), kBlock);
    while (ti < nk && f >= (int)kOnsets[ti] + 400 + jit[ti]) {   // the tap reaches the engine this block
      CHECK(chop::lazy_tap(vi) >= 0);
      ++ti;
    }
  }
  const chop::Lazy* l = chop::lazy_state(vi);
  CHECK(l && l->n == nk - 1);
  for (int k = 1; k < nk; ++k) CHECK(std::abs((int)l->at[k - 1] - (int)kOnsets[k]) <= std::abs(jit[k]) + kBlock + 96);
  wg::trig(kC, vi, 60, 127, P(0));   // a Rytm trig while armed: a tap too (after the sample's end: refused)
  CHECK(chop::lazy_state(vi)->n == nk - 1 && chop::stats().lazy_refused >= 1);
  CHECK(chop::lazy_undo(vi) && chop::lazy_state(vi)->n == nk - 2);
  CHECK(chop::lazy_stop(vi, true) && !chop::lazy_on(vi) && chop::slices(vi) == nk - 2);
  const chop::Map* m = chop::map_of(vi);
  CHECK(std::abs((int)m->s[0].start - (int)kOnsets[1]) <= 96 + kBlock + 30);
  CHECK(chop::lazy_arm(vi) && chop::lazy_stop(vi, false) && chop::slices(vi) == nk - 2);   // cancelled: unchanged
  chop::set_latency(-1);
  printf("test_chop: LAZY (engine) PASS, latency estimate %d frames\n", chop::latency());
}

// ---- 7. a card sample, its map on the card and in the INSTRUMENTS pool
static int C(const char* row) { return pu::code(row); }
static std::string file_of(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  fclose(f);
  return s;
}
static void test_file_map() {
  const int vi = 0;
  host_card_model(true);   // (every card operation from here on charged to the core that runs it)
  uint64_t c3 = host_card_us(3), c0 = host_card_us(0);
  CHECK(pu::put(pools::kSamples, "A001", "/Samples/LIVE/break.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A002", "/Samples/LIVE/tones.wav") == pools::kOk);
  run(50);
  // a trig before the sample is in: it waits, then plays (the map: none yet, GRID 16)
  uint32_t w0 = chop::stats().waits;
  wg::trig(kC, vi, 60, 127, P((float)C("A001"), slc_v(0)));
  CHECK(chop::stats().waits == w0 + 1);
  for (int i = 0; i < 200 && !chop::sample_ready(vi); ++i) run(1);
  CHECK(chop::sample_ready(vi) && chop::slices(vi) == chop::kDefaultGrid && chop::sample_frames(vi) == (uint32_t)kBreakN);
  CHECK(chop::voice_notes(vi) == 1);
  std::vector<float> o = render(vi, 4800, true);
  CHECK(rms(o, 0, 2400) > 0.05);
  CHECK(!chop::map_file(vi)[0] && !chop::map_dirty(vi));   // GRID 16 by default: not written until edited
  // an edit: written a second later into /CHOP/LIVE/, the file into the INSTRUMENTS pool
  CHECK(chop::edit_transient(vi, 64));
  for (int i = 0; i < 100 && chop::transient_busy(vi); ++i) run(1);
  CHECK(chop::slices(vi) == (int)(sizeof kOnsets / sizeof kOnsets[0]) && chop::map_dirty(vi));
  CHECK(chop::edit_field(vi, 3, chop::kFPitch, -5));
  run(1200);
  const char* fn = chop::map_file(vi);
  printf("test_chop: map written to %s, INSTRUMENTS row %d\n", fn, chop::map_row(vi));
  CHECK(fn[0] && !strncmp(fn, "/CHOP/LIVE/break-", 17) && strstr(fn, ".chop.sfz") && !chop::map_dirty(vi));
  std::string txt = file_of(fn);
  CHECK(txt.find("// sidecar slicemap 2") == 0 && txt.find("transpose=-5") != std::string::npos);
  CHECK(txt.find("sc_sha=") != std::string::npos && txt.find("sc_sha=-") == std::string::npos);   // (the library hashed the sample)
  CHECK(chop::map_row(vi) >= 0 && pools::at(pools::kInstr, chop::map_row(vi)).type == pools::kInstrument);
  int nm = 0;
  const pools::Member* mem = pools::members(pools::kInstr, chop::map_row(vi), &nm);
  CHECK(nm == 1 && mem && mem[0].ref.has_sha && pools::at(pools::kSamples, C("A001")).ref.has_sha &&
        !memcmp(mem[0].ref.sha, pools::at(pools::kSamples, C("A001")).ref.sha, 32));
  // forgotten (a reboot's state), the sample on voice 3: its map found by the sample's hash and read
  int inst_row = chop::map_row(vi);
  chop::reset();
  run(5);
  wg::trig(kC, 3, 60, 127, P((float)C("A001"), slc_v(3)));
  for (int i = 0; i < 400 && !(chop::sample_ready(3) && chop::map_of(3) && chop::stats().maps_read); ++i) run(1);
  CHECK(chop::stats().maps_read == 1 && chop::slices(3) == (int)(sizeof kOnsets / sizeof kOnsets[0]) &&
        chop::map_of(3)->s[3].pitch == -5 && chop::map_row(3) == inst_row);
  // the sampler's v1 slice map of tones.wav in INSTRUMENTS: found by its member, upgraded (CHOP's own file next)
  std::string v1 = "// Sidecar sample tones: 8 slices (grid), 48 kHz; key 36 (C1) is the first\n<global> loop_mode=one_shot\n";
  for (int k = 0; k < 8; ++k) {
    char b[160];
    snprintf(b, sizeof b, "<region> sample=tones.wav key=%d pitch_keycenter=%d offset=%d end=%d\n", 36 + k, 36 + k, k * 12000,
             k * 12000 + 11999);
    v1 += b;
  }
  wg::write_file(g_card + "/Samples/LIVE/tones-slices.sfz", std::vector<uint8_t>(v1.begin(), v1.end()));
  host_card_core(0);
  lib::touched("/Samples/LIVE/tones-slices.sfz", nullptr, 0);   // (as the web's upload tells the library)
  host_card_core(3);
  for (int i = 0; i < 400 && (i < 20 || lib::busy()); ++i) run(1);
  int ir = -1, rr = pools::set_file(pools::kInstr, C("B001"), "/Samples/LIVE/tones-slices.sfz", &ir, pools::kForced);
  for (int i = 0; i < 400 && pools::pending(); ++i) run(1);   // (its .sfz read on the card core)
  if (rr == pools::kPending) rr = pools::last_result(&ir);
  CHECK(rr == pools::kOk && pools::at(pools::kInstr, C("B001")).type == pools::kInstrument);
  run(20);
  wg::trig(kC, 4, 60, 127, P((float)C("A002"), slc_v(6)));
  for (int i = 0; i < 400 && !(chop::sample_ready(4) && chop::slices(4) == 8 && chop::played_slice(4) >= 0); ++i) run(1);
  CHECK(chop::slices(4) == 8 && chop::map_of(4)->s[6].start == 72000 && chop::played_slice(4) == 6);
  o = render(4, 9000, true);
  CHECK(std::fabs(zc(o, 1000, 9000) - hz_of_slice(6)) < 8);
  run(1500);   // (the upgraded map is written as CHOP's own file; the sampler's is left as it was)
  CHECK(strstr(chop::map_file(4), "/CHOP/LIVE/tones-") && file_of("/Samples/LIVE/tones-slices.sfz") == v1);
  uint64_t c3b = host_card_us(3), c0b = host_card_us(0);
  printf("test_chop: card time over the map tests: the control core %llu us, the card core %llu us\n",
         (unsigned long long)(c3b - c3), (unsigned long long)(c0b - c0));
  CHECK(c3b == c3 && c0b > c0);
  chop::silence(0), chop::silence(3), chop::silence(4);
  render(0, 960), render(3, 960), render(4, 960);
  printf("test_chop: card sample and map PASS\n");
}

// ---- 7b. a Rytm export's chunk layout (item 16 of the hardware list): JUNK before fmt, an smpl chunk (a loop) before data.
// CHOP walks the chunks (pcache, core/wavetable/wav_read.h) and ignores smpl: the whole file is the sample, sliced by its
// map, never looped by the file's own loop points.
static void test_rytm_export_layout() {
  const int vi = 1;
  std::vector<uint8_t> w, d;
  auto u32 = [&](std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); };
  auto u16 = [&](std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v), b.push_back((uint8_t)(v >> 8)); };
  for (int i = 0; i < kBreakN; ++i) u16(d, (uint16_t)g_break[i]);
  std::vector<uint8_t> smpl(60, 0);
  smpl[28] = 1;                       // one loop...
  smpl[36 + 12] = 0x40, smpl[36 + 13] = 0x1f;   // ...of frames 0..8000 (a tiny region: it must not loop the sample)
  w.insert(w.end(), {'R', 'I', 'F', 'F'});
  u32(w, (uint32_t)(4 + 8 + 52 + 8 + 16 + 8 + 60 + 8 + d.size()));
  w.insert(w.end(), {'W', 'A', 'V', 'E', 'J', 'U', 'N', 'K'});
  u32(w, 52), w.insert(w.end(), 52, 0);
  w.insert(w.end(), {'f', 'm', 't', ' '});
  u32(w, 16), u16(w, 1), u16(w, 1), u32(w, 48000), u32(w, 96000), u16(w, 2), u16(w, 16);
  w.insert(w.end(), {'s', 'm', 'p', 'l'});
  u32(w, 60), w.insert(w.end(), smpl.begin(), smpl.end());
  w.insert(w.end(), {'d', 'a', 't', 'a'});
  u32(w, (uint32_t)d.size()), w.insert(w.end(), d.begin(), d.end());
  CHECK(system(("mkdir -p '" + g_card + "/Samples/rytm'").c_str()) == 0);
  wg::write_file(g_card + "/Samples/rytm/export.wav", w);
  host_card_core(0);
  lib::touched("/Samples/rytm/export.wav", nullptr, 0);
  host_card_core(3);
  for (int i = 0; i < 400 && (i < 20 || lib::busy()); ++i) run(1);
  CHECK(pu::put(pools::kSamples, "A003", "/Samples/rytm/export.wav") == pools::kOk);
  run(50);
  wg::trig(kC, vi, 60, 127, P((float)C("A003"), slc_v(8)));
  for (int i = 0; i < 400 && !chop::sample_ready(vi); ++i) run(1);
  CHECK(chop::sample_ready(vi) && chop::sample_frames(vi) == (uint32_t)kBreakN && chop::slices(vi) == chop::kDefaultGrid);
  // slice 8 of the 16 grid starts at frame 48000, the break's next hit: sound, not a tick
  std::vector<float> o = render(vi, 4800, true);
  CHECK(rms(o, 0, 2400) > 0.05);
  chop::silence(vi);
  render(vi, 480);
  printf("test_chop: Rytm export layout (JUNK, fmt, smpl, data) PASS: %u frames, rms %.3f\n", chop::sample_frames(vi), rms(o, 0, 2400));
}

// ---- 8. RESAMPLE: slice 2 of voice 0 through its INSERT chain (a DRIVE) into a new sample, offered, accepted
static int16_t g_ring[kBlock][kVoices];
static void test_resample() {
  const int vi = 0;
  ins::set_type(vi, 0, ins::kDrive);
  wg::params(kC, vi, P((float)C("A002")));
  for (int i = 0; i < 400 && !chop::sample_ready(vi); ++i) run(1);
  CHECK(chop::edit_grid(vi, 8));
  run(5);
  uint64_t c3 = host_card_us(3);
  samp::Config mine = samp::config();   // review M4: the user's sampler settings survive a RESAMPLE
  mine.src = samp::kSrcIn, mine.max_s = 7;
  snprintf(mine.name, sizeof mine.name, "MINE");
  samp::configure(mine);
  CHECK(chop::resample(vi, 2, false));
  CHECK(chop::resample_state() == chop::kRsPlaying);
  CHECK(samp::config().src == samp::kSrcIn && samp::config().max_s == 7 && !strcmp(samp::config().name, "MINE"));
  uint64_t mach[kMachines] = {0};
  int16_t (*lr)[16] = nullptr;
  auto take = [&]() {
    for (int b = 0; b < 48000 / kBlock && chop::resample_state() == chop::kRsPlaying; ++b) {
      render_voices(g_ring, kBlock, 0, 0, kVoices, mach);
      samp::voice_block(g_ring, kBlock, lr);
      if (b % 4 == 0) run(1);
    }
    for (int i = 0; i < 3000 && chop::resample_state() == chop::kRsWriting; ++i) {   // (the OUT fill keeps feeding the
      if (samp::wants_voices())                                                         // sampler: its stop lands there)
        for (int b = 0; b < 4; ++b) render_voices(g_ring, kBlock, 0, 0, kVoices, mach), samp::voice_block(g_ring, kBlock, lr);
      run(1);
    }
  };
  take();
  int k = -1;
  char row[8];
  CHECK(chop::resample_state(&k, row) == chop::kRsOffer && k == 2 && row[0]);
  CHECK(!chop::resample(vi, 3, false));   // review M5: one take at a time (this one's offer not answered yet)
  samp::Status ss;
  samp::status(&ss);
  printf("test_chop: RESAMPLE slice 3 -> %s (%s)\n", row, ss.wav);
  CHECK(ss.wav[0] && file_of(ss.wav).size() > 44 && pools::at(pools::kSamples, C(row)).type == pools::kSample);
  CHECK(chop::resample_answer(true) && chop::map_of(vi)->s[2].alt == 0 && !strcmp(chop::map_of(vi)->alt[0], ss.wav));
  for (int i = 0; i < 400; ++i) run(1);
  wg::trig(kC, vi, 60, 127, P((float)C("A002"), slc_v(2)));
  run(5);
  std::vector<float> o = render(vi, 9000, true);
  CHECK(rms(o, 1000, 8000) > 0.05 && std::fabs(zc(o, 1000, 8000) - hz_of_slice(2)) < 10);   // the take: its tone, driven
  run(1200);
  CHECK(file_of(chop::map_file(vi)).find(std::string("sample=") + ss.wav) != std::string::npos);
  printf("test_chop: RESAMPLE's card time on the control core: %llu us\n", (unsigned long long)(host_card_us(3) - c3));
  CHECK(host_card_us(3) == c3);   // no card time on the control core (the sampler and the map: the card core)
  // review M5: a take whose slice moved meanwhile is not used (slice 4's start moved before the YES)
  CHECK(chop::resample(vi, 4, false));
  take();
  CHECK(chop::resample_state() == chop::kRsOffer);
  uint32_t s4 = chop::map_of(vi)->s[4].start;
  CHECK(chop::edit_start(vi, 4, (int32_t)(s4 + 480)));
  CHECK(!chop::resample_answer(true) && chop::map_of(vi)->s[4].alt < 0 && chop::resample_state() == chop::kRsIdle);
  // review minor: an edit refused leaves UNDO as it was (it takes back the start move, not a refused DEL)
  CHECK(!chop::edit_del(vi, 99));
  CHECK(chop::edit_undo(vi) && chop::map_of(vi)->s[4].start == s4);
  ins::set_type(vi, 0, ins::kOff);
  chop::silence(vi);
  render(vi, 960);
  samp::configure(samp::Config());
  printf("test_chop: RESAMPLE PASS\n");
}

// ---- 9. the render core: no allocation, no card time; the CPU per voice
static void test_render_core() {
  const int vi = 5;
  fresh_voice(vi, g_break, kBreakN, P(0));
  CHECK(chop::edit_grid(vi, 16));
  for (int k = 0; k < 8; ++k) CHECK(chop::edit_field(vi, k, chop::kFLoop, chop::kLoopFwd));
  CHECK(chop::edit_field(vi, 2, chop::kFStut, 4) && chop::edit_field(vi, 3, chop::kFTape, chop::kTapeStop));
  host_card_model(true);
  host_card_core(1);
  uint64_t c1 = host_card_us(1);
  for (int k = 0; k < 8; ++k) chop::live_on(vi, k, 100, poly::kSeqCh, k);   // 8 players (VOICES' most), MORPH on
  wg::params(kC, vi, P(0, 0, 64, 64));
  std::vector<float> o(kBlock);
  long n0 = g_news;
  for (int b = 0; b < 4000; ++b) chop::render_add(vi, o.data(), kBlock);
  float l[kBlock], r[kBlock];
  for (int b = 0; b < 400; ++b) chop::render_add2(vi, l, r, kBlock);
  long news = g_news - n0;
  uint64_t cu = host_card_us(1) - c1;
  host_card_core(3);
  printf("test_chop: render core: %ld allocations, %llu us of card time over 4400 blocks\n", news, (unsigned long long)cu);
  CHECK(news == 0 && cu == 0 && chop::voice_notes(vi) == 8);
  using clk = std::chrono::steady_clock;
  double best = 1e9;
  for (int rep = 0; rep < 5; ++rep) {
    auto t0 = clk::now();
    for (int b = 0; b < 4000; ++b) chop::render_add(vi, o.data(), kBlock);
    best = std::min(best, std::chrono::duration<double, std::micro>(clk::now() - t0).count());
  }
  double pct8 = best / 1e6 * 100.0, pct1 = pct8 / 8;
  printf("test_chop: CPU: 8 slice players (MORPH on) %.3f %% of a Mac core, %.3f %% a player; Pi 4 est. x 4.4: %.2f %% / "
         "%.2f %%\n", pct8, pct1, pct8 * 4.4, pct1 * 4.4);
  CHECK(pct8 < 3.0);
  for (int k = 0; k < 8; ++k) chop::live_off(vi, poly::kSeqCh, k);
  chop::silence(vi);
  render(vi, 960);
}

// ---- 10. the Rytm pages: the SRC view and the editor's views (PNGs, the text-fit check)
static int extra_index(const char* title) {
  for (int k = 0; k < ui::extra_pages(); ++k)
    if (!strcmp(ui::extra_page(k).title, title)) return k;
  return -1;
}
static void test_pages() {
  const int vi = 0;
  std::string dir = rui_png_dir("RUI_OUT", "build/rui_chop");
  ui::Canvas c;
  uint8_t fb[ui::kBytes];
  wg::params(kC, vi, P((float)C("A001"), slc_v(3)));
  for (int i = 0; i < 400 && !chop::sample_ready(vi); ++i) run(1);
  CHECK(chop::edit_transient(vi, 64));
  for (int i = 0; i < 100 && chop::transient_busy(vi); ++i) run(1);
  run(60);
  wg::trig(kC, vi, 60, 127, P((float)C("A001"), slc_v(3)));
  render(vi, 2400);
  CHECK(ui::remote_render(1, vi, nullptr, c));   // the voice's page: the SRC view's
  ui::preview(c, fb);
  rui_png(dir, "chop_src.png", fb);
  int k = extra_index("CHOP");
  CHECK(k >= 0 && ui::extra_page(k).off_ring && ui::extra_page(k).shown());
  const char* names[] = {"chop_edit.png", "chop_slice1.png", "chop_slice2.png"};
  for (int view = 0; view < 3; ++view) {
    ui::chop_page_set(view, 3, 1);
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb);
    rui_png(dir, names[view], fb);
  }
  ui::chop_page_set(0, 3, 3);   // START zoom
  CHECK(ui::remote_render(6 + k, 0, nullptr, c));
  ui::preview(c, fb);
  rui_png(dir, "chop_zoom_start.png", fb);
  // the keys: T5 selects (and auditions) slice 5 of bank 1, RIGHT the next, push B deletes, push D undoes
  ui::chop_page_set(0, 0, 1);
  CHECK(ui::extra_page(k).key(4) && ui::chop_page_sel() == 4);
  CHECK(ui::extra_page(k).key(0x41) && ui::chop_page_sel() == 5);
  int n0 = chop::slices(vi);
  CHECK(ui::extra_page(k).key(0x71) && chop::slices(vi) == n0 - 1);
  CHECK(ui::extra_page(k).key(0x73) && chop::slices(vi) == n0);
  ui::extra_page(k).enc(1, 3, false);   // B: the start a few pixels on
  CHECK(chop::map_dirty(vi));
  // LAZY on the page (push G), a trig key taps, YES keeps
  CHECK(ui::extra_page(k).key(0x76) && chop::lazy_on(vi));
  render(vi, 24000);
  CHECK(ui::extra_page(k).key(2));
  render(vi, 12000);
  CHECK(ui::extra_page(k).key(5));
  CHECK(ui::remote_render(6 + k, 0, nullptr, c));
  ui::preview(c, fb);
  rui_png(dir, "chop_lazy.png", fb);
  CHECK(ui::extra_page(k).key(0x44) && !chop::lazy_on(vi) && chop::slices(vi) == 2);
  CHECK(chop::edit_undo(vi));
  // the empty states
  chop::reset();
  for (int t = 0; t < kVoices; ++t) v[t].cp = false;
  CHECK(ui::remote_render(6 + k, 0, nullptr, c));
  ui::preview(c, fb);
  rui_png(dir, "chop_none.png", fb);
  ui::chop_page_reset();
  printf("test_chop: pages PASS (PNGs in %s)\n", dir.c_str());
}

int main() {
  char tmpl[] = "/tmp/chop-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/LIVE'").c_str()) == 0);
  make_break();
  make_tones();
  wg::write_file(g_card + "/Samples/LIVE/break.wav", wg::wav_bytes(kBreakN, 1, 16, 48000, 1, "", [](int i, int) {
                   return g_break[i] / 32767.0;
                 }));
  wg::write_file(g_card + "/Samples/LIVE/tones.wav", wg::wav_bytes(kTonesN, 1, 16, 48000, 1, "", [](int i, int) {
                   return g_tones[i] / 32767.0;
                 }));
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  hooks.to_rytm = [](const uint8_t*, int) {};
  voices_init();
  card_ok = true;
  snprintf(core::project, sizeof core::project, "LIVE");
  cardq::reset();
  cardq::set_worker(true);   // the Pi: the card core runs every card request (run() is its turn)
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  nm::begin(true);
  for (int i = 0; i < 4000 && !(pools::ready() && !strcmp(pools::project(), "LIVE")); ++i) run(1);
  CHECK(pools::ready());
  pu::clear(pools::kSamples);
  run(10);

  test_format();
  test_edits();
  test_onsets();
  test_lazy_pure();
  test_engine_buffer();
  test_engine_lazy();
  test_file_map();
  test_rytm_export_layout();
  test_resample();
  test_render_core();
  test_pages();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_chop: PASS\n");
  return 0;
}
