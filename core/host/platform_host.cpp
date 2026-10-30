// core/host/platform_host.cpp - the Mac's platform functions and files (core/host/core_port.h).
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "platform.h"

namespace core {

static std::string g_card = "/tmp/core-card";
static void (*g_sink)(const char*, int) = nullptr;

void host_set_card(const char* dir) { g_card = dir; }
void host_set_log_sink(void (*sink)(const char*, int)) { g_sink = sink; }
// removable volumes ("USB:" -> a folder; "" pulled): a path "USB:/x" is that folder's x
static constexpr int kHostVols = 6;
static std::string g_vol_name[kHostVols], g_vol_dir[kHostVols];
static int vol_len(const char* p) {   // "USB:/x" -> 4 (the drive with its colon), 0 a card path
  int i = 0;
  while (i < 6 && ((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) ++i;
  return i > 0 && p[i] == ':' ? i + 1 : 0;
}
static int vol_of(const char* p, int n) {
  for (int i = 0; i < kHostVols; ++i)
    if (g_vol_name[i].size() == (size_t)n && !strncmp(g_vol_name[i].c_str(), p, n)) return i;
  return -1;
}
void host_set_volume(const char* drive, const char* dir) {
  int k = vol_of(drive, (int)strlen(drive));
  for (int i = 0; k < 0 && i < kHostVols; ++i)
    if (g_vol_name[i].empty()) k = i, g_vol_name[i] = drive;
  if (k >= 0) g_vol_dir[k] = dir ? dir : "";
}
static std::string root_of(const char* p) {   // the Mac folder a path's volume is ("" a pulled one)
  int n = vol_len(p);
  if (!n) return g_card;
  int k = vol_of(p, n);
  return k < 0 ? std::string() : g_vol_dir[k];
}
std::string host_path(const char* p) {
  int n = vol_len(p);
  if (n) {
    std::string r = root_of(p);
    if (r.empty()) return "/nonexistent-volume/" + std::string(p + n);   // pulled: nothing there
    const char* rest = p + n;
    return r + (rest[0] == '/' ? "" : "/") + rest;
  }
  return g_card + (p[0] == '/' ? "" : "/") + p;
}
static bool vol_gone(const std::string& p) { return vol_len(p.c_str()) && root_of(p.c_str()).empty(); }

static uint64_t ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
// tests and the emulator link: a clock set from outside (host_fake_time), else the monotonic clock
static bool g_fake = false;
static uint64_t g_fake_us = 0;
void host_fake_time(bool on, uint64_t us) { g_fake = on, g_fake_us = us; }
uint32_t now_us() { return g_fake ? (uint32_t)g_fake_us : (uint32_t)(ns() / 1000); }
uint32_t now_ms() { return g_fake ? (uint32_t)(g_fake_us / 1000) : (uint32_t)(ns() / 1000000); }
uint32_t cycles() { return (uint32_t)ns(); }
uint32_t cycles_per_second() { return 1000000000u; }
uint32_t random_seed() { return 1; }

void log(const char* fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 199) n = 199;
  if (g_sink) g_sink(b, n);
  else fprintf(stderr, "core: %.*s\n", n, b);
}

uint32_t sample_ram_total() {
  const char* e = getenv("CORE_SAMPLE_RAM_MB");   // tests: 0 or unset = none (as a Teensy without PSRAM)
  return e ? (uint32_t)atoi(e) << 20 : 0;
}
void* sample_ram_alloc(uint32_t bytes) { return malloc(bytes); }
void sample_ram_free(void* p) { free(p); }
#if CORE_MSAMP
uint64_t msamp_ram_total() {
  const char* e = getenv("CORE_MSAMP_RAM_MB");
  return (uint64_t)(e ? atoi(e) : 64) << 20;
}
void* msamp_ram_alloc(uint64_t bytes) { return malloc(bytes); }
uint64_t pool_ram_budget() {
  const char* e = getenv("CORE_POOL_RAM_MB");
  return (uint64_t)(e ? atoi(e) : 64) << 20;
}
uint64_t card_free_bytes() {
  struct statvfs s;
  if (statvfs(g_card.c_str(), &s) != 0) return 0;
  return (uint64_t)s.f_bavail * s.f_frsize;
}
#endif

// ---- the card model (tests): the Pi's card time per file operation [inferred from measured figures: a 32 KB read
// ~1.5-2 ms and an open + header ~3 ms (docs/MULTISAMPLE.md), DSP SAMPLE's refill 1.2 ms a pass (docs/pi-engine.md);
// a sector write 2 ms is a typical SD small-write latency, its worst case (a card's own pauses, 100-137 ms measured
// inside the recorder's writes) is not modeled]. A write is FatFs's: its data sectors, the FAT and the directory entry
// go to the card at the close (f_sync); an open for writing truncates (a FAT write).
namespace {
bool g_cm = false;
int g_cm_core = 3;
uint64_t g_cm_us[4];
constexpr uint32_t kCmOpen = 1000, kCmSector = 2000, kCmReadCall = 100, kCmReadBps = 20000000, kCmDirEntry = 100,
                   kCmMkdirSeen = 500;
void cm(uint64_t us) {
  if (g_cm) g_cm_us[g_cm_core & 3] += us;
}
}  // namespace
void host_card_model(bool on) {
  g_cm = on, g_cm_core = 3;
  memset(g_cm_us, 0, sizeof g_cm_us);
}
void host_card_core(int c) { g_cm_core = c & 3; }
static int64_t g_cut = -1;
static uint64_t g_changes = 0;
void host_power_cut(int64_t after) { g_cut = after; }
uint64_t host_changes() { return g_changes; }
static bool change() {   // one more change to the card: false once the power is "cut"
  if (g_cut == 0) return false;
  if (g_cut > 0) --g_cut;
  ++g_changes;
  return true;
}
uint64_t host_card_us(int c) { return g_cm_us[c & 3]; }

// ---- files
static constexpr int kReopen = 4;   // a copy of a kReadWrite handle: read and write, without truncating

File& File::operator=(const File& o) {
  if (this == &o) return *this;
  close();
  if (o.isOpen()) {
    open_full(o.path_, o.mode_ == kRead ? kRead : o.mode_ == kReadWrite ? kReopen : kAppend, o.index_);
    seekSet(o.pos_);
    next_ = o.next_;
  }
  return *this;
}

bool File::open_full(const std::string& p, int mode, uint32_t index) {
  close();
  cm(kCmOpen + (mode == kWrite || mode == kReadWrite ? kCmSector : 0));   // the directory walk (+ the truncate)
  std::string m = host_path(p.c_str());
  struct stat st;
  bool there = stat(m.c_str(), &st) == 0;
  if (there && S_ISDIR(st.st_mode)) {
    if (mode != kRead) return false;
    DIR* d = opendir(m.c_str());
    if (!d) return false;
    names_.clear();
    while (dirent* e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names_.push_back(e->d_name);
    closedir(d);
    std::sort(names_.begin(), names_.end());
    dir_ = true;
  } else {
    if ((mode == kRead || mode == kReopen) && !there) return false;
    if (mode != kRead && mode != kReopen && !change()) return false;
    static const char* const how[] = {"rb", "wb", "ab", "w+b", "r+b"};
    fp_ = fopen(m.c_str(), how[mode]);
    if (!fp_) return false;
    if (mode == kAppend) fseek(fp_, 0, SEEK_END);
    size_ = mode == kWrite || mode == kReadWrite ? 0 : (uint64_t)(there ? st.st_size : 0);
    pos_ = mode == kAppend ? size_ : 0;
  }
  path_ = p;
  mode_ = mode == kReopen ? kReadWrite : mode;
  index_ = index;
  next_ = 0;
  return true;
}

static std::string g_fail_open;
void host_fail_open(const char* p, bool on) { g_fail_open = on && p ? p : ""; }
static std::string g_fail_rd, g_fail_lk;
static int g_fail_rd_after = 0;
void host_fail_readdir(const char* p, int after, bool on) { g_fail_rd = on && p ? p : "", g_fail_rd_after = after; }
void host_fail_lookup(const char* p, bool on) { g_fail_lk = on && p ? p : ""; }
bool File::open(const char* path, int mode) {
  if (!g_fail_open.empty() && path && g_fail_open == path) return false;
  return open_full(path, mode, 0);
}

bool File::open(File* dir, uint32_t index, int mode) {
  if (!dir || !dir->dir_ || index >= dir->names_.size()) return false;
  std::string p = dir->path_ + (dir->path_ == "/" ? "" : "/") + dir->names_[index];
  return open_full(p, mode, index);
}

bool File::openNext(File* dir, int mode) {
  if (!dir || !dir->dir_) return false;
  while (dir->next_ < dir->names_.size()) {
    uint32_t i = dir->next_++;
    cm(kCmDirEntry);
    if (open(dir, i, mode)) return true;
  }
  close();
  return false;
}

static void fat_dt(time_t t0, uint16_t* date, uint16_t* time) {
  struct tm t;
  localtime_r(&t0, &t);
  *date = (uint16_t)((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
  *time = (uint16_t)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
}

bool File::nextEntry(char* name, size_t n, bool* dir, uint64_t* size, uint16_t* date, uint16_t* time) {
  rd_err_ = false;
  if (!dir_ || !n) return false;
  if (!g_fail_rd.empty() && path_ == g_fail_rd && next_ >= (uint32_t)g_fail_rd_after && next_ < names_.size())
    return rd_err_ = true, false;   // host_fail_readdir: a card error in the middle of the folder
  while (next_ < names_.size()) {
    const std::string& nm = names_[next_++];
    cm(kCmDirEntry);
    struct stat st;
    std::string full = host_path((path_ + (path_ == "/" ? "" : "/") + nm).c_str());
    if (stat(full.c_str(), &st) != 0) continue;
    size_t k = nm.size() < n - 1 ? nm.size() : n - 1;
    memcpy(name, nm.data(), k);
    name[k] = 0;
    *dir = S_ISDIR(st.st_mode);
    *size = *dir ? 0 : (uint64_t)st.st_size;
    fat_dt(st.st_mtime, date, time);
    return true;
  }
  return false;
}

static std::string g_fail_close;
void host_fail_close(const char* p, bool on) { g_fail_close = on && p ? p : ""; }
bool File::close() {
  bool was = isOpen();
  if (was && !g_fail_close.empty() && path_ == g_fail_close) was = false;   // host_fail_close: its last sync failed
  if (fp_) fclose(fp_);
  if (was && !dir_ && mode_ != kRead) cm((wrote_ + 511) / 512 * kCmSector + 2 * kCmSector);   // f_sync: data, FAT, entry
  fp_ = nullptr;
  dir_ = false;
  names_.clear();
  size_ = pos_ = wrote_ = 0;
  rsec_ = ~0ull;
  return was;
}

bool File::getModifyDateTime(uint16_t* date, uint16_t* time) const {
  struct stat st;
  *date = *time = 0;
  if (!isOpen() || stat(host_path(path_.c_str()).c_str(), &st) != 0) return false;
  struct tm t;
  localtime_r(&st.st_mtime, &t);
  *date = (uint16_t)((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
  *time = (uint16_t)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
  return true;
}

size_t File::getName(char* out, size_t n) const {
  size_t s = path_.rfind('/');
  std::string base = s == std::string::npos ? path_ : path_.substr(s + 1);
  size_t k = base.size() < n - 1 ? base.size() : n - 1;
  memcpy(out, base.data(), k);
  out[k] = 0;
  return k;
}

int File::read(void* buf, size_t n) {
  if (!fp_ || (mode_ != kRead && mode_ != kReadWrite)) return -1;
  if (vol_gone(path_)) return -1;   // its stick pulled
  if (g_cm && n) {   // FatFs: the sectors past the one it holds, from the card
    uint64_t s0 = pos_ / 512, s1 = (pos_ + n - 1) / 512, k = s1 - s0 + 1 - (s0 == rsec_ ? 1 : 0);
    if (k) cm(kCmReadCall + k * 512 * 1000000 / kCmReadBps);
    rsec_ = s1;
  }
  fseek(fp_, (long)pos_, SEEK_SET);
  size_t got = fread(buf, 1, n, fp_);
  pos_ += got;
  return (int)got;
}

int File::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}

size_t File::write(const void* buf, size_t n) {
  if (!fp_ || mode_ == kRead) return 0;
  if (vol_gone(path_)) return 0;
  if (!change()) return 0;
  if (mode_ == kAppend) pos_ = size_;   // as O_APPEND: every write at the end
  fseek(fp_, (long)pos_, SEEK_SET);
  size_t w = fwrite(buf, 1, n, fp_);
  fflush(fp_);
  wrote_ += w;
  pos_ += w;
  if (pos_ > size_) size_ = pos_;
  return w;
}

bool File::seekSet(uint64_t pos) {
  if (dir_) return pos == 0;
  if (!fp_ || pos > size_) return false;
  pos_ = pos;
  return true;
}

// ---- the card
bool Fs::begin() {
  struct stat st;
  return stat(g_card.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool Fs::mkdir(const char* p) {   // with its parents, as SdFat's mkdir (pFlag true)
  std::string full = host_path(p), root = root_of(p);
  if (root.empty()) return false;
  for (size_t i = root.size() + 1; i <= full.size(); ++i)
    if (i == full.size() || full[i] == '/') {
      std::string part = full.substr(0, i);
      struct stat st;
      bool seen = stat(part.c_str(), &st) == 0;
      cm(seen ? kCmMkdirSeen : 3 * kCmSector);
      if (!seen && (!change() || ::mkdir(part.c_str(), 0755) != 0)) return false;
    }
  return true;
}
bool Fs::rename(const char* a, const char* b) { cm(2 * kCmSector); if (!change()) return false; return ::rename(host_path(a).c_str(), host_path(b).c_str()) == 0; }
bool Fs::remove(const char* p) { cm(2 * kCmSector); if (!change()) return false; return ::remove(host_path(p).c_str()) == 0; }
bool Fs::exists(const char* p) {
  if (!g_fail_lk.empty() && p && g_fail_lk == p) return false;
  struct stat st;
  return stat(host_path(p).c_str(), &st) == 0;
}
int Fs::lookup(const char* p) {
  if (!g_fail_lk.empty() && p && g_fail_lk == p) return -1;   // host_fail_lookup
  struct stat st;
  if (stat(host_path(p).c_str(), &st) == 0) return 1;
  return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
}

static void fnv(uint64_t* h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) {
    *h ^= b[i];
    *h *= 0x100000001b3ull;
  }
}

static void walk(const std::string& mac, int depth, uint64_t* h, uint32_t* entries, uint32_t* hidden) {
  DIR* d = opendir(mac.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = readdir(d))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& nm : names) {
    if (nm[0] == '.') {
      ++*hidden;
      continue;
    }
    struct stat st;
    std::string full = mac + "/" + nm;
    if (stat(full.c_str(), &st) != 0) continue;
    uint64_t size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    uint8_t mark = S_ISDIR(st.st_mode) ? 1 : 0;
    fnv(h, nm.data(), nm.size());
    fnv(h, &mark, 1);
    fnv(h, &size, sizeof size);
    ++*entries;
    if (mark && depth < 12) {
      walk(full, depth + 1, h, entries, hidden);
      fnv(h, "/", 1);
    }
  }
}

uint64_t Fs::tree_hash(const char* path, uint32_t* entries, uint32_t* hidden) {
  *entries = *hidden = 0;
  struct stat st;
  std::string m = host_path(path);
  if (stat(m.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
  uint64_t h = 0xcbf29ce484222325ull;
  walk(m, 0, &h, entries, hidden);
  return h;
}

}  // namespace core
