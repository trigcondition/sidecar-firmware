// core/bundle/projects.cpp - see projects.h.
#include "projects.h"

#if CORE_MSAMP
#include <string.h>

#include "../msamp/cardq.h"
#include "../platform.h"

namespace core {
namespace projects {

namespace {

constexpr const char* kFile = "/.rytm/projects.txt";
constexpr int kLine = kKeyMax + kRytm + 12 + kDate + 4;

Info* g_ix = nullptr;   // kMax entries (the heap, once)
int g_n = 0;
volatile uint32_t g_gen = 0;   // seqlock: odd while the control core changes g_ix
bool g_dirty = false;

// the writer: one kWriteReq at a time, the text formatted when it is posted
struct Writer {
  cardq::Req rq;
  File f;
  char* text = nullptr;
  bool busy = false;
} g_w;

// core 0's requests (mark_imported / forget): a single-producer / single-consumer ring the control core drains
struct Req {
  char op;   // 'i' imported, 'f' forget
  char key[kKeyMax];
  char rytm[kRytm];
};
constexpr uint32_t kReqs = 16;
Req g_req[kReqs];
volatile uint32_t g_req_wr = 0, g_req_rd = 0;

volatile uint32_t g_epoch = 0, g_epoch_ms = 0;
volatile int g_clock = kClockNone;
volatile int g_zone = 0;   // minutes east of UTC (set_clock_zone)

inline uint32_t ld(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void sto(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void ensure() {
  if (g_ix) return;
  g_ix = new Info[kMax];
  g_w.text = new char[kMax * kLine + 1];
  memset(g_ix, 0, sizeof(Info) * kMax);
}
void begin_change() {
  sto(&g_gen, g_gen + 1);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);   // the odd generation before any change of g_ix
}
void end_change() {
  __atomic_thread_fence(__ATOMIC_RELEASE);   // every change of g_ix before the even generation
  sto(&g_gen, g_gen + 1);
  g_dirty = true;
}
void clean(const char* in, char* out, int cap) {   // printable, no tab
  int k = 0;
  for (const char* c = in ? in : ""; *c && k < cap - 1; ++c) out[k++] = (*c < 0x20 || *c > 0x7e) ? '_' : *c;
  out[k] = 0;
}
int index_of(const char* key) {
  for (int i = 0; i < g_n; ++i)
    if (!strcmp(g_ix[i].key, key)) return i;
  return -1;
}
uint32_t top() {
  uint32_t m = 0;
  for (int i = 0; i < g_n; ++i)
    if (g_ix[i].used > m) m = g_ix[i].used;
  return m;
}
// a new entry's room: the end, or the least recently used one when full
int slot_for(const char* key) {
  int i = index_of(key);
  if (i >= 0) return i;
  if (g_n < kMax) {
    memset(&g_ix[g_n], 0, sizeof(Info));
    return g_n++;
  }
  int lo = 0;
  for (int j = 1; j < g_n; ++j)
    if (g_ix[j].used < g_ix[lo].used) lo = j;
  memset(&g_ix[lo], 0, sizeof(Info));
  return lo;
}
bool field(const char*& p, char* out, int cap) {   // up to the next tab / the end
  int k = 0;
  while (*p && *p != '\t') {
    if (k < cap - 1) out[k++] = *p;
    ++p;
  }
  out[k] = 0;
  if (*p == '\t') {
    ++p;
    return true;
  }
  return false;
}
bool parse(const char* line, Info* e) {
  const char* p = line;
  char used[12], flags[8];
  memset(e, 0, sizeof *e);
  if (!field(p, e->key, sizeof e->key) || !e->key[0] || !field(p, e->rytm, sizeof e->rytm) ||
      !field(p, used, sizeof used) || !field(p, e->date, sizeof e->date))
    return false;
  field(p, flags, sizeof flags);
  char k[kKeyMax];
  project_key(e->key, k, sizeof k);
  if (strcmp(k, e->key)) return false;   // not a key the rule makes
  uint32_t u = 0;
  for (const char* c = used; *c; ++c) {
    if (*c < '0' || *c > '9' || u > 100000000u) return false;
    u = u * 10 + (uint32_t)(*c - '0');
  }
  if (!used[0]) return false;
  e->used = u;
  if (!strcmp(e->date, "-")) e->date[0] = 0;
  e->imported = strchr(flags, 'i') != nullptr;
  return true;
}
uint32_t format() {
  uint32_t n = 0;
  for (int i = 0; i < g_n; ++i) {
    const Info& e = g_ix[i];
    n += (uint32_t)sniprintf(g_w.text + n, kLine + 1, "%s\t%s\t%lu\t%s\t%s\n", e.key, e.rytm, (unsigned long)e.used,
                             e.date[0] ? e.date : "-", e.imported ? "i" : "-");
  }
  return n;
}
void date_now(char* out) {
  uint32_t ep;
  int src;
  if (clock_now(&ep, &src)) iso_date(ep, out, kDate);
  else out[0] = 0;
}
void apply_requests() {
  while (ld(&g_req_rd) != ld(&g_req_wr)) {
    const Req& r = g_req[g_req_rd % kReqs];
    int i = index_of(r.key);
    if (r.op == 'i') {
      begin_change();
      if (i < 0) {
        i = slot_for(r.key);
        sniprintf(g_ix[i].key, sizeof g_ix[i].key, "%s", r.key);
        g_ix[i].used = 0;
      }
      sniprintf(g_ix[i].rytm, sizeof g_ix[i].rytm, "%s", r.rytm);
      g_ix[i].imported = true;
      end_change();
    } else if (i >= 0) {
      begin_change();
      g_ix[i] = g_ix[--g_n];
      end_change();
    }
    sto(&g_req_rd, g_req_rd + 1);
  }
}
bool request(char op, const char* key, const char* rytm) {
  uint32_t w = ld(&g_req_wr);
  if (w - ld(&g_req_rd) >= kReqs) return false;
  Req& r = g_req[w % kReqs];
  r.op = op;
  sniprintf(r.key, sizeof r.key, "%s", key);
  clean(rytm, r.rytm, sizeof r.rytm);
  sto(&g_req_wr, w + 1);
  return true;
}

}  // namespace

CORE_COLD void load() {
  ensure();
  begin_change();
  g_n = 0;
  File f;
  if (f.open(kFile, kRead)) {
    char line[kLine + 64];
    int k = 0;
    for (;;) {
      int c = f.read();
      if (c < 0 || c == '\n') {
        while (k && line[k - 1] == '\r') --k;
        line[k] = 0;
        Info e;
        if (k && g_n < kMax && parse(line, &e) && index_of(e.key) < 0) g_ix[g_n++] = e;
        k = 0;
        if (c < 0) break;
      } else if (k < (int)sizeof line - 1) {
        line[k++] = (char)c;
      }
    }
    f.close();
  }
  sto(&g_gen, g_gen + 1);   // (read: nothing to write)
}

CORE_COLD void note(const char* rytm_name, const char* rytm) {
  ensure();
  char key[kKeyMax];
  project_key(rytm_name, key, sizeof key);
  uint32_t next = top() + 1;
  begin_change();
  int i = slot_for(key);
  sniprintf(g_ix[i].key, sizeof g_ix[i].key, "%s", key);
  clean(rytm ? rytm : rytm_name, g_ix[i].rytm, sizeof g_ix[i].rytm);
  g_ix[i].used = next;
  date_now(g_ix[i].date);
  g_ix[i].imported = false;
  end_change();
}

CORE_COLD bool save() {
  if (!g_ix) return false;
  apply_requests();
  if (g_w.busy) {
    if (!cardq::async(&g_w.rq, cardq::kWriteReq, &g_w.f, kFile, 0, (uint8_t*)g_w.text, g_w.rq.len)) return true;
    if (g_w.rq.got < 0) log(CORE_STR("projects: cannot write %s"), kFile);
    g_w.busy = false;
  }
  if (!g_dirty) return false;
  g_dirty = false;
  uint32_t n = format();
  g_w.busy = true;
  g_w.rq.state = cardq::kFree;
  if (!cardq::async(&g_w.rq, cardq::kWriteReq, &g_w.f, kFile, 0, (uint8_t*)g_w.text, n)) return true;
  if (g_w.rq.got < 0) log(CORE_STR("projects: cannot write %s"), kFile);
  g_w.busy = false;
  return true;
}

CORE_COLD bool mark_imported(const char* key, const char* rytm_name) {
  ensure();
  return request('i', key, rytm_name);
}
CORE_COLD bool forget(const char* key) {
  ensure();
  return request('f', key, "");
}
CORE_COLD bool requests_idle() { return ld(&g_req_rd) == ld(&g_req_wr); }

CORE_COLD bool rekey(const char* from, const char* to) {
  ensure();
  int i = index_of(from);
  if (i < 0) return false;
  begin_change();
  int j = index_of(to);
  if (j >= 0 && j != i) g_ix[j] = g_ix[--g_n], i = index_of(from);   // (the target's own entry goes: this one is it now)
  sniprintf(g_ix[i].key, sizeof g_ix[i].key, "%s", to);
  end_change();
  return true;
}
CORE_COLD void drop(const char* key) {
  if (!g_ix) return;
  int i = index_of(key);
  if (i < 0) return;
  begin_change();
  g_ix[i] = g_ix[--g_n];
  end_change();
}
CORE_COLD const Info* at(int i) { return g_ix && i >= 0 && i < g_n ? &g_ix[i] : nullptr; }

CORE_COLD int count() { return g_n; }
CORE_COLD const Info* find(const char* key) {
  if (!g_ix) return nullptr;
  int i = index_of(key);
  return i >= 0 ? &g_ix[i] : nullptr;
}
CORE_COLD int copy(Info* out, int max) {
  if (!g_ix) return 0;
  for (int tries = 0; tries < 1000; ++tries) {
    uint32_t g0 = ld(&g_gen);
    if (g0 & 1) continue;
    int n = g_n < max ? g_n : max;
    memcpy(out, g_ix, sizeof(Info) * (n > 0 ? n : 0));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);   // the copy before the generation is read again
    if (ld(&g_gen) == g0) return n;
  }
  return -1;   // (never settled: the caller treats it as unknown)
}

CORE_COLD void set_clock(uint32_t epoch, int source) {
  g_epoch = epoch;
  g_epoch_ms = now_ms();
  g_clock = source;
}
CORE_COLD bool clock_now(uint32_t* epoch, int* source) {
  *source = g_clock;
  if (g_clock == kClockNone) return false;
  *epoch = g_epoch + (now_ms() - g_epoch_ms) / 1000;
  return true;
}
CORE_COLD void set_clock_zone(int minutes) {
  if (minutes >= -840 && minutes <= 840) g_zone = minutes;
}
CORE_COLD int clock_zone() { return g_zone; }
CORE_COLD bool clock_local(uint32_t* local, int* source) {
  uint32_t ep;
  if (!clock_now(&ep, source)) return false;
  *local = (uint32_t)((int64_t)ep + (int64_t)g_zone * 60);
  return true;
}
CORE_COLD const char* clock_name(int s) {
  return s == kClockBrowser ? "browser" : s == kClockNtp ? "ntp" : s == kClockRtc ? "rtc" : "none";
}

static void civil(uint32_t epoch, int* y, int* mo, int* d, int* h, int* mi, int* s) {
  uint32_t days = epoch / 86400, rem = epoch % 86400;
  *h = (int)(rem / 3600), *mi = (int)(rem / 60 % 60), *s = (int)(rem % 60);
  // Howard Hinnant's civil_from_days
  int64_t z = (int64_t)days + 719468, era = z / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = (int64_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yy + (*mo <= 2));
}
CORE_COLD void iso_date(uint32_t epoch, char* out, int cap) {
  int y, mo, d, h, mi, s;
  civil(epoch, &y, &mo, &d, &h, &mi, &s);
  sniprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, s);
}
CORE_COLD void stamp(uint32_t epoch, char* out, int cap) {
  int y, mo, d, h, mi, s;
  civil(epoch, &y, &mo, &d, &h, &mi, &s);
  sniprintf(out, cap, "%04d%02d%02d-%02d%02d", y, mo, d, h, mi);
}

CORE_COLD void reset_for_tests() {
  ensure();
  g_n = 0;
  g_dirty = false;
  g_w.busy = false;
  g_w.rq.state = cardq::kFree;
  g_req_wr = g_req_rd = 0;
  g_clock = kClockNone;
  g_zone = 0;
}

}  // namespace projects
}  // namespace core
#endif
