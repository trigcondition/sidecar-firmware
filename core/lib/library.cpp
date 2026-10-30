// core/lib/library.cpp - see library.h. Sections: the records and the working index (core 0's), the published
// snapshots and the lock-free read side (any core), references and resolution, the card files (index.bin, log.txt,
// state.txt), the boot load, the walk, the hashing, the writers' hooks, the scheduler, the console.
#include "library.h"

#if CORE_MSAMP
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

#include "../fmt/fmt.h"
#include "../platform.h"
#include "sha256.h"
#include "srclib.h"

namespace core {
namespace lib {

namespace {

// ---- records: 64 bytes, the same in RAM and on the card (little-endian: the Pi and the Mac)
enum : uint16_t {
  fUnhashed = 1, fGone = 2, fUnreadable = 4, fDup = 8, fNoHash = 16,   // on the card
  fGoneOld = 0x1000, fVerify = 0x2000, fDel = 0x4000, fSeen = 0x8000    // RAM only
};
constexpr uint16_t kCardFlags = 0x00FF;
enum : uint8_t { kKindNone = 0, kKindWav, kKindAiff, kKindSfz };
struct Rec {
  uint8_t sha[32];
  uint64_t size;
  uint32_t crc;
  uint32_t quick;    // the first 4 bytes of the sha256 of the first 64 KB (0: not known)
  uint32_t fatdt;    // FAT date << 16 | time
  uint16_t flags;
  uint8_t kind, ch;
  uint32_t rate;
  uint32_t path;     // the path's offset in its table
};
static_assert(sizeof(Rec) == 64 && offsetof(Rec, path) == 60, "the library's record is 64 bytes on the card (index_check.cpp)");
inline bool hashed(const Rec& r) { return !(r.flags & fUnhashed); }
inline bool live(const Rec& r) { return !(r.flags & (fDel | fGone)); }

constexpr char kDir[] = "/.rytm/library";
constexpr char kIndex[] = "/.rytm/library/index.bin";
constexpr char kLog[] = "/.rytm/library/log.txt";
constexpr char kState[] = "/.rytm/library/state.txt";
constexpr char kLogHead[] = "#sidecar library-log 1\n";
constexpr uint32_t kLogCap = 256u << 10;      // log lines waiting for a card write (RAM)
constexpr uint32_t kQuick = 65536;
constexpr int kDepth = 12;

uint32_t hash_ci(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) h = (h ^ (uint8_t)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s)) * 16777619u;
  return h;
}

// ---- the working index: core 0's only
struct Index {
  Rec* r = nullptr;
  uint32_t n = 0, cap = 0;
  char* p = nullptr;
  uint32_t plen = 0, pcap = 0, garbage = 0;
  uint32_t* ht = nullptr;   // path -> record + 1 (0 empty, ~0u removed)
  uint32_t hcap = 0, hfill = 0;
};
Index W;
uint32_t g_wver = 0;   // bumped when the working index is made anew (w_clear, w_compact): its path offsets restart
constexpr uint32_t kTomb = 0xFFFFFFFFu;

const char* wpath(uint32_t i) { return W.p + W.r[i].path; }
void ht_put(Index& x, uint32_t i) {
  uint32_t h = hash_ci(x.p + x.r[i].path) & (x.hcap - 1);
  while (x.ht[h] && x.ht[h] != kTomb) h = (h + 1) & (x.hcap - 1);
  if (!x.ht[h]) ++x.hfill;
  x.ht[h] = i + 1;
}
void ht_rebuild(Index& x, uint32_t want) {
  uint32_t c = 1024;
  while (c < want * 2) c <<= 1;
  if (c != x.hcap) {
    delete[] x.ht;
    x.ht = new uint32_t[c];
    x.hcap = c;
  }
  memset(x.ht, 0, sizeof(uint32_t) * c);
  x.hfill = 0;
  for (uint32_t i = 0; i < x.n; ++i)
    if (!(x.r[i].flags & fDel)) ht_put(x, i);
}
int w_find(const char* path) {
  if (!W.hcap) return -1;
  uint32_t h = hash_ci(path) & (W.hcap - 1);
  for (uint32_t k = 0; k < W.hcap; ++k, h = (h + 1) & (W.hcap - 1)) {
    uint32_t v = W.ht[h];
    if (!v) return -1;
    if (v != kTomb && !strcasecmp(wpath(v - 1), path)) return (int)(v - 1);
  }
  return -1;
}
void ht_remove(uint32_t i) {
  uint32_t h = hash_ci(wpath(i)) & (W.hcap - 1);
  for (uint32_t k = 0; k < W.hcap; ++k, h = (h + 1) & (W.hcap - 1)) {
    if (!W.ht[h]) return;
    if (W.ht[h] == i + 1) {
      W.ht[h] = kTomb;
      return;
    }
  }
}
uint32_t add_path(const char* s) {
  uint32_t l = (uint32_t)strlen(s) + 1;
  if (W.plen + l > W.pcap) {
    uint32_t c = W.pcap ? W.pcap * 2 : 65536;
    while (c < W.plen + l) c *= 2;
    char* np = new char[c];
    if (W.plen) memcpy(np, W.p, W.plen);
    delete[] W.p;
    W.p = np, W.pcap = c;
  }
  memcpy(W.p + W.plen, s, l);
  W.plen += l;
  return W.plen - l;
}
bool w_room() {
  if (W.n < W.cap) return true;
  if (W.n >= kMaxFiles) return false;
  uint32_t c = W.cap ? W.cap * 2 : 1024;
  if (c > kMaxFiles) c = kMaxFiles;
  Rec* nr = new Rec[c];
  if (W.n) memcpy(nr, W.r, sizeof(Rec) * W.n);
  delete[] W.r;
  W.r = nr, W.cap = c;
  return true;
}
int w_add(const char* path) {
  if (!w_room()) return -1;
  if ((W.hfill + 1) * 10 > W.hcap * 7) ht_rebuild(W, W.n + 1);
  uint32_t i = W.n++;
  memset(&W.r[i], 0, sizeof(Rec));
  W.r[i].flags = fUnhashed;
  W.r[i].path = add_path(path);
  ht_put(W, i);
  return (int)i;
}
void w_setpath(uint32_t i, const char* path) {
  ht_remove(i);
  W.garbage += (uint32_t)strlen(wpath(i)) + 1;
  W.r[i].path = add_path(path);
  if ((W.hfill + 1) * 10 > W.hcap * 7) ht_rebuild(W, W.n + 1);   // (puts i with its new path: not again)
  else ht_put(W, i);
}
void w_del(uint32_t i) {
  if (W.r[i].flags & fDel) return;
  ht_remove(i);
  W.r[i].flags = (uint16_t)(W.r[i].flags | fDel);
  W.garbage += (uint32_t)strlen(wpath(i)) + 1;
}
void w_compact() {   // the deleted records and the old paths dropped (indices change: only between jobs)
  Index x;
  x.cap = W.n ? W.n : 1;
  x.r = new Rec[x.cap];
  uint32_t need = 1;
  for (uint32_t i = 0; i < W.n; ++i)
    if (!(W.r[i].flags & fDel)) need += (uint32_t)strlen(wpath(i)) + 1;
  x.pcap = need;
  x.p = new char[need];
  for (uint32_t i = 0; i < W.n; ++i) {
    if (W.r[i].flags & fDel) continue;
    Rec& r = x.r[x.n++];
    r = W.r[i];
    uint32_t l = (uint32_t)strlen(wpath(i)) + 1;
    memcpy(x.p + x.plen, wpath(i), l);
    r.path = x.plen;
    x.plen += l;
  }
  delete[] W.r;
  delete[] W.p;
  delete[] W.ht;
  ++g_wver;
  W = x;
  W.ht = nullptr, W.hcap = 0;
  ht_rebuild(W, W.n);
}
void w_clear() {
  ++g_wver;
  delete[] W.r;
  delete[] W.p;
  delete[] W.ht;
  W = Index();
  ht_rebuild(W, 0);
}

// ---- the published snapshots: two, swapped by core 0; a reader marks the one it reads (spec 7.2: lock-free)
struct Snap {
  int readers;
  bool ready, readonly;
  bool loaded;        // published after the boot load (never a snapshot of part of index.bin + log.txt): list() needs it
  uint32_t gen;       // the gen() this snapshot is published as
  uint32_t to_hash;   // records not hashed yet that will be (a sha not found may still appear: PENDING, not MISSING)
  uint32_t n, nh;   // records; the first nh are hashed, sorted by sha256
  Rec* rec;
  uint32_t rcap;
  char* paths;
  uint32_t plen, pcap;
  uint32_t* ht;
  uint32_t hcap;
  // the folder view (lib::list): every record and every folder of the walk ("/A/B/", in paths after the records' paths)
  // in path order, case ignored; an entry: a record's index, or kFoldBit | a folder path's offset
  uint32_t* byp;
  uint32_t nb, bcap;
};
constexpr uint32_t kFoldBit = 0x80000000u;
Snap g_snap[2];
Snap* g_pub = nullptr;
uint32_t g_gen = 0;

Snap* acquire() {
  for (int t = 0; t < 100000; ++t) {   // (a retry only when core 0 swapped meanwhile: once a second at most)
    Snap* s = __atomic_load_n(&g_pub, __ATOMIC_SEQ_CST);
    if (!s) return nullptr;
    __atomic_fetch_add(&s->readers, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_pub, __ATOMIC_SEQ_CST) == s) return s;
    __atomic_fetch_sub(&s->readers, 1, __ATOMIC_SEQ_CST);
  }
  return nullptr;
}
void release(Snap* s) {
  if (s) __atomic_fetch_sub(&s->readers, 1, __ATOMIC_SEQ_CST);
}
const char* spath(const Snap* s, uint32_t i) { return s->paths + s->rec[i].path; }
int s_find_path(const Snap* s, const char* path) {
  if (!s->hcap) return -1;
  uint32_t h = hash_ci(path) & (s->hcap - 1);
  for (uint32_t k = 0; k < s->hcap; ++k, h = (h + 1) & (s->hcap - 1)) {
    uint32_t v = s->ht[h];
    if (!v) return -1;
    if (!strcasecmp(spath(s, v - 1), path)) return (int)(v - 1);
  }
  return -1;
}
// the first hashed record with this sha (-1 none) and how many there are
int s_find_sha(const Snap* s, const uint8_t* sha, int* count) {
  uint32_t lo = 0, hi = s->nh;
  while (lo < hi) {
    uint32_t m = (lo + hi) / 2;
    if (memcmp(s->rec[m].sha, sha, 32) < 0) lo = m + 1;
    else hi = m;
  }
  int c = 0;
  for (uint32_t i = lo; i < s->nh && !memcmp(s->rec[i].sha, sha, 32); ++i) ++c;
  if (count) *count = c;
  return c ? (int)lo : -1;
}

// the folder view's order: bytes compared with A-Z folded (FAT ignores case), so a folder's paths are one run
inline uint8_t fold(char c) { return (uint8_t)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
int ci_cmp(const char* a, const char* b) {
  for (;; ++a, ++b) {
    uint8_t x = fold(*a), y = fold(*b);
    if (x != y || !x) return (int)x - (int)y;
  }
}
bool ci_pre(const char* p, const char* pre, size_t l) {
  for (size_t i = 0; i < l; ++i)
    if (fold(p[i]) != fold(pre[i])) return false;
  return true;
}
const char* bpath(const Snap* s, uint32_t e) { return e & kFoldBit ? s->paths + (e & ~kFoldBit) : spath(s, e); }
uint32_t b_lower(const Snap* s, const char* key, uint32_t lo) {   // the first entry at or after key
  uint32_t hi = s->nb;
  while (lo < hi) {
    uint32_t m = lo + (hi - lo) / 2;
    if (ci_cmp(bpath(s, s->byp[m]), key) < 0) lo = m + 1;
    else hi = m;
  }
  return lo;
}

// the folder view's sort (core 0, at a publish): a multikey quicksort (Bentley & Sedgewick) of the paths, A-Z folded:
// each character compared once per level, not the shared "/Samples/..." prefix again at every comparison
struct Key {
  const char* s;
  uint32_t id;
};
inline int kch(const Key& k, int d) { return fold(k.s[d]); }
void mkqs(Key* a, uint32_t n, int d) {
  while (n > 1) {
    if (n < 12) {   // short runs: insertion, compared from d
      for (uint32_t i = 1; i < n; ++i)
        for (uint32_t j = i; j > 0 && ci_cmp(a[j].s + d, a[j - 1].s + d) < 0; --j) std::swap(a[j], a[j - 1]);
      return;
    }
    int x = kch(a[0], d), y = kch(a[n / 2], d), z = kch(a[n - 1], d);
    int v = x < y ? (y < z ? y : x < z ? z : x) : (x < z ? x : y < z ? z : y);   // the median of three
    uint32_t lt = 0, i = 0, gt = n;
    while (i < gt) {
      int c = kch(a[i], d);
      if (c < v) std::swap(a[lt++], a[i++]);
      else if (c > v) std::swap(a[i], a[--gt]);
      else ++i;
    }
    mkqs(a, lt, d);
    mkqs(a + gt, n - gt, d);
    if (!v) return;   // (the equal ones ended here: equal strings)
    a += lt, n = gt - lt, ++d;
  }
}

// ---- the walk's folders (core 0): "/A/B/" each; cur the last walk's (with the writers' changes since), next the
// running walk's (cur at its end)
struct Folders {
  char* p = nullptr;
  uint32_t plen = 0, pcap = 0;
  uint32_t* off = nullptr;
  uint32_t n = 0, cap = 0;
};
Folders g_fcur, g_fnext;
bool g_fwalk = false;   // a walk fills g_fnext
uint32_t g_fver = 0;    // bumped with every change of g_fcur (the published folders; g_fnext's at the walk's end)
constexpr size_t kFolderMax = kPath - 2;   // a folder's longest path ("/A/B": its "/A/B/" key + NUL in kPath)
void f_clear(Folders& f) {
  if (&f == &g_fcur) ++g_fver;
  delete[] f.p;
  delete[] f.off;
  f = Folders();
}
const char* f_at(const Folders& f, uint32_t i) { return f.p + f.off[i]; }
void f_add(Folders& f, const char* path, size_t l, bool slash) {   // (no check for a copy: the walk sees each once)
  uint32_t need = (uint32_t)l + 2;
  if (f.plen + need > f.pcap) {
    uint32_t c = f.pcap ? f.pcap * 2 : 16384;
    while (c < f.plen + need) c *= 2;
    char* np = new char[c];
    if (f.plen) memcpy(np, f.p, f.plen);
    delete[] f.p;
    f.p = np, f.pcap = c;
  }
  if (f.n >= f.cap) {
    uint32_t c = f.cap ? f.cap * 2 : 256;
    uint32_t* no = new uint32_t[c];
    if (f.n) memcpy(no, f.off, sizeof(uint32_t) * f.n);
    delete[] f.off;
    f.off = no, f.cap = c;
  }
  if (&f == &g_fcur) ++g_fver;
  f.off[f.n++] = f.plen;
  memcpy(f.p + f.plen, path, l);
  f.plen += (uint32_t)l;
  if (slash) f.p[f.plen++] = '/';
  f.p[f.plen++] = 0;
}
// a folder made (touched on a folder, moved into the scope): into both sets, once
bool f_made(const char* path) {
  size_t l = strlen(path);
  if (!l || l > kFolderMax) return false;
  char k[kPath + 2];
  memcpy(k, path, l), k[l] = '/', k[l + 1] = 0;
  bool any = false;
  Folders* sets[2] = {&g_fcur, &g_fnext};
  for (Folders* f : sets) {
    if (f == &g_fnext && !g_fwalk) continue;
    bool have = false;
    for (uint32_t i = 0; i < f->n && !have; ++i) have = !ci_cmp(f_at(*f, i), k);
    if (!have) f_add(*f, k, l + 1, false), any = true;
  }
  return any;
}
// a folder gone (or moved: to its new place when to): it and everything under it
bool f_moved(const char* from, const char* to) {
  size_t fl = strlen(from);
  if (!fl) return false;
  bool any = false;
  char np[kPath + 2];
  Folders* sets[2] = {&g_fcur, &g_fnext};
  for (Folders* f : sets) {
    for (uint32_t i = 0; i < f->n;) {
      const char* q = f_at(*f, i);
      if (!ci_pre(q, from, fl) || q[fl] != '/') {
        ++i;
        continue;
      }
      any = true;
      if (f == &g_fcur) ++g_fver;
      int m = to ? sniprintf(np, sizeof np, "%s%s", to, q + fl) : 0;
      f->off[i] = f->off[--f->n];   // (its bytes left: the next walk's set is new)
      if (m > 1 && (size_t)(m - 1) <= kFolderMax) f_add(*f, np, (size_t)m, false);   // (m: with its '/')
    }
  }
  return any;
}

// ---- the stats (a seqlock: core 0 writes, any core copies)
Stats g_st;
uint32_t g_walked = 0;   // entries the walk has seen
volatile uint32_t g_st_seq = 0;
void st_begin() {
  __atomic_store_n(&g_st_seq, g_st_seq + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
void st_end() {
  __atomic_thread_fence(__ATOMIC_RELEASE);
  ++g_st.seq;
  __atomic_store_n(&g_st_seq, g_st_seq + 1, __ATOMIC_RELEASE);
}

// ---- core 0's state
Env g_env = {nullptr, nullptr, nullptr, nullptr};
Work g_work;
Bench g_bres;
bool g_card = false;
uint8_t g_phase = kOff;
bool g_readonly = false;          // the index on the card is a newer build's (or unreadable that way): never written
char g_why[80] = "";              // why read-only
bool g_walk_done = false, g_walk_req = false, g_compacted_boot = false;
bool g_dirty_pub = false, g_pub_now = false;
uint32_t g_wchg = 0;              // bumped with every change of the working index (hash_wanted's cache)
void dirty() {
  g_dirty_pub = true;
  ++g_wchg;
}
uint32_t g_last_pub_ms = 0;
uint64_t g_tree = 0, g_tree_card = 0;
bool g_state_dirty = false;
uint32_t g_walk_changes = 0;
uint32_t g_lines_card = 0;        // log.txt's lines on the card (after its header)
char* g_log = nullptr;            // lines waiting to be appended
uint32_t g_loglen = 0, g_lines_pending = 0;
bool g_log_head = false;          // log.txt has its header on the card
bool g_log_lost = false;          // a line could not be kept (the RAM log full, a compaction failed): compact soon
bool g_log_torn = false;          // log.txt may end inside a line (a cut, a short write): the next append starts a line
uint32_t g_fail_ms = 0;           // a card write failed then: writes rest a minute (a full or locked card: no busy loop)
bool g_failed = false;
bool resting() { return g_failed && now_ms() - g_fail_ms < 60000; }
void write_failed() { g_failed = true, g_fail_ms = now_ms(); }
uint32_t g_last_us = 0;
uint32_t g_verify_diff = 0, g_verify_done = 0;
bool g_verifying = false;
uint32_t g_compactions = 0;
char g_waiting[16] = "";
uint32_t g_step_rd = 0, g_step_wr = 0;

void logf(const char* fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  vsniprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  log(CORE_STR("lib: %s"), b);
}

// a library line for the card's log (pending until writes are allowed)
void log_line(const char* s) {
  if (g_readonly) return;
  if (!g_log) g_log = new char[kLogCap];
  uint32_t l = (uint32_t)strlen(s);
  if (g_loglen + l > kLogCap) {   // full (writes held back a long time): the next compaction writes it all
    g_log_lost = true;
    return;
  }
  memcpy(g_log + g_loglen, s, l);
  g_loglen += l;
  ++g_lines_pending;
}
void log_rec(uint32_t i) {
  const Rec& r = W.r[i];
  char sha[65], b[400];
  if (hashed(r)) hex(r.sha, 32, sha);
  else sha[0] = '-', sha[1] = 0;
  sniprintf(b, sizeof b, "+ %s %llu %08lx %08lx %08lx %04x %s\n", sha, (unsigned long long)r.size, (unsigned long)r.fatdt,
            (unsigned long)r.crc, (unsigned long)r.quick, (unsigned)(r.flags & kCardFlags), wpath(i));
  log_line(b);
}
void log_gone(const char* path) {
  char b[300];
  sniprintf(b, sizeof b, "- %s\n", path);
  log_line(b);
}

// ---- scope: the whole card minus the hidden roots, dot names, /.rytm (spec 6.1, open question 7)
bool in_scope(const char* path) {
  if (!path || path[0] != '/' || strchr(path, ':') || strlen(path) >= (size_t)kPath) return false;
  const char* c = path + 1;
  char first[kPath];
  const char* sl = strchr(c, '/');
  size_t l = sl ? (size_t)(sl - c) : strlen(c);
  if (!l || l >= sizeof first) return false;
  memcpy(first, c, l);
  first[l] = 0;
  if (srclib::hidden_name(first)) return false;
  for (const char* q = path; *q; ++q)
    if (*q == '/' && q[1] == '.') return false;   // a dot name anywhere (/.rytm too)
  return true;
}
uint8_t kind_of(const char* name) {
  size_t l = strlen(name);
  auto ends = [&](const char* e) { size_t k = strlen(e); return l > k && !strcasecmp(name + l - k, e); };
  return ends(".wav") ? kKindWav : ends(".aif") || ends(".aiff") ? kKindAiff : ends(".sfz") ? kKindSfz : kKindNone;
}
bool is_recording(const char* path) { return !strncasecmp(path, "/Recordings/", 12); }
bool under(const char* path, const char* dir) {
  size_t l = strlen(dir);
  return !strncasecmp(path, dir, l) && path[l] == '/';
}

bool H_idle();        // no hash job and no priority queue holds a record index (w_compact moves them)
void compacted_w();   // after w_compact: the index-holding queues reset
// ---- publish: the working index as a new snapshot (core 0; skipped while a reader holds the spare)
void st_counts();
bool publish() {
  Snap* cur = __atomic_load_n(&g_pub, __ATOMIC_SEQ_CST);
  Snap* sp = cur == &g_snap[0] ? &g_snap[1] : &g_snap[0];
  if (__atomic_load_n(&sp->readers, __ATOMIC_SEQ_CST) != 0) return false;
  if (W.garbage > W.plen / 2 && W.garbage > 65536 && H_idle()) w_compact(), compacted_w();
  static uint32_t* order = nullptr;
  static uint32_t ocap = 0;
  if (ocap < W.n + 1) {
    delete[] order;
    ocap = W.n + 1024;
    order = new uint32_t[ocap];
  }
  uint32_t n = 0, nh = 0, pl = 0;
  for (uint32_t i = 0; i < W.n; ++i)
    if (live(W.r[i]) && hashed(W.r[i])) order[n++] = i, pl += (uint32_t)strlen(wpath(i)) + 1;
  nh = n;
  for (uint32_t i = 0; i < W.n; ++i)
    if (live(W.r[i]) && !hashed(W.r[i])) order[n++] = i, pl += (uint32_t)strlen(wpath(i)) + 1;
  uint32_t nf = g_fcur.n;
  for (uint32_t i = 0; i < nf; ++i) pl += (uint32_t)strlen(f_at(g_fcur, i)) + 1;
  std::sort(order, order + nh, [](uint32_t a, uint32_t b) {
    int c = memcmp(W.r[a].sha, W.r[b].sha, 32);
    return c ? c < 0 : strcasecmp(wpath(a), wpath(b)) < 0;
  });
  if (sp->rcap < n + 1) {
    delete[] sp->rec;
    sp->rcap = n + 1024;
    sp->rec = new Rec[sp->rcap];
  }
  if (sp->pcap < pl + 1) {
    delete[] sp->paths;
    sp->pcap = pl + 65536;
    sp->paths = new char[sp->pcap];
  }
  uint32_t hc = 1024;
  while (hc < n * 2) hc <<= 1;
  if (sp->hcap != hc) {
    delete[] sp->ht;
    sp->ht = new uint32_t[hc];
    sp->hcap = hc;
  }
  memset(sp->ht, 0, sizeof(uint32_t) * hc);
  uint32_t off = 0;
  for (uint32_t k = 0; k < n; ++k) {
    Rec& r = sp->rec[k];
    uint32_t i = order[k];
    r = W.r[i];
    r.flags = (uint16_t)(r.flags & ~fDup & kCardFlags);
    W.r[i].flags = (uint16_t)(W.r[i].flags & ~fDup);
    uint32_t l = (uint32_t)strlen(wpath(i)) + 1;
    memcpy(sp->paths + off, wpath(i), l);
    r.path = off;
    off += l;
    uint32_t h = hash_ci(sp->paths + r.path) & (hc - 1);
    while (sp->ht[h]) h = (h + 1) & (hc - 1);
    sp->ht[h] = k + 1;
  }
  for (uint32_t k = 1; k < nh; ++k)   // duplicates are adjacent (sorted by sha)
    if (!memcmp(sp->rec[k].sha, sp->rec[k - 1].sha, 32)) {
      sp->rec[k].flags |= fDup, sp->rec[k - 1].flags |= fDup;
      W.r[order[k]].flags |= fDup, W.r[order[k - 1]].flags |= fDup;
    }
  // the folder view: the records and the walk's folders in path order. Sorted again only when the live records'
  // paths or the folders changed (a hash of the live records' indices and path offsets - a path's offset is new when
  // it changes - the index's and the folders' versions): the hashing's publishes do not sort
  static Key* keys = nullptr;
  static uint32_t *word = nullptr, *inv = nullptr, *foff = nullptr;
  static uint32_t kcap = 0, icap = 0, fcap = 0, nword = 0;
  static uint64_t wsig = 0;
  uint32_t nb = n + nf;
  uint64_t sig = 0xcbf29ce484222325ull ^ ((uint64_t)g_wver << 32 | g_fver);
  for (uint32_t i = 0; i < W.n; ++i)
    if (live(W.r[i])) sig = (sig ^ ((uint64_t)i << 32 | W.r[i].path)) * 0x100000001b3ull;
  if (kcap < nb + 1) {
    delete[] keys, delete[] word;
    kcap = nb + 1024;
    keys = new Key[kcap];
    word = new uint32_t[kcap];
    nword = 0;
  }
  if (sig != wsig || nword != nb) {
    for (uint32_t k = 0; k < n; ++k) keys[k] = Key{wpath(order[k]), order[k]};
    for (uint32_t f = 0; f < nf; ++f) keys[n + f] = Key{f_at(g_fcur, f), kFoldBit | f};
    mkqs(keys, nb, 0);
    for (uint32_t j = 0; j < nb; ++j) word[j] = keys[j].id;
    wsig = sig, nword = nb;
    ++g_work.folder_sorts;
  }
  if (icap < W.n + 1) {
    delete[] inv;
    icap = W.n + 1024;
    inv = new uint32_t[icap];
  }
  if (fcap < nf + 1) {
    delete[] foff;
    fcap = nf + 256;
    foff = new uint32_t[fcap];
  }
  for (uint32_t k = 0; k < n; ++k) inv[order[k]] = k;
  for (uint32_t f = 0; f < nf; ++f) {
    uint32_t l = (uint32_t)strlen(f_at(g_fcur, f)) + 1;
    memcpy(sp->paths + off, f_at(g_fcur, f), l);
    foff[f] = off;
    off += l;
  }
  if (sp->bcap < nb + 1) {
    delete[] sp->byp;
    sp->bcap = nb + 1024;
    sp->byp = new uint32_t[sp->bcap];
  }
  for (uint32_t j = 0; j < nb; ++j) sp->byp[j] = word[j] & kFoldBit ? kFoldBit | foff[word[j] & ~kFoldBit] : inv[word[j]];
  sp->nb = nb;
  sp->n = n, sp->nh = nh, sp->plen = off;
  sp->to_hash = 0;
  for (uint32_t k = nh; k < n; ++k)
    if (!(sp->rec[k].flags & (fNoHash | fUnreadable))) ++sp->to_hash;
  sp->ready = g_walk_done;
  sp->loaded = g_phase != kLoading;
  sp->gen = g_gen + 1;
  sp->readonly = g_readonly;
  __atomic_store_n(&g_pub, sp, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_gen, g_gen + 1, __ATOMIC_RELEASE);
  ++g_work.publishes;
  g_dirty_pub = g_pub_now = false;
  g_last_pub_ms = now_ms();
  st_counts();
  return true;
}

// the counts (core 0, at a publish): a pass over the working index
void st_counts() {
  Stats s = g_st;
  s.files = s.unhashed = s.dups = s.hashed = s.gone = s.unreadable = s.recordings = 0;
  s.bytes = s.hashed_bytes = s.dup_bytes = 0;
  uint64_t hashable = 0;
  Snap* p = g_pub;
  for (uint32_t i = 0; i < W.n; ++i) {
    const Rec& r = W.r[i];
    if (r.flags & fDel) continue;
    if (r.flags & fGone) {
      ++s.gone;
      continue;
    }
    ++s.files;
    s.bytes += r.size;
    if (r.flags & fNoHash) ++s.recordings;
    if (r.flags & fUnreadable) ++s.unreadable;
    if (hashed(r)) ++s.hashed, s.hashed_bytes += r.size;
    else if (!(r.flags & (fNoHash | fUnreadable))) ++s.unhashed;
    if (!(r.flags & (fNoHash | fUnreadable)) || hashed(r)) hashable += r.size;
  }
  if (p)
    for (uint32_t k = 1; k < p->nh; ++k)
      if (!memcmp(p->rec[k].sha, p->rec[k - 1].sha, 32)) ++s.dups, s.dup_bytes += p->rec[k].size;
  s.pct = (uint8_t)(hashable ? s.hashed_bytes * 100 / hashable : 100);
  if (s.pct == 100 && s.unhashed) s.pct = 99;
  s.state = g_phase;
  st_begin();
  g_st = s;
  st_end();
}
void st_misc() {   // the cheap fields (each step)
  st_begin();
  g_st.state = g_phase;
  g_st.log_lines = g_lines_card + g_lines_pending;
  g_st.pending_log = g_lines_pending;
  g_st.compactions = g_compactions;
  g_st.walked = g_walked;
  sniprintf(g_st.waiting, sizeof g_st.waiting, "%s", g_waiting);
  st_end();
}

// ---- hash_now's ring: any core posts, core 0 takes (a slot claimed with a CAS, published by its sequence)
constexpr uint32_t kRing = 32;
struct Slot {
  volatile uint32_t seq;
  char path[kPath];
};
Slot g_ring[kRing];
volatile uint32_t g_rhead = 0, g_rtail = 0;
uint32_t g_ring_dropped = 0;

// ---- the hashing's queues (core 0)
constexpr int kPrio = 512;
uint32_t g_prio[kPrio];
int g_nprio = 0;
uint32_t g_refs[1024];
int g_nrefs = 0, g_refs_i = 0;
bool g_refs_dirty = true;
uint32_t g_cursor = 0;
uint64_t* g_gone_sizes = nullptr;
uint32_t g_ngone = 0, g_gone_cap = 0;

void prio_add(uint32_t i) {
  for (int k = 0; k < g_nprio; ++k)
    if (g_prio[k] == i) return;
  if (g_nprio < kPrio) g_prio[g_nprio++] = i;
}

// ---- the hash job
struct HashJob {
  int rec = -1;
  File f;
  uint64_t off = 0, size = 0;
  Sha256 sh, qs;
  uint32_t crc = 0;
  uint16_t d = 0, t = 0;
  uint8_t ch = 0;
  uint32_t rate = 0;
  bool prio = false, ref = false;
};
HashJob H;
bool H_idle() { return H.rec < 0 && g_nprio == 0; }
void compacted_w() {
  g_nrefs = g_refs_i = 0, g_refs_dirty = true;   // (made again from their paths)
  g_cursor = 0;
}
void hash_abort() {
  if (H.rec >= 0) H.f.close();
  H.rec = -1;
}
void index_changed_at(uint32_t i) {   // a record's indices are about to move (w_compact) or the record changed
  if (H.rec == (int)i) hash_abort();
}

// ---- the card files' state machine
enum Load { L_RECOVER = 0, L_INDEX_OPEN, L_INDEX_READ, L_LOG_OPEN, L_LOG_READ, L_STATE, L_DONE };
int g_load = L_RECOVER;
File g_lf;
uint8_t* g_img = nullptr;
uint32_t g_img_len = 0, g_img_off = 0;
char g_carry[1024];
uint32_t g_carry_n = 0;
bool g_log_bad = false;

// the walk
struct Level {
  File d;
  uint16_t plen;
};
Level g_lv[kDepth + 1];
int g_depth = -1;
char g_wpath[512];

// compaction
fmt::AtomicWriter g_aw;
uint8_t* g_cimg = nullptr;
uint32_t g_cimg_len = 0;
bool g_compacting = false;

// bench
int g_bench = 0;   // 0 off, 1 sha, 2 crc, 3 read
uint32_t g_bench_i = 0;
uint64_t g_bench_ns = 0, g_bench_bytes = 0;
File g_bf;
uint8_t* g_bbuf = nullptr;

// ---- the index image (index.bin): header 16 (fmt.h's binary header), then u32 nrec, u32 path bytes, u32 0, u32 0,
// the records (hashed by sha256, then the rest), the path table
constexpr uint32_t kBody = 16;
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

bool decode(const uint8_t* p, uint32_t n) {   // (index_check ran in fmt::load just before)
  uint32_t hb = rd32(p + 8);
  const uint8_t* b = p + hb;
  uint32_t nr = rd32(b), pl = rd32(b + 4);
  const Rec* rr = (const Rec*)(b + kBody);
  const char* pt = (const char*)(b + kBody + (size_t)nr * sizeof(Rec));
  (void)pl;
  w_clear();
  for (uint32_t i = 0; i < nr; ++i) {
    Rec r;
    memcpy(&r, rr + i, sizeof r);
    if (!in_scope(pt + r.path)) continue;
    int k = w_add(pt + r.path);
    if (k < 0) break;
    uint32_t po = W.r[k].path;
    W.r[k] = r;
    W.r[k].path = po;
    W.r[k].flags &= kCardFlags;
    if (W.r[k].flags & fGone) W.r[k].flags |= fGoneOld;   // gone at the last compaction: dropped at the next
  }
  return true;
}
// the image of the working index for index.bin (core 0): the live records and the newly gone ones
uint32_t encode(uint8_t** out) {
  uint32_t n = 0, pl = 0;
  for (uint32_t i = 0; i < W.n; ++i) {
    const Rec& r = W.r[i];
    if ((r.flags & fDel) || (r.flags & fGoneOld)) continue;
    ++n, pl += (uint32_t)strlen(wpath(i)) + 1;
  }
  uint32_t len = fmt::kBinHeader + kBody + n * (uint32_t)sizeof(Rec) + pl;
  uint8_t* img = new uint8_t[len];
  uint8_t* b = img + fmt::kBinHeader;
  wr32(b, n), wr32(b + 4, pl), wr32(b + 8, 0), wr32(b + 12, 0);
  Rec* rr = (Rec*)(b + kBody);
  char* pt = (char*)(b + kBody + (size_t)n * sizeof(Rec));
  static uint32_t* order = nullptr;
  static uint32_t ocap = 0;
  if (ocap < W.n + 1) delete[] order, ocap = W.n + 1024, order = new uint32_t[ocap];
  uint32_t k = 0, nh;
  for (uint32_t i = 0; i < W.n; ++i)
    if (!(W.r[i].flags & (fDel | fGoneOld)) && hashed(W.r[i])) order[k++] = i;
  nh = k;
  for (uint32_t i = 0; i < W.n; ++i)
    if (!(W.r[i].flags & (fDel | fGoneOld)) && !hashed(W.r[i])) order[k++] = i;
  std::sort(order, order + nh, [](uint32_t a, uint32_t b) { return memcmp(W.r[a].sha, W.r[b].sha, 32) < 0; });
  uint32_t off = 0;
  for (uint32_t j = 0; j < n; ++j) {
    Rec r = W.r[order[j]];
    r.flags &= kCardFlags;
    uint32_t l = (uint32_t)strlen(wpath(order[j])) + 1;
    memcpy(pt + off, wpath(order[j]), l);
    r.path = off;
    off += l;
    memcpy(rr + j, &r, sizeof r);
  }
  fmt::put_bin_header(img, "SLIB", 1, 1, fmt::kBinHeader, crc32(0, b, len - fmt::kBinHeader));
  *out = img;
  return len;
}

// ---- the log's lines (replay at boot)
uint64_t parse_u(const char*& p, int base) {
  uint64_t v = 0;
  for (;; ++p) {
    int d = *p >= '0' && *p <= '9' ? *p - '0' : base == 16 && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
    if (d < 0) break;
    v = v * (uint64_t)base + (uint64_t)d;
  }
  return v;
}
void rename_prefix(const char* from, const char* to, bool log_it);
void replay(const char* l) {
  if (l[0] == '+' && l[1] == ' ') {
    const char* p = l + 2;
    uint8_t sha[32];
    bool has = false;
    if (*p == '-') ++p;
    else if (unhex_prefix(p, sha, 32)) has = true, p += 64;
    else return;
    if (*p++ != ' ') return;
    uint64_t size = parse_u(p, 10);
    if (*p++ != ' ') return;
    uint32_t dt = (uint32_t)parse_u(p, 16);
    if (*p++ != ' ') return;
    uint32_t crc = (uint32_t)parse_u(p, 16);
    if (*p++ != ' ') return;
    uint32_t quick = (uint32_t)parse_u(p, 16);
    if (*p++ != ' ') return;
    uint16_t fl = (uint16_t)parse_u(p, 16);
    if (*p++ != ' ' || !in_scope(p)) return;
    int i = w_find(p);
    if (i < 0) i = w_add(p);
    if (i < 0) return;
    Rec& r = W.r[i];
    r.size = size, r.fatdt = dt, r.crc = crc, r.quick = quick;
    r.flags = (uint16_t)(fl & kCardFlags & ~fGone & ~fDup);
    if (has) memcpy(r.sha, sha, 32), r.flags &= (uint16_t)~fUnhashed;
    else memset(r.sha, 0, 32), r.flags |= fUnhashed;
  } else if (l[0] == '-' && l[1] == ' ') {
    int i = w_find(l + 2);
    if (i >= 0) W.r[i].flags = (uint16_t)((W.r[i].flags | fGone) & ~fGoneOld);
  } else if (l[0] == '>' && l[1] == ' ') {
    const char* tab = strchr(l + 2, '\t');
    if (!tab) return;
    char from[kPath];
    size_t fl = (size_t)(tab - (l + 2));
    if (fl >= sizeof from) return;
    memcpy(from, l + 2, fl);
    from[fl] = 0;
    if (in_scope(tab + 1)) rename_prefix(from, tab + 1, false);
  }
}

// a file or a folder renamed: the records follow (their hashes kept)
void rename_prefix(const char* from, const char* to, bool log_it) {
  int i = w_find(from);
  bool any = false, folder = false;
  char np[512];
  if (i >= 0 && live(W.r[i])) {
    index_changed_at((uint32_t)i);
    int old = w_find(to);
    if (old >= 0 && old != i) w_del((uint32_t)old);
    w_setpath((uint32_t)i, to);
    if (!g_walk_done) W.r[i].flags |= fSeen;
    any = true;
  } else {
    size_t fl = strlen(from);
    for (uint32_t k = 0; k < W.n; ++k) {
      if ((W.r[k].flags & fDel) || !under(wpath(k), from)) continue;
      int m = sniprintf(np, sizeof np, "%s%s", to, wpath(k) + fl);
      if (m <= 0 || m >= kPath) {
        w_del(k);
        continue;
      }
      index_changed_at(k);
      int old = w_find(np);
      if (old >= 0 && old != (int)k) w_del((uint32_t)old);
      w_setpath(k, np);
      if (!g_walk_done) W.r[k].flags |= fSeen;
      any = true;
      folder = true;
    }
  }
  if (any && log_it) {
    char b[400];
    sniprintf(b, sizeof b, "> %s\t%s\n", from, to);
    log_line(b);
  }
  if (any) dirty();
  if (folder && g_phase == kWalking && log_it) g_walk_req = true;   // a folder moved under the walk: walked again
}

// ---- a file seen (the walk, touched): its record (made or updated); changed -> unhashed
void saw_file(const char* path, uint64_t size, uint32_t dt, bool from_walk) {
  int i = w_find(path);
  bool rec = is_recording(path);
  if (i < 0) {
    i = w_add(path);
    if (i < 0) return;
    Rec& r = W.r[i];
    r.size = size, r.fatdt = dt, r.kind = kind_of(path);
    if (rec) r.flags |= fNoHash;
    if (from_walk) r.flags |= fSeen;
    log_rec((uint32_t)i);
    ++g_walk_changes;
    dirty();
    return;
  }
  Rec& r = W.r[i];
  bool was_gone = (r.flags & fGone) != 0;
  if (from_walk) r.flags |= fSeen;
  r.flags = (uint16_t)(r.flags & ~(fGone | fGoneOld));
  if (r.size == size && r.fatdt == dt && !was_gone) return;
  if (r.size != size || r.fatdt != dt) {
    index_changed_at((uint32_t)i);
    r.size = size, r.fatdt = dt;
    r.flags = (uint16_t)((r.flags | fUnhashed) & ~(fUnreadable | fDup));
    memset(r.sha, 0, 32);
    r.quick = r.crc = 0;
  }
  if (rec) r.flags |= fNoHash;
  log_rec((uint32_t)i);
  ++g_walk_changes;
  dirty();
}

// ---- the walk: the card's folders in pieces; the fingerprint and the diff in one pass
void fnv(uint64_t* h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) *h = (*h ^ b[i]) * 0x100000001b3ull;
}
void walk_close() {
  for (int d = 0; d <= g_depth; ++d) g_lv[d].d.close();
  g_depth = -1;
}
void walk_begin() {
  walk_close();
  for (uint32_t i = 0; i < W.n; ++i) W.r[i].flags &= (uint16_t)~fSeen;
  g_tree = 0xcbf29ce484222325ull;
  g_walked = 0, g_walk_changes = 0;
  g_walk_done = false;
  f_clear(g_fnext), g_fwalk = true;
  sniprintf(g_wpath, sizeof g_wpath, "/");
  if (!g_lv[0].d.open("/", kRead) || !g_lv[0].d.isDir()) {
    g_lv[0].d.close();
    g_walk_done = true;
    return;
  }
  g_lv[0].plen = 0;
  g_depth = 0;
  g_wpath[0] = 0;
  g_phase = kWalking;
}
void walk_end() {
  walk_close();
  f_clear(g_fcur);
  g_fcur = g_fnext, g_fnext = Folders(), g_fwalk = false, ++g_fver;   // (the folders: the walk's)
  g_ngone = 0;
  for (uint32_t i = 0; i < W.n; ++i) {
    Rec& r = W.r[i];
    if (r.flags & fDel) continue;
    if (!(r.flags & fSeen) && !(r.flags & fGone)) {
      index_changed_at(i);
      r.flags |= fGone;
      log_gone(wpath(i));
      ++g_walk_changes;
    }
    if ((r.flags & fGone) && hashed(r)) {   // a move's candidates: new files of a gone file's size hashed first
      if (g_ngone >= g_gone_cap) {
        uint32_t c = g_gone_cap ? g_gone_cap * 2 : 256;
        uint64_t* ng = new uint64_t[c];
        if (g_ngone) memcpy(ng, g_gone_sizes, sizeof(uint64_t) * g_ngone);
        delete[] g_gone_sizes;
        g_gone_sizes = ng, g_gone_cap = c;
      }
      g_gone_sizes[g_ngone++] = r.size;
    }
    r.flags &= (uint16_t)~fSeen;
  }
  std::sort(g_gone_sizes, g_gone_sizes + g_ngone);
  g_walk_done = true;
  if (g_walk_changes || g_tree != g_tree_card) g_state_dirty = true;
  g_tree_card = g_tree;
  g_pub_now = true, dirty();
  g_cursor = 0;
  g_refs_dirty = true;
  st_begin();
  g_st.walked = g_walked;
  st_end();
  logf("walk: %lu entries, %lu changes", (unsigned long)g_walked, (unsigned long)g_walk_changes);
}
// up to n entries; true when the walk is over
bool walk_step(int n) {
  for (int k = 0; k < n; ++k) {
    if (g_depth < 0) return walk_end(), true;
    Level& L = g_lv[g_depth];
    char name[256];
    bool dir;
    uint64_t size;
    uint16_t d, t;
    if (!L.d.nextEntry(name, sizeof name, &dir, &size, &d, &t)) {
      L.d.close();
      g_wpath[L.plen] = 0;
      --g_depth;
      if (g_depth >= 0) {
        g_wpath[g_lv[g_depth].plen] = 0;
        fnv(&g_tree, "/", 1);
      }
      continue;
    }
    if (name[0] == '.') continue;
    if (g_depth == 0 && srclib::hidden_name(name)) continue;
    size_t pl = L.plen, nl = strlen(name);
    if (pl + 1 + nl + 1 > sizeof g_wpath) continue;
    g_wpath[pl] = '/';
    memcpy(g_wpath + pl + 1, name, nl + 1);
    uint8_t mark = dir ? 1 : 0;
    uint32_t dt = (uint32_t)d << 16 | t;
    if (dir) {
      fnv(&g_tree, name, nl), fnv(&g_tree, &mark, 1);
      if (g_depth < kDepth && g_lv[g_depth + 1].d.open(g_wpath, kRead) && g_lv[g_depth + 1].d.isDir()) {
        if (pl + 1 + nl <= kFolderMax) f_add(g_fnext, g_wpath, pl + 1 + nl, true);   // (a folder walked: its contents known)
        ++g_depth;
        g_lv[g_depth].plen = (uint16_t)(pl + 1 + nl);
      } else {
        g_lv[g_depth + 1].d.close();
        g_wpath[pl] = 0;
      }
      continue;
    }
    if (!kind_of(name) || strlen(g_wpath) >= (size_t)kPath) {
      g_wpath[pl] = 0;
      continue;
    }
    fnv(&g_tree, name, nl), fnv(&g_tree, &mark, 1), fnv(&g_tree, &size, sizeof size), fnv(&g_tree, &dt, sizeof dt);
    ++g_walked;
    saw_file(g_wpath, size, dt, true);
    g_wpath[pl] = 0;
  }
  return false;
}

// ---- the hashing
bool hashable(const Rec& r) { return live(r) && ((!hashed(r) && !(r.flags & (fNoHash | fUnreadable))) || (r.flags & fVerify)); }
bool gone_size(uint64_t s) { return g_ngone && std::binary_search(g_gone_sizes, g_gone_sizes + g_ngone, s); }
void take_ring();
int pick(bool* prio, bool* ref) {
  *prio = *ref = false;
  take_ring();
  while (g_nprio) {
    uint32_t i = g_prio[0];
    memmove(g_prio, g_prio + 1, sizeof(uint32_t) * (size_t)--g_nprio);
    if (i < W.n && live(W.r[i]) && (!hashed(W.r[i]) || (W.r[i].flags & fVerify))) return *prio = true, (int)i;
  }
  if (!g_walk_done) return -1;   // the rest after the walk (its paths known)
  if (g_refs_dirty) {
    g_refs_dirty = false;
    g_nrefs = g_refs_i = 0;
    if (g_env.current_refs) {
      static const char* paths[1024];
      int n = g_env.current_refs(paths, 1024);
      for (int k = 0; k < n && g_nrefs < 1024; ++k) {
        if (!paths[k]) continue;
        int i = w_find(paths[k]);
        if (i >= 0 && hashable(W.r[i])) g_refs[g_nrefs++] = (uint32_t)i;
      }
    }
  }
  while (g_refs_i < g_nrefs) {
    uint32_t i = g_refs[g_refs_i++];
    if (i < W.n && hashable(W.r[i])) return *ref = true, (int)i;
  }
  if (g_ngone)
    for (uint32_t i = 0; i < W.n; ++i)
      if (hashable(W.r[i]) && !hashed(W.r[i]) && gone_size(W.r[i].size)) return (int)i;
  for (uint32_t k = 0; k < W.n; ++k, ++g_cursor) {
    if (g_cursor >= W.n) g_cursor = 0;
    if (hashable(W.r[g_cursor])) return (int)g_cursor;
  }
  return -1;
}
bool hash_wanted() {
  if (H.rec >= 0) return true;
  if (g_nprio || g_rtail != __atomic_load_n(&g_rhead, __ATOMIC_ACQUIRE)) return true;
  if (!g_walk_done) return false;
  // the scan only after a change (READY: nothing changes, nothing scanned), from the cursor (the next one is near)
  static uint32_t s_chg = ~0u;
  static bool s_want = false;
  if (s_chg == g_wchg) return s_want;
  s_chg = g_wchg, s_want = false;
  for (uint32_t k = 0; k < W.n && !s_want; ++k) {
    uint32_t i = (g_cursor + k) % W.n;
    if (hashable(W.r[i])) s_want = true, g_cursor = i;
  }
  return s_want;
}
void wav_info(const uint8_t* b, uint32_t n, uint8_t kind, uint8_t* ch, uint32_t* rate) {
  *ch = 0, *rate = 0;
  if (kind == kKindWav && n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
    for (uint32_t o = 12; o + 8 <= n;) {
      uint32_t l = rd32(b + o + 4);
      if (!memcmp(b + o, "fmt ", 4) && o + 16 <= n) {
        *ch = b[o + 10];
        *rate = rd32(b + o + 12);
        return;
      }
      if (l > n) return;
      o += 8 + l + (l & 1);
    }
  } else if (kind == kKindAiff && n >= 12 && !memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4))) {
    for (uint32_t o = 12; o + 8 <= n;) {
      uint32_t l = (uint32_t)b[o + 4] << 24 | (uint32_t)b[o + 5] << 16 | (uint32_t)b[o + 6] << 8 | b[o + 7];
      if (!memcmp(b + o, "COMM", 4) && o + 26 <= n) {
        *ch = b[o + 9];
        int e = ((b[o + 16] & 0x7F) << 8 | b[o + 17]) - 16383;   // the 80-bit rate: exponent, then the mantissa's top
        uint32_t m = (uint32_t)b[o + 18] << 24 | (uint32_t)b[o + 19] << 16 | (uint32_t)b[o + 20] << 8 | b[o + 21];
        *rate = e >= 0 && e < 32 ? m >> (31 - e) : 0;
        return;
      }
      if (l > n) return;
      o += 8 + l + (l & 1);
    }
  }
}
void hash_done() {
  Rec& r = W.r[H.rec];
  uint8_t sha[32], q[32];
  H.sh.final(sha);
  H.qs.final(q);
  if ((r.flags & fVerify) && hashed(r) && memcmp(r.sha, sha, 32)) {
    ++g_verify_diff;
    char a[65];
    hex(sha, 32, a);
    logf("verify: %s differs (now %.12s)", wpath(H.rec), a);
  }
  if (r.flags & fVerify) ++g_verify_done;
  memcpy(r.sha, sha, 32);
  r.crc = H.crc, r.quick = rd32(q), r.ch = H.ch, r.rate = H.rate, r.size = H.size;
  r.fatdt = (uint32_t)H.d << 16 | H.t;
  r.flags = (uint16_t)(r.flags & ~(fUnhashed | fVerify | fUnreadable));
  log_rec((uint32_t)H.rec);
  ++g_work.files_hashed;
  dirty();
  if (H.prio) g_pub_now = true;
  if (H.ref && g_refs_i >= g_nrefs) g_pub_now = true;   // the current project's references done: published at once
  H.f.close();
  H.rec = -1;
}
// one piece of the hash job (or its open); true: did something
bool hash_step(uint32_t piece) {
  if (H.rec < 0) {
    bool prio, ref;
    int i = pick(&prio, &ref);
    if (i < 0) return false;
    H.rec = i, H.prio = prio, H.ref = ref;
    if (!H.f.open(wpath(i), kRead) || H.f.isDir()) {
      H.f.close();
      W.r[i].flags = (uint16_t)((W.r[i].flags | fUnreadable) & ~fVerify);
      log_rec((uint32_t)i);
      H.rec = -1;
      dirty();
      return true;
    }
    H.size = H.f.fileSize();
    H.f.getModifyDateTime(&H.d, &H.t);
    H.off = 0, H.crc = 0;
    H.sh.init(), H.qs.init();
    return true;
  }
  static uint8_t buf[kPiece];
  uint32_t n = H.size - H.off < piece ? (uint32_t)(H.size - H.off) : piece;
  if (n) {
    int got = H.f.read(buf, n);
    g_step_rd += got > 0 ? (uint32_t)got : 0;
    if (got != (int)n) {   // cut short or a card error: unreadable (the walk tries again when it changes)
      Rec& r = W.r[H.rec];
      r.flags = (uint16_t)((r.flags | fUnreadable) & ~fVerify);
      log_rec((uint32_t)H.rec);
      H.f.close();
      H.rec = -1;
      dirty();
      return true;
    }
    if (H.off == 0) wav_info(buf, n, W.r[H.rec].kind ? W.r[H.rec].kind : kind_of(wpath(H.rec)), &H.ch, &H.rate);
    H.sh.update(buf, n);
    H.crc = crc32(H.crc, buf, n);
    if (H.off < kQuick) H.qs.update(buf, H.off + n <= kQuick ? n : (uint32_t)(kQuick - H.off));
    H.off += n;
  }
  if (H.off >= H.size) hash_done();
  return true;
}

// hash_now's ring: paths into the priority list (a path not known yet: looked at now)
void touched_core(const char* path, const uint8_t* sha, uint32_t crc, bool prio);
void take_ring() {
  for (;;) {
    uint32_t t = g_rtail;
    if (t == __atomic_load_n(&g_rhead, __ATOMIC_ACQUIRE)) return;
    Slot& s = g_ring[t % kRing];
    if (__atomic_load_n(&s.seq, __ATOMIC_ACQUIRE) != t + 1) return;   // claimed, not written yet
    char p[kPath];
    memcpy(p, s.path, kPath);
    p[kPath - 1] = 0;
    __atomic_store_n(&g_rtail, t + 1, __ATOMIC_RELEASE);
    int i = w_find(p);
    if (i >= 0 && live(W.r[i])) {
      if (!hashed(W.r[i])) prio_add((uint32_t)i);
    } else {
      touched_core(p, nullptr, 0, true);
    }
  }
}

// a writer's report while the index is still being read (the load would replace the record): kept and applied after
struct Early {
  char op;   // 't' touched, 'm' moved
  bool has_sha;
  uint8_t sha[32];
  uint32_t crc;
  char a[kPath], b[kPath];
};
constexpr int kEarly = 64;
Early g_early[kEarly];
int g_nearly = 0;
bool g_early_lost = false;
bool early(char op, const char* a, const char* b, const uint8_t* sha, uint32_t crc) {
  if (g_phase != kLoading) return false;
  if (strlen(a) >= (size_t)kPath || (b && strlen(b) >= (size_t)kPath)) return true;   // (not a library path: dropped)
  if (g_nearly >= kEarly) return g_early_lost = true;
  Early& e = g_early[g_nearly++];
  e.op = op, e.has_sha = sha != nullptr, e.crc = crc;
  if (sha) memcpy(e.sha, sha, 32);
  sniprintf(e.a, sizeof e.a, "%s", a);
  sniprintf(e.b, sizeof e.b, "%s", b ? b : "");
  return true;
}
void touched_core(const char* path, const uint8_t* sha, uint32_t crc, bool prio) {
  if (!g_card || !in_scope(path)) return;
  if (early('t', path, nullptr, sha, crc)) return;
  File f;
  int i = w_find(path);
  if (!f.open(path, kRead)) {   // gone (deleted by the Pi)
    if (f_moved(path, nullptr)) dirty();   // (a folder)
    if (i >= 0 && live(W.r[i])) {
      index_changed_at((uint32_t)i);
      W.r[i].flags = (uint16_t)(W.r[i].flags | fGone);
      log_gone(path);
      dirty();
    }
    return;
  }
  bool dir = f.isDir();
  uint64_t size = f.fileSize();
  uint16_t d = 0, t = 0;
  f.getModifyDateTime(&d, &t);
  f.close();
  if (dir && f_made(path)) dirty();
  if (dir || !kind_of(path)) return;
  uint32_t dt = (uint32_t)d << 16 | t;
  if (i < 0) {
    i = w_add(path);
    if (i < 0) return;
    W.r[i].kind = kind_of(path);
    if (is_recording(path)) W.r[i].flags |= fNoHash;
  }
  index_changed_at((uint32_t)i);
  Rec& r = W.r[i];
  r.size = size, r.fatdt = dt;
  r.flags = (uint16_t)(r.flags & ~(fGone | fGoneOld | fUnreadable | fDup | fVerify));
  if (!g_walk_done) r.flags |= fSeen;   // (a walk running: it is there, whether the walk has passed it or not)
  if (sha) {   // the writer's hash: nothing to read
    memcpy(r.sha, sha, 32);
    r.crc = crc, r.quick = 0;
    r.flags &= (uint16_t)~fUnhashed;
    g_pub_now = true;
  } else {
    memset(r.sha, 0, 32);
    r.crc = r.quick = 0;
    r.flags |= fUnhashed;
    if (prio) prio_add((uint32_t)i);
  }
  log_rec((uint32_t)i);
  dirty();
}

// ---- writes: the log's lines, the state, the compaction (only when the rules allow writes)
bool log_flush(uint32_t piece) {
  if (!g_loglen || g_readonly) return false;
  File f;
  Fs fs;
  if (!g_log_head) {   // a log without its header (none yet, or a damaged one): written whole first
    if (!fs.exists(kDir)) fs.mkdir(kDir);
    if (!fmt::write_atomic(kLog, kLogHead, sizeof kLogHead - 1)) {   // the lines stay in RAM; a compaction later
      logf("cannot write %s", kLog);
      write_failed();
      g_log_lost = true;
      return true;
    }
    g_step_wr += sizeof kLogHead - 1;
    g_log_head = true, g_log_torn = false;
    g_lines_card = 0;
    return true;
  }
  if (g_log_torn) {   // (a cut mid-line before: a newline first, so the next line is whole)
    if (!f.open(kLog, kAppend) || f.write("\n", 1) != 1) {
      f.close();
      write_failed();
      g_log_lost = true;
      return true;
    }
    f.close();
    g_log_torn = false;
    return true;
  }
  uint32_t n = g_loglen < piece ? g_loglen : piece;
  while (n < g_loglen && n && g_log[n - 1] != '\n') --n;   // whole lines
  if (!n) n = g_loglen;
  if (!f.open(kLog, kAppend)) {
    logf("cannot append to %s", kLog);
    write_failed();
    g_log_lost = true;
    return true;
  }
  bool ok = f.write(g_log, n) == n;
  ok = f.close() && ok;
  g_step_wr += n;
  if (!ok) {   // part of it may be on the card: never appended again (a compaction writes it all)
    write_failed();
    g_log_lost = g_log_torn = true;
    g_loglen = 0, g_lines_pending = 0;
    return true;
  }
  uint32_t lines = 0;
  for (uint32_t k = 0; k < n; ++k) lines += g_log[k] == '\n';
  memmove(g_log, g_log + n, g_loglen - n);
  g_loglen -= n;
  g_lines_pending -= lines < g_lines_pending ? lines : g_lines_pending;
  g_lines_card += lines;
  return true;
}
bool state_write() {
  if (g_readonly) return false;
  st_counts();
  Stats s = stats();
  char b[400];   // (tree=: 16 hex digits always: two 8-digit halves)
  int n = sniprintf(b, sizeof b,
                    "format=library-state/1\ntree=%08lx%08lx\nfiles=%lu\nbytes=%llu\nhashed=%lu\nunhashed=%lu\ngone=%lu\n"
                    "dups=%lu\nrecordings=%lu\n",
                    (unsigned long)(uint32_t)(g_tree_card >> 32), (unsigned long)(uint32_t)g_tree_card, (unsigned long)s.files,
                    (unsigned long long)s.bytes, (unsigned long)s.hashed, (unsigned long)s.unhashed,
                    (unsigned long)s.gone, (unsigned long)s.dups, (unsigned long)s.recordings);
  Fs fs;
  if (!fs.exists(kDir)) fs.mkdir(kDir);
  if (!fmt::write_atomic(kState, b, (uint32_t)n)) logf("cannot write %s", kState), write_failed();
  g_step_wr += (uint32_t)n;
  g_state_dirty = false;
  return true;
}
bool compact_wanted() {
  if (g_readonly || g_compacting) return g_compacting;
  if (g_lines_card + g_lines_pending >= (uint32_t)kCompactLines || g_log_lost) return true;
  // after the boot's work (the walk and the hashing queue empty), once, when the log has anything
  return g_walk_done && !g_compacted_boot && H.rec < 0 && !hash_wanted() && (g_lines_card + g_lines_pending) > 0;
}
bool compact_step(uint32_t piece) {
  if (!g_compacting) {
    Fs fs;
    if (!fs.exists(kDir)) fs.mkdir(kDir);
    delete[] g_cimg;
    g_cimg_len = encode(&g_cimg);
    g_loglen = 0, g_lines_pending = 0;   // (in the image)
    g_log_lost = false;
    g_aw.begin(kIndex, g_cimg, g_cimg_len, piece);
    g_compacting = true;
    return true;
  }
  bool more = g_aw.step();
  g_step_wr += g_aw.bytes_io();
  if (more) return true;
  g_compacting = false;
  delete[] g_cimg;
  g_cimg = nullptr;
  g_compacted_boot = true;
  if (!g_aw.ok()) {   // the lines dropped from RAM above are in no file now: another compaction later
    g_log_lost = true;
    write_failed();
    logf("cannot write %s (tried again later)", kIndex);
    return true;
  }
  ++g_compactions;
  for (uint32_t i = 0; i < W.n; ++i) {   // written once as gone: dropped at the next compaction
    if (W.r[i].flags & fGoneOld) w_del(i);
    else if (W.r[i].flags & fGone) W.r[i].flags |= fGoneOld;
  }
  if (fmt::write_atomic(kLog, kLogHead, sizeof kLogHead - 1)) g_log_head = true, g_log_torn = false, g_lines_card = 0;
  else g_log_head = false;
  g_failed = false;
  g_state_dirty = true;
  return true;
}

// ---- the boot load, a piece a step
bool load_step(uint32_t piece) {
  switch (g_load) {
    case L_RECOVER:
      fmt::recover(kIndex), fmt::recover(kLog), fmt::recover(kState);
      g_load = L_INDEX_OPEN;
      return true;
    case L_INDEX_OPEN: {
      if (!g_lf.open(kIndex, kRead) || g_lf.isDir()) {
        g_lf.close();
        g_load = L_LOG_OPEN;
        return true;
      }
      uint64_t n = g_lf.fileSize();
      if (n > (uint64_t)kMaxFiles * 300) {   // not an index this build can hold
        g_lf.close();
        logf("%s too big: rebuilt", kIndex);
        g_load = L_LOG_OPEN;
        return true;
      }
      g_img_len = (uint32_t)n, g_img_off = 0;
      delete[] g_img;
      g_img = new uint8_t[g_img_len + 1];
      g_load = L_INDEX_READ;
      return true;
    }
    case L_INDEX_READ: {
      uint32_t n = g_img_len - g_img_off < piece ? g_img_len - g_img_off : piece;
      if (n) {
        int got = g_lf.read(g_img + g_img_off, n);
        g_step_rd += got > 0 ? (uint32_t)got : 0;
        if (got != (int)n) g_img_len = g_img_off;   // (cut short: the check below says so)
        else g_img_off += n;
      }
      if (g_img_off < g_img_len) return true;
      g_lf.close();
      fmt::Info info;
      fmt::Bytes b = {g_img, g_img_len};
      fmt::Status st = fmt::load(*fmt::find("library"), b, nullptr, &info);
      if (st == fmt::kOk) {
        decode(g_img, g_img_len);
      } else if (st == fmt::kNewer || st == fmt::kForeignNewer) {
        g_readonly = true;   // (a foreign-newer index could be read, but its records may mean more: RAM only here)
        sniprintf(g_why, sizeof g_why, "index v%u: a newer Sidecar's (read-only here)", (unsigned)info.version);
        logf("%s: %s", kIndex, g_why);
      } else {
        logf("%s: %s: rebuilt (every file hashed again)", kIndex, info.why);
      }
      delete[] g_img;
      g_img = nullptr;
      g_load = L_LOG_OPEN;
      return true;
    }
    case L_LOG_OPEN:
      g_carry_n = 0, g_log_bad = false, g_lines_card = 0;
      g_log_head = false;
      if (g_readonly || !g_lf.open(kLog, kRead) || g_lf.isDir()) {
        g_lf.close();
        g_load = L_STATE;
        return true;
      }
      g_img_off = 0, g_img_len = (uint32_t)g_lf.fileSize();
      g_load = L_LOG_READ;
      return true;
    case L_LOG_READ: {
      static char buf[kPiece + sizeof g_carry + 1];
      uint32_t n = g_img_len - g_img_off < piece ? g_img_len - g_img_off : piece;
      memcpy(buf, g_carry, g_carry_n);
      int got = n ? g_lf.read(buf + g_carry_n, n) : 0;
      if (got < 0 || (got != (int)n && !g_log_head)) {   // the log there but not readable: never written over
        g_lf.close();
        g_readonly = true;
        sniprintf(g_why, sizeof g_why, "%s could not be read (read-only here)", kLog);
        logf("%s", g_why);
        g_load = L_STATE;
        return true;
      }
      g_step_rd += (uint32_t)got;
      g_img_off += n;
      bool end = g_img_off >= g_img_len || got != (int)n;
      uint32_t len = g_carry_n + (uint32_t)got, a = 0;
      buf[len] = 0;
      for (uint32_t k = 0; k < len; ++k) {
        if (buf[k] != '\n') continue;
        buf[k] = 0;
        if (k > a && buf[k - 1] == '\r') buf[k - 1] = 0;
        const char* l = buf + a;
        if (!g_log_head) {
          if (strcmp(l, "#sidecar library-log 1")) {
            g_log_bad = true;
            fmt::Sniffed sn;
            fmt::Bytes hb = {(const uint8_t*)l, (uint32_t)strlen(l)};
            if (fmt::text_header(hb, "library-log", &sn) && sn.version > 1) {   // a newer build's: never written over
              g_readonly = true;
              sniprintf(g_why, sizeof g_why, "log v%u: a newer Sidecar's (read-only here)", (unsigned)sn.version);
              logf("%s: %s", kLog, g_why);
            }
          }
          g_log_head = true;
        } else if (!g_log_bad) {
          replay(l);
          ++g_lines_card;
        }
        a = k + 1;
      }
      g_carry_n = len - a;
      if (g_carry_n > sizeof g_carry - 1) g_carry_n = 0;   // a line too long: dropped
      memmove(g_carry, buf + a, g_carry_n);
      if (!end) return true;
      g_lf.close();
      if (g_carry_n && !g_log_bad) g_log_torn = true;   // its last line cut short (a power cut): dropped, a newline next
      if (g_log_bad) {
        logf("%s is not a log this build reads: started again", kLog);
        g_log_head = false, g_lines_card = 0;
      }
      g_load = L_STATE;
      return true;
    }
    case L_STATE: {
      g_tree_card = 0;
      File f;
      char b[512];
      if (f.open(kState, kRead)) {
        int n = f.read(b, sizeof b - 1);
        f.close();
        b[n > 0 ? n : 0] = 0;
        const char* t = strstr(b, "\ntree=");
        if (t) {
          t += 6;
          g_tree_card = parse_u(t, 16);
        }
      }
      g_load = L_DONE;
      g_phase = kWalking;   // (the writers' reports apply directly from here)
      for (int k = 0; k < g_nearly; ++k) {
        const Early& e = g_early[k];
        if (e.op == 'm') moved(e.a, e.b);
        else touched_core(e.a, e.has_sha ? e.sha : nullptr, e.crc, false);
      }
      g_nearly = 0;
      if (g_early_lost) {   // more reports than kept while loading: every file hashed again (never a stale hash)
        g_early_lost = false;
        logf("writers' reports lost during the boot load: verifying every file");
        verify();
      }
      g_pub_now = true, dirty();
      publish();   // (the readers have the index: resolve works while the walk runs)
      walk_begin();
      return true;
    }
    default:
      return false;
  }
}

// ---- the bench (lib=bench): sha256 and CRC-32 from RAM, then the card's reads of the biggest file
constexpr uint32_t kBenchRam = 262144;
bool bench_step(uint32_t piece) {
  if (!g_bbuf) {
    g_bbuf = new uint8_t[kBenchRam];
    for (uint32_t k = 0; k < kBenchRam; ++k) g_bbuf[k] = (uint8_t)(k * 2654435761u >> 13);
  }
  if (g_bench == 1 || g_bench == 2) {
    uint32_t c0 = cycles();
    if (g_bench == 1) {
      uint8_t d[32];
      sha256(g_bbuf, kBenchRam, d);
    } else {
      volatile uint32_t c = crc32(0, g_bbuf, kBenchRam);
      (void)c;
    }
    g_bench_ns += (uint64_t)(uint32_t)(cycles() - c0) * 1000000000ull / cycles_per_second();
    g_bench_bytes += kBenchRam;
    if (++g_bench_i < 32) return true;
    uint32_t v = g_bench_ns ? (uint32_t)((double)g_bench_bytes / 1048576.0 / ((double)g_bench_ns / 1e9) * 100) : 1;
    (g_bench == 1 ? g_bres.sha_mbs100 : g_bres.crc_mbs100) = v ? v : 1;
    g_bench_ns = g_bench_bytes = 0, g_bench_i = 0;
    ++g_bench;
    return true;
  }
  if (g_bench == 3) {
    if (!g_bf.isOpen()) {   // the biggest hashed file
      int best = -1;
      for (uint32_t i = 0; i < W.n; ++i)
        if (live(W.r[i]) && (best < 0 || W.r[i].size > W.r[best].size)) best = (int)i;
      if (best < 0 || !g_bf.open(wpath(best), kRead)) {
        g_bench = 0, g_bres.running = false;
        logf("bench: sha256 %lu.%02lu MB/s, crc32 %lu.%02lu MB/s, no file to read", (unsigned long)(g_bres.sha_mbs100 / 100),
             (unsigned long)(g_bres.sha_mbs100 % 100), (unsigned long)(g_bres.crc_mbs100 / 100), (unsigned long)(g_bres.crc_mbs100 % 100));
        return true;
      }
      return true;
    }
    static uint8_t buf[kPiece];
    uint32_t c0 = cycles();
    int got = g_bf.read(buf, piece);
    g_bench_ns += (uint64_t)(uint32_t)(cycles() - c0) * 1000000000ull / cycles_per_second();
    if (got > 0) g_bench_bytes += (uint32_t)got, g_step_rd += (uint32_t)got;
    if (got == (int)piece && g_bench_bytes < (16u << 20)) return true;
    g_bf.close();
    g_bres.read_mbs100 = g_bench_ns ? (uint32_t)((double)g_bench_bytes / 1048576.0 / ((double)g_bench_ns / 1e9) * 100) : 1;
    if (!g_bres.read_mbs100) g_bres.read_mbs100 = 1;
    logf("bench: sha256 %lu.%02lu MB/s, crc32 %lu.%02lu MB/s (RAM, one core); card reads %lu.%02lu MB/s (%lu KB in %lu KB pieces)",
         (unsigned long)(g_bres.sha_mbs100 / 100), (unsigned long)(g_bres.sha_mbs100 % 100),
         (unsigned long)(g_bres.crc_mbs100 / 100), (unsigned long)(g_bres.crc_mbs100 % 100),
         (unsigned long)(g_bres.read_mbs100 / 100), (unsigned long)(g_bres.read_mbs100 % 100),
         (unsigned long)(g_bench_bytes >> 10), (unsigned long)(piece >> 10));
    g_bench = 0, g_bres.running = false;
    return true;
  }
  return false;
}

}  // namespace

// ---- references (spec 6.6): parse_ref / format_ref are in ref.cpp (portable: the format registry's readers use them)
const char* state_name(int s) {
  static const char* const n[] = {"PENDING", "EXACT", "MOVED", "UNVERIFIED", "CHANGED", "MISSING"};
  return s >= 0 && s <= kMissing ? n[s] : "?";
}
const char* phase_name(int p) {
  static const char* const n[] = {"OFF", "LOADING", "WALKING", "HASHING", "READY", "FAILED"};
  return p >= 0 && p <= kFailed ? n[p] : "?";
}

namespace {
// leading folders two paths share (case ignored)
int shared(const char* a, const char* b) {
  int n = 0;
  for (size_t i = 0; a[i] && b[i] && (a[i] | 32) == (b[i] | 32); ++i)
    if (a[i] == '/') ++n;
  return n;
}
bool same_folder(const char* a, const char* b) {
  const char* sa = strrchr(a, '/');
  const char* sb = strrchr(b, '/');
  return sa && sb && sa - a == sb - b && !strncasecmp(a, b, (size_t)(sa - a));
}
void out_path(Resolved* o, const char* p, State st, uint32_t rec) {
  o->st = st, o->rec = rec;
  sniprintf(o->path, sizeof o->path, "%s", p ? p : "");
}
}  // namespace

State resolve(const Ref& r, Resolved* out) {
  Resolved tmp;
  Resolved* o = out ? out : &tmp;
  Snap* s = acquire();
  if (!s) {
    out_path(o, r.hint, kPending, ~0u);
    return kPending;
  }
  int h = r.hint[0] ? s_find_path(s, r.hint) : -1;
  if (r.has_sha) {
    if (h >= 0 && hashed(s->rec[h]) && !memcmp(s->rec[h].sha, r.sha, 32)) {
      out_path(o, spath(s, (uint32_t)h), kExact, (uint32_t)h);
    } else {
      int c;
      int f = s_find_sha(s, r.sha, &c);
      if (f >= 0) {   // MOVED: the copy in the hint's folder, else the nearest path, else the first by name
        int best = f, bs = -1;
        for (int k = f; k < f + c; ++k) {
          const char* p = spath(s, (uint32_t)k);
          int sc = same_folder(p, r.hint) ? 1000 : shared(p, r.hint);
          if (sc > bs || (sc == bs && strcasecmp(p, spath(s, (uint32_t)best)) < 0)) bs = sc, best = k;
        }
        out_path(o, spath(s, (uint32_t)best), kMoved, (uint32_t)best);
      } else if (h >= 0 && !hashed(s->rec[h])) {
        out_path(o, spath(s, (uint32_t)h), s->rec[h].size == r.size ? kUnverified : kChanged, (uint32_t)h);
      } else if (h >= 0) {
        out_path(o, spath(s, (uint32_t)h), kChanged, (uint32_t)h);
      } else {   // (with files still to hash the content may yet turn up: PENDING, played by its path)
        bool final = s->ready && !s->to_hash;
        out_path(o, final ? "" : r.hint, final ? kMissing : kPending, ~0u);
      }
    }
  } else if (h >= 0) {
    out_path(o, spath(s, (uint32_t)h), hashed(s->rec[h]) ? kExact : kUnverified, (uint32_t)h);
  } else {
    out_path(o, s->ready ? "" : r.hint, s->ready ? kMissing : kPending, ~0u);
  }
  release(s);
  return o->st;
}

bool ref_for_path(const char* path, Ref* out) {
  memset(out, 0, sizeof *out);
  sniprintf(out->hint, sizeof out->hint, "%s", path ? path : "");
  Snap* s = acquire();
  int h = s && path ? s_find_path(s, path) : -1;
  bool known = h >= 0;
  if (known) {
    out->size = s->rec[h].size;
    if (hashed(s->rec[h])) memcpy(out->sha, s->rec[h].sha, 32), out->has_sha = true;
  }
  release(s);
  if (path && (!known || !out->has_sha)) hash_now(path);
  return known;
}

uint32_t gen() { return __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE); }

int find(const uint8_t* sha, const char** paths, int max) {
  Snap* s = acquire();
  if (!s) return 0;
  int c, n = 0;
  int f = s_find_sha(s, sha, &c);
  for (int k = 0; f >= 0 && k < c && n < max; ++k) paths[n++] = spath(s, (uint32_t)(f + k));
  release(s);
  return n;
}
bool find_file(const uint8_t* sha, uint64_t size, char* path, int cap, const char* near) {
  Snap* s = acquire();
  if (!s) return false;
  int c;
  int f = s_find_sha(s, sha, &c);
  int best = -1, bs = -1;
  for (int k = f; f >= 0 && k < f + c; ++k) {
    if (s->rec[k].size != size || strlen(spath(s, (uint32_t)k)) >= (size_t)cap) continue;
    const char* p = spath(s, (uint32_t)k);
    int sc = !near ? 0 : same_folder(p, near) ? 1000 : shared(p, near);
    if (sc > bs || (sc == bs && strcasecmp(p, spath(s, (uint32_t)best)) < 0)) bs = sc, best = k;
  }
  if (best >= 0) sniprintf(path, cap, "%s", spath(s, (uint32_t)best));
  release(s);
  return best >= 0;
}

bool hashes_of(const char* path, uint64_t size, uint8_t sha[32], uint32_t* crc) {
  Snap* s = acquire();
  int h = s && path ? s_find_path(s, path) : -1;
  bool ok = h >= 0 && hashed(s->rec[h]) && live(s->rec[h]) && !(s->rec[h].flags & fUnreadable) && s->rec[h].size == size &&
            s->rec[h].crc;
  if (ok) memcpy(sha, s->rec[h].sha, 32), *crc = s->rec[h].crc;
  release(s);
  return ok;
}

void touched(const char* path, const uint8_t* sha, uint32_t crc) { touched_core(path, sha, crc, false); }

// the folder's key: "/" the root, else "/A/B/" (false: too long)
static bool folder_key(const char* dir, char* k, size_t* kl) {
  size_t l = dir ? strlen(dir) : 0;
  while (l > 0 && dir[l - 1] == '/') --l;
  if (l > kFolderMax) return false;
  if (l) memcpy(k, dir, l);
  k[l] = '/', k[l + 1] = 0;
  *kl = l + 1;
  return true;
}
int list(const char* dir, ListFn fn, void* ctx, ListPos* pos, uint32_t budget) {
  Snap* s = acquire();
  if (s && !s->loaded) release(s), s = nullptr;   // (the boot load's own: not until it is whole)
  if (!s) return -1;
  if (pos && pos->at && pos->gen != s->gen) {   // a listing in pieces, the snapshot changed since: from the start
    release(s);
    return -2;
  }
  char pre[kPath + 2], key[kPath + 2], name[kPath];
  size_t lp;
  int n = 0;
  if ((dir && dir[0] && dir[0] != '/') || !folder_key(dir, pre, &lp)) {
    if (pos) pos->gen = s->gen, pos->at = 0, pos->done = true;
    release(s);
    return 0;
  }
  uint32_t i = pos && pos->at ? pos->at : b_lower(s, pre, 0), seen = 0;
  bool done = true;
  while (i < s->nb) {
    if (budget && seen >= budget) {   // (the caller's piece: on from here at its next call)
      done = false;
      break;
    }
    ++seen;
    uint32_t e = s->byp[i];
    const char* p = bpath(s, e);
    if (!ci_pre(p, pre, lp)) break;
    const char* r = p + lp;
    const char* sl = strchr(r, '/');
    if (!r[0] || sl == r) {   // (the folder's own entry)
      ++i;
      continue;
    }
    bool go;
    if (sl) {   // a subfolder: once, then past everything under it ("<pre><name>0": '0' follows '/')
      size_t nl = (size_t)(sl - r);
      memcpy(name, r, nl), name[nl] = 0;
      ++n;
      go = fn(ctx, name, true, 0);
      memcpy(key, p, lp + nl), key[lp + nl] = '0', key[lp + nl + 1] = 0;
      i = b_lower(s, key, i + 1);
    } else {
      ++n;
      go = fn(ctx, r, false, (e & kFoldBit) ? 0 : s->rec[e].size);
      ++i;
    }
    if (!go) break;
  }
  if (pos) pos->gen = s->gen, pos->at = done ? 0 : i, pos->done = done;
  release(s);
  return n;
}
int is_folder(const char* dir) {
  Snap* s = acquire();
  if (s && !s->loaded) release(s), s = nullptr;
  if (!s) return -1;
  char pre[kPath + 2];
  size_t lp;
  int r = 0;
  if (!dir || !dir[0] || !strcmp(dir, "/")) r = 1;
  else if (folder_key(dir, pre, &lp)) {
    uint32_t i = b_lower(s, pre, 0);
    r = i < s->nb && ci_pre(bpath(s, s->byp[i]), pre, lp);
  }
  release(s);
  return r;
}

void moved(const char* from, const char* to) {
  if (!g_card || !from || !to) return;
  if ((in_scope(from) || in_scope(to)) && early('m', from, to, nullptr, 0)) return;
  bool a = in_scope(from), b = in_scope(to);
  if (a && b) {
    if (f_moved(from, to)) dirty();
    rename_prefix(from, to, true);
    if (w_find(to) < 0 && kind_of(to)) touched_core(to, nullptr, 0, false);   // (a file renamed into a library type)
    return;
  }
  if (a) {   // out of the library's scope: gone
    if (g_phase == kWalking) g_walk_req = true;
    if (f_moved(from, nullptr)) dirty();
    size_t fl = strlen(from);
    for (uint32_t k = 0; k < W.n; ++k)
      if (live(W.r[k]) && (!strcasecmp(wpath(k), from) || (under(wpath(k), from) && fl))) {
        index_changed_at(k);
        W.r[k].flags |= fGone;
        log_gone(wpath(k));
        dirty();
      }
    return;
  }
  if (b) {   // into it: a file is looked at now, a folder by a walk
    File f;
    bool dir = f.open(to, kRead) && f.isDir();
    f.close();
    if (dir) {
      g_walk_req = true;
      if (f_made(to)) dirty();
    } else touched_core(to, nullptr, 0, false);
  }
}

void hash_now(const char* path) {
  if (!path || path[0] != '/' || strlen(path) >= (size_t)kPath) return;
  for (;;) {
    uint32_t h = __atomic_load_n(&g_rhead, __ATOMIC_ACQUIRE);
    if (h - __atomic_load_n(&g_rtail, __ATOMIC_ACQUIRE) >= kRing) {
      __atomic_fetch_add(&g_ring_dropped, 1, __ATOMIC_RELAXED);
      return;
    }
    if (__atomic_compare_exchange_n(&g_rhead, &h, h + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      Slot& s = g_ring[h % kRing];
      sniprintf(s.path, sizeof s.path, "%s", path);
      __atomic_store_n(&s.seq, h + 1, __ATOMIC_RELEASE);
      return;
    }
  }
}

Stats stats() {
  Stats s;
  for (int t = 0; t < 1000; ++t) {
    uint32_t a = __atomic_load_n(&g_st_seq, __ATOMIC_ACQUIRE);
    if (a & 1) continue;
    memcpy(&s, &g_st, sizeof s);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_st_seq, __ATOMIC_ACQUIRE) == a) return s;
  }
  memcpy(&s, &g_st, sizeof s);
  return s;
}

void set_env(const Env& e) { g_env = e; }

void begin(bool card_ok) {
  g_card = card_ok;
  g_load = L_RECOVER;
  g_phase = card_ok ? kLoading : kOff;
  g_walk_done = false;
  st_misc();
}

bool busy() {
  if (!g_card || g_phase == kOff) return false;
  if (g_phase == kLoading || g_phase == kWalking || g_bench || g_compacting) return true;
  if (hash_wanted() || g_walk_req) return true;
  if (g_dirty_pub) return true;
  if (!g_readonly && !resting() && (g_loglen || g_state_dirty || compact_wanted())) return true;
  return false;
}
void rescan() { g_walk_req = true; }
void refs_changed() { g_refs_dirty = true; }
void verify() {
  for (uint32_t i = 0; i < W.n; ++i)
    if (live(W.r[i]) && hashed(W.r[i])) W.r[i].flags |= fVerify;
  g_verify_diff = g_verify_done = 0;
  g_verifying = true;
  g_cursor = 0;
  dirty();
}
Bench bench() { return g_bres; }
const Work& work() { return g_work; }

bool step() {
  if (!g_card || g_phase == kOff) return false;
  IoRule r = {kPiece, kPiece, 0, ""};
  if (g_env.rules) g_env.rules(&r);
  const char* why = nullptr;
  if (g_env.card_busy && g_env.card_busy()) why = "STREAMS";
  else if (g_env.other_job && g_env.other_job()) why = "BUNDLE";
  else if (!r.read_piece && !r.write_piece) why = r.why && r.why[0] ? r.why : "PAUSED";
  else if (r.read_gap_us && now_us() - g_last_us < r.read_gap_us) return false;
  if (why) {
    ++g_work.waits;
    if (strcmp(g_waiting, why)) sniprintf(g_waiting, sizeof g_waiting, "%s", why), st_misc();
    return false;
  }
  bool was_waiting = false;
  if (!r.write_piece && r.why && r.why[0]) {   // reads only: the waiting line still says why writes wait
    if (strcmp(g_waiting, r.why)) sniprintf(g_waiting, sizeof g_waiting, "%s", r.why), was_waiting = true;
  } else if (g_waiting[0]) {
    g_waiting[0] = 0, was_waiting = true;
  }
  uint32_t rp = r.read_piece < kPiece ? r.read_piece : kPiece, wp = r.write_piece < kPiece ? r.write_piece : kPiece;
  g_last_us = now_us();
  g_step_rd = g_step_wr = 0;
  uint32_t c0 = cycles();
  bool did = false;
  if (g_phase == kLoading) {
    did = rp && load_step(rp);
  } else if (g_compacting && wp) {
    did = compact_step(wp);
  } else if (g_phase == kWalking) {
    if (g_walk_req) g_walk_req = false, walk_begin();   // (a folder moved under it: from the start)
    did = rp && walk_step(rp >= kPiece ? 32 : 8);
    if (g_walk_done && g_phase == kWalking) g_phase = kHashing;
    if (!did) did = true;
  } else {
    if (g_walk_req && H.rec < 0 && !g_compacting) {
      g_walk_req = false;
      walk_begin();
      did = true;
    } else if (resting()) {
      if (g_bench && rp) did = bench_step(rp);
      else if (rp && g_loglen + 1024 < kLogCap) did = hash_step(rp);
    } else if (wp && g_loglen && !g_compacting && (g_loglen > kLogCap / 2 || !hash_wanted() || g_lines_pending >= 64)) {
      did = log_flush(wp);
    } else if (wp && compact_wanted()) {
      did = compact_step(wp);
    } else if (wp && g_state_dirty && !hash_wanted()) {
      did = state_write();
    } else if (g_bench && rp) {
      did = bench_step(rp);
    } else if (rp && g_loglen + 1024 < kLogCap && hash_step(rp)) {
      did = true;
    }
    if (g_phase != kWalking) g_phase = hash_wanted() || g_walk_req ? kHashing : kReady;
  }
  // publish: at once for a pick or a writer's file, else at most once a second
  // a publish copies and sorts the whole index (O(n log n), one step): at once for a pick, a writer's file or when the
  // work is done (on a big index not more than 4 a second), else at most once a second, once every n / 20 000 s on a
  // big index
  uint32_t gap = W.n / 20 > 1000 ? W.n / 20 : 1000, since = now_ms() - g_last_pub_ms;
  bool soon = g_pub_now || (!hash_wanted() && g_phase != kWalking);
  // (never during the boot load: a log line's change - a '>' move - would publish part of the index; L_STATE does)
  if (g_dirty_pub && g_phase != kLoading && ((soon && (W.n < 20000 || since >= 250)) || since >= gap)) did |= publish();
  uint32_t us = (uint32_t)((uint64_t)(uint32_t)(cycles() - c0) * 1000000ull / cycles_per_second());
  if (did) {
    ++g_work.steps;
    if (g_step_rd > g_work.max_read) g_work.max_read = g_step_rd;
    if (g_step_wr > g_work.max_write) g_work.max_write = g_step_wr;
    if (us > g_work.max_us) g_work.max_us = us;
    g_work.read += g_step_rd, g_work.written += g_step_wr;
  }
  if (g_verifying && !hash_wanted()) {
    g_verifying = false;
    logf("verify: %lu files hashed again, %lu differ", (unsigned long)g_verify_done, (unsigned long)g_verify_diff);
  }
  (void)was_waiting;
  st_misc();
  return did;
}

bool command(const char* line, char* out, int cap) {
  if (strncmp(line, "lib=", 4)) return false;
  const char* a = line + 4;
  if (!strcmp(a, "status") || !strcmp(a, "")) {
    Stats s = stats();
    sniprintf(out, cap,
              "library: %s, %lu files (%llu MB), %lu hashed (%u%%), %lu pending, %lu duplicates (%llu MB), %lu missing, "
              "%lu unreadable, %lu recordings indexed; log %lu lines (%lu waiting), %lu compactions%s%s%s%s; verify: %lu differ",
              phase_name(s.state), (unsigned long)s.files, (unsigned long long)(s.bytes >> 20), (unsigned long)s.hashed,
              (unsigned)s.pct, (unsigned long)s.unhashed, (unsigned long)s.dups, (unsigned long long)(s.dup_bytes >> 20),
              (unsigned long)s.gone, (unsigned long)s.unreadable, (unsigned long)s.recordings, (unsigned long)s.log_lines,
              (unsigned long)s.pending_log, (unsigned long)s.compactions, s.waiting[0] ? "; waiting: " : "", s.waiting,
              g_readonly ? "; " : "", g_readonly ? g_why : "", (unsigned long)g_verify_diff);
    return true;
  }
  if (!strcmp(a, "rescan")) {
    rescan();
    sniprintf(out, cap, "library: the card walked again");
    return true;
  }
  if (!strcmp(a, "verify")) {
    verify();
    sniprintf(out, cap, "library: every hashed file hashed again (lib=status: the differences)");
    return true;
  }
  if (!strcmp(a, "bench")) {
    g_bench = 1, g_bench_i = 0, g_bench_ns = g_bench_bytes = 0;
    g_bres.running = true;
    g_bres.sha_mbs100 = g_bres.crc_mbs100 = g_bres.read_mbs100 = 0;
    sniprintf(out, cap, "library: bench started (the figures in the log)");
    return true;
  }
  if (!strncmp(a, "find:", 5)) {
    uint8_t sha[32];
    if (!unhex(a + 5, sha, 32)) {
      sniprintf(out, cap, "library: lib=find:<64 hex digits>");
      return true;
    }
    const char* p[8];
    int n = find(sha, p, 8);
    int k = sniprintf(out, cap, "library: %d file%s", n, n == 1 ? "" : "s");
    for (int i = 0; i < n && k > 0 && k < cap; ++i) k += sniprintf(out + k, cap - k, "%s%s", i ? ", " : ": ", p[i]);
    return true;
  }
  if (!strncmp(a, "hash:", 5)) {
    hash_now(a + 5);
    sniprintf(out, cap, "library: %s hashed next", a + 5);
    return true;
  }
  if (!strchr(a, ':') && strcmp(a, "gr") && strcmp(a, "wt") && strcmp(a, "ms")) {
    sniprintf(out, cap, "library: lib=status|rescan|verify|bench|find:<sha256>|hash:<path>");
    return true;
  }
  return false;
}

int status_json(char* out, int cap) {
  Stats s = stats();
  return sniprintf(out, cap,
                   ",\"library\":{\"state\":\"%s\",\"files\":%lu,\"bytes\":%llu,\"hashed\":%lu,\"pending\":%lu,\"pct\":%u,"
                   "\"dups\":%lu,\"dupBytes\":%llu,\"missing\":%lu,\"unreadable\":%lu,\"recordings\":%lu,\"waiting\":\"%s\","
                   "\"logLines\":%lu,\"readOnly\":%s}",
                   phase_name(s.state), (unsigned long)s.files, (unsigned long long)s.bytes, (unsigned long)s.hashed,
                   (unsigned long)s.unhashed, (unsigned)s.pct, (unsigned long)s.dups, (unsigned long long)s.dup_bytes,
                   (unsigned long)s.gone, (unsigned long)s.unreadable, (unsigned long)s.recordings, s.waiting,
                   (unsigned long)s.log_lines, g_readonly ? "true" : "false");
}

void reset_for_tests() {
  hash_abort();
  walk_close();
  g_lf.close();
  g_bf.close();
  g_aw.cancel();
  w_clear();
  f_clear(g_fcur), f_clear(g_fnext), g_fwalk = false;
  delete[] g_img, g_img = nullptr;
  delete[] g_cimg, g_cimg = nullptr;
  delete[] g_gone_sizes, g_gone_sizes = nullptr, g_ngone = g_gone_cap = 0;
  __atomic_store_n(&g_pub, (Snap*)nullptr, __ATOMIC_SEQ_CST);
  for (Snap& s : g_snap) {
    delete[] s.rec, delete[] s.paths, delete[] s.ht, delete[] s.byp;
    memset(&s, 0, sizeof s);
  }
  memset(&g_work, 0, sizeof g_work);
  memset(&g_st, 0, sizeof g_st);
  g_bres = Bench();
  g_bench = 0;
  g_card = false, g_phase = kOff, g_readonly = false, g_why[0] = 0;
  g_walk_done = g_walk_req = g_compacted_boot = g_dirty_pub = g_pub_now = false;
  g_state_dirty = false, g_compacting = false, g_verifying = false;
  g_loglen = g_lines_pending = g_lines_card = 0, g_log_head = false;
  g_log_lost = false, g_nearly = 0, g_early_lost = false, g_log_torn = false, g_failed = false;
  g_nprio = g_nrefs = g_refs_i = 0, g_refs_dirty = true, g_cursor = 0;
  g_rhead = g_rtail = 0;
  for (Slot& s : g_ring) s.seq = 0;
  g_compactions = 0, g_verify_diff = g_verify_done = 0;
  g_waiting[0] = 0;
  g_tree = g_tree_card = 0;
  g_bench_i = 0, g_bench_ns = g_bench_bytes = 0;
  g_env = Env{nullptr, nullptr, nullptr, nullptr};
}

}  // namespace lib
}  // namespace core
#endif
