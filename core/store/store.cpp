// core/store/store.cpp - see store.h: the RAM store, a flat array of entries with linear lookup.
#include <string.h>

#include "../platform.h"
#include "store.h"
#if CORE_MSAMP
#include "../fmt/fmt.h"
#include "../msamp/cardq.h"
#endif

namespace core {
namespace store {

namespace {
struct Entry {
  char path[kPathMax];
  State state;
  uint8_t* data;
  uint32_t len;
};
Entry g_e[kMaxEntries];
int g_n = 0;
uint32_t g_bytes = 0;

Entry* find(const char* path) {
  for (int i = 0; i < g_n; ++i)
    if (!strcmp(g_e[i].path, path)) return &g_e[i];
  return nullptr;
}
void drop_data(Entry* e) {
  delete[] e->data;
  g_bytes -= e->len;
  e->data = nullptr;
  e->len = 0;
}
// the entry for path (made if new); null when the path is too long or the store is full
Entry* entry_for(const char* path) {
  if (!path || strlen(path) >= (size_t)kPathMax) return nullptr;
  if (Entry* e = find(path)) return e;
  if (g_n >= kMaxEntries) return nullptr;
  Entry* e = &g_e[g_n++];
  strcpy(e->path, path);
  e->state = kUnknown;
  e->data = nullptr;
  e->len = 0;
  return e;
}
void drop_all() {
  for (int i = 0; i < g_n; ++i) delete[] g_e[i].data;
  g_n = 0;
  g_bytes = 0;
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
  g_bytes += n;
  e->state = kPresent;
  return true;
}
#if CORE_MSAMP
int g_nw = 0, g_nf = 0;   // the fill's wanted paths and their folders (below)
void abandon();
#endif
}  // namespace

CORE_COLD void clear() {
#if CORE_MSAMP
  abandon();
  g_nw = 0;
  g_nf = 0;
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

uint32_t bytes() { return g_bytes; }
int entries() { return g_n; }
int present() {
  int k = 0;
  for (int i = 0; i < g_n; ++i) k += g_e[i].state == kPresent;
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
char g_want[kMaxEntries][kPathMax];   // the paths the coming fill reads (g_nw of them)
int16_t g_wfold[kMaxEntries];         // each wanted path's folder (g_fold), -1 none (asked per path)
int16_t g_order[kMaxEntries];         // the order the fill reads them in: folder by folder
bool g_nolist[kMaxEntries];           // a write of it was pending at its folder's listing (or since): opened, not listed
char g_fold[kMaxFolders][kPathMax];
uint8_t g_list[kListMax];   // the listed folder's names, each NUL-ended (cardq kListReq)
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
  int at;         // the place in g_order of the wanted path being read
  int listed;     // the folder g_list holds (-1 none)
  int32_t list_n; // its listing's bytes, or cardq::kListMissing / kListUnread
  uint8_t* buf;   // the read's buffer (the whole file)
  uint32_t size;
  int wait_at;            // final review: the place in g_order whose pending write the fill waits for (-1 none),
  uint32_t wait_ms, said_ms;   //   since when, and when that was last logged
} g_f;
constexpr uint32_t kWaitSayMs = 2000;     // a fill waiting this long for one path's write says so (a normal one: a pass)
constexpr uint32_t kWaitAgainMs = 10000;  // and again at most this often, as long as it waits
// the fill waits for the path's pending write (fmt::pending): a write behind a card core held up (a held slot, a stalled
// card) never lands and the fill never completes - one line now and then says why, rather than a load that never ends
void waiting(const char* path) {
  uint32_t ms = now_ms();
  if (g_f.wait_at != g_f.at) g_f.wait_at = g_f.at, g_f.wait_ms = ms, g_f.said_ms = ms - kWaitAgainMs;
  if (ms - g_f.wait_ms < kWaitSayMs || ms - g_f.said_ms < kWaitAgainMs) return;
  g_f.said_ms = ms;
  log(CORE_STR("store: fill waiting for %s's write (%u ms; the card core has not written it yet)"), path,
      (unsigned)(ms - g_f.wait_ms));
}

// the folder index of path's parent (made if new); -1 when it has none or there are kMaxFolders already
int folder_of(const char* path) {
  const char* sl = strrchr(path, '/');
  if (!sl || sl == path) return -1;
  size_t n = (size_t)(sl - path);
  for (int i = 0; i < g_nf; ++i)
    if (strlen(g_fold[i]) == n && !strncmp(g_fold[i], path, n)) return i;
  if (g_nf >= kMaxFolders) return -1;
  memcpy(g_fold[g_nf], path, n);
  g_fold[g_nf][n] = 0;
  return g_nf++;
}
// FAT names compare without case (a file copied onto the card by hand may be P000.SQP)
bool same_name(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
    if (x != y) return false;
    if (!x) return true;
  }
}
// the path's name is in g_list (the listing of its folder)
bool listed_name(const char* path) {
  const char* name = strrchr(path, '/') + 1;
  for (uint32_t at = 0; at < (uint32_t)g_f.list_n;) {
    const char* e = (const char*)g_list + at;
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
  if (g_f.rq.kind == cardq::kOpenReq && g_f.rq.got == 1) g_f.open = true;
  if (g_f.rq.kind == cardq::kCloseReq) g_f.open = false;
  g_f.rq.state = cardq::kFree;
}
// the fill dropped where it is: what is the card core's already is let finish (fill_step drains it), the rest freed now
CORE_COLD void abandon() {
  g_f.running = false;
  g_f.done = false;
  uint32_t s = g_f.rq.state;
  if (s == cardq::kPosted && cardq::cancel(&g_f.rq)) {
    // never ran: the card core skips it (an open that never ran opened nothing)
  } else if (s == cardq::kPosted || s == cardq::kRunning) {
    g_f.wait_rq = true;   // on the card core now: its buffer, g_list and file are not ours until it is done
  } else if (s == cardq::kDone) {
    take_late();   // done on the card core but not yet taken here
  }
  if (!g_f.wait_rq) {
    delete[] g_f.buf;
    g_f.buf = nullptr;
  }
  g_f.drain = g_f.wait_rq || g_f.open || g_f.buf;
}
// one step of the leftovers; true when there are none
CORE_COLD bool drain_step() {
  if (g_f.wait_rq) {
    if (!cardq::idle(&g_f.rq)) return false;   // the card core still has it
    if (g_f.rq.state == cardq::kDone) take_late();
    g_f.rq.state = cardq::kFree;
    g_f.wait_rq = false;
    delete[] g_f.buf;
    g_f.buf = nullptr;
  }
  if (g_f.open) {
    if (!cardq::async(&g_f.rq, cardq::kCloseReq, &g_f.f, nullptr, 0, nullptr, 0)) return false;
    g_f.open = false;
  }
  g_f.drain = false;
  return true;
}
// 2026-10-26 (Task 4): an entry for a wanted path the fill has not held yet is a save's write-through (put) since
// fill_begin - newer than the card's bytes, so the fill keeps it and drops what it read (fill_begin empties the store)
bool written(const char* path) { return find(path) != nullptr; }
// the path read: the next one; true when that was the last
bool next() {
  g_f.ph = kOpening;
  g_f.there = false;
  if (++g_f.at < g_nw) return false;
  g_f.running = false;
  g_f.done = true;
  return true;
}
}  // namespace

CORE_COLD void want(const char* path) {
  if (!path || strlen(path) >= (size_t)kPathMax) {
    log(CORE_STR("store: cannot want %s (path too long)"), path ? path : "(null)");
    return;
  }
  for (int i = 0; i < g_nw; ++i)
    if (!strcmp(g_want[i], path)) return;
  if (g_nw >= kMaxEntries) {
    log(CORE_STR("store: cannot want %s (%d paths wanted already)"), path, g_nw);
    return;
  }
  g_wfold[g_nw] = (int16_t)folder_of(path);
  strcpy(g_want[g_nw++], path);
}

CORE_COLD void fill_begin() {
  abandon();
  drop_all();
  g_f.running = true;
  g_f.again = false;
  g_f.there = false;
  g_f.at = 0;
  g_f.wait_at = -1;
  g_f.ph = kOpening;
  g_f.listed = -1;
  // folder by folder (each listed once), in the order the folders were first wanted; the paths with none last
  int k = 0;
  for (int f = 0; f < g_nf; ++f)
    for (int i = 0; i < g_nw; ++i)
      if (g_wfold[i] == f) g_order[k++] = (int16_t)i;
  for (int i = 0; i < g_nw; ++i)
    if (g_wfold[i] < 0) g_order[k++] = (int16_t)i;
  memset(g_nolist, 0, sizeof g_nolist);
  if (!g_nw) g_f.running = false, g_f.done = true;   // nothing wanted: complete at once
}

CORE_COLD bool fill_step() {
  if (g_f.drain && !drain_step()) return false;
  if (!g_f.running) return g_f.done;
  int wi = g_order[g_f.at];
  const char* path = g_want[wi];
  int fold = g_wfold[wi];
  switch (g_f.ph) {
    case kListing:
      if (!asked(g_f.rq)) {   // (the listing not yet asked for) the folder's paths with a write on its way: not listed
        for (int i = 0; i < g_nw; ++i)
          if (g_wfold[i] == fold && fmt::pending(g_want[i])) g_nolist[i] = true;
      }
      if (!cardq::async(&g_f.rq, cardq::kListReq, nullptr, g_fold[fold], 0, g_list, kListMax)) return false;
      g_f.listed = fold;
      g_f.list_n = g_f.rq.got;
      if (g_f.list_n == cardq::kListUnread) log(CORE_STR("store: %s not listed: its files asked one by one"), g_fold[fold]);
      g_f.ph = kOpening;
      return false;
    case kOpening:
      if (!asked(g_f.rq)) {
        // (the open not yet asked for)
        if (written(path)) return next();
        // a write of it queued or on its way: the card's bytes are about to be replaced - this path again at a later
        // step, once it has landed (fmt::post_step runs from the same idle pass; nothing here waits); its folder's
        // listing may be from before it landed, so it is opened then, not taken from the listing
        if (fmt::pending(path)) return waiting(path), g_nolist[wi] = true, false;
        if (fold >= 0 && g_f.listed != fold) {
          g_f.ph = kListing;
          return false;
        }
        if (fold >= 0 && g_f.list_n != cardq::kListUnread && !g_nolist[wi]) {
          if (g_f.list_n == cardq::kListMissing || !listed_name(path)) {   // not on the card: no card operation
            mark(path, kAbsent);
            return next();
          }
          g_f.there = true;
        }
      }
      if (!cardq::async(&g_f.rq, cardq::kOpenReq, &g_f.f, path, 0, nullptr, 0)) return false;
      if (g_f.rq.got != 1) {
        if (g_f.there) {   // listed, but it will not open: never written over
          if (fmt::pending(path)) return g_f.there = false, g_nolist[wi] = true, false;   // asked anew once it landed
          if (!written(path)) {
            mark(path, kLocked);
            log(CORE_STR("store: %s is there but cannot be read: locked"), path);
          }
          return next();
        }
        g_f.ph = kAsking;   // an error, or no such file: the card tells which
        return false;
      }
      g_f.open = true;
      g_f.ph = kClosing;
      if (written(path)) return false;   // saved while the open ran: the save's bytes, the file just closed
      if (fmt::pending(path)) return g_f.again = true, false;   // a write queued meanwhile: closed, then read anew
      {
        uint64_t size = g_f.f.fileSize();
        if (size > 0x7fffffffu) {   // (a read's result is an int32)
          mark(path, kLocked);
          log(CORE_STR("store: %s is too large to hold: locked"), path);
          return false;
        }
        g_f.size = (uint32_t)size;
      }
      g_f.buf = new uint8_t[g_f.size ? g_f.size : 1];   // (out of memory: a halt on the Pi, not 0 - put()'s note)
      if (!g_f.buf) {
        mark(path, kLocked);
        log(CORE_STR("store: out of memory for %s (%u bytes): locked"), path, (unsigned)g_f.size);
        return false;
      }
      if (!g_f.size) {   // empty: nothing to read
        adopt(path, g_f.buf, 0);
        g_f.buf = nullptr;
        return false;
      }
      g_f.ph = kReading;
      return false;
    case kReading:
      if (!cardq::async(&g_f.rq, cardq::kReadReq, &g_f.f, nullptr, 0, g_f.buf, g_f.size)) return false;
      if (written(path)) {   // saved while the read ran: the save's bytes are the newer
        delete[] g_f.buf;
      } else if (fmt::pending(path)) {   // a write queued while it was read: these bytes may be the old ones
        delete[] g_f.buf;
        g_f.again = true;
      } else if (g_f.rq.got != (int32_t)g_f.size) {
        delete[] g_f.buf;
        mark(path, kLocked);
        log(CORE_STR("store: %s is there but cannot be read (%d of %u bytes): locked"), path, (int)g_f.rq.got,
            (unsigned)g_f.size);
      } else {
        adopt(path, g_f.buf, g_f.size);
      }
      g_f.buf = nullptr;
      g_f.ph = kClosing;
      return false;
    case kClosing:
      if (!cardq::async(&g_f.rq, cardq::kCloseReq, &g_f.f, nullptr, 0, nullptr, 0)) return false;
      g_f.open = false;
      if (g_f.again) return g_f.again = false, g_f.there = false, g_f.ph = kOpening, false;
      return next();
    case kAsking:
      if (!cardq::async(&g_f.rq, cardq::kExistsReq, nullptr, path, 0, nullptr, 0)) return false;
      if (written(path)) {
        // saved while the card was asked: the save's bytes
      } else if (fmt::pending(path)) {   // a write queued meanwhile: asked anew once it has landed
        g_f.ph = kOpening;
        return false;
      } else if (g_f.rq.got == 1) {
        mark(path, kLocked);
        log(CORE_STR("store: %s is there but cannot be read: locked"), path);
      } else {
        mark(path, kAbsent);
      }
      return next();
  }
  return false;
}

bool ready() { return g_f.done; }
#endif

}  // namespace store
}  // namespace core
