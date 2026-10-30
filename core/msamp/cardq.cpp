// core/msamp/cardq.cpp - see cardq.h.
#include "cardq.h"

#include <string.h>

#include "../fmt/fmt.h"
#include "../samp/out.h"

namespace core {
namespace cardq {

namespace {
Req* ring[kQueue];
volatile uint32_t wr = 0, rd = 0;   // wr: the control core's, rd: the card core's
volatile bool on = false;
volatile uint32_t need_ms = ~0u, low = 0;
Stats st;

inline uint32_t ld(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void sto(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

// kListReq: path's entry names into buf, each NUL-ended (cardq.h)
int32_t list(const char* path, uint8_t* buf, uint32_t len) {
  File d;
  if (!d.open(path, core::kRead) || !d.isDir()) {
    d.close();   // (a plain file of that name opened: closed again)
    // missing only when the card says so (FR_NO_FILE / FR_NO_PATH): a card error is never "nothing there" - the
    // fill would take every path in the folder for absent and a save would write over a real file
    Fs fs;
    return fs.lookup(path) == 0 ? kListMissing : kListUnread;
  }
  char name[256];
  bool dir;
  uint64_t size;
  uint16_t fd, ft;
  uint32_t n = 0;
  int32_t got = 0;
  while (d.nextEntry(name, sizeof name, &dir, &size, &fd, &ft)) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    uint32_t k = (uint32_t)strlen(name) + 1;
    if (!buf || n + k > len) {   // (never a partial list: a name left out would read as absent)
      got = kListUnread;
      break;
    }
    memcpy(buf + n, name, k);
    n += k;
  }
  if (got == 0 && d.nextFailed()) got = kListUnread;   // a readdir error is never the folder's end (a short list)
  d.close();
  return got < 0 ? got : (int32_t)n;
}

// kCopyReq: path from off -> to, appended (cardq.h); the card core's own buffer (one request at a time)
int32_t copy(const char* path, const char* to, uint64_t off, uint32_t len) {
  static uint8_t buf[8192];
  if (!to || !to[0] || len > kCopyMax) return -1;
  Fs fs;
  if (off == 0) {
    if (fs.exists(to)) return -1;   // never over a file that is there
    char d[kPathMax];
    sniprintf(d, sizeof d, "%s", to);
    char* sl = strrchr(d, '/');
    if (sl && sl != d) *sl = 0, samp::mkdirs(d);
  }
  File in, out;
  if (!in.open(path, core::kRead) || in.isDir()) return -1;
  if (off > in.fileSize()) return in.close(), -1;
  if (!in.seekSet(off) || !out.open(to, off == 0 ? core::kWrite : core::kAppend)) return in.close(), -1;
  uint32_t done = 0;
  bool ok = true;
  while (done < len) {
    uint32_t want = len - done < sizeof buf ? len - done : (uint32_t)sizeof buf;
    int n = in.read(buf, want);
    if (n < 0) { ok = false; break; }
    if (n == 0) break;
    if (out.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
    done += (uint32_t)n;
    if ((uint32_t)n < want) break;
  }
  in.close();
  out.close();
  return ok ? (int32_t)done : -1;
}

void run(Req* r) {
  if (r->kind < kKinds) ++st.ran[r->kind];
  if (r->kind == kOpenReq) {
    r->got = r->f->open(r->path, core::kRead) ? 1 : 0;
  } else if (r->kind == kCloseReq) {
    r->f->close();
    r->got = 0;
  } else if (r->kind == kWriteReq) {
    char d[kPathMax];
    sniprintf(d, sizeof d, "%s", r->path);
    char* sl = strrchr(d, '/');
    if (sl && sl != d) *sl = 0, samp::mkdirs(d);
    bool ok = r->f->open(r->path, core::kWrite);
    ok = ok && r->f->write(r->buf, r->len) == r->len;
    r->f->close();
    r->got = ok ? (int32_t)r->len : -1;
  } else if (r->kind == kExistsReq) {
    Fs fs;
    r->got = fs.exists(r->path) ? 1 : 0;
  } else if (r->kind == kRenameReq) {
    Fs fs;
    const char* to = (const char*)r->buf;
    r->got = to && to[0] && !fs.exists(to) && fs.rename(r->path, to) ? 1 : 0;   // never over a file that is there
  } else if (r->kind == kAtomicReq) {
    r->got = fmt::write_atomic(r->path, r->buf, r->len) ? (int32_t)r->len : -1;
  } else if (r->kind == kListReq) {
    r->got = list(r->path, r->buf, r->len);
  } else if (r->kind == kCopyReq) {
    r->got = copy(r->path, (const char*)r->buf, r->off, r->len);
  } else if (r->kind == kRemoveReq) {
    Fs fs;   // (rel-host: fmt::post_remove's "removed or not there" and prjslot's "removed, checked" both hold)
    r->got = !fs.exists(r->path) || (fs.remove(r->path) && !fs.exists(r->path)) ? 1 : 0;
  } else {
    r->got = r->f->seekSet(r->off) ? r->f->read(r->buf, r->len) : -1;
  }
}
}  // namespace

CORE_COLD void set_worker(bool w) { on = w; }
CORE_COLD bool worker() { return on; }

CORE_COLD bool post(Req* r) {
  r->posted_us = now_us();
  if (!on) {   // no card core: at once
    r->state = kRunning;
    run(r);
    sto(&r->state, kDone);
    ++st.posted, ++st.served;
    return true;
  }
  uint32_t w = wr;
  if (w - ld(&rd) >= (uint32_t)kQueue) {
    ++st.full;
    return false;
  }
  // a release store: the fields written above are the card core's once it sees kPosted, also through a ghost slot (a
  // cancelled request reposted: its old slot still in the ring, reached before the new one, where wr's release does
  // not cover the fields)
  sto(&r->state, kPosted);
  ring[w % kQueue] = r;
  sto(&wr, w + 1);
  ++st.posted;
  uint32_t p = w + 1 - ld(&rd);
  if (p > st.max_pending) st.max_pending = p;
  return true;
}

CORE_COLD bool cancel(Req* r) {
  uint32_t s = kPosted;
  if (__atomic_compare_exchange_n(&r->state, &s, (uint32_t)kCancelled, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    ++st.cancelled;
    return true;
  }
  return false;
}

CORE_COLD bool take_back(Req* r) {
  if (!cancel(r)) return false;
  --st.cancelled;
  r->state = kRunning;
  run(r);
  sto(&r->state, kDone);
  ++st.taken_back;
  return true;
}

CORE_COLD void wait(Req* r) {
  while (ld(&r->state) == kPosted || ld(&r->state) == kRunning) {
    if (!on) break;   // (cannot happen: inline requests are done when post returns)
  }
}

CORE_COLD bool async(Req* r, Kind kind, File* f, const char* path, uint64_t off, uint8_t* buf, uint32_t len) {
  uint32_t s = ld(&r->state);
  if (s == kPosted || s == kRunning) return false;
  if (s == kDone) {
    r->state = kFree;
    return true;
  }
  r->kind = kind, r->f = f, r->off = off, r->buf = buf, r->len = len, r->got = -1;
  sniprintf(r->path, sizeof r->path, "%s", path ? path : "");
  if (!post(r)) return false;   // the queue full: again at the next call
  if (ld(&r->state) != kDone) return false;
  r->state = kFree;             // no card core: run at once
  return true;
}

CORE_COLD bool idle(const Req* r) {
  uint32_t s = ld(&r->state);
  return s == kFree || s == kDone || s == kCancelled;
}

CORE_COLD void set_need(uint32_t ahead_ms, uint32_t low_ms) {
  need_ms = ahead_ms;
  low = low_ms;
}

CORE_COLD Req* service() {
  for (;;) {
    uint32_t r0 = rd;
    if (r0 == ld(&wr)) return nullptr;
    Req* r = ring[r0 % kQueue];
    sto(&rd, r0 + 1);
    uint32_t s = kPosted;
    if (!__atomic_compare_exchange_n(&r->state, &s, (uint32_t)kRunning, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      continue;   // cancelled meanwhile
    uint32_t waited = now_us() - r->posted_us;
    if (waited > st.max_wait_us) st.max_wait_us = waited;
    run(r);
    sto(&r->state, kDone);
    ++st.served;
    return r;
  }
}

CORE_COLD int pending() { return (int)(ld(&wr) - ld(&rd)); }
CORE_COLD uint32_t oldest_ms() {
  uint32_t r0 = ld(&rd);
  if (r0 == ld(&wr)) return 0;
  const Req* r = ring[r0 % kQueue];   // (a diagnostic read from another core: a request the card core has just taken, or a ghost slot of a cancelled one, can be read as the oldest - an age a little off, never a fault: ring slots are never freed)
  return r ? (now_us() - r->posted_us) / 1000 : 0;
}
CORE_COLD bool busy() { return pending() > 0 || need_ms < low; }
const Stats& stats() { return st; }
CORE_COLD void note_yield() { ++st.yields; }
CORE_COLD void note_upload_wait() { ++st.upload_waits; }

CORE_COLD void reset() {
  wr = rd = 0;
  on = false;
  need_ms = ~0u, low = 0;
  memset(&st, 0, sizeof st);
  // the posted settings write keeps a Req of its own here (fmt.h post_end): emptying the queue from under it would
  // leave its slot busy for good, and every later LFO / INSERT / keys save blocked
  fmt::reset_for_tests();
}

}  // namespace cardq
}  // namespace core
