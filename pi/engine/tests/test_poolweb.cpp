// pi/engine/tests/test_poolweb.cpp - the web page's Pool tab JSON (poolweb.cpp, GET /pool) on the core's Mac port:
// before the lists exist (no bank), the template's banks (SAMPLES H: the inputs and PAD / BELLS, WAVETABLES A: the
// built-in tables, every row's nine fields), the list and bank arguments (tags, digits, letters, defaults), a file added
// (its name, state, path, size), the used count and the voices on a row, u=1's unused rows with the warning, a buffer
// too small. make -C pi/engine/tests (make -C core engines lib first).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../poolweb.h"
#include "check.h"
#include "core_port.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "msamp/cardq.h"
#include "platform.h"

namespace core {
extern bool card_ok;        // pool/pool.h's (its header needs the engines' include paths)
extern char project[24];
}
using namespace core;

static uint64_t g_us = 50000000;
static std::vector<char> g_out(192 * 1024);

static std::string get(const char* params, int cap = 0) {
  int n = poolweb::json(params, g_out.data(), cap ? cap : (int)g_out.size());
  return std::string(g_out.data(), n > 0 ? n : 0);
}
// brackets and quotes balance (the JSON's shape; the page parses it)
static bool balanced(const std::string& s) {
  int depth = 0;
  bool str = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (str) {
      if (c == '\\') ++i;
      else if (c == '"') str = false;
      continue;
    }
    if (c == '"') str = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') { if (--depth < 0) return false; }
  }
  return depth == 0 && !str;
}
// a mono 16-bit 48 kHz WAV of n frames of silence
static void wav(const std::string& path, uint32_t n) {
  FILE* f = fopen(path.c_str(), "wb");
  CHECK(f);
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f), u32(36 + n * 2), fwrite("WAVEfmt ", 1, 8, f), u32(16), u16(1), u16(1), u32(48000), u32(96000);
  u16(2), u16(16), fwrite("data", 1, 4, f), u32(n * 2);
  std::vector<int16_t> z(n, 100);
  fwrite(z.data(), 2, n, f);
  fclose(f);
}
static void run(int ms) {
  for (int i = 0; i < ms; ++i) {
    for (int k = 0; k < 8 && (pools::step() || srclib::step()); ++k) {}
    if (cardq::worker())
      for (int k = 0; k < 8 && cardq::service(); ++k) {}
    g_us += 1000;
    host_fake_time(true, g_us);
  }
}

int main() {
  // before the lists exist: the status is empty, no bank
  std::string j = get("l=smp&b=0");
  CHECK(balanced(j) && j.find("\"bank\":null") != std::string::npos && j.find("\"ram\":{\"budget\":0,") != std::string::npos);

  char tmpl[] = "/tmp/poolweb-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples/Drums'").c_str()) == 0);
  wav(card + "/Samples/Drums/kick \"808\".wav", 4800);
  host_set_card(card.c_str());
  host_fake_time(true, g_us);
  card_ok = true;
  cardq::reset();
  cardq::set_worker(true);
  lib::begin(true);
  srclib::begin(true);
  pools::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  snprintf(core::project, sizeof core::project, "%s", "WEB");
  for (int i = 0; i < 2000 && (!pools::ready() || strcmp(pools::project(), "WEB")); ++i) run(1);
  CHECK(pools::ready());

  // the template: SAMPLES bank H holds the ten inputs and PAD / BELLS
  j = get("l=smp&b=7");
  CHECK(balanced(j));
  CHECK(j.find("\"st\":{\"project\":\"WEB\",\"ready\":true,\"smp\":{\"n\":12,") != std::string::npos);
  CHECK(j.find("\"bank\":{\"list\":\"smp\",\"bank\":7,") != std::string::npos);
  CHECK(j.find("[1,\"input\",\"IN\",\"INPUT\",\"\",0,0,0,0,0,0]") != std::string::npos);   // (+ channels, always in RAM)
  CHECK(j.find("[11,\"builtin\",\"PAD\",\"BUILT-IN\",\"\",0,0,0,0,0,0]") != std::string::npos);
  CHECK(j.find("\"unused\"") == std::string::npos);
  CHECK(get("l=smp&b=H") == j && get("b=h&l=smp") == j);   // a letter, either order
  // WAVETABLES bank A: the six built-in tables
  j = get("l=wt&b=0");
  CHECK(balanced(j) && j.find("\"list\":\"wt\",\"bank\":0") != std::string::npos);
  CHECK(j.find("[1,\"builtin\",\"BASIC\",\"BUILT-IN\"") != std::string::npos && j.find("[6,\"builtin\",\"SYNC\"") != std::string::npos);
  // INSTRUMENTS: empty; an unknown list or bank: SAMPLES bank A
  j = get("l=inst&b=3");
  CHECK(balanced(j) && j.find("\"list\":\"inst\",\"bank\":3,\"locked\":false,\"ro\":false,\"count\":0,\"rows\":[]") != std::string::npos);
  CHECK(get("l=xx&b=9").find("\"list\":\"smp\",\"bank\":0,") != std::string::npos);
  CHECK(get("").find("\"list\":\"smp\",\"bank\":0,") != std::string::npos);

  // a file into SAMPLES C001: its name (a quote escaped), state, path, size; then played and on two voices now
  int out = -1;
  CHECK(pools::add_file(pools::kSamples, 2, "/Samples/Drums/kick \"808\".wav", &out) == pools::kOk && out == pools::code(2, 1));
  pools::seen(pools::kSamples, out);
  pools::seen(pools::kSamples, out);
  pools::seen(pools::kSamples, out);
  // (voice_on counts only voices on GRANULAR now: none in this host test, so users stays 0)
  pools::voice_on(pools::kSamples, 1, out);
  pools::voice_on(pools::kSamples, 5, out);
  j = get("l=smp&b=2");
  CHECK(balanced(j));
  char want[200];
  snprintf(want, sizeof want, "[1,\"sample\",\"kick \\\"808\\\"\",\"EXACT\",\"/Samples/Drums/kick \\\"808\\\".wav\",3,0,%u,0,0,0]",
           (unsigned)pools::at(pools::kSamples, out).ref.size);
  CHECK(pools::at(pools::kSamples, out).ref.size > 4800);
  CHECK(j.find(want) != std::string::npos);

  // u=1: SELECT UNUSED's rows of the whole list (files only: the inputs, PAD and BELLS hold no RAM; C001 is in use) and
  // the warning
  j = get("l=smp&b=2&u=1");
  CHECK(balanced(j));
  CHECK(j.find("\"unused\":[],\"warn\":\"NOT SEEN PLAYING - A RYTM LOCK MAY STILL USE IT\"") != std::string::npos);
  pools::voice_on(pools::kSamples, 1, -1), pools::voice_on(pools::kSamples, 5, -1);

  // too small a buffer: nothing (web.cpp answers {}), never a cut JSON
  CHECK(get("l=smp&b=2", 100).empty());
  j = get("l=smp&b=2", 700);
  CHECK(j.empty() || balanced(j));

  system(("rm -rf '" + card + "'").c_str());
  printf("test_poolweb: ok\n");
  return 0;
}
