// core/tests/test_lockfill.cpp - 0.5.0 FILL (notes/design sampler-lock-fill): the pure fill engine (core/seq/lockfill.h)
// on a synthetic pattern record (the dump's layout, every byte the fill does not own random: they must stay as they
// were), every ORDER, the TRIGS / EVERY / OFFSET step choice, a fill's slot (pair) allocation, a fine half added to a
// 7-bit lock, the list compacted, LOCKS FULL, clear, one undo after another track's edit; the 0x5A answer's repack round
// trip; the target registry (core/seq/filltargets.h); the pattern writer (core/seq/patwrite.h) against a fake Rytm:
// read -> write -> verify, a DIRTY between request and answer (read again; PATTERN BUSY), a verify mismatch (the read
// record written back), no answer, STOP TO FILL / allowed while playing, PLAY pressed while reading, SONG BUSY; the FILL
// page through the console and the remote UI (49 1 1 16 / 15, the claim after a hello with caps2 bit 0).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "core.h"
#include "host/core_port.h"
#include "hooks.h"
#include "machines.h"
#include "seq/filltargets.h"
#include "seq/lockfill.h"
#include "seq/patwrite.h"
#include "seq/rytm_dump.h"
#include "msamp/cardq.h"
#include "song/song.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "user/um.h"

using namespace core;
namespace lf = lockfill;
namespace ft = filltargets;

// ---- a synthetic record: tracks of random bytes (their trig masks clear, length 16, velocities default), the slot
// list free, a random tail; then the trigs and slots a case wants
static uint32_t g_r = 12345;
static uint8_t rnd() { return (uint8_t)((g_r = g_r * 1103515245u + 12345u) >> 16); }
static uint8_t* trk(uint8_t* rec, int t) { return rec + 4 + t * lf::kTrackBytes; }
static uint8_t* slot(uint8_t* rec, int i) { return rec + lf::kSlotsAt + i * lf::kSlotBytes; }
static void set_trig(uint8_t* rec, int t, int s) {
  int bit = s * 14 + 13;
  trk(rec, t)[bit >> 3] |= (uint8_t)(1 << (7 - (bit & 7)));
}
static std::vector<uint8_t> make_record() {
  std::vector<uint8_t> r(lf::kRecord);
  for (auto& b : r) b = rnd();
  r[0] = 0, r[1] = 0, r[2] = 0, r[3] = 5;
  for (int t = 0; t < 13; ++t) {
    memset(trk(r.data(), t), 0, 112);   // 64 x 14 trig bits
    memset(trk(r.data(), t) + lf::kVelAt, 0xFF, 64);
    trk(r.data(), t)[lf::kLenAt] = 16;
  }
  for (int i = 0; i < lf::kSlots; ++i) {   // free, as the Rytm keeps them: FF FF, the values 00
    memset(r.data() + lf::kSlotsAt + i * lf::kSlotBytes, 0, lf::kSlotBytes);
    r[lf::kSlotsAt + i * lf::kSlotBytes] = r[lf::kSlotsAt + i * lf::kSlotBytes + 1] = 0xFF;
  }
  return r;
}
static void put_slot(uint8_t* rec, int i, int type, int t, uint8_t fill) {
  memset(slot(rec, i), fill, lf::kSlotBytes);
  slot(rec, i)[0] = (uint8_t)type, slot(rec, i)[1] = (uint8_t)t;
}
// the bytes that differ outside [lo, hi)
static int diff_outside(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int lo, int hi) {
  int n = 0;
  for (int i = 0; i < lf::kRecord; ++i)
    if ((i < lo || i >= hi) && a[i] != b[i]) ++n;
  return n;
}
static const int kSlotLo = lf::kSlotsAt, kSlotHi = lf::kSlotsAt + lf::kSlots * lf::kSlotBytes;

static std::vector<uint16_t> seq_of(int order, int nv, int n, int seed = 7) {
  std::vector<uint16_t> set(nv), out(n);
  for (int i = 0; i < nv; ++i) set[i] = lf::value(i + 1);
  lf::Options o;
  o.order = (uint8_t)order, o.seed = (uint16_t)seed;
  lf::sequence(set.data(), nv, n, o, out.data());
  for (auto& v : out) v = (uint16_t)lf::coarse_of(v);
  return out;
}

// ---- the 0x5A answer around a record
static std::vector<uint8_t> answer_of(const std::vector<uint8_t>& rec, uint8_t id = 0x5A, int pattern = 0) {
  std::vector<uint8_t> m(rytm_dump::kAnswer, 0);
  const uint8_t head[9] = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, id, 0x01, 0x01};
  memcpy(m.data(), head, 9);
  m[9] = (uint8_t)pattern, m[10] = 0;
  memcpy(m.data() + 11, rec.data(), 4);
  int n = rytm_dump::kAnswer;
  m[n - 3] = (uint8_t)((n - 10) >> 7), m[n - 2] = (uint8_t)((n - 10) & 0x7F), m[n - 1] = 0xF7;
  rytm_dump::repack(m.data(), rec.data());
  return m;
}

// ---- a fake Rytm: answers 0x64 n (pattern n: 0x54, byte 9 = n) and 0x6A (pattern X: 0x5A) with the active pattern's
// working copy; a second pattern (other_n) answers its own number. Two copies of the active pattern, as stock keeps them
// [measured, rytm-emu: fill-persist-report.md]: the working (live) one the sequencer plays and 0x6A answers, and the stored
// (compact) one the power-off save keeps. A written 0x5A reaches only the working copy (lost at a power-off); a 0x54 to
// the active pattern reaches both; a 0x54 to another pattern neither of these.
struct Fake {
  std::vector<uint8_t> rec;       // the working copy
  std::vector<uint8_t> stored;    // the stored copy (what a power-off keeps)
  int active = 0;                 // the active pattern (the 2D 02 the host saw: sw())
  std::vector<std::pair<int, int>> wrote;   // each write's dump id and pattern byte
  std::vector<uint8_t> other;     // a second pattern (a queued one): its content, its number (-1 none)
  int other_n = -1;
  bool mixup = false;             // a request for the active pattern answered with the other one (its content and
                                  // number), as a 0x6A in a queued switch's last step was [measured, rytm-emu 1.74]
  bool answer = true;
  int dirty_on_request = 0;       // a DIRTY seen before the next n answers
  bool corrupt = false;           // the written pattern stored with one byte changed (a verify mismatch)
  bool resort = false;            // the written lock list stored in another order (the Rytm sorts it [measured])
  int corrupt_at = -1;            // ... that byte changed (inside the lock list: a verify mismatch there)
  int dirties_on_write = 2;       // a received 0x5A gives two DIRTYs (2D 0B) [measured, rytm-emu fix round]
  bool answer_after_write = true; // false: the verify's requests never answered
  int stale_verify = 0;           // the next n verify answers carry the pattern as before the write (a late answer)
  std::vector<uint8_t> before;    // (the record as before the last write)
  int requests = 0, writes = 0;
  std::vector<uint8_t> in;        // a write being received (pieces)
  std::deque<uint8_t> q;          // bytes to the host
};
static Fake F;
static std::vector<std::vector<uint8_t>> g_out;   // every message to the Rytm
static void out(const uint8_t* m, int n) {
  g_out.emplace_back(m, m + n);
  if (n == 15 && m[0] == 0xF0 && (m[6] == 0x6A || m[6] == 0x64)) {
    ++F.requests;
    if (F.dirty_on_request > 0) --F.dirty_on_request, rytm_dump::dirty();
    if (F.answer && (F.writes == 0 || F.answer_after_write)) {
      bool stale = F.writes > 0 && F.stale_verify > 0;
      if (stale) --F.stale_verify;
      uint8_t id = m[6] == 0x64 ? 0x54 : 0x5A;
      int want = m[6] == 0x64 ? m[9] : F.active;
      std::vector<uint8_t> a;
      if (F.mixup && F.other_n >= 0) a = answer_of(F.other, id, F.other_n);
      else if (want == F.active) a = answer_of(stale ? F.before : F.rec, id, F.active);
      else if (want == F.other_n) a = answer_of(F.other, id, F.other_n);
      F.q.insert(F.q.end(), a.begin(), a.end());
    }
    return;
  }
  bool head = n >= 7 && m[0] == 0xF0 && (m[6] == 0x5A || m[6] == 0x54);
  if (head) F.in.clear();
  if (!F.in.empty() || head) {
    F.in.insert(F.in.end(), m, m + n);
    if (F.in.back() == 0xF7) {
      std::vector<uint8_t> r(lf::kRecord);
      int id = F.in[6], pat = -1;
      CHECK(rytm_dump::parse_id(F.in.data(), (int)F.in.size(), (uint8_t)id, r.data(), &pat));
      F.wrote.emplace_back(id, pat);
      if (F.corrupt) r[100] ^= 1;
      if (F.corrupt_at >= 0) r[F.corrupt_at] ^= 1;
      if (F.resort) {   // the used slots (a fine half with its slot) moved: the last pair first
        std::vector<std::vector<uint8_t>> u;
        for (int i = 0; i < lf::kSlots; ++i)
          if (!lf::slot_free(r.data(), i)) u.emplace_back(slot(r.data(), i), slot(r.data(), i) + 66);
        std::vector<std::vector<uint8_t>> o;
        size_t last = u.size() >= 2 && u.back()[0] == 0x80 ? u.size() - 2 : u.size() - 1;
        for (size_t i = last; i < u.size(); ++i) o.push_back(u[i]);
        for (size_t i = 0; i < last; ++i) o.push_back(u[i]);
        for (size_t i = 0; i < o.size(); ++i) memcpy(slot(r.data(), (int)i), o[i].data(), 66);
      }
      if (id == 0x5A || pat == F.active) F.before = F.rec, F.rec = r;   // the working copy
      if (id == 0x54 && pat == F.active) F.stored = r;                  // ... and the stored one
      if (id == 0x54 && pat == F.other_n) F.other = r;                  // the other pattern's
      ++F.writes;
      for (int k = 0; k < F.dirties_on_write; ++k) rytm_dump::dirty();
      F.in.clear();
    }
  }
}
static uint64_t g_us = 1000000;
static void pump(int ms) {   // the control loop: the Rytm's bytes in, ticks, ms of time
  for (int i = 0; i < ms; ++i) {
    for (int k = 0; k < 4000 && !F.q.empty(); ++k) {
      uint8_t b = F.q.front();
      F.q.pop_front();
      patwrite::byte(b);
      rytm_dump::byte(b);
    }
    g_us += 1000;
    host_fake_time(true, g_us);
    patwrite::tick((uint32_t)(g_us / 1000));
  }
}
static int patch_set(uint8_t* rec, void*) {   // a writer job's patch: one lock on track 1
  uint8_t steps[1] = {0};
  uint16_t v[1] = {lf::value(42)};
  lf::Target g = {lf::kLock, 3, false};
  return lf::fill(rec, 1, g, steps, v, 1) == lf::kOk ? 0 : 1;
}
static void sw(int p) {   // the Rytm's active pattern now p, its 2D 02 seen by the host
  F.active = p;
  rytm_dump::pattern_changed(p);
}
static void writer_reset(const std::vector<uint8_t>& rec, int pattern = 0) {   // pattern -1: no 2D 02 seen yet
  patwrite::reset();
  rytm_dump::reset();
  F = Fake();
  F.rec = F.stored = rec;
  g_out.clear();
  if (pattern >= 0) sw(pattern);
}
// the dump written last, its pieces joined
static std::vector<uint8_t> last_write() {
  std::vector<uint8_t> w;
  for (auto& o : g_out) {
    if (o.size() == 15 && (o[6] == 0x6A || o[6] == 0x64)) continue;   // a request
    if (o[0] == 0xF0) w.clear();
    w.insert(w.end(), o.begin(), o.end());
  }
  return w;
}
static bool sent(const std::vector<uint8_t>& m) {
  for (auto& o : g_out)
    if (o == m) return true;
  return false;
}
static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static void send(const std::vector<uint8_t>& m) { engine_sysex(m.data(), (int)m.size()); }
static std::string cmd(const char* c) {
  char b[512] = "";
  CHECK(ui::fill_command(c, b, sizeof b));
  return b;
}

int main() {
  // ================= the pure engine
  // reading: trigs, length, steps_for (TRIGS, EVERY, OFFSET; past the length and trigless steps left alone)
  {
    auto r = make_record();
    for (int s : {0, 2, 4, 6, 8, 10, 12, 14, 20}) set_trig(r.data(), 3, s);
    CHECK(lf::trig_on(r.data(), 3, 2) && !lf::trig_on(r.data(), 3, 1) && !lf::trig_on(r.data(), 2, 2));
    CHECK(lf::track_len(r.data(), 3) == 16);
    trk(r.data(), 4)[lf::kLenAt] = 0;
    CHECK(lf::track_len(r.data(), 4) == 64);
    lf::Target smp = {lf::kLock, 10, true};
    lf::Options o;
    uint8_t st[64];
    CHECK(lf::steps_for(r.data(), 3, smp, o, st) == 8 && st[0] == 0 && st[7] == 14);   // step 20 past the length
    o.every = 2, o.offset = 1;
    CHECK(lf::steps_for(r.data(), 3, smp, o, st) == 4 && st[0] == 2 && st[1] == 6 && st[3] == 14);
    o.every = 3, o.offset = 0;
    CHECK(lf::steps_for(r.data(), 3, smp, o, st) == 3 && st[0] == 0 && st[1] == 6 && st[2] == 12);
    o = lf::Options();
    CHECK(lf::steps_for(r.data(), 2, smp, o, st) == 0);   // no trigs
    // EMPTY / LOCKED after locks on steps 0 and 4
    uint8_t s2[2] = {0, 4};
    uint16_t v2[2] = {lf::value(5, 0x10), lf::value(6, 0x20)};
    CHECK(lf::fill(r.data(), 3, smp, s2, v2, 2) == lf::kOk);
    o.trigs = lf::kEmpty;
    CHECK(lf::steps_for(r.data(), 3, smp, o, st) == 6 && st[0] == 2);
    o.trigs = lf::kLocked;
    CHECK(lf::steps_for(r.data(), 3, smp, o, st) == 2 && st[0] == 0 && st[1] == 4);
  }
  // every ORDER
  {
    CHECK((seq_of(lf::kUp, 4, 8) == std::vector<uint16_t>{1, 2, 3, 4, 1, 2, 3, 4}));
    CHECK((seq_of(lf::kDown, 4, 8) == std::vector<uint16_t>{4, 3, 2, 1, 4, 3, 2, 1}));
    CHECK((seq_of(lf::kPing, 4, 8) == std::vector<uint16_t>{1, 2, 3, 4, 3, 2, 1, 2}));
    CHECK((seq_of(lf::kPing, 1, 3) == std::vector<uint16_t>{1, 1, 1}));
    CHECK((seq_of(lf::kSprd, 4, 8) == std::vector<uint16_t>{1, 1, 2, 2, 3, 3, 4, 4}));
    CHECK((seq_of(lf::kSprd, 10, 4) == std::vector<uint16_t>{1, 4, 7, 10}));   // FROM first, TO last
    CHECK((seq_of(lf::kSprd, 5, 1) == std::vector<uint16_t>{1}));
    // RND: in the set, the same seed the same, another seed another
    auto a = seq_of(lf::kRnd, 10, 64, 7), b = seq_of(lf::kRnd, 10, 64, 7), c = seq_of(lf::kRnd, 10, 64, 8);
    CHECK(a == b && a != c);
    int seen = 0;
    for (auto v : a) {
      CHECK(v >= 1 && v <= 10);
      seen |= 1 << v;
    }
    CHECK(seen == 0x7FE);   // 64 draws of 10: every one
    // SHUF: each cycle a permutation, seeded
    auto s = seq_of(lf::kShuf, 6, 18, 3);
    for (int cyc = 0; cyc < 3; ++cyc) {
      int m = 0;
      for (int i = 0; i < 6; ++i) m |= 1 << s[cyc * 6 + i];
      CHECK(m == 0x7E);
    }
    CHECK(s == seq_of(lf::kShuf, 6, 18, 3) && s != seq_of(lf::kShuf, 6, 18, 4));
    CHECK(!strcmp(lf::order_name(lf::kSprd), "SPRD") && !strcmp(lf::trigs_name(lf::kLocked), "LOCKED"));
  }
  // a fill: a new slot pair, the values coarse + fine, every byte outside the slot list as it was; again in place
  {
    auto r = make_record();
    put_slot(r.data(), 0, 2, 0, 0x33);   // somebody else's lock
    for (int s = 0; s < 16; s += 2) set_trig(r.data(), 3, s);
    auto r0 = r;
    lf::Target smp = {lf::kLock, 10, true};
    CHECK(lf::slots_after(r.data(), 3, smp) == 3);
    uint8_t st[64];
    lf::Options o;
    int n = lf::steps_for(r.data(), 3, smp, o, st);
    CHECK(n == 8);
    uint16_t set[3] = {lf::value(1, 0), lf::value(17, 0x10), lf::value(127, 0x70)}, vals[8];
    lf::sequence(set, 3, n, o, vals);
    CHECK(lf::fill(r.data(), 3, smp, st, vals, n) == lf::kOk);
    CHECK(diff_outside(r0, r, kSlotLo, kSlotHi) == 0);
    int i = lf::find_slot(r.data(), 10, 3);
    CHECK(i == 1 && lf::has_fine(r.data(), i) && lf::slots_used(r.data()) == 3);
    CHECK(slot(r.data(), 2)[0] == 0x80 && slot(r.data(), 2)[1] == 0x80);
    CHECK(lf::get(r.data(), 3, smp, 2) == lf::value(17, 0x10) && lf::get(r.data(), 3, smp, 4) == lf::value(127, 0x70));
    CHECK((lf::get(r.data(), 3, smp, 1) & 0xFF) == lf::kNoValue);   // a trigless step left alone
    CHECK(memcmp(slot(r.data(), 0), slot(r0.data(), 0), lf::kSlotBytes) == 0);
    auto r1 = r;
    uint16_t again[8];
    for (auto& v : again) v = lf::value(9, 0x30);
    CHECK(lf::fill(r.data(), 3, smp, st, again, n) == lf::kOk && lf::slots_used(r.data()) == 3);
    CHECK(diff_outside(r1, r, kSlotLo + 1 * 66, kSlotLo + 3 * 66) == 0);
    // clear on half the steps: the slot stays; on all: the pair is freed
    CHECK(lf::clear(r.data(), 3, smp, st, 4) == lf::kOk && lf::find_slot(r.data(), 10, 3) == 1);
    CHECK((lf::get(r.data(), 3, smp, 0) & 0xFF) == lf::kNoValue && lf::get(r.data(), 3, smp, 8) == lf::value(9, 0x30));
    CHECK(lf::clear(r.data(), 3, smp, st, n) == lf::kOk && lf::find_slot(r.data(), 10, 3) < 0);
    CHECK(lf::slots_used(r.data()) == 1 && r == r0);
  }
  // a 7-bit target; a fine half inserted after an existing 7-bit lock of the word (the run after it moved down one)
  {
    auto r = make_record();
    set_trig(r.data(), 5, 0), set_trig(r.data(), 5, 1);
    put_slot(r.data(), 0, 10, 5, 0xFF);
    slot(r.data(), 0)[2 + 1] = 40;          // a coarse-only SMP lock on step 1
    put_slot(r.data(), 1, 4, 6, 0x11);
    put_slot(r.data(), 2, 5, 6, 0x22);
    lf::Target smp = {lf::kLock, 10, true};
    CHECK(lf::slots_after(r.data(), 5, smp) == 4);
    uint8_t st[1] = {0};
    uint16_t v[1] = {lf::value(3, 0x20)};
    CHECK(lf::fill(r.data(), 5, smp, st, v, 1) == lf::kOk);
    CHECK(lf::find_slot(r.data(), 10, 5) == 0 && lf::has_fine(r.data(), 0));
    CHECK(lf::get(r.data(), 5, smp, 1) == lf::value(40, 0));   // the old lock plays bank A as before
    CHECK(lf::get(r.data(), 5, smp, 0) == lf::value(3, 0x20));
    CHECK(slot(r.data(), 2)[0] == 4 && slot(r.data(), 2)[2] == 0x11 && slot(r.data(), 3)[0] == 5 && slot(r.data(), 3)[2] == 0x22);
    // 7-bit: no fine half
    lf::Target p1 = {lf::kLock, 0, false};
    uint16_t e[1] = {lf::value(64)};
    CHECK(lf::fill(r.data(), 5, p1, st, e, 1) == lf::kOk);
    int i = lf::find_slot(r.data(), 0, 5);
    CHECK(i >= 0 && !lf::has_fine(r.data(), i) && lf::get(r.data(), 5, p1, 0) == lf::value(64));
  }
  // the list compacted when no two free slots are adjacent; LOCKS FULL changes nothing
  {
    auto r = make_record();
    set_trig(r.data(), 0, 0);
    for (int i = 0; i < lf::kSlots; i += 2) put_slot(r.data(), i, i % 16, (i / 16) + 1, (uint8_t)i);   // 36 used, gaps of 1
    std::vector<std::vector<uint8_t>> before;
    for (int i = 0; i < lf::kSlots; i += 2) before.emplace_back(slot(r.data(), i), slot(r.data(), i) + 66);
    lf::Target smp = {lf::kLock, 10, true};
    uint8_t st[1] = {0};
    uint16_t v[1] = {lf::value(7, 0x10)};
    CHECK(lf::fill(r.data(), 0, smp, st, v, 1) == lf::kOk);
    for (int k = 0; k < 36; ++k) CHECK(memcmp(slot(r.data(), k), before[k].data(), 66) == 0);   // in order, moved up
    CHECK(lf::find_slot(r.data(), 10, 0) == 36 && lf::has_fine(r.data(), 36) && lf::slots_used(r.data()) == 38);
    // full: 71 used, a pair wanted
    auto f = make_record();
    set_trig(f.data(), 0, 0);
    for (int i = 0; i < 71; ++i) put_slot(f.data(), i, i % 16, 1 + i / 16, 0);
    auto f0 = f;
    CHECK(lf::fill(f.data(), 0, smp, st, v, 1) == lf::kFull && f == f0);
    lf::Target p1 = {lf::kLock, 0, false};
    CHECK(lf::fill(f.data(), 0, p1, st, v, 1) == lf::kOk && lf::slots_used(f.data()) == 72);   // one slot: fits
    CHECK(!strcmp(lf::result_text(lf::kFull), "LOCKS FULL (72)"));
  }
  // VELOCITY: the track's array, only the chosen steps; clear back to the default
  {
    auto r = make_record();
    for (int s = 0; s < 16; ++s) set_trig(r.data(), 7, s);
    auto r0 = r;
    lf::Target vel = {lf::kVelocity, 0, false};
    lf::Options o;
    o.every = 4;
    uint8_t st[64];
    int n = lf::steps_for(r.data(), 7, vel, o, st);
    CHECK(n == 4);
    uint16_t v[4] = {lf::value(100), lf::value(1), lf::value(127), lf::value(0)};
    CHECK(lf::fill(r.data(), 7, vel, st, v, n) == lf::kOk);
    int lo = 4 + 7 * lf::kTrackBytes + lf::kVelAt;
    CHECK(diff_outside(r0, r, lo, lo + 64) == 0);
    CHECK(trk(r.data(), 7)[lf::kVelAt + 0] == 100 && trk(r.data(), 7)[lf::kVelAt + 12] == 1);   // 0 written as 1
    CHECK(trk(r.data(), 7)[lf::kVelAt + 1] == 0xFF && lf::slots_used(r.data()) == 0);
    CHECK(lf::clear(r.data(), 7, vel, st, n) == lf::kOk && r == r0);
  }
  // undo: the target as it was put back into the pattern as it is now (another track's edit since stays)
  {
    auto r = make_record();
    for (int s = 0; s < 16; ++s) set_trig(r.data(), 2, s);
    put_slot(r.data(), 0, 10, 2, 0xFF);
    slot(r.data(), 0)[2 + 3] = 50;   // a coarse-only lock the fill will widen
    auto r0 = r;
    lf::Target smp = {lf::kLock, 10, true};
    lf::Saved sv;
    lf::save(r.data(), 2, smp, &sv);
    CHECK(sv.valid && sv.had && !sv.fine);
    uint8_t st[64];
    lf::Options o;
    int n = lf::steps_for(r.data(), 2, smp, o, st);
    std::vector<uint16_t> vals(n, lf::value(9, 0x40));
    CHECK(lf::fill(r.data(), 2, smp, st, vals.data(), n) == lf::kOk && lf::has_fine(r.data(), 0));
    // the user edits track 8 meanwhile
    put_slot(r.data(), 5, 1, 8, 0x44);
    auto edited = r0;
    put_slot(edited.data(), 5, 1, 8, 0x44);
    CHECK(lf::restore(r.data(), sv) == lf::kOk);
    CHECK(r == edited);   // the fine half freed in place, the lock as it was, the other edit kept
    // a fill that made the slot: undone, the slot goes
    auto q = make_record();
    set_trig(q.data(), 1, 0);
    auto q0 = q;
    lf::save(q.data(), 1, smp, &sv);
    CHECK(!sv.had);
    uint16_t v1[1] = {lf::value(4, 0x10)};
    uint8_t s1[1] = {0};
    CHECK(lf::fill(q.data(), 1, smp, s1, v1, 1) == lf::kOk && lf::slots_used(q.data()) == 2);
    CHECK(lf::restore(q.data(), sv) == lf::kOk && q == q0);
    // a lock pair cleared since the save: it comes back
    auto w = make_record();
    set_trig(w.data(), 1, 0);
    CHECK(lf::fill(w.data(), 1, smp, s1, v1, 1) == lf::kOk);
    auto w0 = w;
    lf::save(w.data(), 1, smp, &sv);
    CHECK(lf::clear(w.data(), 1, smp, s1, 1) == lf::kOk && lf::slots_used(w.data()) == 0);
    CHECK(lf::restore(w.data(), sv) == lf::kOk && w == w0);
    // velocity
    auto u = make_record();
    lf::Target vel = {lf::kVelocity, 0, false};
    lf::save(u.data(), 4, vel, &sv);
    auto u0 = u;
    uint16_t v100[1] = {lf::value(100)};
    CHECK(lf::fill(u.data(), 4, vel, s1, v100, 1) == lf::kOk && u != u0);
    CHECK(lf::restore(u.data(), sv) == lf::kOk && u == u0);
    CHECK(lf::restore(u.data(), lf::Saved()) == lf::kNothing);
  }
  // the 0x5A answer: parse, repack with a changed record, parse again; the header bytes kept, the checksum right
  {
    auto r = make_record();
    for (int i = 0; i < 300; ++i) r[4 + rnd() % (lf::kRecord - 4)] |= 0x80;   // top bits set too
    auto m = answer_of(r);
    std::vector<uint8_t> back(lf::kRecord);
    int pat = -1;
    CHECK(rytm_dump::parse_id(m.data(), (int)m.size(), 0x5A, back.data(), &pat) && back == r && pat == 0);
    CHECK(!rytm_dump::parse_id(m.data(), (int)m.size(), 0x54, back.data(), nullptr));
    auto m0 = m;
    r[5000] ^= 0x81, r[lf::kRecord - 1] ^= 0x7F;
    rytm_dump::repack(m.data(), r.data());
    CHECK(memcmp(m.data(), m0.data(), 15) == 0 && m[m.size() - 1] == 0xF7);
    CHECK(rytm_dump::parse_id(m.data(), (int)m.size(), 0x5A, back.data(), nullptr) && back == r);
    m[2000] ^= 1;
    CHECK(!rytm_dump::parse_id(m.data(), (int)m.size(), 0x5A, back.data(), nullptr));   // the checksum
  }

  // a captured pattern (rytm-emu, OS 1.73 + 0026, A01 working buffer; tests/golden/lockfill): the layout as measured
  // (14 slots, BD 16 steps, LT 64), a fill of BD's SMP + VELOCITY with the real code packs to exactly what was sent to
  // the emulator, and its read-back - the lock list re-sorted by the Rytm - is the same pattern by lockfill::same
  {
    auto load = [](const char* f) {
      std::vector<uint8_t> m(rytm_dump::kAnswer);
      FILE* fp = fopen(f, "rb");
      CHECK(fp && fread(m.data(), 1, m.size(), fp) == m.size());
      fclose(fp);
      return m;
    };
    auto m = load("tests/golden/lockfill/a01_read.sysex"), w = load("tests/golden/lockfill/a01_written.sysex"),
         b = load("tests/golden/lockfill/a01_back.sysex");
    std::vector<uint8_t> r(lf::kRecord), rb(lf::kRecord), rw(lf::kRecord);
    CHECK(rytm_dump::parse_id(m.data(), (int)m.size(), 0x5A, r.data(), nullptr));
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 5);
    CHECK(lf::slots_used(r.data()) == 14 && lf::track_len(r.data(), 0) == 16 && lf::track_len(r.data(), 3) == 64);
    CHECK(lf::track_len(r.data(), 9) == 18 && lf::trig_on(r.data(), 1, 4) && !lf::trig_on(r.data(), 1, 0));
    CHECK(lf::find_slot(r.data(), 5, 1) == 2 && lf::has_fine(r.data(), 2) && slot(r.data(), 16)[0] == 0xFF);
    lf::Target smp = {lf::kLock, 10, true}, vel = {lf::kVelocity, 0, false};
    uint8_t st[4] = {0, 4, 8, 12};
    uint16_t v[4] = {lf::value(10, 0x20), lf::value(11, 0x20), lf::value(12, 0x20), lf::value(13, 0x20)};
    uint16_t vv[4] = {lf::value(77), lf::value(90), lf::value(100), lf::value(127)};
    auto r0 = r;
    CHECK(lf::fill(r.data(), 0, smp, st, v, 4) == lf::kOk && lf::fill(r.data(), 0, vel, st, vv, 4) == lf::kOk);
    int lo = 4 + lf::kVelAt;
    for (int i = 0; i < lf::kRecord; ++i)
      if (r[i] != r0[i]) CHECK((i >= kSlotLo && i < kSlotHi) || (i >= lo && i < lo + 64));
    auto out = m;
    rytm_dump::repack(out.data(), r.data());
    CHECK(out == w);
    CHECK(rytm_dump::parse_id(b.data(), (int)b.size(), 0x5A, rb.data(), nullptr));
    CHECK(rb != r && lf::same(rb.data(), r.data()) && lf::same(r.data(), rb.data()));
    CHECK(lf::get(rb.data(), 0, smp, 8) == lf::value(12, 0x20) && rb[lo + 12] == 127);
    CHECK(!lf::same(rb.data(), r0.data()));
    auto rc = rb;
    rc[lo + 1] = 5;
    CHECK(!lf::same(rc.data(), r.data()));   // a byte outside the list
    rc = rb;
    slot(rc.data(), lf::find_slot(rc.data(), 10, 0) + 1)[2 + 4] = 0x30;
    CHECK(!lf::same(rc.data(), r.data()));   // a fine value
    // L3: a lock more, a lock less
    rc = rb;
    int fr = 0;
    while (!lf::slot_free(rc.data(), fr)) ++fr;
    put_slot(rc.data(), fr, 3, 7, 0x40);
    CHECK(!lf::same(rc.data(), rb.data()) && !lf::same(rb.data(), rc.data()));
    // L1: an orphan fine half on one side hides nothing (same() is both ways): a's orphan, b's lock in its place
    auto oa = rb, ob = rb;
    put_slot(oa.data(), fr + 1, 0x80, 0x80, 0x10);   // (after a free slot: nobody's fine half)
    put_slot(ob.data(), fr + 1, 3, 7, 0x10);
    CHECK(lf::slots_used(oa.data()) == lf::slots_used(ob.data()));
    CHECK(!lf::same(oa.data(), ob.data()) && !lf::same(ob.data(), oa.data()));
    // L5: the pattern's mode and master length in its tail [measured, rytm-emu fix round: 0026's SCALE report read
    // len 32 mode 0 after tail +0..1 = 00 20, +6 = 00 were written]: ADVANCED (1, this pattern) the tracks' own lengths;
    // NORMAL (0) the master length for every track (at most 64 steps here)
    const int tail = lf::kRecord - 12;
    CHECK(r0[tail + 6] == 1 && r0[tail] == 0 && r0[tail + 1] == 1);
    auto nm = r0;
    nm[tail + 6] = 0, nm[tail] = 0, nm[tail + 1] = 32;
    for (int t = 0; t < 12; ++t) CHECK(lf::track_len(nm.data(), t) == 32);
    nm[tail] = 1, nm[tail + 1] = 0;   // 256
    CHECK(lf::track_len(nm.data(), 9) == 64);
    nm[tail] = 0, nm[tail + 1] = 0;   // (not a length: the track's own)
    CHECK(lf::track_len(nm.data(), 9) == 18);
  }

  // ================= the registry
  {
    ft::Def d[ft::kMax];
    int n = ft::for_machine(-1, 0, d, ft::kMax);
    CHECK(n == 1 && !strcmp(d[0].label, "VELOCITY") && d[0].g.where == lf::kVelocity && d[0].order == lf::kRnd);
    n = ft::for_machine(kPlaits, 0, d, ft::kMax);
    CHECK(n == 2 && !strcmp(d[0].label, "ENGINE") && d[0].g.type == 0 && !d[0].g.fine && d[0].n == kPlaitsEngs);
    CHECK(d[0].order == lf::kUp && !strcmp(d[1].label, "VELOCITY"));
    for (int i = 0; i < d[0].n; ++i) CHECK(ft::index_of(d[0], ft::encode(d[0], i)) == i);
    char b[48];
    CHECK(ft::name(d[0], 0, b, sizeof b)[0]);
    n = ft::for_machine(kDaisy, 0, d, ft::kMax);
    CHECK(n == 2 && d[0].n == 3 && !strcmp(ft::name(d[0], 1, b, sizeof b), "STRING"));
    for (int i = 0; i < 3; ++i) CHECK(ft::index_of(d[0], ft::encode(d[0], i)) == i);
    n = ft::for_machine(kJuno, 0, d, ft::kMax);
    CHECK(n == 2 && !strcmp(d[0].label, "PRESET"));
    n = ft::for_machine(kFm, 0, d, ft::kMax);
    CHECK(n == 2 && !strcmp(d[0].label, "PRESET"));
#if CORE_MSAMP
    for (int m : {(int)kDspSample, kSmplMachine}) {
      n = ft::for_machine(m, 0, d, ft::kMax);
      CHECK(n == 2 && !strcmp(d[0].label, "SAMPLE") && d[0].g.type == 10 && d[0].g.fine && d[0].n == 1024);
    }
    CHECK(ft::encode(d[0], 2 * 128 + 5) == lf::value(5, 0x20) && ft::index_of(d[0], lf::value(5, 0x20)) == 261);
    CHECK(ft::skip(d[0], 0) && ft::skip(d[0], 128));   // row 000 of a bank: OFF
    n = ft::for_machine(kGranMachine, 0, d, ft::kMax);
    CHECK(n == 2 && !strcmp(d[0].label, "SOURCE") && d[0].g.type == 0 && d[0].g.fine);
#endif
#if CORE_MD
    n = ft::for_machine(kMd, 1, d, ft::kMax);
    CHECK(n == 3 && !strcmp(d[0].label, "ENGINE") && d[0].g.type == 8 && !strcmp(d[1].label, "TYPE") && d[1].g.type == 9);
    CHECK(d[1].arg == 1);
    for (int i = 0; i < d[1].n; ++i) CHECK(ft::index_of(d[1], ft::encode(d[1], i)) == i);
#endif
    // L3: a user machine's targets from its knobs (um::fill_knobs' kinds): the preset knob (its presets), a pool knob
    // (SAMPLE: bank A's rows, 7-bit), a list knob (its long name; zones of its names); VELOCITY last, at most kMax
    {
      um::FillKnob k[4] = {};
      k[0].p = 1, k[0].kind = um::kFillPreset;
      k[1].p = 3, k[1].kind = um::kFillPool;
      k[2].p = 5, k[2].kind = um::kFillList, k[2].count = 4;
      snprintf(k[2].label, sizeof k[2].label, "SHAPE");
      k[3].p = 6, k[3].kind = um::kFillList, k[3].count = 1;   // one name: nothing to fill
      n = ft::for_knobs(2, k, 4, 10, d, ft::kMax);
#if CORE_MSAMP
      CHECK(n == 4);
      CHECK(!strcmp(d[1].label, "SAMPLE") && d[1].g.type == 2 && !d[1].g.fine && d[1].n == 128 && d[1].values == ft::kPool);
      CHECK(ft::encode(d[1], 5) == lf::value(5) && ft::index_of(d[1], lf::value(5)) == 5);
#else
      CHECK(n == 3);
#endif
      CHECK(!strcmp(d[0].label, "PRESET") && d[0].g.type == 0 && d[0].n == 10 && d[0].values == ft::kUserPresets && d[0].arg == 2);
      const ft::Def& sh = d[n - 2];
      CHECK(!strcmp(sh.label, "SHAPE") && sh.g.type == 4 && sh.p1 == 5 && sh.n == 4 && sh.values == ft::kUserZones);
      for (int i = 0; i < 4; ++i) CHECK(ft::index_of(sh, ft::encode(sh, i)) == i);
      CHECK(!strcmp(d[n - 1].label, "VELOCITY"));
      CHECK(ft::for_knobs(2, k, 4, 0, d, ft::kMax) == n - 1);   // no presets on the card: no PRESET
      um::FillKnob many[6] = {};
      for (int i = 0; i < 6; ++i) many[i].p = (uint8_t)(i + 1), many[i].kind = um::kFillList, many[i].count = 3;
      CHECK(ft::for_knobs(0, many, 6, 0, d, ft::kMax) == ft::kMax && !strcmp(d[ft::kMax - 1].label, "VELOCITY"));
    }
    // VELOCITY's values: 1..127; FROM above TO: downwards
    n = ft::for_machine(-1, 0, d, ft::kMax);
    uint16_t set[200];
    CHECK(ft::value_set(d[0], 59, 126, set, 200) == 68 && set[0] == lf::value(60) && set[67] == lf::value(127));
    CHECK(ft::value_set(d[0], 4, 2, set, 200) == 3 && set[0] == lf::value(5) && set[2] == lf::value(3));
    CHECK(ft::index_of(d[0], lf::value(1)) == 0 && ft::index_of(d[0], 0xFF) == -1);
    CHECK(ft::step(d[0], 126, 1) == 126 && ft::step(d[0], 0, -1) == 0 && ft::step(d[0], 5, 1) == 6);
  }

  // ================= the pattern writer against a fake Rytm
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  {
    auto r = make_record();
    // read -> write -> verify
    writer_reset(r);
    CHECK(patwrite::start(patch_set, nullptr) && patwrite::busy());
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && patwrite::changed() && F.writes == 1 && F.requests == 2);
    auto want = r;
    CHECK(patch_set(want.data(), nullptr) == 0 && F.rec == want);
    CHECK(diff_outside(r, F.rec, kSlotLo, kSlotHi) == 0);
    CHECK(patwrite::before() && memcmp(patwrite::before(), r.data(), lf::kRecord) == 0);
    CHECK(patwrite::stats().verifies == 1 && patwrite::stats().restarts == 0);
    // the written message: the answer as read with the record repacked, in pieces of at most 3069
    bool pieces = true;
    for (auto& o : g_out)
      if (o[0] == 0xF0 && o.size() > 7 && o[6] == 0x54) pieces &= o.size() == 3069;
    CHECK(pieces);
    // fix/fill-persist (the owner, OS 1.74: a fill lost after SAVE PATTERN and a power-off): the write is a 0x54 to the
    // active pattern, which reaches the stored copy a power-off keeps too [measured, rytm-emu]; a 0x5A reached only the
    // working copy
    CHECK(F.stored == F.rec && F.stored == want);
    CHECK(F.wrote.size() == 1 && F.wrote[0] == std::make_pair(0x54, 0));
    // nothing to change: done, nothing written
    uint32_t jobs = patwrite::jobs();
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && !patwrite::changed() && F.writes == 1 && patwrite::jobs() == jobs + 1);
    {   // the message: 0x54, byte 9 the active pattern (another than 0: A02 .. here 13), its checksum and length right
      writer_reset(r, 13);
      CHECK(patwrite::start(patch_set, nullptr));
      pump(400);
      CHECK(patwrite::state() == patwrite::kDone && F.rec == want && F.stored == want);
      auto w = last_write();
      int n = (int)w.size();
      CHECK(n == rytm_dump::kAnswer && w[6] == 0x54 && w[9] == 13 && w[n - 1] == 0xF7);
      uint32_t sum = 0;   // the Rytm's sum: the bytes after the pattern byte, to the checksum
      for (int i = 10; i < n - 5; ++i) sum += w[i];
      CHECK(((w[n - 5] << 7) | w[n - 4]) == (int)(sum & 0x3FFF) && ((w[n - 3] << 7) | w[n - 2]) == n - 10);
      std::vector<uint8_t> got(lf::kRecord);
      int pat = -1;
      CHECK(rytm_dump::parse_id(w.data(), n, 0x54, got.data(), &pat) && pat == 13 && got == want);
      auto a = answer_of(want, 0x54, 13);   // every other byte as the read answer's, with the record repacked
      for (int i = 0; i < n; ++i)
        CHECK(w[i] == a[i]);
    }
    // no 2D 02 seen yet (the active pattern unknown): nothing read, nothing written - PATTERN UNKNOWN (a 0x5A instead
    // would be the volatile write again)
    writer_reset(r, -1);
    CHECK(!patwrite::start(patch_set, nullptr) && patwrite::why() == patwrite::kWhyNoPattern && !patwrite::busy());
    pump(400);
    CHECK(F.requests == 0 && F.writes == 0 && g_out.empty());
    CHECK(!strcmp(patwrite::why_text(patwrite::kWhyNoPattern), "PATTERN UNKNOWN"));
    // fix round 1 (review M1): the read names its pattern - 0x64 n, n the active one - and only an answer whose byte 9
    // is n is taken; so the written 0x54 is that pattern's own read, by construction
    writer_reset(r, 13);
    CHECK(patwrite::start(patch_set, nullptr));
    CHECK(!g_out.empty() && g_out[0].size() == 15 && g_out[0][6] == 0x64 && g_out[0][9] == 13);
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && F.rec == want && F.stored == want);
    for (auto& o : g_out)
      if (o.size() == 15) CHECK(o[6] == 0x64 && o[9] == 13);   // the verify's read too
    {   // two patterns: A01 plays, A02 queued; the Rytm answers the request for A01 with A02 (its content, byte 9 = 1),
        // as a 0x6A in the queue's last step did [measured, rytm-emu 1.74 sq fillqueue]: never taken, nothing written
        // to A01 or A02 (NO ANSWER after the tries)
      writer_reset(r);
      auto q = r;
      set_trig(q.data(), 0, 6);
      F.other = q, F.other_n = 1, F.mixup = true;
      CHECK(patwrite::start(patch_set, nullptr));
      pump(9000);
      CHECK(patwrite::why() == patwrite::kWhyNoAnswer && F.writes == 0 && F.requests == patwrite::kTries);
      CHECK(F.rec == r && F.stored == r && F.other == q);
      // ... the other pattern's answer in the verify: not taken for it (no verify answer: the read one back to A01
      // only, as the writer does for an unconfirmed write; A02 untouched)
      writer_reset(r);
      F.other = q, F.other_n = 1;
      CHECK(patwrite::start(patch_set, nullptr));
      pump(30);
      CHECK(patwrite::state() == patwrite::kSettle && F.writes == 1);
      F.mixup = true;
      pump(9000);
      CHECK(patwrite::why() == patwrite::kWhyVerify && F.rec == r && F.stored == r && F.other == q);
      for (auto& w : F.wrote) CHECK(w == std::make_pair(0x54, 0));
    }
    {   // fix round 2 (re-review gap): a switch while reading restarts the read at once with the new number (no answer
        // needed); kReads reads at most, then PATTERN BUSY, nothing written
      writer_reset(r);
      F.answer = false;
      CHECK(patwrite::start(patch_set, nullptr));
      sw(3);
      pump(5);
      sw(4);
      pump(5);
      std::vector<int> named;
      for (auto& o : g_out)
        if (o.size() == 15 && o[6] == 0x64) named.push_back(o[9]);
      CHECK((named == std::vector<int>{0, 3, 4}) && patwrite::busy() && patwrite::stats().restarts == 2);
      sw(5);
      pump(5);
      CHECK(patwrite::why() == patwrite::kWhyBusy && F.writes == 0 && F.requests == 3);
    }
    // a DIRTY between the request and its answer: read again
    writer_reset(r);
    F.dirty_on_request = 1;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && patwrite::stats().restarts == 1 && F.writes == 1 && F.rec == want);
    // three in a row: PATTERN BUSY, nothing written
    writer_reset(r);
    F.dirty_on_request = 3;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kFailed && patwrite::why() == patwrite::kWhyBusy && F.writes == 0 && F.rec == r);
    CHECK(!strcmp(patwrite::why_text(patwrite::kWhyBusy), "PATTERN BUSY"));
    // a pattern change meanwhile: the same guard
    writer_reset(r);
    sw(3);
    CHECK(patwrite::start(patch_set, nullptr));
    sw(4);
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && patwrite::stats().restarts == 1);
    // the verify differs: the read pattern goes back
    writer_reset(r);
    F.corrupt = true;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kFailed && patwrite::why() == patwrite::kWhyVerify && F.writes == 2);
    auto rc = r;
    rc[100] ^= 1;   // (the fake corrupts the restore too: what the host sent was the original)
    CHECK(F.rec == rc && patwrite::stats().mismatches == 1 && patwrite::stats().restores == 1);
    // the restore: a 0x54 to the active pattern as well (a 0x5A would leave the fill in the stored copy: back at a power-off)
    CHECK(F.stored == rc && F.wrote.size() == 2 && F.wrote[1] == std::make_pair(0x54, 0));
    // the Rytm's own lock order on the read-back: still the same locks, done
    writer_reset(r);
    put_slot(F.rec.data(), 0, 4, 9, 0x10);
    F.resort = true;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && patwrite::stats().mismatches == 0 && lf::find_slot(F.rec.data(), 3, 1) == 0);
    // ---- fix round H1: after the read's answer. A received 0x5A gives two DIRTYs of its own (the fake does as the Rytm),
    // so after the write only a pattern change, or more DIRTYs than those, mean the user's pattern moved under us.
    // a verify mismatch inside the lock list: the read pattern back (nothing else changed)
    writer_reset(r);
    F.corrupt_at = kSlotLo + 2;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyVerify && F.writes == 2 && patwrite::stats().restores == 1);
    // a pattern change while settling: no write-back, CHANGED MEANWHILE
    writer_reset(r);
    CHECK(patwrite::start(patch_set, nullptr));
    pump(30);
    CHECK(patwrite::state() == patwrite::kSettle && F.writes == 1);
    sw(7);
    pump(400);
    CHECK(patwrite::state() == patwrite::kFailed && patwrite::why() == patwrite::kWhyChanged && F.writes == 1);
    CHECK(patwrite::stats().restores == 0 && !strcmp(patwrite::why_text(patwrite::kWhyChanged), "CHANGED MEANWHILE: CHECK"));
    // ... while the verify is asked (its answer then is another pattern's): the same
    writer_reset(r);
    F.answer_after_write = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(200);
    CHECK(patwrite::state() == patwrite::kVerifying);
    sw(8);
    F.answer_after_write = true;
    pump(2000);
    CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    // a mismatch with the user's edits meanwhile (more DIRTYs than the write's own): theirs kept, no write-back
    writer_reset(r);
    F.corrupt = true;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(30);
    rytm_dump::dirty();
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    // N2 (re-review): restore only when the DIRTYs since the write are exactly kOwnDirties: a write the Rytm did not
    // (fully) take (0 or 1 DIRTYs) is not put back over whatever the user may have edited
    for (int own = 0; own < 2; ++own) {
      writer_reset(r);
      F.corrupt = true, F.dirties_on_write = own;
      CHECK(patwrite::start(patch_set, nullptr));
      pump(400);
      CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    }
    writer_reset(r);   // ... and a verify that never answers: NOT CONFIRMED, nothing written back
    F.answer_after_write = false, F.dirties_on_write = 1;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(9000);
    CHECK(patwrite::why() == patwrite::kWhyUnconfirmed && F.writes == 1 && patwrite::stats().restores == 0);
    // the verify never answered, nothing changed: the read pattern written back (VERIFY FAILED: RESTORED)
    writer_reset(r);
    F.answer_after_write = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(9000);
    CHECK(patwrite::why() == patwrite::kWhyVerify && F.writes == 2 && F.rec == r && patwrite::stats().restores == 1);
    CHECK(F.stored == r && F.wrote.size() == 2 && F.wrote[1] == std::make_pair(0x54, 0));
    // ... with an edit meanwhile: NOT CONFIRMED, nothing written back
    writer_reset(r);
    F.answer_after_write = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(1000);
    rytm_dump::dirty();
    pump(9000);
    CHECK(patwrite::why() == patwrite::kWhyUnconfirmed && F.writes == 1 && patwrite::stats().restores == 0);
    CHECK(!strcmp(patwrite::why_text(patwrite::kWhyUnconfirmed), "NOT CONFIRMED: CHECK IT"));
    // ... with a pattern change meanwhile: nothing written back either (the switch ends the job at once: CHANGED)
    writer_reset(r);
    F.answer_after_write = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(1000);
    sw(9);
    pump(9000);
    CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    // ... a verify answer arriving after a pattern change, before a tick saw the change: another pattern's answer
    // (even one equal to what was written): CHANGED, not DONE
    writer_reset(r);
    F.answer_after_write = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(200);
    CHECK(patwrite::state() == patwrite::kVerifying);
    sw(10);
    {
      auto a = answer_of(F.rec, 0x54);
      for (uint8_t b : a) patwrite::byte(b);
    }
    CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    // L2: a late answer (the pattern as before the write) taken for the verify: asked once more, then DONE
    writer_reset(r);
    F.stale_verify = 1;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && F.writes == 1 && patwrite::stats().verifies == 2);
    // ... twice: the Rytm did not take it: it still has the pattern as read, so nothing is written back (a restore
    // could only overwrite an edit made meanwhile): NOT TAKEN
    writer_reset(r);
    F.stale_verify = 5;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyNotTaken && F.writes == 1 && F.rec != r && patwrite::stats().restores == 0);
    CHECK(!strcmp(patwrite::why_text(patwrite::kWhyNotTaken), "NOT TAKEN: UNCHANGED"));
    // M1: a song dump answer arriving while the verify waits, with the song's hold already up (before the next tick):
    // the job ends SONG BUSY on that answer, nothing written back (it may be the song's answer, not ours)
    writer_reset(r);
    F.answer_after_write = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(200);
    CHECK(patwrite::state() == patwrite::kVerifying);
    {
      auto other = F.rec;
      other[100] ^= 1;
      auto a = answer_of(other, 0x54);
      rytm_dump::set_hold(2);
      for (uint8_t b : a) patwrite::byte(b);   // (the whole answer before a tick)
    }
    CHECK(patwrite::why() == patwrite::kWhySong && F.writes == 1 && patwrite::stats().restores == 0);
    rytm_dump::set_hold(0);
    // M1: the song's own dumps start mid-job (hold 2): the job ends SONG BUSY, nothing written back
    writer_reset(r);
    CHECK(patwrite::start(patch_set, nullptr));
    pump(30);
    rytm_dump::set_hold(2);
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhySong && F.writes == 1 && patwrite::stats().restores == 0);
    rytm_dump::set_hold(0);
    // ... during the read: dropped, nothing written
    writer_reset(r);
    F.answer = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(10);
    rytm_dump::set_hold(2);
    F.answer = true;
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhySong && F.writes == 0);
    rytm_dump::set_hold(0);
    // the job's read's pattern count (M2: the undo's)
    writer_reset(r);
    sw(11);
    uint32_t sw0 = rytm_dump::switch_count();
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    sw(12);
    CHECK(patwrite::read_switch() == sw0);
    // no answer: 5 tries, NO ANSWER, nothing written
    writer_reset(r);
    F.answer = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(8000);
    CHECK(patwrite::state() == patwrite::kFailed && patwrite::why() == patwrite::kWhyNoAnswer && F.requests == 5 && F.writes == 0);
    // a failing patch: nothing written, its number kept
    writer_reset(r);
    CHECK(patwrite::start([](uint8_t*, void*) { return (int)lf::kFull; }, nullptr));
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyPatch && patwrite::patch_result() == lf::kFull && F.writes == 0);
    // playing: STOP TO FILL; allowed with play_ok (E4)
    writer_reset(r);
    rytm_dump::set_playing(true);
    CHECK(!patwrite::start(patch_set, nullptr) && patwrite::why() == patwrite::kWhyPlaying && F.requests == 0);
    patwrite::set_play_ok(true);
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && F.writes == 1);
    patwrite::set_play_ok(false);
    rytm_dump::set_playing(false);
    // PLAY pressed while reading: dropped before anything is written
    writer_reset(r);
    F.answer = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(10);
    rytm_dump::set_playing(true);
    pump(10);
    CHECK(patwrite::state() == patwrite::kFailed && patwrite::why() == patwrite::kWhyPlayed && F.writes == 0);
    rytm_dump::set_playing(false);
    // the song's own dumps: SONG BUSY; a job running: refused
    writer_reset(r);
    rytm_dump::set_hold(2);
    CHECK(!patwrite::start(patch_set, nullptr) && patwrite::why() == patwrite::kWhySong);
    rytm_dump::set_hold(0);
    F.answer = false;
    CHECK(patwrite::start(patch_set, nullptr) && !patwrite::start(patch_set, nullptr) && patwrite::why() == patwrite::kWhyRunning);
    writer_reset(r);
  }

  // ================= the FILL page and the remote UI
  {
    char tmpl[] = "/tmp/core_lockfill_XXXXXX";
    CHECK(mkdtemp(tmpl));
    std::string card = tmpl;
    CHECK(system(("mkdir -p '" + card + "/Samples' '" + card + "/.rytm'").c_str()) == 0);
    host_set_card(tmpl);
    CHECK(engine_begin() && card_ok);
    // a hello with caps2 bit 0: the host claims UP + LEFT / RIGHT; without it, no claim
    const std::vector<uint8_t> claim = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 0x01, 0x7E, 0x01, 0xF7};
    g_out.clear();
    send(msg(0x35, {0, 5, 127}));
    CHECK(!sent(claim) && ui::remote_caps2() == 0);
    send(msg(0x35, {0, 6, 127, ui::kCaps2Arrows}));
    CHECK(sent(claim) && ui::remote_caps2() == ui::kCaps2Arrows);
    // fix round (fill-keys review L1): FILL's keys off (fill=keys:off) drops the claim (4A 7E 0) and no hello claims
    // again until fill=keys:on; the host going down (a planned reboot / power-off: remote_going_down, any core) drops
    // it at the control core's next tick
    {
      const std::vector<uint8_t> unclaim = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 0x01, 0x7E, 0x00, 0xF7};
      g_out.clear();
      cmd("fill=keys:off");
      CHECK(sent(unclaim) && !sent(claim));
      g_out.clear();
      send(msg(0x35, {0, 6, 127, ui::kCaps2Arrows}));
      CHECK(!sent(claim));
      cmd("fill=keys:off");   // (already off: nothing again)
      CHECK(!sent(unclaim));
      cmd("fill=keys:on");
      CHECK(sent(claim));
      g_out.clear();
      ui::remote_going_down();
      CHECK(!sent(unclaim));   // (not from the caller's core: at the tick)
      ui::remote_tick(1000);
      CHECK(sent(unclaim));
      g_out.clear();
      send(msg(0x35, {0, 6, 127, ui::kCaps2Arrows}));
      CHECK(!sent(claim));     // going down: no claim after it
      // N4 (re-review): a hello without caps2 (an older mod) drops the claim state: a later release sends no 4A 7E 0
      cmd("fill=keys:on");     // (claimed on the new mod)
      CHECK(sent(claim));
      g_out.clear();
      send(msg(0x35, {0, 5, 127}));   // the older mod: the Rytm dropped the claim at its hello
      cmd("fill=keys:off");
      CHECK(!sent(unclaim));
      cmd("fill=keys:on");     // (no caps2 now: no claim)
      CHECK(!sent(claim));
      send(msg(0x35, {0, 6, 127, ui::kCaps2Arrows}));   // (the tests go on with the keys claimed)
      CHECK(sent(claim));
    }
    // the tracks: BD PLAITS, the rest Rytm machines
    std::vector<uint8_t> a33 = {0, 12};
    a33.push_back(1), a33.push_back(0x00);
    for (int t = 1; t < 12; ++t) a33.push_back(1), a33.push_back(0x7F);
    send(msg(0x33, a33));
    CHECK(ft::track_machine(0) == kPlaits && ft::track_machine(1) == -1);
    // 49 1 1 16: the FILL page opens (the selected track's; none known: SELECT A TRACK FIRST on YES)
    ui::fill_page_reset();
    send(msg(0x49, {1, ui::kPageFill}));
    CHECK(ui::fill_page_view().on);
    // 49 1 1 15: GENERATOR, a popup only
    ui::fill_page_reset();
    send(msg(0x49, {1, ui::kPageGenerator}));
    CHECK(!ui::fill_page_view().on && !strcmp(ui::remote_popup_text(), "GENERATOR: 0.5.1"));   // L3: its text
    // the console: open on BD, ENGINE first (UP, the first .. last engine), VELOCITY 60..127 RND; a fresh seed
    cmd("fill=open:0");
    auto v = ui::fill_page_view();
    CHECK(v.on && v.track == 0 && v.ntargets == 2 && v.target == 0 && v.order == lf::kUp && v.from == 0 && v.to == kPlaitsEngs - 1);
    CHECK(v.trigs == lf::kAll && v.every == 1 && v.offset == 0 && v.seed >= 0 && v.seed < 1000 && !v.undo);
    {   // the owner's test (2026-11-13): each control's label starts with its encoder's letter (A..H, the Rytm's knobs);
        // the left column (A..D, x 2) within 13 characters, the right (E..H, x 66) within 12: at 5 px a character
        // (kRSmall's widest) neither runs into the other column nor off the screen
      static ui::Canvas c;
      auto drawn = [&](const char* want_a) {
        ui::kFillPage.draw(c, 0, 1, now_ms());
        for (int i = 0; i < 8; ++i) {
          const char* t = c.txt[ui::kSlotFree + i].s;
          CHECK(t[0] == "ABCDEFGH"[i] && t[1] == ' ' && (int)strlen(t) <= (i < 4 ? 13 : 12));
          CHECK(c.txt[ui::kSlotFree + i].x == (i < 4 ? 2 : 66));
        }
        CHECK(!strcmp(c.txt[ui::kSlotFree].s, want_a));
        printf("lockfill: FILL's labels: %s | %s | %s | %s || %s | %s | %s | %s\n", c.txt[ui::kSlotFree].s, c.txt[ui::kSlotFree + 1].s,
               c.txt[ui::kSlotFree + 2].s, c.txt[ui::kSlotFree + 3].s, c.txt[ui::kSlotFree + 4].s, c.txt[ui::kSlotFree + 5].s,
               c.txt[ui::kSlotFree + 6].s, c.txt[ui::kSlotFree + 7].s);
      };
      drawn("A TGT ENGINE");
      CHECK(!strncmp(c.txt[ui::kSlotFree + 1].s, "B FR ", 5) && !strncmp(c.txt[ui::kSlotFree + 2].s, "C TO ", 5) &&
            !strcmp(c.txt[ui::kSlotFree + 3].s, "D ORD UP") && !strcmp(c.txt[ui::kSlotFree + 4].s, "E TRG ALL") &&
            !strcmp(c.txt[ui::kSlotFree + 5].s, "F EVRY 1") && !strcmp(c.txt[ui::kSlotFree + 6].s, "G OFS 0") &&
            !strncmp(c.txt[ui::kSlotFree + 7].s, "H SEED ", 7));
      CHECK(ui::kFillPage.key(0x41) && ui::fill_page_view().target == 1);   // VELOCITY: 8 letters, the TGT word dropped
      drawn("A VELOCITY");
      CHECK(ui::kFillPage.key(0x40) && ui::fill_page_view().target == 0);
    }
    // YES: the Rytm's pattern filled on BD's trigs, only its ENGINE lock slot added
    auto r = make_record();
    for (int s = 0; s < 16; s += 4) set_trig(r.data(), 0, s);
    for (int s = 0; s < 16; ++s) set_trig(r.data(), 1, s);
    writer_reset(r);
    cmd("fill=yes");
    CHECK(!strcmp(ui::fill_page_view().note, "FILLING.."));
    pump(400);
    std::string st = cmd("fill");
    v = ui::fill_page_view();
    CHECK(!strcmp(v.note, "FILLED 4: FN+NO UNDO") && v.undo);
    CHECK(diff_outside(r, F.rec, kSlotLo, kSlotHi) == 0 && lf::slots_used(F.rec.data()) == 1);
    lf::Target eng = {lf::kLock, 0, false};
    ft::Def d[ft::kMax];
    CHECK(ft::for_track(0, d, ft::kMax) == 2);
    for (int s = 0; s < 16; s += 4) CHECK(ft::index_of(d[0], lf::get(F.rec.data(), 0, eng, s)) == s / 4);   // UP: 0 1 2 3
    // the user edits track 2 meanwhile; FN+NO: BD's ENGINE back as it was, the edit stays
    put_slot(F.rec.data(), 10, 3, 2, 0x21);
    auto edited = r;
    put_slot(edited.data(), 10, 3, 2, 0x21);
    cmd("fill=undo");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "UNDONE") && !ui::fill_page_view().undo && F.rec == edited);
    CHECK(F.wrote.size() == 2 && F.wrote[1] == std::make_pair(0x54, 0) && F.stored == F.rec);   // the undo: 0x54 too
    {   // the header's count says what it counts: lock tracks (the 72 lock slots) in use after the fill (the owner took
        // "SLOTS 2/72" for a count of locks); within the header's right part (one page: no dots; 5 px a character)
      auto ans = answer_of(F.rec, 0x54);
      for (uint8_t b : ans) rytm_dump::byte(b);
      static ui::Canvas c;
      ui::kFillPage.draw(c, 0, 1, now_ms());
      lf::Target g0 = {lf::kLock, 0, false};
      char want_r[16];
      snprintf(want_r, sizeof want_r, "LOCK TRK %d/72", lf::slots_after(F.rec.data(), 0, g0));
      const char* hr = c.txt[ui::kSlotRight].s;
      CHECK(!strcmp(hr, want_r) && 2 + (int)strlen(c.txt[ui::kSlotTitle].s) * 5 + 2 < ui::kW - 3 - (int)strlen("LOCK TRK 72/72") * 5);
    }
    cmd("fill=undo");
    CHECK(!strcmp(ui::fill_page_view().note, "NOTHING TO UNDO"));
    // FN+YES clears; an undo expires with a pattern change
    cmd("fill=yes");
    pump(400);
    cmd("fill=clear");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "CLEARED 4: FN+NO UNDO") && F.rec == edited);
    sw(9);
    cmd("fill=undo");
    CHECK(!strcmp(ui::fill_page_view().note, "UNDO EXPIRED (PATTERN)"));
    // the keys: RIGHT to VELOCITY, YES fills with 60..127 at random; FUNC + NO undoes
    CHECK(ui::kFillPage.key(0x41) && ui::fill_page_view().target == 1);
    v = ui::fill_page_view();
    CHECK(v.from == 59 && v.to == 126 && v.order == lf::kRnd);
    auto before = F.rec;
    CHECK(ui::kFillPage.key(0x44));
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "FILLED 4: FN+NO UNDO"));
    int lo = 4 + lf::kVelAt;
    CHECK(diff_outside(before, F.rec, lo, lo + 64) == 0);
    for (int s = 0; s < 16; ++s) {
      uint8_t x = trk(F.rec.data(), 0)[lf::kVelAt + s];
      CHECK(s % 4 ? x == 0xFF : x >= 60 && x <= 127);
    }
    CHECK(ui::kFillPage.key(0x80) && ui::kFillPage.key(0x45));
    ui::kFillPage.key_up(0x80);
    pump(400);
    CHECK(F.rec == before);
    // playing: STOP TO FILL, nothing asked of the Rytm; fill=playing:on allows it
    rytm_dump::set_playing(true);
    int req = F.requests;
    cmd("fill=yes");
    CHECK(!strcmp(ui::fill_page_view().note, "STOP TO FILL") && F.requests == req);
    cmd("fill=playing:on");
    cmd("fill=yes");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "FILLED 4: FN+NO UNDO"));
    cmd("fill=playing:off");
    rytm_dump::set_playing(false);
    // no trigs on the track: NO TRIGS TO FILL, nothing written
    cmd("fill=open:4");
    int w = F.writes;
    cmd("fill=yes");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "NO TRIGS TO FILL"));
    CHECK(F.writes == w);
    // UP held + RIGHT: the page closes
    cmd("fill=open:0");
    CHECK(ui::kFillPage.key(0x42) && ui::kFillPage.key(0x41) && !ui::fill_page_view().on);
    ui::kFillPage.key_up(0x42);
    // L3: ... through the Rytm's own key messages (31: UP down, RIGHT down, both up) with the page shown (opened as
    // the Rytm opens it: 49 1 1 16, over the Sidecar's page open: 30)
    send(msg(0x30, {0, 1, 0}));
    send(msg(0x49, {1, ui::kPageFill}));
    CHECK(ui::fill_page_view().on && ui::remote_open() && ui::remote_page_title() && !strcmp(ui::remote_page_title(), "FILL"));
    send(msg(0x31, {0, 0x42, 1}));
    send(msg(0x31, {0, 0x41, 1}));
    send(msg(0x31, {0, 0x41, 0}));
    send(msg(0x31, {0, 0x42, 0}));
    CHECK(!ui::fill_page_view().on);
    // fix/fill-persist: no 2D 02 seen yet: the page says why nothing is written
    writer_reset(r, -1);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "PATTERN UNKNOWN") && F.requests == 0 && F.writes == 0);
    // ---- fix round M2: the undo's validity
    // YES, then NO at once (the page closes while the job runs); the job ends unseen; a pattern change; FILL again:
    // the undo belongs to the pattern the job read - expired
    writer_reset(r);
    sw(20);
    cmd("fill=open:0");
    CHECK(ui::kFillPage.key(0x44));
    CHECK(!ui::kFillPage.key(0x45) && !ui::fill_page_view().on);
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone);
    sw(21);
    cmd("fill=open:0");
    cmd("fill=undo");
    CHECK(!strcmp(ui::fill_page_view().note, "UNDO EXPIRED (PATTERN)"));
    // owner 2026-11-13 (hardware: UNDO EXPIRED right after a fill): a Rytm hello and its resync to the SAME pattern
    // (forget_pattern, then 2D 02) is no pattern change: the undo works
    writer_reset(r);
    sw(20);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(400);
    rytm_dump::forget_pattern();
    sw(20);
    cmd("fill=undo");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "UNDONE"));
    // away and back to the same pattern: the undo still works (its content check guards a changed track)
    cmd("fill=yes");
    pump(400);
    sw(21);
    sw(20);
    cmd("fill=undo");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "UNDONE"));
    {   // owner 2026-11-13: the Rytm's "Received PATTERN" popup covers the note while it would time out: a job's result
        // note stays on the footer until the next key
      static ui::Canvas c;
      uint32_t t = now_ms();
      ui::kFillPage.draw(c, 0, 1, t);
      CHECK(!strcmp(c.txt[ui::kSlotFoot].s, "UNDONE"));
      ui::kFillPage.draw(c, 0, 1, t + 20000);
      CHECK(!strcmp(c.txt[ui::kSlotFoot].s, "UNDONE"));
      CHECK(ui::kFillPage.key(0x43));   // DOWN: any key
      ui::kFillPage.draw(c, 0, 1, t + 40000);
      CHECK(strcmp(c.txt[ui::kSlotFoot].s, "UNDONE") != 0);
    }
    // a project load (the Rytm's 10, the same pattern number): expired
    writer_reset(r);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(400);
    CHECK(ui::fill_page_view().undo);
    send({0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 'E', 'R', 0xF7});
    cmd("fill=undo");
    CHECK(!strcmp(ui::fill_page_view().note, "UNDO EXPIRED (PROJECT)"));
    // the filled target edited since (a step's value changed on the Rytm): CHANGED SINCE, nothing written
    writer_reset(r);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(400);
    CHECK(ui::fill_page_view().undo);
    {
      int i = lf::find_slot(F.rec.data(), 0, 0);
      CHECK(i >= 0);
      slot(F.rec.data(), i)[2 + 4] = 99;
    }
    auto edited2 = F.rec;
    int w2 = F.writes;
    cmd("fill=undo");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "CHANGED SINCE: NO UNDO") && F.rec == edited2 && F.writes == w2);
    // ... edited elsewhere only (another track): the undo still runs
    writer_reset(r);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(400);
    put_slot(F.rec.data(), 30, 2, 5, 0x22);
    cmd("fill=undo");
    pump(400);
    CHECK(!strcmp(ui::fill_page_view().note, "UNDONE") && lf::find_slot(F.rec.data(), 0, 0) < 0 && lf::find_slot(F.rec.data(), 2, 5) == 30);
    // N1 (re-review): the undo's read restarted on another pattern (a switch before the answer) with the same content
    // there (a copy): CHANGED SINCE, nothing written (the content check alone would put B's target back)
    writer_reset(r);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(400);
    CHECK(ui::fill_page_view().undo);
    {
      auto filled = F.rec;
      int wn1 = F.writes;
      F.answer = false;
      cmd("fill=undo");
      CHECK(patwrite::busy());
      sw(40);   // B, holding the same filled locks
      F.answer = true;
      pump(3000);
      CHECK(!patwrite::busy() && F.rec == filled && F.writes == wn1);
      CHECK(!strcmp(ui::fill_page_view().note, "CHANGED SINCE: NO UNDO") && !ui::fill_page_view().undo);
    }
    // ... and a project load during the undo's read
    writer_reset(r);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(400);
    CHECK(ui::fill_page_view().undo);
    {
      auto filled = F.rec;
      int wn1 = F.writes;
      F.answer = false;
      cmd("fill=undo");
      CHECK(patwrite::busy());
      send({0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'R', 'O', 'J', '2', 0xF7});
      F.answer = true;
      pump(3000);
      CHECK(!patwrite::busy() && F.rec == filled && F.writes == wn1);
      CHECK(!strcmp(ui::fill_page_view().note, "CHANGED SINCE: NO UNDO"));
    }
    // fix round 1 (review M2): a project load between the read and the write: the pattern read may be the old
    // project's, the write would land in the new one's stored pattern: nothing written (PROJECT LOADED: STOPPED)
    writer_reset(r);
    CHECK(patwrite::start(patch_set, nullptr));
    send({0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'R', 'O', 'J', '3', 0xF7});   // (before the answer is taken)
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyProject && F.writes == 0 && F.rec == r && F.stored == r);
    CHECK(!strcmp(patwrite::why_text(patwrite::kWhyProject), "PROJECT LOADED: STOPPED"));
    // ... after the write, a verify mismatch: no write-back into the new project's pattern
    writer_reset(r);
    F.corrupt = true;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(30);
    CHECK(patwrite::state() == patwrite::kSettle && F.writes == 1);
    send({0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'R', 'O', 'J', '4', 0xF7});
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    // fix round 2 (re-review MED): a project load after the write (settle), the verify then good: not DONE / FILLED -
    // CHANGED MEANWHILE: CHECK, nothing written back
    writer_reset(r);
    cmd("fill=open:0");
    cmd("fill=yes");
    pump(30);
    CHECK(patwrite::state() == patwrite::kSettle && F.writes == 1);
    send({0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'R', 'O', 'J', '5', 0xF7});
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    CHECK(!strcmp(ui::fill_page_view().note, "CHANGED MEANWHILE: CHECK"));
    // ... in the verify (its request out, the answer not yet in): the same
    writer_reset(r);
    F.answer_after_write = false;
    CHECK(patwrite::start(patch_set, nullptr));
    pump(200);
    CHECK(patwrite::state() == patwrite::kVerifying);
    send({0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'R', 'O', 'J', '6', 0xF7});
    {
      auto a = answer_of(F.rec, 0x54);   // a good verify answer arriving before a tick
      for (uint8_t b : a) patwrite::byte(b);
    }
    CHECK(patwrite::why() == patwrite::kWhyChanged && F.writes == 1 && patwrite::stats().restores == 0);
    // fix round 2 (re-review LOW): a hello during a read, then a restart (a DIRTY): PATTERN UNKNOWN, never a request
    // for byte 9 = 0xFF
    writer_reset(r);
    F.dirty_on_request = 1;
    CHECK(patwrite::start(patch_set, nullptr));
    send(msg(0x35, {0, 6, 127, ui::kCaps2Arrows}));
    pump(400);
    CHECK(patwrite::why() == patwrite::kWhyNoPattern && F.writes == 0);
    for (auto& o : g_out)
      if (o.size() == 15 && o[6] == 0x64) CHECK(o[9] < 0x80);
    // fix round 1 (review L): a Rytm hello (it rebooted) forgets the active pattern until the resync's 2D 02
    writer_reset(r);
    send(msg(0x35, {0, 6, 127, ui::kCaps2Arrows}));
    CHECK(rytm_dump::active_pattern() == -1);
    CHECK(!patwrite::start(patch_set, nullptr) && patwrite::why() == patwrite::kWhyNoPattern && F.requests == 0);
    sw(0);
    CHECK(patwrite::start(patch_set, nullptr));
    pump(400);
    CHECK(patwrite::state() == patwrite::kDone && F.writes == 1);
    // N3 (re-review): the song's arm waiting on its take's name (the card) does not start its dumps once a FILL runs:
    // the FILL started at each step of the arm's walk (the folder's listing, the name's checks)
    cardq::set_worker(true);   // (the card on its own core, as on the Pi: a job asked now is answered a pass later)
    for (int k = 0; k < 30; ++k) {
      writer_reset(r);
      char so[512] = "";
      song::command("song=arm", so, sizeof so);
      int i = 0;
      for (; i < k && song::status().state == song::kOff; ++i) {   // (k single operations: tick, idle, a card job)
        if (i % 3 == 0) song::tick((uint32_t)(g_us / 1000));
        else if (i % 3 == 1) song::idle();
        else host_card_core(0), cardq::service(), host_card_core(3);
      }
      if (song::status().state != song::kOff) {   // (armed before the FILL: the walk is over)
        song::command("song=disarm", so, sizeof so);
        continue;
      }
      F.answer = false;
      cmd("fill=yes");
      for (int n = 0; n < 40; ++n) {
        song::tick((uint32_t)(g_us / 1000));
        for (int m = 0; m < 8; ++m) song::idle();
        host_card_core(0);
        while (cardq::service()) {}
        host_card_core(3);
      }
      CHECK(song::status().state == song::kOff);
      patwrite::reset();
    }
    cardq::set_worker(false);
    // M1: the song refuses to arm while a fill writes (and the fill refuses while the song's dumps run: SONG BUSY)
    writer_reset(r);
    F.answer = false;
    cmd("fill=yes");
    CHECK(patwrite::busy());
    {
      char so[512] = "";
      song::command("song=arm", so, sizeof so);
      CHECK(strstr(so, "a FILL is writing the pattern"));
    }
    writer_reset(r);
    CHECK(st.find("fill: track 0 target ENGINE") == 0);
    CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  }
  printf("lockfill: ALL PASS\n");
  return 0;
}
