// core/tests/test_ui_slots.cpp - the USER SLOTS page (core/ui/page_slots.cpp): U1..U5's machines on the box's screen.
//   1. Its logic against a platform model (remote.h RemoteSlots): on the ring after SCOPE / AUTO (the A4's last: LEFT on SIDECAR) and in the SIDECAR MENU, the
//      rows, knob B through the installed list one entry a detent (list_turn's 4 counts, a hard turn not skipping), YES
//      sending the console's own um=assign / um=clear, NO dropping a pick, an empty /Plugins, no page without the hooks.
//   2. The real user machines (core/user/um.cpp, arm64 only: the packages are AArch64 images): the page assigns and
//      clears through um::command, and its slot map, its file and the slot's state are what the console's command
//      gives; an empty /Plugins says so. On the Rytm and on the A4's profile (4 voices, kRemoteUi | kUserSlots, no
//      kProject, state in /.a4), whose text goes in the frame's pixels.
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

#include "check.h"
#include "rui_png.h"
#include "core_port.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "hooks.h"
#include "ui/extra.h"
#include "ui/remote.h"
#if defined(__aarch64__)
#include "fmt/fmt.h"
#include "rmp_api.h"
#include "rmp_format.h"
#include "rmp_loader.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"
#endif

using namespace core;
using namespace core::ui;

enum : int { kLeft = 0x40, kRight = 0x41, kUp = 0x42, kDown = 0x43, kYes = 0x44, kNo = 0x45 };

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint32_t g_t = 10000;
static uint64_t g_us = 1000000000;
static void wait_ms(int ms) { host_fake_time(true, g_us += (uint64_t)ms * 1000); }

static void sx(std::vector<uint8_t> b) {   // F0 7D 52 54 <b> F7
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), b.begin(), b.end());
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void tick() { wait_ms(30), remote_tick(g_t += 30); }
static void key(int k) { sx({0x31, 1, 0, (uint8_t)k, 1}), sx({0x31, 1, 0, (uint8_t)k, 0}), tick(); }
static void turn(int e, int counts) { sx({0x32, 1, (uint8_t)e, (uint8_t)(64 + counts), 0}), tick(); }
static bool on_page(const char* t) { return remote_page_title() && !strcmp(remote_page_title(), t); }
static bool shown(const char* s) {   // a text item of the frame the box holds
  static uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  for (int i = 0; i < n; ++i)
    if (!strcasecmp((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
// The frame the box holds as a PNG (sim/rui_render.py's look; $RUI_PAGES or build/rui_pages). The text-fit check
// (check.h) runs on every frame: a text cut or past the bottom row fails the test.
static void png(const char* name) {
  static uint8_t fb[1024];
  preview_sent(fb);
  CHECK(rui_png(rui_png_dir("RUI_PAGES", "build/rui_pages"), name, fb));
}
static int mask_enc() {   // the last catch mask's encoders (37: 7 bits a byte), -1 none
  for (size_t i = g_out.size(); i-- > 0;)
    if (g_out[i].size() > 9 && g_out[i][4] == 0x37) return g_out[i][6] | g_out[i][7] << 7 | g_out[i][8] << 14;
  return -1;
}
static int lit() {   // the frame's lit pixels
  int n = 0;
  for (int i = 0; i < 1024; ++i) n += __builtin_popcount(remote_frame()[i]);
  return n;
}
// From SIDECAR to USER SLOTS on the ring: RIGHT until it shows (at most n presses); the presses it took
static int ring_to_slots(int n) {
  for (int i = 1; i <= n; ++i) {
    key(kRight);
    if (on_page("USER SLOTS")) return i;
  }
  return -1;
}
// From SIDECAR through the MENU (YES on SIDECAR, DOWN to the row, YES)
static bool menu_to_slots() {
  key(kYes);
  if (!on_page("MENU")) return false;
  for (int i = 0; i < menu_rows() + 2; ++i) key(kUp);
  // the rows this box has (the MENU leaves out what it lacks: extra_for_box) down to USER SLOTS
  int down = -1;
  for (int i = 0, r = 0; i < menu_rows() && down < 0; ++i) {
    const char* p = menu_row_page(i);
    if (p && !strcmp(p, "USER SLOTS")) down = r;
    r += extra_for_box(p ? p : "KEYS");
  }
  if (down < 0) return false;
  for (int i = 0; i < down; ++i) key(kDown);
  key(kYes);
  return on_page("USER SLOTS");
}

// ---- 1. a platform model
struct Model {
  std::vector<std::pair<std::string, std::string>> list;   // name, id
  std::string slot[5];                                     // the map's id
  std::vector<std::string> cmds;
  std::vector<std::pair<std::string, std::string>> eff;    // the installed effects: name, id
  std::string ufx[4];                                      // UFX1..UFX4's id
};
static Model M;
static const RemoteSlots kModel = {
    [] { return (int)M.list.size(); },
    [](int i, char* name, int cap, const char** id) {
      if (i < 0 || i >= (int)M.list.size()) return false;
      snprintf(name, (size_t)cap, "%s", M.list[(size_t)i].first.c_str());
      *id = M.list[(size_t)i].second.c_str();
      return true;
    },
    [](int s, char* name, int cap) -> const char* {
      const std::string& id = M.slot[s];
      std::string nm = "---";
      for (auto& e : M.list)
        if (e.second == id) nm = e.first;
      if (!id.empty() && nm == "---") nm = "!" + id.substr(id.rfind('.') + 1);
      snprintf(name, (size_t)cap, "%s", nm.c_str());
      return M.slot[s].c_str();
    },
    [](const char* c, char* o, int cap) {
      M.cmds.emplace_back(c);
      int s = -1;
      if (!strncmp(c, "um=assign:", 10)) {
        s = c[10] - '1';
        M.slot[s] = c + 12;
        snprintf(o, (size_t)cap, "U%d = %s", s + 1, c + 12);
        return true;
      }
      if (!strncmp(c, "um=clear:", 9)) {
        s = c[9] - '1';
        M.slot[s] = "";
        snprintf(o, (size_t)cap, "U%d cleared", s + 1);
        return true;
      }
      return false;
    }};

// The same platform with user effects (UFX1..UFX4): the three hooks added at the end
static const RemoteSlots kModelFx = {
    kModel.machines,
    kModel.machine,
    kModel.slot,
    [](const char* c, char* o, int cap) {
      if (!strncmp(c, "um=fx:", 6)) {
        const int u = c[6] - '1';
        M.cmds.emplace_back(c);
        M.ufx[u] = strcmp(c + 8, "-") ? c + 8 : "";
        snprintf(o, (size_t)cap, "UFX%d: %s", u + 1, M.ufx[u].empty() ? "empty" : M.ufx[u].c_str());
        return true;
      }
      return kModel.command(c, o, cap);
    },
    [] { return (int)M.eff.size(); },
    [](int i, char* name, int cap, const char** id) {
      if (i < 0 || i >= (int)M.eff.size()) return false;
      snprintf(name, (size_t)cap, "%s", M.eff[(size_t)i].first.c_str());
      *id = M.eff[(size_t)i].second.c_str();
      return true;
    },
    [](int u, char* name, int cap) -> const char* {
      const std::string& id = M.ufx[u];
      std::string nm = "---";
      for (auto& e : M.eff)
        if (e.second == id) nm = e.first;
      if (!id.empty() && nm == "---") nm = "!" + id.substr(id.rfind('.') + 1);
      snprintf(name, (size_t)cap, "%s", nm.c_str());
      return M.ufx[u].c_str();
    }};

static void open_page() {
  remote_reset();
  slots_page_reset();
  menu_reset();
  sx({0x30, 1, 0, 1, 0});   // UP + YES on the box
  CHECK(remote_open());
  tick();
}

static void test_model() {
  M = Model();
  M.list = {{"NUSAW", "rytm.nusaw"}, {"VOX", "rytm.vox"}, {"HUSH1", "rytm.hush1"}};
  M.slot[2] = "rytm.vox";
  remote_slots(nullptr);
  open_page();
  CHECK(ring_to_slots(30) < 0);   // no user machines on this platform: no page
  remote_slots(&kModel);
  open_page();
  const int at = ring_to_slots(40);   // RIGHT from SIDECAR: SYSTEM, SCOPE, SAMPLE, AUTO, then USER SLOTS (MASTER .. PROJECT after it)
  CHECK(at >= 3);
  CHECK(shown("USER SLOTS") && shown("U1") && shown("U5") && shown("VOX") && shown("B: MACHINE  YES: SET"));
  png("slots_1_rows.png");
  CHECK(mask_enc() == 0xFF);   // A..H the page's: C..H do nothing, never the stock parameter under it
  key(kLeft);
  CHECK(!on_page("USER SLOTS") && remote_stats().page == 3);   // LEFT: AUTO
  key(kNo);
  CHECK(remote_stats().page == 0);
  open_page();
  CHECK(menu_to_slots());
  printf("ui slots: on the ring after AUTO and in the SIDECAR MENU; none without the platform's user machines\n");

  // the rows: UP / DOWN and knob A, clamped
  CHECK(slots_page_row() == 0);
  key(kUp);
  CHECK(slots_page_row() == 0);
  key(kDown), key(kDown);
  CHECK(slots_page_row() == 2);
  turn(0, 1);
  CHECK(slots_page_row() == 3);
  for (int i = 0; i < 8; ++i) turn(0, 1);
  CHECK(slots_page_row() == 4);
  turn(0, -1), turn(0, -1);
  CHECK(slots_page_row() == 2);

  // knob B on U3 (VOX): the first detent moves from what it plays
  turn(1, 1);
  CHECK(slots_page_pick() == 3 && shown("HUSH1?") && shown("YES: SET U3  NO: KEEP"));
  png("slots_2_pick.png");
  turn(1, 5);   // clamped at the end
  CHECK(slots_page_pick() == 3);
  turn(1, -1), turn(1, -1);
  CHECK(slots_page_pick() == 1 && shown("NUSAW?"));
  key(kNo);   // NO: the pick dropped, the page stays
  CHECK(on_page("USER SLOTS") && slots_page_pick() == -1 && shown("KEPT") && shown("VOX") && M.cmds.empty());
  turn(1, -1);
  CHECK(slots_page_pick() == 1);
  key(kYes);   // YES: the console's command
  CHECK(M.cmds.size() == 1 && M.cmds[0] == "um=assign:3:rytm.nusaw" && M.slot[2] == "rytm.nusaw" && slots_page_pick() == -1);
  CHECK(shown("U3 = NUSAW"));
  png("slots_3_set.png");
  tick();
  CHECK(shown("NUSAW"));
  // clear: "---" first in the list
  turn(1, -1);
  CHECK(slots_page_pick() == 0 && shown("---?") && shown("YES: CLEAR U3  NO: KEEP"));
  turn(1, -1);
  CHECK(slots_page_pick() == 0);
  key(kYes);
  CHECK(M.cmds.size() == 2 && M.cmds[1] == "um=clear:3" && M.slot[2].empty() && shown("U3 CLEARED"));
  // a pick does not survive a row change, nor the page left
  turn(1, 1);
  CHECK(slots_page_pick() == 1);
  key(kDown);
  CHECK(slots_page_row() == 3 && slots_page_pick() == -1);
  turn(1, 1);
  CHECK(slots_page_pick() == 1);
  key(kNo);   // drops the pick
  key(kNo);   // back to SIDECAR
  CHECK(remote_stats().page == 0 && !on_page("USER SLOTS"));
  CHECK(ring_to_slots(40) == at && slots_page_pick() == -1);
  turn(1, 1);
  for (int i = 0; i < 30; ++i) remote_tick(g_t += 30);   // (frames while the page shows: the pick stays)
  CHECK(slots_page_pick() == 1);
  key(kLeft);   // (AUTO)
  g_t += 2000, wait_ms(2000);
  key(kRight);
  CHECK(on_page("USER SLOTS") && slots_page_pick() == -1);
  // YES without a pick: a hint, no command
  key(kYes);
  CHECK(M.cmds.size() == 2 && shown("B: PICK A MACHINE"));
  // a missing package (assigned, not installed): shown as the box shows it, the knob starts from "---"
  M.slot[3] = "rytm.gone";
  tick();
  CHECK(shown("!gone"));
  turn(1, 1);
  CHECK(slots_page_pick() == 1);
  key(kNo);
  // rc040 review L4: a rescan between knob B and YES (a package uploaded from the web) reorders the list: YES assigns
  // the machine shown, not the entry now at the picked index
  CHECK(slots_page_row() == 3 && slots_page_pick() == -1);
  turn(1, 1), turn(1, 1), turn(1, 1);
  CHECK(slots_page_pick() == 3 && shown("HUSH1?"));
  M.list = {{"AARDV", "rytm.aardv"}, {"HUSH1", "rytm.hush1"}, {"NUSAW", "rytm.nusaw"}, {"VOX", "rytm.vox"}};
  tick();
  CHECK(slots_page_pick() == 2 && shown("HUSH1?"));   // the pick follows its machine
  M.list = {{"VOX", "rytm.vox"}, {"NUSAW", "rytm.nusaw"}, {"AARDV", "rytm.aardv"}, {"HUSH1", "rytm.hush1"}};
  key(kYes);   // (reordered again, no frame in between)
  CHECK(M.cmds.size() == 3 && M.cmds[2] == "um=assign:4:rytm.hush1" && M.slot[3] == "rytm.hush1" && shown("U4 = HUSH1"));
  turn(1, 1);   // the knob steps from the held machine's place now: HUSH1 is last, so it stays
  CHECK(slots_page_pick() == 4 && shown("HUSH1?"));
  turn(1, -1);
  CHECK(slots_page_pick() == 3 && shown("AARDV?"));
  M.list = {{"VOX", "rytm.vox"}, {"NUSAW", "rytm.nusaw"}, {"HUSH1", "rytm.hush1"}};   // the picked machine removed
  tick();
  CHECK(shown("AARDV?"));   // still the one shown: YES sends its id (um::command answers "is not installed")
  key(kYes);
  CHECK(M.cmds.size() == 4 && M.cmds[3] == "um=assign:4:rytm.aardv");
  M.list = {{"NUSAW", "rytm.nusaw"}, {"VOX", "rytm.vox"}, {"HUSH1", "rytm.hush1"}};
  M.slot[3] = "rytm.gone";
  tick();
  printf("ui slots: rows, knob B through the list, YES um=assign / um=clear, NO keeps, a pick dropped on a row change or leaving\n");

  // list knobs: one machine every 4 counts while turning, the first report after a rest at once, a hard turn skips none
  list_pacing(true);
  M.list.clear();
  for (int i = 0; i < 20; ++i) M.list.push_back({"M" + std::to_string(i), "rytm.m" + std::to_string(i)});
  key(kUp), key(kUp), key(kUp), key(kUp);
  M.slot[0] = "";
  wait_ms(1000);
  kSlotsPage.enc(1, 1, false);   // after a rest: at once, whatever its size
  CHECK(slots_page_pick() == 1);
  wait_ms(67), kSlotsPage.enc(1, 1, false);
  wait_ms(67), kSlotsPage.enc(1, 2, false);
  CHECK(slots_page_pick() == 1);   // 3 counts: not yet
  wait_ms(67), kSlotsPage.enc(1, 1, false);
  CHECK(slots_page_pick() == 2);   // the 4th: one entry
  for (int i = 0; i < 12; ++i) wait_ms(67), kSlotsPage.enc(1, 30, false);   // a hard turn: one a report at most
  CHECK(slots_page_pick() == 14);
  wait_ms(1000), kSlotsPage.enc(1, -63, false);
  CHECK(slots_page_pick() == 13);
  list_pacing(false);
  key(kNo);
  printf("ui slots: knob B one machine a detent (list_turn: 4 counts, a lone report at once, a hard turn one a report)\n");

  // an empty /Plugins: says so; "---" is still there to clear a slot
  M.list.clear();
  M.slot[0] = "rytm.nusaw";
  M.cmds.clear();
  g_t += 2100, wait_ms(2100);   // ("KEPT" gone)
  tick();
  CHECK(shown("NO MACHINES IN /PLUGINS") && shown("!nusaw"));
  png("slots_4_empty_plugins.png");
  key(kYes);
  CHECK(M.cmds.empty() && shown("NO MACHINES IN /PLUGINS"));
  turn(1, 1);
  CHECK(slots_page_pick() == 0);
  key(kYes);
  CHECK(M.cmds.size() == 1 && M.cmds[0] == "um=clear:1" && M.slot[0].empty());
  printf("ui slots: an empty /Plugins says so, and a slot can still be cleared\n");

  remote_slots(nullptr);
  remote_reset();
  M = Model();
}

// ---- 1b. the model with user effects: UFX1..UFX4 after U1..U5, the list scrolling
static void test_model_fx() {
  M = Model();
  M.list = {{"NUSAW", "rytm.nusaw"}, {"VOX", "rytm.vox"}};
  M.eff = {{"PHASER", "fx.phaser"}, {"BBD", "fx.bbd"}};
  M.slot[0] = "rytm.vox";
  M.ufx[1] = "fx.bbd";
  remote_slots(&kModelFx);
  open_page();
  CHECK(ring_to_slots(40) >= 3);
  // U1..U5 first, as without effects: the window shows rows 0..4
  CHECK(shown("U1") && shown("U5") && !shown("UFX1") && shown("VOX") && shown("B: MACHINE  YES: SET"));
  png("slots_fx_1_top.png");
  // U1..U5 behave as before: knob B lists machines, YES is um=assign
  turn(1, 1);
  CHECK(slots_page_pick() == 2 && shown("VOX?"));   // (U1 plays VOX, entry 2: a step on is the end of the machines)
  turn(1, -1);
  CHECK(slots_page_pick() == 1 && shown("NUSAW?"));
  key(kYes);
  CHECK(M.cmds.size() == 1 && M.cmds[0] == "um=assign:1:rytm.nusaw" && M.slot[0] == "rytm.nusaw");
  M.cmds.clear();
  g_t += 2100, wait_ms(2100);   // ("U1 = NUSAW" gone)
  tick();
  // DOWN to UFX1 (row 5): the window scrolls, the selected row is always there
  for (int i = 0; i < 4; ++i) key(kDown);
  CHECK(slots_page_row() == 4 && shown("U5") && shown("U1"));
  key(kDown);
  CHECK(slots_page_row() == 5 && shown("UFX1") && shown("U5") && !shown("U1"));
  png("slots_fx_2_ufx1.png");
  CHECK(shown("---") && shown("B: EFFECT  YES: SET"));
  for (int i = 0; i < 3; ++i) key(kDown);
  CHECK(slots_page_row() == 8 && shown("UFX4") && shown("UFX2") && shown("BBD") && !shown("U4"));
  for (int i = 0; i < 6; ++i) key(kDown);
  CHECK(slots_page_row() == 8);   // clamped at UFX4
  turn(0, -1);
  CHECK(slots_page_row() == 7 && shown("UFX4") == true);
  for (int i = 0; i < 7; ++i) key(kUp);
  CHECK(slots_page_row() == 0 && shown("U1") && !shown("UFX1"));   // back at the top: U1 visible
  for (int i = 0; i < 5; ++i) turn(0, 1);   // knob A also scrolls
  CHECK(slots_page_row() == 5 && shown("UFX1"));
  // knob B on UFX1: effects with "---" first, from what it plays (empty)
  turn(1, 1);
  CHECK(slots_page_pick() == 1 && shown("PHASER?") && shown("YES: SET UFX1  NO: KEEP"));
  png("slots_fx_3_pick.png");
  turn(1, 1);
  CHECK(slots_page_pick() == 2 && shown("BBD?"));
  turn(1, 5);
  CHECK(slots_page_pick() == 2);   // clamped at the last effect (the machines are not in this list)
  turn(1, -1);
  CHECK(slots_page_pick() == 1);
  key(kNo);
  CHECK(slots_page_pick() == -1 && M.cmds.empty() && shown("KEPT"));
  turn(1, 1);
  key(kYes);   // YES: the console's um=fx
  CHECK(M.cmds.size() == 1 && M.cmds[0] == "um=fx:1:fx.phaser" && M.ufx[0] == "fx.phaser" && slots_page_pick() == -1);
  CHECK(shown("UFX1 = PHASER"));
  tick();
  CHECK(shown("PHASER"));
  // "---" + YES empties it
  turn(1, -1);
  CHECK(slots_page_pick() == 0 && shown("---?") && shown("YES: CLEAR UFX1  NO: KEEP"));
  key(kYes);
  CHECK(M.cmds.size() == 2 && M.cmds[1] == "um=fx:1:-" && M.ufx[0].empty() && shown("UFX1 CLEARED"));
  // a pick does not survive a row change
  turn(1, 1);
  key(kDown);
  CHECK(slots_page_row() == 6 && slots_page_pick() == -1);
  // UFX2 plays BBD: its row says so; the knob steps on from it
  CHECK(shown("BBD"));
  turn(1, -1);   // from BBD (entry 2): one back is PHASER
  CHECK(slots_page_pick() == 1 && shown("PHASER?"));
  key(kYes);
  CHECK(M.cmds.size() == 3 && M.cmds[2] == "um=fx:2:fx.phaser");
  // a missing effect: "!NAME" as the box shows it
  M.ufx[2] = "fx.gone";
  key(kDown);
  tick();
  CHECK(slots_page_row() == 7 && shown("!gone"));
  // an empty effect list says so; "---" still clears
  M.eff.clear();
  g_t += 2100, wait_ms(2100);
  tick();
  CHECK(shown("NO EFFECTS IN /PLUGINS"));
  key(kYes);
  CHECK(M.cmds.size() == 3 && shown("NO EFFECTS IN /PLUGINS"));
  turn(1, 1);
  CHECK(slots_page_pick() == 0);
  key(kYes);
  CHECK(M.cmds.size() == 4 && M.cmds[3] == "um=fx:3:-" && M.ufx[2].empty());
  // the machines rows do not see the effects (and the other way round)
  M.eff = {{"PHASER", "fx.phaser"}};
  for (int i = 0; i < 9; ++i) key(kUp);
  turn(1, 1);
  CHECK(slots_page_row() == 0 && shown("VOX?"));
  key(kNo);
  // a platform without effects: the rows stop at U5
  remote_slots(&kModel);
  open_page();
  CHECK(ring_to_slots(40) >= 3);
  for (int i = 0; i < 12; ++i) key(kDown);
  CHECK(slots_page_row() == 4 && !shown("UFX1") && shown("U5"));
  printf("ui slots with effects: UFX1..UFX4 after U1..U5, knob B through the effects, YES um=fx, scrolling keeps the row\n");
  remote_slots(nullptr);
  remote_reset();
  M = Model();
}

#if defined(__aarch64__)
// ---- 2. the real user machines (a package as test_um_scan.cpp's, smaller)
static std::string package(const char* id) {
  std::vector<uint8_t> img(8192, 0);
  auto w32 = [&](size_t a, uint32_t v) { memcpy(&img[a], &v, 4); };
  auto w64 = [&](size_t a, uint64_t v) { memcpy(&img[a], &v, 8); };
  w32(0, RMP_PLUGIN_MAGIC), w32(4, 2), w32(8, 64), w32(12, 10);
  const uint32_t init[] = {0x52800000, 0xd65f03c0};
  const uint32_t ret[] = {0xd65f03c0};
  const uint32_t render[] = {0x7100005f, 0x5400008d, 0xb800443f, 0x51000442, 0x17fffffc, 0xd65f03c0};
  memcpy(&img[0x100], init, sizeof init);
  memcpy(&img[0x110], ret, sizeof ret);
  memcpy(&img[0x120], render, sizeof render);
  const uint64_t fn[] = {0x100, 0x110, 0x110, 0x110, 0x110, 0x120};
  std::string relocs;
  for (int k = 0; k < 6; ++k) {
    w64(16 + 8 * k, fn[k]);
    rmp_reloc r = {(uint32_t)(16 + 8 * k) | (uint32_t)RMP_REL_ABS64 << 28, 0};
    relocs.append((const char*)&r, sizeof r);
  }
  rmp_manifest_head3 h3;
  memset(&h3, 0, sizeof h3);
  const char* dot = strrchr(id, '.');
  snprintf(h3.name, sizeof h3.name, "%.9s", dot ? dot + 1 : id);
  memcpy(h3.short_name, "UMP", 3), memcpy(h3.licence, "MIT", 3);
  h3.knob_count = 1;
  rmp_knob3 k;
  memset(&k, 0, sizeof k);
  k.p = 1, k.min = 0, k.max = 127, k.def = 10, k.text = RMP_TEXT_NUMBER, k.role = RMP_ROLE_FREE;
  memcpy(k.long_name, "Level", 5), memcpy(k.short_name, "LEV", 3);
  std::string man = std::string((const char*)&h3, sizeof h3) + std::string((const char*)&k, sizeof k);
  std::string body = std::string(img.begin(), img.end()) + relocs + man;
  body.append((4 - body.size() % 4) % 4, '\0');
  rmp_header h;
  memset(&h, 0, sizeof h);
  h.magic = RMP_FILE_MAGIC_A64, h.format = 3, h.api = 2;
  snprintf(h.id, sizeof h.id, "%s", id);
  h.version = h.compatible_from = 1u << 16;
  h.image_size = 8192, h.entry_offset = 0, h.reloc_count = 6, h.manifest_size = (uint32_t)man.size();
  uint32_t ds = 4096;
  memcpy(h.reserved, &ds, 4);
  h.crc32 = ~rmp_crc32_update(0xFFFFFFFFu, (const uint8_t*)body.data(), (uint32_t)body.size());
  return std::string((const char*)&h, sizeof h) + body;
}
static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
static std::string slurp(const std::string& p) {
  std::string r;
  FILE* f = fopen(host_path(p.c_str()).c_str(), "rb");
  if (!f) return r;
  char b[512];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) r.append(b, n);
  fclose(f);
  return r;
}
static void idle_all() {
  int quiet = 0;
  for (int i = 0; i < 400000; ++i) {
    bool did = um::idle();
    fmt::post_step();
    if (did) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static bool same(const um::SlotView& a, const um::SlotView& b) {
  return a.state == b.state && !strcmp(a.id, b.id) && a.version == b.version && a.src == b.src && a.flags == b.flags &&
         a.want_fp == b.want_fp && !strcmp(a.name, b.name);
}
// The page sets slot s (0..4) to installed entry e (-1: cleared) with knob B and YES: from the slot's own entry
static void page_set(int s, int e) {
  for (int i = 0; i < 10; ++i) key(kUp);
  for (int i = 0; i < s; ++i) key(kDown);
  CHECK(slots_page_row() == s);
  turn(1, -40);   // to "---"
  if (slots_page_pick() != 0) turn(1, -1);
  CHECK(slots_page_pick() == 0);
  for (int i = 0; i <= e; ++i) turn(1, 1);
  CHECK(slots_page_pick() == e + 1);
  key(kYes);
}

static void test_um(bool a4) {
  const std::string st = a4 ? "/.a4" : "/.rytm";
  char o[200];
  hooks.to_rytm = ::out;
  device::reset();
  if (!a4) {
    um::begin(host_um_exec());   // (once: it installs the page's hooks)
    CHECK(remote_slots() != nullptr);
    sx({0x10, 'U', 'S', 'L'});   // the project "USL"
  } else {   // the A4's hello (its folder chosen already: no restart), its screen's 36; no project from it
    remove(host_path("/Plugins/a.rmp").c_str()), remove(host_path("/Plugins/b.rmp").c_str());
    device::g_link.state = "/.a4";
    sx({0x4F, 1, (uint8_t)(device::kIdA4 >> 7), (uint8_t)(device::kIdA4 & 0x7F), 1});
    CHECK(device::foreign() && device::active().id == device::kIdA4 && device::active().voices == 4);
    CHECK(!(device::active().features & device::kProject) && extra_for_box("USER SLOTS") && !extra_for_box("PROJECT"));
    sx({0x36, 1, 0, 100, 1});
    project[0] = 0;   // (the box names none: the default's map)
    hooks.project_changed();
    CHECK(um::command("um=rescan", o, sizeof o));
  }
  idle_all();
  const std::string map = st + "/slots/" + (a4 ? "_default" : "USL") + ".txt";

  // an empty /Plugins
  open_page();
  if (a4) {   // the A4's ring: SIDECAR, SYSTEM, SCOPE, USER SLOTS (the Rytm's pages after it need features it lacks): LEFT on SIDECAR
    key(kLeft);
    CHECK(on_page("USER SLOTS"));
  } else {
    CHECK(ring_to_slots(40) >= 3);
  }
  if (!a4) CHECK(shown("NO MACHINES IN /PLUGINS") && shown("---"));
  else CHECK(lit() > 300);   // (the box's text is in the pixels)
  CHECK(um::installed() == 0);

  // three machines; the page lists them as the box's list has them
  put("/Plugins/a.rmp", package("rytm.alpha"));
  put("/Plugins/b.rmp", package("rytm.bravo"));
  put("/Plugins/c.rmp", package("rytm.charlie"));
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  CHECK(um::installed() == 3);
  {
    char nm[16];
    const char* id = nullptr;
    CHECK(remote_slots()->machine(1, nm, sizeof nm, &id) && !strcmp(id, "rytm.bravo") && !strcmp(nm, "bravo"));
    CHECK(!remote_slots()->machine(3, nm, sizeof nm, &id));
  }
  // U2 = bravo by the page, U3 = bravo by the console: the same state, the same lines in the map
  page_set(1, 1);
  idle_all();
  tick();
  CHECK(um::slot_view(1).state == RMP_SLOT_READY && !strcmp(um::slot_view(1).id, "rytm.bravo"));
  if (!a4) CHECK(shown("bravo") && shown("U2 = bravo"));
  png(a4 ? "slots_a4_set.png" : "slots_rytm_set.png");
  CHECK(um::command("um=assign:3:rytm.bravo", o, sizeof o) && !strcmp(o, "U3 = rytm.bravo"));
  idle_all();
  CHECK(same(um::slot_view(1), um::slot_view(2)));
  std::string f = slurp(map);
  printf("  map %s:\n%s", map.c_str(), f.c_str());
  CHECK(!f.empty());
  // the page's U2 line and the console's U3 line say the same but the slot
  auto line = [&](const std::string& t, int s) {
    for (size_t a = 0; a < t.size();) {
      size_t b = t.find('\n', a);
      std::string l = t.substr(a, b == std::string::npos ? std::string::npos : b - a);
      if (l.size() > 1 && l[0] - '0' == s) return l.substr(1);
      if (l.size() > 2 && l[0] == 'U' && l[1] - '0' == s) return l.substr(2);
      if (b == std::string::npos) break;
      a = b + 1;
    }
    return std::string("?");
  };
  CHECK(line(f, 2) != "?" && line(f, 2) == line(f, 3));
  // the web page's view (the JSON) and the display's (ui_view) carry the change
  um::publish_now();
  um::UiView uv;
  CHECK(um::ui_view(&uv) && !strcmp(uv.slot[1].name, "bravo") && !strcmp(uv.slot[2].name, "bravo"));
  // the page clears U2, the console U3: the same again
  page_set(1, -1);
  CHECK(um::command("um=clear:3", o, sizeof o) && !strcmp(o, "U3 cleared"));
  idle_all();
  tick();
  CHECK(um::slot_view(1).state == RMP_SLOT_EMPTY && same(um::slot_view(1), um::slot_view(2)));
  f = slurp(map);
  CHECK(line(f, 2) == line(f, 3));   // (both explicitly empty: neither line written)
  if (!a4) CHECK(shown("U2 CLEARED"));
  // U5 = charlie, the last entry
  page_set(4, 2);
  idle_all();
  CHECK(um::slot_view(4).state == RMP_SLOT_READY && !strcmp(um::slot_view(4).id, "rytm.charlie"));
  // a package gone from the card: the row shows it as the box does, and the page clears it
  remove(host_path("/Plugins/c.rmp").c_str());
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  tick();
  CHECK(um::installed() == 2 && um::slot_view(4).state != RMP_SLOT_READY);
  {
    char nm[16];
    remote_slots()->slot(4, nm, sizeof nm);
    printf("  U5 with its package gone: \"%s\"\n", nm);
    CHECK(nm[0] == '!');
    if (!a4) CHECK(shown(nm));
  }
  page_set(4, -1);
  idle_all();
  CHECK(um::slot_view(4).state == RMP_SLOT_EMPTY);
  if (a4) {   // the page's frames reach the box (the slot protocol's own answers, 1B / 1D, are the Rytm's: filtered as
              // for a web page's assign), its text in the pixels: no 3D
    const uint32_t d0 = device::dropped();
    g_out.clear();
    key(kDown), key(kUp), turn(1, 1), key(kNo);
    CHECK(device::dropped() == d0 && !g_out.empty());
    for (auto& m : g_out) CHECK(m.size() > 5 && (m[4] == 0x37 || m[4] == 0x38 || m[4] == 0x39 || m[4] == 0x3C));
  }
  printf("ui slots, the real user machines (%s): the page's assign / clear are the console's (state, map file, display)\n",
         a4 ? "the A4: no project, /.a4" : "the Rytm");
  device::reset();
  remote_reset();
}

// ---- 3. the real user effects (Plugin API 3 packages of tests/api3_pkgs): UFX1..UFX4 on the page are the console's um=fx
static std::string file_of(const char* path) {
  FILE* f = fopen(path, "rb");
  CHECK(f);
  std::string b;
  char t[4096];
  size_t n;
  while ((n = fread(t, 1, sizeof t, f)) > 0) b.append(t, n);
  fclose(f);
  return b;
}
static void test_um_fx() {
  char o[200];
  hooks.to_rytm = ::out;
  device::reset();
  const int machines0 = um::installed();
  put("/Plugins/ufx_pingpong.rmp", file_of("tests/api3_pkgs/ufx_pingpong.a64.rmp"));
  put("/Plugins/api3_fx.rmp", file_of("tests/api3_pkgs/api3_fx.a64.rmp"));
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  CHECK(um::installed() == machines0);   // (effects are not machines)
  const RemoteSlots* h = remote_slots();
  CHECK(h && h->effects && h->effect && h->ufx && h->effects() == 2);
  char nm0[16], nm1[16], un[16];
  const char *id0 = nullptr, *id1 = nullptr;
  CHECK(h->effect(0, nm0, sizeof nm0, &id0) && h->effect(1, nm1, sizeof nm1, &id1) && !h->effect(2, un, sizeof un, &id1));
  h->effect(1, nm1, sizeof nm1, &id1);
  std::string e0 = id0, e1 = id1;
  CHECK(e0 != e1 && !strncmp(e0.c_str(), "fx.", 3) && !strncmp(e1.c_str(), "fx.", 3));
  CHECK(!strcmp(h->ufx(0, un, sizeof un), "") && !strcmp(un, "---"));
  // the page: UFX1 = effect 0, UFX2 = effect 1 (through knob B and YES)
  open_page();
  CHECK(ring_to_slots(40) >= 3);
  page_set(5, 0);
  idle_all();
  tick();
  CHECK(um::slot_view(5).state == RMP_SLOT_READY && e0 == um::slot_view(5).id);
  CHECK(shown("UFX1") && shown(("UFX1 = " + std::string(nm0)).c_str()));
  png("slots_fx_rytm_set.png");
  const std::string f1 = file_of(host_path("/.rytm/fxslots/USL.txt").c_str());   // the console's file
  CHECK(f1.find("\t" + e0 + " 1.0.0\n") != std::string::npos);
  // UFX2 by the console: the same state as UFX1 by the page, the same kind of line in the file
  CHECK(um::command(("um=fx:2:" + e0).c_str(), o, sizeof o));
  idle_all();
  CHECK(same(um::slot_view(5), um::slot_view(6)));
  page_set(7, 1);
  idle_all();
  CHECK(um::slot_view(7).state == RMP_SLOT_READY && e1 == um::slot_view(7).id);
  const std::string f2 = file_of(host_path("/.rytm/fxslots/USL.txt").c_str());
  CHECK(f2.find("\t" + e1 + " 1.0.0\n") != std::string::npos && f2.find("\t" + e0 + " 1.0.0\n") != std::string::npos);
  // the row says what the box shows elsewhere
  tick();
  CHECK(h->ufx(0, un, sizeof un) == e0 && !strcmp(un, nm0));
  // "---" + YES empties UFX1 (the file's line goes)
  page_set(5, -1);
  idle_all();
  CHECK(um::slot_view(5).state == RMP_SLOT_EMPTY && um::slot_view(6).state == RMP_SLOT_READY);
  const std::string f3 = file_of(host_path("/.rytm/fxslots/USL.txt").c_str());
  CHECK(f3 != f2 && f3.find("\t" + e1 + " 1.0.0\n") != std::string::npos);
  // the status JSON: this build has the user effects
  um::publish_now();
  static char js[65536];
  CHECK(um::json_copy(js, sizeof js) > 0 && strstr(js, "\"ufx_slots\":4"));
  // the effect packages do not change U1..U5's list; clean up
  remove(host_path("/Plugins/ufx_pingpong.rmp").c_str()), remove(host_path("/Plugins/api3_fx.rmp").c_str());
  CHECK(um::command("um=fx:2:-", o, sizeof o) && um::command("um=fx:3:-", o, sizeof o));
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  CHECK(h->effects() == 0);
  printf("ui slots, the real user effects: the page's um=fx / empty are the console's (state, the project's fxslots file)\n");
  device::reset();
  remote_reset();
}
#endif

int main() {
  host_fake_time(true, g_us);
  hooks.to_rytm = ::out;
  char tmpl[] = "/tmp/core-uislots-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  for (const char* d : {"/Samples", "/Plugins", "/.rytm", "/.rytm/slots", "/.rytm/fxslots", "/.a4", "/.a4/slots"})
    CHECK(!mkdir(host_path(d).c_str(), 0755));
  engine_begin();
  test_model();
  test_model_fx();
#if defined(__aarch64__)
  test_um(false);
  test_um_fx();
  test_um(true);
#else
  printf("ui slots: the real user machines not run (AArch64 packages: arm64 only)\n");
#endif
  printf("test_ui_slots: PASS\n");
  return 0;
}
