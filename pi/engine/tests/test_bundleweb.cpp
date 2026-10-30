// pi/engine/tests/test_bundleweb.cpp - the web page's bundle endpoints (bundleweb.cpp) on the core's Mac port: the
// project list, an export to the card and one for the browser (every range of the served archive - at each header,
// data, padding and central-directory boundary - equal to the card's file), the strict same-origin refusals, the
// paths the page may name, an upload's import (verify, plan, import under a new name), delete, /time, the service
// worker. make -C pi/engine/tests (make -C core engines lib first).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../bundleweb.h"
#include "bundle/bundle.h"
#include "bundle/prjslot.h"
#include "lib/pools.h"
#include "fmt/fmt.h"
#include "bundle/projects.h"
#include "bundle/zip.h"
#include "check.h"
#include "core_port.h"
#include "platform.h"

using namespace core;

static std::string g_card;
static std::vector<uint8_t> g_out(256 * 1024);
static bundleweb::Reply g_r;

static std::string call(const char* path, const char* params, const char* form = "", bool page = true) {
  CHECK(bundleweb::handle(path, params, form, page, g_out.data(), (uint32_t)g_out.size(), &g_r));
  return std::string((const char*)g_out.data(), g_r.len);
}
static void put(const std::string& p, const std::string& s) {
  system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str());
  FILE* f = fopen((g_card + p).c_str(), "wb");
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static std::string get(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[65536];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static bool g_stick = false;
static int g_ejects = 0;
static int volumes(bundle::Volume* out, int max) {
  if (!g_stick || max < 1) return 0;
  snprintf(out[0].drive, sizeof out[0].drive, "USB:");
  snprintf(out[0].name, sizeof out[0].name, "USB");
  out[0].free = 3ull << 30, out[0].total = 4ull << 30, out[0].free_known = true;
  return 1;
}
static bool vol_ok(const char* d) { return g_stick && !strcmp(d, "USB:"); }
static const char* vol_eject(const char* d) {
  if (!vol_ok(d)) return "nothing mounted there";
  ++g_ejects, g_stick = false;
  return nullptr;
}
static void run() {
  for (int i = 0; i < 100000 && bundle::busy(); ++i) bundle::step();
}
static void trash_jobs() {   // 2026-10-27: the project trash's jobs (the control core's, core/bundle/prjslot.h)
  uint64_t t = 50000000000ull;   // (the clock on: a posted write nobody drives here is waited out, kFlushMs)
  host_fake_time(true, t);
  core::prjslot::tick(core::now_ms());
  for (int i = 0; i < 100000 && core::prjslot::busy(); ++i) host_fake_time(true, t += 20000), core::fmt::post_step(), core::pools::step(), core::prjslot::tick(core::now_ms());
  host_fake_time(false, 0);
}

int main() {
  char tmpl[] = "/tmp/bundleweb-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(g_card.c_str());
  host_fake_time(true, 5000000);
  put("/Samples/Drums/kick.wav", std::string(70000, 'k'));   // over one 64 KB range
  put("/Samples/Drums/hat.wav", std::string(5000, 'h'));
  put("/.rytm/projects/WEB TEST/samples.pool.txt",   // the project's SAMPLES list (core/lib/pools.h): refs by path
      "#sidecar samplepool 1\nA001 sample - 70000 /Samples/Drums/kick.wav\nA002 sample - 5000 /Samples/Drums/hat.wav\n");
  put("/.rytm/seq/WEB TEST/p001.sqp", "SQP1\x03xxxxxxxx");
  bundle::Env env{};
  env.version = "test";
  env.board = "mac";
  bundle::set_env(env);
  bundle::reset_for_tests();
  projects::reset_for_tests();

  // ---- the projects, the bundles (none yet), the worker, the clock
  std::string s = call("/bundle/projects", "");
  CHECK(g_r.status == 200 && s.find("\"key\":\"WEB TEST\"") != std::string::npos && s.find("\"files\":2") != std::string::npos);
  CHECK(call("/bundle/list", "src=card") == "[]");
  CHECK(call("/bundle/list", "src=usb") == "[]");
  s = call("/bundle-sw.js", "");
  CHECK(g_r.status == 200 && s.find("/bundle-dl/") != std::string::npos && strstr(g_r.type, "javascript"));
  call("/time", "", "epoch=1792677780");
  CHECK(g_r.status == 200);
  call("/time", "", "epoch=12");
  CHECK(g_r.status == 400);
  // the browser's time zone (minutes east of UTC: the take folders' names and the files' dates are local time)
  CHECK(core::projects::clock_zone() == 0);
  call("/time", "", "epoch=1792677780&tz=120");
  CHECK(g_r.status == 200 && core::projects::clock_zone() == 120);
  call("/time", "", "epoch=1792677780&tz=-300");
  CHECK(g_r.status == 200 && core::projects::clock_zone() == -300);
  call("/time", "", "epoch=1792677780&tz=5000");   // out of range: refused, the zone kept
  CHECK(g_r.status == 400 && core::projects::clock_zone() == -300);
  call("/time", "", "epoch=1792677780");            // none sent: the zone kept
  CHECK(g_r.status == 200 && core::projects::clock_zone() == -300);
  {
    uint32_t ep = 0;
    int src = 0;
    CHECK(core::projects::clock_local(&ep, &src) && src == core::projects::kClockBrowser && ep >= 1792677780u - 300 * 60);
  }
  CHECK(call("/bundle/status", "").find("\"clock\":\"browser\"") != std::string::npos);
  CHECK(!bundleweb::handle("/fs/ls", "", "", true, g_out.data(), 100, &g_r));   // not ours

  // ---- every POST from another page: refused, nothing started
  for (const char* p : {"/bundle/export", "/bundle/verify", "/bundle/import", "/bundle/cancel", "/bundle/delete", "/time",
                        "/bundle/trash/restore", "/bundle/trash/remove", "/bundle/trash/empty", "/bundle/remove"}) {
    call(p, "", "key=WEB+TEST&dest=card&path=/Bundles/x.sidecar.zip&epoch=1792677780&confirm=WEB+TEST", false);
    CHECK(g_r.status == 403);
  }
  CHECK(!bundle::busy() && bundle::status().phase == bundle::kIdle);

  // ---- export to the card, then the same for the browser: every range equal
  s = call("/bundle/export", "", "key=WEB+TEST&dest=card");
  CHECK(g_r.status == 200 && s.find("\"phase\":\"FLUSH\"") != std::string::npos);
  call("/bundle/export", "", "key=WEB+TEST&dest=card");
  CHECK(g_r.status == 409);   // busy
  run();
  s = call("/bundle/status", "");
  CHECK(s.find("\"phase\":\"DONE\"") != std::string::npos);
  std::string file = "/Bundles/WEB TEST 20261022-1403.sidecar.zip";
  std::string z = get(file);
  CHECK(z.size() > 70000 && s.find(file) != std::string::npos);
  call("/bundle/finish", "", "x=1");
  CHECK(call("/bundle/list", "src=card").find("WEB TEST 20261022-1403.sidecar.zip") != std::string::npos);
  call("/bundle/export", "", "key=WEB+TEST&dest=browser");
  run();
  s = call("/bundle/status", "");
  CHECK(s.find("\"phase\":\"SERVE\"") != std::string::npos && s.find("\"name\":\"WEB TEST 20261022-1403.sidecar.zip\"") != std::string::npos);
  CHECK(s.find("\"bytes\":" + std::to_string(z.size())) != std::string::npos);
  {
    // the boundaries: each entry's local header, data start and end, the central directory, the end record
    std::vector<uint64_t> at = {0, 1, 29, 30, 4095, 4096, 4097, z.size() - 22, z.size() - 1};
    for (size_t i = 0; i + 4 <= z.size(); ++i)
      if (!memcmp(z.data() + i, "PK\x03\x04", 4) || !memcmp(z.data() + i, "PK\x01\x02", 4)) at.push_back(i), at.push_back(i + 30), at.push_back(i ? i - 1 : 0);
    for (uint64_t a : at)
      for (uint32_t len : {1u, 7u, 4096u, 65536u}) {
        if (a >= z.size()) continue;
        std::string got = call("/bundle/zip", ("off=" + std::to_string(a) + "&len=" + std::to_string(len)).c_str());
        CHECK(g_r.status == 200 && got.size() == std::min<uint64_t>(len, z.size() - a));
        CHECK(!memcmp(got.data(), z.data() + a, got.size()));
      }
    call("/bundle/zip", ("off=" + std::to_string(z.size())).c_str());
    CHECK(g_r.status == 404);
    call("/bundle/zip", "off=x&len=1");
    CHECK(g_r.status == 400);
    std::string all;
    for (uint64_t off = 0; off < z.size();) {   // as the service worker: 64 KB in order
      std::string got = call("/bundle/zip", ("off=" + std::to_string(off) + "&len=65536").c_str());
      CHECK(!got.empty());
      all += got;
      off += got.size();
    }
    CHECK(all == z);
  }
  call("/bundle/cancel", "", "x=1");
  call("/bundle/finish", "", "x=1");

  // ---- an upload's import: the paths the page may name, the plan, a new name
  put("/Bundles/incoming/up.sidecar.zip", z);
  for (const char* bad : {"path=/etc/x.sidecar.zip", "path=/Bundles/../x.sidecar.zip", "path=/Bundles/x.zip", "path=/.rytm/x.sidecar.zip", ""}) {
    call("/bundle/verify", "", bad);
    CHECK(g_r.status == 400);
  }
  call("/bundle/verify", "", "path=%2FBundles%2Fincoming%2Fup.sidecar.zip");
  CHECK(g_r.status == 200);
  run();
  s = call("/bundle/plan", "");
  CHECK(g_r.status == 200 && s.find("\"name\":\"WEB TEST 2\"") != std::string::npos && s.find("\"clash\":true") != std::string::npos);
  call("/bundle/import", "", "name=ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  CHECK(g_r.status == 400);
  call("/bundle/import", "", "name=FROM+WEB&packages=1");
  CHECK(g_r.status == 200);
  run();
  s = call("/bundle/status", "");
  CHECK(s.find("\"phase\":\"DONE\"") != std::string::npos);
  CHECK(call("/bundle/plan", "").find("save it on the Rytm as FROM WEB") != std::string::npos);
  CHECK(get("/.rytm/projects/FROM WEB/samples.pool.txt") == get("/.rytm/projects/WEB TEST/samples.pool.txt"));
  call("/bundle/finish", "", "x=1");

  // ---- delete: the confirmation must match
  call("/bundle/delete", "", "key=FROM+WEB&confirm=WEB+TEST");
  CHECK(g_r.status == 409);
  call("/bundle/delete", "", "key=FROM+WEB&confirm=FROM+WEB");
  CHECK(g_r.status == 200);
  run();
  trash_jobs();   // 2026-10-27: into the project trash, not deleted
  CHECK(get("/.rytm/projects/FROM WEB/samples.pool.txt").empty() && !get("/Samples/Drums/kick.wav").empty());
  call("/bundle/finish", "", "x=1");
  // ---- the trash (2026-10-27): listed, restored, refused without its confirmation, emptied
  {
    std::string t = call("/bundle/trash", "");
    CHECK(g_r.status == 200 && t.find("\"key\":\"FROM WEB\"") != std::string::npos && t.find("\"why\":\"web\"") != std::string::npos);
    size_t p = t.find("\"id\":");
    CHECK(p != std::string::npos);
    std::string id = t.substr(p + 5, t.find(',', p) - p - 5);
    CHECK(get("/.rytm/trash/" + std::string(6 - id.size(), '0') + id + "/projects!%/samples.pool.txt") ==
          get("/.rytm/projects/WEB TEST/samples.pool.txt"));
    call("/bundle/trash/restore", "", ("id=" + id).c_str());
    CHECK(g_r.status == 200);
    trash_jobs();
    CHECK(get("/.rytm/projects/FROM WEB/samples.pool.txt") == get("/.rytm/projects/WEB TEST/samples.pool.txt"));
    CHECK(call("/bundle/prjslot", "").find("RESTORED") != std::string::npos);
    call("/bundle/delete", "", "key=FROM+WEB&confirm=FROM+WEB");
    run();
    trash_jobs();
    call("/bundle/trash/empty", "", "confirm=YES");
    CHECK(g_r.status == 400);
    call("/bundle/trash/remove", "", "id=1");
    CHECK(g_r.status == 400);   // (no confirm)
    call("/bundle/trash/empty", "", "confirm=EMPTY");
    CHECK(g_r.status == 200);
    trash_jobs();
    CHECK(call("/bundle/trash", "") == "[]");
    call("/bundle/finish", "", "x=1");
    printf("  the trash: listed, restored, emptied (confirmed)\n");
  }
  CHECK(call("/usb/status", "").find("\"present\":false") != std::string::npos);

  // ---- phase 5: a USB stick - its status, an export to it, its bundles (root and /SIDECAR), a verify from it, eject
  std::string stick = g_card + "-stick";
  system(("mkdir -p '" + stick + "'").c_str());
  host_set_volume("USB:", stick.c_str());
  g_stick = true;
  env.volumes = volumes, env.vol_ok = vol_ok, env.vol_eject = vol_eject;
  bundle::set_env(env);
  s = call("/usb/status", "");
  CHECK(s.find("\"present\":true") != std::string::npos && s.find("\"drive\":\"USB:\"") != std::string::npos &&
        s.find("\"free\":3221225472") != std::string::npos);
  call("/bundle/export", "", "key=WEB+TEST&dest=usb&vol=USB2:");   // 0.3.0: export to a stick is off (refused, said why)
  CHECK(g_r.status == 409 && call("/bundle/status", "").find("export to a USB stick is off") != std::string::npos);
  bundle::set_usb_export(true);   // the stick paths below (the host's switch; the release has it off)
  call("/bundle/export", "", "key=WEB+TEST&dest=usb&vol=USB2:");
  CHECK(g_r.status == 409 && call("/bundle/status", "").find("not mounted") != std::string::npos);
  call("/bundle/export", "", "key=WEB+TEST&dest=usb", false);
  CHECK(g_r.status == 403);
  call("/bundle/export", "", "key=WEB+TEST&dest=usb");   // vol= left out: the first stick
  CHECK(g_r.status == 200);
  run();
  CHECK(call("/bundle/status", "").find("\"phase\":\"DONE\"") != std::string::npos);
  call("/bundle/finish", "", "x=1");
  system(("cp \"" + stick + "/SIDECAR/\"*.sidecar.zip '" + stick + "/loose.sidecar.zip'").c_str());   // one at its root too
  s = call("/bundle/list", "src=usb");
  CHECK(s.find("\"path\":\"USB:/SIDECAR/WEB TEST ") != std::string::npos);
  CHECK(s.find("\"path\":\"USB:/loose.sidecar.zip\"") != std::string::npos);
  // 2026-10-29: a bundle file deleted from the lists, an interrupted export's .part on the stick too
  system(("printf PK > '" + stick + "/SIDECAR/old 001.sidecar.zip.part'").c_str());
  CHECK(call("/bundle/list", "src=usb").find("\"incomplete\":true") != std::string::npos);
  call("/bundle/remove", "", "path=USB:/SIDECAR/old+001.sidecar.zip.part");
  CHECK(g_r.status == 200);
  CHECK(call("/bundle/list", "src=usb").find("old 001") == std::string::npos);
  call("/bundle/remove", "", "path=USB:/SIDECAR/old+001.sidecar.zip.part");
  CHECK(g_r.status == 409);   // gone
  call("/bundle/remove", "", "path=/Samples/Drums/kick.wav");
  CHECK(g_r.status == 409 && !get("/Samples/Drums/kick.wav").empty());
  call("/bundle/remove", "", "x=1");
  CHECK(g_r.status == 400);
  call("/bundle/verify", "", "path=USB:/../x.sidecar.zip");
  CHECK(g_r.status == 400);
  call("/bundle/verify", "", "path=USB:/loose.sidecar.zip");
  CHECK(g_r.status == 200);
  for (int i = 0; i < 3; ++i) bundle::step();
  call("/usb/eject", "", "vol=USB:");
  CHECK(g_r.status == 200 && g_ejects == 1);
  CHECK(call("/bundle/status", "").find("\"phase\":\"CANCELLED\"") != std::string::npos);
  call("/bundle/finish", "", "x=1");
  call("/usb/eject", "", "vol=USB:");
  CHECK(g_r.status == 409);
  system(("rm -rf '" + stick + "'").c_str());

  // ---- 2026-10-29: slot mode (a card keyed by the Rytm's slots): the plan's slots, slot= checked, the import into it
  {
    put("/.rytm/prjslot.txt", "mode=slot\ntrash_next=1\n");
    core::prjslot::reset();
    core::prjslot::load();
    call("/bundle/verify", "", "path=%2FBundles%2Fincoming%2Fup.sidecar.zip");
    CHECK(g_r.status == 200);
    run();
    s = call("/bundle/plan", "");
    CHECK(s.find("\"slot\":1,\"rytm_list\":false,\"slots\":[[\"\",0,0],") != std::string::npos && s.find("\"key\":\"@001\"") != std::string::npos);
    for (const char* bad : {"slot=0", "slot=129", "slot=x", "slot=00000000000000000000005", "packages=1&slot=", "slot=5x"}) {
      call("/bundle/import", "", bad);
      CHECK(g_r.status == 400);
    }
    call("/bundle/import", "", "slot=5&packages=1");
    CHECK(g_r.status == 200);
    run();
    CHECK(call("/bundle/status", "").find("imported to slot 005") != std::string::npos);
    CHECK(get("/.rytm/projects/@005/samples.pool.txt") == get("/.rytm/projects/WEB TEST/samples.pool.txt"));
    call("/bundle/finish", "", "x=1");
    // into it again: refused without replace (its files), then replaced - the old ones in the project trash
    call("/bundle/verify", "", "path=%2FBundles%2Fincoming%2Fup.sidecar.zip");
    run();
    call("/bundle/import", "", "slot=5");
    CHECK(g_r.status == 409 && call("/bundle/status", "").find("slot 005 has Sidecar files") != std::string::npos);
    call("/bundle/import", "", "slot=5&mode=replace");
    CHECK(g_r.status == 200);
    run();
    CHECK(call("/bundle/status", "").find("in the project trash") != std::string::npos);
    CHECK(call("/bundle/trash", "").find("\"key\":\"@005\"") != std::string::npos);
    call("/bundle/finish", "", "x=1");
    core::prjslot::reset();
    printf("  slot mode: slot= checked, imported into @005, replaced into the trash\n");
  }

  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_bundleweb: OK\n");
  return 0;
}
