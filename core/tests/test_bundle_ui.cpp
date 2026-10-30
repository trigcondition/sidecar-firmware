// core/tests/test_bundle_ui.cpp - bundles phase 3 (docs/BUNDLES.md "PROJECT pages"): core 0's feed (the lists read
// when asked, after a job and when a stick comes or goes; the status and an import's summary published; the console
// lines) and the Rytm's PROJECT remote page driven with its keys and knobs: the list, an export to the card and to a
// stick (encoder H), the progress with WAITING (PLAYING), a cancel, an import of a bundle (its summary, the machines
// left out with T3, the done screen's "save the Rytm project as"), INFO, DELETE twice confirmed (never the current
// project), EJECT. The page itself (draw, keys, knobs: the control core's calls) never touches the card: checked
// with the host's card model charged to core 3. PROJECT_SHOTS=<dir>: the screens as PGMs (sim/rui_render-style).
#include <vector>

#include "bundle_fixture.h"
#include "rui_png.h"
#include "bundle/feed.h"
#include "bundle/prjslot.h"
#include "lib/pools.h"
#include "fmt/fmt.h"
#include "ui/extra.h"
#include "ui/ui.h"

using namespace core;
using namespace core::bundle;
using namespace bfx;
namespace ui = core::ui;

static IoRule g_rule = {kPiece, kPiece, 0, ""};
static void rules(IoRule* r) { *r = g_rule; }
static char g_cur[24] = "LIVE SET";
static void current(char* out, int cap) { snprintf(out, cap, "%s", g_cur); }
static bool g_stick = false;
static int g_ejects = 0;
static int volumes(Volume* out, int max) {
  if (!g_stick || max < 1) return 0;
  snprintf(out[0].drive, sizeof out[0].drive, "USB:");
  snprintf(out[0].name, sizeof out[0].name, "USB");
  out[0].free = 14ull << 30, out[0].total = 16ull << 30, out[0].free_known = true;
  return 1;
}
static bool vol_ok(const char* d) { return g_stick && !strcmp(d, "USB:"); }
static const char* vol_eject(const char* d) {
  if (!vol_ok(d)) return "nothing there";
  ++g_ejects, g_stick = false;
  host_set_volume("USB:", nullptr);
  return nullptr;
}

static const ui::ExtraPage* g_page = nullptr;
static ui::Canvas g_c;
static uint64_t g_card3 = 0;   // card time charged to the page's calls (the control core)
template <class F> static void on_core3(F f) {
  uint64_t a = host_card_us(3);
  host_card_core(3);
  f();
  host_card_core(0);
  g_card3 += host_card_us(3) - a;
}
static std::string screen() {   // the page drawn now: its text items, one line each
  on_core3([] { g_page->draw(g_c, 13, 14, 1000); });
  std::string s;
  for (int i = 0; i < ui::kTextSlots; ++i)
    if (g_c.txt[i].s[0]) s += std::string(g_c.txt[i].s) + "\n";
  return s;
}
static void key(int code) { on_core3([code] { g_page->key(code); }); }
static void turn(int e, int d) { on_core3([e, d] { g_page->enc(e, d, false); }); }
static uint64_t g_us = 1000000;
static void core0(int n = 1) {   // core 0's loop (a millisecond a pass): the feed's commands and lists, the job's steps
  for (int i = 0; i < n; ++i) {
    host_fake_time(true, g_us += 1000);
    feed::tick();
    if (busy()) step();
    projects::save();   // (the control core's idle save: the index's requests applied)
    fmt::post_step(), pools::step();   // (the control core's idle pass: posted writes, the lists' saves)
    prjslot::tick(now_ms());   // (2026-10-27: the control core's project trash jobs - a delete goes there)
  }
}
static void settle() {
  for (int i = 0; i < 200000; ++i) {
    core0();
    if (!busy() && status().phase != kVerify && !prjslot::busy()) break;
  }
  core0(3);
}
static bool has(const std::string& s, const char* t) {
  if (s.find(t) != std::string::npos) return true;
  fprintf(stderr, "screen without \"%s\":\n%s\n", t, s.c_str());
  return false;
}
static int g_shot = 0;
static void shot(const char* name) {
  static ui::Canvas pv;
  ui::preview(g_c, pv.fb);
  {   // rui1046: always a PNG for the docs ($RUI_PROJECT, default build/rui_project), as the other pages' renders
    static const std::string pd = rui_png_dir("RUI_PROJECT", "build/rui_project");
    char n[64];
    snprintf(n, sizeof n, "project_%02d_%s.png", g_shot + 1, name);
    CHECK(rui_png(pd, n, pv.fb));
  }
  const char* dir = getenv("PROJECT_SHOTS");
  if (!dir) { ++g_shot; return; }
  char p[512];
  snprintf(p, sizeof p, "%s/rytm-project-%02d-%s.pgm", dir, ++g_shot, name);
  FILE* f = fopen(p, "wb");
  if (!f) return;
  const int S = 4;
  fprintf(f, "P5\n%d %d\n255\n", 128 * S, 64 * S);
  for (int y = 0; y < 64 * S; ++y)
    for (int x = 0; x < 128 * S; ++x) {
      int px = x / S, py = y / S;
      bool on = pv.get(px, py);
      fputc(on ? 0xE8 : 0x10, f);
    }
  fclose(f);
}

int main() {
  core::bundle::set_usb_export(true);   // the page's stick paths (0.3.0 ships with stick export off: test_bundle_usb)
  char tmpl[] = "/tmp/bundle-ui-XXXXXX";
  CHECK(mkdtemp(tmpl));
  root() = tmpl;
  host_fake_time(true, 1000000);
  Env env{};
  env.rules = rules;
  env.current = current;
  env.version = "0.2.0-test";
  env.board = "mac";
  env.flush_ms = 0;
  env.volumes = volumes, env.vol_ok = vol_ok, env.vol_eject = vol_eject;
  set_env(env);
  reset_for_tests();
  feed::reset_for_tests();
  ui::project_page_reset();
  for (int k = 0; k < ui::extra_pages(); ++k)
    if (!strcmp(ui::extra_page(k).title, "PROJECT")) g_page = &ui::extra_page(k);
  // last on the ring, after SONG (MENU and KEYS after it are off the ring: UX review R3)
  // (MENU, KEYS, POLY CONFIG...: every page after PROJECT is off the ring, whatever their number)
  int pj = -1;
  for (int k = 0; k < ui::extra_pages(); ++k)
    if (!ui::extra_page(k).off_ring) pj = k;
  CHECK(g_page && pj > 0 && !strcmp(ui::extra_page(pj).title, "PROJECT"));
  CHECK(!strcmp(ui::extra_page(pj - 1).title, "SONG"));
  CHECK(g_page->trigs_now && g_page->trigs_now() == 0x001F && ui::extra_encs(*g_page) == 0x81);

  use_card("A");
  make_project("LIVE SET");
  make_project("JAM 1021");
  put(samples_list("JAM 1021"), samples_text({"/Samples/Live/hit.wav"}));
  host_card_model(true);
  host_card_core(0);

  // ---- not shown before the card is up; then the list (the current project first, marked *)
  feed::tick();
  CHECK(!g_page->shown());
  feed::set_available(true);
  core0(2);
  CHECK(g_page->shown());
  feed::Feed* F = new feed::Feed;
  CHECK(feed::copy(F) && F->nproj == 2 && !strcmp(F->proj[0].key, "LIVE SET") && F->proj[0].current);
  std::string s = screen();
  CHECK(has(s, "LIVE SET*") && has(s, "JAM 1021") && has(s, "T1 EXP T2 IMP T4 DEL") && has(s, "CARD"));
  uint8_t led[16];
  CHECK(g_page->leds_now(led) && led[0] == ui::kLedBlue && led[3] == 15 && led[4] == ui::kLedOff);
  shot("list");

  // ---- the console's lines
  char out[200];
  CHECK(feed::command("bundle", out, sizeof out) && strstr(out, "IDLE"));
  CHECK(!feed::command("bundles", out, sizeof out) && !feed::command("rec=x", out, sizeof out));

  // ---- T4 on the current project: refused on the page
  key(3);
  CHECK(has(screen(), "NOT DELETED"));

  // ---- T1: export JAM 1021 to the card (the cursor down on encoder A), the progress waits while the Rytm plays
  turn(0, 1);
  key(0);
  s = screen();
  CHECK(has(s, "EXPORT JAM 1021") && has(s, "TO CARD?"));
  shot("ask-export");
  g_rule = {kSlowPiece, 0, kSlowGapUs, "PLAYING"};
  key(0x44);
  for (int i = 0; i < 2000 && status().phase != kWriting; ++i) core0();
  CHECK(status().phase == kWriting);
  core0(20);
  s = screen();
  CHECK(has(s, "EXPORT JAM 1021") && has(s, "WRITE") && has(s, "WAITING (PLAYING)") && has(s, "NO: CANCEL"));
  CHECK(g_page->leds_now(led) && led[0] == ui::kLedOff);   // off while a job runs
  CHECK(feed::command("bundle=finish", out, sizeof out) && strstr(out, "a job runs") && status().phase == kWriting);
  shot("progress-waiting");
  g_rule = {kPiece, kPiece, 0, ""};
  settle();
  CHECK(status().phase == kDone);
  s = screen();
  CHECK(has(s, "DONE") && has(s, "EXPORTED JAM 1021"));
  shot("export-done");
  key(0x44);
  core0(3);
  CHECK(status().phase == kIdle);

  // ---- a cancel (NO) in the middle of an export
  key(0);
  key(0x44);
  for (int i = 0; i < 4000 && status().phase != kHash; ++i) core0();
  key(0x45);
  core0(3);
  CHECK(status().phase == kCancelled && has(screen(), "CANCELLED"));
  key(0x45);
  core0(3);

  // ---- T2: the card's bundles, YES checks one, the summary (the same card: JAM 1021 2), T3 leaves the machine out
  key(1);
  core0(3);
  s = screen();
  CHECK(has(s, "JAM 1021 0") && has(s, "YES: CHECK IT"));
  shot("bundles");
  key(0x44);
  settle();
  CHECK(status().phase == kReady);
  s = screen();
  CHECK(has(s, "IMPORT AS JAM 1021 2?") && has(s, "MACHINES") && has(s, "RYTM: SAVE AS JAM 1021 2") && has(s, "IS ON THE CARD"));
  shot("import-summary");
  key(2);
  CHECK(has(screen(), "MACHINES: 1 LEFT OUT"));
  key(0x44);
  settle();
  CHECK(status().phase == kDone);
  s = screen();
  CHECK(has(s, "SAVE THE RYTM PROJECT AS") && has(s, "JAM 1021 2"));
  shot("import-done");
  CHECK(there(samples_list("JAM 1021 2")));
  CHECK(get("/.rytm/slots/JAM 1021 2.txt").find("sc.live") != std::string::npos);
  key(0x45);
  core0(5);
  CHECK(feed::copy(F) && F->nproj == 3);

  // ---- T3 INFO, then T4 DELETE twice confirmed on the imported one
  int at = -1;
  for (int i = 0; i < F->nproj; ++i)
    if (!strcmp(F->proj[i].key, "JAM 1021 2")) at = i;
  CHECK(at >= 0);
  turn(0, -10);
  turn(0, at);
  key(2);
  s = screen();
  CHECK(has(s, "JAM 1021 2") && has(s, "SIDECAR FILES") && has(s, "IMPORTED (RYTM: NOT YET)"));
  shot("info");
  key(0x45);
  key(3);
  CHECK(has(screen(), "DELETE JAM 1021 2?"));
  key(0x44);
  CHECK(has(screen(), "RESTORE: THE WEB PAGE"));   // (2026-10-27: to the project trash)
  shot("delete-sure");
  key(0x44);
  settle();
  CHECK(status().phase == kDone && !there(samples_list("JAM 1021 2")) && there(samples_list("JAM 1021")));
  key(0x44);
  core0(5);

  // ---- a stick: H picks it (its free space, T5 EJECT lit), T1 exports into its /SIDECAR, T2 lists its bundles, T5 ejects
  std::string stick = folder("stick");
  host_set_volume("USB:", stick.c_str());
  g_stick = true;
  core0(3);   // the stick seen: the lists again
  s = screen();   // fix1049: the card still the destination, and the line names both (the hardware pass: "only USB")
  CHECK(has(s, "FREE H:CARD/USB"));
  key(0);
  CHECK(has(screen(), "TO CARD?") && has(screen(), "H: CARD/USB"));
  key(0x45);
  turn(7, 1);
  s = screen();
  CHECK(has(s, "USB 14G H:CARD T5:EJECT"));   // the way back named
  CHECK(g_page->leds_now(led) && led[4] == 63);
  shot("list-usb");
  turn(0, -10);
  key(0);
  CHECK(has(screen(), "TO USB?"));
  key(0x44);
  settle();
  CHECK(status().phase == kDone);
  key(0x44);
  core0(3);
  key(1);
  core0(3);
  s = screen();
  CHECK(has(s, "LIVE SET 0"));
  shot("bundles-usb");
  key(0x45);
  key(4);
  core0(3);
  CHECK(g_ejects == 1 && !g_stick && has(screen(), "USB: SAFE TO PULL NOW"));
  shot("ejected");
  core0(3);
  CHECK(has(screen(), "CARD"));

  // ---- 2026-10-29: slot mode (a card keyed by the Rytm's slots): the summary names the slot the import goes to; a slot
  // with Sidecar files is not replaced from here (the web page asks)
  put("/.rytm/prjslot.txt", "mode=slot\ntrash_next=1\n");
  prjslot::reset();
  prjslot::load();
  key(0x45);
  core0(3);
  key(1);
  core0(3);
  CHECK(has(screen(), "JAM 1021 0"));
  key(0x44);
  settle();
  CHECK(status().phase == kReady);
  s = screen();
  CHECK(has(s, "IMPORT TO SLOT 001?") && has(s, "RYTM: ") && has(s, "SLOT 001"));
  shot("import-slot-summary");
  key(0x44);
  settle();
  CHECK(status().phase == kDone);
  s = screen();
  CHECK(has(s, "IMPORTED TO SLOT 001") && has(s, "ON THE RYTM:"));
  shot("import-slot-done");
  CHECK(there(samples_list("@001")));
  key(0x45);
  core0(5);
  key(1);
  core0(3);
  key(0x44);
  settle();
  s = screen();
  CHECK(status().phase == kReady && has(s, "IMPORT TO SLOT 001?") && has(s, "HAS FILES: USE WEB PAGE"));
  key(0x44);   // (YES: nothing - the web page replaces)
  core0(5);
  CHECK(status().phase == kReady);
  key(0x45);
  core0(5);
  CHECK(status().phase == kIdle);
  // every slot taken on the Rytm (its slot list): no default - the page says to pick one on the web page, YES does nothing
  {
    int occ = 0;
    for (int first = 1; first <= prjslot::kSlots; first += 8) {
      std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x4B, 0x01, 0x08, (uint8_t)(first >> 7), (uint8_t)(first & 0x7F), 8};
      for (int k = first; k < first + 8; ++k, ++occ) {
        m.push_back(1);
        for (int c = 0; c < 15; ++c) m.push_back(c == 0 ? 'P' : 0);
      }
      m.push_back(0xF7);
      prjslot::sysex(m.data(), (int)m.size());
    }
    uint8_t end[] = {0xF0, 0x7D, 0x52, 0x54, 0x4B, 0x01, 0x09, (uint8_t)(occ >> 7), (uint8_t)(occ & 0x7F), 0, 0, 0xF7};
    prjslot::sysex(end, (int)sizeof end);
  }
  key(1);
  core0(3);
  key(0x44);
  settle();
  s = screen();
  CHECK(status().phase == kReady && has(s, "IMPORT: NO EMPTY SLOT") && has(s, "WEB PAGE (PROJECT TAB)"));
  shot("import-slot-pick");
  key(0x44);
  core0(5);
  CHECK(status().phase == kReady);
  key(0x45);
  core0(5);
  CHECK(status().phase == kIdle);
  prjslot::reset();

  // ---- the page never touched the card itself
  printf("test_bundle_ui: the page's own card time %llu us\n", (unsigned long long)g_card3);
  CHECK(g_card3 == 0);
  host_card_model(false);
  delete F;
  system(("rm -rf '" + root() + "'").c_str());
  printf("test_bundle_ui: PASS\n");
  return 0;
}
