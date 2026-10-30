// core/tests/test_bundle_stall.cpp - bundles: an export and an import never hold up the audio (docs/BUNDLES.md "Never
// disturbing the audio"; the fix1041 rule, docs/SRC-BANKS.md section 7). As test_loadstall: the Pi's control core
// (core 3) renders a block only between its passes (the FIFO: 2 ms), card time is the host's card model charged to the
// core that runs it. The control core's passes are nm::step() (with a GRANULAR file loading through the card queue,
// as a stream would), the card core (core 0's loop) runs cardq::service() and then, only when the card queue is idle,
// one bundle step.
//
// Checked: no card time on the control core while a 12 MB project is exported to the card and imported back under a
// new name; no control pass over 2 ms; no bundle step while the streams need the card (cardq::busy); every step reads
// and writes at most one 64 KB piece; the pause rules: while the Rytm plays no card change at all and reads in 16 KB
// pieces, while the Overbridge recorder records not a byte; the job then finishes by itself. Phase 5: an export to a
// USB stick and an import from it, the same checks, every step at most a stick piece (32 KB), no stick I/O at all
// while the Rytm plays.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "bundle/bundle.h"
#include "bundle/projects.h"
#include "check.h"
#include "timing.h"
#include "granular/granular.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "msamp/cardq.h"
#include "msamp/msamp.h"
#include "nmach.h"
#include "voices.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
constexpr uint32_t kFifoUs = 2000;

struct Run {
  uint32_t passes = 0, over = 0, bundle_steps = 0, held = 0;
  uint64_t worst_us = 0, card3 = 0, card0 = 0, worst_step_card = 0;   // (of the steps that wrote nothing: the card
                                                                       // model charges a file's writes at its close)
};
static bundle::IoRule g_rule = {bundle::kPiece, bundle::kPiece, 0, ""};
static void rules(bundle::IoRule* r) { *r = g_rule; }
static bool card_busy() { return cardq::busy(); }
static bool g_need = false;   // a stream below its low mark (DSP SAMPLE's refill): the card is the streams'
static bool g_toggle = false;  // a stream low for 50 passes in every 200
static bool g_stick = false;   // the stick (USB:) mounted
static int volumes(bundle::Volume* out, int max) {
  if (!g_stick || max < 1) return 0;
  snprintf(out[0].drive, sizeof out[0].drive, "USB:");
  snprintf(out[0].name, sizeof out[0].name, "USB");
  out[0].free = 8ull << 30, out[0].total = 16ull << 30, out[0].free_known = true;
  return 1;
}
static bool vol_ok(const char* d) { return g_stick && !strcmp(d, "USB:"); }

// one control pass, then the card core's turn: the card queue first, a bundle step only when it is idle
static void pass(Run& r) {
  uint64_t c3 = host_card_us(3);
  uint64_t t0 = timing_cpu_us();   // (the thread's CPU: a preempted pass is not slow - tests/timing.h)
  nm::step();
  cardq::set_need(g_need || (g_toggle && r.passes / 50 % 4 == 3) ? 5 : ~0u, 20);
  uint64_t cpu = (timing_cpu_us() - t0) * 44 / 10;
  uint64_t d = host_card_us(3) - c3 + cpu;
  ++r.passes;
  r.card3 += host_card_us(3) - c3;
  if (d > r.worst_us) r.worst_us = d;
  if (d > kFifoUs) ++r.over;
  host_card_core(0);
  uint64_t c0 = host_card_us(0);
  for (int i = 0; i < 8 && cardq::service(); ++i) {}
  uint32_t st0 = bundle::stats().steps, w0 = bundle::stats().waits;
  uint64_t wr0 = bundle::stats().written;
  uint64_t b0 = host_card_us(0);
  int ph0 = bundle::status().phase;
  bundle::step();   // (it checks card_busy itself: held back while a request waits or a stream is low)
  if (bundle::stats().steps != st0) {
    ++r.bundle_steps;
    uint64_t sc = host_card_us(0) - b0;
    // (an export's close is a step of its own since 2026-10-29: the step after WRITE's last piece; the model charges
    // the whole file's sectors there, the Pi each piece's at its own f_sync)
    static bool s_wrote = false;
    bool closing = s_wrote && ph0 == bundle::kWriting;
    if (bundle::stats().written == wr0 && !closing && sc > r.worst_step_card) r.worst_step_card = sc;
    s_wrote = bundle::stats().written != wr0;
  }
  if (bundle::stats().waits != w0) ++r.held;
  r.card0 += host_card_us(0) - c0;
  host_card_core(3);
  g_us += 1000;
  host_fake_time(true, g_us);
}
static void print(const char* what, const Run& r) {
  printf("test_bundle_stall: %s: %u control passes, longest %llu us, %u over 2 ms, control-core card time %llu us; "
         "%u bundle steps (%u held back), card core %llu us, longest bundle step's card time %llu us\n",
         what, r.passes, (unsigned long long)r.worst_us, r.over, (unsigned long long)r.card3, r.bundle_steps, r.held,
         (unsigned long long)r.card0, (unsigned long long)r.worst_step_card);
}
static Run until(int phase_a, int phase_b, int max = 400000) {
  Run r;
  for (int i = 0; i < max; ++i) {
    pass(r);
    int p = bundle::status().phase;
    if (p == phase_a || p == phase_b || p == bundle::kFailed) break;
  }
  return r;
}

int main() {
  core::bundle::set_usb_export(true);   // its stick cases (0.3.0 ships with stick export off: test_bundle_usb)
  char tmpl[] = "/tmp/bstall-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Banks' '" + g_card + "/GRANULAR' '" + g_card + "/WAVETABLES' '" + g_card +
                "/MULTISAMPLE' '" + g_card + "/Samples/Live' '" + g_card + "/.rytm/projects/LIVE'").c_str()) == 0);
  auto tone = [](double hz) { return [hz](int i, int) { return 0.4 * std::sin(2 * M_PI * hz * i / 48000); }; };
  wg::write_file(g_card + "/Banks/ten.wav", wg::wav_bytes(1747626, 2, 24, 48000, 1, "", tone(330)));   // 10 MB
  std::string pool = "#sidecar samplepool 1\n";   // the project's SAMPLES list (core/lib/pools.h): refs by path
  for (int i = 0; i < 4; ++i) {   // the project: 4 x 3 MB samples
    char n[64];
    snprintf(n, sizeof n, "/Samples/Live/s%d.wav", i);
    wg::write_file(g_card + n, wg::wav_bytes(1500000, 1, 16, 48000, 1, "", tone(100 + 50 * i)));
    char line[96];
    snprintf(line, sizeof line, "A%03d sample - %zu %s\n", i + 1, wg::wav_bytes(1500000, 1, 16, 48000, 1, "", tone(1)).size(), n);
    pool += line;
  }
  wg::write_file(g_card + "/.rytm/projects/LIVE/samples.pool.txt", std::vector<uint8_t>(pool.begin(), pool.end()));
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
  bundle::Env env{};
  env.rules = rules;
  env.card_busy = card_busy;
  env.version = "test";
  env.board = "mac";
  env.volumes = volumes;
  env.vol_ok = vol_ok;
  bundle::set_env(env);
  bundle::reset_for_tests();
  projects::reset_for_tests();
  bundle::stats_reset();

  // ---- export to the card while a 10 MB GRANULAR file loads (the card queue busy with its reads)
  for (int k = 0; k < 20000 && !pools::ready(); ++k) nm::step(), cardq::service();   // the project's pool lists in
  {
    int out = -1;
    CHECK(pools::set_file(pools::kSamples, pools::code(2, 1), "/Banks/ten.wav", &out, pools::kForced) == pools::kOk);   // C001
  }
  wg::params(kGranMachine, 0, {1 + 2 / 8.0f, 0, 0, 64, 64, 0, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0});
  bundle::ExportOpts eo;
  eo.epoch = 1792677780u;
  CHECK(bundle::start_export("LIVE", bundle::kToCard, eo));
  Run a = until(bundle::kDone, bundle::kDone);
  print("export 12 MB to the card", a);
  CHECK(bundle::status().phase == bundle::kDone);
  std::string zip = std::string("/Bundles/") + bundle::bundle_name();
  bundle::finish();
  // the GRANULAR file loaded meanwhile
  for (int i = 0; i < 20000 && gr::entry_state(2 * 128 + 1) != gr::kStateRam && gr::entry_state(2 * 128 + 1) != gr::kStateCut; ++i) pass(a);
  CHECK(gr::entry_state(2 * 128 + 1) == gr::kStateRam || gr::entry_state(2 * 128 + 1) == gr::kStateCut);

  // ---- the streams need the card: no bundle step runs
  {
    CHECK(bundle::start_export("LIVE", bundle::kToBrowser, eo));
    g_need = true;
    Run h;
    for (int i = 0; i < 200; ++i) pass(h);
    CHECK(h.bundle_steps == 0 && h.held > 0);
    g_need = false;
    bundle::cancel();
    bundle::finish();
  }

  // ---- import onto another card (every sample new): VERIFY, PLAN with a stream low now and then; then STAGE with the
  // Rytm playing (no card change, 16 KB reads), the recorder recording (nothing), then stopped (it finishes)
  std::string b_card = g_card + "-b";
  CHECK(system(("mkdir -p '" + b_card + "/Bundles/incoming' && cp '" + g_card + zip + "' '" + b_card + "/Bundles/incoming/live.sidecar.zip'").c_str()) == 0);
  host_set_card(b_card.c_str());
  zip = "/Bundles/incoming/live.sidecar.zip";
  g_toggle = true;
  bundle::stats_reset();
  CHECK(bundle::start_verify(zip.c_str()));
  Run b = until(bundle::kReady, bundle::kReady);
  print("verify + plan", b);
  CHECK(bundle::status().phase == bundle::kReady);
  CHECK(bundle::start_import(bundle::ImportOpts()));
  g_rule = {bundle::kSlowPiece, 0, bundle::kSlowGapUs, "PLAYING"};
  uint64_t ch0 = host_changes();
  bundle::stats_reset();
  Run p;
  for (int i = 0; i < 500; ++i) pass(p);
  CHECK(host_changes() == ch0);   // not a single card change while the Rytm plays
  CHECK(!strcmp(bundle::status().waiting, "PLAYING"));
  g_rule = {0, 0, 0, "RECORDING"};
  uint64_t rd = bundle::stats().read;
  for (int i = 0; i < 200; ++i) pass(p);
  CHECK(bundle::stats().read == rd && host_changes() == ch0 && !strcmp(bundle::status().waiting, "RECORDING"));
  g_rule = {bundle::kPiece, bundle::kPiece, 0, ""};
  Run c = until(bundle::kDone, bundle::kDone);
  print("import 12 MB (stage + commit)", c);
  CHECK(bundle::status().phase == bundle::kDone);
  bundle::finish();

  CHECK(b.held > 0 && c.held > 0);   // held back while the stream was low
  FILE* chk = fopen((b_card + "/Samples/Live/s3.wav").c_str(), "rb");
  CHECK(chk);
  fclose(chk);
  for (const Run* r : {&a, &b, &c, &p}) {
    CHECK(r->card3 == 0);           // no card operation on the control core
    CHECK(r->over <= kTimingSpikes);   // no control pass longer than the FIFO (CPU time: tests/timing.h)
  }
  const bundle::Stats& s = bundle::stats();
  printf("test_bundle_stall: bundle steps: %u, the most read in one %u B, written %u B, the longest step's CPU %u us (Mac)\n",
         s.steps, s.max_read, s.max_write, s.max_us);
  CHECK(s.max_read <= bundle::kPiece && s.max_write <= bundle::kPiece);
  for (const Run* r : {&a, &b, &c}) CHECK(r->worst_step_card < cardq::kTakeBackUs);   // reads: one piece a step
  // while playing: reads in 16 KB pieces at most (the window above: its own stats)
  {
    CHECK(bundle::start_verify(zip.c_str()));
    g_rule = {bundle::kSlowPiece, 0, bundle::kSlowGapUs, "PLAYING"};
    bundle::stats_reset();
    Run v = until(bundle::kReady, bundle::kReady);
    CHECK(bundle::status().phase == bundle::kReady && bundle::stats().max_read <= bundle::kSlowPiece && bundle::stats().written == 0);
    print("verify while playing (16 KB pieces, one every 2 ms)", v);
    g_rule = {bundle::kPiece, bundle::kPiece, 0, ""};
    bundle::cancel();
    bundle::finish();
  }
  // ---- phase 5: export the imported project to a USB stick, import it from there onto a third card
  std::string stick = g_card + "-stick", c_card = g_card + "-c";
  CHECK(system(("mkdir -p '" + stick + "' '" + c_card + "'").c_str()) == 0);
  host_set_volume("USB:", stick.c_str());
  g_stick = true;
  {
    bundle::ExportOpts so;
    so.epoch = 1792677780u;
    snprintf(so.vol, sizeof so.vol, "USB:");
    bundle::stats_reset();
    CHECK(bundle::start_export("LIVE", bundle::kToVolume, so));
    Run h = until(bundle::kWriting, bundle::kDone);
    CHECK(bundle::status().phase == bundle::kWriting);
    g_rule = {bundle::kSlowPiece, 0, bundle::kSlowGapUs, "PLAYING"};   // playing: not a byte to the stick
    uint64_t rd = bundle::stats().read, wr = bundle::stats().written;
    Run q;
    for (int i = 0; i < 300; ++i) pass(q);
    CHECK(bundle::stats().read == rd && bundle::stats().written == wr && !strcmp(bundle::status().waiting, "PLAYING"));
    g_rule = {bundle::kPiece, bundle::kPiece, 0, ""};
    Run e = until(bundle::kDone, bundle::kDone);
    print("export 12 MB to a USB stick", e);
    CHECK(bundle::status().phase == bundle::kDone);
    CHECK(bundle::stats().max_write <= bundle::kStickPiece);
    zip = std::string("USB:/SIDECAR/") + bundle::bundle_name();
    bundle::finish();
    host_set_card(c_card.c_str());
    projects::reset_for_tests();
    bundle::stats_reset();
    CHECK(bundle::start_verify(zip.c_str()));
    Run v = until(bundle::kReady, bundle::kReady);
    CHECK(bundle::status().phase == bundle::kReady && bundle::stats().max_read <= bundle::kStickPiece);
    CHECK(bundle::start_import(bundle::ImportOpts()));
    Run i = until(bundle::kDone, bundle::kDone);
    print("import 12 MB from a USB stick", i);
    CHECK(bundle::status().phase == bundle::kDone);
    bundle::finish();
    FILE* f3 = fopen((c_card + "/Samples/Live/s3.wav").c_str(), "rb");
    CHECK(f3);
    fclose(f3);
    for (const Run* r : {&h, &q, &e, &v, &i}) CHECK(r->card3 == 0 && r->over <= kTimingSpikes);
  }
  host_card_model(false);
  cardq::reset();
  system(("rm -rf '" + g_card + "' '" + b_card + "' '" + stick + "' '" + c_card + "'").c_str());
  printf("test_bundle_stall: PASS\n");
  return 0;
}
