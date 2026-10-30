// core/tests/test_library_stall.cpp - the library's hashing never holds up the audio (docs/LIBRARY.md "Cost"; the
// fix1041 rule, docs/SRC-BANKS.md section 7; the formats spec 7.2 "scheduler"). As test_bundle_stall: the Pi's control
// core (core 3) renders a block only between its passes (the FIFO: 2 ms), card time is the host's card model charged to
// the core that runs it. The control core's passes are nm::step() (a 10 MB GRANULAR file loading through the card
// queue, as a stream would) and a resolve() of every library reference it plays (RAM only); the card core (core 0's
// loop) runs cardq::service() and then one library step.
//
// Checked: the boot load, the walk and the first hash of ~40 MB of samples run with no card time on the control core
// and no control pass over 2 ms; no library step while the streams need the card (cardq::busy); every step at most one
// 64 KB piece; while the Rytm plays: no card change at all and reads in 16 KB pieces, one every 2 ms; while the
// Overbridge recorder records: not a byte; then it finishes by itself. Printed: the card model's hash rate (the card is
// the limit: the spec's 12-14 MB/s on the Pi) and the card core's busiest step.
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
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "msamp/cardq.h"
#include "msamp/msamp.h"
#include "nmach.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
constexpr uint32_t kFifoUs = 2000;
static lib::IoRule g_rule = {lib::kPiece, lib::kPiece, 0, ""};
static void rules(lib::IoRule* r) { *r = g_rule; }
static bool card_busy() { return cardq::busy(); }
static bool g_need = false;

struct Run {
  uint32_t passes = 0, over = 0, steps = 0, held = 0;
  uint64_t worst_us = 0, card3 = 0, card0 = 0, worst_step_card = 0;
};
static lib::Ref g_refs[4];
static void pass(Run& r) {
  uint64_t c3 = host_card_us(3);
  uint64_t t0 = timing_cpu_us();   // (the thread's CPU: a preempted pass is not slow - tests/timing.h)
  nm::step();
  for (const lib::Ref& ref : g_refs) {   // the control core plays by resolve: RAM only
    lib::Resolved o;
    lib::resolve(ref, &o);
  }
  cardq::set_need(g_need ? 5 : ~0u, 20);
  uint64_t cpu = (timing_cpu_us() - t0) * 44 / 10;
  uint64_t d = host_card_us(3) - c3 + cpu;
  ++r.passes;
  r.card3 += host_card_us(3) - c3;
  if (d > r.worst_us) r.worst_us = d;
  if (d > kFifoUs) ++r.over;
  host_card_core(0);
  uint64_t c0 = host_card_us(0);
  for (int i = 0; i < 8 && cardq::service(); ++i) {}
  uint32_t s0 = lib::work().steps, w0 = lib::work().waits;
  uint64_t wr0 = lib::work().written, b0 = host_card_us(0);
  lib::step();
  if (lib::work().steps != s0) {
    ++r.steps;
    uint64_t sc = host_card_us(0) - b0;
    if (lib::work().written == wr0 && sc > r.worst_step_card) r.worst_step_card = sc;
  }
  if (lib::work().waits != w0) ++r.held;
  r.card0 += host_card_us(0) - c0;
  host_card_core(3);
  g_us += 1000;
  host_fake_time(true, g_us);
}
static void print(const char* what, const Run& r) {
  printf("test_library_stall: %s: %u control passes, longest %llu us, %u over 2 ms, control-core card time %llu us; "
         "%u library steps (%u held back), card core %llu us, longest step's card time %llu us\n",
         what, r.passes, (unsigned long long)r.worst_us, r.over, (unsigned long long)r.card3, r.steps, r.held,
         (unsigned long long)r.card0, (unsigned long long)r.worst_step_card);
}

int main() {
  char tmpl[] = "/tmp/lstall-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Banks' '" + g_card + "/GRANULAR' '" + g_card + "/WAVETABLES' '" + g_card +
                "/MULTISAMPLE' '" + g_card + "/Samples/Live' '" + g_card + "/.rytm/pools'").c_str()) == 0);
  auto tone = [](double hz) { return [hz](int i, int) { return 0.4 * std::sin(2 * M_PI * hz * i / 48000); }; };
  wg::write_file(g_card + "/Banks/ten.wav", wg::wav_bytes(1747626, 2, 24, 48000, 1, "", tone(330)));   // 10 MB
  uint64_t total = 10u << 20;
  for (int i = 0; i < 10; ++i) {   // ~30 MB of samples to hash
    char n[64];
    snprintf(n, sizeof n, "/Samples/Live/s%d.wav", i);
    wg::write_file(g_card + n, wg::wav_bytes(1500000, 1, 16, 48000, 1, "", tone(100 + 50 * i)));
    total += 3000044;
  }
  host_set_card(g_card.c_str());
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, g_us);
  voices_init();
  card_ok = true;
  nm::begin(true);
  msamp::begin(true);
  for (int i = 0; i < 20000 && (nm::step() || msamp::step()); ++i) {}
  cardq::reset();
  cardq::set_worker(true);
  host_card_model(true);
  lib::reset_for_tests();
  lib::Env e{};
  e.rules = rules, e.card_busy = card_busy;
  lib::set_env(e);
  for (int i = 0; i < 4; ++i) lib::parse_ref(("- 0 /Samples/Live/s" + std::to_string(i) + ".wav").c_str(), &g_refs[i]);
  lib::begin(true);

  // ---- boot: load, walk and the first hash, with a 10 MB GRANULAR file loading through the card queue meanwhile
  for (int k = 0; k < 20000 && !pools::ready(); ++k) nm::step(), cardq::service();   // the project's pool lists in
  {
    int out = -1;
    CHECK(pools::set_file(pools::kSamples, pools::code(2, 1), "/Banks/ten.wav", &out, pools::kForced) == pools::kOk);   // C001
  }
  wg::params(kGranMachine, 0, {1 + 2 / 8.0f, 0, 0, 64, 64, 0, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0});
  Run a;
  uint64_t c0 = host_card_us(0);
  int i = 0;
  for (; i < 400000 && (lib::busy() || lib::stats().state == lib::kLoading); ++i) {
    pass(a);
    if (i == 100) g_need = true;    // a stream low for a while: the library holds back
    if (i == 300) g_need = false;
  }
  CHECK(i < 400000);
  print("boot: load, walk, hash ~40 MB", a);
  lib::Stats st = lib::stats();
  CHECK(st.state == lib::kReady && st.files == 11 && st.hashed == 11);
  uint64_t lib_us = host_card_us(0) - c0;
  printf("test_library_stall: the card model: %.1f MB hashed in %.1f s of card-core time (%.1f MB/s, the card the limit)\n",
         total / 1048576.0, lib_us / 1e6, total / 1048576.0 / (lib_us / 1e6));
  CHECK(a.held > 0);   // held back while the stream was low
  for (int k = 0; k < 20000 && gr::entry_state(2 * 128 + 1) != gr::kStateRam && gr::entry_state(2 * 128 + 1) != gr::kStateCut; ++k) pass(a);
  CHECK(gr::entry_state(2 * 128 + 1) == gr::kStateRam || gr::entry_state(2 * 128 + 1) == gr::kStateCut);

  // ---- the streams need the card: no library step runs
  wg::write_file(g_card + "/Samples/Live/more.wav", wg::wav_bytes(1500000, 2, 24, 48000, 1, "", tone(77)));   // 9 MB
  lib::rescan();
  {
    g_need = true;
    Run h;
    for (int k = 0; k < 200; ++k) pass(h);
    CHECK(h.steps == 0 && h.held > 0 && !strcmp(lib::stats().waiting, "STREAMS"));
    g_need = false;
  }
  // ---- the Rytm plays: reads in 16 KB pieces, one every 2 ms, no card change; the recorder records: nothing
  g_rule = {lib::kSlowPiece, 0, lib::kSlowGapUs, "PLAYING"};
  uint64_t ch0 = host_changes();
  Run p;
  uint64_t rd = lib::work().read;
  for (int k = 0; k < 3000; ++k) {
    uint64_t r0 = lib::work().read;
    pass(p);
    CHECK(lib::work().read - r0 <= lib::kSlowPiece);
  }
  CHECK(host_changes() == ch0 && lib::work().read > rd);
  print("hashing while the Rytm plays (16 KB pieces, one every 2 ms)", p);
  g_rule = {0, 0, 0, "RECORDING"};
  rd = lib::work().read;
  Run q;
  for (int k = 0; k < 300; ++k) pass(q);
  CHECK(lib::work().read == rd && host_changes() == ch0 && !strcmp(lib::stats().waiting, "RECORDING"));
  g_rule = {lib::kPiece, lib::kPiece, 0, ""};
  Run c;
  for (int k = 0; k < 400000 && lib::busy(); ++k) pass(c);
  print("then stopped: the rest at full rate", c);
  CHECK(lib::stats().files == 12 && lib::stats().hashed == 12 && host_changes() > ch0);

  for (const Run* r : {&a, &p, &q, &c}) {
    CHECK(r->card3 == 0);           // no card operation on the control core
    CHECK(r->over <= kTimingSpikes);   // no control pass longer than the FIFO (CPU time: tests/timing.h)
    CHECK(r->worst_step_card < cardq::kTakeBackUs);
  }
  const lib::Work& w = lib::work();
  printf("test_library_stall: library steps %u, the most read in one %u B, written %u B, the longest step's CPU %u us (Mac)\n",
         w.steps, w.max_read, w.max_write, w.max_us);
  CHECK(w.max_read <= lib::kPiece && w.max_write <= lib::kPiece);
  host_card_model(false);
  cardq::reset();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_library_stall: PASS\n");
  return 0;
}
