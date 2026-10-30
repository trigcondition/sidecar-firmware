// core/tests/test_poolstall.cpp - loadstall (0.3.0): the Rytm's SETTINGS > SAMPLES > SIDECAR browser (rytm-mods 0023)
// and the DSP SAMPLE reports it leads to must not hold up the audio. The SysEx handlers run on the Pi's control core
// (core 3, ToCore in pi/engine/cores.cpp), which renders a block only between its passes (the FIFO: 2 ms); measured on
// the device (/status slowSx, build 0db9a36): 0D (a folder's listing) 1.2 ms, 0F (LOAD TO PROJ) 2.9-7.1 ms, 03 (a DSP
// SAMPLE report) 2.7-4.6 ms - each one card read of /.rytm/files.bin per file record (card::Index::file), many per
// message. Card time is the host's card model (core/host/core_port.h host_card_model), charged to the core that runs
// it; the card core (core 0) is the test calling cardq::service() between the handlers.
//
// Checked, per handler, on a 200-file folder: no card time on the control core (the file records from the index's
// RAM copy, card::Index::file), the answers as before (0E's entries, 14's rows); a DSP SAMPLE stream's prepare
// (idle_prepare) opens its file on the card core (the control core reads the head only), and none at all for a sample
// the sample RAM plays whole; every remote-UI key (31), the page closed and on each page RIGHT reaches, spends no card
// time on the control core either (the device's one 40 ms 31: not reproduced here - this guards the keys).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "timing.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "pool/sample_ram_pool.h"
#include "engine.h"
#include "host_link.h"
#include "voices.h"
#include "wav.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static uint64_t g_card0 = 0;
static void card_core() {   // core 0's loop: the posted card work
  host_card_core(0);
  uint64_t c0 = host_card_us(0);
  for (int i = 0; i < 64 && cardq::service(); ++i) {}
  host_card_core(3);
  g_card0 += host_card_us(0) - c0;
}
// One SysEx message on the control core: its card time there (us)
static uint64_t g_worst_cpu = 0;
static int g_worst_cmd = -1;
static uint64_t handle(const std::vector<uint8_t>& m) {
  uint64_t c3 = host_card_us(3);
  uint64_t t0 = timing_cpu_us();
  engine_sysex(m.data(), (int)m.size());
  uint64_t cpu = (timing_cpu_us() - t0) * 44 / 10;   // the Pi's (the Mac's x 4.4: test_loadstall's ratio)
  if (cpu > g_worst_cpu) g_worst_cpu = cpu, g_worst_cmd = m[4] << 8 | (m.size() > 7 ? m[7] : 0);
  uint64_t d = host_card_us(3) - c3;
  card_core();
  return d;
}
static std::vector<uint8_t> msg(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static std::vector<uint8_t> report(int vb, std::vector<int> p) {   // a 0x03 PARAMS report: the machine and P1..P16
  std::vector<uint8_t> b = {0x03, (uint8_t)vb};
  for (int k = 0; k < 16; ++k) b.push_back((uint8_t)(k < (int)p.size() ? p[k] : 0)), b.push_back(0);
  return msg(b);
}
static void ready() {   // the project's pool lists read (the control core's steps; their card work on the card core)
  char k[kKeyMax];
  project_key(project, k, sizeof k);
  for (int i = 0; i < 20000 && (!pools::ready() || strcmp(pools::project(), k) || pools::saving()); ++i) {
    pools::step();
    card_core();
  }
  CHECK(pools::ready());
}

int main() {
  char tmpl[] = "/tmp/core-poolstall-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  setenv("CORE_SAMPLE_RAM_MB", "8", 1);   // the Pi: DSP SAMPLE reads the pool RAM (sampleram= default 256)
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/Drums") && fs.mkdir("/Samples/Bass"));
  auto z = [](int i, int) { return (int32_t)((i % 64) * 200 - 6400); };
  const int kFiles = 200;
  for (int i = 0; i < kFiles; ++i) {
    char p[64];
    snprintf(p, sizeof p, "/Samples/Drums/hit%03d.wav", i);
    write_wav(p, 4800, 1, 16, 48000, z);
  }
  write_wav("/Samples/Bass/sub.wav", 4800, 1, 16, 48000, z);
  hooks.to_rytm = out;
  card_ok = cards.begin(log, shared, sizeof shared);
  CHECK(card_ok);
  srclib::begin(true);
  pools::begin(true);
  junolib.begin(&cards.sd(), log);
  dxlib.begin(&cards.sd(), log);
  sample_ram_init();
  CHECK(psram != nullptr);
  voices_init();
  int drums = -1;
  for (int k = 1; k < cards.folders(); ++k)
    if (!strcmp(cards.folder(k).name, "Drums")) drums = k;
  CHECK(drums > 0 && cards.folder(drums).nfiles == kFiles);
  cardq::reset();
  cardq::set_worker(true);   // the Pi: core 0's loop runs the card queue
  host_card_model(true);
  set_project((const uint8_t*)"PROJ\xf7", 5);
  ready();
  pool_sync();
  card_core();

  // 0D: Drums' listing - 200 files, 6 a message
  g_out.clear();
  uint64_t l0 = handle(msg({0x0D, (uint8_t)(drums >> 7), (uint8_t)(drums & 127)}));
  {   // (the 0E messages: a 26 may come with them, another report)
    std::vector<std::vector<uint8_t>> l;
    for (auto& m : g_out)
      if (m[4] == 0x0E) l.push_back(m);
    g_out = l;
  }
  CHECK(g_out.size() == (kFiles + 5) / 6);
  {   // its last entry: hit199
    std::string s(g_out.back().begin(), g_out.back().end());
    CHECK(s.find("hit199") != std::string::npos);
  }
  // 0F: LOAD TO PROJ hit150 (the folder's 151st file in name order) into bank A's first free row
  g_out.clear();
  uint64_t a0 = handle(msg({0x0F, (uint8_t)(drums >> 7), (uint8_t)(drums & 127), 1, 150 - 128}));
  ready();
  pool_sync();
  card_core();
  CHECK(npool == 1 && !strcmp(pool[0].path, "Drums/hit150.wav") && pool[0].folder == drums && pool[0].file == 150);
  CHECK(!g_out.empty() && g_out.back()[4] == 0x14);
  uint64_t a1 = handle(msg({0x0F, (uint8_t)(drums >> 7), (uint8_t)(drums & 127), 1, 199 - 128}));   // a second: A002
  ready();
  pool_sync();
  card_core();
  CHECK(npool == 2 && pool[1].file == 199);
  // 13: the pool table
  g_out.clear();
  uint64_t p0 = handle(msg({0x13}));
  CHECK(!g_out.empty() && g_out[0][4] == 0x14 && g_out[0][6] == 2);
  // 03: a DSP SAMPLE report on voice 3, SMP = A002 (P11: the code), then its trig (04)
  std::vector<int> pp = {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 2, 0, 0, 120, 0, 100};
  uint64_t r0 = handle(report(kDspSample | 3, pp));
  pp[10] = 1;
  uint64_t r1 = handle(report(kDspSample | 3, pp));   // another slot: A001
  std::vector<uint8_t> trig = {0xf0, 0x7d, 0x52, 0x54, 0x04, (uint8_t)(kDspSample | 3), 60, 100};
  for (int k = 0; k < 16; ++k) trig.push_back((uint8_t)pp[k]), trig.push_back(0);
  trig.push_back(0xf7);
  uint64_t t0 = handle(trig);
  // its stream's prepare (ONE, the sample not in the RAM yet: idle_prepare between the blocks, the trig waits for it):
  // the file opened by the card core; the control core reads the head only (one read, as a refill)
  uint64_t prep_max = 0;
  bool ready = false;
  for (int i = 0; i < 50 && !ready; ++i) {
    uint64_t c3 = host_card_us(3);
    idle_prepare();
    uint64_t d = host_card_us(3) - c3;
    if (d > prep_max) prep_max = d;
    card_core();
    ready = !prep_pending[3] && !trig_pending[3];
  }
  CHECK(ready && pool_open[0] && prepared[3][0] == drums && prepared[3][1] == 150 && streams[3].playing());
  // A002 whole in the sample RAM (the card core loads it): a report and a trig on it prepare no stream at all
  const int16_t* pcm = nullptr;
  uint32_t frames = 0;
  float rate = 0;
  bool cut = false;
  for (int i = 0; i < 2000 && !psram->get(drums, 199, &pcm, &frames, &rate, &cut, nullptr); ++i) {
    idle_stream();
    card_core();
  }
  CHECK(psram->get(drums, 199, &pcm, &frames, &rate, &cut, nullptr) && !cut);
  pp[10] = 2;
  uint64_t r2 = handle(report(kDspSample | 3, pp));
  CHECK(!prep_pending[3]);
  trig[8 + 2 * 10] = 2;   // (the 04's P11)
  uint64_t t2 = handle(trig);
  CHECK(!prep_pending[3] && !trig_pending[3] && r2 == 0 && t2 == 0);
  printf("test_poolstall: control-core card time: 0D listing %llu us, 0F LOAD TO PROJ %llu / %llu us, 13 pool %llu us, "
         "03 DSP SAMPLE report %llu / %llu us, 04 trig %llu us, its stream's prepare %llu us (card core %llu us)\n",
         (unsigned long long)l0, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)p0,
         (unsigned long long)r0, (unsigned long long)r1, (unsigned long long)t0, (unsigned long long)prep_max, (unsigned long long)g_card0);
  CHECK(l0 == 0);
  CHECK(a0 == 0 && a1 == 0);
  CHECK(p0 == 0);
  CHECK(r0 == 0 && r1 == 0);
  CHECK(t0 == 0);
  CHECK(prep_max < 2000);   // the head's read (535 us) and the stream's copy of the handle (the host's reopens it: 1000
                            // us; the Pi's copies it); before: 6535 us - /Samples, Drums and the file opened here

  // 31: every key, down and up, the remote page closed and open (30: the SIDECAR page)
  uint64_t worst = 0;
  int worst_code = -1;
  for (int open = 0; open < 2 + 64; ++open) {
    if (open) handle(msg({0x30, 1, (uint8_t)(open < 2 ? 0 : open - 2), 1, 0}));
    for (int code = 0; code < 256; ++code)
      for (int down = 1; down >= 0; --down) {
        uint64_t d = handle(msg({0x31, 1, (uint8_t)(code >> 7), (uint8_t)(code & 127), (uint8_t)down}));
        if (d > worst) worst = d, worst_code = code | open << 8;
      }
  }
  // every page the SIDECAR page reaches with RIGHT, each one's keys (the navigation keys left out: the page stays)
  uint64_t worst_cpu_key = 0;
  int worst_cpu_at = -1;
  for (int pg = 0; pg < 48; ++pg) {
    handle(msg({0x30, 1, 0, 1, 0}));
    for (int k = 0; k < pg; ++k) handle(msg({0x31, 1, 0, 0x41, 1})), handle(msg({0x31, 1, 0, 0x41, 0}));
    for (int code = 0; code < 256; ++code) {
      if ((code >= 0x40 && code <= 0x45) || code == 0x84 || code == 0x86 || code == 0x78) continue;
      for (int down = 1; down >= 0; --down) {
        uint64_t c0 = g_worst_cpu;
        g_worst_cpu = 0;
        uint64_t d = handle(msg({0x31, 1, (uint8_t)(code >> 7), (uint8_t)(code & 127), (uint8_t)down}));
        if (g_worst_cpu > worst_cpu_key) worst_cpu_key = g_worst_cpu, worst_cpu_at = pg << 8 | code;
        if (c0 > g_worst_cpu) g_worst_cpu = c0;
        if (d > worst) worst = d, worst_code = code | 1 << 8;
      }
    }
  }
  printf("test_poolstall: the slowest key's CPU time (Pi-scaled) %llu us (RIGHT x%d, key %02x)\n",
         (unsigned long long)worst_cpu_key, worst_cpu_at >> 8, worst_cpu_at & 255);
  printf("test_poolstall: 31 keys: the most control-core card time %llu us (key %02x, page %s)\n",
         (unsigned long long)worst, worst_code & 255, worst_code >> 8 ? "open" : "closed");
  CHECK(worst == 0);
  printf("test_poolstall: the longest handler's CPU time (Pi-scaled) %llu us (%04x)\n", (unsigned long long)g_worst_cpu,
         g_worst_cmd);

  host_card_model(false);
  cardq::reset();
  system((std::string("rm -rf '") + tmpl + "'").c_str());
  printf("test_poolstall: PASS\n");
  return 0;
}
