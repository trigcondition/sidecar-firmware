// core/tests/test_learn.cpp - MIDI learn (core/mod/learn.h, spec-050 2.4, 2.10): a learned CC moves the scenes' fader
// (7 bit, 14 bit), picks A / B, or adds a bipolar offset to a target; the routes' file round trip and a newer file
// locked; a CC no route has is not taken.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "mod/learn.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/target.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void run_ms(int ms) {
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), mod::tick(now_us());
}
static void fresh() {
  mod::reset();
  scene::reset();
  learn::reset();
  at(g_us + 1000), mod::tick(now_us());
}
static std::string rd(const std::string& p) {
  std::string s;
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "<none>";
  char b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void wr(const std::string& p, const std::string& s) {
  FILE* f = fopen((g_card + p).c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static void flush() {
  at(g_us + 1100000);
  for (int i = 0; i < 64; ++i) idle_save();
}

static void learn_fader() {
  fresh();
  CHECK(learn::arm("fader") && learn::armed() == learn::kFader);
  CHECK(learn::midi(1, 0xB2, 7, 64));   // CC 7 on port 1 ch 3: learned and applied
  CHECK(learn::armed() < 0 && scene::state().fader == 8256);   // 64 x 129
  CHECK(learn::midi(1, 0xB2, 7, 127) && scene::state().fader == 16383);
  CHECK(!learn::midi(2, 0xB2, 7, 10) && !learn::midi(1, 0xB3, 7, 10) && scene::state().fader == 16383);   // another port / ch
  printf("learn: the fader by a 7-bit CC (64 -> 8256)\n");
}

static void cc14_bit() {
  fresh();
  learn::arm("fader");
  CHECK(learn::midi(0, 0xB0, 1, 64));   // the Rytm's port: CC 1 (MSB)
  CHECK(learn::midi(0, 0xB0, 33, 5));   // CC 33: its LSB
  CHECK(scene::state().fader == (64 << 7 | 5));
  CHECK(learn::midi(0, 0xB0, 1, 100) && learn::midi(0, 0xB0, 33, 127) && scene::state().fader == (100 << 7 | 127));
  printf("learn: CC n + CC n + 32: 14 bits\n");
}

static void route_offset_bipolar() {
  fresh();
  v[0].p[2] = 60;
  char o[400];
  CHECK(learn::command("learn=V1:P3:32", o, sizeof o) && !strstr(o, "cannot"));
  CHECK(learn::midi(3, 0xB0, 20, 127));
  run_ms(3);
  CHECK(fabsf(v[0].p[2] - 92.0f) < 0.01f);   // base 60 + 32
  CHECK(learn::midi(3, 0xB0, 20, 0));
  run_ms(3);
  CHECK(fabsf(v[0].p[2] - 28.0f) < 0.01f);   // 60 - 32
  CHECK(learn::midi(3, 0xB0, 20, 64));
  run_ms(3);
  CHECK(fabsf(v[0].p[2] - 60.25f) < 0.01f);  // (64 - 63.5) / 63.5 x 32
  CHECK(mod::find(mod::tid(mod::kV, 0, 2)) && mod::base_of(0, 2, 0) == 60.0f);
  CHECK(learn::command("learn=clear:1", o, sizeof o) && strstr(o, "learn: 0 routes"));
  run_ms(3);
  CHECK(v[0].p[2] == 60.0f && !mod::find(mod::tid(mod::kV, 0, 2)));   // the route gone: back to the base
  printf("learn: a target route adds a bipolar offset (127 +32, 0 -32)\n");
}

static void routes_file_round_trip() {
  fresh();
  learn::arm("fader"), learn::midi(1, 0xB0, 7, 0);
  learn::arm("a"), learn::midi(0, 0xBD, 102, 0);
  learn::arm("MFX:LEV:-20"), learn::midi(2, 0xB5, 21, 64);
  CHECK(learn::dirty());
  flush();
  std::string f = rd("/.rytm/learn.txt");
  printf("learn: the file:\n%s", f.c_str());
  CHECK(f == "#sidecar learn 1\nR 1 1 7 fader\nR 0 14 102 a\nR 2 6 21 MFX:LEV -20\n");
  learn::reset();
  learn::load();
  const learn::Route* r;
  CHECK(learn::routes(&r) == 3 && !learn::locked());
  CHECK(r[0].port == 1 && r[0].ch == 0 && r[0].cc == 7 && r[0].what == learn::kFader);
  CHECK(r[1].port == 0 && r[1].ch == 13 && r[1].cc == 102 && r[1].what == learn::kA);
  CHECK(r[2].port == 2 && r[2].ch == 5 && r[2].cc == 21 && r[2].what == learn::kTarget && r[2].depth == -20 &&
        r[2].t == mod::tid(mod::kMfx, 0, 0));
  // a newer build's line: what it can read, never written
  const std::string newer = "#sidecar learn 1\nR 1 1 7 fader\nR 1 1 8 ZZ9:Q1 5\n";
  wr("/.rytm/learn.txt", newer);
  learn::load();
  CHECK(learn::routes(&r) == 1 && learn::locked());
  learn::arm("b"), learn::midi(1, 0xB0, 9, 1);
  flush();
  CHECK(rd("/.rytm/learn.txt") == newer);
  ::remove((g_card + "/.rytm/learn.txt").c_str());
  learn::load();
  printf("learn: the routes' file round trip; a newer file is kept\n");
}

static void ab_by_cc() {
  fresh();
  learn::arm("a");
  CHECK(learn::midi(1, 0xB0, 30, 4) && scene::state().a == 4);   // value 4: scene 5
  learn::arm("b");
  CHECK(learn::midi(1, 0xB0, 31, 11) && scene::state().b == 11);
  CHECK(learn::midi(1, 0xB0, 30, 40) && scene::state().a == 4);   // 12..127: nothing
  printf("learn: A / B by a CC's value (0..11)\n");
}

static void unarmed_cc_not_taken() {
  fresh();
  CHECK(!learn::midi(1, 0xB0, 7, 64) && !learn::midi(1, 0x90, 60, 100));
  learn::arm("fader");
  CHECK(!learn::midi(1, 0x90, 60, 100) && learn::armed() == learn::kFader);   // a note never learns
  learn::disarm();
  printf("learn: a CC no route has is not taken\n");
}

int main() {
  char tmpl[] = "/tmp/core_learn_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples' '" + g_card + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  learn_fader();
  cc14_bit();
  route_offset_bipolar();
  routes_file_round_trip();
  ab_by_cc();
  unarmed_cc_not_taken();
  printf("test_learn: PASS\n");
  return 0;
}
