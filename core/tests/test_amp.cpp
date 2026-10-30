// core/tests/test_amp.cpp - INSERT TYPE 9 AMP (core/fx/amp.h, docs/INSERT-FX.md "AMP"): the loader (a synthetic
// A2-Lite capture as .namb and as .nam JSON; truncated, damaged, garbage, other shapes and rates refused), the engine
// against TONE3000's reference (the Cortex-M7 engine's C tiles on the same weights; the bundled NAMCore test capture
// against its golden output), the slot on a card folder (the /NAM/ list, the async load, the crossfade, the cap, the
// refusals and their texts), no allocation and no card I/O on the audio path, silence in -> silence out, no NaN or
// denormals, the card's AMP line, the CPU.
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "fx/amp.h"
#include "fx/insert.h"
#include "msamp/cardq.h"
#include "ui/extra.h"
#include "ui/remote.h"

// TONE3000's Cortex-M7 A2-Lite engine (its C tiles on a host): the reference the A76 engine is checked against
#undef NAM_A2LITE_H
#define restrict __restrict
namespace m7ref {
#include "../../third_party/nam/embedded/engines/cortex-m7/a2lite/nam_a2lite.c"
}
#undef restrict

using namespace core;

// ---- allocation counter (the audio path must not allocate)
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

static uint32_t crc32(const uint8_t* d, size_t n, size_t skip_at) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    if (i >= skip_at && i < skip_at + 4) continue;
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
static void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = (uint8_t)v, b[at + 1] = (uint8_t)(v >> 8); }
static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(v >> (8 * i));
}
// a .namb (format 1, as namb_reader.c reads it): WaveNet, one layer array of `ch` channels, 23 layers, head kernel 16
static std::vector<uint8_t> make_namb(const std::vector<float>& w, int ch = 3, double rate = 48000.0) {
  const size_t cfg = 4 + 64;   // WaveNet config: 4 bytes + layer array 0 (only its first fields are read)
  size_t woff = (80 + 4 + cfg + 3) & ~(size_t)3, size = woff + 4 * w.size();
  std::vector<uint8_t> b(size, 0);
  put32(b, 0, 0x4E414D42u), put16(b, 4, 1), put32(b, 8, (uint32_t)size), put32(b, 12, (uint32_t)woff);
  put32(b, 16, (uint32_t)w.size()), put32(b, 20, (uint32_t)(4 + cfg));
  memcpy(&b[32 + 4], &rate, 8);
  b[80] = 3, put16(b, 82, (uint16_t)cfg);
  size_t wn = 84;
  b[wn] = 1, b[wn + 1] = 0, b[wn + 2] = 1, b[wn + 3] = 0;
  size_t la = wn + 4;
  put16(b, la + 0, 1), put16(b, la + 2, 1), put16(b, la + 4, 1), put16(b, la + 6, (uint16_t)ch), put16(b, la + 8, (uint16_t)ch);
  put16(b, la + 10, 16), b[la + 12] = 0, b[la + 13] = 23;
  memcpy(&b[woff], w.data(), 4 * w.size());
  put32(b, 24, crc32(b.data(), size, 24));
  return b;
}
// the same capture as NAM's .nam JSON (the fields amp.cpp checks; "%.9g": a float's exact round trip)
static std::string make_nam(const std::vector<float>& w, const char* arch = "WaveNet", int ch = 3, int rate = 48000) {
  std::string s = "{\"version\": \"0.5.4\", \"metadata\": {\"name\": \"synthetic \\\"test\\\"\", \"gain\": null},\n";
  s += std::string("\"architecture\": \"") + arch + "\", \"config\": {\"layers\": [{\"input_size\": 1, \"condition_size\": 1, ";
  s += "\"channels\": " + std::to_string(ch) + ", \"head_size\": 1, \"kernel_sizes\": [6, 15], \"dilations\": [1, 2, 4], ";
  s += "\"activation\": \"LeakyReLU\", \"gated\": false, \"head_bias\": true}], \"head\": null, \"head_scale\": 0.6},\n";
  s += "\"sample_rate\": " + std::to_string(rate) + ", \"weights\": [";
  char b[32];
  for (size_t i = 0; i < w.size(); ++i) {
    snprintf(b, sizeof b, "%s%.9g", i ? ", " : "", w[i]);
    s += b;
  }
  s += "]}\n";
  return s;
}
// TONE3000's A2 file: a SlimmableContainer of submodels (an A2-Lite and an A2-Full WaveNet), each a model as above
static std::string make_slim(const std::vector<std::string>& models) {
  std::string s = "{\"version\":\"0.7.0\",\"architecture\":\"SlimmableContainer\",\"config\":{\"submodels\":[";
  for (size_t i = 0; i < models.size(); ++i) {
    std::string m = models[i];
    while (!m.empty() && (m.back() == '\n' || m.back() == ' ')) m.pop_back();
    s += (i ? "," : "") + std::string("{\"max_value\":") + (i + 1 == models.size() ? "1.0" : "0.5") + ",\"model\":" + m + "}";
  }
  s += "]},\"weights\":[],\"sample_rate\":48000.0,\"metadata\":{\"name\":\"x\",\"gain\":0.9}}";
  return s;
}
static std::vector<float> synth(uint32_t seed) {
  std::vector<float> w(amp::kWeights);
  amp::synthetic_weights(w.data(), seed);
  return w;
}
static amp::Parse parse_json(const std::string& s, float* w, uint32_t budget = 4096) {
  amp::JsonScan j;
  j.begin(s.data(), (uint32_t)s.size(), w);
  int r;
  while ((r = j.step(budget)) == 1) {}
  return j.result();
}
static std::vector<uint8_t> read_file(const char* p) {
  std::vector<uint8_t> b;
  FILE* f = fopen(p, "rb");
  if (!f) return b;
  fseek(f, 0, SEEK_END);
  b.resize((size_t)ftell(f));
  fseek(f, 0, SEEK_SET);
  if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
  fclose(f);
  return b;
}
static void write_file(const std::string& p, const void* d, size_t n) {
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  CHECK(fwrite(d, 1, n, f) == n);
  fclose(f);
}

// the A76 engine (as amp.cpp runs it) and the M7 reference over x, in blocks of `blk`
static std::vector<float> run_a76(const std::vector<float>& w, const std::vector<float>& x, int blk) {
  static nam_a2lite_weights_t W;
  static nam_a2lite_t E;
  static nam_a2lite_state_t S;
  CHECK(nam_a2lite_load_weights(&W, w.data(), (int)w.size()) == 0);
  nam_a2lite_init(&E, &W, &S);
  nam_a2lite_prewarm(&E);
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); i += blk) {
    int n = (int)std::min<size_t>(blk, x.size() - i);
    const float* in[1] = {&x[i]};
    float* out[1] = {&y[i]};
    nam_a2lite_process(&E, in, out, n);
  }
  return y;
}
static std::vector<float> run_m7(const std::vector<float>& w, const std::vector<float>& x, int blk) {
  static m7ref::nam_a2lite_weights_t W;
  static m7ref::nam_a2lite_t E;
  static m7ref::nam_a2lite_state_t S;
  CHECK(m7ref::nam_a2lite_load_weights(&W, w.data(), (int)w.size()) == 0);
  m7ref::nam_a2lite_init(&E, &W, &S);
  m7ref::nam_a2lite_prewarm(&E);
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); i += blk) {
    int n = (int)std::min<size_t>(blk, x.size() - i);
    const float* in[1] = {&x[i]};
    float* out[1] = {&y[i]};
    m7ref::nam_a2lite_process(&E, in, out, n);
  }
  return y;
}
static void compare(const std::vector<float>& y, const std::vector<float>& ref, const char* what, double tol) {
  double e = 0, s = 0, mx = 0, pk = 0;
  for (size_t i = 0; i < y.size(); ++i) {
    double d = (double)y[i] - ref[i];
    e += d * d, s += (double)ref[i] * ref[i];
    mx = std::max(mx, std::fabs(d)), pk = std::max(pk, (double)std::fabs(ref[i]));
    CHECK(std::isfinite(y[i]));
  }
  double esr = 10 * std::log10(e / (s + 1e-300) + 1e-300);
  printf("%s: max abs err %.2e of peak %.3f, ESR %.1f dB\n", what, mx, pk, esr);
  CHECK(pk > 1e-3);
  CHECK(mx <= tol * pk);
}
static std::vector<float> guitarish(int n, float amp = 0.3f) {   // plucked notes: a decaying saw-ish tone, E2..E3
  std::vector<float> x(n);
  const float notes[4] = {82.41f, 110.0f, 146.83f, 164.81f};
  for (int i = 0; i < n; ++i) {
    int k = (i / 12000) & 3;
    float t = (float)(i % 12000) / 48000.0f, ph = notes[k] * t;
    float saw = 2.0f * (ph - std::floor(ph)) - 1.0f;
    x[i] = amp * std::exp(-4.0f * t) * (0.6f * saw + 0.4f * std::sin(2 * (float)M_PI * 2 * ph));
  }
  return x;
}

// the live slot: voice 0's chain through the global engine, a block at a time (the render core's order: service, chain)
static std::vector<float> play(const std::vector<float>& x, int chain = 0, bool steps = true) {
  std::vector<float> y = x;
  for (size_t i = 0; i < y.size(); i += 12) {
    if (steps) amp::step(), amp::step();   // the control core between blocks
    host_card_core(1);
    amp::service(chain);
    ins::engine().ProcessMono(chain, &y[i], (int)std::min<size_t>(12, y.size() - i), ins::control);
    host_card_core(3);
  }
  return y;
}
static void settle() {
  for (int i = 0; i < 4000; ++i) amp::step();
}
static std::string text(int c, int s) {
  char b[16];
  ins::slot_value_text(c, s, 0, ins::param(c, s, 0), b, sizeof b);
  return b;
}

int main() {
  // ---- the loader: .namb
  {
    std::vector<float> w = synth(7), got(amp::kWeights);
    auto nb = make_namb(w);
    std::vector<uint32_t> al((nb.size() + 3) / 4);
    memcpy(al.data(), nb.data(), nb.size());
    CHECK(amp::parse_namb((const uint8_t*)al.data(), (uint32_t)nb.size(), got.data()) == amp::kOk);
    CHECK(!memcmp(got.data(), w.data(), 4 * w.size()));
    int bad = 0;
    for (uint32_t cut : {0u, 10u, 31u, 79u, 100u, (uint32_t)nb.size() / 2, (uint32_t)nb.size() - 1}) {
      CHECK(amp::parse_namb((const uint8_t*)al.data(), cut, got.data()) != amp::kOk);
      ++bad;
    }
    auto flip = nb;
    flip[nb.size() / 2] ^= 0x40;
    memcpy(al.data(), flip.data(), flip.size());
    CHECK(amp::parse_namb((const uint8_t*)al.data(), (uint32_t)flip.size(), got.data()) == amp::kErrCrc);
    uint32_t r = 99;
    std::vector<uint32_t> junk(2000);
    for (int k = 0; k < 50; ++k) {
      for (uint32_t& v : junk) v = r = r * 1664525u + 1013904223u;
      if (k & 1) junk[0] = 0x424D414Eu;   // the magic, garbage after it
      CHECK(amp::parse_namb((const uint8_t*)junk.data(), 8000, got.data()) != amp::kOk);
    }
    std::vector<float> wf(12146, 0.01f);   // A2-Full's shape
    auto full = make_namb(wf, 8);
    std::vector<uint32_t> fa((full.size() + 3) / 4);
    memcpy(fa.data(), full.data(), full.size());
    CHECK(amp::parse_namb((const uint8_t*)fa.data(), (uint32_t)full.size(), got.data()) == amp::kErrNotLite);
    auto r44 = make_namb(w, 3, 44100.0);
    memcpy(al.data(), r44.data(), r44.size());
    CHECK(amp::parse_namb((const uint8_t*)al.data(), (uint32_t)r44.size(), got.data()) == amp::kErrRate);
    auto wn = w;
    wn[100] = NAN;
    auto nn = make_namb(wn);
    memcpy(al.data(), nn.data(), nn.size());
    CHECK(amp::parse_namb((const uint8_t*)al.data(), (uint32_t)nn.size(), got.data()) == amp::kErrNotFinite);
    printf("loader .namb: valid read bit for bit; %d cuts, a flipped bit (CRC), 50 garbage files, A2-Full, 44.1 kHz, NaN refused\n", bad);
  }
  // ---- the loader: .nam (JSON), stepped
  {
    std::vector<float> w = synth(7), got(amp::kWeights, 0), got1(amp::kWeights, 0);
    std::string js = make_nam(w);
    CHECK(parse_json(js, got.data()) == amp::kOk);
    CHECK(!memcmp(got.data(), w.data(), 4 * w.size()));   // "%.9g" back to the same floats
    CHECK(parse_json(js, got1.data(), 1) == amp::kOk && !memcmp(got1.data(), w.data(), 4 * w.size()));   // a byte a step
    int cuts = 0;
    for (size_t cut : {(size_t)0, (size_t)1, (size_t)40, js.size() / 3, js.size() - 3}) {
      CHECK(parse_json(js.substr(0, cut), got.data()) != amp::kOk);
      ++cuts;
    }
    CHECK(parse_json(make_nam(w, "LSTM"), got.data()) == amp::kErrNotLite);
    CHECK(parse_json(make_nam(w, "WaveNet", 16), got.data()) == amp::kErrNotLite);   // NAM's A1 "Standard" channels
    CHECK(parse_json(make_nam(std::vector<float>(w.begin(), w.end() - 1)), got.data()) == amp::kErrNotLite);
    CHECK(parse_json(make_nam(w, "WaveNet", 3, 44100), got.data()) == amp::kErrRate);
    std::string nanj = js;
    nanj.replace(nanj.find("\"weights\": [") + 12, 0, "NaN, ");
    CHECK(parse_json(nanj, got.data()) == amp::kErrNotLite);   // 1,872 values
    std::string nanj2 = js;
    size_t at = nanj2.find("\"weights\": [") + 12;
    nanj2.replace(at, nanj2.find(',', at) - at, "NaN");
    CHECK(parse_json(nanj2, got.data()) == amp::kErrNotFinite);
    // TONE3000's SlimmableContainer: the A2-Lite submodel taken (first or second), a container without one refused
    std::vector<float> full(12146, 0.01f), w3 = synth(3);
    std::string fulls = make_nam(full, "WaveNet", 8);
    std::fill(got.begin(), got.end(), 0.0f);
    CHECK(parse_json(make_slim({js, fulls}), got.data()) == amp::kOk && !memcmp(got.data(), w.data(), 4 * w.size()));
    CHECK(parse_json(make_slim({fulls, make_nam(w3)}), got.data(), 7) == amp::kOk && !memcmp(got.data(), w3.data(), 4 * w3.size()));
    CHECK(parse_json(make_slim({js, make_nam(w3)}), got.data()) == amp::kOk && !memcmp(got.data(), w.data(), 4 * w.size()));
    CHECK(parse_json(make_slim({fulls}), got.data()) == amp::kErrNotLite);
    CHECK(parse_json(make_slim({}), got.data()) == amp::kErrFormat);
    std::string sl = make_slim({js, fulls});
    CHECK(parse_json(sl.substr(0, sl.size() / 2), got.data()) == amp::kErrTruncated);
    std::string two = js + "{}";
    CHECK(parse_json(two, got.data()) != amp::kOk);
    uint32_t r = 5;
    for (int k = 0; k < 50; ++k) {
      std::string g(3000, ' ');
      for (char& ch : g) r = r * 1664525u + 1013904223u, ch = (char)(32 + (r >> 24) % 95);
      if (k & 1) g[0] = '{';
      CHECK(parse_json(g, got.data()) != amp::kOk);
    }
    printf("loader .nam: valid read bit for bit (4 KB and 1 byte a step); %d cuts, LSTM, 16 channels, 1,870 weights, 44.1 kHz, NaN, two objects, 50 garbage texts refused; TONE3000's SlimmableContainer: its A2-Lite submodel taken\n", cuts);
  }
  // ---- the engine against TONE3000's references
  {
    std::vector<float> x = guitarish(48000);
    for (uint32_t seed : {1u, 2u, 3u}) {
      std::vector<float> w = synth(seed);
      auto a = run_a76(w, x, 12), m = run_m7(w, x, 12);
      char what[64];
      snprintf(what, sizeof what, "synthetic capture %u: A76 engine vs M7 reference", seed);
      compare(a, m, what, 1e-5);
      auto a16 = run_a76(w, x, 16), a1 = run_a76(w, x, 1);
      for (size_t i = 0; i < x.size(); ++i) CHECK(std::fabs(a16[i] - a[i]) <= 1e-6f && std::fabs(a1[i] - a[i]) <= 1e-6f);
    }
    // NeuralAmpModelerCore's own A2-Lite test capture (in TONE3000's repository, MIT) against its golden output
    auto nb = read_file("../third_party/nam/embedded/weights/a2lite.namb");
    auto gin = read_file("../third_party/nam/embedded/test/golden_input.raw");
    auto gout = read_file("../third_party/nam/embedded/test/golden_a2lite_output.raw");
    CHECK(!nb.empty() && gin.size() == 384000 && gout.size() == 384000);
    std::vector<uint32_t> al((nb.size() + 3) / 4);
    memcpy(al.data(), nb.data(), nb.size());
    std::vector<float> w(amp::kWeights), xi(96000), yo(96000);
    CHECK(amp::parse_namb((const uint8_t*)al.data(), (uint32_t)nb.size(), w.data()) == amp::kOk);
    memcpy(xi.data(), gin.data(), gin.size()), memcpy(yo.data(), gout.data(), gout.size());
    compare(run_a76(w, xi, 12), yo, "NAMCore A2-Lite test capture (block 12) vs NeuralAmpModelerCore's golden", 1e-4);
  }

  // ---- the slot on a card folder
  char tmpl[] = "/tmp/test_amp.XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  host_set_card(tmpl);
  CHECK(mkdir((card + "/NAM").c_str(), 0755) == 0);
  CHECK(mkdir((card + "/Samples").c_str(), 0755) == 0);   // a card (the index wants its /Samples)
  {
    auto a = make_namb(synth(1));
    write_file(card + "/NAM/Synth A.namb", a.data(), a.size());
    std::string b = make_nam(synth(2));
    write_file(card + "/NAM/synth_b.nam", b.data(), b.size());
    write_file(card + "/NAM/Broken.namb", "NAMB garbage garbage garbage", 28);
    std::vector<float> wf(12146, 0.01f);
    auto full = make_namb(wf, 8);
    write_file(card + "/NAM/Full Rig.namb", full.data(), full.size());
    write_file(card + "/NAM/readme.txt", "x", 1);
  }
  host_fake_time(true, 5000000);
  CHECK(engine_begin());   // the card: ins::load, amp::begin (/NAM/ listed at the next steps)
  ins::reset();
  settle();
  char st[512];
  amp::command("amp", st, sizeof st);
  printf("%s\n", st);
  CHECK(amp::models() == 4);
  CHECK(!strcmp(amp::model_file(0), "Broken.namb") && !strcmp(amp::model_file(1), "Full Rig.namb"));
  CHECK(!strcmp(amp::model_file(2), "Synth A.namb") && !strcmp(amp::model_file(3), "synth_b.nam"));
  CHECK(!strcmp(amp::model_short(2), "SYNTHA") && !strcmp(amp::model_short(3), "SYNTHB"));
  for (int i = 0; i < 4; ++i) CHECK(amp::model_for_knob(amp::knob_for_model(i)) == i);
  CHECK(amp::model_for_knob(0) == 0 && amp::model_for_knob(127) == 3);
  printf("/NAM/ listed: %d captures, sorted, short names SYNTHA SYNTHB\n", amp::models());

  // bus chains do not take AMP
  ins::set_type(ins::kBus0, 0, ins::kAmp);
  CHECK(ins::control.type[ins::kBus0][0] == ins::kOff);

  // the async load and the crossfade in: dry (TONE flat, LEV 0 dB) while it loads, the capture after
  host_card_model(true);
  long news0 = g_news;
  std::vector<float> x = guitarish(48000 * 2);
  ins::set_type(0, 0, ins::kAmp);
  ins::set_param(0, 0, 0, amp::knob_for_model(2));
  CHECK(text(0, 0) == "SYNTHA");   // (the status before the control core's first step: the knob's file)
  std::vector<float> y = x;
  int first_wet = -1;
  {
    std::vector<float> d(12);
    for (size_t i = 0; i < y.size(); i += 12) {
      amp::step();
      host_card_core(1);
      long n0 = g_news;
      amp::service(0);
      ins::engine().ProcessMono(0, &y[i], 12, ins::control);
      CHECK(g_news == n0);   // no allocation on the audio path
      host_card_core(3);
      if (first_wet < 0 && amp::stats().taken) first_wet = (int)i;
    }
  }
  CHECK(host_card_us(1) == 0);   // no card I/O charged to the render core
  CHECK(host_card_us(3) > 0);    // the load's (posted card requests run at once without a card core)
  CHECK(amp::status(0, 0) == amp::kPlaying && text(0, 0) == "SYNTHA" && amp::running() == 1);
  CHECK(first_wet > 0);
  printf("async load: the capture plays from frame %d (%.1f ms: listing, read, prewarm in steps); %ld allocations by the steps, none in a block\n",
         first_wet, first_wet / 48.0, g_news - news0);
  // before the offer the slot is the dry voice (the TYPE's 5 ms fade, the DC block, LEV 0.05 dB)
  {
    double err = 0;
    for (int i = 0; i < first_wet; ++i) err = std::max(err, (double)std::fabs(y[i] - x[i]));
    printf("dry while loading: max |y - x| %.4f\n", err);
    CHECK(err < 0.02);
    // no click at the crossfade: the largest step around it against the signal's own
    float own = 0, step = 0;
    for (int i = 1; i < first_wet; ++i) own = std::max(own, std::fabs(x[i] - x[i - 1]));
    for (int i = first_wet; i < first_wet + 2000; ++i) step = std::max(step, std::fabs(y[i] - y[i - 1]));
    float wet = 0;
    for (size_t i = first_wet + 4800; i < y.size(); ++i) wet = std::max(wet, std::fabs(y[i] - y[i - 1]));
    printf("crossfade in: largest step %.4f (dry signal's own %.4f, the capture's own %.4f)\n", step, own, wet);
    CHECK(step <= 1.5f * std::max(own, wet));
  }
  // a MODL change: B loads while A plays, then 10 ms of crossfade; the old instance goes back to the pool
  {
    uint32_t xf = amp::stats().xfades;
    ins::set_param(0, 0, 0, amp::knob_for_model(3));
    y = play(guitarish(48000));
    settle();
    CHECK(amp::stats().xfades == xf + 1 && amp::status(0, 0) == amp::kPlaying && text(0, 0) == "SYNTHB");
    CHECK(amp::running() == 1);
    // back to A: its instance is reused (a reset and a prewarm, no read)
    uint32_t ld = amp::stats().loads, ru = amp::stats().reuses;
    ins::set_param(0, 0, 0, amp::knob_for_model(2));
    y = play(guitarish(24000));
    CHECK(amp::stats().loads == ld && amp::stats().reuses == ru + 1 && text(0, 0) == "SYNTHA");
    printf("MODL change: crossfaded, the old capture's instance kept and reused (loads %u, reuses %u)\n", amp::stats().loads,
           amp::stats().reuses);
    for (float v : y) CHECK(std::isfinite(v));
  }
  // refusals: a damaged file, an A2-Full capture; the slot plays dry and says why
  {
    ins::set_param(0, 0, 0, amp::knob_for_model(0));
    play(guitarish(9600));
    CHECK(amp::status(0, 0) == amp::kBadFile && text(0, 0) == "BADFILE" && amp::running() == 0);
    ins::set_param(0, 0, 0, amp::knob_for_model(1));
    play(guitarish(9600));
    CHECK(amp::status(0, 0) == amp::kNotLite && text(0, 0) == "NOTLITE");
    char o[512];
    CHECK(amp::command("amp", o, sizeof o));
    printf("%s\n", o);
    CHECK(strstr(o, "LITE MODELS ONLY"));
  }
  // silence in -> silence out (the capture's own silent output taken off, the DC block), no NaN, no denormals
  {
    ins::set_param(0, 0, 0, amp::knob_for_model(2));
    play(guitarish(9600));
    CHECK(amp::status(0, 0) == amp::kPlaying);
    std::vector<float> z(48000 * 2, 0.0f);
    auto s = play(z);
    float tail = 0;
    for (size_t i = 0; i < s.size(); ++i) {
      CHECK(std::isfinite(s[i]) && std::fpclassify(s[i]) != FP_SUBNORMAL);
      if (i > 48000) tail = std::max(tail, std::fabs(s[i]));
    }
    printf("silence in: |y| %.2e after 1 s\n", tail);
    CHECK(tail < 1e-5f);
    // from the start: a fresh slot on silence is silent
    ins::set_type(0, 1, ins::kAmp);
    ins::set_param(0, 1, 0, amp::knob_for_model(3));
    auto s2 = play(z);
    float pk2 = 0;
    for (float v : s2) pk2 = std::max(pk2, std::fabs(v));
    printf("silence through two AMPs from their start: peak %.2e\n", pk2);
    CHECK(pk2 < 1e-5f);
    ins::set_type(0, 1, ins::kOff);
    // full-scale noise at every corner of GAIN / TONE / LEV: finite, bounded, no fault
    uint32_t r = 3;
    std::vector<float> nz(9600);
    for (float& v : nz) r = r * 1664525u + 1013904223u, v = (float)(r >> 8) / 8388608.0f - 1.0f;
    float worst = 0;
    for (int corner = 0; corner < 8; ++corner) {
      ins::set_param(0, 0, 1, (corner & 1) * 127), ins::set_param(0, 0, 2, (corner & 2) ? 127 : 0),
          ins::set_param(0, 0, 3, (corner & 4) ? 127 : 0);
      auto q = play(nz);
      for (float v : q) {
        CHECK(std::isfinite(v) && std::fpclassify(v) != FP_SUBNORMAL);
        worst = std::max(worst, std::fabs(v));
      }
    }
    printf("noise at every corner: peak %.2f, faults %u\n", worst, ins::engine().Faults());
    CHECK(ins::engine().Faults() == 0);
    ins::set_param(0, 0, 1, 64), ins::set_param(0, 0, 2, 64), ins::set_param(0, 0, 3, 109);
  }
  // the cap: 16 AMP slots asked, 6 play (fix1049: the Pi's bench, 3.98 % an instance), the others dry with MAX 6
  {
    for (int c = 0; c < 8; ++c)
      for (int s = 0; s < 2; ++s) ins::set_type(c, s, ins::kAmp), ins::set_param(c, s, 0, amp::knob_for_model(2 + (c & 1)));
    std::vector<float> blk(12, 0.1f);
    for (int b = 0; b < 3000; ++b) {
      amp::step();
      for (int c = 0; c < 8; ++c) {
        amp::service(c);
        std::vector<float> v = blk;
        ins::engine().ProcessMono(c, v.data(), 12, ins::control);
      }
    }
    int cap = 0, playing = 0;
    for (int c = 0; c < 8; ++c)
      for (int s = 0; s < 2; ++s) cap += amp::status(c, s) == amp::kCap, playing += amp::status(c, s) == amp::kPlaying;
    printf("the cap: %d playing, %d refused (MAX 6), refused counted %u\n", playing, cap, amp::stats().refused);
    CHECK(playing == amp::kMaxRunning && cap == 16 - amp::kMaxRunning && amp::running() == amp::kMaxRunning);
    bool seen = false;
    for (int c = 0; c < 8; ++c)
      for (int s = 0; s < 2; ++s)
        if (amp::status(c, s) == amp::kCap) seen = seen || text(c, s) == "MAX 6";
    CHECK(seen);
    // one off: a refused slot takes its place
    ins::set_type(0, 0, ins::kOff);
    for (int b = 0; b < 3000; ++b) {
      amp::step();
      for (int c = 0; c < 8; ++c) {
        amp::service(c);
        std::vector<float> v = blk;
        ins::engine().ProcessMono(c, v.data(), 12, ins::control);
      }
    }
    playing = 0;
    for (int c = 0; c < 8; ++c)
      for (int s = 0; s < 2; ++s) playing += amp::status(c, s) == amp::kPlaying;
    CHECK(playing == amp::kMaxRunning && amp::running() == amp::kMaxRunning);
    for (int c = 0; c < 8; ++c)
      for (int s = 0; s < 2; ++s) ins::set_type(c, s, ins::kOff);
    for (int b = 0; b < 200; ++b) {
      amp::step();
      for (int c = 0; c < 8; ++c) amp::service(c);
    }
    settle();
    CHECK(amp::running() == 0);
  }
  // the labels and texts fit (4 / 7 characters), every value and state
  {
    const ins::Labels& L = ins::labels(ins::kAmp);
    CHECK(!strcmp(L.name, "AMP") && !strcmp(ins::type_name(9), "AMP") && ins::type_from_name("amp") == 9);
    for (int k = 0; k < 4; ++k) CHECK(strlen(L.label[k]) <= 4 && strlen(L.label[k]) > 0);
    char b[32];
    for (int k = 0; k < 4; ++k)
      for (int v = 0; v < 128; ++v) {
        ins::value_text(ins::kAmp, k, v, b, sizeof b);
        CHECK(strlen(b) <= 7 && strlen(b) > 0);
      }
    for (int s = 0; s < amp::kStatuses; ++s) CHECK(strlen(amp::status_short(s)) <= 7);
    ins::value_text(ins::kAmp, 1, 64, b, sizeof b);
    CHECK(!strcmp(b, "0DB"));
    ins::value_text(ins::kAmp, 1, 127, b, sizeof b);
    CHECK(!strcmp(b, "+18DB"));
    ins::value_text(ins::kAmp, 2, 64, b, sizeof b);
    CHECK(!strcmp(b, "FLAT"));
    ins::value_text(ins::kAmp, 2, 0, b, sizeof b);
    CHECK(!strcmp(b, "-6DB"));
    ins::value_text(ins::kAmp, 3, 109, b, sizeof b);
    CHECK(!strcmp(b, "0DB"));
    // the Rytm's INSERT page with an AMP slot (the text-fit check: check.h)
    ins::set_type(2, 0, ins::kAmp);
    ins::set_param(2, 0, 0, amp::knob_for_model(1));
    settle();
    int k = -1;
    for (int i = 0; i < ui::extra_pages(); ++i)
      if (!strcmp(ui::extra_page(i).title, "INSERT")) k = i;
    CHECK(k >= 0);
    ins::set_type(0, 0, ins::kAmp);
    ins::set_param(0, 0, 0, amp::knob_for_model(1));
    settle();
    ui::ins_page_set(false, 0, 0, -1);
    ui::Canvas c;
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    for (int e = 0; e < 4; ++e) ui::extra_page(k).enc(e, 0, false);
    ui::extra_page(k).enc(0, 1, false), ui::extra_page(k).enc(0, -1, false);
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    printf("labels MODL GAIN TONE LEV, every value <= 7 characters, the INSERT page drawn (text fits)\n");
    ins::set_type(2, 0, ins::kOff);
    ins::set_type(0, 0, ins::kOff);
  }
  // the card: the AMP line keeps the capture by name, resolved once /NAM/ is listed (the knob follows the list)
  {
    ins::set_type(3, 1, ins::kAmp);
    ins::set_param(3, 1, 0, amp::knob_for_model(3));
    CHECK(!strcmp(amp::slot_file(3, 1), "synth_b.nam"));
    ins::settings_due();
    CHECK(ins::save());
    std::string all;   // the project's default file (no kit named)
    std::string cmd = "cat " + card + "/.rytm/ins/*.txt > " + card + "/all.txt";
    CHECK(system(cmd.c_str()) == 0);
    auto t = read_file((card + "/all.txt").c_str());
    all.assign(t.begin(), t.end());
    printf("card file:\n%s", all.c_str());
    CHECK(all.find("V4 B AMP ") != std::string::npos && all.find("AMP V4 B synth_b.nam") != std::string::npos);
    // a new capture sorted in front: after the reload and the listing, the knob points at the same file
    std::string z = make_nam(synth(9));
    write_file(card + "/NAM/Aaa First.nam", z.data(), z.size());
    ins::load();
    amp::rescan();
    settle();
    CHECK(amp::models() == 5 && !strcmp(amp::slot_file(3, 1), "synth_b.nam"));
    CHECK(amp::model_for_knob(ins::param(3, 1, 0)) == 4);
    printf("card: AMP V4 B synth_b.nam resolved after a new file shifted the list\n");
    ins::set_type(3, 1, ins::kOff);
  }
  // final review (parked 6b concern 4): with a card core, the /NAM/ listing (amp=rescan, the boot's) is a card-queue
  // request the card core runs - the control core's steps charge the card nothing, the list changes only once it ran
  {
    std::string z = make_nam(synth(11));
    write_file(card + "/NAM/Zed Last.nam", z.data(), z.size());
    CHECK(mkdir((card + "/NAM/Folder.nam").c_str(), 0755) == 0 || errno == EEXIST);
    int n0 = amp::models();
    cardq::reset();
    cardq::set_worker(true);
    host_card_model(true);
    uint64_t c3 = host_card_us(3), c0 = host_card_us(0);
    uint32_t lists = cardq::stats().ran[cardq::kListReq];
    amp::rescan();
    for (int i = 0; i < 50; ++i) amp::step();   // posted, the card core not yet run: the old list
    CHECK(amp::models() == n0 && cardq::stats().ran[cardq::kListReq] == lists);
    for (int i = 0; i < 50; ++i) {
      host_card_core(0);
      while (cardq::service()) {}
      host_card_core(3);
      amp::step();
    }
    CHECK(cardq::stats().ran[cardq::kListReq] == lists + 1);
    CHECK(host_card_us(3) == c3 && host_card_us(0) > c0);   // the listing's card time all on the card core
    // (the listing has no folder flag: Folder.nam is listed as a capture, as the knob would show it, and fails to open)
    CHECK(amp::models() == n0 + 2 && !strcmp(amp::model_file(amp::models() - 1), "Zed Last.nam"));
    printf("/NAM/ listed through the card queue: %d captures, control-core card time %llu us\n", amp::models(),
           (unsigned long long)(host_card_us(3) - c3));
    host_card_model(false);
    cardq::set_worker(false);
    cardq::reset();
    rmdir((card + "/NAM/Folder.nam").c_str());
    unlink((card + "/NAM/Zed Last.nam").c_str());
    amp::rescan();
    settle();
    CHECK(amp::models() == n0);
  }
  // the /status JSON
  {
    ins::set_type(5, 0, ins::kAmp);
    settle();
    static char j[8192];
    int n = ins::json(j, sizeof j);
    CHECK(n > 0 && n < (int)sizeof j);
    CHECK(strstr(j, "\"amp\":{\"models\":5"));
    printf("json: %s\n", strstr(j, "\"amp\""));
  }
  // the CPU: one instance's cost on this Mac (the Pi 4 estimate: x 4.4, the boot bench's PLAITS ratio)
  {
    uint32_t worst = 0;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t ticks = amp::bench(&worst);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double pct = (double)ticks / cycles_per_second() * 100.0;
    printf("CPU: one A2-Lite instance, 1 s of audio in 12-frame blocks: %.2f %% of a Mac core (wall %.1f ms); Pi 4 est. %.1f %%, the cap's 8: %.1f %%\n",
           pct, s * 1000, pct * 4.4, pct * 4.4 * 8);
    printf("RAM: %lu bytes for %d instances and the file buffer (%lu an instance)\n", amp::heap_bytes(), amp::kInstances,
           (unsigned long)(sizeof(nam_a2lite_weights_t) + sizeof(nam_a2lite_t) + sizeof(nam_a2lite_state_t)));
  }
  host_card_model(false);
  ins::reset();
  std::string rm = "rm -rf " + card;
  CHECK(system(rm.c_str()) == 0);
  printf("test_amp: PASS\n");
  return 0;
}
