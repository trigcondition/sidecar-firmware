// core/bundle/kits.cpp - see kits.h.
#include "kits.h"

#include <string.h>

#include "../platform.h"

#if CORE_MSAMP
#include "prjslot.h"
#include "rename.h"
#include "../fmt/fmt.h"
#include "../lfo.h"
#include "../pool/pool.h"
#include "../project_key.h"
#include "../store/store.h"
#if CORE_POLY
#include "../fx/insert.h"
#endif

namespace core {
namespace kits {
namespace {

struct Feature { const char* name; };   // its working file /.rytm/<name>/<key>/kNNN.txt, its default /.rytm/<name>/<key>.txt
const Feature kFeatures[] = {
    {"lfo"},
#if CORE_POLY
    {"ins"},
#endif
};
constexpr int kNumFeatures = sizeof kFeatures / sizeof kFeatures[0];
constexpr uint32_t kRecMax = 3 * fmt::kPostMax;   // a record: the features' files (each at most kPostMax as saved)

struct Ev { uint8_t op, kit, src; uint32_t gen; };
Ev g_q[kPending];
int g_qn = 0;
uint32_t g_gen = 0;      // the Rytm's project changes seen (project_changing): an event of an older one is stale
uint32_t g_all[4];       // RELOAD ALL: the kits still to reload (a bit each), a few a pass
int g_scan = -1;         // RELOAD ALL's look for records: the next kit (-1 none), kScanPass a pass
uint32_t g_all_gen = 0;
char g_name[24] = "";    // the Rytm's last 10 name (name_seen)
Stats g_st;
char g_rec[kRecMax];   // the record built
char g_src[kRecMax];   // the record read (a copy: the store's bytes may move while puts run)
char g_old[kRecMax];   // the .prv there before (a copy)
constexpr int kScanPass = 16;
const char kNone[] = "#none\n";   // a section: the kit had no file of its own (it inherits the project's default file)
constexpr uint32_t kNoneLen = sizeof kNone - 1;

CORE_COLD void key_now(char* k) { project_key(project, k, kKeyMax); }
CORE_COLD void rec_path(const char* key, int kit, const char* ext, char* out, int cap) {
  sniprintf(out, cap, CORE_STR("/.rytm/kits/%s/k%03d.%s"), key, kit + 1, ext);
}
CORE_COLD void work_path(const Feature& f, const char* key, int kit, char* out, int cap) {
  if (kit < 0) sniprintf(out, cap, CORE_STR("/.rytm/%s/%s.txt"), f.name, key);
  else sniprintf(out, cap, CORE_STR("/.rytm/%s/%s/k%03d.txt"), f.name, key, kit + 1);
}

// the modules hold kit k now: their edits saved (queued) into the store, or their files read again from it
CORE_COLD bool live(int k) {
  return lfo::kit() == k
#if CORE_POLY
         || ins::kit() == k
#endif
      ;
}
CORE_COLD void modules_again() {   // each saves its unsaved edits (queued, into the store) and loads its kit's file
  lfo::project_changed();
#if CORE_POLY
  ins::project_changed();
#endif
}

// a feature's working text of kit k: its own file (*d, *n), or none of its own (*d = kNone: it inherits the project's
// default file - round 5, L4: kept so, not copied into a file of its own); false when the store cannot say (kLocked /
// kUnknown): never taken for absent
CORE_COLD bool text_of(const Feature& f, const char* key, int k, const uint8_t** d, uint32_t* n) {
  char p[96];
  work_path(f, key, k, p, sizeof p);
  store::State s = store::get(p, d, n);
  if (s == store::kAbsent) *d = (const uint8_t*)kNone, *n = kNoneLen;
  else if (s != store::kPresent) {
    log(CORE_STR("kits: %s %s: nothing done"), p, s == store::kLocked ? "cannot be read" : "not in the store");
    return false;
  }
  return true;
}
// the text of section name in a record (between "=name" and the next "=..." line or its end); false: not there
CORE_COLD bool section(const char* r, uint32_t n, const char* name, const char** s, uint32_t* sn) {
  uint32_t nl = (uint32_t)strlen(name);
  for (uint32_t i = 0; i < n;) {
    uint32_t e = i;
    while (e < n && r[e] != '\n') ++e;
    if (e - i == nl + 1 && r[i] == '=' && !memcmp(r + i + 1, name, nl)) {
      uint32_t a = e < n ? e + 1 : n, b = a;
      while (b < n && !(r[b] == '=' && (b == a || r[b - 1] == '\n'))) ++b;
      *s = r + a, *sn = b - a;
      return true;
    }
    i = e + 1;
  }
  return false;
}
CORE_COLD bool same(const uint8_t* a, uint32_t an, const char* b, uint32_t bn) {
  if (an && a[an - 1] != '\n' && bn == an + 1 && b[an] == '\n') bn = an;   // (a record ends each section's last line)
  return an == bn && !memcmp(a, b, an);
}

// kit k's record ("# ..." then "=<feature>" and its text, for each feature) into g_rec; false: refused (logged).
// keep (round 5, L2): a reload's .prv - a feature whose working text is already the saved one (sv, the record being
// put back: a reload cut between its features' writes, then done again) keeps the old .prv's section (old), so the
// working copy that reload kept aside is not replaced by the half-reloaded one
CORE_COLD bool build(const char* key, int k, uint32_t* len, const char* sv = nullptr, uint32_t svn = 0,
                     const char* old = nullptr, uint32_t oldn = 0) {
  uint32_t n = (uint32_t)sniprintf(g_rec, kRecMax, CORE_STR("# Sidecar kit record: kit %03d's per-kit files\n"), k + 1);
  for (const Feature& f : kFeatures) {
    const uint8_t* d;
    uint32_t dn;
    if (!text_of(f, key, k, &d, &dn)) return false;
    const char* ss;
    uint32_t ssn;
    if (sv && old && section(sv, svn, f.name, &ss, &ssn) && same(d, dn, ss, ssn) && section(old, oldn, f.name, &ss, &ssn))
      d = (const uint8_t*)ss, dn = ssn, ++g_st.kept_aside;
    for (uint32_t i = 0; i < dn; ++i)   // (a line of its own starting with '=' would end the section early)
      if (d[i] == '=' && (i == 0 || d[i - 1] == '\n')) {
        log(CORE_STR("kits: kit %03d's %s has a line starting with '=': no record"), k + 1, f.name);
        return false;
      }
    uint32_t need = (uint32_t)strlen(f.name) + 2 + dn + 1;
    if (n + need > kRecMax) {
      log(CORE_STR("kits: kit %03d's record does not fit (%u bytes): no record"), k + 1, (unsigned)(n + need));
      return false;
    }
    n += (uint32_t)sniprintf(g_rec + n, kRecMax - n, "=%s\n", f.name);
    memcpy(g_rec + n, d, dn), n += dn;
    if (dn && d[dn - 1] != '\n') g_rec[n++] = '\n';
  }
  *len = n;
  return true;
}
// a file's new text: into the store and posted (queued) to the card
CORE_COLD void put(const char* path, const char* d, uint32_t n) {
  static const char kDefaults[] = "# Sidecar: this kit's defaults\n";   // (an empty section: no line, the defaults)
  if (!n) d = kDefaults, n = sizeof kDefaults - 1;
  store::put(path, (const uint8_t*)d, n);
  if (!fmt::post_queue(path, d, n)) log(CORE_STR("kits: %s not written to the card (refused or out of memory: in RAM only)"), path);
}
// a section put back as kit k's working file of feature f. "#none" (the kit had no file of its own): a kit with none
// now keeps inheriting the project's default; one that has its own now (made by edits since) gets the default's text
// as it is now (there is no posted remove: the closest to "no file of its own"; it no longer follows later edits of
// the default - documented, spec 14)
CORE_COLD void apply(const Feature& f, const char* key, int k, const char* t, uint32_t tn) {
  char p[96];
  work_path(f, key, k, p, sizeof p);
  if (tn == kNoneLen && !memcmp(t, kNone, kNoneLen)) {
    const uint8_t* d;
    uint32_t n;
    if (store::get(p, &d, &n) == store::kAbsent) return;
    char dp[96];
    work_path(f, key, -1, dp, sizeof dp);
    if (store::get(dp, &d, &n) == store::kPresent && n <= kRecMax) memcpy(g_old, d, n), put(p, g_old, n);
    else put(p, "", 0);
    return;
  }
  put(p, t, tn);
}

CORE_COLD void save(int t, int w) {
  char key[kKeyMax], p[96];
  key_now(key);
  if (live(w)) modules_again();   // (the edits of the kit saved into the store first: the record is the kit as it is)
  uint32_t n;
  if (!build(key, w, &n)) { ++g_st.refused; return; }
  rec_path(key, t, "sks", p, sizeof p);
  put(p, g_rec, n);
  if (t != w) {   // SAVE KIT to another kit: its working copy is the saved one now (and the Rytm's active kit)
    memcpy(g_src, g_rec, n);
    for (const Feature& f : kFeatures) {
      const char* s;
      uint32_t sn;
      if (section(g_src, n, f.name, &s, &sn)) apply(f, key, t, s, sn);
    }
    if (live(t)) modules_again();
  }
  ++g_st.saves;
  log(CORE_STR("kits: kit %03d saved (from kit %03d)"), t + 1, w + 1);
}

// RELOAD (from kNNN.sks, keeping the working copy as kNNN.prv first) or RESTORE (from kNNN.prv)
CORE_COLD void reload(int k, bool restore) {
  char key[kKeyMax], p[96];
  key_now(key);
  rec_path(key, k, restore ? "prv" : "sks", p, sizeof p);
  const uint8_t* d;
  uint32_t n;
  store::State s = store::get(p, &d, &n);
  if (s == store::kAbsent) {
    ++g_st.kept;
    log(CORE_STR("kits: kit %03d %s on the Rytm: no %s Sidecar state, the working copy kept"), k + 1,
        restore ? "restored" : "reloaded", restore ? "kept-aside" : "saved");
    return;
  }
  if (s != store::kPresent || n > kRecMax) {
    ++g_st.refused;
    log(CORE_STR("kits: %s %s: kit %03d not followed"), p, s == store::kPresent ? "too big" : "cannot be read", k + 1);
    return;
  }
  memcpy(g_src, d, n);
  uint32_t on = 0;   // the .prv there now (a RELOAD: its sections kept for a feature already reloaded, L2)
  rec_path(key, k, "prv", p, sizeof p);
  if (!restore && store::get(p, &d, &on) == store::kPresent && on <= kRecMax) memcpy(g_old, d, on);
  else on = 0;
  if (live(k)) modules_again();   // (the edits into the store: they are what .prv keeps)
  uint32_t rn;   // the working copy kept aside first (a RESTORE too: a swap, so the copy it replaces is kept in turn)
  if (!(restore ? build(key, k, &rn) : build(key, k, &rn, g_src, n, on ? g_old : nullptr, on))) {
    ++g_st.refused;   // (it could not be kept: nothing replaced)
    return;
  }
  put(p, g_rec, rn);
  for (const Feature& f : kFeatures) {
    const char* t;
    uint32_t tn;
    if (section(g_src, n, f.name, &t, &tn)) apply(f, key, k, t, tn);   // (no section: a record from before it, kept)
  }
  if (live(k)) modules_again();
  if (restore) ++g_st.restores;
  else ++g_st.reloads;
  log(CORE_STR("kits: kit %03d %s"), k + 1, restore ? "restored (the kept-aside copy)" : "reloaded (its saved copy)");
}

CORE_COLD void push(uint8_t op, int kit, int src) {
  if (g_qn >= kPending) {
    ++g_st.dropped;
    log(CORE_STR("kits: %d kit reports waiting: op %d of kit %03d dropped"), g_qn, op, kit + 1);
    return;
  }
  g_q[g_qn++] = {op, (uint8_t)kit, (uint8_t)src, g_gen};
}

}  // namespace

CORE_COLD void sysex(const uint8_t* d, int len) {
  if (len < 12 || d[5] != 1) return;
  int op = d[6], kit = (d[7] << 7 | d[8]) - 1, src = (d[9] << 7 | d[10]) - 1;
  if (kit < 0 || kit > 127 || op < kSave || op > kReloadAll) {
    log(CORE_STR("kits: 4C op %d kit %d not understood"), op, kit + 1);
    return;
  }
  push((uint8_t)op, kit, src >= 0 && src <= 127 ? src : kit);
}

CORE_COLD void project_changing() { ++g_gen; }
CORE_COLD void name_seen(const uint8_t* n, int len) {
  char b[sizeof g_name];
  int k = 0;
  for (; k < len && k < (int)sizeof b - 1 && n[k] != 0xF7; ++k) b[k] = (char)n[k];
  b[k] = 0;
  // (round 6, N3: name mode only - in slot mode 0026's ACTIVE / LOAD mark every change, in order with the 4C; 0021's 10
  // comes on its own path, maybe late, and a rename of the active project is no change of project)
  if (g_name[0] && strcmp(b, g_name) && prjslot::mode() != prjslot::kSlot) project_changing();
  memcpy(g_name, b, sizeof g_name);
}
CORE_COLD void lost(int n) {
  ++g_st.lost;
  log(CORE_STR("kits: the Rytm dropped %d reports: a SAVE / RELOAD KIT may be missed - the kits' saved copies may be "
               "older than the Rytm's (save the kit again to be sure)"), n);
}

CORE_COLD bool tick() {
  // the store is the project's and complete, nothing moves its files, no switch or question waits: the events are of
  // the project now loaded - unless the Rytm changed projects after one came (its generation): dropped, never applied
  // to the next project (round 5, H1)
  if (!store::ready() || project_loading() || rename::moving() || rename::busy() || prjslot::busy()) return false;
  if (g_scan >= 0 || (g_all[0] | g_all[1] | g_all[2] | g_all[3])) {
    if (g_all_gen != g_gen) {
      memset(g_all, 0, sizeof g_all), g_scan = -1, ++g_st.stale;
      log(CORE_STR("kits: RELOAD ALL of the project before not finished: stopped (the Rytm is on another project)"));
      return true;
    }
    if (g_scan >= 0) {   // (round 5, L1: kScanPass store lookups a pass, not 128 in one)
      char key[kKeyMax], p[96];
      key_now(key);
      const uint8_t* d;
      uint32_t n;
      int e = g_scan + kScanPass;
      uint32_t looked = 0;
      for (; g_scan < e && g_scan < 128; ++g_scan, ++looked) {
        rec_path(key, g_scan, "sks", p, sizeof p);
        if (store::get(p, &d, &n) == store::kPresent) g_all[g_scan >> 5] |= 1u << (g_scan & 31);
      }
      if (looked > g_st.scan_max) g_st.scan_max = looked;   // (the store lookups this pass made: round 6, N5)
      if (g_scan >= 128) g_scan = -1;
      return true;
    }
    for (int i = 0, done = 0; i < 128 && done < 4; ++i)   // four kits a pass
      if (g_all[i >> 5] >> (i & 31) & 1) g_all[i >> 5] &= ~(1u << (i & 31)), reload(i, false), ++done;
    return true;
  }
  if (!g_qn) return false;
  Ev e = g_q[0];
  for (int i = 1; i < g_qn; ++i) g_q[i - 1] = g_q[i];
  --g_qn;
  if (e.gen != g_gen) {
    ++g_st.stale;
    log(CORE_STR("kits: op %d of kit %03d came before the Rytm changed projects: dropped (not applied to %s)"), e.op,
        e.kit + 1, project);
    return true;
  }
  switch (e.op) {
    case kSave: save(e.kit, e.src); break;
    case kReload: reload(e.kit, false); break;
    case kRestore: reload(e.kit, true); break;
    case kReloadAll:   // every kit with a saved record (the others keep their working copy, as a single reload's)
      g_scan = 0, g_all_gen = g_gen;
      ++g_st.reload_alls;
      log(CORE_STR("kits: every kit reloaded on the Rytm"));
      break;
    default: break;
  }
  return true;
}

CORE_COLD void want_files() {
  char key[kKeyMax], p[96];
  key_now(key);
  for (int k = 0; k < 128; ++k) {
    rec_path(key, k, "sks", p, sizeof p), store::want(p);
    rec_path(key, k, "prv", p, sizeof p), store::want(p);
  }
}

CORE_COLD void reset() {
  g_qn = 0;
  memset(g_all, 0, sizeof g_all);
  g_scan = -1, g_name[0] = 0;
  g_st = Stats();
}
CORE_COLD const Stats& stats() { return g_st; }

}  // namespace kits
}  // namespace core
#endif
