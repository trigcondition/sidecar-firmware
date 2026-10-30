// core/pool/pcache.cpp - see pcache.h. The control core's and cold; every card operation through cardq (the card core).
#include "../platform.h"

#if CORE_MSAMP
#include "pcache.h"

#include <string.h>
#include <strings.h>

#include "../msamp/cardq.h"
#include "../wavetable/wav_read.h"

namespace core {
namespace pcache {

namespace {

constexpr uint64_t kAlign = 64;

struct Blk {
  uint64_t off, bytes;
  uint32_t used_ms;
  uint16_t pins;
  int16_t ent;      // the entry whose data it is (-1: private, or an orphan waiting for its last pin)
  uint8_t live, priv;
};
struct Ent {
  uint8_t st;       // State
  bool whole, cut, nofit;
  bool filling;     // a machine fills it (fill_begin .. fill_end): no whole load, no forget meanwhile
  bool want_whole;  // a whole load was wanted while it filled: queued at fill_end
  bool drop_after;  // forgotten while it filled: dropped at fill_end
  uint8_t fillch;   // the fill's channels
  int fblk;         // the block the fill writes (-1 none)
  int blk;          // its data (-1 none)
  int grow;         // a whole load's block while the entry is a ready prefix (-1 none)
  uint32_t frames, total;
  float rate;
  uint8_t ch;       // 1 mono, 2 stereo interleaved (the block's layout; a grow may change it)
  uint8_t fch;      // the file's channels (a whole load's; 0 unknown)
  uint32_t used_ms;
  lib::Ref key;
  char path[lib::kPath];   // where its whole load reads (want's)
};

uint8_t* arena = nullptr;
uint64_t arena_bytes = 0;
Blk* B = nullptr;       // [kBlocks] (a block id = its index)
int* ord = nullptr;     // [kBlocks] the live blocks' ids in address order
int nord = 0;
Ent* E = nullptr;       // [kEntries]
bool began = false;
bool g_mono = false;   // poolmono=1
Stats st;

// the load (one at a time) and the queue (wants before prefetches)
struct Job {
  int e;            // -1 idle
  bool pre;
  int phase;        // 0 open, 1 header, 2 read, 3 close
  int blk;          // the block filled
  cardq::Req rq;
  File f;
  char path[lib::kPath];
  wavread::Info a;
  uint32_t got, want, off;
  int ch;           // the block's channels (1, or 2 for a stereo file unless poolmono)
  bool failed;
  uint8_t* buf;     // [kChunk]
  float* cv;        // [kChunk]
};
Job J;
constexpr int kQueue = 64;
int Q[kQueue];
bool Qpre[kQueue];
char Qpath[kQueue][lib::kPath];
int nq = 0;

CORE_COLD bool same_key(const lib::Ref& a, const lib::Ref& b) {
  if (a.has_sha && b.has_sha) return !memcmp(a.sha, b.sha, 32);
  if (strcasecmp(a.hint, b.hint)) return false;
  return !a.size || !b.size || a.size == b.size;
}
CORE_COLD uint64_t up(uint64_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }
CORE_COLD void* data(int b) { return arena + B[b].off; }
CORE_COLD void free_block(int b) {
  if (b < 0 || !B[b].live) return;
  B[b].live = 0;
  for (int i = 0; i < nord; ++i)
    if (ord[i] == b) {
      memmove(ord + i, ord + i + 1, sizeof(int) * (nord - i - 1));
      --nord;
      break;
    }
  st.used -= B[b].bytes;
  if (B[b].priv) st.priv -= B[b].bytes;
}
CORE_COLD void dequeue(int e) {
  for (int i = 0; i < nq; ++i)
    if (Q[i] == e) {
      memmove(Q + i, Q + i + 1, sizeof(int) * (nq - i - 1)), memmove(Qpre + i, Qpre + i + 1, sizeof(bool) * (nq - i - 1));
      memmove(Qpath + i, Qpath + i + 1, sizeof Qpath[0] * (nq - i - 1));
      --nq;
      return;
    }
}
CORE_COLD bool queued(int e) {
  for (int i = 0; i < nq; ++i)
    if (Q[i] == e) return true;
  return false;
}
CORE_COLD void drop_entry(int e) {   // its blocks freed (or orphaned while pinned), the entry free, out of the queue
  Ent& x = E[e];
  dequeue(e);
  if (x.fblk >= 0 && x.fblk != x.blk && x.fblk != x.grow) free_block(x.fblk);
  x.fblk = -1, x.filling = false;
  int* bs[2] = {&x.blk, &x.grow};
  for (int* pb : bs) {
    int b = *pb;
    if (b < 0) continue;
    if (B[b].pins) B[b].ent = -1;
    else free_block(b);
    *pb = -1;
  }
  if (x.st == kReady) --st.ready;
  x.st = kNone;
  --st.entries;
}
// an entry's block may be evicted: it is ready (or failed), nothing pins it, no load writes it
CORE_COLD bool evictable(int b) {
  const Blk& k = B[b];
  if (k.priv || k.pins || k.ent < 0) return false;
  const Ent& x = E[k.ent];
  return x.st == kReady && x.blk == b && x.grow < 0 && !x.filling && J.e != k.ent;
}
// a block of bytes; may_evict: least recently used window of evictable blocks; -1 none
CORE_COLD int alloc(uint64_t bytes, bool may_evict, bool priv, int ent) {
  uint64_t need = up(bytes ? bytes : 1);
  if (!arena || need > arena_bytes) return -1;
  int id = -1;
  for (int i = 0; i < kBlocks && id < 0; ++i)
    if (!B[i].live) id = i;
  if (id < 0) return -1;
  // 1. the smallest hole that fits
  uint64_t best_off = 0, best_hole = ~0ull;
  bool found = false;
  for (int i = 0; i <= nord; ++i) {
    uint64_t s = i ? B[ord[i - 1]].off + B[ord[i - 1]].bytes : 0, e = i < nord ? B[ord[i]].off : arena_bytes;
    if (e - s >= need && e - s < best_hole) best_hole = e - s, best_off = s, found = true;
  }
  int pos = -1;
  if (!found) {
    if (!may_evict) return -1;
    // 2. a window from a hole's start over evictable blocks: the one whose newest use is oldest (fewer bytes on a tie)
    uint32_t now = now_ms();
    int32_t best_age = -1;
    uint64_t best_bytes = ~0ull;
    int bf = -1, bl = -1;
    for (int i = 0; i <= nord; ++i) {   // start at the hole before block i (its previous block's end)
      uint64_t s = i ? B[ord[i - 1]].off + B[ord[i - 1]].bytes : 0;
      int32_t newest = 0;
      uint64_t ev = 0;
      int j = i;
      bool ok = true;
      bool any = false;
      while (true) {
        uint64_t e = j < nord ? B[ord[j]].off : arena_bytes;
        if (e - s >= need) break;
        if (j >= nord || j - i >= 64 || !evictable(ord[j])) {   // (a window of 64 blocks at most)
          ok = false;
          break;
        }
        int32_t age = (int32_t)(now - B[ord[j]].used_ms);
        newest = !any || age < newest ? age : newest;
        any = true;
        ev += B[ord[j]].bytes;
        ++j;
      }
      if (!ok || !any) continue;
      if (newest > best_age || (newest == best_age && ev < best_bytes)) best_age = newest, best_bytes = ev, bf = i, bl = j;
    }
    if (bf < 0) return -1;
    int nev = bl - bf;
    int victims[64];
    int nv = 0;
    for (int k = bf; k < bl && nv < 64; ++k) victims[nv++] = ord[k];
    if (nv < nev) return -1;
    best_off = bf ? B[ord[bf - 1]].off + B[ord[bf - 1]].bytes : 0;
    for (int k = 0; k < nv; ++k) {
      int v = victims[k];
      ++st.evictions;
      drop_entry(B[v].ent);
    }
  }
  for (pos = 0; pos < nord && B[ord[pos]].off < best_off; ++pos) {}
  memmove(ord + pos + 1, ord + pos, sizeof(int) * (nord - pos));
  ord[pos] = id;
  ++nord;
  Blk& k = B[id];
  k.off = best_off, k.bytes = need, k.used_ms = now_ms(), k.pins = 0, k.ent = (int16_t)ent, k.live = 1, k.priv = priv;
  st.used += need;
  if (priv) st.priv += need;
  return id;
}
CORE_COLD int new_entry(const lib::Ref& r) {
  int e = -1;
  for (int i = 0; i < kEntries && e < 0; ++i)
    if (E[i].st == kNone) e = i;
  if (e < 0) {   // full: the least recently used one nothing holds
    for (int i = 0; i < kEntries; ++i) {
      if (E[i].blk >= 0 && !evictable(E[i].blk)) continue;
      if (J.e == i || E[i].filling || queued(i)) continue;
      if (e < 0 || (int32_t)(E[i].used_ms - E[e].used_ms) < 0) e = i;
    }
    if (e < 0) return -1;
    drop_entry(e);
  }
  Ent& x = E[e];
  memset(&x, 0, sizeof x);
  x.blk = x.grow = x.fblk = -1;
  x.key = r;
  x.used_ms = now_ms();
  ++st.entries;
  return e;
}
CORE_COLD bool enqueue(int e, const char* path, bool pre) {
  sniprintf(E[e].path, sizeof E[e].path, "%s", path);
  for (int i = 0; i < nq; ++i)
    if (Q[i] == e) {
      if (!pre && Qpre[i]) {   // a want now: to the front
        memmove(Q + 1, Q, sizeof(int) * i), memmove(Qpre + 1, Qpre, sizeof(bool) * i);
        memmove(Qpath + 1, Qpath, sizeof Qpath[0] * i);
        Q[0] = e, Qpre[0] = false, sniprintf(Qpath[0], sizeof Qpath[0], "%s", path);
      }
      return true;
    }
  if (nq >= kQueue) {
    if (pre || Qpre[nq - 1] == false) return false;   // full of wants: refused (the caller tries again)
    int last = Q[nq - 1];   // the last prefetch goes, its entry with it unless something else holds it
    --nq;
    if (E[last].st == kLoading && !E[last].filling && J.e != last && E[last].blk < 0) drop_entry(last);
  }
  int at = nq;
  if (!pre)
    for (at = 0; at < nq && !Qpre[at]; ++at) {}
  memmove(Q + at + 1, Q + at, sizeof(int) * (nq - at)), memmove(Qpre + at + 1, Qpre + at, sizeof(bool) * (nq - at));
  memmove(Qpath + at + 1, Qpath + at, sizeof Qpath[0] * (nq - at));
  Q[at] = e, Qpre[at] = pre, sniprintf(Qpath[at], sizeof Qpath[at], "%s", path);
  ++nq;
  return true;
}
CORE_COLD void job_end(bool ok) {
  Ent& x = E[J.e];
  if (ok && J.got >= 2) {
    int16_t* d = (int16_t*)data(J.blk);
    for (int c = 0; c < J.ch; ++c) d[J.got * J.ch + c] = d[(J.got - 1) * J.ch + c];   // the guard frame
    if (x.st == kReady && x.blk >= 0 && x.blk != J.blk) {   // a prefix grown: the new block, the old one kept while pinned
      int ob = x.blk;
      if (B[ob].pins) B[ob].ent = -1;
      else free_block(ob);
      ++st.grows;
    } else if (x.st != kReady) {
      ++st.ready;
    }
    x.blk = J.blk, x.grow = -1;
    x.frames = J.got, x.total = J.a.frames, x.rate = (float)(J.a.rate ? J.a.rate : 48000);
    x.ch = (uint8_t)J.ch;
    x.fch = (uint8_t)(J.a.ch > 255 ? 255 : J.a.ch);
    x.whole = true, x.cut = J.a.frames > J.got;
    x.st = kReady, x.used_ms = now_ms();
    B[J.blk].used_ms = now_ms();
    ++st.loads;
    log(CORE_STR("pcache: %s: %lu frames at %lu Hz%s%s (%llu of %llu KB used)"), J.path, (unsigned long)J.got,
        (unsigned long)x.rate, x.ch == 2 ? " stereo" : "", x.cut ? ", cut" : "", (unsigned long long)(st.used >> 10), (unsigned long long)(arena_bytes >> 10));
  } else {
    if (J.blk >= 0 && J.blk != x.blk) free_block(J.blk);
    if (x.st == kReady) {
      x.grow = -1;   // a failed grow: the prefix stays
    } else {
      x.st = kFailed, x.blk = -1;
      ++st.fails;
    }
    if (!J.pre) log(CORE_STR("pcache: %s: %s"), J.path, x.nofit ? "no room in the pool RAM" : "not a sample this reads");
  }
  J.e = -1;
}
CORE_COLD void fail_close(bool nofit) {
  E[J.e].nofit = nofit;
  J.failed = true;
  J.phase = 3;
  J.rq.state = cardq::kFree;
}
}  // namespace

CORE_COLD void begin(uint64_t budget) {
  if (!began) {
    B = new Blk[kBlocks];
    ord = new int[kBlocks];
    E = new Ent[kEntries];
    J.buf = new uint8_t[kChunk];
    J.cv = new float[kChunk];
    if (budget) arena = (uint8_t*)msamp_ram_alloc(budget);
    arena_bytes = arena ? budget & ~(kAlign - 1) : 0;
    began = true;
    log(CORE_STR("pcache: %llu MB of pool RAM%s"), (unsigned long long)(arena_bytes >> 20), arena ? "" : " (none: card sources off)");
    reset_for_tests();
  }
}
bool ok() { return began && arena; }

CORE_COLD int find(const lib::Ref& r, uint32_t min_frames) {
  if (!began) return -1;
  for (int e = 0; e < kEntries; ++e) {
    const Ent& x = E[e];
    if (x.st == kNone || !same_key(x.key, r)) continue;
    if (!min_frames) return e;
    if (x.st != kReady) return -1;
    if (min_frames == kWhole ? x.whole : (x.whole || x.frames >= min_frames)) return e;
    return -1;
  }
  return -1;
}
CORE_COLD bool get(int e, Info* o) {
  if (!began || e < 0 || e >= kEntries || E[e].st != kReady || E[e].blk < 0) return false;
  const Ent& x = E[e];
  o->pcm = (const int16_t*)data(x.blk), o->frames = x.frames, o->total = x.total, o->rate = x.rate;
  o->whole = x.whole, o->cut = x.cut, o->blk = x.blk, o->ch = x.ch ? x.ch : 1, o->fch = x.fch;
  return true;
}
State state(int e) { return began && e >= 0 && e < kEntries ? (State)E[e].st : kNone; }
bool no_room(int e) { return began && e >= 0 && e < kEntries && E[e].st == kFailed && E[e].nofit; }

CORE_COLD int want(const lib::Ref& r, const char* path, bool prefetch) {
  if (!ok() || !path || !path[0]) return -1;
  int e = find(r, 0);
  if (e >= 0) {
    Ent& x = E[e];
    if (r.has_sha && !x.key.has_sha) x.key = r;   // (its hash known now)
    if (x.st == kFailed) {   // no room then: a want tries again (a file it cannot read is not tried again)
      if (prefetch || !x.nofit) return prefetch ? -1 : e;
      x.st = kLoading, x.nofit = false;
      --st.fails;
    }
    if (x.st == kReady && x.whole) {
      ++st.shared_hits;
      touch(e);
      return e;
    }
    if (x.filling) {   // a machine fills its head now: the whole load after that
      if (!prefetch) x.want_whole = true, sniprintf(x.path, sizeof x.path, "%s", path);
      return e;
    }
    if (!enqueue(e, path, prefetch) && x.st == kLoading && J.e != e && x.blk < 0) {
      drop_entry(e);
      return -1;
    }
    return e;
  }
  e = new_entry(r);
  if (e < 0) return -1;
  E[e].st = kLoading;
  if (!enqueue(e, path, prefetch)) {
    drop_entry(e);
    return -1;
  }
  return e;
}

void set_mono(bool on) { g_mono = on; }
bool mono() { return g_mono; }

CORE_COLD int fill_begin(const lib::Ref& r, uint32_t frames, uint32_t total, float rate, int16_t** dst, int* have, int ch) {
  if (have) *have = -1;
  if (!ok() || !frames) return -1;
  ch = ch == 2 ? 2 : 1;
  int e = find(r, 0);
  if (e >= 0 && E[e].st == kReady && (E[e].whole || E[e].frames >= frames)) {
    if (have) *have = e;
    ++st.shared_hits;
    touch(e);
    return -1;
  }
  if (e >= 0 && (J.e == e || E[e].filling)) return -1;   // its whole load runs / another fill: not now
  bool was_queued = e >= 0 && queued(e);
  if (e >= 0 && was_queued && E[e].st == kLoading) dequeue(e);   // a queued load not started: this fill takes it over
  if (e < 0) e = new_entry(r);
  if (e < 0) return -1;
  Ent& x = E[e];
  if (x.st == kFailed) x.st = kLoading, x.nofit = false, --st.fails;
  int b = alloc(((uint64_t)frames + 1) * 2 * ch, true, false, e);
  if (b < 0) {
    if (x.st != kReady) drop_entry(e);
    return -1;
  }
  if (x.st != kReady) x.ch = (uint8_t)ch;
  x.fillch = (uint8_t)ch;
  if (was_queued) x.want_whole = true;   // (the whole load it took over: queued again after the fill)
  if (x.st != kReady) x.st = kLoading;
  x.filling = true, x.fblk = b, x.drop_after = false;
  x.total = total, x.rate = rate;
  *dst = (int16_t*)data(b);
  return e;
}
CORE_COLD void fill_end(int e, uint32_t written, bool okk) {
  if (!began || e < 0 || e >= kEntries || !E[e].filling) return;
  Ent& x = E[e];
  int b = x.fblk;
  x.filling = false, x.fblk = -1;
  if (x.drop_after || !okk || written < 2 || b < 0) {
    free_block(b);
    if (x.drop_after || x.st != kReady) drop_entry(e);
    return;
  }
  int16_t* d = (int16_t*)data(b);
  int ch = x.fillch == 2 ? 2 : 1;
  for (int c = 0; c < ch; ++c) d[written * ch + c] = d[(written - 1) * ch + c];
  x.ch = (uint8_t)ch;
  if (x.st == kReady && x.blk >= 0) {   // a longer prefix: the old block kept while pinned
    int ob = x.blk;
    if (B[ob].pins) B[ob].ent = -1;
    else free_block(ob);
  } else {
    ++st.ready;
  }
  x.blk = b;
  x.frames = written, x.whole = x.total && written >= x.total, x.cut = false;
  x.st = kReady, x.used_ms = now_ms();
  B[b].used_ms = now_ms();
  if (x.want_whole && !x.whole && x.path[0]) enqueue(e, x.path, false);
  x.want_whole = false;
}
CORE_COLD bool busy(int e) {   // a whole load waits or runs for it (queued, running, or after its fill)
  return began && e >= 0 && e < kEntries && E[e].st != kNone && (queued(e) || J.e == e || (E[e].filling && E[e].want_whole));
}
CORE_COLD bool is(int e, const lib::Ref& r) {
  return began && e >= 0 && e < kEntries && E[e].st != kNone && same_key(E[e].key, r);
}

CORE_COLD void pin(int b) {
  if (began && b >= 0 && b < kBlocks && B[b].live) ++B[b].pins, B[b].used_ms = now_ms();
}
CORE_COLD void unpin(int b) {
  if (!began || b < 0 || b >= kBlocks || !B[b].live || !B[b].pins) return;
  if (--B[b].pins == 0 && B[b].ent < 0 && !B[b].priv) free_block(b);   // an orphan's last reader: freed
}
CORE_COLD void touch(int e) {
  if (!began || e < 0 || e >= kEntries) return;
  E[e].used_ms = now_ms();
  if (E[e].blk >= 0) B[E[e].blk].used_ms = E[e].used_ms;
}
CORE_COLD int alloc_private(uint64_t bytes, void** p) {
  if (!ok()) return -1;
  int b = alloc(bytes, true, true, -1);
  if (b >= 0 && p) *p = data(b);
  return b;
}
CORE_COLD void release(int b) {
  if (began && b >= 0 && b < kBlocks && B[b].live && B[b].priv) free_block(b);
}
CORE_COLD void forget(const lib::Ref& r) {
  int e = find(r, 0);
  if (e < 0 || J.e == e) return;
  if (E[e].filling) {   // a machine writes its block now: dropped when the fill ends
    E[e].drop_after = true;
    return;
  }
  drop_entry(e);
}

CORE_COLD bool step() {
  if (!ok()) return false;
  if (J.e < 0) {
    while (nq > 0 && J.e < 0) {
      int e = Q[0];
      bool pre = Qpre[0];
      sniprintf(J.path, sizeof J.path, "%s", Qpath[0]);
      memmove(Q, Q + 1, sizeof(int) * (nq - 1)), memmove(Qpre, Qpre + 1, sizeof(bool) * (nq - 1));
      memmove(Qpath, Qpath + 1, sizeof Qpath[0] * (nq - 1));
      --nq;
      if (E[e].st == kNone || E[e].st == kFailed || (E[e].st == kReady && E[e].whole)) continue;
      if (E[e].filling) {   // (cannot be: a fill dequeues; kept for safety) after the fill
        E[e].want_whole = true;
        continue;
      }
      J.e = e, J.pre = pre, J.phase = 0, J.blk = -1, J.got = 0, J.failed = false;
      J.rq.state = cardq::kFree;
    }
    if (J.e < 0) return false;
  }
  Ent& x = E[J.e];
  switch (J.phase) {
    case 0:
      if (!cardq::async(&J.rq, cardq::kOpenReq, &J.f, J.path, 0, nullptr, 0)) return true;
      if (J.rq.got != 1) {
        x.nofit = false;
        job_end(false);   // (never opened: nothing to close)
        return true;
      }
      J.phase = 1, J.rq.state = cardq::kFree;
      return true;
    case 1: {
      if (!cardq::async(&J.rq, cardq::kReadReq, &J.f, nullptr, 0, J.buf, kChunk)) return true;
      int n = J.rq.got;
      uint32_t size = J.f.fileSize() > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)J.f.fileSize();
      J.rq.state = cardq::kFree;
      if (n < 12 || !wavread::parse(J.buf, (uint32_t)n, &J.a, size) || J.a.frames < 2) {
        fail_close(false);
        return true;
      }
      J.want = J.a.frames > kMaxFrames ? kMaxFrames : J.a.frames;
      J.ch = J.a.ch >= 2 && !g_mono ? 2 : 1;   // 3+ channels: the first two as L / R
      J.blk = alloc(((uint64_t)J.want + 1) * 2 * J.ch, !J.pre, false, J.e);
      if (J.blk < 0) {
        fail_close(true);
        return true;
      }
      if (x.st == kReady) x.grow = J.blk;
      J.off = J.a.data_off, J.got = 0, J.phase = 2;
      return true;
    }
    case 2: {
      uint32_t fb = wavread::frame_bytes(J.a), fr = kChunk / fb, left = J.want - J.got;
      if (fr > left) fr = left;
      int r = 0;
      if (fr) {
        if (!cardq::async(&J.rq, cardq::kReadReq, &J.f, nullptr, J.off, J.buf, fr * fb)) return true;
        r = J.rq.got;
        J.rq.state = cardq::kFree;
        if (r < 0) {   // a read error: not published (a short file reads short, an error fails)
          J.failed = true, J.phase = 3;
          return true;
        }
      }
      uint32_t w = r > 0 ? (uint32_t)r / fb : 0;
      J.off += w * fb;
      if (J.ch == 2) {   // L R interleaved, each channel as the file has it (16-bit exactly)
        int16_t* d = (int16_t*)data(J.blk) + 2 * J.got;
        wavread::to_stereo16(J.a, J.buf, w, d);
      } else {
        wavread::to_mono(J.a, J.buf, w, J.cv);
        int16_t* d = (int16_t*)data(J.blk) + J.got;
        for (uint32_t i = 0; i < w; ++i) {
          float s = J.cv[i] * 32768.0f;   // (16-bit files exactly as they are)
          d[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
      }
      J.got += w;
      if (w && J.got < J.want) return true;
      if (J.got < 2) J.failed = true;
      J.phase = 3, J.rq.state = cardq::kFree;
      return true;
    }
    default:
      if (!cardq::async(&J.rq, cardq::kCloseReq, &J.f, nullptr, 0, nullptr, 0)) return true;
      job_end(!J.failed);
      return true;
  }
}

Stats stats() {
  Stats s = st;
  s.budget = arena_bytes;
  return s;
}
CORE_COLD void reset_for_tests() {
  if (!began) return;
  for (int i = 0; i < kBlocks; ++i) B[i].live = 0;
  for (int i = 0; i < kEntries; ++i) E[i].st = kNone, E[i].blk = E[i].grow = -1;
  nord = 0, nq = 0;
  J.e = -1;
  memset(&st, 0, sizeof st);
}

}  // namespace pcache
}  // namespace core
#endif
