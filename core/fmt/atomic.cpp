// core/fmt/atomic.cpp - see fmt.h: write_atomic, AtomicWriter, recover, the posted settings write. FatFs's f_rename
// does not replace an existing target, hence the three steps (spec 4.4; wifi.h does the same with wifi.txt.prev).
#include <string.h>

#include "fmt.h"
#if CORE_MSAMP
#include "../msamp/cardq.h"
#endif

namespace core {
namespace fmt {

namespace {
bool sibling(char* out, int cap, const char* path, const char* ext) {
  int n = sniprintf(out, cap, "%s%s", path, ext);
  return n > 0 && n < cap;
}
// the folder it goes in, and any folder above it missing (fix1049: the seq's saves come here through the card queue,
// so the control core makes no folders for them)
bool parent_made(const char* path) {
  char d[220];
  if (!sibling(d, sizeof d, path, "")) return false;
  char* sl = strrchr(d, '/');
  if (!sl || sl == d) return true;
  *sl = 0;
  Fs fs;
  if (fs.exists(d)) return true;
  for (char* c = d + 1; *c; ++c)
    if (*c == '/') {
      *c = 0;
      bool ok = fs.exists(d) || fs.mkdir(d);
      *c = '/';
      if (!ok) return false;
    }
  return fs.mkdir(d);
}
// [off, off + n) of the file equal to data's: read in 4 KB pieces (the stack's: a take-back may run this on core 3)
bool same(File& f, uint64_t off, const uint8_t* data, uint32_t n) {
  uint8_t b[4096];
  if (!f.seekSet(off)) return false;
  for (uint32_t d = 0; d < n;) {
    uint32_t k = n - d < sizeof b ? n - d : (uint32_t)sizeof b;
    if (f.read(b, k) != (int)k || memcmp(b, data + d, k)) return false;
    d += k;
  }
  return true;
}
// the swap: <path> -> .prev, .tmp -> <path>, .prev removed
bool swap_in(const char* path, const char* tmp) {
  char prev[220];
  if (!sibling(prev, sizeof prev, path, ".prev")) return false;
  Fs fs;
  if (fs.exists(prev) && !fs.remove(prev)) return false;
  bool had = fs.exists(path);
  if (had && !fs.rename(path, prev)) return false;
  if (!fs.rename(tmp, path)) {
    if (had) fs.rename(prev, path);   // (put back; a cut here is recover()'s)
    return false;
  }
  if (had) fs.remove(prev);
  return true;
}
}  // namespace

bool write_atomic(const char* path, const void* data, uint32_t len) {
  char tmp[220];
  if (!path || path[0] != '/' || !sibling(tmp, sizeof tmp, path, ".tmp") || !parent_made(path)) return false;
  File f;
  if (!f.open(tmp, kWrite)) return false;
  bool ok = !len || f.write(data, len) == len;
  ok = f.close() && ok;
  if (ok) {
    ok = f.open(tmp, kRead) && f.fileSize() == len && same(f, 0, (const uint8_t*)data, len);
    f.close();
  }
  Fs fs;
  if (!ok) {
    fs.remove(tmp);
    return false;
  }
  if (!swap_in(path, tmp)) {
    fs.remove(tmp);
    return false;
  }
  return true;
}

void recover(const char* path) {
  char tmp[220], prev[220];
  if (!path || !sibling(tmp, sizeof tmp, path, ".tmp") || !sibling(prev, sizeof prev, path, ".prev")) return;
  Fs fs;
  bool p = fs.exists(path), v = fs.exists(prev);
  if (!p && v) fs.rename(prev, path);   // cut between the two renames: the old file back
  else if (p && v) fs.remove(prev);     // cut before the .prev's removal: the new file is whole
  if (fs.exists(tmp)) fs.remove(tmp);   // a write that did not finish
}

#if CORE_MSAMP
namespace {
constexpr int kSweepMax = 64, kSweepDepth = 8;
struct Sweep {
  char path[220];
  char base[kSweepMax][220];   // the files to recover, collected first: a rename while FatFs walks a folder can skip
  int n = 0;                   // entries, so nothing is changed until the walk is over
  bool full = false;
};
bool ends(const char* s, const char* tail) {
  size_t a = strlen(s), b = strlen(tail);
  return a > b && !strcmp(s + a - b, tail);
}
void sweep_walk(Sweep& w, int depth) {
  File d;
  if (!d.open(w.path, kRead) || !d.isDir()) return;
  size_t pl = strlen(w.path);
  char name[256];
  bool dir;
  uint64_t size;
  uint16_t fd, ft;
  while (d.nextEntry(name, sizeof name, &dir, &size, &fd, &ft)) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    size_t nl = strlen(name);
    if (pl + 1 + nl >= sizeof w.path) continue;   // (a path this long is no atomic writer's)
    if (dir) {
      if (depth >= kSweepDepth) continue;
      w.path[pl] = '/', memcpy(w.path + pl + 1, name, nl + 1);
      sweep_walk(w, depth + 1);
      w.path[pl] = 0;
      continue;
    }
    size_t cut = ends(name, ".tmp") ? 4 : ends(name, ".prev") ? 5 : 0;
    if (!cut) continue;
    if (w.n == kSweepMax) {
      w.full = true;
      continue;
    }
    sniprintf(w.base[w.n++], sizeof w.base[0], "%s/%.*s", w.path, (int)(nl - cut), name);
  }
  d.close();
}
}  // namespace

int sweep(const char* root) {
  if (!root || root[0] != '/') return 0;
  static Sweep w;   // (14 KB: the card core's boot, one at a time)
  int fixed = 0;
  for (int round = 0; round < 8; ++round) {   // more than kSweepMax at once: walk again for the rest
    w.n = 0, w.full = false;
    sniprintf(w.path, sizeof w.path, "%s", root);
    sweep_walk(w, 0);
    Fs fs;
    for (int i = 0; i < w.n; ++i) {
      char tmp[220], prev[220];
      if (!sibling(tmp, sizeof tmp, w.base[i], ".tmp") || !sibling(prev, sizeof prev, w.base[i], ".prev")) continue;
      bool p = fs.exists(w.base[i]), v = fs.exists(prev), t = fs.exists(tmp);
      if (!v && !t) continue;   // (its .tmp and .prev both listed: the first one did it)
      recover(w.base[i]);
      ++fixed;
      // each one named: "put back" means the file was missing - a cut that would have lost it for its reader
      log(CORE_STR("fmt: boot sweep: %s: %s%s"), w.base[i], !p && v ? "PUT BACK from .prev" : v ? "stale .prev removed" : "",
          t ? (v ? ", stray .tmp removed" : "stray .tmp removed") : "");
    }
    if (!w.full) break;
  }
  if (fixed) log(CORE_STR("fmt: boot sweep %s: %d files"), root, fixed);
  return fixed;
}
int sweep_boot() { return sweep("/.rytm") + sweep("/CHOP") + sweep("/Samples"); }
#endif

void AtomicWriter::begin(const char* path, const uint8_t* data, uint32_t len, uint32_t piece) {
  cancel();
  sniprintf(path_, sizeof path_, "%s", path ? path : "");
  data_ = data, len_ = len, off_ = 0, io_ = 0;
  piece_ = piece ? piece : kPiece;
  st_ = path && path[0] == '/' ? kOpen : kDoneFail;
}

bool AtomicWriter::step() {
  io_ = 0;
  char tmp[220];
  if (!sibling(tmp, sizeof tmp, path_, ".tmp")) st_ = kDoneFail;
  Fs fs;
  switch (st_) {
    case kOpen:
      if (!parent_made(path_) || !f_.open(tmp, kWrite)) {
        st_ = kDoneFail;
        break;
      }
      off_ = 0;
      st_ = kWrite;
      return true;
    case kWrite: {
      uint32_t k = len_ - off_ < piece_ ? len_ - off_ : piece_;
      if (k && f_.write(data_ + off_, k) != k) {
        f_.close();
        fs.remove(tmp);
        st_ = kDoneFail;
        break;
      }
      off_ += k, io_ = k;
      if (off_ >= len_) st_ = kClose;
      return true;
    }
    case kClose:
      if (!f_.close() || !f_.open(tmp, kRead) || f_.fileSize() != len_) {
        f_.close();
        fs.remove(tmp);
        st_ = kDoneFail;
        break;
      }
      off_ = 0;
      st_ = len_ ? kBack : kSwap;
      return true;
    case kBack: {
      uint32_t k = len_ - off_ < piece_ ? len_ - off_ : piece_;
      if (!same(f_, off_, data_ + off_, k)) {
        f_.close();
        fs.remove(tmp);
        st_ = kDoneFail;
        break;
      }
      off_ += k, io_ = k;
      if (off_ >= len_) f_.close(), st_ = kSwap;
      return true;
    }
    case kSwap:
      if (swap_in(path_, tmp)) st_ = kDoneOk;
      else fs.remove(tmp), st_ = kDoneFail;
      break;
    default:
      break;
  }
  return false;
}

void AtomicWriter::cancel() {
  if (!busy()) {
    st_ = kIdle;
    return;
  }
  f_.close();
  char tmp[220];
  if (sibling(tmp, sizeof tmp, path_, ".tmp")) {
    Fs fs;
    if (fs.exists(tmp)) fs.remove(tmp);
  }
  st_ = kIdle;
}

// ---- the posted settings write (fmt.h): one slot, the card core's; post_queue's writes wait behind it, oldest first
namespace {
char g_ptext[kPostMax];
// 160 is well under cardq::kPathMax (192), which is where a posted path would otherwise be cut short - the longest of
// these is "/.rytm/ins/" + a 23-character project key + "/kNNN.txt" = 43. post_end refuses anything longer.
char g_ppath[160];
uint32_t g_plen = 0, g_pstart = 0;   // g_pstart: since the slot took it (now_us), for the held count below
uint32_t g_pat = 0;                  // when the slot's write was first wanted (now_us): g_pstart, or its queue entry's
bool g_pbusy = false, g_pok = true;
bool g_prm = false;                  // the slot's is a remove (post_remove), not a write
// post_result: every write an id (0 never), the slot's own, and the newest kPostResults finished writes' results
uint32_t g_wid = 0, g_pid = 0;
struct Done { uint32_t id; bool ok; };
Done g_done[kPostResults];
uint32_t g_ndone = 0;
uint32_t next_id() { return ++g_wid ? g_wid : ++g_wid; }
// phase 3 (2026-10-26): the slot's write is held (posted past kTakeBackUs, the card core not taken it): counted and
// logged once per write, not once per idle pass
bool g_pheld = false;
uint32_t g_nheld = 0;
// the slot's bytes: g_ptext, or (a queued write over kPostMax: a sequencer pattern, 2026-10-26) the queued copy itself,
// owned by the slot until its write is done
const char* g_pdata = g_ptext;
char* g_pown = nullptr;
#if CORE_MSAMP
cardq::Req g_prq;
// post_queue's writes (2026-10-26): each its own copy of the text, so nothing the caller does after (a kit change's load,
// a refill of the RAM store) can change or lose it. A heap list with no cap and no wait: one entry per path (a newer
// text replaces the queued one), so it is bounded by the settings files (the LFO and INSERT files of one project's
// 128 kits and its own, ~2 x 129, and a few more), never by how often the kits change.
struct Queued {
  Queued* next;
  char path[sizeof g_ppath];
  char* text;
  uint32_t len;
  bool rm;       // post_remove's
  uint32_t id;   // post_result's (a newer text replacing it: a new id)
  uint32_t at;   // now_us() when first queued (a newer text replacing it keeps it: the oldest unwritten edit's age)
};
Queued *g_qh = nullptr, *g_qt = nullptr;   // oldest, newest
int g_nq = 0;
// For unwritten() (read by core 0 for /status): plain scalars core 3 keeps at every queue change, so the reader never
// walks the list (core 3 frees and relinks its nodes). g_qoldest: the `at` of the oldest queued entry (valid while g_nq > 0)
uint32_t g_qoldest = 0;
void note_oldest() {
  uint32_t now = now_us(), best = 0;
  for (const Queued* q = g_qh; q; q = q->next)
    if (q == g_qh || now - q->at > best) best = now - q->at, g_qoldest = q->at;
}
Queued* queued(const char* path) {   // the oldest queued write (path null), or that path's
  for (Queued* q = g_qh; q; q = q->next)
    if (!path || !strcmp(q->path, path)) return q;
  return nullptr;
}
#endif
void post_done() {
  g_pbusy = false;
  delete[] g_pown;
  g_pown = nullptr;
  g_pdata = g_ptext;
  g_done[g_ndone++ % kPostResults] = {g_pid, g_pok};
  if (!g_pok) log(CORE_STR("cannot write %s (the file before it is kept)"), g_ppath);
}
// the slot taken for the text in g_ptext (posted by post_step; with no card core, written by it at once)
void take_slot(const char* path, uint32_t len, uint32_t id) {
  sniprintf(g_ppath, sizeof g_ppath, "%s", path);
  g_pid = id;
  g_prm = false;
  g_pdata = g_ptext;
  g_plen = len, g_pstart = g_pat = now_us();
  g_pbusy = true, g_pok = true, g_pheld = false;
#if CORE_MSAMP
  g_prq.state = cardq::kFree;
#endif
}
#if CORE_MSAMP
// the slot free: the oldest queued write into it
void take_queued() {
  Queued* q = g_qh;
  take_slot(q->path, q->len, q->id);
  g_prm = q->rm;
  g_pat = q->at;
  if (q->len > kPostMax) g_pown = q->text, g_pdata = g_pown;   // (no copy: the slot owns the queued one)
  else {
    if (q->len) memcpy(g_ptext, q->text, q->len);
    delete[] q->text;
  }
  g_qh = q->next;
  if (!g_qh) g_qt = nullptr;
  --g_nq;
  delete q;
  note_oldest();
}
// the slot's write finished here: the queue never took it - run now; still waiting - taken back; running - waited for.
// Only post_wait reaches it (a caller that waits on purpose: the rename's FLUSH), never post_step (phase 3)
void finish_here() {
  uint32_t s = __atomic_load_n(&g_prq.state, __ATOMIC_ACQUIRE);
  if (s == cardq::kFree && g_prm) {
    Fs fs;
    g_prq.got = !fs.exists(g_ppath) || fs.remove(g_ppath) ? 1 : -1;
  } else if (s == cardq::kFree) {
    g_prq.got = write_atomic(g_ppath, g_pdata, g_plen) ? (int32_t)g_plen : -1;
  }
  else if (!cardq::take_back(&g_prq)) cardq::wait(&g_prq);
  g_prq.state = cardq::kFree;
  g_pok = g_prm ? g_prq.got > 0 : g_prq.got >= 0;   // (a remove: 0 is the card's refusal)
  post_done();
}
#endif
}  // namespace

// (null while a queued write waits too: a newer save of a queued file must never land before it)
char* post_begin() {
#if CORE_MSAMP
  if (g_nq) return nullptr;
#endif
  return g_pbusy ? nullptr : g_ptext;
}

bool post_end(const char* path, uint32_t len) {
  if (g_pbusy || !path || path[0] != '/' || len > kPostMax || strlen(path) >= sizeof g_ppath) return false;
  take_slot(path, len, next_id());
#if CORE_MSAMP
  post_step();   // posted (no card core: written now)
#else
  g_pok = write_atomic(g_ppath, g_ptext, g_plen);
  post_done();
#endif
  return true;
}

bool post_queue(const char* path, const char* text, uint32_t len, uint32_t* id) {
  if (id) *id = 0;
  if (!path || path[0] != '/' || len > kQueueMax || strlen(path) >= sizeof g_ppath) return false;
  if (len <= kPostMax)
    if (char* t = post_begin()) {
      if (!(memcpy(t, text, len), post_end(path, len))) return false;
      if (id) *id = g_pid;   // (the slot's: post_end gave it one, written or not by now)
      return true;
    }
#if CORE_MSAMP
  // never a wait here: this runs on the control core at a kit change (a wait is a card write on it - the 40 ms stall)
  // (plain new: 0 on the Pi when the heap is out - then the edit is lost to the card, kept in the RAM store only)
  char* c = new char[len ? len : 1];
  Queued* q = queued(path);   // the same file queued already: this newer text replaces it, at the tail
  Queued* fresh = q || !c ? nullptr : new Queued;
  if (!c || (!q && !fresh)) {
    delete[] c;
    log(CORE_STR("post: OUT OF MEMORY: %s not queued - this edit is not written to the card (RAM only)"), path);
    return false;
  }
  memcpy(c, text, len);
  if (q) {
    delete[] q->text;
    // (fix round 1) moved to the tail: the newer text lands after every write queued before it - a RELOAD's .prv
    // (the working copy kept) before the .sqp that replaces it, when an older .sqp save was queued ahead of both
    if (q != g_qt) {
      Queued** pp = &g_qh;
      while (*pp != q) pp = &(*pp)->next;
      *pp = q->next;
      q->next = nullptr;
      g_qt->next = q;
      g_qt = q;
    }
  } else {
    q = fresh;
    q->next = nullptr;
    q->at = now_us();
    sniprintf(q->path, sizeof q->path, "%s", path);
    if (g_qt) g_qt->next = q;
    else g_qh = q;
    g_qt = q;
    ++g_nq;
  }
  q->text = c;
  q->len = len;
  q->rm = false;
  q->id = next_id();
  if (id) *id = q->id;
  note_oldest();
  if (len > kPostMax && !g_pbusy) post_step();   // over kPostMax, the slot free: posted now (no card core: written now)
  return true;
#else
  return false;   // (no card core: the slot is never busy, post_begin above always took it; over kPostMax: refused)
#endif
}

bool post_remove(const char* path, uint32_t* id) {
  if (id) *id = 0;
  if (!path || path[0] != '/' || strlen(path) >= sizeof g_ppath) return false;
#if CORE_MSAMP
  Queued* q = queued(path);
  if (q) {
    delete[] q->text;
  } else {
    q = new Queued;
    if (!q) return false;
    q->next = nullptr, q->at = now_us();
    sniprintf(q->path, sizeof q->path, "%s", path);
    if (g_qt) g_qt->next = q;
    else g_qh = q;
    g_qt = q;
    ++g_nq;
  }
  q->text = nullptr, q->len = 0, q->rm = true;
  q->id = next_id();
  if (id) *id = q->id;
  note_oldest();
  if (!g_pbusy) post_step();
  return true;
#else
  return false;
#endif
}

bool post_step() {
#if CORE_MSAMP
  if (!g_pbusy && g_nq) take_queued();
#endif
  if (!g_pbusy) return false;
#if CORE_MSAMP
  if (!cardq::async(&g_prq, g_prm ? cardq::kRemoveReq : cardq::kAtomicReq, nullptr, g_ppath, 0, (uint8_t*)g_pdata, g_plen)) {
    // Phase 3 (2026-10-26): a write the card core has not taken after cardq's kTakeBackUs (held up: a long upload, a
    // card's own pause) stays posted. It used to be taken back and run here, but this is the control core, whose OUT
    // FIFO holds 2 ms, and the write may now be a pattern's (the RAM store queues them): a take-back was a dropout
    // of tens of ms. The edit is safe in the RAM store meanwhile and the queue behind keeps growing without a cap or
    // a wait, so nothing is lost; it all lands, in order, once the card core runs again. Counted and logged once so a
    // card core that never comes back is still seen. (The sample streams keep their take-back: msamp.cpp.)
    if (!g_pheld && now_us() - g_pstart > cardq::kTakeBackUs) {
      g_pheld = true, ++g_nheld;
      log(CORE_STR("post: %s not taken by the card core in %u ms - kept posted (the card core held up)"), g_ppath,
          (unsigned)(cardq::kTakeBackUs / 1000));
    }
    return true;
  }
  g_pok = g_prm ? g_prq.got > 0 : g_prq.got >= 0;
#endif
  post_done();
  return false;
}

uint32_t post_held() { return g_nheld; }
bool post_stalled() { return g_pbusy && g_pheld; }

bool post_busy() {
#if CORE_MSAMP
  if (g_nq) return true;
#endif
  return g_pbusy;
}

void post_wait(const char* path) {
#if CORE_MSAMP
  // (as seq's cw_wait); the queue in its order up to the path's own write (no path: all of it)
  for (;;) {
    if (!g_pbusy) {
      if (!queued(path)) return;
      take_queued();
    }
    if (path && strcmp(path, g_ppath) && !queued(path)) return;   // not in this reader's way
    finish_here();
  }
#endif
}

bool pending(const char* path) {
  if (!path) return false;
  if (g_pbusy && !strcmp(g_ppath, path)) return true;   // in the slot: posted, or on the card core now
#if CORE_MSAMP
  if (queued(path)) return true;
#endif
  return false;
}

uint32_t unwritten(uint32_t* oldest_ms) {
  // each write once: the queued ones, and the slot's unless it still sits in cardq's ring (cardq::pending() counts it
  // there). Only scalars (no list walk), read from another core: torn at worst.
  uint32_t n = 0, now = now_us(), age = 0;
  if (g_pbusy) {
#if CORE_MSAMP
    if (__atomic_load_n(&g_prq.state, __ATOMIC_ACQUIRE) != cardq::kPosted) ++n;
#else
    ++n;
#endif
    age = now - g_pat;
  }
#if CORE_MSAMP
  uint32_t nq = (uint32_t)g_nq;
  if (nq) n += nq, age = now - g_qoldest > age ? now - g_qoldest : age;
#endif
  if (oldest_ms) *oldest_ms = age / 1000;
  return n;
}

bool post_ok() { return g_pok; }
PostResult post_result(uint32_t id) {
  if (!id) return kPostUnknown;
  if (g_pbusy && g_pid == id) return kPostPending;
#if CORE_MSAMP
  for (const Queued* q = g_qh; q; q = q->next)
    if (q->id == id) return kPostPending;
#endif
  for (uint32_t k = 0; k < (uint32_t)kPostResults && k < g_ndone; ++k)
    if (g_done[k].id == id) return g_done[k].ok ? kPostOk : kPostFailed;
  return kPostUnknown;
}

void reset_for_tests() {
  g_pbusy = false, g_pok = true, g_plen = 0, g_ppath[0] = 0, g_pid = 0, g_ndone = 0;
  g_pheld = false, g_nheld = 0;
  delete[] g_pown;
  g_pown = nullptr;
  g_pdata = g_ptext;
#if CORE_MSAMP
  memset(&g_prq, 0, sizeof g_prq);
  while (g_qh) {
    Queued* q = g_qh;
    g_qh = q->next;
    delete[] q->text;
    delete q;
  }
  g_qt = nullptr, g_nq = 0, g_qoldest = 0;
#endif
}

}  // namespace fmt
}  // namespace core
