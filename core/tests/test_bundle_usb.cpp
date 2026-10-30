// core/tests/test_bundle_usb.cpp - bundles phase 5 (docs/BUNDLES.md "USB sticks"): export to and import from a
// removable volume (the Pi's USB: through stor_pi; here a Mac folder behind host_set_volume) in stick pieces
// (kStickPiece) at full rate only, the stick pulled in the middle of an export and of an import (the job fails at its
// next step, the card is as before, the staging gone), an eject from the bundle UIs (the job cancelled first), a
// stick too full, a stick not mounted, the bundle list of a stick (its root and /SIDECAR, with each one's path).
#include "bundle_fixture.h"

using namespace core;
using namespace core::bundle;
using namespace bfx;

static IoRule g_rule = {kPiece, kPiece, 0, ""};
static void rules(IoRule* r) { *r = g_rule; }
static const IoRule kFull = {kPiece, kPiece, 0, ""}, kPlaying = {kSlowPiece, 0, kSlowGapUs, "PLAYING"};
static void current(char* out, int cap) { snprintf(out, cap, "%s", "OTHER"); }

static std::string g_stick;              // the stick's folder ("" pulled)
static bool g_mounted = true;
static uint64_t g_free = 8ull << 30;
static int g_ejects = 0;
static int volumes(Volume* out, int max) {
  if (!g_mounted || max < 1) return 0;
  snprintf(out[0].drive, sizeof out[0].drive, "USB:");
  snprintf(out[0].name, sizeof out[0].name, "USB");
  out[0].free = g_free, out[0].total = 16ull << 30, out[0].free_known = true;
  return 1;
}
static bool vol_ok(const char* d) { return g_mounted && !strcmp(d, "USB:"); }
static const char* vol_eject(const char* d) {
  if (!vol_ok(d)) return "nothing mounted there";
  ++g_ejects;
  g_mounted = false;
  host_set_volume("USB:", nullptr);
  return nullptr;
}
static void plug(const std::string& dir) {
  g_stick = dir, g_mounted = true;
  host_set_volume("USB:", dir.c_str());
}
static void pull() {
  g_mounted = false;
  host_set_volume("USB:", nullptr);
}

static std::string export_usb(const char* key) {
  ExportOpts o;
  o.epoch = 1792677780u;
  snprintf(o.vol, sizeof o.vol, "USB:");
  CHECK(start_export(key, kToVolume, o));
  CHECK(!strcmp(status().vol, "USB:"));
  run();
  if (status().phase != kDone) fprintf(stderr, "export: %s %s\n", phase_name(status().phase), status().msg);
  CHECK(status().phase == kDone);
  std::string n = bundle_name();
  finish();
  return n;
}

int main() {
  set_usb_export(true);   // 0.3.0 ships with it off (checked below, "export to a stick is off")

  char tmpl[] = "/tmp/bundle-usb-XXXXXX";
  CHECK(mkdtemp(tmpl));
  root() = tmpl;
  host_fake_time(true, 1000000);
  Env env{};
  env.rules = rules;
  env.current = current;
  env.version = "0.2.0-test";
  env.board = "mac";
  env.flush_ms = 0;
  env.volumes = volumes;
  env.vol_ok = vol_ok;
  env.vol_eject = vol_eject;
  set_env(env);
  reset_for_tests();

  char d[kDriveMax];
  CHECK(drive_of("USB:/SIDECAR/x.sidecar.zip", d) == 4 && !strcmp(d, "USB:"));
  CHECK(drive_of("USB2:/x", d) == 5 && !strcmp(d, "USB2:"));
  CHECK(drive_of("/Bundles/x.sidecar.zip", d) == 0 && !d[0]);

  // ================= export to the stick: nothing on the card changes, every step at most a stick piece
  use_card("A");
  make_project();
  plug(folder("stick1"));
  auto card_before = tree(card_dir());
  stats_reset();
  std::string name = export_usb("LIVE SET");
  CHECK(name == "LIVE SET 20261022-1403.sidecar.zip");
  CHECK(there_at(g_stick, "/SIDECAR/" + name));
  CHECK(!there_at(g_stick, "/SIDECAR/" + name + ".part"));
  CHECK(diff(card_before, tree(card_dir())) == "");
  CHECK(stats().max_write <= kStickPiece && stats().max_write > 0);
  std::string zip = get_at(g_stick, "/SIDECAR/" + name);
  CHECK(zip.size() > 1000000 && zip.compare(0, 4, "PK\x03\x04") == 0);
  {   // the list of a stick: /SIDECAR's bundles with their path
    static char b[8192];
    int n = bundles_json("USB:/SIDECAR", b, sizeof b);
    CHECK(n > 0);
    std::string j(b, n);
    CHECK(j.find("\"path\":\"USB:/SIDECAR/" + name + "\"") != std::string::npos);
    CHECK(j.find("\"incomplete\":false") != std::string::npos);
  }

  {   // 0.3.0: export to a stick is off by default - refused with a message, nothing written to the stick
    set_usb_export(false);
    ExportOpts o0;
    sniprintf(o0.vol, sizeof o0.vol, "USB:");
    CHECK(!start_export("LIVE SET", kToVolume, o0));
    CHECK(strstr(status().msg, "export to a USB stick is off") != nullptr);
    set_usb_export(true);
  }

  // ================= while the Rytm plays: no stick I/O at all (the export waits in WRITE), then on at STOP
  {
    ExportOpts o;
    o.epoch = 1792677840u;
    snprintf(o.vol, sizeof o.vol, "USB:");
    CHECK(start_export("LIVE SET", kToVolume, o));
    CHECK(run_to(kWriting));
    g_rule = kPlaying;
    auto st0 = tree(g_stick);
    for (int i = 0; i < 200; ++i) step();
    CHECK(status().phase == kWriting && !strcmp(status().waiting, "PLAYING"));
    CHECK(diff(st0, tree(g_stick)) == "");   // not a byte (its .part was opened before PLAY)
    g_rule = kFull;
    run();
    CHECK(status().phase == kDone);
    finish();
  }

  // ================= a stick too full, and one not mounted: refused before anything is written
  g_free = 100000;
  {
    ExportOpts o;
    snprintf(o.vol, sizeof o.vol, "USB:");
    auto st0 = tree(g_stick);
    CHECK(start_export("LIVE SET", kToVolume, o));
    run();
    CHECK(status().phase == kFailed && strstr(status().msg, "USB stick full"));
    CHECK(diff(st0, tree(g_stick)) == "");
    finish();
  }
  g_free = 8ull << 30;
  {
    ExportOpts o;
    snprintf(o.vol, sizeof o.vol, "USB2:");
    CHECK(!start_export("LIVE SET", kToVolume, o));
    CHECK(strstr(status().msg, "not mounted"));
  }

  // ================= pulled in the middle of an export: it fails at its next step, the card untouched
  {
    ExportOpts o;
    o.epoch = 1792677900u;
    snprintf(o.vol, sizeof o.vol, "USB:");
    CHECK(start_export("LIVE SET", kToVolume, o));
    CHECK(run_to(kWriting));
    for (int i = 0; i < 5; ++i) step();
    CHECK(status().phase == kWriting && status().done > 0);
    pull();
    step();
    CHECK(status().phase == kFailed);
    CHECK(strstr(status().msg, "USB stick removed"));
    CHECK(diff(card_before, tree(card_dir())) == "");
    finish();
    plug(g_stick);   // back in: the export's .part is left on it (listed as incomplete), nothing else
    static char b[8192];
    std::string j(b, bundles_json("USB:/SIDECAR", b, sizeof b));
    CHECK(j.find("\"incomplete\":true") != std::string::npos);
  }

  // ================= import from the stick onto an empty card (read straight from the stick)
  use_card("B");
  auto empty = tree(card_dir());
  {
    std::string path = "USB:/SIDECAR/" + name;
    auto st0 = tree(g_stick);
    CHECK(start_verify(path.c_str()));
    CHECK(!strcmp(status().vol, "USB:"));
    stats_reset();
    run();
    CHECK(status().phase == kReady);
    CHECK(stats().max_read <= kStickPiece);
    ImportOpts io;
    CHECK(start_import(io));
    run();
    if (status().phase != kDone) fprintf(stderr, "import: %s %s\n", phase_name(status().phase), status().msg);
    CHECK(status().phase == kDone);
    finish();
    CHECK(get("/Samples/Live/loop.wav") == get_at(root() + "/A", "/Samples/Live/loop.wav"));
    CHECK(get(samples_list("LIVE SET")) == get_at(root() + "/A", samples_list("LIVE SET")));
    CHECK(there("/Plugins/sc.live-1.2.0.rmp"));
    CHECK(!there("/Bundles/.staging"));
    CHECK(diff(st0, tree(g_stick)) == "");   // the stick only read
  }

  // ================= the stick read only at full rate: VERIFY waits while the Rytm plays (the card's reads would not)
  use_card("C");
  {
    std::string path = "USB:/SIDECAR/" + name;
    g_rule = kPlaying;
    CHECK(start_verify(path.c_str()));
    for (int i = 0; i < 100; ++i) step();
    CHECK(status().phase == kVerify && !strcmp(status().waiting, "PLAYING") && status().done == 0);
    g_rule = kFull;
    run();
    CHECK(status().phase == kReady);
    finish();
  }

  // ================= pulled during VERIFY and during STAGE: the card exactly as before
  for (int when : {kVerify, kStage}) {
    use_card(when == kVerify ? "D" : "E");
    auto before = tree(card_dir());
    std::string path = "USB:/SIDECAR/" + name;
    CHECK(start_verify(path.c_str()));
    if (when == kStage) {
      run();
      CHECK(status().phase == kReady);
      ImportOpts io;
      CHECK(start_import(io));
      CHECK(run_to(kStage));
    }
    for (int i = 0; i < 6; ++i) step();
    CHECK(status().phase == when);
    pull();
    step();
    CHECK(status().phase == kFailed && strstr(status().msg, "USB stick removed"));
    if (diff(before, tree(card_dir())) != "") fprintf(stderr, "left: %s\n", diff(before, tree(card_dir())).c_str());
    CHECK(diff(before, tree(card_dir())) == "");
    finish();
    plug(g_stick);
  }

  // ================= EJECT from a bundle UI: the job on the stick cancelled first, then the platform's eject
  use_card("F");
  {
    auto before = tree(card_dir());
    std::string path = "USB:/SIDECAR/" + name;
    CHECK(start_verify(path.c_str()));
    for (int i = 0; i < 6; ++i) step();
    int e0 = g_ejects;
    CHECK(eject("USB:") == nullptr);
    CHECK(g_ejects == e0 + 1 && !g_mounted);
    CHECK(status().phase == kCancelled);
    CHECK(diff(before, tree(card_dir())) == "");
    finish();
    CHECK(eject("USB:") != nullptr);   // nothing there now: why not
    plug(g_stick);
  }
  // a card-only job is left alone by an eject
  use_card("A", false);
  {
    CHECK(start_export("LIVE SET", kToCard, ExportOpts()));
    CHECK(run_to(kHash));
    CHECK(eject("USB:") == nullptr);
    CHECK(status().phase == kHash);
    plug(g_stick);
    run();
    CHECK(status().phase == kDone);
    finish();
  }


  // ================= 2026-10-29: the end of a stick export (core 0 hung there on the Pi, the watchdog reset it): the
  // last piece, the close and the rename in passes of their own, each one behind a breadcrumb on the card that a
  // reset leaves for recover() to report at the next boot
  use_card("A", false);
  {
    ExportOpts o;
    o.epoch = 1792678020u;
    snprintf(o.vol, sizeof o.vol, "USB:");
    CHECK(start_export("LIVE SET", kToVolume, o));
    CHECK(run_to(kWriting));
    CHECK(there(kMarkPath) && get(kMarkPath).find("the write") != std::string::npos);
    CHECK(get(kMarkPath).find("export USB: USB:/SIDECAR/") != std::string::npos);
    uint64_t total = status().total;
    while (status().phase == kWriting && status().done < total) step();
    CHECK(status().phase == kWriting && status().done == total);   // every byte out, not closed yet
    std::string part = std::string("/SIDECAR/") + bundle_name() + ".part";
    CHECK(there_at(g_stick, part));
    step();   // the close: its own pass
    CHECK(status().phase == kWriting && get(kMarkPath).find("f_close of the last source file") != std::string::npos);
    CHECK(there_at(g_stick, part));
    step();   // the rename: the next pass
    CHECK(status().phase == kDone);
    CHECK(!there_at(g_stick, part) && there_at(g_stick, part.substr(0, part.size() - 5)));
    CHECK(!there(kMarkPath));   // removed once the job ended
    finish();
    // a reset in that step: the breadcrumb is still there at the next boot
    put(kMarkPath, "export USB: USB:/SIDECAR/x.sidecar.zip: f_rename of the .part (step 9)\n");
    recover();
    CHECK(strstr(status().msg, "the last run stopped in: export USB: USB:/SIDECAR/x.sidecar.zip: f_rename of the .part"));
    CHECK(!there(kMarkPath));
    // a failed stick export removes its breadcrumb too
    CHECK(start_export("LIVE SET", kToVolume, o));
    CHECK(run_to(kWriting));
    CHECK(there(kMarkPath));
    pull();
    step();
    CHECK(status().phase == kFailed && !there(kMarkPath));
    finish();
    plug(g_stick);
    // a card export writes none
    CHECK(start_export("LIVE SET", kToCard, ExportOpts()));
    CHECK(run_to(kWriting));
    CHECK(!there(kMarkPath));
    run();
    CHECK(status().phase == kDone);
    finish();
  }

  // ================= the bundle lists: one directory read an entry (no open / stat by path: O(n^2) over USB)
  {
    std::string big = folder("bigroot");
    for (int i = 0; i < 400; ++i) put_at(big, "/f" + std::to_string(i) + ".wav", "x");
    put_at(big, "/a.sidecar.zip", "PK"), put_at(big, "/b.sidecar.zip.part", "PK");
    host_set_volume("USB2:", big.c_str());
    host_card_model(true);
    BundleRow rows[8];
    int n = bundle_rows("USB2:/", rows, 8);
    uint64_t us = host_card_us(3);
    host_card_model(false);
    host_set_volume("USB2:", nullptr);
    CHECK(n == 2);
    CHECK(rows[0].incomplete != rows[1].incomplete);
    if (us >= 402 * 1000) fprintf(stderr, "the listing: %llu us modeled\n", (unsigned long long)us);
    CHECK(us < 402 * 1000);   // under one modeled open an entry (it was two opens and a stat each)
  }

  // ================= a bundle file deleted from the lists (2026-10-29: an interrupted export's .part on the stick could
  // not be): the stick's and the card's, a .part too; never the file a job writes; only bundle paths
  {
    put_at(g_stick, "/SIDECAR/old 001.sidecar.zip.part", "PK");
    put_at(g_stick, "/loose.sidecar.zip", "PK");
    CHECK(remove_bundle("USB:/SIDECAR/old 001.sidecar.zip.part") == nullptr);
    CHECK(!there_at(g_stick, "/SIDECAR/old 001.sidecar.zip.part"));
    CHECK(remove_bundle("USB:/loose.sidecar.zip") == nullptr && !there_at(g_stick, "/loose.sidecar.zip"));
    CHECK(remove_bundle("USB:/SIDECAR/old 001.sidecar.zip.part") != nullptr);   // gone already
    put("/Bundles/c 001.sidecar.zip.part", "PK");
    put("/Bundles/c 002.sidecar.zip", "PK");
    CHECK(remove_bundle("/Bundles/c 001.sidecar.zip.part") == nullptr && !there("/Bundles/c 001.sidecar.zip.part"));
    CHECK(remove_bundle("/Bundles/c 002.sidecar.zip") == nullptr && !there("/Bundles/c 002.sidecar.zip"));
    put("/Samples/Live/x.sidecar.zip", "PK");
    for (const char* bad : {"/Samples/Live/x.sidecar.zip", "/Bundles/../Samples/Live/x.sidecar.zip", "/Bundles/a/b.sidecar.zip",
                            "USB:/SIDECAR/x.wav", "USB:/Recordings/x.sidecar.zip", "SD:/Bundles/x.sidecar.zip", "/.rytm/bundle-mark.txt", ""})
      CHECK(remove_bundle(bad) != nullptr);
    CHECK(there("/Samples/Live/x.sidecar.zip"));
    // the running export's .part: refused, then deleted once the job is gone
    ExportOpts o;
    o.epoch = 1792678080u;
    snprintf(o.vol, sizeof o.vol, "USB:");
    CHECK(start_export("LIVE SET", kToVolume, o));
    CHECK(run_to(kWriting));
    std::string part = std::string(status().file) + ".part";
    CHECK(remove_bundle(part.c_str()) != nullptr);
    CHECK(remove_bundle(status().file) != nullptr);
    cancel();
    finish();
    put_at(g_stick, part.substr(4), "PK");   // (as a reset would leave it)
    CHECK(remove_bundle(part.c_str()) == nullptr);
    pull();
    put_at(g_stick, "/SIDECAR/y.sidecar.zip", "PK");
    CHECK(remove_bundle("USB:/SIDECAR/y.sidecar.zip") != nullptr);   // not mounted
    plug(g_stick);
  }

  // ================= review 2026-10-29: a failed close fails the export (FatFs's f_close: its last sync not kept), the
  // .part removed and nothing renamed into place; an import's backup (an export to the card inside an import from the
  // stick) writes no breadcrumb
  use_card("A", false);
  {
    ExportOpts o;
    o.epoch = 1792678140u;
    snprintf(o.vol, sizeof o.vol, "USB:");
    CHECK(start_export("LIVE SET", kToVolume, o));
    CHECK(run_to(kWriting));
    std::string fin = status().file, part = fin + ".part";
    host_fail_close(part.c_str(), true);
    run();
    host_fail_close(part.c_str(), false);
    CHECK(status().phase == kFailed && strstr(status().msg, "did not keep"));
    CHECK(!there_at(g_stick, fin.substr(4)) && !there_at(g_stick, part.substr(4)));
    CHECK(!there(kMarkPath));
    finish();
    // the import from the stick replacing LIVE SET: its BACKUP exports the card's project to the card
    std::string path = "USB:/SIDECAR/" + name;
    CHECK(start_verify(path.c_str()));
    run();
    CHECK(status().phase == kReady);
    ImportOpts io;
    io.mode = kReplace;
    CHECK(start_import(io));
    bool backed = false, crumb = false;
    for (int i = 0; i < 200000 && busy(); ++i) {
      if (status().phase == kBackup) backed = true;
      if (status().phase == kBackup && there(kMarkPath)) crumb = true;
      step();
      if (settled()) break;
    }
    if (status().phase != kDone) fprintf(stderr, "replace: %s %s\n", phase_name(status().phase), status().msg);
    CHECK(status().phase == kDone && backed && !crumb && !there(kMarkPath));
    finish();
  }
  system(("rm -rf '" + root() + "'").c_str());
  printf("test_bundle_usb: PASS (%d steps)\n", steps());
  return 0;
}
