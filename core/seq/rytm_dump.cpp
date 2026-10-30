// core/seq/rytm_dump.cpp - see rytm_dump.h: the stock pattern dump of the Rytm's working buffer. Control side; cold.
#include "seq/rytm_dump.h"

#include <string.h>

#include "hooks.h"
#include "platform.h"

namespace core {
namespace rytm_dump {

static const uint8_t kReq[15] = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x64, 0x01, 0x01, 0x80, 0x00, 0x00, 0x00, 0x05, 0xF7};
constexpr int kReqNr = 9;
static const uint8_t kHead[9] = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x54, 0x01, 0x01};

CORE_COLD int request(uint8_t out[15], int pattern) {
  memcpy(out, kReq, sizeof kReq);
  if (pattern >= 0 && pattern < 128) out[kReqNr] = (uint8_t)pattern;
  return (int)sizeof kReq;
}

CORE_COLD bool parse(const uint8_t* m, int n, uint8_t* record, int* pattern) {
  if (n != kAnswer || memcmp(m, kHead, sizeof kHead) || m[n - 1] != 0xF7) return false;
  int pat = m[9];   // (record may be m itself: everything read before it is written)
  uint32_t sum = 0;
  for (int i = 10; i < n - 5; ++i) sum += m[i];   // the pattern number not in it [measured, a real Rytm MKII]
  if ((int)((m[n - 5] << 7) | m[n - 4]) != (int)(sum & 0x3FFF)) return false;
  if ((m[n - 3] << 7 | m[n - 2]) != n - 10) return false;
  uint8_t head[4];
  memcpy(head, m + 11, 4);     // the record's header travels in the message's header
  int k = 4;
  for (int i = 15; i < n - 5; i += 8) {   // 7-bit groups: a byte of top bits (the first's in bit 6), then up to 7
    uint8_t hi = m[i];
    for (int j = 1; j < 8 && i + j < n - 5; ++j) {
      if (k >= kRecord) return false;
      record[k++] = (uint8_t)(m[i + j] | ((hi << j) & 0x80));
    }
  }
  if (k != kRecord) return false;
  memcpy(record, head, 4);
  if (pattern) *pattern = pat;
  return true;
}

#if CORE_POLY
namespace {
int pg = kOff;               // who wants it (kOff: nobody)
uint8_t* rec = nullptr;      // the last good record
uint8_t* buf = nullptr;      // the answer being read
int rec_pattern = -1;
uint32_t gen = 0;
int nbuf = -1;               // bytes of the answer so far, -1 not inside one
bool due = false, waiting = false, retried = false;
uint32_t due_ms = 0, sent_ms = 0;
bool pat_due = false, dirty_due = false;   // a pattern change / a DIRTY not fetched yet
uint32_t pat_ms = 0, dirty_first = 0, dirty_last = 0;
bool playing = false, sent_any = false;
int hold = 0;
int last_pattern = -1;
void (*consumer)(const uint8_t*, int, int) = nullptr;
Stats st;

CORE_COLD void alloc() {
  if (!rec) rec = new uint8_t[kRecord], buf = new uint8_t[kAnswer];
}
bool after(uint32_t ms, uint32_t t) { return (int32_t)(ms - t) >= 0; }
// a DIRTY's fetch: not now (held), or the time it is due
bool dirty_held() { return hold || (playing && pg == kPageVoice); }
uint32_t dirty_at() {
  if (playing) {   // at most one every kPlayGapMs, kDirtyMs after the first edit at the soonest
    uint32_t a = dirty_first + kDirtyMs, g = sent_ms + kPlayGapMs;
    return !sent_any || (int32_t)(a - g) > 0 ? a : g;
  }
  uint32_t q = dirty_last + kQuietMs, c = dirty_first + kStaleMs;   // edits settled, or too long since the first
  return (int32_t)(q - c) < 0 ? q : c;
}
bool pat_held() { return hold >= 2 || (hold == 1 && pg != kPagePattern); }
}  // namespace

CORE_COLD void reset() {
  pg = kOff, rec_pattern = -1, gen = 0, nbuf = -1, due = waiting = retried = false, last_pattern = -1;
  pat_due = dirty_due = playing = sent_any = false, hold = 0;
  consumer = nullptr;
  memset(&st, 0, sizeof st);
}

CORE_COLD void set_page(int p) {
  if (p && !pg) want(0);
  pg = p;
}
CORE_COLD void set_active(bool a) { set_page(a ? kPagePattern : kOff); }
CORE_COLD bool active() { return pg != kOff; }
CORE_COLD int page() { return pg; }
CORE_COLD void set_playing(bool on) { playing = on; }
bool is_playing() { return playing; }
CORE_COLD void set_hold(int level) { hold = level; }
CORE_COLD bool pending() { return pg && (due || waiting); }
CORE_COLD int stale() {
  if (!pg || !(dirty_due || pat_due)) return 0;
  return (dirty_due && dirty_held()) || (pat_due && pat_held()) ? 2 : 1;
}

CORE_COLD void want(uint32_t delay_ms) {
  uint32_t t = now_ms() + delay_ms;
  if (!due || (int32_t)(t - due_ms) > 0) due_ms = t;   // debounced: the latest wish's time
  due = true;
}

CORE_COLD void dirty() {
  uint32_t t = now_ms();
  ++st.dirties;
  if (!dirty_due) dirty_due = true, dirty_first = t;
  dirty_last = t;
}

CORE_COLD void pattern_changed(int p) {
  if (p == last_pattern) return;
  last_pattern = p;
  if (pg) pat_due = true, pat_ms = now_ms();   // trailing: the last switch of a quick run
}

CORE_COLD void byte(uint8_t b) {
  if (b >= 0xF8) return;   // realtime, anywhere
  if (b == 0xF0) {
    nbuf = 0;
  } else if (nbuf < 0) {
    return;
  }
  if (nbuf >= 0 && nbuf < (int)sizeof kHead && b != kHead[nbuf]) {   // another SysEx: not ours
    nbuf = -1;
    return;
  }
  alloc();
  if (nbuf >= kAnswer) {   // too long: not a pattern dump
    nbuf = -1;
    ++st.bad;
    return;
  }
  buf[nbuf++] = b;
  ++st.bytes;
  if (b != 0xF7) return;
  int n = nbuf, p = -1;
  nbuf = -1;
  if (!parse(buf, n, buf, &p)) {   // (the record is written over the answer's start: parse reads ahead of its writes)
    ++st.bad;
    return;
  }
  memcpy(rec, buf, kRecord);
  rec_pattern = p, ++gen, ++st.answers;
  waiting = false, retried = false;
  if (consumer) consumer(rec, kRecord, p);
}

CORE_COLD void tick(uint32_t ms) {
  if (waiting && ms - sent_ms > kTimeoutMs) {   // lost: once more, then wait for the next trigger
    waiting = false;
    ++st.timeouts;
    if (!retried) retried = true, due = true, due_ms = ms;
  }
  if (!pg || waiting) return;
  bool go = due && hold < 2 && after(ms, due_ms);
  go = go || (pat_due && !pat_held() && after(ms, pat_ms + kPatternMs));
  go = go || (dirty_due && !dirty_held() && after(ms, dirty_at()));
  if (!go) return;
  due = pat_due = dirty_due = false;   // the answer has every edit up to now
  waiting = true, sent_any = true, sent_ms = ms;
  ++st.requests;
  uint8_t m[15];
  request(m, last_pattern);
  to_rytm(m, (int)sizeof m);
}

CORE_COLD const uint8_t* record(int* p, uint32_t* g) {
  if (p) *p = rec_pattern;
  if (g) *g = gen;
  return gen ? rec : nullptr;
}
CORE_COLD void on_record(void (*fn)(const uint8_t*, int, int)) { consumer = fn; }
const Stats& stats() { return st; }

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "rytmpat", 7) || (p[7] && p[7] != '=')) return false;
  if (!strcmp(p, "rytmpat=on")) set_active(true);
  else if (!strcmp(p, "rytmpat=voice")) set_page(kPageVoice);
  else if (!strcmp(p, "rytmpat=off")) set_active(false);
  else if (!strcmp(p, "rytmpat=fetch")) pg = pg ? pg : kPagePattern, want(0);
  else if (p[7]) return sniprintf(out, cap, CORE_STR("rytmpat | rytmpat=on|voice|off|fetch")), true;
  sniprintf(out, cap, CORE_STR("rytmpat: %s%s%s, pattern %d (gen %lu), requests %lu answers %lu bad %lu timeouts %lu"
                                " dirties %lu"),
            pg == kPageVoice ? "voice" : pg ? "on" : "off", hold ? ", held (song)" : "", stale() ? ", stale" : "",
            rec_pattern, (unsigned long)gen, (unsigned long)st.requests, (unsigned long)st.answers,
            (unsigned long)st.bad, (unsigned long)st.timeouts, (unsigned long)st.dirties);
  return true;
}
#endif

}  // namespace rytm_dump
}  // namespace core
