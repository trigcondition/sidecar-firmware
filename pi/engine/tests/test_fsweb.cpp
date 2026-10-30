// pi/engine/tests/test_fsweb.cpp - the web file browser's logic (fsweb.cpp) on a folder standing in for the card:
// paths and the read-only boot files, a 20 MB upload in the page's 48 KB chunks (each chunk's time measured: the
// longest is what one request holds core 0's web task on the Pi, before its card's own speed), resume, a wrong
// CRC, a full card, packages checked before they go in place (good RMPA, a Teensy RMP1, a damaged one, junk),
// delete, mkdir, rename; the kept folders (/.rytm, /Samples, /Plugins) and config.txt.bak; a dropped folder
// (mkdirs, then each file). FSWEB_PKG: a good RMPA package (default plugins/sine_test/sine_test.a64.rmp; its test
// skips without it).
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include "check.h"
#include "fsweb.h"
#include "lib/sha256.h"

static std::string g_root;
static std::string mac(const char* p) { return g_root + p; }
static uint64_t g_free = 1ull << 40;
static int g_yields = 0;

static uint16_t g_fdate = 0, g_ftime = 0;   // every entry's FAT date / time (0: none written, no clock)
static bool b_stat(const char* p, uint64_t* size, bool* dir) {
  struct stat st;
  if (::stat(mac(p).c_str(), &st)) return false;
  *size = (uint64_t)st.st_size;
  *dir = S_ISDIR(st.st_mode);
  return true;
}
static int b_list(const char* p, fsweb::Entry* out, int max, bool* more) {
  DIR* d = opendir(mac(p).c_str());
  if (!d) return -1;
  int n = 0;
  *more = false;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (n == max) { *more = true; break; }
    snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
    std::string full = std::string(p) + (strcmp(p, "/") ? "/" : "") + e->d_name;
    b_stat(full.c_str(), &out[n].size, &out[n].dir);
    out[n].fdate = g_fdate, out[n].ftime = g_ftime;   // (FatFs's, as FILINFO has them: the test's own)
    ++n;
  }
  closedir(d);
  return n;
}
static bool b_read(const char* p, uint64_t off, void* buf, uint32_t n, uint32_t* got) {
  FILE* f = fopen(mac(p).c_str(), "rb");
  if (!f) return false;
  fseeko(f, (off_t)off, SEEK_SET);
  *got = (uint32_t)fread(buf, 1, n, f);
  fclose(f);
  return true;
}
static bool b_write(const char* p, uint64_t off, const void* buf, uint32_t n, bool create) {
  FILE* f = fopen(mac(p).c_str(), create ? "wb" : "r+b");
  if (!f) return false;
  fseeko(f, (off_t)off, SEEK_SET);
  bool ok = fwrite(buf, 1, n, f) == n;
  fclose(f);
  return ok;
}
static bool b_remove(const char* p) { return ::remove(mac(p).c_str()) == 0; }
static bool b_rename(const char* a, const char* b) { return ::rename(mac(a).c_str(), mac(b).c_str()) == 0; }
static bool b_mkdir(const char* p) { return ::mkdir(mac(p).c_str(), 0755) == 0; }
static bool b_space(uint64_t* f, uint64_t* t) { *f = g_free; *t = 1ull << 41; return true; }
static void b_yield() { ++g_yields; }
static std::vector<std::string> g_changed;
static void b_changed(const char* p) { g_changed.push_back(p); }
// the library's hooks (core/lib/library.h): what an upload, a delete and a rename report
struct LibEv { char op; std::string a, b, sha; uint32_t crc; };
static std::vector<LibEv> g_lib;
static void b_stored(const char* p, const uint8_t sha[32], uint32_t crc) {
  char h[65];
  core::lib::hex(sha, 32, h);
  g_lib.push_back({'s', p, "", h, crc});
}
static void b_removed(const char* p) { g_lib.push_back({'d', p, "", "", 0}); }
static void b_renamed(const char* a, const char* b) { g_lib.push_back({'r', a, b, "", 0}); }
// the take being recorded, as rec_pi::TakeBusy asks fsweb::live_take (its id: "take_009", "USB:take_009")
static const char* g_live = nullptr;
static bool b_busy(const char* p) { return g_live && fsweb::live_take(p, g_live); }
static const fsweb::Backend B = {b_stat, b_list, b_read, b_write, b_remove, b_rename, b_mkdir, b_space, b_yield, b_changed,
                                 b_stored, b_removed, b_renamed, b_busy};

static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  uint8_t b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}

// The whole file through put() in the page's chunks; returns the last result (and the longest chunk in us).
static fsweb::Result upload(const char* path, const std::vector<uint8_t>& data, uint32_t chunk = 48 * 1024,
                            double* max_us = nullptr, uint32_t crc_fix = 0) {
  uint32_t crc = fsweb::crc32(0, data.data(), (uint32_t)data.size()) ^ crc_fix;
  fsweb::Result r{0, ""};
  double worst = 0;
  for (uint64_t off = 0; off < data.size() || (data.empty() && off == 0);) {
    uint32_t n = (uint32_t)std::min<uint64_t>(chunk, data.size() - off);
    auto t0 = std::chrono::steady_clock::now();
    fsweb::put(B, path, off, data.size(), crc, data.data() + off, n, &r);
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (us > worst) worst = us;
    if (r.status != 200) break;
    off += n;
    if (data.empty()) break;
  }
  if (max_us) *max_us = worst;
  return r;
}

int main() {
  char tmpl[] = "/tmp/fsweb-XXXXXX";
  g_root = mkdtemp(tmpl);
  mkdir(mac("/Plugins").c_str(), 0755);
  mkdir(mac("/Samples").c_str(), 0755);
  mkdir(mac("/overlays").c_str(), 0755);
  b_write("/config.txt", 0, "arm_64bit=1\n", 12, true);

  // paths and the boot files
  CHECK(fsweb::path_ok("/Samples/a b.wav") && !fsweb::path_ok("Samples") && !fsweb::path_ok("/a/../b") &&
        !fsweb::path_ok("/a//b") && !fsweb::path_ok("/a\\b") && !fsweb::path_ok("/x:y"));
  for (const char* p : {"/config.txt", "/CMDLINE.TXT", "/sdboot.img", "/engine.img", "/engine.img.prev", "/kernel8-rpi4.img",
                        "/start4.elf", "/fixup4.dat", "/bcm2711-rpi-4-b.dtb", "/overlays", "/overlays/x.dtbo", "/",
                        "/tryboot.txt", "/armstub8.bin"})
    CHECK(fsweb::read_only(p));
  for (const char* p : {"/Samples/config.txt", "/Plugins/a.rmp", "/notes.txt", "/.rytm/slots/X.txt"}) CHECK(!fsweb::read_only(p));
  fsweb::Result r;
  // the content confinement (reachable()) now answers first and says "no such path": a boot file is not a file this
  // API has, rather than one it refuses to write. read_only() above still holds, behind it.
  fsweb::put(B, "/config.txt", 0, 3, fsweb::crc32(0, (const uint8_t*)"abc", 3), (const uint8_t*)"abc", 3, &r);
  CHECK(r.status == 404);
  fsweb::remove(B, "/overlays", &r);
  CHECK(r.status == 404);
  fsweb::rename(B, "/config.txt", "/c.txt", &r);
  CHECK(r.status == 404);
  CHECK(slurp(mac("/config.txt")).size() == 12);

  // ---- what this API can reach at all: content only, every operation (fsweb.h "WHAT IT CAN REACH"). /fs/get
  // served /.rytm/wifi.txt before this, and a write could land on /.rytm/pair.txt, which is a way past the
  // pairing code itself.
  mkdir(mac("/.rytm").c_str(), 0755);
  b_write("/.rytm/wifi.txt", 0, "ssid=Fake\npsk=not-a-real-pass\n", 30, true);
  b_write("/.rytm/pair.txt", 0, "v=1\n", 4, true);
  mkdir(mac("/Recordings").c_str(), 0755);
  for (const char* p : {"/.rytm", "/.rytm/wifi.txt", "/.rytm/pair.txt", "/.rytm/slots/X.txt", "/config.txt",
                        "/cmdline.txt", "/engine.img", "/sdboot.img", "/overlays", "/overlays/x.dtbo", "/notes.txt",
                        "/System Volume Information", "/Sample", "/Samples2", "/samplesx"})
  {
    CHECK(!fsweb::reachable(p));
    char buf[4096];
    fsweb::Result q;
    CHECK(fsweb::list_json(B, p, buf, sizeof buf, &q) == 0 && q.status == 404);
    fsweb::put(B, p, 0, 1, fsweb::crc32(0, (const uint8_t*)"x", 1), (const uint8_t*)"x", 1, &q);
    CHECK(q.status == 404);
    fsweb::remove(B, p, &q);
    CHECK(q.status == 404);
    fsweb::mkdir(B, p, &q);
    CHECK(q.status == 404);
    fsweb::mkdirs(B, p, &q);
    CHECK(q.status == 404);
    fsweb::rename(B, p, "/Samples/x.wav", &q);		// neither end of a move may reach out
    CHECK(q.status == 404);
    fsweb::rename(B, "/Samples/big.bin", p, &q);
    CHECK(q.status == 404);
  }
  {								// untouched
    auto w = slurp(mac("/.rytm/wifi.txt"));
    std::string ws(w.begin(), w.end());
    auto pr = slurp(mac("/.rytm/pair.txt"));
    CHECK(ws.find("psk=") != std::string::npos && pr.size() == 4);
  }
  // the content folders themselves are reachable, and so is everything under them
  for (const char* p : {"/", "/Samples", "/Samples/Kit/kick.wav", "/Plugins", "/Plugins/a.rmp", "/Recordings",
                        "/Bundles/incoming/x.zip", "/MULTISAMPLE/Piano/Piano.sfz", "/WAVETABLES/a.wav",
                        "/GRANULAR/x.wav", "/PRESETS/p.txt", "/MACHINES/m.rmp", "/NAM/a.nam", "/CHOP/K/a.chop.sfz",
                        "/JUNO/a.syx", "/DX7/a.syx"})
    CHECK(fsweb::reachable(p));
  // and they still work: a folder, an upload, a rename, a delete
  fsweb::mkdir(B, "/Samples/Kit", &r);
  CHECK(r.status == 200);
  fsweb::put(B, "/Samples/Kit/a.wav", 0, 3, fsweb::crc32(0, (const uint8_t*)"abc", 3), (const uint8_t*)"abc", 3, &r);
  CHECK(r.status == 200);
  fsweb::rename(B, "/Samples/Kit/a.wav", "/Samples/Kit/b.wav", &r);
  CHECK(r.status == 200);
  fsweb::remove(B, "/Samples/Kit/b.wav", &r);
  CHECK(r.status == 200);
  fsweb::remove(B, "/Samples/Kit", &r);
  CHECK(r.status == 200);
  g_lib.clear();		// those four were this block's own: the library's calls are checked further down
  // the card's root lists the content folders and nothing else
  {
    char buf[8192];
    fsweb::Result q;
    int n = fsweb::list_json(B, "/", buf, sizeof buf, &q);
    CHECK(n > 0 && q.status == 200);
    std::string j(buf, (size_t)n);
    CHECK(j.find("\"n\":\"Samples\"") != std::string::npos && j.find("\"n\":\"Plugins\"") != std::string::npos);
    for (const char* hid : {"config.txt", ".rytm", "overlays", "cmdline.txt"})
      CHECK(j.find(std::string("\"n\":\"") + hid + "\"") == std::string::npos);
  }

  // a USB drive's recordings (/USB/Recordings: the recorder's destination): listed, downloaded, deleted; nothing else
  // of the drive, nothing written there
  {
    const char* rest = nullptr;
    char vp[64];
    CHECK(fsweb::drive_of("/USB/Recordings/t", &rest) == 1 && !strcmp(rest, "/Recordings/t"));
    CHECK(fsweb::drive_of("/USB2/Recordings", &rest) == 2 && fsweb::drive_of("/NVME", &rest) == 4 && !strcmp(rest, "/"));
    CHECK(fsweb::drive_of("/USBX/Recordings", &rest) == 0 && fsweb::drive_of("/Recordings") == 0);
    CHECK(fsweb::volume_path("/USB/Recordings/t/00_MASTER.wav", vp, sizeof vp) && !strcmp(vp, "USB:/Recordings/t/00_MASTER.wav"));
    CHECK(fsweb::volume_path("/USB3/Recordings", vp, sizeof vp) && !strcmp(vp, "USB3:/Recordings"));
    CHECK(fsweb::volume_path("/Samples/a.wav", vp, sizeof vp) && !strcmp(vp, "SD:/Samples/a.wav"));
    CHECK(!fsweb::volume_path("/Samples/a.wav", vp, 8));
    for (const char* p : {"/USB/Recordings", "/USB/Recordings/take_004", "/USB2/Recordings/t/00_MASTER.wav", "/NVME/Recordings"})
      CHECK(fsweb::reachable(p));
    for (const char* p : {"/USB", "/USB/", "/USB/SIDECAR/x.zip", "/USB/.rytm/wifi.txt", "/USB/config.txt", "/USB/Recordings/../x",
                          "/USBX/Recordings", "/USB/RecordingsX", "/USB:/Recordings"})
      CHECK(!fsweb::reachable(p));
    mkdir(mac("/USB").c_str(), 0755);
    mkdir(mac("/USB/Recordings").c_str(), 0755);
    mkdir(mac("/USB/Recordings/take_004").c_str(), 0755);
    FILE* f = fopen(mac("/USB/Recordings/take_004/00_MASTER.wav").c_str(), "wb");
    fputs("RIFF", f), fclose(f);
    char buf[4096];
    fsweb::Result q;
    int n = fsweb::list_json(B, "/USB/Recordings/take_004", buf, sizeof buf, &q);
    CHECK(n > 0 && q.status == 200 && std::string(buf, (size_t)n).find("\"n\":\"00_MASTER.wav\"") != std::string::npos);
    fsweb::put(B, "/USB/Recordings/take_004/x.wav", 0, 3, fsweb::crc32(0, (const uint8_t*)"abc", 3), (const uint8_t*)"abc", 3, &q);
    CHECK(q.status == 403 && access(mac("/USB/Recordings/take_004/x.wav").c_str(), F_OK) != 0);
    fsweb::mkdir(B, "/USB/Recordings/new", &q);
    CHECK(q.status == 403);
    fsweb::mkdirs(B, "/USB/Recordings/a/b", &q);
    CHECK(q.status == 403);
    fsweb::rename(B, "/USB/Recordings/take_004", "/USB/Recordings/take_005", &q);
    CHECK(q.status == 403);
    fsweb::rename(B, "/Recordings", "/USB/Recordings/x", &q);
    CHECK(q.status == 403);
    fsweb::remove(B, "/USB/Recordings/take_004/00_MASTER.wav", &q);
    CHECK(q.status == 200);
    fsweb::remove(B, "/USB/Recordings/take_004", &q);
    CHECK(q.status == 200 && access(mac("/USB/Recordings/take_004").c_str(), F_OK) != 0);
    g_lib.clear();
    printf("test_fsweb: a drive's recordings (/USB/Recordings): list, delete; writes refused; nothing else reachable: OK\n");
    // the take being recorded (on the SD or a drive): never deleted or renamed, nor the folders above it
    for (const char* live : {"/Recordings/take_009", "/USB/Recordings/take_009"}) {
      std::string d = live;
      const char* id = d[1] == 'R' ? "take_009" : "USB:take_009";
      mkdir(mac(d.c_str()).c_str(), 0755);
      FILE* lf = fopen(mac((d + "/00_MASTER.wav").c_str()).c_str(), "wb");
      fputs("LIVE", lf), fclose(lf);
      g_live = id;
      fsweb::remove(B, (d + "/00_MASTER.wav").c_str(), &q);
      CHECK(q.status == 409 && strstr(q.msg, "recording"));
      fsweb::remove(B, live, &q);
      CHECK(q.status == 409);
      fsweb::remove(B, d.substr(0, d.rfind('/')).c_str(), &q);   // its /Recordings
      CHECK(q.status == 409);
      if (d[1] == 'R') {
        fsweb::rename(B, live, "/Recordings/other", &q);
        CHECK(q.status == 409);
        fsweb::rename(B, (d + "/00_MASTER.wav").c_str(), "/Samples/x.wav", &q);
        CHECK(q.status == 409);
      }
      CHECK(slurp(mac((d + "/00_MASTER.wav").c_str())).size() == 4);
      g_live = nullptr;   // stopped: it can go
      fsweb::remove(B, (d + "/00_MASTER.wav").c_str(), &q);
      CHECK(q.status == 200);
      fsweb::remove(B, live, &q);
      CHECK(q.status == 200);
    }
    g_lib.clear();
    // fsweb::live_take: the folder, in it, above it, the volume's marker; not other takes, not another volume's
    CHECK(fsweb::live_take("/Recordings/take_009", "take_009") && fsweb::live_take("/recordings/TAKE_009/x.wav", "take_009"));
    CHECK(fsweb::live_take("/Recordings", "take_009") && fsweb::live_take("/Recordings/.recording", "take_009"));
    CHECK(!fsweb::live_take("/Recordings/take_0090", "take_009") && !fsweb::live_take("/Recordings/take_008", "take_009"));
    CHECK(!fsweb::live_take("/USB/Recordings/take_009", "take_009") && !fsweb::live_take("/USB/Recordings/.recording", "take_009"));
    CHECK(fsweb::live_take("/USB/Recordings/take_009/00_MASTER.wav", "USB:take_009") && fsweb::live_take("/USB/Recordings/.recording", "USB:take_009"));
    CHECK(fsweb::live_take("/USB", "USB:take_009") && !fsweb::live_take("/Recordings/.recording", "USB:take_009"));
    CHECK(!fsweb::live_take("/Samples/x.wav", "take_009") && !fsweb::live_take("/Recordings/take_009", ""));
    // the marker while it records: not deleted (the take's crash recovery)
    mkdir(mac("/Recordings").c_str(), 0755);
    FILE* mk = fopen(mac("/Recordings/.recording").c_str(), "wb");
    fputs("take_009", mk), fclose(mk);
    g_live = "take_009";
    fsweb::remove(B, "/Recordings/.recording", &q);
    CHECK(q.status == 409 && access(mac("/Recordings/.recording").c_str(), F_OK) == 0);
    // an upload over the live take's WAV (the last chunk would replace it: an unlink under the recorder): refused at
    // every chunk, the file untouched
    mkdir(mac("/Recordings/take_009").c_str(), 0755);
    FILE* lw = fopen(mac("/Recordings/take_009/00_MASTER.wav").c_str(), "wb");
    fputs("LIVE", lw), fclose(lw);
    {
      std::vector<uint8_t> up(100000, 7);
      uint32_t crc = fsweb::crc32(0, up.data(), (uint32_t)up.size());
      fsweb::put(B, "/Recordings/take_009/00_MASTER.wav", 0, up.size(), crc, up.data(), 50000, &q);
      CHECK(q.status == 409);
      fsweb::put(B, "/Recordings/take_009/00_MASTER.wav", 50000, up.size(), crc, up.data() + 50000, 50000, &q);
      CHECK(q.status == 409);
      fsweb::put(B, "/Recordings/take_009/new.txt", 0, 3, fsweb::crc32(0, up.data(), 3), up.data(), 3, &q);
      CHECK(q.status == 409);   // (anything in the take's folder while it records)
      CHECK(slurp(mac("/Recordings/take_009/00_MASTER.wav")).size() == 4);
      CHECK(access(mac("/Recordings/take_009/00_MASTER.wav.part").c_str(), F_OK) != 0);
    }
    g_live = nullptr;
    fsweb::remove(B, "/Recordings/take_009/00_MASTER.wav", &q);
    fsweb::remove(B, "/Recordings/take_009", &q);
    unlink(mac("/Recordings/.recording").c_str());
    g_lib.clear();
    printf("test_fsweb: the take being recorded is never deleted or renamed: OK\n");
  }

  // 20 MB in 48 KB chunks
  std::vector<uint8_t> big(20u << 20);
  uint32_t x = 1;
  for (auto& c : big) { x = x * 1103515245u + 12345u; c = (uint8_t)(x >> 16); }
  double worst = 0;
  auto t0 = std::chrono::steady_clock::now();
  r = upload("/Samples/big.bin", big, 48 * 1024, &worst);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r.status == 200 && !strncmp(r.msg, "done", 4));
  CHECK(slurp(mac("/Samples/big.bin")) == big);
  CHECK(access(mac("/Samples/big.bin.part").c_str(), 0) != 0);
  printf("20 MB upload: %u chunks of 48 KB, %.2f s on the Mac, longest chunk %.0f us, %d yields (4 KB pieces)\n",
         (unsigned)((big.size() + 49151) / 49152), secs, worst, g_yields);
  CHECK(g_yields >= (int)(big.size() / 4096));

  // resume: a chunk out of place gets "expected <offset>"
  std::vector<uint8_t> mid(200000, 7);
  uint32_t crc = fsweb::crc32(0, mid.data(), (uint32_t)mid.size());
  fsweb::put(B, "/Samples/mid.bin", 0, mid.size(), crc, mid.data(), 49152, &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "ok 49152"));
  fsweb::put(B, "/Samples/mid.bin", 98304, mid.size(), crc, mid.data() + 98304, 49152, &r);
  CHECK(r.status == 409 && !strcmp(r.msg, "expected 49152"));
  for (uint64_t off = 49152; off < mid.size(); off += 49152) {
    uint32_t n = (uint32_t)std::min<uint64_t>(49152, mid.size() - off);
    fsweb::put(B, "/Samples/mid.bin", off, mid.size(), crc, mid.data() + off, n, &r);
    CHECK(r.status == 200);
  }
  CHECK(slurp(mac("/Samples/mid.bin")) == mid);
  {   // the library got each finished upload with the sha256 of its bytes (made from the chunks: a resume included)
    uint8_t h[32];
    char hx[65];
    core::lib::sha256(mid.data(), mid.size(), h);
    core::lib::hex(h, 32, hx);
    CHECK(g_lib.size() == 2 && g_lib[1].op == 's' && g_lib[1].a == "/Samples/mid.bin" && g_lib[1].sha == hx && g_lib[1].crc == crc);
    core::lib::sha256(big.data(), big.size(), h);
    core::lib::hex(h, 32, hx);
    CHECK(g_lib[0].a == "/Samples/big.bin" && g_lib[0].sha == hx);
    fsweb::rename(B, "/Samples/mid.bin", "/Samples/mid2.bin", &r);
    CHECK(r.status == 200 && g_lib.back().op == 'r' && g_lib.back().a == "/Samples/mid.bin" && g_lib.back().b == "/Samples/mid2.bin");
    fsweb::rename(B, "/Samples/mid2.bin", "/Samples/mid.bin", &r);
    CHECK(r.status == 200);
  }

  // abandoned uploads (a page that stopped mid-file): when the slots run out, the reused slot's .part is removed
  for (int i = 0; i < 5; i++) {
    char p[32];
    snprintf(p, sizeof p, "/Samples/ab%d.bin", i);
    fsweb::put(B, p, 0, mid.size(), crc, mid.data(), 49152, &r);
    CHECK(r.status == 200);
  }
  CHECK(access(mac("/Samples/ab0.bin.part").c_str(), 0) != 0);
  CHECK(access(mac("/Samples/ab4.bin.part").c_str(), 0) == 0);
  for (int i = 1; i < 5; i++) {
    char p[40];
    snprintf(p, sizeof p, "/Samples/ab%d.bin.part", i);
    fsweb::remove(B, p, &r);   // the page deletes a failed file's .part
    CHECK(r.status == 200);
  }

  // a wrong CRC: nothing replaced
  std::vector<uint8_t> v2(5000, 9);
  r = upload("/Samples/mid.bin", v2, 48 * 1024, nullptr, 1);
  CHECK(r.status == 400 && strstr(r.msg, "CRC"));
  CHECK(slurp(mac("/Samples/mid.bin")) == mid);
  // a full card
  g_free = 1000;
  r = upload("/Samples/nope.bin", v2);
  CHECK(r.status == 507);
  g_free = 1ull << 40;
  // no such folder
  r = upload("/NoFolder/x.bin", v2);
  CHECK(r.status == 404);

  // packages: junk, a damaged one, a Teensy one, a good one
  r = upload("/Plugins/junk.rmp", v2);
  CHECK(r.status == 422 && strstr(r.msg, "not a machine package"));
  CHECK(access(mac("/Plugins/junk.rmp").c_str(), 0) != 0);
  const char* pkg = getenv("FSWEB_PKG") ? getenv("FSWEB_PKG") : "../../../plugins/sine_test/sine_test.a64.rmp";
  std::vector<uint8_t> good = slurp(pkg);
  if (good.empty()) {
    printf("SKIP the package checks: no %s (make -C plugins/sine_test a64)\n", pkg);
  } else {
    std::vector<uint8_t> bad = good;
    bad[bad.size() / 2] ^= 0x40;
    r = upload("/Plugins/bad.rmp", bad);
    CHECK(r.status == 422 && strstr(r.msg, "CRC wrong"));
    std::vector<uint8_t> arm = good;
    memcpy(arm.data(), "RMP1", 4);
    r = upload("/Plugins/arm.rmp", arm);
    CHECK(r.status == 422 && strstr(r.msg, "Teensy"));
    g_changed.clear();
    r = upload("/Plugins/sine.rmp", good);
    CHECK(r.status == 200);
    CHECK(g_changed.size() == 1 && g_changed[0] == "/Plugins/sine.rmp");
    CHECK(fsweb::check_package(B, "/Plugins/sine.rmp", good.size()) == nullptr);
    fsweb::rename(B, "/Samples/mid.bin", "/Plugins/mid.rmp", &r);
    CHECK(r.status == 422);
  }

  // each entry's date and time (FatFs's fdate / ftime: "YYYY-MM-DD HH:MM", "" when none was written)
  {
    char t[20];
    fsweb::fat_time((uint16_t)((2026 - 1980) << 9 | 10 << 5 | 27), (uint16_t)(14 << 11 | 32 << 5 | 29), t);
    CHECK(!strcmp(t, "2026-10-27 14:32"));
    fsweb::fat_time((uint16_t)(0 << 9 | 1 << 5 | 1), 0, t);
    CHECK(!strcmp(t, "1980-01-01 00:00"));
    fsweb::fat_time(0, 0, t);
    CHECK(!strcmp(t, ""));
    fsweb::fat_time((uint16_t)(45 << 9 | 13 << 5 | 1), 0, t);   // month 13: not a date
    CHECK(!strcmp(t, ""));
    static char lj[65536];
    CHECK(fsweb::list_json(B, "/", lj, sizeof lj, &r) > 0 && strstr(lj, "\"n\":\"Samples\",\"d\":1,\"s\":") && strstr(lj, "\"t\":\"\""));
    g_fdate = (uint16_t)((2026 - 1980) << 9 | 10 << 5 | 27), g_ftime = (uint16_t)(9 << 11 | 5 << 5);
    CHECK(fsweb::list_json(B, "/", lj, sizeof lj, &r) > 0 && strstr(lj, "\"t\":\"2026-10-27 09:05\"") && !strstr(lj, "\"t\":\"\""));
    g_fdate = g_ftime = 0;
  }

  // list, mkdir, rename, delete
  static char js[65536];
  int n = fsweb::list_json(B, "/", js, sizeof js, &r);
  // the root is the content folders now: config.txt is on the card but not a file this API has (reachable())
  CHECK(n > 0 && !strstr(js, "\"n\":\"config.txt\"") && strstr(js, "\"n\":\"Samples\",\"d\":1"));
  fsweb::mkdir(B, "/Samples/kit", &r);
  CHECK(r.status == 200);
  fsweb::mkdir(B, "/Samples/kit", &r);
  CHECK(r.status == 409);
  fsweb::rename(B, "/Samples/mid.bin", "/Samples/kit/mid.bin", &r);
  CHECK(r.status == 200 && slurp(mac("/Samples/kit/mid.bin")) == mid);
  fsweb::remove(B, "/Samples/kit", &r);
  CHECK(r.status == 409);   // not empty
  fsweb::remove(B, "/Samples/kit/mid.bin", &r);
  CHECK(r.status == 200 && g_lib.back().op == 'd' && g_lib.back().a == "/Samples/kit/mid.bin");
  fsweb::remove(B, "/Samples/kit", &r);
  CHECK(r.status == 200);
  // the Sidecar's own folders: kept (no delete, no rename), their contents free; config.txt.bak locked, .prev not
  mkdir(mac("/.rytm").c_str(), 0755);
  b_write("/config.txt.bak", 0, "x", 1, true);
  b_write("/config.txt.prev", 0, "y", 1, true);
  CHECK(fsweb::read_only("/config.txt.bak") && fsweb::read_only("/CONFIG.TXT.BAK") && !fsweb::read_only("/config.txt.prev"));
  for (const char* k : {"/.rytm", "/Samples", "/Plugins", "/plugins", "/Bundles", "/bundles"}) {
    CHECK(fsweb::kept_dir(k));
    // the content ones are reachable and kept (403); /.rytm is not reachable at all, so it is 404 before that
    int want = fsweb::reachable(k) ? 403 : 404;
    fsweb::remove(B, k, &r);
    CHECK(r.status == want);
    fsweb::rename(B, k, "/Moved", &r);
    CHECK(r.status == 404);				// "/Moved" is outside the content folders either way
    fsweb::rename(B, k, "/Samples/Moved", &r);
    CHECK(r.status == want);
  }
  CHECK(!fsweb::kept_dir("/Samples/kit") && !fsweb::kept_dir("/Samples2") && !fsweb::kept_dir("/x/Samples"));
  CHECK(!fsweb::kept_dir("/Bundles/a.sidecar.zip"));   // a bundle itself can go
  // the boot files are not reachable here at all now (the root is content only), so these are 404, not 403
  fsweb::remove(B, "/config.txt.bak", &r);
  CHECK(r.status == 404);
  fsweb::rename(B, "/config.txt.bak", "/c.bak", &r);
  CHECK(r.status == 404);
  fsweb::rename(B, "/config.txt.prev", "/config.txt.bak", &r);
  CHECK(r.status == 404);
  fsweb::put(B, "/config.txt.bak", 0, 1, fsweb::crc32(0, (const uint8_t*)"z", 1), (const uint8_t*)"z", 1, &r);
  CHECK(r.status == 404 && slurp(mac("/config.txt.bak"))[0] == 'x');
  // a loose file at the root used to be renameable and deletable here; the root is content only now
  fsweb::rename(B, "/config.txt.prev", "/config.txt.old", &r);
  CHECK(r.status == 404);
  fsweb::remove(B, "/config.txt.prev", &r);
  CHECK(r.status == 404);
  n = fsweb::list_json(B, "/", js, sizeof js, &r);
  CHECK(strstr(js, "\"n\":\"Samples\",\"d\":1,\"s\":") && !strstr(js, "\"n\":\".rytm\""));
  CHECK(!strstr(js, "\"n\":\"config.txt.bak\""));
  {
    const char* q = strstr(js, "\"n\":\"Samples\"");
    CHECK(q && std::string(q, strchr(q, '}') - q).find("\"ro\":2") != std::string::npos);
  }
  b_write("/Samples/s.wav", 0, "w", 1, true);
  fsweb::rename(B, "/Samples/s.wav", "/Samples/t.wav", &r);
  CHECK(r.status == 200);
  fsweb::remove(B, "/Samples/t.wav", &r);
  CHECK(r.status == 200);

  // a dropped folder: mkdir -p of every folder, then each file put into it
  fsweb::mkdirs(B, "/Samples/Kit A/sub/deep", &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "made"));
  fsweb::mkdirs(B, "/Samples/Kit A/sub", &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "exists"));
  fsweb::mkdirs(B, "/Samples/Kit A/sub2", &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "made"));
  std::vector<uint8_t> f1(70000, 3), f2(10, 4);
  r = upload("/Samples/Kit A/sub/deep/one.wav", f1);
  CHECK(r.status == 200 && slurp(mac("/Samples/Kit A/sub/deep/one.wav")) == f1);
  r = upload("/Samples/Kit A/two.wav", f2);
  CHECK(r.status == 200 && slurp(mac("/Samples/Kit A/two.wav")) == f2);
  fsweb::mkdirs(B, "/Samples/Kit A/two.wav/x", &r);
  CHECK(r.status == 409);   // a file in the way
  fsweb::mkdirs(B, "/overlays/x", &r);
  CHECK(r.status == 404);   // a boot folder: not reachable here at all
  fsweb::mkdirs(B, "/a/../b", &r);
  CHECK(r.status == 400);
  fsweb::mkdirs(B, "/NewTop", &r);		// a new folder at the root: not content, so not this API's to make
  CHECK(r.status == 404);
  fsweb::mkdirs(B, "/Recordings/take 1", &r);
  {
    uint64_t sz;
    bool dir = false;
    CHECK(r.status == 200 && b_stat("/Recordings/take 1", &sz, &dir) && dir);
  }
  n = fsweb::list_json(B, "/nope", js, sizeof js, &r);
  CHECK(n == 0 && r.status == 404);
  system(("rm -rf " + g_root).c_str());
  printf("test_fsweb: OK\n");
  return 0;
}
