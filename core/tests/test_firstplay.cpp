// core/tests/test_firstplay.cpp - load on first play, keep in RAM (BACKLOG 7ad,
// docs/superpowers/specs/2026-10-30-load-on-first-play.md), on a card core (cardq::set_worker + manual service): a
// streamed sample's first note sounds with no gap and brings the whole sample into the pool RAM; the second note does
// no card read at all; both are bit-identical to a RAM-loaded and to a purely streamed instrument, a chord's in-flight
// note through the change included. A small budget gives kept samples back by least recent play and never one a note
// plays. A stream under pressure beats the fill. A project change abandons the fill (its file closed, its block gone,
// the head still playing). The control core spends no card time throughout.
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "pool_util.h"
#include "pool/pcache.h"
#include "lib/library.h"
#include "host_link.h"
#include "msamp/cardq.h"
#include "engine.h"
#include "msamp/msamp.h"
#include "voices.h"

using namespace core;

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back(note); m.push_back(vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}
static void send(const std::vector<uint8_t>& m) { host_sysex(m.data(), (int)m.size()); }
static void off(int vi) { send({0xf0, 0x7d, 0x52, 0x54, 0x11, (uint8_t)vi, 0xf7}); }
static std::vector<float> ms(int inst) { return {(float)inst, 127, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0}; }
static constexpr int kFmB = 0x48;

static std::string g_card;
static void text(const std::string& path, const std::string& t) {
  FILE* fp = fopen(path.c_str(), "wb");
  CHECK(fp);
  fwrite(t.data(), 1, t.size(), fp);
  fclose(fp);
}
// frames of a WAV of tones no period of which repeats exactly (a seam or a gap shows)
static void wav(const std::string& path, int frames, int ch, int bits, int seed) {
  std::vector<uint8_t> b;
  auto le = [&b](uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back((uint8_t)(v >> (8 * i))); };
  int fb = ch * bits / 8;
  b.insert(b.end(), {'R', 'I', 'F', 'F'}), le(36 + (uint64_t)frames * fb, 4), b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  le(16, 4), le(1, 2), le(ch, 2), le(48000, 4), le(48000 * fb, 4), le(fb, 2), le(bits, 2);
  b.insert(b.end(), {'d', 'a', 't', 'a'}), le((uint64_t)frames * fb, 4);
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) {
      double x = 0.4 * std::sin(i * (0.011 + 0.003 * seed) + c) + 0.3 * std::sin(i * i * 1e-8 * (seed + 1)) + 0.1 * std::sin(i * 0.31);
      le((uint64_t)(int64_t)std::lround(x * ((1ll << (bits - 1)) - 1)), bits / 8);
    }
  FILE* fp = fopen(path.c_str(), "wb");
  CHECK(fp);
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
}
static constexpr int kSamples = 6;
static constexpr int kFrames = 960000;   // 20 s
// /MULTISAMPLE/<name>/<name>.sfz over the samples of /MULTISAMPLE/fp: key 48 + i plays sample i
static std::string inst(const char* name, const char* load) {
  std::string d = g_card + "/MULTISAMPLE/" + name;
  CHECK(system(("mkdir -p '" + d + "'").c_str()) == 0);
  std::string t = std::string("<control> sidecar_load=") + load + " default_path=../fp/\n<global> ampeg_release=0.05 loop_mode=no_loop\n";
  for (int i = 0; i < kSamples; ++i)
    t += "<region> sample=s" + std::to_string(i) + ".wav key=" + std::to_string(48 + i) + " pitch_keycenter=" + std::to_string(48 + i) + "\n";
  text(d + "/" + name + ".sfz", t);
  return std::string("/MULTISAMPLE/") + name + "/" + name + ".sfz";
}

// ---- the two cores' passes
constexpr int kOut = 48000 * 12;
static int16_t ring[kOut][kVoices];
static uint64_t g_us = 1000000;
static int g_w = 0;
static int g_serve = 8;       // requests the card core runs a block (0: stalled)
static uint64_t g_card3 = 0;
static bool g_idle = false;   // the engine's idle pass (core/engine.h), not msamp's own step
static void pass() {
  host_card_core(3);
  uint64_t c = host_card_us(3);
  if (g_idle) idle_stream();
  else for (int i = 0; i < 4 && msamp::step(); ++i) {}
  g_card3 += host_card_us(3) - c;
  host_card_core(0);
  for (int i = 0; i < g_serve && cardq::service(); ++i) {}
  host_card_core(3);
}
static void render(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) {
    pass();
    core::render_voices(ring, kOut, g_w, 0, core::kVoices, mach);
    g_w = (g_w + core::kBlock) % kOut;
    g_us += 250;
    host_fake_time(true, g_us);
  }
}
static void settle() {
  for (int i = 0; i < 400000; ++i) {
    render(1);
    if (!msamp::load_phase() && !msamp::keep_waiting() && !cardq::pending() && msamp::stats().streams == 0 && msamp::stats().active == 0 && i > 8) return;
  }
  CHECK(!"never settled");
}
static void restart() { g_w = 0, memset(ring, 0, sizeof ring); }
static int open_fds() {
  int n = 0;
  for (int fd = 0; fd < 1024; ++fd) n += fcntl(fd, F_GETFD) != -1;
  return n;
}
static int entry(const std::string& p) {
  for (int k = 0; k < msamp::kList; ++k)
    if (*msamp::list_path(k) && strstr(msamp::list_path(k), p.c_str())) return k;
  return -1;
}
static int g_row = 1;
static char g_ef_row[8];   // first.sfz's row
static int add(const std::string& path) {
  char r[8];
  snprintf(r, sizeof r, "A%03d", g_row++);
  if (path.find("/first/") != std::string::npos) snprintf(g_ef_row, sizeof g_ef_row, "%s", r);
  bool w = cardq::worker();
  cardq::set_worker(false);   // (the pool's own card work, step by step: no card core in this helper)
  CHECK(pu::put(pools::kInstr, r, path.c_str()) == pools::kOk);
  cardq::set_worker(w);
  int e = entry(path);
  CHECK(e >= 0);
  return e;
}
static void select(int e, int vi = 0) {
  send(report(0x03, kFmB | vi, -1, 0, ms(e)));
  for (int i = 0; i < 2000000 && (msamp::voice_slot(vi) < 0 || msamp::load_phase()); ++i) render(1);
  CHECK(msamp::voice_slot(vi) >= 0);
}
static void trig(int e, int key, int vi = 0) { send(report(0x04, kFmB | vi, key, 100, ms(e))); }
static uint32_t card_reads() { return cardq::stats().ran[cardq::kReadReq]; }
static uint32_t card_all() {
  uint32_t n = 0;
  for (int k = 0; k < cardq::kKinds; ++k) n += cardq::stats().ran[k];
  return n;
}
// A phrase: keys one after the other, 1 s each, the next trig cutting the last (its tail going on); the voice's output
static std::vector<int16_t> phrase(int e, const std::vector<int>& keys, int blocks = 4000) {
  restart();
  std::vector<int16_t> o;
  for (int k : keys) {
    trig(e, k);
    int w0 = g_w;
    render(blocks);
    for (int i = w0; i != g_w; i = (i + 1) % kOut) o.push_back(ring[i][0]);
  }
  off(0);
  int w0 = g_w;
  render(4000);
  for (int i = w0; i != g_w; i = (i + 1) % kOut) o.push_back(ring[i][0]);
  return o;
}
static double rms_at(const std::vector<int16_t>& o, size_t a, size_t n) {
  double s = 0;
  for (size_t i = a; i < a + n; ++i) s += (double)o[i] * o[i];
  return std::sqrt(s / n);
}

static void forget_all(const char* dir) {
  for (int i = 0; i < kSamples; ++i) {
    lib::Ref r;
    lib::ref_for_path((std::string("/MULTISAMPLE/") + dir + "/s" + std::to_string(i) + ".wav").c_str(), &r);
    pcache::forget(r);
  }
}
static bool whole(const char* dir, int i) {
  lib::Ref r;
  lib::ref_for_path((std::string("/MULTISAMPLE/") + dir + "/s" + std::to_string(i) + ".wav").c_str(), &r);
  return pcache::find(r, pcache::kWhole) >= 0;
}
static void unload(const char* dir) {   // no slot, nothing of it in RAM
  for (int v = 0; v < 2; ++v) msamp::silence(v);
  char o[80];
  CHECK(msamp::command("msamp=rescan", o, sizeof o));
  settle();
  forget_all(dir);
}
static void voice1_only(std::vector<int16_t>& out, int blocks) {
  for (int i = 0; i < blocks * core::kBlock; ++i) out.push_back(ring[i][1]);
}

int main() {
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  setenv("CORE_POOL_RAM_MB", "24", 1);
  setenv("CORE_SAMPLE_RAM_MB", "8", 1);
  char tmpl[] = "/tmp/firstplay-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/MULTISAMPLE/fp' '" + g_card + "/.rytm' '" + g_card + "/.sidecar'").c_str()) == 0);
  for (int i = 0; i < kSamples; ++i) wav(g_card + "/MULTISAMPLE/fp/s" + std::to_string(i) + ".wav", kFrames, 2, 24, i);
  std::string stream = inst("streamed", "stream"), ram = inst("inram", "ram"), first = inst("first", "auto");
  // another instrument in RAM, and a sample of its own (other contents than fp's: nothing of it in the cache)
  CHECK(system(("mkdir -p '" + g_card + "/MULTISAMPLE/fq' '" + g_card + "/MULTISAMPLE/other'").c_str()) == 0);
  wav(g_card + "/MULTISAMPLE/fq/y.wav", kFrames / 2, 1, 16, 21);
  wav(g_card + "/MULTISAMPLE/fq/x.wav", kFrames / 2, 1, 16, 22);
  text(g_card + "/MULTISAMPLE/other/other.sfz", "<control> sidecar_load=ram default_path=../fq/\n<region> sample=y.wav key=48 pitch_keycenter=48\n");
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  voices_init();
  card_ok = true;
  srclib::begin(true);
  pools::begin(true);
  msamp::begin(true);
  pu::steps();
  int es = add(stream), er = add(ram), ef = add(first), eo = add("/MULTISAMPLE/other/other.sfz");
  char o[300];
  CHECK(msamp::command("msamp=voice:0", o, sizeof o));
  CHECK(msamp::command("msamp=voice:1", o, sizeof o));
  msamp::set_head_ms(100);
  pcache::set_mono(true);   // (the RAM reference fits half the budget summed; stereo for the eviction below)
  cardq::set_worker(true);
  host_card_model(true);
  const int fds0 = open_fds();
  const std::vector<int> keys = {48, 49, 50};

  // ---- the references: held whole, and purely streamed (sidecar_load=stream: never kept)
  select(er);
  std::vector<int16_t> ref_ram = phrase(er, keys);
  unload("fp");
  select(es);
  std::vector<int16_t> ref_str = phrase(es, keys);
  CHECK(ref_ram == ref_str);   // (a streamed instrument plays what a RAM one plays, as ever)
  CHECK(msamp::stats().kept == 0 && msamp::stats().fills == 0);
  CHECK(msamp::stats().underruns == 0);
  unload("fp");

  // ---- a project load: the heads, nothing whole
  uint32_t r0 = card_reads(), rd0;
  uint64_t used0 = pcache::stats().used;
  select(ef);
  uint64_t used1 = pcache::stats().used;
  CHECK(used1 - used0 < 1u << 20);
  CHECK(card_reads() - r0 <= 6 * 3);
  CHECK(msamp::stats().kept == 0 && !msamp::keep_waiting());

  // ---- first play: at once, no gap, and the data lands in RAM
  g_card3 = 0;
  rd0 = card_reads();
  uint32_t op0 = cardq::stats().ran[cardq::kOpenReq], cl0 = cardq::stats().ran[cardq::kCloseReq];
  std::vector<int16_t> p1 = phrase(ef, keys);
  CHECK(p1 == ref_ram);   // bit-identical through head, ring and the change of the ring's source
  CHECK(msamp::stats().underruns == 0);
  settle();
  CHECK(msamp::stats().kept == 3 && msamp::stats().fills == 3);
  CHECK(msamp::stats().ram_feeds > 0);   // the rings fed from the fill, not the card
  uint32_t first_reads = card_reads() - rd0;
  // each frame read about once: three files in 32 KB reads, a stream's few before its fill got past it on top
  uint32_t one = (uint32_t)((uint64_t)(kFrames - 4800) * 6 / 32768) + 1;
  printf("test_firstplay: first play of 3 samples: %u card reads (%u a file), %u by the fills, %u by the streams, %u ring feeds from RAM\n",
         first_reads, one, msamp::stats().fill_reads, msamp::stats().stream_reads, msamp::stats().ram_feeds);
  CHECK(first_reads <= 3 * one + 3 * 4);
  CHECK(pcache::stats().used > used1 + 3ull * (kFrames - 9600) * 2);
  CHECK(whole("fp", 0) && whole("fp", 1) && whole("fp", 2));
  // every file closed on the card core, the streams' too
  CHECK(cardq::stats().ran[cardq::kOpenReq] > op0);
  CHECK(cardq::stats().ran[cardq::kCloseReq] - cl0 == cardq::stats().ran[cardq::kOpenReq] - op0);
  CHECK(msamp::stats().closes_here == 0);

  // ---- the second play: no card at all, the same audio
  rd0 = card_all();
  uint32_t reads_str = msamp::stats().stream_reads;
  std::vector<int16_t> p2 = phrase(ef, keys);
  CHECK(card_all() == rd0);   // zero: no open, no read, no close
  CHECK(msamp::stats().stream_reads == reads_str);
  CHECK(p2 == ref_ram);
  CHECK(msamp::stats().streams == 0 && msamp::stats().underruns == 0);
  CHECK(msamp::stats().fills == 3);   // read once
  CHECK(msamp::stats().kept == 3);

  // ---- notes in flight across the change: a long note of a new sample sounds on while its fill ends under it, a
  // second note of it comes during the fill, a third after: bit-identical to the streamed instrument's
  {
    auto notes3 = [](int e) {
      restart();
      trig(e, 51);
      render(200);      // 50 ms: the fill has begun
      trig(e, 51);
      render(8000);     // 2 s: it has ended
      trig(e, 51);
      render(16000);
      std::vector<int16_t> a;
      for (int i = 0; i < g_w; ++i) a.push_back(ring[i][0]);
      off(0);
      settle();
      return a;
    };
    std::vector<int16_t> a = notes3(ef);
    CHECK(msamp::stats().kept == 4 && msamp::stats().underruns == 0);
    select(es);
    std::vector<int16_t> b = notes3(es);
    CHECK(a == b);
    printf("test_firstplay: three notes across the change: %zu frames equal\n", a.size());
    select(ef);
  }

  // ---- a short note keeps the whole sample all the same
  {
    trig(ef, 52);
    render(40);
    off(0);
    settle();
    CHECK(whole("fp", 4));
  }

  // ---- a stream under pressure beats the fill
  {
    unload("fp");
    select(ef);
    const uint32_t f0 = msamp::stats().fill_reads, s0 = msamp::stats().stream_reads;
    // the card core held for 40 ms from the note on, then a read each 2 ms: the stream first, no underrun
    g_serve = 0;
    trig(ef, 48);
    render(160);
    CHECK(msamp::stats().fill_reads == f0 && cardq::pending() >= 1 && cardq::busy());
    for (int i = 0; i < 40000 && msamp::stats().fill_reads == f0; ++i) {
      g_serve = i % 8 == 0;
      render(1);
    }
    CHECK(msamp::stats().fill_reads > f0 && msamp::stats().stream_reads > s0);
    g_serve = 8;
    off(0);
    settle();
    CHECK(msamp::stats().underruns == 0);
    unload("fp");
    select(ef);
    // a card slower than the stream needs (a read of 32 KB each 150 ms: 218 KB/s, it wants 288): it never gets above its
    // low mark, and the fill gets nothing, for 2 s
    const uint32_t f1 = msamp::stats().fill_reads, s1 = msamp::stats().stream_reads;
    trig(ef, 49);
    for (int i = 0; i < 8000; ++i) {
      g_serve = i % 600 == 0;
      render(1);
    }
    CHECK(msamp::stats().fill_reads == f1);
    CHECK(msamp::stats().stream_reads >= s1 + 8);
    printf("test_firstplay: a card slower than the stream: %u stream reads, %u fill reads in 2 s\n", msamp::stats().stream_reads - s1,
           msamp::stats().fill_reads - f1);
    g_serve = 8;
    off(0);
    settle();
  }

  // ---- a project change abandons the fill: mid-fill (a request out, the card core held), the row's instrument gone
  {
    unload("fp");
    select(ef);
    uint64_t used = pcache::stats().used;
    int kept0 = msamp::stats().kept;
    const uint32_t f0 = msamp::stats().fill_reads;
    trig(ef, 50);
    for (int i = 0; i < 40000 && msamp::stats().fill_reads < f0 + 2; ++i) render(1);
    CHECK(msamp::stats().fill_reads >= f0 + 2 && msamp::keep_waiting() == 1);
    uint64_t during = pcache::stats().used;
    CHECK(during > used + (1u << 20));   // the block being filled
    g_serve = 0;
    render(4);                         // (a request out)
    CHECK(pu::put(pools::kInstr, g_ef_row, "") == pools::kOk);   // gone from the list, as in a new project
    g_serve = 8;
    render(40);
    for (int i = 0; i < 4000 && msamp::keep_waiting(); ++i) render(1);
    CHECK(msamp::keep_waiting() == 0);
    CHECK(msamp::stats().kept == kept0);
    CHECK(pcache::stats().used < during);   // its block gone
    CHECK(!whole("fp", 2));
    msamp::silence(0);
    settle();
    ef = add(first);
    select(ef);
    CHECK(open_fds() == fds0);
    trig(ef, 50);        // the instrument back in the list: the sample is kept after all
    render(40);
    off(0);
    settle();
    CHECK(whole("fp", 2));
  }

  // ---- a real project change: the slot goes, its fill with it; what was kept stays in the cache as today (LRU), and
  // coming back it plays from RAM at once
  {
    trig(ef, 48);
    render(40);
    off(0);
    settle();
    CHECK(whole("fp", 0));
    trig(ef, 49);
    render(4);
    CHECK(msamp::keep_waiting() == 1);
    msamp::silence(0);
    render(40);
    bool w = cardq::worker();
    cardq::set_worker(false);
    for (int i = 0; i < 3000 && pools::saving(); ++i) pu::steps(1), g_us += 1000, host_fake_time(true, g_us);
    snprintf(core::prj().key, sizeof core::prj().key, "%s", "ELSEWHERE");
    for (int i = 0; i < 4000 && (strcmp(pools::project(), "ELSEWHERE") || !pools::ready()); ++i) pu::steps(1), g_us += 1000, host_fake_time(true, g_us);
    CHECK(!strcmp(pools::project(), "ELSEWHERE"));
    cardq::set_worker(w);
    for (int i = 0; i < 4000 && msamp::keep_waiting(); ++i) render(1);
    CHECK(msamp::keep_waiting() == 0 && msamp::voice_slot(0) < 0);
    CHECK(whole("fp", 0) && !whole("fp", 1));   // the whole one stays, the fill's is gone
    cardq::set_worker(false);
    snprintf(core::prj().key, sizeof core::prj().key, "%s", "");
    for (int i = 0; i < 4000 && (strcmp(pools::project(), "_default") || !pools::ready()); ++i) pu::steps(1), g_us += 1000, host_fake_time(true, g_us);
    cardq::set_worker(w);
    settle();
    select(ef);
    uint32_t c0 = card_reads();
    std::vector<int16_t> p = phrase(ef, {48});
    CHECK(card_reads() == c0);   // from RAM, no card
    (void)p;
  }

  // ---- eviction under a small budget (24 MB less 7.5 MB; stereo, 3.7 MB each): the least recently played goes back to
  // its head, never one a note plays; the budget holds; a sample given back plays right, and is kept again
  {
    unload("fp");
    void* ballast_p = nullptr;
    int ballast = pcache::alloc_private(7500000, &ballast_p);
    CHECK(ballast >= 0);
    pcache::set_mono(false);
    msamp::set_keep(false);
    select(ef);
    auto v1 = [](int e) {   // sample 0's long note on voice 1, the rest on voice 0
      restart();
      trig(e, 48, 1);
      render(2000);
      for (int k : {49, 50, 51, 52, 53}) {
        trig(e, k);
        render(6000);
        off(0);
      }
      std::vector<int16_t> a;
      voice1_only(a, g_w / core::kBlock);
      for (int v = 0; v < 2; ++v) msamp::silence(v);
      settle();
      return a;
    };
    std::vector<int16_t> a0 = v1(ef);   // the reference: nothing kept, streamed
    std::vector<int16_t> r49 = phrase(ef, {49});
    CHECK(msamp::stats().kept == 0);
    unload("fp");
    msamp::set_keep(true);
    select(ef);
    const uint32_t g0 = msamp::stats().keep_given_back, u0 = msamp::stats().underruns;
    restart();
    trig(ef, 48, 1);
    render(2000);
    uint64_t top = 0;
    int low = 99;
    for (int k : {49, 50, 51, 52, 53}) {
      if (k == 52) {   // (the first three now idle for longer than a fill's minimum)
        g_us += 31000000;
        host_fake_time(true, g_us);
      }
      trig(ef, k);
      for (int i = 0; i < 6000; ++i) {
        render(1);
        if (pcache::stats().used > top) top = pcache::stats().used;
      }
      off(0);
      CHECK(whole("fp", 0));   // the sample a note plays stays, however old
      low = msamp::stats().kept < low ? msamp::stats().kept : low;
    }
    std::vector<int16_t> a1;
    voice1_only(a1, g_w / core::kBlock);
    CHECK(a1 == a0);   // and sounds as it would
    CHECK(top <= pcache::stats().budget);
    CHECK(msamp::stats().keep_given_back > g0);
    printf("test_firstplay: small budget: %u given back, %d kept, %llu of %llu KB at most\n", msamp::stats().keep_given_back - g0,
           msamp::stats().kept, (unsigned long long)(top >> 10), (unsigned long long)(pcache::stats().budget >> 10));
    CHECK(!whole("fp", 1));   // the oldest not playing went first
    for (int v = 0; v < 2; ++v) msamp::silence(v);
    settle();
    std::vector<int16_t> b49 = phrase(ef, {49});   // given back: head + stream, right; kept again (another goes)
    CHECK(b49 == r49);
    settle();
    CHECK(whole("fp", 1));
    CHECK(msamp::stats().underruns == u0);
    // a working set bigger than the pool: all played lately: no fill takes room from another: no swapping, the card read
    // once; what does not fit streams
    for (int v = 0; v < 2; ++v) msamp::silence(v);
    settle();
    const uint32_t g1 = msamp::stats().keep_given_back, f1 = msamp::stats().fills;
    for (int round = 0; round < 3; ++round)
      for (int k = 48; k < 48 + kSamples; ++k) {
        trig(ef, k);
        render(16000);   // (4 s: past a refused sample's retry)
        off(0);
        settle();
      }
    CHECK(msamp::stats().keep_given_back == g1);
    CHECK(msamp::stats().fills - f1 <= 1 && msamp::stats().keep_refused > 0);
    CHECK(msamp::stats().underruns == u0);
    printf("test_firstplay: working set over the pool: %u fills in 3 rounds, %u refused, none given back\n", msamp::stats().fills - f1, msamp::stats().keep_refused);
    pcache::set_mono(true);
    pcache::release(ballast);
  }

  // ---- kept samples yield to every path that needs room: a sidecar_load=ram instrument and a machine's want, the pool
  // RAM full of them in a resident slot no voice selects: nothing left out, no failure, the slot's samples play on
  {
    unload("fp");
    void* bp = nullptr;
    int ballast = pcache::alloc_private(13500000, &bp);
    CHECK(ballast >= 0);
    select(ef);
    for (int k = 48; k < 48 + kSamples; ++k) {
      trig(ef, k);
      render(20);
      off(0);
      settle();
    }
    CHECK(msamp::stats().kept == kSamples);
    render(5000);   // (older than the cache's guard)
    g_us += 2000000;
    host_fake_time(true, g_us);
    const uint32_t g0 = msamp::stats().keep_given_back, l0 = msamp::stats().left_out;
    select(eo);
    CHECK(msamp::stats().left_out == l0 && msamp::stats().keep_given_back > g0);
    CHECK(msamp::slot_ram(msamp::voice_slot(0)));
    trig(eo, 48);
    render(400);
    off(0);
    settle();
    lib::Ref r;
    lib::ref_for_path("/MULTISAMPLE/fq/x.wav", &r);
    render(4000);
    g_us += 2000000;
    host_fake_time(true, g_us);
    int e = pcache::want(r, "/MULTISAMPLE/fq/x.wav");   // as GRANULAR's and CHOP's
    for (int i = 0; i < 8000 && pcache::state(e) == pcache::kLoading; ++i) pcache::step(), render(1);
    CHECK(pcache::state(e) == pcache::kReady && !pcache::no_room(e));
    pcache::release(ballast);
    msamp::silence(0);
    render(10);
    CHECK(msamp::command("msamp=rescan", o, sizeof o));
    settle();
    select(ef);
    std::vector<int16_t> ra = phrase(ef, {49});   // given back, and right
    settle();
    select(es);
    CHECK(ra == phrase(es, {49}));
    settle();
  }

  // ---- a fill's reads paced while a DSP SAMPLE stream plays (it reads the card past the queue); a machine waiting for
  // the sample that fills is not left to the low marks
  {
    unload("fp");
    select(ef);
    msamp::pace_for_tests(true);
    uint32_t f0 = msamp::stats().fill_reads;
    trig(ef, 48);
    render(4000);   // 1 s: 50 reads at most (and not one with a stream to feed: the note wants its reads first)
    uint32_t paced = msamp::stats().fill_reads - f0;
    CHECK(paced <= 51 && paced > 10);
    msamp::pace_for_tests(false);
    off(0);
    settle();
    unload("fp");
    select(ef);
    lib::Ref r;
    lib::ref_for_path("/MULTISAMPLE/fp/s1.wav", &r);
    msamp::set_stream_limits(600, 32);   // (a long read-ahead: a low mark the card below can't reach)
    trig(ef, 49);
    render(4);
    CHECK(msamp::keep_waiting() == 1);
    int e = pcache::want(r, "/MULTISAMPLE/fp/s1.wav");
    CHECK(pcache::busy(e));
    uint32_t f1 = msamp::stats().fill_reads;
    for (int i = 0; i < 1600; ++i) {
      g_serve = i % 40 == 0;
      render(1);
    }
    g_serve = 8;
    CHECK(msamp::stats().fill_reads > f1);
    msamp::set_stream_limits(300, 32);
    off(0);
    settle();
  }

  // ---- a slot that plays on through a project change, a load in the way: the fill still ends, at once
  {
    unload("fp");
    select(ef);
    CHECK(msamp::stats().active == 0);
    uint32_t f0 = msamp::stats().fill_reads;
    trig(ef, 50);
    for (int i = 0; i < 40000 && msamp::stats().fill_reads < f0 + 2; ++i) render(1);
    uint64_t during = pcache::stats().used;
    CHECK(pu::put(pools::kInstr, g_ef_row, "") == pools::kOk);   // the note plays on, its instrument out of the list
    msamp::silence(1);
    CHECK(msamp::command("msamp=rescan", o, sizeof o));
    g_serve = 0;
    send(report(0x03, kFmB | 1, -1, 0, ms(es)));   // a voice's load: the job busy from here on
    render(8);
    CHECK(msamp::load_phase() != 0);
    g_serve = 1;
    render(6);
    CHECK(msamp::load_phase() != 0 && msamp::keep_waiting() == 0);
    CHECK(pcache::stats().used < during);
    g_serve = 8;
    msamp::silence(0);
    msamp::silence(1);
    settle();
    ef = add(first);
  }

  // ---- the engine's order: a whole load of the shared cache (DSP SAMPLE's, GRANULAR's) under way does not keep the
  // streams from running (its step says true while it waits for the card)
  {
    unload("fp");
    sample_ram_init();
    CHECK(psram);
    select(ef);
    g_idle = true;
    const uint32_t u0 = msamp::stats().underruns;
    lib::Ref r;
    std::string p = "/MULTISAMPLE/fp/s5.wav";
    lib::ref_for_path(p.c_str(), &r);
    int e = pcache::want(r, p.c_str());
    CHECK(e >= 0);
    trig(ef, 48);
    for (int i = 0; i < 16000; ++i) {
      g_serve = i % 8 == 0;
      render(1);
    }
    CHECK(pcache::state(e) == pcache::kReady && msamp::stats().underruns == u0);
    g_serve = 8;
    g_idle = false;
    off(0);
    settle();
    forget_all("fp");
  }
  // ---- the card test's stream limits read at begin through the card core
  {
    text(g_card + "/.sidecar/sdtest.txt", "verdict=x\nahead_ms=250\nstreams=12\n");
    msamp::begin(true);
    for (int i = 0; i < 1000 && msamp::load_phase(); ++i) render(1);
    render(100);
    CHECK(msamp::command("msamp", o, sizeof o));
    CHECK(strstr(o, "max 12, ahead 250 ms"));
    CHECK(msamp::command("msamp=voice:0", o, sizeof o));
    pu::steps();
    ef = entry(first);
    CHECK(ef >= 0);
  }
  printf("test_firstplay: control core card time %llu us\n", (unsigned long long)g_card3);
  CHECK(g_card3 == 0);
  unload("fp");
  CHECK(open_fds() == fds0);
  cardq::set_worker(false);
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_firstplay: PASS\n");
  return 0;
}
