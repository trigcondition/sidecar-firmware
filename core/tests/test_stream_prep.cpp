// core/tests/test_stream_prep.cpp - DSP SAMPLE's prepare with a card core (voices.cpp prepare_one, 7z review 2026-10-31): a
// sample changed while another's prepare is under way. SMP A -> B -> A with B's head on its way: A prepared and its trig played,
// nothing left pending; SMP off with a head on its way: silent, the head never counted.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
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
static void send(const std::vector<uint8_t>& m) { engine_sysex(m.data(), (int)m.size()); }
static void report(int code) {   // voice 3, DSP SAMPLE, P11 the pool slot's code
  std::vector<uint8_t> b = {0x03, (uint8_t)(kDspSample | 3)};
  int p[16] = {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, code, 0, 0, 120, 0, 100};
  for (int k = 0; k < 16; ++k) b.push_back((uint8_t)p[k]), b.push_back(0);
  send(msg(b));
}
static void trig(int code) {
  std::vector<uint8_t> b = {0x04, (uint8_t)(kDspSample | 3), 60, 100};
  int p[16] = {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, code, 0, 0, 120, 0, 100};
  for (int k = 0; k < 16; ++k) b.push_back((uint8_t)p[k]), b.push_back(0);
  send(msg(b));
}
static void ready() {
  char k[kKeyMax];
  project_key(prj().key, k, sizeof k);
  for (int i = 0; i < 20000 && (!pools::ready() || strcmp(pools::project(), k) || pools::saving()); ++i) {
    pools::step();
    card_core();
  }
  CHECK(pools::ready());
}
static void settle() {
  for (int i = 0; i < 200; ++i) {
    idle_prepare();
    card_core();
  }
}

int main() {
  char tmpl[] = "/tmp/core-prep-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Samples/S"));
  auto tone = [](int v) { return [v](int i, int) { return (float)v * 0.1f * (float)(i % 100) / 100.0f; }; };
  std::string card = tmpl;
  wg::write_file(card + "/Samples/S/a.wav", wg::wav_bytes(96000, 1, 16, 48000, 1, "", tone(1)));
  wg::write_file(card + "/Samples/S/b.wav", wg::wav_bytes(96000, 1, 16, 48000, 1, "", tone(5)));
  hooks.to_rytm = out;
  CHECK(engine_begin());
  cardq::reset();
  cardq::set_worker(true);
  set_project((const uint8_t*)"PROJ\xf7", 5);
  ready();
  pool_sync();
  card_core();
  int folder = -1;
  for (int k = 0; k < cards.folders(); ++k)
    if (!strcmp(cards.folder(k).name, "S")) folder = k;
  CHECK(folder >= 0);
  for (int file = 0; file < 2; ++file) {
    send(msg({0x0F, (uint8_t)(folder >> 7), (uint8_t)(folder & 127), 0, (uint8_t)file}));
    ready();
    pool_sync();
    card_core();
  }
  CHECK(prj().npool == 2);
  const int A = 1, B = 2;   // P11's codes of A001, A002
  report(A);
  settle();
  CHECK(!prep_pending[3] && !strcmp(streams[3].name(), "a.wav"));

  // A -> B -> A with B's head on its way
  report(B);
  for (int i = 0; i < 30 && !cardq::pending(); ++i) { idle_prepare(); if (!cardq::pending()) card_core(); }
  for (int i = 0; i < 8; ++i) {   // to B's head posted: its close done, its head asked for
    if (!strcmp(streams[3].name(), "b.wav")) break;
    card_core();
    idle_prepare();
  }
  CHECK(!strcmp(streams[3].name(), "b.wav") && cardq::pending() == 1 && prep_pending[3]);
  report(A);
  CHECK(prep_pending[3]);
  trig(A);
  settle();
  CHECK(!prep_pending[3] && !trig_pending[3]);
  CHECK(!strcmp(streams[3].name(), "a.wav") && streams[3].playing());
  CHECK(prepared[3][1] == pool[A - 1].file);

  // SMP off with a head on its way: no sound of it
  streams[3].stop();
  report(B);
  for (int i = 0; i < 12; ++i) {
    idle_prepare();
    if (!strcmp(streams[3].name(), "b.wav")) break;
    card_core();
  }
  CHECK(!strcmp(streams[3].name(), "b.wav") && cardq::pending() == 1);
  report(0x7f);   // an empty slot
  CHECK(!prep_pending[3]);
  card_core();
  settle();
  streams[3].trigger();
  CHECK(!streams[3].playing());
  cardq::set_worker(false);
  printf("test_stream_prep: PASS\n");
  return 0;
}
