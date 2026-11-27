// core/tests/nolooper/test_looper_off.cpp - 0.5.2 ships with the master looper off (core/sidecar_features.h, SIDECAR_LOOPER 0): the
// looper is in no page list, no MENU row and no XY target list, its console commands answer "not in this release", and its engine
// can never act: no ring, no grab, no playback, no Rytm track mute, no LIVE VIA, a loop.txt left on the card turns nothing on and is
// not rewritten. Built with -DSIDECAR_LOOPER=0 (core/Makefile NOLOOPER); the looper's own tests run with it on.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "core.h"
#include "engine.h"
#include "hooks.h"
#include "loop/loop.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/target.h"
#include "mod/xypad.h"
#include "ui/extra.h"
#include "ui/page_keys.h"
#include "ui/remote.h"

using namespace core;

static_assert(!SIDECAR_LOOPER, "build with -DSIDECAR_LOOPER=0");

typedef std::vector<uint8_t> Msg;
static std::vector<Msg> g_wire;
static void wire(const uint8_t* m, int n) { g_wire.emplace_back(m, m + n); }

static uint64_t g_fake = 10000000, g_frames = 0;
static bool env_beat(double* o, double* p, int* l) { *o = 10000, *p = 6000, *l = 384; return true; }   // 120 BPM: bar lines are there
static uint64_t env_frames() { return g_frames; }
static uint16_t env_muted() { return 0; }

static void slurp(const std::string& f, std::string* out) {
  out->clear();
  if (FILE* fp = fopen(f.c_str(), "rb")) {
    char b[256];
    size_t n;
    while ((n = fread(b, 1, sizeof b, fp)) > 0) out->append(b, n);
    fclose(fp);
  }
}

int main() {
  host_fake_time(true, g_fake);
  engine_begin();
  hooks.to_rytm = wire;
  mod::rytm_host_cmd(true);   // the hello's caps: the mod command is there, so a mute would go out as a host command

  // no page list has it, no MENU row, no key row
  for (int i = 0; i < ui::extra_pages(); ++i) CHECK(strcmp(ui::extra_page(i).title, "LOOP") != 0);
  CHECK(!ui::extra_for_box("LOOP"));
  for (int i = 0; i < ui::menu_rows(); ++i) CHECK(!ui::menu_row_page(i) || strcmp(ui::menu_row_page(i), "LOOP") != 0);
  CHECK(!sc_page_of_name("LOOP"));

  // the card's loop.txt from 052h: mute on, LIVE VIA 3, BAL 1: read by nothing, never rewritten
  char tmpl[] = "/tmp/core_looper_off_XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  card_ok = true;
  std::string file = std::string(tmpl) + "/.rytm/loop.txt", before, after;
  CHECK(system((std::string("mkdir -p ") + tmpl + "/.rytm; printf 'bal=1\\nlev=5\\nmute=1\\nlive=3\\nout=pre\\npostgain=3000\\n' > " + file).c_str()) == 0);
  slurp(file, &before);
  CHECK(!before.empty());

  // even handed a ring and a store, the engine stays off
  static loop::Env env;
  env.beat_map = env_beat, env.frames = env_frames, env.muted = env_muted;
  loop::set_env(&env);
  const uint32_t kRing = 1u << 16;
  std::vector<int32_t> ring(2 * (size_t)kRing, 0x55555555);
  std::vector<uint8_t> store(8u << 20, 0);
  loop::init(ring.data(), kRing, store.data(), store.size());
  CHECK(!loop::on() && !loop::wants_in() && !loop::wants_out());
  loop::load();
  CHECK(loop::bal() == 64 && loop::lev() == 100 && !loop::auto_mute() && loop::live_via() == 0 && loop::out() == loop::kOutPost && loop::postgain() == 0.0f);
  loop::set_auto_mute(true);
  loop::set_live_via(5);
  CHECK(!loop::auto_mute() && loop::live_via() == 0);
  CHECK(!loop::dirty() && !loop::save());

  // no record: MAIN blocks do not reach the ring; no playback: out_mix leaves the outputs alone
  int32_t lr[14];
  for (int i = 0; i < 14; ++i) lr[i] = 1000 + i;
  loop::in_block(lr, 7, 6 + 10000, 0);
  for (int32_t x : ring) CHECK(x == 0x55555555);
  CHECK(loop::ring_seconds() == 0);
  int32_t F[12][12];
  memset(F, 0, sizeof F);
  loop::out_mix(F, 12, 10000);
  for (auto& f : F)
    for (int32_t x : f) CHECK(x == 0);

  // no grab, no play, no keep, no stop effect
  CHECK(loop::grab(loop::k1) == loop::kErrOff);
  CHECK(!loop::play(0));
  char msg[64];
  CHECK(!loop::keep(0, msg, sizeof msg));
  loop::stop(true, true);
  CHECK(loop::state() == loop::kRing && loop::grabs() == 0 && loop::playing() == -1);
  loop::cal_start();
  CHECK(!loop::cal_wanted());

  // the console: every loop command is answered "not in this release", none changes anything
  char o[256];
  static const char* const kCmds[] = {"loop", "loop=grab:4", "loop=play:0", "loop=keep:#1", "loop=stop:cut:now", "loop=mute:on", "loop=live:3",
                                      "loop=bal:90", "loop=lev:10", "loop=fade:2", "loop=out:pre", "loop=cal"};
  for (const char* c : kCmds) {
    o[0] = 0;
    CHECK(loop::command(c, o, sizeof o));
    CHECK(strstr(o, "not in this release") != nullptr);
  }
  CHECK(!loop::auto_mute() && loop::live_via() == 0 && loop::bal() == 64 && loop::out() == loop::kOutPost && loop::grabs() == 0);
  CHECK(!loop::command("seq", o, sizeof o));   // (not a loop command: still not taken)
  CHECK(loop::json(o, sizeof o) > 0 && strstr(o, "\"on\":0") != nullptr && strstr(o, "grabs") == nullptr);

  // the control loop runs for a minute of bar lines: no Rytm message, no mute, no PERF macro move
  g_wire.clear();
  for (int ms = 0; ms < 60000; ++ms) {
    g_frames += 48, g_fake += 1000;
    host_fake_time(true, g_fake);
    mod::rytm_out_tick(now_ms());
    loop::tick((uint32_t)(1000000 + ms * 1000));
  }
  CHECK(g_wire.empty());

  // the card's loop.txt is as it was (settings are not written back)
  loop::tick(0);
  slurp(file, &after);
  CHECK(after == before);

  // LOOP is no XY target (BAL / LEV): refused; a real target still works
  mod::Tid t = 0, lt = 0;
  CHECK(mod::parse("V1:P1", &t));
  CHECK(mod::parse("LOOP:BAL", &lt));   // (the target exists: the looper's tests and a saved LFO use it)
  CHECK(!xypad::set(0, 0, lt, 50));
  CHECK(xypad::cfg().s[0][0].t == 0);
  CHECK(xypad::set(0, 0, t, 50));

  hooks.to_rytm = nullptr;
  host_fake_time(false, 0);
  printf("test_looper_off: ok\n");
  return 0;
}
