// core/tests/test_pool_code.cpp - the address of a SAMPLES row for a plugin's pool_info / pool_read (API 2).
// Two address spaces meet there: the lists and the Rytm use the code bank * 128 + row; the host's pool lines are
// bank * 127 + row (pool_slot). Facts checked: what a plugin's SAMPLES knob holds (the row, the bank in the fraction),
// what the old slot meaning reached (bank A only, anything computed from the list code the wrong row past bank A), and
// that um::pool_line takes both the old bank-A slot (1..127) and the list code and lands on the row's own sample.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "msamp/cardq.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "machines.h"
#include "msamp/msamp.h"
#include "nmach.h"
#include "pool/pool.h"
#include "rmp_api.h"
#include "user/um.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
static void pass() {   // the control core's pass and the card core's turn; 1 ms each
  host_card_core(3);
  for (int k = 0; k < 4; ++k) idle_stream();
  host_card_core(0);
  for (int k = 0; k < 16 && cardq::service(); ++k) {}
  host_card_core(3);
  for (int k = 0; k < 4 && lib::step(); ++k) {}
  g_us += 1000;
  host_fake_time(true, g_us);
}

static std::string row_name(int bank, int row) { return std::string("s") + (char)('a' + bank) + std::to_string(row) + ".wav"; }

int main() {
  char tmpl[] = "/tmp/poolcode-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/t' '" + g_card + "/Plugins'").c_str()) == 0);
  const int rows[] = {20, 21, 50, 127};   // (bank H's rows 1..10 are the live inputs, 11.. built-ins: not files)
  for (int b = 0; b < 8; ++b)
    for (int r : rows)
      wg::write_file(g_card + "/Samples/t/" + row_name(b, r), wg::wav_bytes(480 + b * 16 + r, 1, 16, 48000, 1, "", [](int i, int) { return 0.1 * std::sin(i * 0.1); }));
  host_set_card(g_card.c_str());
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  hooks.to_rytm = [](const uint8_t*, int) {};
  setenv("CORE_POOL_RAM_MB", "64", 1);
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);   // the Pi: the sample RAM the plugin's pool_info reads from
  cardq::reset();
  cardq::set_worker(true);
  CHECK(engine_begin() && card_ok && psram);
  snprintf(core::prj().key, sizeof core::prj().key, "LIVE");
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) pass();
  for (int i = 0; i < 4000 && !(pools::ready() && !strcmp(pools::project(), "LIVE")); ++i) pass();
  CHECK(pools::ready());
  for (int b = 0; b < 8; ++b)
    for (int r : rows) {
      char row[8];
      snprintf(row, sizeof row, "%c%03d", 'A' + b, r);
      std::string path = "/Samples/t/" + row_name(b, r);
      char c[256], o[256];
      snprintf(c, sizeof c, "pool=smp:set:%s:%s", row, path.c_str());
      host_card_core(0);
      lib::touched(path.c_str(), nullptr, 0);
      host_card_core(3);
      for (int i = 0; i < 200000 && (i < 20 || lib::busy()); ++i) pass();
      CHECK(pools::command(c, o, sizeof o));
      for (int i = 0; i < 4000 && pools::pending(); ++i) pass();
    }
  pool_sync();

  // 1. what a plugin's SAMPLES knob holds: the report's integer is the row, its fraction (lo = bank << 4) the bank
  const uint8_t raw[] = {5, 2 << 4};   // row 5, bank C
  float p[1];
  decode_params(raw, 1, p);
  CHECK((int)p[0] == 5);                                      // `(int)p[4]`: the row, whatever the bank
  CHECK(srclib::code_of(p[0]) == 2 * 128 + 5);                // the list code the lists and the Rytm use
  CHECK(pool_slot(p[0]) == 2 * kPool + 5);                    // the host's own pool line

  // 2. the old meaning: a slot was a pool line (bank * 127 + row): bank A only for 1..127, and the list code of any other
  //    bank (bank * 128 + row) read as a line is another row's sample
  for (int b = 1; b < 8; ++b)
    for (int r : rows) {
      int code = b * 128 + r, line = b * kPool + r;
      CHECK(code != line);
      if (code <= prj().npool) CHECK(strcmp(pool[code - 1].path, pool[line - 1].path) != 0);   // (the wrong row, when there is one)
    }

  // 3. the fix: pool_line takes the list code and the old bank-A slot, and gives the row's own pool line
  for (int b = 0; b < 8; ++b)
    for (int r : rows) {
      int code = b * 128 + r, line = um::pool_line(code);
      CHECK(line == b * kPool + r);
      CHECK(line >= 1 && line <= prj().npool);
      CHECK(!strcmp(pool[line - 1].path, ("t/" + row_name(b, r)).c_str()));
      if (b == 0) CHECK(um::pool_line(r) == line);   // the old slot of bank A: the same line
    }
  CHECK(um::pool_line(7 * 128 + 1) == 7 * kPool + 1 && pool[7 * kPool].folder < 0);   // H001: an input, no file: EMPTY to a plugin
  CHECK(um::pool_line(0) == 0 && um::pool_line(-1) == 0 && um::pool_line(-129) == 0 && um::pool_line(INT32_MIN) == 0);
  CHECK(um::pool_line(INT32_MAX) == 0);
  CHECK(um::pool_line(128) == 0 && um::pool_line(7 * 128) == 0);   // row 0 of a bank is OFF
  CHECK(um::pool_line(8 * 128) == 0 && um::pool_line(100000) == 0);   // no bank I
  CHECK(um::pool_line(1) == 1 && um::pool_line(127) == 127);


  // 4. the SDK's rmp_pool_code_of_knob is srclib::code_of (the host's own reading of a value): every value of a 14-bit
  //    report (row 0..127, lo 0..127), and the edges: 128 and above clamp to row 127, NaN / negative / 0 are 0
  for (int row = 0; row < 128; ++row)
    for (int lo = 0; lo < 128; ++lo) {
      float v = (float)row + (float)lo / 128.0f;
      CHECK(rmp_pool_code_of_knob(v) == srclib::code_of(v));
    }
  const float edge[] = {128.0f, 200.0f, 1000.0f, 5.9999f, 5.004f, 5.996f, 0.25f, 127.999f, -0.5f, 0.0f, NAN, 1e9f};
  for (float v : edge) CHECK(rmp_pool_code_of_knob(v) == srclib::code_of(v));
  CHECK(rmp_pool_code_of_knob(200.0f) == 127 && rmp_pool_code_of_knob(NAN) == 0 && rmp_pool_code_of_knob(-1.0f) == 0);

  // 5. through the host functions, as a plugin calls them: the first pool_info asks (NOT_READY), the control core's pass
  //    resolves it, then pool_info / pool_read answer for the same spelling, bank A by the row, any bank by the list code
  um::test_pool_voice(0);
  for (int b = 0; b < 8; ++b)
    for (int r : rows) {
      int code = b * 128 + r, frames = 480 + b * 16 + r;
      rmp_pool_info info;
      int rc = -1;
      for (int i = 0; i < 20000 && rc != RMP_POOL_OK; ++i) {
        rc = um::test_pool_info(0, code, &info);
        CHECK(rc == RMP_POOL_OK || rc == RMP_POOL_NOT_READY);
        um::test_pool_pass();
        pass();
      }
      CHECK(rc == RMP_POOL_OK && (int)info.frames == frames);   // the row's own sample (its length names the bank)
      float buf[8];
      CHECK(um::test_pool_read(0, code, 0, buf, 8) == 8);      // pool_read with the same code
      for (int i = 0; i < 8; ++i) CHECK(fabsf(buf[i] - 0.1f * sinf(i * 0.1f)) < 0.002f);
      CHECK(um::test_pool_read(0, code, frames - 3, buf, 8) == 3);   // its own length
      if (b == 0) {   // the old bank-A spelling: the row alone, the same sample
        CHECK(um::test_pool_info(0, r, &info) == RMP_POOL_OK && (int)info.frames == frames);
        CHECK(um::test_pool_read(0, r, 0, buf, 8) == 8);
      }
    }
  // a code that names no file: EMPTY, and pool_read gives nothing
  {
    rmp_pool_info info;
    float buf[4];
    CHECK(um::test_pool_info(0, 2 * 128, &info) == RMP_POOL_EMPTY);       // C OFF
    CHECK(um::test_pool_info(0, 7 * 128 + 1, &info) == RMP_POOL_EMPTY);   // H001: an input
    CHECK(um::test_pool_info(0, 8 * 128 + 5, &info) == RMP_POOL_EMPTY);   // no bank I
    CHECK(um::test_pool_info(0, 0, &info) == RMP_POOL_EMPTY && um::test_pool_info(0, -3, &info) == RMP_POOL_EMPTY);
    CHECK(um::test_pool_read(0, 3 * 128 + 7, 0, buf, 4) == 0);            // D007: not the resolved one (and empty)
  }

  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_pool_code: PASS\n");
  return 0;
}
