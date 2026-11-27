// core/tests/test_loop_two.cpp - the master looper with two boxes (core/loop/loop.h: one looper per box, decided 2026-11-27):
// each slot's ring, grabs, bars, settings, card file and returns are its own; slot 0 and slot 1 behave the same.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core.h"
#include "device/device.h"
#include "fmt/fmt.h"
#include "host/core_port.h"
#include "loop/loop.h"
#include "msamp/cardq.h"

using namespace core;

static const uint32_t kRingFrames = 1u << 20;   // 21.8 s: room for a bar at both tempos
static const int64_t kD = 1000;                 // a block's last frame arrives this late (host frame = ring index + kD)
static int g_hand = 0;                          // the slot in hand (the pages' / console's)
static int hand() { return g_hand; }
static std::string read_file(const std::string& p) {
  std::string t;
  if (FILE* f = fopen(p.c_str(), "rb")) {
    char b[512];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n);
    fclose(f);
  }
  return t;
}

// each box's own clock (its own transport and tempo) and its own source
struct Box {
  double origin, per16;
  int lat;
  int32_t (*sig)(uint64_t);
  uint64_t fed;
  int32_t* ring;
  std::vector<uint8_t> store;
};
static Box g_box[2];
static uint64_t g_now = 0;
static uint32_t g_us = 1000000;
static uint64_t g_fake = 10000000;

static bool beat0(double* o, double* p, int* l) { *o = g_box[0].origin, *p = g_box[0].per16, *l = g_box[0].lat; return true; }
static bool beat1(double* o, double* p, int* l) { *o = g_box[1].origin, *p = g_box[1].per16, *l = g_box[1].lat; return true; }
static uint64_t frames() { return g_now; }
static uint16_t muted() { return 0; }

static int32_t ramp(uint64_t k) { return (int32_t)(k + 1); }
static int32_t neg3(uint64_t k) { return -(int32_t)(3 * (k + 1)); }

static double bar(int s) { return 16 * g_box[s].per16; }
static double line(int s, double k) { return g_box[s].origin + g_box[s].lat + k * bar(s); }

static void feed() {
  for (int s = 0; s < 2; ++s) {
    Box& b = g_box[s];
    while ((int64_t)(b.fed + 6 + kD) <= (int64_t)g_now) {
      int32_t x[14];
      for (int i = 0; i < 7; ++i) x[2 * i] = b.sig(b.fed + i), x[2 * i + 1] = -x[2 * i];
      loop::in_block(s, x, 7, b.fed + 6 + kD, 0);
      b.fed += 7;
    }
  }
}
static void to(uint64_t h) {   // (never back: both boxes share the one render clock)
  if (h < g_now) h = g_now;
  while (g_now + 48 <= h) {
    g_now += 48, g_us += 1000, g_fake += 1000;
    host_fake_time(true, g_fake);
    feed();
    loop::tick(g_us);
  }
  g_now = h;
  feed();
  loop::tick(g_us);
}
static void in_hand(int s) { g_hand = s; }

// slot sA's box: origin / tempo A, source ramp; slot sB's box: origin / tempo B, source neg3. Each box's env is set in its slot.
static void fresh(int sA = 0) {
  loop::set_slot_fn(hand);
  loop::reset();
  static loop::Env env[2];
  for (int s = 0; s < 2; ++s) {
    Box& b = g_box[s];
    bool a = s == sA;
    b.origin = a ? 10000 : 13000, b.per16 = a ? 6000 : 5000, b.lat = a ? 384 : 300, b.sig = a ? ramp : neg3, b.fed = 0;
    if (!b.ring) b.ring = new int32_t[2 * (size_t)kRingFrames];
    b.store.assign(16u << 20, 0);
    env[s].beat_map = s == 0 ? beat0 : beat1, env[s].frames = frames, env[s].muted = muted;
    in_hand(s);
    loop::set_env(&env[s]);
    loop::init(s, b.ring, kRingFrames, b.store.data(), b.store.size());
    static const uint8_t post[2] = {0, 1}, pre[2] = {2, 3};
    loop::set_returns(s, post, pre);
  }
  loop::reset();
  host_fake_time(true, g_fake);
  g_now = 0, g_us = 1000000;
  in_hand(0);
}

static int32_t played(int s, uint64_t h, int ch = 0) {
  int32_t F[12][12] = {};
  loop::out_mix(s, F, 12, h);
  return F[0][ch];
}

// a 1-bar grab on slot s pressed a third into its bar 40, playing from its bar 41
static void grab_on(int s) {
  in_hand(s);
  to((uint64_t)(line(s, 40) + bar(s) / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
}

static void test_own_ring_and_grabs() {
  fresh();
  grab_on(0);
  to(g_now + 48 * 60);
  in_hand(0);
  CHECK(loop::grabs() == 1 && loop::grab_at(0)->frames == (uint32_t)(16 * 6000));   // its own tempo's bar
  in_hand(1);
  CHECK(loop::grabs() == 0 && loop::state() == loop::kRing);   // the other box: nothing
  uint64_t s0 = (uint64_t)(g_box[0].origin + 41 * bar(0));
  to(s0 + 500);
  in_hand(0);
  CHECK(loop::state() == loop::kPlaying);
  CHECK(played(0, s0 + 5000) == loop::grab_at(0)->pcm[2 * 5000] && played(0, s0 + 5000) > 0);   // its own box's audio (the ramp)
  CHECK(played(1, s0 + 5000) == 0);   // nothing onto the other box's returns
  // the other box grabs too: its own bar length, its own audio, each loop on its own box
  grab_on(1);
  to(g_now + 48 * 60);
  CHECK(loop::grabs() == 1 && loop::grab_at(0)->frames == (uint32_t)(16 * 5000));
  uint64_t s1 = (uint64_t)(g_box[1].origin + std::ceil((g_now + 96 - g_box[1].origin) / bar(1)) * bar(1));
  to(s1 + 500);
  CHECK(loop::state() == loop::kPlaying);
  CHECK(played(1, s1 + 3000) < 0 && played(1, s1 + 3000) == loop::grab_at(0)->pcm[2 * 3000]);
  in_hand(0);
  CHECK(loop::grabs() == 1 && loop::state() == loop::kPlaying && played(0, s1 + 3000) > 0);
  loop::stop(true, true);   // a stop on one box leaves the other playing
  to(g_now + 48 * 30);
  CHECK(loop::state() == loop::kRing);
  in_hand(1);
  CHECK(loop::state() == loop::kPlaying);
  CHECK(loop::stats(0).grabs == 1 && loop::stats(1).grabs == 1);
  printf("  each box its own ring, grabs, bars and returns ok\n");
}

static void test_own_settings_and_card() {
  fresh();
  in_hand(1);
  loop::set_lev(50), loop::set_fade(loop::kFade4), loop::set_out(loop::kOutPre);
  in_hand(0);
  CHECK(loop::lev() == 100 && loop::fade() == loop::kFade1 && loop::out() == loop::kOutPost);
  // a box without a PRE return (a stereo-mix box): OUT = PRE refused, its loop on the channels it has
  static const uint8_t post1[2] = {6, 7};
  loop::set_returns(1, post1, nullptr);
  in_hand(1);
  CHECK(!loop::has_pre(1) && loop::has_pre(0));
  CHECK(loop::out() == loop::kOutPre);   // (kept as set: it plays on POST while the box has no PRE, test_post_channels)
  loop::set_out(loop::kOutPost);
  loop::set_out(loop::kOutPre);
  CHECK(loop::out() == loop::kOutPost);
  // each box's loop.txt in its own folder: written and read in its slot's Scope (state_path), as the engine's per-slot save
  // loop and its attach read (core/pool/pool.cpp kSettings) do
  char tmpl[] = "/tmp/core_loop2_XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  card_ok = true;
  system((std::string("mkdir -p ") + tmpl + "/.rytm " + tmpl + "/.a4").c_str());
  device::g_links[1].state = "/.a4";
  in_hand(0), loop::set_bal(20);
  in_hand(1), loop::set_bal(110);
  for (int s = 0; s < 2; ++s) {
    device::Scope in(device::g_links[s]);
    in_hand(s);
    CHECK(loop::dirty() && loop::save());
    for (int i = 0; i < 2000 && fmt::post_busy(); ++i) {
      fmt::post_step();
      host_card_core(0);
      while (cardq::service()) {
      }
      host_card_core(3);
    }
    CHECK(!loop::dirty());
  }
  CHECK(read_file(std::string(tmpl) + "/.rytm/loop.txt").find("bal=20\n") != std::string::npos);
  CHECK(read_file(std::string(tmpl) + "/.a4/loop.txt").find("bal=110\n") != std::string::npos);
  loop::reset();
  for (int s = 0; s < 2; ++s) {
    device::Scope in(device::g_links[s]);
    in_hand(s);
    loop::load();
  }
  in_hand(0);
  CHECK(loop::bal() == 20 && loop::lev() == 100);
  in_hand(1);
  CHECK(loop::bal() == 110 && loop::lev() == 50 && loop::fade() == loop::kFade4);
  // the attach read: the text taken into the slot in hand's settings
  loop::take_text("bal=33\nlev=90\n", 14);
  CHECK(loop::bal() == 33 && loop::lev() == 90);
  in_hand(0);
  CHECK(loop::bal() == 20);
  device::g_links[1].state = nullptr;
  card_ok = false;
  printf("  each box its own settings, returns and loop.txt ok\n");
}

static void test_post_channels() {
  fresh();
  static const uint8_t post1[2] = {6, 7};
  in_hand(1);
  loop::set_out(loop::kOutPre);   // set while the box had a PRE return, then a box without one: on its POST
  loop::set_returns(1, post1, nullptr);
  grab_on(1);
  to(g_now + 48 * 60);
  uint64_t s1 = (uint64_t)(g_box[1].origin + std::ceil((g_now + 96 - g_box[1].origin) / bar(1)) * bar(1));
  to(s1 + 500);
  int32_t F[12][12] = {};
  loop::out_mix(1, F, 12, s1 + 3000);
  CHECK(F[0][6] != 0 && F[0][7] != 0 && F[0][0] == 0 && F[0][1] == 0 && F[0][2] == 0);
  printf("  a loop on its box's own return channels ok\n");
}

// the same box in slot 0 or in slot 1: the same loop, sample for sample
static std::vector<int32_t> scenario(int s) {
  fresh(s);
  grab_on(s);
  to(g_now + 48 * 60);
  in_hand(s);
  loop::set_lev(80);
  uint64_t s0 = (uint64_t)(g_box[s].origin + 41 * bar(s));
  to(s0 + 500);
  std::vector<int32_t> v;
  for (uint64_t h = s0 + 1000; h < s0 + 1000 + 12 * 200; h += 12) {
    int32_t F[12][12] = {};
    loop::out_mix(s, F, 12, h);
    for (int f = 0; f < 12; ++f) v.push_back(F[f][0]), v.push_back(F[f][1]);
  }
  char j[1200];
  CHECK(loop::json(j, sizeof j) > 0 && strstr(j, "\"state\":\"playing\""));
  return v;
}
static void test_symmetric() {
  std::vector<int32_t> a = scenario(0), b = scenario(1);
  CHECK(a.size() == b.size() && a == b);
  bool any = false;
  for (int32_t x : a) any |= x != 0;
  CHECK(any);
  printf("  slot 0 and slot 1 the same, sample for sample ok\n");
}

int main() {
  test_own_ring_and_grabs();
  test_own_settings_and_card();
  test_post_channels();
  test_symmetric();
  printf("test_loop_two: OK\n");
  return 0;
}
