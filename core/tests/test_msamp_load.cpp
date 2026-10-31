// core/tests/test_msamp_load.cpp - MULTISAMPLE's load job (core/msamp/msamp.cpp: the .sfz's text, every sample's
// header, the heads and loop parts) on the card queue (core/msamp/cardq.h), a request a step. An instrument of many
// formats (WAV 8/16/24/32-bit, float, extensible, a LIST chunk past the first block before fmt and before data, a smpl
// chunk past the data, an odd-sized chunk's pad byte, AIFF, AIFC sowt / fl32, a data chunk longer than its file, a
// smpl cut short, an unsupported format, a file too short for a header, a missing sample) loads, held whole (RAM) and
// streamed (heads and loop parts), to the same slot contents (regions, sample paths, formats, parts, int16 data:
// msamp::slot_digest) and the same log lines as the load did when it read the card on the control core (the
// reference hashes below: captured from that code). Without a card core (each request at once) and with one
// (cardq::service() run between the control core's steps): then the control core spends no card time (the card
// model, host_card_us), and no step converts more than a bounded piece (each step timed). A load abandoned mid-read
// (msamp::begin again: a request posted and not run, one run and not taken in, one in the headers) settles its
// request and closes its file; the next load of it is whole and the same. An .sfz gone from the card fails as before.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
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
static std::vector<float> ms(int inst) { return {(float)inst, 127, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0}; }
static constexpr int kFmB = 0x48;

// ---- the files
using Bytes = std::vector<uint8_t>;
static void le(Bytes& b, uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void be(Bytes& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
static void put(Bytes& b, const char* s) { b.insert(b.end(), s, s + strlen(s)); }
struct Chunk {
  std::string id;
  Bytes body;
  uint32_t size = ~0u;   // the size the header says (~0u: the body's)
  Chunk(const char* i, Bytes b = {}) : id(i), body(std::move(b)) {}
};
static void write(const std::string& path, const Bytes& b, size_t cut = 0) {
  FILE* fp = fopen(path.c_str(), "wb");
  CHECK(fp);
  fwrite(b.data(), 1, cut && cut < b.size() ? cut : b.size(), fp);
  fclose(fp);
}
static Bytes form(bool wav, const char* type, const std::vector<Chunk>& cs) {
  Bytes b;
  put(b, wav ? "RIFF" : "FORM");
  size_t at = b.size();
  le(b, 0, 4);
  put(b, type);
  for (const Chunk& c : cs) {
    put(b, c.id.c_str());
    uint32_t sz = c.size != ~0u ? c.size : (uint32_t)c.body.size();
    if (wav) le(b, sz, 4); else be(b, sz, 4);
    b.insert(b.end(), c.body.begin(), c.body.end());
    if (c.body.size() & 1) b.push_back(0);
  }
  uint32_t total = (uint32_t)b.size() - 8;
  for (int i = 0; i < 4; ++i) b[at + i] = wav ? (uint8_t)(total >> (8 * i)) : (uint8_t)(total >> (8 * (3 - i)));
  return b;
}
// frames of a seeded tone, each channel its own phase: bits 8 (unsigned), 16, 24, 32 (integer), or float (32)
static Bytes pcm(int frames, int ch, int bits, bool flt, bool big, int seed, double f) {
  Bytes b;
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) {
      double x = 0.7 * std::sin(2 * M_PI * f * i / 48000.0 + c + seed) + 0.2 * std::sin(0.013 * i * (seed + 1));
      if (flt) {
        float y = (float)(x * 1.2);   // (over full scale now and then: clipped at load)
        uint32_t u;
        memcpy(&u, &y, 4);
        if (big) be(b, u, 4); else le(b, u, 4);
        continue;
      }
      int64_t v = (int64_t)std::lround(x * ((1ll << (bits - 1)) - 1));
      if (bits == 8) v += 128;
      if (big) be(b, (uint64_t)v, bits / 8); else le(b, (uint64_t)v, bits / 8);
    }
  return b;
}
static Chunk fmt(int ch, int rate, int bits, int tag = 1, bool ext = false) {
  Chunk c{"fmt "};
  int bps = bits / 8;
  le(c.body, ext ? 0xFFFE : tag, 2), le(c.body, ch, 2), le(c.body, rate, 4), le(c.body, rate * ch * bps, 4);
  le(c.body, ch * bps, 2), le(c.body, bits, 2);
  if (ext) {
    le(c.body, 22, 2), le(c.body, bits, 2), le(c.body, ch == 2 ? 3 : 4, 4);
    le(c.body, tag, 2);   // the SubFormat GUID's first two bytes
    static const uint8_t rest[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    c.body.insert(c.body.end(), rest, rest + 14);
  }
  return c;
}
static Chunk smpl(uint32_t s, uint32_t e, int loops = 1) {
  Chunk c{"smpl"};
  for (int i = 0; i < 7; ++i) le(c.body, 0, 4);
  le(c.body, loops, 4), le(c.body, 0, 4);
  for (int k = 0; k < loops; ++k) le(c.body, k, 4), le(c.body, 0, 4), le(c.body, s, 4), le(c.body, e, 4), le(c.body, 0, 4), le(c.body, 0, 4);
  return c;
}
static Chunk junk(const char* id, size_t n, int seed) {
  Chunk c{id};
  for (size_t i = 0; i < n; ++i) c.body.push_back((uint8_t)(i * 7 + seed));
  return c;
}
static void ext80(Bytes& b, uint32_t rate) {
  int e = 31;
  while (!(rate >> e)) --e;
  be(b, 16383 + e, 2);
  be(b, (uint64_t)rate << (63 - e), 8);
}
static Bytes aiff(bool aifc, const char* comp, int ch, int rate, int bits, const Bytes& data, int frames) {
  Chunk comm{"COMM"};
  be(comm.body, ch, 2), be(comm.body, frames, 4), be(comm.body, bits, 2);
  ext80(comm.body, rate);
  if (aifc) put(comm.body, comp), comm.body.push_back(0), comm.body.push_back(0);   // (an empty pstring, padded)
  Chunk ssnd{"SSND"};
  be(ssnd.body, 4, 4), be(ssnd.body, 0, 4);
  le(ssnd.body, 0xdeadbeef, 4);   // the offset's 4 bytes before the frames
  ssnd.body.insert(ssnd.body.end(), data.begin(), data.end());
  std::vector<Chunk> cs;
  if (aifc) {
    Chunk fver{"FVER"};
    be(fver.body, 0xA2805140u, 4);
    cs.push_back(fver);
  }
  cs.push_back(comm), cs.push_back(ssnd);
  return form(false, aifc ? "AIFC" : "AIFF", cs);
}

static std::string g_card;
// Instrument seed s at /MULTISAMPLE/ld<s>/ld<s>.sfz (load: "ram" or "stream"); every file's frames are the seed's
static std::string make_inst(int s, const char* load) {
  std::string dir = g_card + "/MULTISAMPLE/ld" + std::to_string(s), d = dir + "/";
  CHECK(system(("mkdir -p '" + dir + "'").c_str()) == 0);
  write(d + "a16.wav", form(true, "WAVE", {fmt(1, 48000, 16), {"data", pcm(30000, 1, 16, false, false, s, 220)}, smpl(10000, 25000)}));
  write(d + "b24s.wav", form(true, "WAVE", {fmt(2, 44100, 24), {"data", pcm(40000, 2, 24, false, false, s, 330)}}));
  write(d + "c8.wav", form(true, "WAVE", {fmt(1, 22050, 8), {"data", pcm(5000, 1, 8, false, false, s, 440)}}));
  {
    Chunk fact{"fact"};
    le(fact.body, 20000, 4);
    write(d + "d32f.wav", form(true, "WAVE", {fmt(2, 48000, 32, 3), fact, {"data", pcm(20000, 2, 32, true, false, s, 550)}}));
  }
  write(d + "e_ext.wav", form(true, "WAVE", {fmt(2, 48000, 16, 1, true), {"data", pcm(9000, 2, 16, false, false, s, 660)}}));
  // a LIST of 20 KB between fmt and data, a smpl after the data: both past the first block
  write(d + "f_list.wav", form(true, "WAVE", {fmt(1, 48000, 16), junk("LIST", 20000, s), {"data", pcm(15000, 1, 16, false, false, s, 770)}, smpl(3000, 14000)}));
  // a LIST of 9001 bytes (odd: a pad byte) before fmt: fmt and data past the first block
  write(d + "g_listfirst.wav", form(true, "WAVE", {junk("LIST", 9001, s), fmt(1, 48000, 24), {"data", pcm(7000, 1, 24, false, false, s, 880)}}));
  write(d + "h.aif", aiff(false, "", 1, 32000, 16, pcm(8000, 1, 16, false, true, s, 990), 8000));
  write(d + "i.aifc", aiff(true, "sowt", 2, 44100, 16, pcm(6000, 2, 16, false, false, s, 1100), 6000));
  write(d + "j.aifc", aiff(true, "fl32", 1, 48000, 32, pcm(5000, 1, 32, true, true, s, 1210), 5000));
  {   // the data chunk says 50000 frames, the file ends after 12000 (and half a frame)
    Bytes b = form(true, "WAVE", {fmt(1, 48000, 16), {"data", pcm(50000, 1, 16, false, false, s, 1320)}});
    write(d + "k_short.wav", b, 44 + 12000 * 2 + 1);
  }
  write(d + "l_bad.wav", form(true, "WAVE", {fmt(1, 48000, 12), {"data", pcm(1000, 1, 16, false, false, s, 1430)}}));
  write(d + "m_tiny.wav", Bytes{'R', 'I', 'F', 'F', 0, 0, 0, 0});
  write(d + "n32.wav", form(true, "WAVE", {fmt(1, 96000, 32), {"data", pcm(10000, 1, 32, false, false, s, 1540)}}));
  {   // a smpl whose loops the file cuts off: no loop
    Bytes b = form(true, "WAVE", {fmt(1, 48000, 16), {"data", pcm(4000, 1, 16, false, false, s, 1650)}, smpl(100, 3000)});
    write(d + "o_smplcut.wav", b, b.size() - 20);
  }
  const char* smp[] = {"a16.wav", "b24s.wav", "c8.wav", "d32f.wav", "e_ext.wav", "f_list.wav", "g_listfirst.wav", "h.aif",
                       "i.aifc", "j.aifc", "k_short.wav", "l_bad.wav", "m_tiny.wav", "nope.wav", "n32.wav", "o_smplcut.wav"};
  std::string t = std::string("<control> sidecar_load=") + load + "\n<global> ampeg_release=0.2\n";
  for (int i = 0; i < 16; ++i) {
    int k = 36 + 2 * i;
    t += "<region> sample=" + std::string(smp[i]) + " lokey=" + std::to_string(k) + " hikey=" + std::to_string(k + 1) +
         " pitch_keycenter=" + std::to_string(k);
    if (i == 2) t += " loop_start=100 loop_end=4000 loop_mode=loop_continuous";
    t += "\n";
  }
  t += "<region> sample=a16.wav key=100 lovel=1 hivel=63 offset=500 volume=-6\n";   // a second region on a sample
  write(d + "ld" + std::to_string(s) + ".sfz", Bytes(t.begin(), t.end()));
  return "/MULTISAMPLE/ld" + std::to_string(s) + "/ld" + std::to_string(s) + ".sfz";
}

// ---- the log: the load's lines (its time left out), hashed
static uint64_t g_log = 1469598103934665603ull;
static int g_loaded_lines = 0;
static bool g_capture = false;
static void sink(const char* l, int n) {
  std::string s(l, n);
  if (g_capture) fprintf(stderr, "log: %s\n", s.c_str());
  if (s.rfind("msamp: ", 0) != 0 || s.find("/ld") == std::string::npos) return;
  if (s.find("msamp: loaded") == 0) ++g_loaded_lines;
  size_t at = s.find(" ms; ");
  if (at != std::string::npos) {
    size_t b = at;
    while (b > 0 && isdigit((unsigned char)s[b - 1])) --b;
    s.erase(b, at - b);
  }
  for (char c : s) g_log = (g_log ^ (uint8_t)c) * 1099511628211ull;
}

// ---- the control core's steps, a card core's service between them
static bool g_worker = false;
static uint64_t g_card3 = 0;
static double g_max_step_us = 0, g_sum_step_us = 0;
static uint32_t g_steps = 0;
static int g_phase_max[5];   // the longest step (us) by the load's phase it started in
static bool step3() {
  host_card_core(3);
  uint64_t c = host_card_us(3);
  int ph = msamp::load_phase();
  auto t0 = std::chrono::steady_clock::now();
  bool did = msamp::step();
  double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  if (us > g_phase_max[ph]) g_phase_max[ph] = (int)us;
  if (us > g_max_step_us) g_max_step_us = us;
  g_sum_step_us += us, ++g_steps;
  g_card3 += host_card_us(3) - c;
  return did;
}
static bool serve() {
  host_card_core(0);
  bool r = cardq::service() != nullptr;
  host_card_core(3);
  return r;
}
static void settle() {   // until the job is idle and nothing waits
  int quiet = 0;
  for (int i = 0; i < 400000; ++i) {
    bool did = step3();
    bool s = g_worker && serve();
    if (did || s || cardq::pending() || msamp::load_phase()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"msamp never settled");
}
static int open_fds() {   // the process's open files (a file the load left open shows here)
  int n = 0;
  for (int fd = 0; fd < 1024; ++fd) n += fcntl(fd, F_GETFD) != -1;
  return n;
}
static int entry(const char* p) {
  for (int k = 0; k < msamp::kList; ++k)
    if (*msamp::list_path(k) && strstr(msamp::list_path(k), p)) return k;
  return -1;
}
static int g_row = 1;
static int add(const std::string& path) {   // into the INSTRUMENTS pool: its list entry
  char r[8];
  snprintf(r, sizeof r, "A%03d", g_row++);
  CHECK(pu::put(pools::kInstr, r, path.c_str()) == pools::kOk);
  int e = entry(path.c_str());
  CHECK(e >= 0);
  return e;
}
static void select(int e) { send(report(0x03, kFmB | 0, -1, 0, ms(e))); }

struct Result {
  uint64_t digest, log;
  uint32_t loads, fails;
};
// A load of entry e on voice 0 to its end: the slot's digest and the log's hash
static Result load(int e) {
  uint32_t l0 = msamp::stats().loads, f0 = msamp::stats().load_fails;
  g_log = 1469598103934665603ull;
  select(e);
  settle();
  Result r{0, g_log, msamp::stats().loads - l0, msamp::stats().load_fails - f0};
  int s = msamp::voice_slot(0);
  if (s >= 0) r.digest = msamp::slot_digest(s);
  return r;
}
static void unload() {   // nothing resident (the heads stay in the cache)
  select(0x7f);
  char o[64];
  msamp::command("msamp=rescan", o, sizeof o);
  settle();
}

// The reference: the load as it was, reading the card on the control core (captured before the card queue took it)
struct Ref {
  int seed;
  const char* load;
  bool mono;
  uint64_t digest, log;
};
static const Ref kRef[] = {
    {1, "ram", false, 0xff0ab8b5c8b2b716ull, 0x30171317cf32f393ull},     // (stereo files kept stereo)
    {2, "stream", true, 0xd96d20d7ca4e92aeull, 0xdcd9f86c7cd9ba9cull},   // (poolmono: a stereo instrument over the stream budget would be held whole)
    {3, "ram", true, 0xe94998cdb4b7d317ull, 0xb073f05de45a4473ull},
    {4, "stream", true, 0xdc9af687c6c59c4aull, 0x96f5289566ca15a8ull},
    {5, "stream", true, 0xf07c7afba53d0761ull, 0xc0fbd5daf905779cull},
    {6, "ram", false, 0xc753b7dd67b46bd4ull, 0xba3152bfc53d8583ull},
    {7, "stream", true, 0x6cd53532f7436ec6ull, 0xedff8a243a65aa48ull},
};
static const uint64_t kMissingLog = 0x78ee4bf886dafa61ull;   // "msamp: <path>: cannot open"
static void check_ref(const Ref& f, const Result& r, const char* what) {
  printf("test_msamp_load: seed %d %s%s %s: digest 0x%016llx log 0x%016llx\n", f.seed, f.load, f.mono ? " mono" : "", what,
         (unsigned long long)r.digest, (unsigned long long)r.log);
  CHECK(r.loads == 1 && r.fails == 0 && r.digest);
  if (g_capture) return;
  CHECK(r.digest == f.digest);
  CHECK(r.log == f.log);
}

int main() {
  g_capture = getenv("MSAMP_LOAD_CAPTURE") != nullptr;
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  setenv("CORE_POOL_RAM_MB", "48", 1);
  char tmpl[] = "/tmp/msamp-load-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/MULTISAMPLE'").c_str()) == 0);
  std::vector<std::string> paths;
  for (const Ref& f : kRef) paths.push_back(make_inst(f.seed, f.load));
  std::string gone = make_inst(8, "ram");
  host_set_card(g_card.c_str());
  host_set_log_sink(sink);
  host_fake_time(true, 1000000);
  voices_init();
  card_ok = true;
  srclib::begin(true);
  pools::begin(true);
  msamp::begin(true);
  pu::steps();
  std::vector<int> ent;
  for (const std::string& p : paths) ent.push_back(add(p));
  int gone_e = add(gone);
  char o[200];
  CHECK(msamp::command("msamp=voice:0", o, sizeof o));
  msamp::set_head_ms(50);   // (heads of 50 ms: the longer samples stream, with loop parts past their heads)
  host_card_model(true);
  const int fds0 = open_fds();

  // ---- no card core: every request at once (seeds 1, 2)
  for (int i = 0; i < 2; ++i) {
    pcache::set_mono(kRef[i].mono);
    check_ref(kRef[i], load(ent[i]), "inline");
    unload();
  }
  // ---- a card core: the control core posts, never touches the card (seeds 3, 4)
  g_worker = true;
  cardq::set_worker(true);
  for (int i = 2; i < 4; ++i) {
    pcache::set_mono(kRef[i].mono);
    g_card3 = 0, g_max_step_us = 0, g_sum_step_us = 0, g_steps = 0;
    memset(g_phase_max, 0, sizeof g_phase_max);
    uint64_t c0 = host_card_us(0);
    Result r = load(ent[i]);
    check_ref(kRef[i], r, "card core");
    if (!g_capture) CHECK(g_card3 == 0);   // no card time on the control core
    // a read step converts kConvFrames at most: ~5 us here, the Pi 4 ~34x slower (a loose bound: a first touch of
    // fresh memory can take longer on the Mac)
    if (!g_capture) CHECK(g_phase_max[4] < 100);
    if (!g_capture) CHECK(host_card_us(0) > c0);
    printf("test_msamp_load: seed %d: %u steps, the longest %.1f us, mean %.2f us (Mac; by phase idle %d sfz %d header %d "
           "open %d read %d); control core card time %llu us\n", kRef[i].seed, g_steps, g_max_step_us, g_sum_step_us / g_steps,
           g_phase_max[0], g_phase_max[1], g_phase_max[2], g_phase_max[3], g_phase_max[4], (unsigned long long)g_card3);
    unload();
  }
  if (!g_capture) {
    // ---- abandoned mid-load: msamp::begin again (the boot's, a card change) with a request in flight (seeds 5..7)
    auto until = [](int phase) {   // steps (the card core serving) until the job is in phase with a request posted
      for (int i = 0; i < 400000; ++i) {
        step3();
        if (msamp::load_phase() == phase && cardq::pending() == 1) return;
        serve();
      }
      CHECK(!"phase never reached");
    };
    for (int k = 0; k < 3; ++k) {
      const Ref& f = kRef[4 + k];
      pcache::set_mono(f.mono);
      select(ent[4 + k]);
      uint32_t reads = cardq::stats().ran[cardq::kReadReq];
      uint32_t cancelled = cardq::stats().cancelled;
      if (k == 0) {   // a sample's read posted, not run: cancelled
        until(4);
        CHECK(cardq::stats().ran[cardq::kReadReq] > reads);
      } else if (k == 1) {   // a sample's read run by the card core, not taken in yet
        until(4);
        CHECK(serve() && cardq::pending() == 0);
      } else {   // a header's request posted
        until(2);
      }
      uint32_t opens = cardq::stats().ran[cardq::kOpenReq], closes = cardq::stats().ran[cardq::kCloseReq];
      msamp::begin(true);   // abandoned: the request settled (cancelled or taken), its file closed
      CHECK(msamp::load_phase() == 0);
      CHECK(cardq::stats().cancelled == cancelled + (k != 1 ? 1u : 0u));
      for (int i = 0; i < 16; ++i) serve();   // (the cancelled one's slot passed)
      CHECK(cardq::pending() == 0 && cardq::stats().ran[cardq::kOpenReq] == opens && cardq::stats().ran[cardq::kCloseReq] == closes);
      pu::steps();
      CHECK(msamp::command("msamp=voice:0", o, sizeof o));
      g_card3 = 0;
      Result r = load(ent[4 + k]);   // loaded again from the start: whole and the same
      check_ref(f, r, k == 0 ? "after an abandoned read (posted)" : k == 1 ? "after an abandoned read (run)" : "after an abandoned header");
      CHECK(g_card3 == 0);
      unload();
    }
    CHECK(open_fds() == fds0);   // every file the loads opened, the abandoned ones' too, closed
  } else {
    for (int k = 0; k < 3; ++k) {
      pcache::set_mono(kRef[4 + k].mono);
      check_ref(kRef[4 + k], load(ent[4 + k]), "card core");
      unload();
    }
  }
  // ---- an .sfz gone from the card: the load fails as before (one fail, nothing resident)
  {
    CHECK(remove((g_card + gone).c_str()) == 0);
    Result r = load(gone_e);
    printf("test_msamp_load: a missing .sfz: %u loads, %u fails, log 0x%016llx\n", r.loads, r.fails, (unsigned long long)r.log);
    CHECK(r.loads == 0 && r.fails == 1 && msamp::voice_slot(0) < 0);
    if (!g_capture) CHECK(r.log == kMissingLog);
  }
  g_worker = false;
  cardq::set_worker(false);
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_msamp_load: PASS\n");
  return 0;
}
