// core/tests/test_loadstall.cpp - fix1041: a sample set into a bank row and loaded (the Rytm's FUNC + YES browser,
// YES on a file: srclib::bank_set, then the row's preview loads it) must never hold up the audio. The Pi's control
// core (core 3) renders a block only between its passes, and the FIFO holds 96 frames (2 ms, docs/pi-engine.md "FIFO
// target"), so any pass longer than that is silence. Card time is the host's card model (core/host/core_port.h
// host_card_model: Circle's EMMC busy-waits, FatFs's volume lock for a whole call), charged to the core that runs it:
// the control core's passes are the test's loop calling nm::step() (and the commit itself), the card core (core 0's
// loop on the Pi) is the test calling cardq::service() between them.
//
// Checked: no control-core pass spends card time on the commit or the load (every card operation goes through the card
// queue), the longest control pass stays under the FIFO's 2 ms, the file loads whole and plays, the pool file is
// written (by the card core) and read back the same, and a second commit while the first write still waits is
// coalesced into one later write. The figures (before / after, 1 MB and 10 MB WAVs) are printed for docs/SRC-BANKS.md.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "timing.h"
#include "granular/granular.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "pool/pool.h"
#include "msamp/cardq.h"
#include "msamp/msamp.h"
#include "nmach.h"
#include "voices.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
constexpr uint32_t kFifoUs = 2000;   // 96 frames at 48 kHz

struct Run {
  uint32_t passes = 0, over = 0;          // control passes, those over the FIFO
  uint64_t worst_us = 0, worst_card3 = 0, card3 = 0, silent_us = 0;   // the longest pass, card time on core 3
  uint64_t card0 = 0, worst_card0 = 0;    // the card core's
};

static void pass_done(Run& r, uint64_t c3_before, uint64_t t0) {
  uint64_t c3 = host_card_us(3) - c3_before;
  // the pass: its modeled card time plus its own CPU time on the Pi (the Mac's x 4.4, the boot bench's PLAITS ratio;
  // the thread's CPU clock: a pass preempted on a loaded Mac is not slow - tests/timing.h)
  uint64_t cpu = (timing_cpu_us() - t0) * 44 / 10;
  uint64_t d = c3 + cpu;
  ++r.passes;
  r.card3 += c3;
  if (c3 > r.worst_card3) r.worst_card3 = c3;
  if (d > r.worst_us) r.worst_us = d;
  if (d > kFifoUs) ++r.over, r.silent_us += d - kFifoUs;
}
static void card_core(Run& r) {   // core 0's loop: card_service(8)
  if (!cardq::worker()) return;
  host_card_core(0);
  uint64_t c0 = host_card_us(0);
  for (int i = 0; i < 8 && cardq::service(); ++i) {}
  host_card_core(3);
  uint64_t d = host_card_us(0) - c0;
  r.card0 += d;
  if (d > r.worst_card0) r.worst_card0 = d;
}
// One control pass: an nm::step() (the idle work between blocks), then the card core's turn; 1 ms of time a pass.
static bool step(Run& r) {
  uint64_t c3 = host_card_us(3);
  uint64_t t0 = timing_cpu_us();
  bool did = nm::step();
  pass_done(r, c3, t0);
  card_core(r);
  g_us += 1000;
  host_fake_time(true, g_us);
  return did;
}
static void settle(Run& r, int ms) {
  for (int i = 0; i < ms; ++i) step(r);
}

static std::string slurp(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}

// The commit as page_wavegrain.cpp's YES on a file does it (bank_set), then the list's rest on the row (the preview,
// 300 ms later), passes until the row's file is in RAM.
static Run commit_and_load(int row, const char* path, bool* loaded) {
  Run r;
  uint64_t c3 = host_card_us(3);
  uint64_t t0 = timing_cpu_us();
  int out = -1;
  CHECK(pools::set_file(pools::kSamples, pools::code(2, row), path, &out, pools::kForced) == pools::kOk);
  pass_done(r, c3, t0);
  card_core(r);
  settle(r, 300);
  int e = 2 * srclib::kBankRows + row;
  // the list's YES on the row: P1 = C<row> (the Rytm's 48 write comes back as the 0x03 report), the voice loads it
  wg::params(kGranMachine, 0, {row + 2 / 8.0f, 0, 0, 64, 64, 0, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0});
  *loaded = false;
  for (int i = 0; i < 40000 && !*loaded; ++i) {
    step(r);
    *loaded = gr::entry_state(e) == gr::kStateRam || gr::entry_state(e) == gr::kStateCut;
  }
  settle(r, 50);
  return r;
}

static void print(const char* what, const Run& r) {
  printf("test_loadstall: %s: %u control passes, longest %llu us (card %llu us of it), %u over 2 ms (%llu us of silence); "
         "control-core card time %llu us; card core %llu us (longest service %llu us)\n",
         what, r.passes, (unsigned long long)r.worst_us, (unsigned long long)r.worst_card3, r.over,
         (unsigned long long)r.silent_us, (unsigned long long)r.card3, (unsigned long long)r.card0,
         (unsigned long long)r.worst_card0);
}

int main() {
  char tmpl[] = "/tmp/loadstall-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Banks' '" + g_card + "/GRANULAR' '" + g_card + "/WAVETABLES' '" + g_card +
                "/MULTISAMPLE'").c_str()) == 0);
  auto tone = [](double hz) { return [hz](int i, int) { return 0.4 * std::sin(2 * M_PI * hz * i / 48000); }; };
  // 1 MB: mono 16-bit 48 kHz, 10.9 s; 10 MB: stereo 24-bit 48 kHz, 36.4 s
  wg::write_file(g_card + "/Banks/one.wav", wg::wav_bytes(524288, 1, 16, 48000, 1, "", tone(220)));
  wg::write_file(g_card + "/Banks/ten.wav", wg::wav_bytes(1747626, 2, 24, 48000, 1, "", tone(330)));
  wg::write_file(g_card + "/Banks/two.wav", wg::wav_bytes(48000, 1, 16, 48000, 1, "", tone(440)));
  wg::write_file(g_card + "/Banks/four.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(550)));
  wg::write_file(g_card + "/Banks/five.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(660)));
  host_set_card(g_card.c_str());
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, g_us);
  voices_init();
  card_ok = true;
  nm::begin(true);
  msamp::begin(true);
  for (int i = 0; i < 20000 && (nm::step() || msamp::step()); ++i) {}
  cardq::reset();
  cardq::set_worker(true);   // the Pi: core 0's loop runs the card queue (cmdline cardq=1, the default)
  host_card_model(true);

  bool ok1 = false, ok10 = false;
  Run a = commit_and_load(1, "/Banks/one.wav", &ok1);
  print("1 MB WAV (C001)", a);
  Run b = commit_and_load(2, "/Banks/ten.wav", &ok10);
  print("10 MB WAV (C002)", b);
  CHECK(ok1 && ok10);
  CHECK(gr::entry_state(2 * 128 + 2) == gr::kStateRam || gr::entry_state(2 * 128 + 2) == gr::kStateCut);
  for (const Run* r : {&a, &b}) {
    CHECK(r->card3 == 0);           // no card operation on the control core
    CHECK(r->over <= kTimingSpikes);   // no control pass longer than the FIFO (CPU time: tests/timing.h)
    CHECK(r->card0 > 0);            // the card core did the work
  }
  // the SAMPLES pool's file as written by the card core (fmt::write_atomic through cardq): C001, C002
  std::string pf = slurp(g_card + "/.rytm/projects/_default/samples.pool.txt");
  CHECK(pf.find("C001 sample - ") != std::string::npos && pf.find(" /Banks/one.wav\tone") != std::string::npos);
  CHECK(pf.find("C002 sample - ") != std::string::npos && pf.find(" /Banks/ten.wav\tten") != std::string::npos);
  // three commits back to back: one later write with every row (the edits coalesce)
  uint32_t saves0 = pools::stats().saves;
  int o3 = -1;
  CHECK(pools::set_file(pools::kSamples, pools::code(2, 3), "/Banks/two.wav", &o3, pools::kForced) == pools::kOk);
  CHECK(pools::set_file(pools::kSamples, pools::code(2, 4), "/Banks/four.wav", &o3, pools::kForced) == pools::kOk);
  CHECK(pools::set_file(pools::kSamples, pools::code(2, 5), "/Banks/five.wav", &o3, pools::kForced) == pools::kOk);
  Run c;
  settle(c, 400);
  CHECK(c.card3 == 0);
  pf = slurp(g_card + "/.rytm/projects/_default/samples.pool.txt");
  CHECK(pf.find("C003 sample") != std::string::npos && pf.find("C004 sample") != std::string::npos &&
        pf.find("C005 sample") != std::string::npos);
  CHECK(pools::stats().saves - saves0 == 1);
  printf("test_loadstall: three commits back to back: %u pool writes\n", pools::stats().saves - saves0);
  // a project switch: the old project's lists flushed, the new one's three lists and its used set read - every card
  // operation on the card core
  Run sw;
  snprintf(project, sizeof project, "%s", "OTHER");
  for (int i = 0; i < 2000 && (!pools::ready() || strcmp(pools::project(), "OTHER")); ++i) step(sw);
  CHECK(pools::ready() && !strcmp(pools::project(), "OTHER"));
  snprintf(project, sizeof project, "%s", "");
  for (int i = 0; i < 2000 && (!pools::ready() || strcmp(pools::project(), "_default")); ++i) step(sw);
  CHECK(pools::ready() && pools::at(pools::kSamples, pools::code(2, 5)).type == pools::kSample);
  print("project switch and back (the lists read)", sw);
  CHECK(sw.card3 == 0 && sw.over <= kTimingSpikes && sw.card0 > 0);
  // the used set (pool-used.txt): written by the card core a second after a new code was seen
  Run us;
  pools::seen(pools::kSamples, pools::code(2, 2));
  settle(us, 1200);
  CHECK(us.card3 == 0 && slurp(g_card + "/.rytm/projects/_default/pool-used.txt").find("smp C002 1") != std::string::npos);
  // the voice plays the loaded file (C002) after the swap
  wg::params(kGranMachine, 0, {2 + 2 / 8.0f, 0, 0, 64, 64, 0, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0});
  settle(c, 50);
  wg::trig(kGranMachine, 0, 60, 100, {2 + 2 / 8.0f, 0, 0, 64, 64, 0, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0});
  settle(c, 20);
  CHECK(gr::voice_entry(0) == 2 * 128 + 2 && gr::voice_notes(0) > 0);
  host_card_model(false);
  cardq::reset();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_loadstall: PASS\n");
  return 0;
}
