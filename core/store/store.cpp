// core/store/store.cpp - see store.h: the RAM store, a flat array of entries with linear lookup.
#include <string.h>

#include "../platform.h"
#include "store.h"
#include "../machines.h"
#if CORE_MSAMP
#include "../fmt/fmt.h"
#include "../msamp/cardq.h"
#endif

namespace core {
int prj_slot();   // (core/pool/pool.h: device::cur()'s voice slot)
namespace store {

namespace {
// One store per voice slot (each box's project its own, core/pool/pool.h): the slot in hand's (the control core's Scope);
// a slot's fill runs in its Scope (project_fill_step), so its requests read its box's folder.
inline int sl() {
  int s = prj_slot();
  return s >= 0 && s < kVoiceSlots ? s : 0;
}
struct Entry {
  char path[kPathMax];
  State state;
  uint8_t* data;
  uint32_t len;
};
Entry g_e[kVoiceSlots][kMaxEntries];
int g_n[kVoiceSlots];
uint32_t g_bytes[kVoiceSlots];

Entry* find(const char* path) {
  const int k = sl(), n = g_n[k];
  Entry* e = g_e[k];
  for (int i = 0; i < n; ++i)
    if (!strcmp(e[i].path, path)) return &e[i];
  return nullptr;
}
void drop_data(Entry* e) {
  delete[] e->data;
  g_bytes[sl()] -= e->len;
  e->data = nullptr;
  e->len = 0;
}
// the entry for path (made if new); null when the path is too long or the store is full
Entry* entry_for(const char* path) {
  if (!path || strlen(path) >= (size_t)kPathMax) return nullptr;
  if (Entry* e = find(path)) return e;
  if (g_n[sl()] >= kMaxEntries) return nullptr;
  Entry* e = &g_e[sl()][g_n[sl()]++];
  strcpy(e->path, path);
  e->state = kUnknown;
  e->data = nullptr;
  e->len = 0;
  return e;
}
void drop_all() {
  for (int i = 0; i < g_n[sl()]; ++i) delete[] g_e[sl()][i].data;
  g_n[sl()] = 0;
  g_bytes[sl()] = 0;
}
// p (n bytes, new[]'d) held for path, kPresent: the store owns p from here (deleted when it cannot be held)
bool adopt(const char* path, uint8_t* p, uint32_t n) {
  Entry* e = entry_for(path);
  if (!e) {
    delete[] p;
    log(CORE_STR("store: cannot hold %s (full or path too long)"), path ? path : "(null)");
    return false;
  }
  if (e->data) drop_data(e);
  e->data = p;
  e->len = n;
  g_bytes[sl()] += n;
  e->state = kPresent;
  return true;
}
#if CORE_MSAMP
int g_nw[kVoiceSlots], g_nf[kVoiceSlots];   // the fill's wanted paths and their folders (below)
// want() finds a repeat through this table, not by comparing the path with every one wanted before: a project load wants ~1000
// paths in one control pass, and that scan was 10 ms of it on the Pi (2026-11-20 black box; the FIFO holds 2 ms). Open
// addressing over g_want's indexes (+1, 0 empty), a power of two at least twice kMaxEntries, so a probe is short. One per slot.
constexpr int kWantTab = 4096;
static_assert(kWantTab >= 2 * kMaxEntries && (kWantTab & (kWantTab - 1)) == 0, "the want table: a power of two, half empty at most");
uint16_t g_wtab[kVoiceSlots][kWantTab];
uint32_t want_hash(const char* p) {   // FNV-1a
  uint32_t h = 2166136261u;
  for (; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
  return h;
}
void abandon();
#endif
}  // namespace

CORE_COLD void clear() {
#if CORE_MSAMP
  abandon();
  g_nw[sl()] = 0;
  g_nf[sl()] = 0;
  memset(g_wtab[sl()], 0, sizeof g_wtab[sl()]);
#endif
  drop_all();
}

CORE_COLD bool put(const char* path, const uint8_t* d, uint32_t n) {
  Entry* e = entry_for(path);
  if (!e) {
    log(CORE_STR("store: cannot hold %s (full or path too long)"), path ? path : "(null)");
    return false;
  }
  if (e->data) drop_data(e);
  // plain new as the rest of core (the Pi's kernel has no std::nothrow). On the Pi this check is not what saves us:
  // Circle's operator new takes the low heap (HEAP_DEFAULT_NEW), and the first allocation that would reach into that
  // heap's last 256 KB (its reserve) logs "Out of memory" as a panic, which halts the system, before any new returns 0
  // (lib/heapallocator.cpp, STDLIB_SUPPORT 1). The host's new throws. So the store relies on its texts being small (KB
  // each, a few hundred paths) against hundreds of MB of heap - /status "store" heapFree shows the room (kernel.cpp)
  uint8_t* p = new uint8_t[n ? n : 1];
  if (!p) {
    e->state = kLocked;
    log(CORE_STR("store: out of memory for %s (%u bytes): locked"), path, (unsigned)n);
    return false;
  }
  if (n) memcpy(p, d, n);
  return adopt(path, p, n);
}

CORE_COLD void mark(const char* path, State s) {
  if (s != kAbsent && s != kLocked) return;
  Entry* e = entry_for(path);
  if (!e) {
    log(CORE_STR("store: cannot mark %s (full or path too long)"), path ? path : "(null)");
    return;
  }
  if (e->data) drop_data(e);
  e->state = s;
}

State get(const char* path, const uint8_t** d, uint32_t* n) {
  const Entry* e = path ? find(path) : nullptr;
  if (!e) {
    *d = nullptr;
    *n = 0;
    return kUnknown;
  }
  *d = e->state == kPresent ? e->data : nullptr;
  *n = e->state == kPresent ? e->len : 0;
  return e->state;
}

uint32_t bytes() { return g_bytes[sl()]; }
int entries() { return g_n[sl()]; }
int present() { return present(sl()); }
uint32_t bytes(int s) { return s >= 0 && s < kVoiceSlots ? g_bytes[s] : 0; }
int present(int s) {
  if (s < 0 || s >= kVoiceSlots) return 0;
  int k = 0;
  for (int i = 0; i < g_n[s]; ++i) k += g_e[s][i].state == kPresent;
  return k;
}

#if CORE_MSAMP
// ---- the fill: one path at a time, each card operation a cardq::async request (the card core runs it; without one,
// at once). The read buffer is the fill's until its bytes are adopted by the store (no second copy).
//
// Ruling 2 (2026-10-26): the paths go folder by folder, and each folder is listed once (cardq kListReq) before its
// paths: a path not in the listing is absent with no card operation of its own, a listed one is opened and read as
// before (one that will not open is locked: the listing said it is there). A folder that is not there makes all of its
// paths absent; one that could not be listed (cardq::kListUnread: not opened, or its names over kListMax) has its paths
// asked one by one as before (open, then exists). Why: the boot's fill of 258 paths took 7.3 s on the device, nearly
// all of them absent per-kit files, each a failed open plus an exists through the card queue.
//
// A write of a path pending when its folder is listed (or when the path's turn comes) may land after the listing:
// such a path is never taken for absent from the listing - once the write has landed it is opened as before. A write
// posted after the listing is the filling project's own (its writers put their bytes into the store first: written()),
// so it never needs the listing either.
namespace {
constexpr int kMaxFolders = 16;      // distinct folders listed (a project's files are in five); more: asked per path
constexpr uint32_t kListMax = 8192;  // a folder's names (a project's folder: 128 x "kNNN.txt" = 1152 bytes)
char g_want[kVoiceSlots][kMaxEntries][kPathMax];   // the paths the coming fill reads (g_nw[sl()] of them)
int16_t g_wfold[kVoiceSlots][kMaxEntries];         // each wanted path's folder (g_fold[sl()]), -1 none (asked per path)
int16_t g_order[kVoiceSlots][kMaxEntries];         // the order the fill reads them in: folder by folder
bool g_nolist[kVoiceSlots][kMaxEntries];           // a write of it was pending at its folder's listing (or since): opened, not listed
char g_fold[kVoiceSlots][kMaxFolders][kPathMax];
uint8_t g_list[kVoiceSlots][kListMax];   // the listed folder's names, each NUL-ended (cardq kListReq)
enum Phase : uint8_t { kOpening, kReading, kClosing, kAsking, kListing };
struct Fill {
  cardq::Req rq;
  File f;
  bool running;   // a fill_begin()'s fill not yet complete
  bool done;      // ready()
  bool drain;     // an abandoned fill's leftovers (a request still on the card core, an open file) to finish first
  bool wait_rq;   //   its request was on the card core: wait until the card core has finished it
  bool open;      // f is open (an open request came back 1)
  bool again;     // a write of the path was queued while it was read: what was read dropped, the path read anew
  bool there;     // the path is in its folder's listing: an open that fails means locked (no exists asked)
  Phase ph;
  int at;         // the place in g_order[sl()] of the wanted path being read
  int listed;     // the folder g_list[sl()] holds (-1 none)
  int32_t list_n; // its listing's bytes, or cardq::kListMissing / kListUnread
  uint8_t* buf;   // the read's buffer (the whole file)
  uint32_t size;
  int wait_at;            // final review: the place in g_order[sl()] whose pending write the fill waits for (-1 none),
  uint32_t wait_ms, said_ms;   //   since when, and when that was last logged
} g_f[kVoiceSlots];
inline Fill& F() { return g_f[sl()]; }   // the slot in hand's fill
constexpr uint32_t kWaitSayMs = 2000;     // a fill waiting this long for one path's write says so (a normal one: a pass)
constexpr uint32_t kWaitAgainMs = 10000;  // and again at most this often, as long as it waits
// the fill waits for the path's pending write (fmt::pending): a write behind a card core held up (a held slot, a stalled
// card) never lands and the fill never completes - one line now and then says why, rather than a load that never ends
void waiting(const char* path) {
  uint32_t ms = now_ms();
  if (F().wait_at != F().at) F().wait_at = F().at, F().wait_ms = ms, F().said_ms = ms - kWaitAgainMs;
  if (ms - F().wait_ms < kWaitSayMs || ms - F().said_ms < kWaitAgainMs) return;
  F().said_ms = ms;
  log(CORE_STR("store: fill waiting for %s's write (%u ms; the card core has not written it yet)"), path,
      (unsigned)(ms - F().wait_ms));
}

// the folder index of path's parent (made if new); -1 when it has none or there are kMaxFolders already
int folder_of(const char* path) {
  const char* sep = strrchr(path, '/');
  if (!sep || sep == path) return -1;
  size_t n = (size_t)(sep - path);
  for (int i = 0; i < g_nf[sl()]; ++i)
    if (strlen(g_fold[sl()][i]) == n && !strncmp(g_fold[sl()][i], path, n)) return i;
  if (g_nf[sl()] >= kMaxFolders) return -1;
  memcpy(g_fold[sl()][g_nf[sl()]], path, n);
  g_fold[sl()][g_nf[sl()]][n] = 0;
  return g_nf[sl()]++;
}
// FAT names compare without case (a file copied onto the card by hand may be P000.SQP)
bool same_name(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
    if (x != y) return false;
    if (!x) return true;
  }
}
// the path's name is in g_list[sl()] (the listing of its folder)
bool listed_name(const char* path) {
  const char* name = strrchr(path, '/') + 1;
  for (uint32_t at = 0; at < (uint32_t)F().list_n;) {
    const char* e = (const char*)g_list[sl()] + at;
    if (same_name(e, name)) return true;
    at += (uint32_t)strlen(e) + 1;
  }
  return false;
}
bool asked(const cardq::Req& r) {   // a request of the current phase is posted, running, or done and not yet taken
  return r.state == cardq::kPosted || r.state == cardq::kRunning || r.state == cardq::kDone;
}

// an abandoned fill's request the card core finished: what it did to the file (an open opened it, a close closed it)
void take_late() {
  if (F().rq.kind == cardq::kOpenReq && F().rq.got == 1) F().open = true;
  if (F().rq.kind == cardq::kCloseReq) F().open = false;
  F().rq.state = cardq::kFree;
}
// the fill dropped where it is: what is the card core's already is let finish (fill_step drains it), the rest freed now
CORE_COLD void abandon() {
  F().running = false;
  F().done = false;
  uint32_t s = F().rq.state;
  if (s == cardq::kPosted && cardq::cancel(&F().rq)) {
    // never ran: the card core skips it (an open that never ran opened nothing)
  } else if (s == cardq::kPosted || s == cardq::kRunning) {
    F().wait_rq = true;   // on the card core now: its buffer, g_list[sl()] and file are not ours until it is done
  } else if (s == cardq::kDone) {
    take_late();   // done on the card core but not yet taken here
  }
  if (!F().wait_rq) {
    delete[] F().buf;
    F().buf = nullptr;
  }
  F().drain = F().wait_rq || F().open || F().buf;
}
// one step of the leftovers; true when there are none
CORE_COLD bool drain_step() {
  if (F().wait_rq) {
    if (!cardq::idle(&F().rq)) return false;   // the card core still has it
    if (F().rq.state == cardq::kDone) take_late();
    F().rq.state = cardq::kFree;
    F().wait_rq = false;
    delete[] F().buf;
    F().buf = nullptr;
  }
  if (F().open) {
    if (!cardq::async(&F().rq, cardq::kCloseReq, &F().f, nullptr, 0, nullptr, 0)) return false;
    F().open = false;
  }
  F().drain = false;
  return true;
}
// 2026-10-26 (Task 4): an entry for a wanted path the fill has not held yet is a save's write-through (put) since
// fill_begin - newer than the card's bytes, so the fill keeps it and drops what it read (fill_begin empties the store)
bool written(const char* path) { return find(path) != nullptr; }
// the path read: the next one; true when that was the last
bool next() {
  F().ph = kOpening;
  F().there = false;
  if (++F().at < g_nw[sl()]) return false;
  F().running = false;
  F().done = true;
  return true;
}
}  // namespace

CORE_COLD void want(const char* path) {
  if (!path || strlen(path) >= (size_t)kPathMax) {
    log(CORE_STR("store: cannot want %s (path too long)"), path ? path : "(null)");
    return;
  }
  uint16_t* const tab = g_wtab[sl()];
  uint32_t h = want_hash(path) & (kWantTab - 1);
  for (; tab[h]; h = (h + 1) & (kWantTab - 1))
    if (!strcmp(g_want[sl()][tab[h] - 1], path)) return;
  if (g_nw[sl()] >= kMaxEntries) {
    log(CORE_STR("store: cannot want %s (%d paths wanted already)"), path, g_nw[sl()]);
    return;
  }
  g_wfold[sl()][g_nw[sl()]] = (int16_t)folder_of(path);
  strcpy(g_want[sl()][g_nw[sl()]++], path);
  tab[h] = (uint16_t)g_nw[sl()];   // (the index + 1)
}

CORE_COLD void fill_begin() {
  abandon();
  drop_all();
  F().running = true;
  F().again = false;
  F().there = false;
  F().at = 0;
  F().wait_at = -1;
  F().ph = kOpening;
  F().listed = -1;
  // folder by folder (each listed once), in the order the folders were first wanted; the paths with none last
  int k = 0;
  for (int f = 0; f < g_nf[sl()]; ++f)
    for (int i = 0; i < g_nw[sl()]; ++i)
      if (g_wfold[sl()][i] == f) g_order[sl()][k++] = (int16_t)i;
  for (int i = 0; i < g_nw[sl()]; ++i)
    if (g_wfold[sl()][i] < 0) g_order[sl()][k++] = (int16_t)i;
  memset(g_nolist[sl()], 0, sizeof g_nolist[sl()]);
  if (!g_nw[sl()]) F().running = false, F().done = true;   // nothing wanted: complete at once
}

CORE_COLD bool fill_step() {
  if (F().drain && !drain_step()) return false;
  if (!F().running) return F().done;
  int wi = g_order[sl()][F().at];
  const char* path = g_want[sl()][wi];
  int fold = g_wfold[sl()][wi];
  switch (F().ph) {
    case kListing:
      if (!asked(F().rq)) {   // (the listing not yet asked for) the folder's paths with a write on its way: not listed
        for (int i = 0; i < g_nw[sl()]; ++i)
          if (g_wfold[sl()][i] == fold && fmt::pending(g_want[sl()][i])) g_nolist[sl()][i] = true;
      }
      if (!cardq::async(&F().rq, cardq::kListReq, nullptr, g_fold[sl()][fold], 0, g_list[sl()], kListMax)) return false;
      F().listed = fold;
      F().list_n = F().rq.got;
      if (F().list_n == cardq::kListUnread) log(CORE_STR("store: %s not listed: its files asked one by one"), g_fold[sl()][fold]);
      F().ph = kOpening;
      return false;
    case kOpening:
      if (!asked(F().rq)) {
        // (the open not yet asked for)
        if (written(path)) return next();
        // a write of it queued or on its way: the card's bytes are about to be replaced - this path again at a later
        // step, once it has landed (fmt::post_step runs from the same idle pass; nothing here waits); its folder's
        // listing may be from before it landed, so it is opened then, not taken from the listing
        if (fmt::pending(path)) return waiting(path), g_nolist[sl()][wi] = true, false;
        if (fold >= 0 && F().listed != fold) {
          F().ph = kListing;
          return false;
        }
        if (fold >= 0 && F().list_n != cardq::kListUnread && !g_nolist[sl()][wi]) {
          if (F().list_n == cardq::kListMissing || !listed_name(path)) {   // not on the card: no card operation
            mark(path, kAbsent);
            return next();
          }
          F().there = true;
        }
      }
      if (!cardq::async(&F().rq, cardq::kOpenReq, &F().f, path, 0, nullptr, 0)) return false;
      if (F().rq.got != 1) {
        if (F().there) {   // listed, but it will not open: never written over
          if (fmt::pending(path)) return F().there = false, g_nolist[sl()][wi] = true, false;   // asked anew once it landed
          if (!written(path)) {
            mark(path, kLocked);
            log(CORE_STR("store: %s is there but cannot be read: locked"), path);
          }
          return next();
        }
        F().ph = kAsking;   // an error, or no such file: the card tells which
        return false;
      }
      F().open = true;
      F().ph = kClosing;
      if (written(path)) return false;   // saved while the open ran: the save's bytes, the file just closed
      if (fmt::pending(path)) return F().again = true, false;   // a write queued meanwhile: closed, then read anew
      {
        uint64_t size = F().f.fileSize();
        if (size > 0x7fffffffu) {   // (a read's result is an int32)
          mark(path, kLocked);
          log(CORE_STR("store: %s is too large to hold: locked"), path);
          return false;
        }
        F().size = (uint32_t)size;
      }
      F().buf = new uint8_t[F().size ? F().size : 1];   // (out of memory: a halt on the Pi, not 0 - put()'s note)
      if (!F().buf) {
        mark(path, kLocked);
        log(CORE_STR("store: out of memory for %s (%u bytes): locked"), path, (unsigned)F().size);
        return false;
      }
      if (!F().size) {   // empty: nothing to read
        adopt(path, F().buf, 0);
        F().buf = nullptr;
        return false;
      }
      F().ph = kReading;
      return false;
    case kReading:
      if (!cardq::async(&F().rq, cardq::kReadReq, &F().f, nullptr, 0, F().buf, F().size)) return false;
      if (written(path)) {   // saved while the read ran: the save's bytes are the newer
        delete[] F().buf;
      } else if (fmt::pending(path)) {   // a write queued while it was read: these bytes may be the old ones
        delete[] F().buf;
        F().again = true;
      } else if (F().rq.got != (int32_t)F().size) {
        delete[] F().buf;
        mark(path, kLocked);
        log(CORE_STR("store: %s is there but cannot be read (%d of %u bytes): locked"), path, (int)F().rq.got,
            (unsigned)F().size);
      } else {
        adopt(path, F().buf, F().size);
      }
      F().buf = nullptr;
      F().ph = kClosing;
      return false;
    case kClosing:
      if (!cardq::async(&F().rq, cardq::kCloseReq, &F().f, nullptr, 0, nullptr, 0)) return false;
      F().open = false;
      if (F().again) return F().again = false, F().there = false, F().ph = kOpening, false;
      return next();
    case kAsking:
      if (!cardq::async(&F().rq, cardq::kExistsReq, nullptr, path, 0, nullptr, 0)) return false;
      if (written(path)) {
        // saved while the card was asked: the save's bytes
      } else if (fmt::pending(path)) {   // a write queued meanwhile: asked anew once it has landed
        F().ph = kOpening;
        return false;
      } else if (F().rq.got == 1) {
        mark(path, kLocked);
        log(CORE_STR("store: %s is there but cannot be read: locked"), path);
      } else {
        mark(path, kAbsent);
      }
      return next();
  }
  return false;
}

bool ready() { return F().done; }
bool ready(int s) { return s >= 0 && s < kVoiceSlots && g_f[s].done; }
#endif

}  // namespace store
}  // namespace core
