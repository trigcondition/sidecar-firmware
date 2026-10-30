// core/bundle/feed.cpp - see feed.h.
#include "feed.h"

#if CORE_MSAMP
#include <stdlib.h>
#include <string.h>

#include "../platform.h"

namespace core {
namespace bundle {
namespace feed {

namespace {

Feed* g_work = nullptr;          // core 0's (built here)
Feed* g_pub = nullptr;           // the published copy (the seqlock's)
volatile uint32_t g_seq = 0;     // odd while g_pub is written
volatile uint32_t g_count = 0;   // publishes
bool g_avail = false;
bool g_want = true;              // the lists owed
char g_want_src[kDriveMax] = "";
uint32_t g_st_seq = ~0u;
bool g_force = false;            // publish at the next tick (an eject's answer)
uint8_t g_last_phase = kIdle;
uint32_t g_jobs_seen = ~0u;      // prjslot's jobs ended (the trash read again)
char g_vols_seen[64] = "?";

// the Rytm page's commands: one producer (the control core), one consumer (core 0)
constexpr int kRing = 8, kLine = 200;
char g_ring[kRing][kLine];
volatile uint32_t g_rd = 0, g_wr = 0;

void ensure() {
  if (g_work) return;
  g_work = new Feed;
  memset(g_work, 0, sizeof *g_work);
  Feed* p = new Feed;
  memset(p, 0, sizeof *p);
  __atomic_store_n(&g_pub, p, __ATOMIC_RELEASE);   // (other cores see it only cleared)
}

void publish() {
  __atomic_store_n(&g_seq, g_seq + 1, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  memcpy(g_pub, g_work, sizeof *g_pub);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  __atomic_store_n(&g_seq, g_seq + 1, __ATOMIC_RELEASE);
  __atomic_store_n(&g_count, g_count + 1, __ATOMIC_RELEASE);
}

// "USB" -> "USB:" (a stick's drive), "card" / "" -> ""
void drive_from(const char* t, char* d) {
  d[0] = 0;
  if (!t || !t[0] || !strcmp(t, "card") || !strcmp(t, "CARD")) return;
  int n = 0;
  while (t[n] && t[n] != ':' && n < kDriveMax - 2) d[n] = t[n], ++n;
  d[n] = ':', d[n + 1] = 0;
}

void read_vols() {
  Feed& f = *g_work;
  f.nvol = env().volumes ? env().volumes(f.vol, kVols) : 0;
  if (f.nvol < 0) f.nvol = 0;
}
// the volumes' fingerprint: a stick in or out moves it
void vols_key(char* out, int cap) {
  Volume v[kVols];
  int n = env().volumes ? env().volumes(v, kVols) : 0;
  out[0] = 0;
  for (int i = 0; i < n; ++i) {
    size_t l = strlen(out);
    sniprintf(out + l, cap - (int)l, "%s,", v[i].drive);
  }
}

void read_lists() {
  Feed& f = *g_work;
  f.nproj = project_rows(f.proj, kProjects);
  read_vols();
  sniprintf(f.src, sizeof f.src, "%s", g_want_src);
  bool on = !f.src[0];
  for (int i = 0; i < f.nvol && !on; ++i) on = !strcmp(f.vol[i].drive, f.src);
  if (!on) f.src[0] = 0;   // that stick is gone: the card's
  if (!f.src[0]) {
    f.nbund = bundle_rows("/Bundles", f.bund, kBundles);
  } else {
    char a[24];
    sniprintf(a, sizeof a, "%s%s", f.src, kStickFolder);
    f.nbund = bundle_rows(a, f.bund, kBundles);
    sniprintf(a, sizeof a, "%s/", f.src);
    f.nbund += bundle_rows(a, f.bund + f.nbund, kBundles - f.nbund);
  }
  f.card_free = card_free_bytes();
  f.ntrash = prjslot::trash_rows(f.trash, kTrash);
  sniprintf(f.prj_last, sizeof f.prj_last, "%s", prjslot::last());
  f.reading = false;
  ++f.gen;
}

void run(const char* line);

}  // namespace

void set_available(bool on) {
  ensure();
  g_avail = on;
}

// (out may be torn when this returns false: a caller keeps its last good copy - copy into a scratch Feed first)
bool copy(Feed* out) {
  Feed* pub = __atomic_load_n(&g_pub, __ATOMIC_ACQUIRE);
  if (!pub) return false;
  for (int k = 0; k < 2000; ++k) {
    uint32_t a = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
    if (a & 1) {   // the writer is in its memcpy (a few microseconds)
#if defined(__aarch64__)
      __asm__ volatile("yield");
#endif
      continue;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(out, pub, sizeof *out);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_seq, __ATOMIC_ACQUIRE) == a) return a != 0;
  }
  return false;
}
bool available() {
  Feed* p = __atomic_load_n(&g_pub, __ATOMIC_ACQUIRE);
  return p && __atomic_load_n(&p->available, __ATOMIC_ACQUIRE);
}
uint32_t published() { return __atomic_load_n(&g_count, __ATOMIC_ACQUIRE); }

bool post(const char* line) {
  uint32_t w = __atomic_load_n(&g_wr, __ATOMIC_RELAXED), r = __atomic_load_n(&g_rd, __ATOMIC_ACQUIRE);
  if (w - r >= (uint32_t)kRing) return false;
  sniprintf(g_ring[w % kRing], kLine, "%s", line);
  __atomic_store_n(&g_wr, w + 1, __ATOMIC_RELEASE);
  return true;
}

void tick() {
  ensure();
  for (;;) {   // the ring
    uint32_t r = __atomic_load_n(&g_rd, __ATOMIC_RELAXED), w = __atomic_load_n(&g_wr, __ATOMIC_ACQUIRE);
    if (r == w) break;
    char line[kLine];
    memcpy(line, g_ring[r % kRing], kLine);
    line[kLine - 1] = 0;
    __atomic_store_n(&g_rd, r + 1, __ATOMIC_RELEASE);
    run(line);
  }
  Feed& f = *g_work;
  bool changed = f.available != g_avail;
  f.available = g_avail;
  // a job that ended: the lists again (a new bundle, a deleted project, an imported one)
  uint8_t ph = status().phase;
  if (ph != g_last_phase && (ph == kDone || ph == kFailed || ph == kCancelled)) g_want = true;
  g_last_phase = ph;
  uint32_t jd = prjslot::jobs_done();
  if (jd != g_jobs_seen) g_jobs_seen = jd, g_want = true;   // (a trash, a restore, a copy: the lists again)
  char vk[64];
  vols_key(vk, sizeof vk);
  if (strcmp(vk, g_vols_seen)) {   // a stick in or out
    sniprintf(g_vols_seen, sizeof g_vols_seen, "%s", vk);
    g_want = true;
  }
  if (g_want && !f.reading) f.reading = true, changed = true;
  bool stick_paused = false;   // a stick's list: only at full rate (as the job's stick I/O: not while the Rytm plays)
  if (g_want_src[0] && env().rules) {
    IoRule r = {kPiece, kPiece, 0, ""};
    env().rules(&r);
    stick_paused = !r.write_piece;
  }
  if (g_force) g_force = false, changed = true;
  if (g_want && g_avail && !stick_paused && !(env().card_busy && env().card_busy()) && !busy()) {   // (never while a job reads: its pieces first)
    g_want = false;
    read_lists();
    changed = true;
  } else if (g_want && busy()) {
    read_vols();   // the sticks at least (cheap)
  }
  if (status().seq != g_st_seq || changed) {
    g_st_seq = status().seq;
    f.st = status();
    f.have_plan = plan_info(&f.plan);
    changed = true;
  }
  if (changed) publish();
}

namespace {
void run(const char* line) {
  char out[160];
  command(line, out, sizeof out);
}
}  // namespace

bool command(const char* c, char* out, int cap) {
  ensure();
  if (strncmp(c, "bundle", 6) || (c[6] && c[6] != '=')) return false;
  out[0] = 0;
  if (!c[6]) {
    const Status& s = status();
    sniprintf(out, cap, "bundle: %s%s%s %s", phase_name(s.phase), s.waiting[0] ? " waiting " : "", s.waiting, s.msg);
    return true;
  }
  const char* a = c + 7;
  char d[kDriveMax];
  if (!strncmp(a, "list", 4)) {
    drive_from(a[4] == ':' ? a + 5 : "", d);
    sniprintf(g_want_src, sizeof g_want_src, "%s", d);
    g_want = true;
    sniprintf(out, cap, "bundle: the lists (%s) read next", d[0] ? d : "card");
  } else if (!strncmp(a, "export:", 7)) {
    const char* k = strchr(a + 7, ':');
    if (!k) return sniprintf(out, cap, "bundle=export:<card|USB>:<key>"), true;
    char t[8];
    sniprintf(t, sizeof t, "%.*s", (int)(k - (a + 7)), a + 7);
    drive_from(t, d);
    ExportOpts o;
    if (d[0]) sniprintf(o.vol, sizeof o.vol, "%s", d);
    bool ok = start_export(k + 1, d[0] ? kToVolume : kToCard, o);
    sniprintf(out, cap, "bundle: %s", status().msg);
    (void)ok;
  } else if (!strncmp(a, "verify:", 7)) {
    start_verify(a + 7);
    sniprintf(out, cap, "bundle: %s", status().msg);
  } else if (!strncmp(a, "import", 6)) {
    ImportOpts o = import_opts();   // (what READY was planned with: a name or replace from the web page kept)
    if (a[6] == ':') {
      char* e = nullptr;
      o.skip_slots = (uint8_t)strtoul(a + 7, &e, 10);
      if (e && *e == ':') o.packages = e[1] != '0';
    }
    start_import(o);
    sniprintf(out, cap, "bundle: %s", status().msg);
  } else if (!strncmp(a, "delete:", 7)) {
    start_delete(a + 7, a + 7);
    sniprintf(out, cap, "bundle: %s", status().msg);
  } else if (!strncmp(a, "restore:", 8)) {   // 2026-10-27: the touchscreen's TRASH > RESTORE
    uint32_t id = (uint32_t)strtoul(a + 8, nullptr, 10);
    bool ok = id && prjslot::request_restore(id, "", false);
    sniprintf(out, cap, "bundle: restore %lu %s", (unsigned long)id, ok ? "asked" : "refused (busy)");
  } else if (!strcmp(a, "cancel")) {
    cancel();
    sniprintf(out, cap, "bundle: %s", phase_name(status().phase));
  } else if (!strcmp(a, "finish")) {
    if (busy()) return sniprintf(out, cap, "bundle: a job runs (%s): cancel it first", phase_name(status().phase)), true;
    finish();
    g_want = true;
    sniprintf(out, cap, "bundle: idle");
  } else if (!strncmp(a, "eject:", 6)) {
    drive_from(a + 6, d);
    const char* why = d[0] ? eject(d) : "eject:<USB|USB2|USB3>";
    sniprintf(out, cap, "bundle: %s %s", d, why ? why : "ejected: safe to remove");
    sniprintf(g_work->eject_msg, sizeof g_work->eject_msg, "%s %s", d, why ? why : "safe to pull now");
    ++g_work->eject_n;
    g_force = true;
    g_want = true;
  } else {
    sniprintf(out, cap, "bundle | bundle=list[:card|USB] | export:<card|USB>:<key> | verify:<path> | import[:skip[:pk]] | "
                        "delete:<key> | restore:<id> | cancel | finish | eject:<USB>");
  }
  return true;
}

void reset_for_tests() {
  ensure();
  memset(g_work, 0, sizeof *g_work);
  memset(g_pub, 0, sizeof *g_pub);
  g_seq = 0, g_count = 0, g_rd = g_wr = 0;
  g_avail = false, g_want = true, g_force = false, g_want_src[0] = 0, g_st_seq = ~0u, g_last_phase = kIdle;
  sniprintf(g_vols_seen, sizeof g_vols_seen, "?");
}

}  // namespace feed
}  // namespace bundle
}  // namespace core
#endif
