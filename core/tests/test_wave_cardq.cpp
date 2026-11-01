// core/tests/test_wave_cardq.cpp - the sample overview of DSP SAMPLE's page with a card core (core/ui/page_wave.cpp, 2026-10-31): measured
// on the Pi, its reads in remote_tick held the control core 3.6-7.5 ms a column, 17 ms beside a whole load. The columns are requests on
// the card core: the same overview as without one, no card time on the control core, one request at a time, another sample chosen
// mid-read (its columns dropped, the file closed), a file that cannot be used.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "lib/pools.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "ui/extra.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;
static void out(const uint8_t*, int) {}
static void card_core() {
  host_card_core(0);
  for (int i = 0; i < 64 && cardq::service(); ++i) {}
  host_card_core(3);
}
static std::vector<uint8_t> msg(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static void ready() {
  char k[kKeyMax];
  project_key(project, k, sizeof k);
  for (int i = 0; i < 20000 && (!pools::ready() || strcmp(pools::project(), k) || pools::saving()); ++i) {
    pools::step();
    card_core();
  }
  CHECK(pools::ready());
}
static uint32_t g_ms = 1000;
static void lg(const char*, ...) {}
static uint8_t scr[256 * 1024];
static std::vector<int> run(int slot, bool worker) {   // the overview read to its end: its 128 columns
  int st = 0, prog = 0;
  const void* w = nullptr;
  for (int i = 0; i < 20000; ++i) {
    w = ui::wave_of_slot(slot, &st, &prog);
    CHECK(w);
    if (st != 1) break;
    uint64_t c = host_card_us(3);
    ui::wave_idle(++g_ms);
    if (worker) {
      CHECK(host_card_us(3) == c);
      CHECK(cardq::pending() <= 1);
      card_core();
    }
  }
  CHECK(st == 2);
  std::vector<int> o;
  for (int c = 0; c < 128; ++c) {
    int lo, hi;
    ui::wave_column(w, c, &lo, &hi);
    o.push_back(lo), o.push_back(hi);
  }
  return o;
}

int main() {
  char tmpl[] = "/tmp/core-wave-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Samples/S"));
  std::string card = tmpl;
  auto tone = [](int v) { return [v](int i, int c) { return 0.8f * sinf((float)i * 0.0007f * (float)v + (float)c); }; };
  wg::write_file(card + "/Samples/S/a.wav", wg::wav_bytes(132177, 2, 16, 44100, 1, "", tone(1)));
  wg::write_file(card + "/Samples/S/b.wav", wg::wav_bytes(90000, 1, 24, 48000, 1, "", tone(3)));
  hooks.to_rytm = out;
  CHECK(engine_begin());
  set_project((const uint8_t*)"PROJ\xf7", 5);
  ready();
  pool_sync();
  card_core();
  int folder = -1;
  for (int k = 0; k < cards.folders(); ++k)
    if (!strcmp(cards.folder(k).name, "S")) folder = k;
  CHECK(folder >= 0);
  for (int file = 0; file < 2; ++file) {
    auto m = msg({0x0F, (uint8_t)(folder >> 7), (uint8_t)(folder & 127), 0, (uint8_t)file});
    engine_sysex(m.data(), (int)m.size());
    ready();
    pool_sync();
    card_core();
  }
  CHECK(npool == 2);
  const std::vector<int> a0 = run(1, false), b0 = run(2, false);
  ui::wave_reset();
  host_card_model(true);
  cardq::reset();
  cardq::set_worker(true);
  CHECK(run(1, true) == a0);
  CHECK(run(2, true) == b0);
  ui::wave_reset();
  // another sample with a column on its way: b first, its columns not mixed into a
  int st, prog;
  ui::wave_of_slot(2, &st, &prog);
  for (int i = 0; i < 40; ++i) {
    ui::wave_idle(++g_ms);
    if (i < 39) card_core();
  }
  CHECK(cardq::pending() == 1);
  ui::wave_reset();
  card_core();   // (the cancelled request's place in the queue)
  CHECK(run(1, true) == a0);
  CHECK(run(2, true) == b0);
  // another sample chosen mid-read, no reset: the newer one read, the older one finished after it, nothing mixed
  ui::wave_reset();
  card_core();
  ui::wave_of_slot(2, &st, &prog);
  for (int i = 0; i < 20; ++i) {
    ui::wave_idle(++g_ms);
    card_core();
  }
  CHECK(run(1, true) == a0);
  CHECK(run(2, true) == b0);
  // a pool line changed mid-read (a project switch, a slot edit): the entry free again, the file read for its new line
  ui::wave_reset();
  card_core();
  ui::wave_of_slot(1, &st, &prog);
  PoolEntry pe = pool[0];
  pool_forget(0), pool_forget(1);
  pool[0] = pool[1], pool[1] = pe;   // a <-> b
  for (int i = 0; i < 20; ++i) {
    ui::wave_idle(++g_ms);
    card_core();
    pool_closes_step();
    card_core();
  }
  CHECK(run(2, true) == a0);
  CHECK(run(1, true) == b0);
  pe = pool[0], pool[0] = pool[1], pool[1] = pe;
  pool_forget(0), pool_forget(1);
  for (int i = 0; i < 10; ++i) pool_closes_step(), card_core();
  // the entry whose file is being copied is not given away: its read goes on, the pick takes another
  ui::wave_reset();
  card_core();
  {
    int fld = folder;
    for (int file = 2; file < 9; ++file) {   // 9 files: more than the cache's 8
      char n[40];
      snprintf(n, sizeof n, "/Samples/S/c%d.wav", file);
      wg::write_file(card + n, wg::wav_bytes(50000, 1, 16, 48000, 1, "", tone(file)));
    }
    cardq::set_worker(false);
    host_card_model(false);
    CHECK(cards.begin(lg, scr, sizeof scr));
    (void)fld;
    for (int file = 2; file < 9; ++file) {
      auto m = msg({0x0F, (uint8_t)(folder >> 7), (uint8_t)(folder & 127), 0, (uint8_t)file});
      engine_sysex(m.data(), (int)m.size());
      ready();
      pool_sync();
      card_core();
    }
    CHECK(npool == 9);
    host_card_model(true);
    cardq::set_worker(true);
    for (int sl = 1; sl <= 8; ++sl) run(sl, true);
    ui::wave_of_slot(9, &st, &prog);   // the first's entry, its file opening, then its copy posted
    for (int i = 0; i < 30 && !pool_open[8]; ++i) {
      ui::wave_idle(++g_ms);
      card_core();
    }
    ui::wave_idle(++g_ms);
    CHECK(cardq::pending() == 1);
    for (int sl = 2; sl <= 8; ++sl) ui::wave_of_slot(sl, &st, &prog);   // it is the oldest now
    ui::wave_of_slot(1, &st, &prog);   // an eviction
    card_core();
    run(1, true);
    run(9, true);
  }
  // a line's file closed by the card core: forgotten at once, closed a pass later, its next open after that
  {
    CHECK(pool_open[0]);
    uint32_t c0 = cardq::stats().ran[cardq::kCloseReq];
    pool_forget(0);
    CHECK(!pool_open[0] && pool_files[0].isOpen());
    CHECK(!pool_line_open(0));
    pool_closes_step();
    card_core();
    pool_closes_step();
    CHECK(!pool_files[0].isOpen() && cardq::stats().ran[cardq::kCloseReq] == c0 + 1);
    for (int i = 0; i < 6 && !pool_line_open(0); ++i) card_core();
    CHECK(pool_open[0]);
  }
  cardq::set_worker(false);
  printf("test_wave_cardq: PASS\n");
  return 0;
}
