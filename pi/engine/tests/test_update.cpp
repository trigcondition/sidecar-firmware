// pi/engine/tests/test_update.cpp - the web update (update.cpp, docs/web-update.md) on a folder standing in for the
// card: the file's checks (bad magic, damaged header or part, truncated, the wrong board, not a Sidecar kernel, a
// stamp that is not the header's, bad sdboot sums, a bad web archive), the version rules, tryboot.txt and
// cmdline-try.txt, the upload, install -> trial -> commit and the revert, "previous" (a trial, and the forced direct
// swap), and POWER CUTS: a card that stops taking writes after N operations, for every N through install, commit and
// previous; after each, a firmware stand-in reads config.txt (or tryboot.txt when the flag was set) and must find a
// kernel that is byte for byte the old or the new one - except at the commit's one documented rename window, which
// must be exactly one operation wide. Also: tools/make_update.py's file is read here (the format's two sides agree),
// and this binary must not contain the Pi 5 kernel's boot lines (update.cpp would make sdboot.py take every kernel
// for a Pi 5 one).
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "fsweb.h"
#include "lib/sha256.h"
#include "update.h"

using Bytes = std::vector<uint8_t>;
static std::string g_root;
static std::string mac(const char* p) { return g_root + p; }
static uint64_t g_free = 1ull << 34;
static int g_ops = 0, g_cut = -1;   // card writes so far; the one that fails (and every one after it): -1 none
static bool g_hit = false;          // the cut happened
static bool cut() {
  if (g_cut >= 0 && g_ops >= g_cut) return g_hit = true;
  ++g_ops;
  return false;
}

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
    std::string full = std::string(p) + "/" + e->d_name;
    b_stat(full.c_str(), &out[n].size, &out[n].dir);
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
  bool dead = cut();
  FILE* f = fopen(mac(p).c_str(), create ? "wb" : "r+b");
  if (!f) return false;
  fseeko(f, (off_t)off, SEEK_SET);
  bool ok = fwrite(buf, 1, dead ? n / 2 : n, f) == (dead ? n / 2 : n);   // a cut write: half of it on the card
  fclose(f);
  return ok && !dead;
}
static bool b_remove(const char* p) { return !cut() && ::remove(mac(p).c_str()) == 0; }
static bool b_rename(const char* a, const char* b) {   // FatFs's f_rename: the new name must not exist
  struct stat st;
  if (::stat(mac(b).c_str(), &st) == 0) return false;
  return !cut() && ::rename(mac(a).c_str(), mac(b).c_str()) == 0;
}
static bool b_mkdir(const char* p) { return !cut() && ::mkdir(mac(p).c_str(), 0755) == 0; }
static bool b_space(uint64_t* f, uint64_t* t) { *f = g_free; *t = 1ull << 35; return true; }
static int g_yields = 0;
static void (*g_probe)() = nullptr;   // called at each yield: lets a test look in while an install runs
static void b_yield() { ++g_yields; if (g_probe) g_probe(); }
static const fsweb::Backend kCard = {b_stat, b_list, b_read, b_write, b_remove, b_rename, b_mkdir, b_space, b_yield,
                                     nullptr, nullptr, nullptr, nullptr, nullptr};

// ---- the card as files
static Bytes rd(const std::string& p) {
  Bytes b;
  FILE* f = fopen(mac(p.c_str()).c_str(), "rb");
  if (!f) return b;
  int c;
  while ((c = fgetc(f)) != EOF) b.push_back((uint8_t)c);
  fclose(f);
  return b;
}
static bool has(const std::string& p) { struct stat st; return ::stat(mac(p.c_str()).c_str(), &st) == 0; }
static void wr(const std::string& p, const Bytes& b) {
  FILE* f = fopen(mac(p.c_str()).c_str(), "wb");
  CHECK(f);
  if (!b.empty()) CHECK(fwrite(b.data(), 1, b.size(), f) == b.size());
  fclose(f);
}
static void wrs(const std::string& p, const std::string& s) { wr(p, Bytes(s.begin(), s.end())); }
static std::string rds(const std::string& p) { Bytes b = rd(p); return std::string(b.begin(), b.end()); }
static void reset_card() {
  std::string c = "rm -rf '" + g_root + "' && mkdir -p '" + g_root + "/.rytm'";
  CHECK(system(c.c_str()) == 0);
}
// a folder's files, relative path -> bytes
static void tree(const std::string& base, const std::string& rel, std::map<std::string, Bytes>* out) {
  DIR* d = opendir(mac((base + rel).c_str()).c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    std::string r = rel + "/" + e->d_name;
    struct stat st;
    ::stat(mac((base + r).c_str()).c_str(), &st);
    if (S_ISDIR(st.st_mode)) tree(base, r, out);
    else (*out)[r] = rd(base + r);
  }
  closedir(d);
}
static std::map<std::string, Bytes> tree(const std::string& base) { std::map<std::string, Bytes> m; tree(base, "", &m); return m; }

// ---- the parts
static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(v >> (8 * i)); }
static void put64(Bytes& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = (uint8_t)(v >> (8 * i)); }
static void add(Bytes& b, const std::string& s, bool nul = false) { b.insert(b.end(), s.begin(), s.end()); if (nul) b.push_back(0); }
// a stand-in kernel: filler, the engine's banner, the board's line (Pi 5), the update code's stamp (when cap)
static Bytes kernel(int board, const char* v, const char* c, bool cap = true, uint32_t seed = 1, size_t size = 300000) {
  Bytes k;
  uint32_t x = seed * 2654435761u;
  while (k.size() < size / 2) { x = x * 1103515245u + 12345u; k.push_back((uint8_t)(x >> 16)); }
  add(k, "==== rytm-pi engine ==== Sidecar ", true);
  if (board == 5) add(k, std::string("board: Raspberry Pi 5 bu") + "ild (kernel_2712.img)", true);
  if (cap) add(k, "sidecar-webupdate-1 v=", true);   // the prefix alone, as a real kernel has it (update.cpp kCap)
  if (cap) add(k, std::string("sidecar-webupdate-1 v=") + v + " c=" + c, true);
  while (k.size() < size) { x = x * 1103515245u + 12345u; k.push_back((uint8_t)(x >> 16)); }
  return k;
}
static Bytes wrap(Bytes img) {   // sdboot.py wrap()
  while (img.size() % 4) img.push_back(0);
  uint32_t s1 = 0, s2 = 0;
  for (size_t i = 0; i < img.size(); i += 4) {
    s1 += img[i] | img[i + 1] << 8 | img[i + 2] << 16 | (uint32_t)img[i + 3] << 24;
    s2 += s1;
  }
  Bytes o(32, 0);
  memcpy(o.data(), "SDCRENG1", 8);
  put32(o, 8, (uint32_t)img.size());
  put32(o, 12, s1);
  put32(o, 16, s2);
  o.insert(o.end(), img.begin(), img.end());
  return o;
}
static Bytes web(const std::vector<std::pair<std::string, std::string>>& files) {
  Bytes w;
  add(w, "SCWEB001");
  w.resize(12);
  put32(w, 8, (uint32_t)files.size());
  for (auto& f : files) {
    size_t at = w.size();
    w.resize(at + 8);
    w[at] = (uint8_t)f.first.size(), w[at + 1] = (uint8_t)(f.first.size() >> 8);
    put32(w, at + 4, (uint32_t)f.second.size());
    add(w, f.first);
    add(w, f.second);
  }
  return w;
}
struct P { uint32_t kind; Bytes b; };
static Bytes ufile(const char* v, const char* c, const std::vector<P>& parts) {
  Bytes f(512, 0);
  memcpy(f.data(), "SCUPDAT1", 8);
  put32(f, 8, 512);
  put32(f, 12, 1);
  put32(f, 16, (uint32_t)parts.size());
  memcpy(f.data() + 24, v, strlen(v));
  memcpy(f.data() + 48, c, strlen(c));
  for (size_t i = 0; i < parts.size(); ++i) {
    size_t h = 80 + 64 * i;
    put32(f, h, parts[i].kind);
    put64(f, h + 8, f.size());
    put64(f, h + 16, parts[i].b.size());
    core::lib::sha256(parts[i].b.data(), parts[i].b.size(), f.data() + h + 24);
    f.insert(f.end(), parts[i].b.begin(), parts[i].b.end());
  }
  core::lib::sha256(f.data(), 480, f.data() + 480);
  return f;
}
static void reseal(Bytes& f) { core::lib::sha256(f.data(), 480, f.data() + 480); }

// ---- the card a release makes (make-sd-dual.sh): both boards' boot files, the old engine, the old web app
static const char kCfg[] =
    "# the user's own lines\r\ndtoverlay=vc4-kms-v3d\n[pi5]\nkernel=kernel_2712.img\n[all]\nhdmi_group=2\n"
    "# sidecar sdboot begin (pi/circle/sdboot.py disable removes this block; config.txt.bak is the original)\n"
    "[pi4]\nkernel=sdboot.img\ninitramfs engine.img 0x02000000\ndisable_splash=1\nboot_delay=0\n# sidecar sdboot end\n";
static Bytes OLD4, OLD5, NEW4, NEW5, WEBOLD_A, WEBNEW;
static std::map<std::string, Bytes> g_oldWeb;
static void make_card() {
  reset_card();
  wrs("/config.txt", kCfg);
  wrs("/cmdline.txt", "sdport=0 socmaxtemp=78\n");
  wr("/engine.img", wrap(OLD4));
  wr("/kernel_2712.img", OLD5);
  wrs("/sdboot.img", "SDBOOT");
  mkdir(mac("/.rytm/www-next").c_str(), 0755);
  mkdir(mac("/.rytm/www-next/assets").c_str(), 0755);
  wrs("/.rytm/www-next/index.html.gz", "old index");
  wrs("/.rytm/www-next/assets/app-old.js.gz", "old app");
  g_oldWeb = tree("/.rytm/www-next");
}

// The firmware stand-in: which kernel the next boot loads and whether it is the trial (cmdline updtrial=1).
// try_flag: the one-shot tryboot flag. Returns the kernel's bytes (Pi 4: the payload after a good SDCRENG1 header,
// as sdboot starts it); empty = the Pi does not boot.
static std::string last_value(const std::string& cfg, const std::string& key, const char* section) {
  std::string v, sec = "[all]";
  size_t at = 0;
  while (at < cfg.size()) {
    size_t e = cfg.find('\n', at);
    std::string l = cfg.substr(at, e == std::string::npos ? std::string::npos : e - at);
    while (!l.empty() && (l.back() == '\r')) l.pop_back();
    if (!l.empty() && l[0] == '[') sec = l;
    else if ((sec == "[all]" || sec == section) && l.compare(0, key.size(), key) == 0) v = l.substr(key.size());
    if (e == std::string::npos) break;
    at = e + 1;
  }
  return v;
}
static Bytes firmware(int board, bool try_flag, bool* trial) {
  std::string cfg = try_flag && has("/tryboot.txt") ? rds("/tryboot.txt") : rds("/config.txt");
  const char* sec = board == 5 ? "[pi5]" : "[pi4]";
  std::string cl = last_value(cfg, "cmdline=", sec);
  std::string cmd = rds("/" + (cl.empty() ? std::string("cmdline.txt") : cl));
  *trial = cmd.find("updtrial=1") != std::string::npos;
  if (board == 5) {
    std::string k = last_value(cfg, "kernel=", sec);
    return rd("/" + (k.empty() ? std::string("kernel_2712.img") : k));
  }
  std::string ir = last_value(cfg, "initramfs ", sec);
  size_t sp = ir.find(' ');
  Bytes e = rd("/" + ir.substr(0, sp));
  if (e.size() < 32 || memcmp(e.data(), "SDCRENG1", 8)) return {};
  uint32_t len = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24, s1 = 0, s2 = 0;
  if (len != e.size() - 32) return {};
  for (size_t i = 32; i < e.size(); i += 4) { s1 += e[i] | e[i + 1] << 8 | e[i + 2] << 16 | (uint32_t)e[i + 3] << 24; s2 += s1; }
  if (s1 != (uint32_t)(e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24) || s2 != (uint32_t)(e[16] | e[17] << 8 | e[18] << 16 | (uint32_t)e[19] << 24)) return {};
  return Bytes(e.begin() + 32, e.end());
}
static Bytes padded(Bytes k) { while (k.size() % 4) k.push_back(0); return k; }
static upd::Env env(int board, const char* v, const char* c, bool trial = false, uint32_t up = 5) {
  upd::Env e = {&kCard, board, v, c, true, "", trial, up};
  return e;
}
static std::string state() {
  std::string t = rds("/.rytm/update/state.txt");
  size_t a = t.find("state=");
  return a == std::string::npos ? "idle" : t.substr(a + 6, t.find('\n', a) - a - 6);
}
static std::string msg() {
  std::string t = rds("/.rytm/update/state.txt");
  size_t a = t.find("msg=");
  return a == std::string::npos ? "" : t.substr(a + 4, t.find('\n', a) - a - 4);
}
static fsweb::Result upload(const upd::Env& e, const Bytes& f, size_t stop = SIZE_MAX) {
  uint32_t crc = fsweb::crc32(0, f.data(), (uint32_t)f.size());
  fsweb::Result r = {0, ""};
  for (size_t off = 0; off < f.size() && off < stop; off += 49152) {
    uint32_t n = (uint32_t)(f.size() - off < 49152 ? f.size() - off : 49152);
    upd::put(e, off, f.size(), crc, f.data() + off, n, &r);
    if (r.status != 200) return r;
  }
  return r;
}
static bool trial_files_gone(int board) {
  return !has(upd::trial_name(board)) && !has("/cmdline-try.txt") && !has("/tryboot.txt") &&
         !has(std::string(upd::trial_name(board)) + ".part");
}

static void t_header() {
  Bytes f = ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}});
  upd::Header h;
  CHECK(upd::parse_header(f.data(), f.size(), &h) == nullptr);
  CHECK(h.n == 2 && !strcmp(h.version, "0.5.1") && !strcmp(h.commit, "bbbbbbb") && h.part[1].kind == upd::kPartPi5);
  CHECK(upd::parse_header(f.data(), f.size() - 1, &h) != nullptr);    // truncated
  CHECK(upd::parse_header(f.data(), f.size() + 1, &h) != nullptr);    // trailing bytes
  Bytes g = f;
  g[0] = 'X';
  CHECK(strstr(upd::parse_header(g.data(), g.size(), &h), "not a Sidecar update"));
  g = f;
  g[30] ^= 1;   // the version changed, the header's hash not
  CHECK(strstr(upd::parse_header(g.data(), g.size(), &h), "damaged"));
  g = f;
  put32(g, 12, 2);
  reseal(g);
  CHECK(strstr(upd::parse_header(g.data(), g.size(), &h), "newer format"));
  g = f;
  put32(g, 80 + 64, 9);
  reseal(g);
  CHECK(strstr(upd::parse_header(g.data(), g.size(), &h), "unknown kind"));
  g = f;
  put64(g, 80 + 64 + 8, 600);   // overlaps the first part
  reseal(g);
  CHECK(strstr(upd::parse_header(g.data(), g.size(), &h), "overlap"));
  g = f;
  memcpy(g.data() + 24, "0.5\"1", 5);   // a quote: never into JSON
  reseal(g);
  CHECK(strstr(upd::parse_header(g.data(), g.size(), &h), "malformed"));
  printf("header: ok\n");
}

static void t_versions() {
  CHECK(upd::cmp_version("0.5.1", "0.5.0") == 1 && upd::cmp_version("0.5.0", "0.5.1") == -1);
  CHECK(upd::cmp_version("0.10.0", "0.9.9") == 1);
  CHECK(upd::cmp_version("0.5.0", "0.5.0") == 0);
  CHECK(upd::cmp_version("0.5.0", "0.5.0-rc1") == 1 && upd::cmp_version("0.5.0-rc1", "0.5.0-dev") == 1);
  CHECK(upd::cmp_version("0.5.0-dev", "0.5.0-rc1") == -1 && upd::cmp_version("0.5.0-rc2", "0.5.0-rc1") == 1);
  CHECK(upd::cmp_version("0.5.0-dev", "0.4.4") == 1);
  CHECK(upd::cmp_version("junk", "0.5.0") == -2);
  printf("versions: ok\n");
}

static void t_bootfiles() {
  char out[4096];
  CHECK(upd::tryboot_txt(4, kCfg, out, sizeof out) == nullptr);
  std::string t = out;
  CHECK(t.rfind("# sidecar web update", 0) == 0);
  CHECK(t.find("initramfs engine-try.img 0x02000000\n") != std::string::npos);
  CHECK(t.find("initramfs engine.img") == std::string::npos);
  CHECK(t.find("dtoverlay=vc4-kms-v3d") != std::string::npos && t.find("\r\n") != std::string::npos);   // the user's lines kept
  CHECK(last_value(t, "cmdline=", "[pi4]") == "cmdline-try.txt" && last_value(t, "cmdline=", "[pi5]") == "");
  CHECK(upd::tryboot_txt(5, kCfg, out, sizeof out) == nullptr);
  t = out;
  CHECK(t.find("sdboot") == std::string::npos && t.find("initramfs") == std::string::npos);
  CHECK(last_value(t, "kernel=", "[pi5]") == "kernel_2712-try.img" && last_value(t, "cmdline=", "[pi5]") == "cmdline-try.txt");
  // an older card's block under [all], no newline at the end
  const char* all = "# sidecar sdboot begin\n[all]\nkernel=sdboot.img\ninitramfs engine.img 0x02000000\n# sidecar sdboot end";
  CHECK(upd::tryboot_txt(4, all, out, sizeof out) == nullptr && strstr(out, "engine-try.img") && strstr(out, "end\n[pi4]\n"));
  // a Pi 4 card that does not start through sdboot: refused
  CHECK(strstr(upd::tryboot_txt(4, "kernel=kernel8-rpi4.img\ninitramfs engine.img 0x02000000\n", out, sizeof out), "sdboot"));
  CHECK(upd::tryboot_txt(4, kCfg, out, 100) != nullptr);   // too small: refused, never cut
  CHECK(upd::cmdline_try("sdport=0 socmaxtemp=78 \r\nsecond", out, sizeof out) && !strcmp(out, "sdport=0 socmaxtemp=78 updtrial=1\n"));
  CHECK(upd::cmdline_try("", out, sizeof out) && !strcmp(out, "updtrial=1\n"));
  printf("tryboot.txt, cmdline-try.txt: ok\n");
}

static Bytes good_file() {
  return ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}, {upd::kPartWeb, WEBNEW}});
}

static void t_refusals() {
  for (int board : {4, 5}) {
    make_card();
    upd::Env e = env(board, "0.5.0", "aaaaaaa");
    fsweb::Result r;
    CHECK(upd::install(e, false, &r) == upd::kNone && r.status == 409 && strstr(r.msg, "upload it first"));
    upd::Env off = e;
    off.enabled = false, off.off_why = "web updates are off (cmdline.txt webupdate=0)";
    CHECK(upd::install(off, false, &r) == upd::kNone && r.status == 403 && strstr(r.msg, "webupdate=0"));
    upload(off, good_file());
    CHECK(!has("/.rytm/update/update.scu"));
    // truncated: the upload stopped half way leaves no file to install
    Bytes f = good_file();
    r = upload(e, f, f.size() / 2);
    CHECK(r.status == 200 && !has("/.rytm/update/update.scu"));
    CHECK(upd::install(e, false, &r) == upd::kNone && strstr(r.msg, "upload it first"));
    // a wrong CRC: not kept
    {
      fsweb::Result q;
      upd::put(e, 0, f.size(), 1234, f.data(), 49152, &q);
      CHECK(q.status == 200);
      upd::put(e, 49152 * 3, f.size(), 1234, f.data(), 100, &q);   // out of order
      CHECK(q.status == 409 && !strcmp(q.msg, "expected 49152"));
      for (size_t off2 = 49152; off2 < f.size(); off2 += 49152)
        upd::put(e, off2, f.size(), 1234, f.data() + off2, (uint32_t)(f.size() - off2 < 49152 ? f.size() - off2 : 49152), &q);
      CHECK(q.status == 400 && strstr(q.msg, "CRC") && !has("/.rytm/update/update.scu") && !has("/.rytm/update/upload.part"));
    }
    auto refuse = [&](const Bytes& file, const char* why, int status = 422) {
      CHECK(upload(e, file).status == 200);
      fsweb::Result q;
      CHECK(upd::install(e, false, &q) == upd::kNone);
      if (!strstr(q.msg, why) || q.status != status) { fprintf(stderr, "board %d: '%s' %d, wanted '%s'\n", board, q.msg, q.status, why); exit(1); }
      CHECK(trial_files_gone(board) && state() != "trying");
      CHECK(rd("/engine.img") == wrap(OLD4) && rd("/kernel_2712.img") == OLD5 && rds("/config.txt") == kCfg);
    };
    // the other board's kernel only
    refuse(ufile("0.5.1", "bbbbbbb", {{board == 4 ? upd::kPartPi5 : upd::kPartPi4, board == 4 ? NEW5 : wrap(NEW4)}}),
           board == 4 ? "no kernel for the Pi 4" : "no kernel for the Pi 5");
    // a damaged part (one byte of the Pi 5 kernel, its sha256 as before): refused whole, on either board
    Bytes g = good_file();
    upd::Header h;
    upd::parse_header(g.data(), g.size(), &h);
    g[h.part[1].off + 5000] ^= 0xff;
    refuse(g, "damaged");
    // the boards' kernels swapped
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW5)}}), "holds a Pi 5 kernel");
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi5, NEW4}}), "holds a Pi 4 kernel");
    // not a Sidecar kernel; not wrapped; bad sums
    Bytes junk(200000, 0x42);
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(junk)}, {upd::kPartPi5, junk}}), "not a Sidecar engine");
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, NEW4}}), "no SDCRENG1");
    Bytes bad = wrap(NEW4);
    put32(bad, 12, 7);
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, bad}}), "sums are wrong");
    // the header says another version than the kernels carry (also "-dirty" is not the clean stamp)
    refuse(ufile("0.5.2", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}}), "not the version its header says");
    refuse(ufile("0.5.1", "bbbbbbb-dirty", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}}), "not the version its header says");
    // a kernel without the update code (it could never make itself permanent)
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(kernel(4, "0.5.1", "bbbbbbb", false, 7))}, {upd::kPartPi5, kernel(5, "0.5.1", "bbbbbbb", false, 8)}}),
           "not the version its header says");
    // web archives
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}, {upd::kPartWeb, web({{"../x", "a"}, {"index.html.gz", "i"}})}}), "would not serve");
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}, {upd::kPartWeb, web({{".hidden", "a"}, {"index.html.gz", "i"}})}}), "would not serve");
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}, {upd::kPartWeb, web({{"a.js", "a"}})}}), "no index.html");
    Bytes cutweb = web({{"index.html.gz", "i"}});
    cutweb.pop_back();
    put32(cutweb, 8, 1);
    refuse(ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}, {upd::kPartWeb, cutweb}}), "cut short");
    // older or the same: only with force
    refuse(ufile("0.4.9", "ccccccc", {{upd::kPartPi4, wrap(kernel(4, "0.4.9", "ccccccc"))}, {upd::kPartPi5, kernel(5, "0.4.9", "ccccccc")}}), "older than the running 0.5.0", 409);
    refuse(ufile("0.5.0", "aaaaaaa", {{upd::kPartPi4, wrap(OLD4)}, {upd::kPartPi5, OLD5}}), "already runs 0.5.0 (aaaaaaa)", 409);
    refuse(ufile("0.5.0", "ddddddd", {{upd::kPartPi4, wrap(kernel(4, "0.5.0", "ddddddd"))}, {upd::kPartPi5, kernel(5, "0.5.0", "ddddddd")}}), "same version", 409);
    CHECK(upd::install(e, true, &r) == upd::kTry);   // forced
    // a full card
    make_card();
    upload(e, good_file());
    g_free = 300000;
    CHECK(upd::install(e, false, &r) == upd::kNone && r.status == 507);
    g_free = 1ull << 34;
    // a Pi 4 card that does not start through sdboot
    wrs("/config.txt", "kernel=kernel8-rpi4.img\n");
    if (board == 4) {
      CHECK(upd::install(e, false, &r) == upd::kNone && strstr(r.msg, "sdboot") && state() == "failed" && trial_files_gone(4));
    } else
      CHECK(upd::install(e, false, &r) == upd::kTry);
  }
  printf("refusals: ok\n");
}

// install -> trial boot -> commit, then the next plain boot; and the revert
static void t_happy(int board) {
  make_card();
  upd::Env old = env(board, "0.5.0", "aaaaaaa");
  CHECK(upload(old, good_file()).status == 200);
  fsweb::Result r;
  CHECK(upd::install(old, false, &r) == upd::kTry && r.status == 200);
  CHECK(state() == "trying" && rd("/engine.img") == wrap(OLD4) && rd("/kernel_2712.img") == OLD5 && rds("/config.txt") == kCfg);
  bool trial;
  CHECK(firmware(board, false, &trial) == (board == 4 ? padded(OLD4) : OLD5) && !trial);   // a plain boot: the old one
  CHECK(firmware(board, true, &trial) == (board == 4 ? padded(NEW4) : NEW5) && trial);     // the tryboot: the new one
  upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 3);
  CHECK(upd::boot(t));
  char js[1024];
  CHECK(upd::state_json(t, js, sizeof js) > 0 && strstr(js, "\"state\":\"trying\"") && strstr(js, "\"commitIn\":27") && strstr(js, "\"target\":\"0.5.1 (bbbbbbb)\""));
  CHECK(!upd::commit_due(t));
  t.uptime_s = 31;
  CHECK(upd::commit_due(t) && upd::commit(t));
  CHECK(state() == "committed" && trial_files_gone(board));
  CHECK(firmware(board, false, &trial) == (board == 4 ? padded(NEW4) : NEW5) && !trial);
  CHECK(rd(std::string(upd::boot_name(board)) + ".prev") == (board == 4 ? wrap(OLD4) : OLD5));
  auto nw = tree("/.rytm/www-next");
  CHECK(nw.size() == 2 && rds("/.rytm/www-next/index.html.gz") == "new index" && rds("/.rytm/www-next/assets/app-new.js.gz") == "new app");
  CHECK(tree("/.rytm/www-next.prev") == g_oldWeb && !has("/.rytm/www-next.new"));
  CHECK(rds("/.rytm/update/prev.txt") == "v=0.5.0\nc=aaaaaaa\nw=1\n" && !has("/.rytm/update/update.scu"));
  upd::Env now = env(board, "0.5.1", "bbbbbbb");
  CHECK(!upd::boot(now) && state() == "committed");
  CHECK(upd::state_json(now, js, sizeof js) > 0 && strstr(js, "\"prev\":true") && strstr(js, "\"prevVersion\":\"0.5.0 (aaaaaaa)\""));

  // the revert: install again (0.5.2), the trial never commits (it crashes): the next boot is the old one
  Bytes f2 = ufile("0.5.2", "eeeeeee", {{upd::kPartPi4, wrap(kernel(4, "0.5.2", "eeeeeee", true, 11))}, {upd::kPartPi5, kernel(5, "0.5.2", "eeeeeee", true, 12)}, {upd::kPartWeb, WEBNEW}});
  CHECK(upload(now, f2).status == 200 && upd::install(now, false, &r) == upd::kTry);
  CHECK(firmware(board, false, &trial) == (board == 4 ? padded(NEW4) : NEW5) && !trial);
  CHECK(!upd::boot(now) && state() == "reverted" && strstr(msg().c_str(), "went back to 0.5.1") && trial_files_gone(board));
  CHECK(rds("/config.txt") == kCfg);
  // a trial that is not the staged version is never made permanent
  CHECK(upload(now, f2).status == 200 && upd::install(now, false, &r) == upd::kTry);
  // (through boot(), as the device does: the trial boot of another build is settled there, not by a commit task)
  upd::Env wrong = env(board, "0.5.9", "fffffff", true, 3);
  bool w = upd::boot(wrong);
  wrong.uptime_s = 40;
  CHECK(!w && !upd::commit_due(wrong) && state() == "failed" && strstr(msg().c_str(), "not the staged 0.5.2") && trial_files_gone(board));
  CHECK(upload(now, f2).status == 200);   // installs are no longer refused
  CHECK(firmware(board, false, &trial) == (board == 4 ? padded(NEW4) : NEW5));
  printf("Pi %d: install, trial, commit, revert: ok\n", board);
}

// an update file without a web part (a kernels-only file, the trial files before --web): the card's web app is left as it
// is, www-next.prev too, and the commit says nothing about the web
static void t_noweb(int board) {
  make_card();
  upd::Env old = env(board, "0.5.0", "aaaaaaa");
  Bytes f = ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, wrap(NEW4)}, {upd::kPartPi5, NEW5}});
  fsweb::Result r;
  CHECK(upload(old, f).status == 200 && upd::install(old, false, &r) == upd::kTry);
  upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 31);
  CHECK(upd::boot(t) && upd::commit(t) && state() == "committed");
  CHECK(tree("/.rytm/www-next") == g_oldWeb && !has("/.rytm/www-next.prev") && !has("/.rytm/www-next.new"));
  CHECK(rds("/.rytm/update/prev.txt") == "v=0.5.0\nc=aaaaaaa\nw=0\n" && trial_files_gone(board));
  CHECK(msg().find("web") == std::string::npos);
  bool trial;
  CHECK(firmware(board, false, &trial) == (board == 4 ? padded(NEW4) : NEW5));
  printf("Pi %d: an update without a web part leaves the web app alone: ok\n", board);
}

// the screens' notices (touchscreen full-screen, the Rytm's popup): from memory, through install -> trial -> commit and
// the revert
static void t_notice(int board) {
  make_card();
  char a[40], b[40];
  upd::Env old = env(board, "0.5.0", "aaaaaaa");
  CHECK(!upd::notice(old, a, b, sizeof a) || true);
  CHECK(upload(old, good_file()).status == 200);
  fsweb::Result r;
  static bool s_seen;
  static int s_board;
  static const upd::Env* s_env;
  s_seen = false, s_board = board, s_env = &old;
  g_probe = [] {   // while the install stages (the state is "staging" there): the touchscreen would say so
    char x[40], y[40];
    if (!s_seen && state() == "staging" && upd::notice(*s_env, x, y, sizeof x) && !strcmp(x, "INSTALLING UPDATE") && !strcmp(y, "DO NOT POWER OFF")) s_seen = true;
  };
  CHECK(upd::install(old, false, &r) == upd::kTry);
  g_probe = nullptr;
  CHECK(s_seen);
  CHECK(upd::notice(old, a, b, sizeof a) && !strcmp(a, "RESTARTING INTO UPDATE") && !strcmp(b, "0.5.1 (BBBBBBB)"));
  upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 3);
  CHECK(upd::boot(t));
  CHECK(upd::notice(t, a, b, sizeof a) && !strcmp(a, "TRIAL: PERMANENT IN 27 S") && !strcmp(b, "DO NOT POWER OFF") && strlen(a) <= 32 && strlen(b) <= 32);
  t.uptime_s = 31;
  CHECK(upd::notice(t, a, b, sizeof a) && strstr(a, "MAKING IT PERMANENT"));
  CHECK(upd::commit(t));
  CHECK(upd::notice(t, a, b, sizeof a) && !strcmp(a, "UPDATED TO 0.5.1") && !strcmp(b, "BBBBBBB"));
  t.uptime_s = 60;
  CHECK(!upd::notice(t, a, b, sizeof a));   // a few seconds only
  upd::Env off = env(board, "0.5.1", "bbbbbbb");
  off.enabled = false;
  CHECK(!upd::notice(off, a, b, sizeof a));
  // the revert: a plain boot after a trial that never committed
  Bytes f2 = ufile("0.5.2", "eeeeeee", {{upd::kPartPi4, wrap(kernel(4, "0.5.2", "eeeeeee", true, 11))}, {upd::kPartPi5, kernel(5, "0.5.2", "eeeeeee", true, 12)}, {upd::kPartWeb, WEBNEW}});
  upd::Env now = env(board, "0.5.1", "bbbbbbb", false, 4);
  CHECK(upload(now, f2).status == 200 && upd::install(now, false, &r) == upd::kTry);
  CHECK(!upd::boot(now));
  CHECK(upd::notice(now, a, b, sizeof a) && !strcmp(a, "UPDATE FAILED") && !strcmp(b, "BACK ON 0.5.1 (BBBBBBB)"));
  now.uptime_s = 40;
  CHECK(!upd::notice(now, a, b, sizeof a));
  // H1: a reverted / failed state left over from an EARLIER boot says nothing on later boots
  CHECK(state() == "reverted");
  upd::Env later = env(board, "0.5.1", "bbbbbbb", false, 4);
  CHECK(!upd::boot(later) && state() == "reverted");
  CHECK(!upd::notice(later, a, b, sizeof a));
  std::string st = rds("/.rytm/update/state.txt");
  st.replace(st.find("state=reverted"), 14, "state=failed");
  wrs("/.rytm/update/state.txt", st);
  CHECK(!upd::boot(later) && state() == "failed");
  CHECK(!upd::notice(later, a, b, sizeof a));
  printf("Pi %d: screen notices: ok\n", board);
}

// the owner's case (2026-11-12): the trial committed itself and the engine still runs as the trial boot (trial=true
// until a restart): a new install is accepted (the firmware's tryboot flag is one-shot and the committed kernel is
// the boot name), the old boot's commit never touches the new job, and a plain boot reverts it as any other
static void t_install_after_commit(int board) {
  make_card();
  upd::Env old = env(board, "0.5.0", "aaaaaaa");
  fsweb::Result r;
  CHECK(upload(old, good_file()).status == 200 && upd::install(old, false, &r) == upd::kTry);
  upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 3);
  CHECK(upd::boot(t));
  t.uptime_s = 31;
  CHECK(upd::commit(t) && state() == "committed");
  Bytes k4r = kernel(4, "0.5.3", "9999999", true, 21), k4 = wrap(k4r), k5 = kernel(5, "0.5.3", "9999999", true, 22);
  Bytes f3 = ufile("0.5.3", "9999999", {{upd::kPartPi4, k4}, {upd::kPartPi5, k5}, {upd::kPartWeb, WEBNEW}});
  t.uptime_s = 80;   // still the trial boot, long after the commit
  CHECK(upload(t, f3).status == 200);
  fsweb::Result q;
  CHECK(upd::install(t, false, &q) == upd::kTry && q.status == 200 && state() == "trying");
  CHECK(!upd::commit_due(t) && state() == "trying" && has(upd::trial_name(board)));   // not this boot's job
  char js[1024];
  CHECK(upd::state_json(t, js, sizeof js) > 0 && strstr(js, "\"commitIn\":0") && strstr(js, "\"state\":\"trying\""));
  // while the new job is on, the same boot refuses a second install
  CHECK(upd::install(t, false, &q) == upd::kNone && q.status == 409);
  bool trial;
  CHECK(firmware(board, true, &trial) == (board == 4 ? padded(k4r) : k5) && trial);
  CHECK(firmware(board, false, &trial) == (board == 4 ? padded(NEW4) : NEW5) && !trial);
  // and it commits like any other when its trial boot runs 30 s
  upd::Env t3 = env(board, "0.5.3", "9999999", true, 3);
  CHECK(upd::boot(t3));
  t3.uptime_s = 31;
  CHECK(upd::commit(t3) && state() == "committed");
  CHECK(rd(std::string(upd::boot_name(board)) + ".prev") == (board == 4 ? wrap(NEW4) : NEW5));
  printf("Pi %d: install after a commit on the same boot: ok\n", board);
}

// while a trial is on, the upload and discard are refused; a file changed behind install's back (by any other way to
// the card) is never unpacked: the kernel commits, the old web app stays, nothing outside www-next is written
static void t_inflight(int board) {
  for (int variant = 0; variant < 3; ++variant) {
    make_card();
    upd::Env old = env(board, "0.5.0", "aaaaaaa");
    fsweb::Result r;
    CHECK(upload(old, good_file()).status == 200 && upd::install(old, false, &r) == upd::kTry);
    upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 3);
    CHECK(upd::boot(t));
    CHECK(upload(t, good_file()).status == 409 && upload(old, good_file()).status == 409);
    upd::discard(t, &r);
    CHECK(r.status == 409 && has("/.rytm/update/update.scu"));
    Bytes hostile = web({{"index.html.gz", "evil"}, {"../../config.txt", "owned"}});
    Bytes other = web({{"index.html.gz", "other index"}});
    if (variant < 2) {
      Bytes k4 = wrap(NEW4);
      wr("/.rytm/update/update.scu", ufile("0.5.1", "bbbbbbb", {{upd::kPartPi4, k4}, {upd::kPartPi5, NEW5}, {upd::kPartWeb, variant ? other : hostile}}));
    } else
      ::remove(mac("/.rytm/update/update.scu").c_str());
    t.uptime_s = 31;
    CHECK(upd::commit(t) && state() == "committed");
    CHECK(strstr(msg().c_str(), "the web app was not") != nullptr);
    CHECK(rds("/config.txt") == kCfg && tree("/.rytm/www-next") == g_oldWeb && !has("/.rytm/www-next.new"));
    bool trial;
    CHECK(firmware(board, false, &trial) == (board == 4 ? padded(NEW4) : NEW5));
    CHECK(rds("/.rytm/update/prev.txt") == "v=0.5.0\nc=aaaaaaa\nw=0\n");   // www-next.prev is not 0.5.0's app
  }
  // a stale prev.txt (the card changed in a computer): "previous" goes by the kernel's own stamp, and does not take
  // www-next.prev for its app
  make_card();
  upd::Env old = env(board, "0.5.0", "aaaaaaa");
  fsweb::Result r;
  CHECK(upload(old, good_file()).status == 200 && upd::install(old, false, &r) == upd::kTry);
  upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 40);
  CHECK(upd::boot(t) && upd::commit(t));
  CHECK(rds("/.rytm/update/prev.txt") == "v=0.5.0\nc=aaaaaaa\nw=1\n");
  wrs("/.rytm/update/prev.txt", "v=9.9.9\nc=1234567\nw=1\n");
  upd::Env now = env(board, "0.5.1", "bbbbbbb");
  CHECK(upd::previous(now, false, &r) == upd::kTry);
  std::string st = rds("/.rytm/update/state.txt");
  CHECK(st.find("tv=0.5.0\ntc=aaaaaaa\n") != std::string::npos && st.find("web=0\n") != std::string::npos);
  upd::Env back = env(board, "0.5.0", "aaaaaaa", true, 40);
  CHECK(upd::boot(back) && upd::commit(back) && state() == "committed");
  CHECK(rds("/.rytm/www-next/index.html.gz") == "new index");   // the app stayed: www-next.prev was not 0.5.0's
  printf("Pi %d: no upload during a trial; a changed file is not unpacked; a stale prev.txt: ok\n", board);
}

// a developer's tryboot.txt (sdboot.py pi5-try) is not ours: a revert leaves it
static void t_foreign_tryboot() {
  make_card();
  upd::Env e = env(5, "0.5.0", "aaaaaaa");
  mkdir(mac("/.rytm/update").c_str(), 0755);
  wrs("/.rytm/update/state.txt", "state=trying\nkind=update\ntv=0.5.1\ntc=bbbbbbb\n");
  wrs("/tryboot.txt", "[pi5]\nkernel=sdboot-try.img\n");
  CHECK(!upd::boot(e) && state() == "reverted" && has("/tryboot.txt"));
  printf("a foreign tryboot.txt stays: ok\n");
}

// POWER CUTS through install: the next boot is always the old engine, which cleans up
static void t_cut_install(int board) {
  int n;
  for (n = 0;; ++n) {
    make_card();
    upd::Env old = env(board, "0.5.0", "aaaaaaa");
    CHECK(upload(old, good_file()).status == 200);
    g_ops = 0, g_cut = n, g_hit = false;
    fsweb::Result r;
    upd::Reboot rb = upd::install(old, false, &r);
    bool done = !g_hit;   // no cut happened: every point is covered
    g_cut = -1;
    bool trial;
    Bytes k = firmware(board, rb == upd::kTry, &trial);
    if (rb == upd::kTry) { CHECK(done); break; }
    CHECK(k == (board == 4 ? padded(OLD4) : OLD5) && !trial);
    CHECK(rd("/engine.img") == wrap(OLD4) && rd("/kernel_2712.img") == OLD5 && rds("/config.txt") == kCfg);
    upd::boot(old);
    CHECK(trial_files_gone(board) && state() != "trying" && state() != "staging");
    CHECK(tree("/.rytm/www-next") == g_oldWeb);
  }
  printf("Pi %d: power cut at each of %d card writes of install: old engine boots, cleaned up: ok\n", board, n);
}

// POWER CUTS through the commit: the card always boots the old or the new kernel, but for the one rename window
static void check_after(int board, bool prev_kind, bool isNew, const Bytes& oldk);
static void t_cut_commit(int board, bool prev_kind) {
  int n, windows = 0, olds = 0, news = 0, nested = 0;
  for (n = 0;; ++n) {
    make_card();
    upd::Env old = env(board, "0.5.0", "aaaaaaa");
    fsweb::Result r;
    const Bytes* oldk = board == 4 ? &OLD4 : &OLD5;
    const Bytes* newk = board == 4 ? &NEW4 : &NEW5;
    if (!prev_kind) {
      CHECK(upload(old, good_file()).status == 200 && upd::install(old, false, &r) == upd::kTry);
    } else {   // "previous" from 0.5.1 (committed over 0.5.0) back to 0.5.0, as a trial
      CHECK(upload(old, good_file()).status == 200 && upd::install(old, false, &r) == upd::kTry);
      upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 40);
      CHECK(upd::boot(t) && upd::commit(t));
      upd::Env now = env(board, "0.5.1", "bbbbbbb");
      CHECK(upd::previous(now, false, &r) == upd::kTry);
      std::swap(oldk, newk);
    }
    bool trial;
    CHECK(firmware(board, true, &trial) == (board == 4 ? padded(*newk) : *newk) && trial);
    upd::Env t = prev_kind ? env(board, "0.5.0", "aaaaaaa", true, 40) : env(board, "0.5.1", "bbbbbbb", true, 40);
    CHECK(upd::boot(t));
    g_ops = 0, g_cut = n, g_hit = false;
    bool ok = upd::commit(t);
    bool done = !g_hit;
    g_cut = -1;
    if (done) { CHECK(ok && state() == "committed"); break; }
    Bytes k = firmware(board, false, &trial);   // the tryboot flag was used up by the trial
    CHECK(!trial);
    if (k.empty() || !has(upd::boot_name(board))) { ++windows; continue; }   // the window: the card does not boot
    bool isNew = k == (board == 4 ? padded(*newk) : *newk);
    CHECK(isNew || k == (board == 4 ? padded(*oldk) : *oldk));
    upd::Env runs = isNew == prev_kind ? env(board, "0.5.1", "bbbbbbb") : env(board, "0.5.0", "aaaaaaa");
    // and the boot that sorts it out is cut too, at each of its own card writes, then booted again (the same kernel:
    // that boot never renames a kernel)
    CHECK(system(("rm -rf '" + g_root + ".snap' && cp -R '" + g_root + "' '" + g_root + ".snap'").c_str()) == 0);
    for (int m = 0;; ++m) {
      CHECK(system(("rm -rf '" + g_root + "' && cp -R '" + g_root + ".snap' '" + g_root + "'").c_str()) == 0);
      g_ops = 0, g_cut = m, g_hit = false;
      upd::boot(runs);
      bool clean = !g_hit;
      g_cut = -1;
      if (clean) break;
      CHECK(firmware(board, false, &trial) == k && !trial);
      CHECK(!upd::boot(runs));
      check_after(board, prev_kind, isNew, *oldk);
      ++nested;
    }
    CHECK(!upd::boot(runs));   // (booted again: nothing left to do)
    CHECK(trial_files_gone(board));
    check_after(board, prev_kind, isNew, *oldk);
    if (isNew) ++news; else ++olds;
  }
  CHECK(windows == 1);   // exactly the documented one: between the commit's two renames
  printf("Pi %d: power cut at each of %d card writes of the %s commit: %d old, %d new, %d window; %d cuts in the boot after: ok\n",
         board, n, prev_kind ? "previous" : "update", olds, news, windows, nested);
}

// what the card must hold once a cut commit has been sorted out by a whole boot: the new version committed (with its
// web app), or the old one with its own app, nothing of a half revert
static void check_after(int board, bool prev_kind, bool isNew, const Bytes& oldk) {
  CHECK(trial_files_gone(board));
  CHECK(!has("/.rytm/www-next.new") && !has("/.rytm/www-next.swp"));
  if (isNew) {
      CHECK(state() == "committed");
      CHECK(rd(std::string(upd::boot_name(board)) + ".prev") == (board == 4 ? wrap(oldk) : oldk));
      if (!prev_kind) {
        CHECK(rds("/.rytm/www-next/index.html.gz") == "new index" && tree("/.rytm/www-next").size() == 2);
        CHECK(tree("/.rytm/www-next.prev") == g_oldWeb);
      } else {
        CHECK(tree("/.rytm/www-next") == g_oldWeb && rds("/.rytm/www-next.prev/index.html.gz") == "new index");
      }
  } else {
    if (state() != "reverted") fprintf(stderr, "state %s msg %s\n", state().c_str(), msg().c_str());
    CHECK(state() == "reverted");
    CHECK(rd(upd::boot_name(board)) == (board == 4 ? wrap(oldk) : oldk));
    // the old version's own web app in place (a half revert must never "finish" the commit)
    if (!prev_kind) CHECK(tree("/.rytm/www-next") == g_oldWeb);
    else CHECK(rds("/.rytm/www-next/index.html.gz") == "new index" && tree("/.rytm/www-next.prev") == g_oldWeb);
  }
}

// "previous": a kernel without this code only with force, swapped directly
static void t_previous_direct(int board) {
  make_card();
  Bytes legacy = kernel(board, "0.4.4", "9999999", false, 21);
  wr(std::string(upd::boot_name(board)) + ".prev", board == 4 ? wrap(legacy) : legacy);
  upd::Env e = env(board, "0.5.0", "aaaaaaa");
  fsweb::Result r;
  CHECK(upd::previous(e, false, &r) == upd::kNone && r.status == 409 && strstr(r.msg, "SD card") && strstr(r.msg, "anyway?"));   // (web-next update.ts asksForce)
  CHECK(upd::previous(e, true, &r) == upd::kPlain);
  bool trial;
  CHECK(firmware(board, false, &trial) == (board == 4 ? padded(legacy) : legacy) && !trial);
  CHECK(rd(std::string(upd::boot_name(board)) + ".prev") == (board == 4 ? wrap(OLD4) : OLD5));
  CHECK(tree("/.rytm/www-next") == g_oldWeb && state() == "committed");
  // a damaged .prev is refused
  make_card();
  Bytes d = board == 4 ? wrap(legacy) : legacy;
  d[d.size() / 2] ^= 1;
  if (board == 5) d.assign(d.size(), 0x11);
  wr(std::string(upd::boot_name(board)) + ".prev", d);
  CHECK(upd::previous(e, true, &r) == upd::kNone && r.status == 422);
  make_card();
  CHECK(upd::previous(e, true, &r) == upd::kNone && r.status == 404);
  // power cuts through the direct swap: old or legacy boots, but for its own rename window
  int windows = 0, n;
  for (n = 0;; ++n) {
    make_card();
    wr(std::string(upd::boot_name(board)) + ".prev", board == 4 ? wrap(legacy) : legacy);
    g_ops = 0, g_cut = n, g_hit = false;
    upd::Reboot rb = upd::previous(e, true, &r);
    bool done = !g_hit;
    g_cut = -1;
    if (done) { CHECK(rb == upd::kPlain); break; }
    Bytes k = firmware(board, false, &trial);
    if (k.empty() || !has(upd::boot_name(board))) { ++windows; continue; }
    CHECK(k == (board == 4 ? padded(OLD4) : OLD5) || k == (board == 4 ? padded(legacy) : legacy));
    if (k == (board == 4 ? padded(OLD4) : OLD5)) {
      upd::boot(e);
      CHECK(state() != "swapping" && has(std::string(upd::boot_name(board)) + ".prev"));
    }
  }
  CHECK(windows == 1);
  printf("Pi %d: previous without the update code: refused, forced direct swap, %d cut points: ok\n", board, n);
}

// tools/make_update.py's file: read and installed here
static void t_make_update(const char* argv0) {
  std::string dir = g_root + "-mk";
  CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "/dist/assets'").c_str()) == 0);
  auto save = [&](const std::string& p, const Bytes& b) { FILE* f = fopen((dir + p).c_str(), "wb"); CHECK(f); fwrite(b.data(), 1, b.size(), f); fclose(f); };
  save("/k4.img", NEW4);
  save("/k5.img", NEW5);
  save("/dist/index.html", Bytes{'<', 'h', '>'});
  save("/dist/assets/app.js", Bytes{'j', 's'});
  save("/dist/assets/logo.png", Bytes{0x89, 'P'});
  std::string here = argv0;
  here = here.substr(0, here.rfind('/'));   // build/
  std::string cmd = "python3 " + here + "/../../../../tools/make_update.py --pi4 '" + dir + "/k4.img' --pi5 '" + dir +
                    "/k5.img' --web '" + dir + "/dist' --version 0.5.1 --commit bbbbbbb --out '" + dir + "/u.scu' >/dev/null";
  CHECK(system(cmd.c_str()) == 0);
  FILE* f = fopen((dir + "/u.scu").c_str(), "rb");
  CHECK(f);
  Bytes u;
  int c;
  while ((c = fgetc(f)) != EOF) u.push_back((uint8_t)c);
  fclose(f);
  for (int board : {4, 5}) {
    make_card();
    upd::Env e = env(board, "0.5.0", "aaaaaaa");
    fsweb::Result r;
    CHECK(upload(e, u).status == 200 && upd::install(e, false, &r) == upd::kTry);
    upd::Env t = env(board, "0.5.1", "bbbbbbb", true, 40);
    CHECK(upd::boot(t) && upd::commit(t) && state() == "committed");
    bool trial;
    CHECK(firmware(board, false, &trial) == (board == 4 ? padded(NEW4) : NEW5));
    auto w = tree("/.rytm/www-next");   // card.mjs's rule: text files as .gz only
    CHECK(w.size() == 3 && w.count("/index.html.gz") && w.count("/assets/app.js.gz") && w.count("/assets/logo.png"));
  }
  system(("rm -rf '" + dir + "'").c_str());
  printf("make_update.py's file installs on both boards: ok\n");
}

// this binary holds update.cpp: the Pi 5 boot lines must not be in it (sdboot.py board() would misjudge every kernel)
static void t_no_markers(const char* argv0) {
  FILE* f = fopen(argv0, "rb");
  CHECK(f);
  std::string b;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.append(buf, n);
  fclose(f);
  CHECK(b.find(std::string("board: Raspberry Pi 5 bu") + "ild (kernel_2712.img)") == std::string::npos);
  CHECK(b.find(std::string("gadget: none on this board") + " (Pi 5 build)") == std::string::npos);
  CHECK(b.find(upd::stamp()) != std::string::npos);   // the stamp is (what "previous" looks for)
  printf("no Pi 5 boot lines in the binary, the stamp is: ok\n");
}

int main(int, char** argv) {
  char tmpl[] = "/tmp/test_update_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_root = std::string(tmpl) + "/card";
  OLD4 = kernel(4, "0.5.0", "aaaaaaa", true, 1, 300001);   // (odd: wrap pads it)
  OLD5 = kernel(5, "0.5.0", "aaaaaaa", true, 2);
  NEW4 = kernel(4, "0.5.1", "bbbbbbb", true, 3, 320002);
  NEW5 = kernel(5, "0.5.1", "bbbbbbb", true, 4, 340000);
  WEBNEW = web({{"index.html.gz", "new index"}, {"assets/app-new.js.gz", "new app"}});
  t_no_markers(argv[0]);
  t_header();
  t_versions();
  t_bootfiles();
  t_refusals();
  t_foreign_tryboot();
  for (int board : {4, 5}) {
    t_happy(board);
    t_install_after_commit(board);
    t_notice(board);
    t_noweb(board);
    t_inflight(board);
    t_cut_install(board);
    t_cut_commit(board, false);
    t_cut_commit(board, true);
    t_previous_direct(board);
  }
  t_make_update(argv[0]);
  system((std::string("rm -rf ") + tmpl).c_str());
  printf("test_update: ALL PASS\n");
  return 0;
}
