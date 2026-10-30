// core/bundle/prjslot.cpp - see prjslot.h (docs/superpowers/specs/2026-10-27-project-slot-keys-design.md).
#include "prjslot.h"

#include <stdlib.h>
#include <string.h>

#include "../platform.h"

namespace core {
namespace prjslot {

CORE_COLD void slot_key(int slot, char* out, int cap) {
  if (slot < 0 || slot > kSlots) slot = 0;
  sniprintf(out, cap, "@%03d", slot);
}
CORE_COLD int slot_of(const char* key) {
  if (!is_slot_key(key)) return -1;
  return (key[1] - '0') * 100 + (key[2] - '0') * 10 + (key[3] - '0');
}

}  // namespace prjslot
}  // namespace core

#if CORE_MSAMP
#include "bundle_int.h"
#include "json.h"
#include "kits.h"
#include "projects.h"
#include "rename.h"
#include "../fmt/fmt.h"
#include "../host_link.h"
#include "../lfo.h"
#include "../lib/pools.h"
#include "../msamp/cardq.h"
#include "../pool/pool.h"
#include "../user/um.h"
#if CORE_POLY
#include "../fx/insert.h"
#include "../seq/seq.h"
#include "../song/song.h"
#endif

namespace core {
namespace prjslot {

namespace {

constexpr char kFile[] = "/.rytm/prjslot.txt";
constexpr char kTrash[] = "/.rytm/trash";
constexpr int kMaxCand = 24;
constexpr int kQueue = 24;
constexpr int kDepth = 4;               // the trash's folders: entry / item / file (and one spare)
constexpr uint32_t kListMax = 32768;    // a folder's names (a pattern folder: 257 names of ~10 B)
constexpr int kMarks = 32;
constexpr int kDeclined = 16;
constexpr int kBadIds = 8;              // trash entries the cap could not remove (skipped, the next oldest taken)
constexpr int kGoneMax = 4;             // a LIST's empty slots trashed at once at most (more: logged, not followed)

// ---- the candidate paths of a key: bundle.cpp's kProjectFiles (the pool lists' folder whole) -----------------------
struct Cand {
  const char* pat;   // "%s" = the key
  bool dir, songs;
};
Cand g_cand[kMaxCand];
int g_ncand = 0;
constexpr char kProjDir[] = "/.rytm/projects/%s";
void cands() {
  if (g_ncand) return;
  for (int i = 0; i < bundle::kNumProjectFiles && g_ncand < kMaxCand - 1; ++i) {
    const bundle::ProjectFile& r = bundle::kProjectFiles[i];
    if (!strncmp(r.card, "/.rytm/projects/", 16)) continue;   // (in the folder below)
    g_cand[g_ncand++] = {r.card, r.how != bundle::kOne, r.how == bundle::kSongs};
  }
  g_cand[g_ncand++] = {kProjDir, true, false};
}
void cand_path(int i, const char* key, char* out, int cap) { sniprintf(out, cap, g_cand[i].pat, key); }
// the trash's name for candidate i: its path without /.rytm/, '/' as '!', the key as '%' ("lfo!%.txt", "seq!%")
void cand_item(int i, char* out, int cap) {
  const char* p = g_cand[i].pat + 7;   // past "/.rytm/"
  int k = 0;
  for (; *p && k < cap - 1; ++p) {
    if (p[0] == '%' && p[1] == 's') out[k++] = '%', ++p;
    else out[k++] = *p == '/' ? '!' : *p;
  }
  out[k] = 0;
}
void stage_of(const char* key, char* out, int cap) { sniprintf(out, cap, "%s~", key); }   // a copy's staging key
uint32_t songs_mask() {
  uint32_t m = 0;
  for (int i = 0; i < g_ncand; ++i)
    if (g_cand[i].songs) m |= 1u << i;
  return m;
}

// ---- the jobs --------------------------------------------------------------------------------------------------------
enum JobK : uint8_t { kJNone = 0, kJTrash, kJMove, kJCopy, kJSwitch, kJRestore, kJRemove, kJEmpty, kJRecover };
struct Job {
  uint8_t k;
  bool reload, replace, checked, rekey, again;
  int slot, mark;                // mark: the slot whose next name is taken without a question once this job succeeds
  uint32_t id;
  char a[kKeyMax + 2], b[kKeyMax + 2];
  char name[24];
  char why[12];
  char line[96];                 // kJRecover: the journal line
  char pair[kKeyMax + 2];        // kJTrash of a SAVE TO / PASTE: the copy's source, journaled with it (one operation)
};
enum Res { kWait = 0, kMore, kDone };

// a tree removal (the trash's cap, emptying, one entry removed for good): a folder listed once, its entries one by one
struct RmLevel {
  char path[cardq::kPathMax];
  uint8_t st;          // 0 try remove, 1 list, 2 children, 3 remove again
  uint32_t len, pos;
  char* names;
};

struct S {
  uint8_t mode;                  // the session's Mode
  bool card_slot;                // prjslot.txt mode=slot (the card was migrated)
  bool dirty;                    // prjslot.txt to write
  uint32_t next;                 // the trash's next number
  char jline[96];                // the journal line (prjslot.txt job=): a card operation under way ("" none)
  char held[24];                 // a 10 held (undecided, a card keyed by slot)
  uint32_t held_ms;
  int rytm_slot;                 // the Rytm's last ACTIVE (-1 none yet)
  char rytm_name[24];
  // the active slot cleared / pasted over: resolved when it is left or reloaded, cancelled by a SAVE of it
  struct { uint8_t kind; int slot, src; char why[12]; } pend;   // kind 0 none, 1 trash, 2 replace (from src)
  uint8_t renamed[kMarks];       // slots whose next name is taken without a question (1-based; 0 free)
  struct { uint8_t slot; char key[kKeyMax]; } declined[kDeclined];
  uint32_t bad[kBadIds];         // trash entries the cap could not remove
  uint8_t suspect[kSlots + 1];   // slots whose files a trash / copy could not follow: the next switch asks (keep / trash);
                                 // persisted (prjslot.txt suspect=): a SAVE TO / PASTE sets it until its copy is placed
  // the Rytm's slot list (4B 08 / 09)
  char list_name[kSlots + 1][16];
  uint8_t list_occ[kSlots + 1];
  bool lost;                     // a LOST since the last LIST: resync from the next
  uint16_t list_seen;            // the 16 LIST messages of the list being received (bit k: first slot 8k + 1)
  uint32_t list_hash, list_prev; // this list's fingerprint, the last complete one's (0: none since the connect)
  bool skip_songs;               // the job leaves the songs folder (SONG RECORD runs on it: quiet's bound)
  // the queue (a deque: a question's answer puts its job in front)
  Job q[kQueue];
  int qh, qn;
  Job cur;                       // the job running (k kJNone: none)
  uint8_t ph, qst, js, sub;
  bool moving, logged;
  int i;                         // the candidate / op being looked at
  uint32_t mask_a, mask_b, mask_s;
  uint32_t t0;
  // renames: src -> dst, undone newest first on a refusal
  char src[kMaxCand][cardq::kPathMax], dst[kMaxCand][cardq::kPathMax];
  int nops, done, stuck;
  bool failed;
  // copies
  char cfrom[cardq::kPathMax], cto[cardq::kPathMax];
  uint64_t off;
  uint32_t lpos, llen, cdone, cfiles;
  bool songs_phase;
  // the trash listing
  uint32_t tcount, tmin, tmax;
  // a removal
  RmLevel rm[kDepth];
  int rmd;
  bool rm_failed;
  // the question
  uint8_t ask;
  uint32_t asks, ask_ms;
  char ask_l[5][28];
  char ask_key[kKeyMax];
  int ask_files;
  char last[48];
  uint32_t failures;             // jobs that did not do what the Rytm did (logged, said, counted: /status)
  volatile uint32_t jobs;        // jobs ended (the PROJECT pages' lists read again: core/bundle/feed.cpp)
  char info[640];
  char tmp[cardq::kPathMax];
};
S s;
char* g_list = nullptr;          // a folder's names (copies, the trash listing, restore)
cardq::Req R;
File g_file;

// core 0's requests (the web page, the touchscreen), drained by the control core's tick
struct WebReq {
  char op;   // 'T' trash a key, 'R' restore, 'X' remove an entry, 'E' empty
  uint32_t id;
  bool replace;
  char key[kKeyMax], name[24], why[12];
};
constexpr uint32_t kWebReqs = 8;
WebReq g_wr[kWebReqs];
volatile uint32_t g_wr_w = 0, g_wr_r = 0;
volatile uint32_t g_hold = 0;    // the bundle import's COMMIT (core 0): no new job starts (hold_jobs)
inline uint32_t ld(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void sto(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void upper(char* b) {
  for (; *b; ++b)
    if (*b >= 'a' && *b <= 'z') *b = (char)(*b - 32);
}
void say(const char* line) {
  sniprintf(s.last, sizeof s.last, "%s", line);
  upper(s.last);
}
void suspect(const char* key) {   // (a slot key's files that may not be the Rytm's project there now)
  int sl = slot_of(key);
  if (sl >= 1 && !s.suspect[sl]) s.suspect[sl] = 1, s.dirty = true;
}
void unsuspect(const char* key) {
  int sl = slot_of(key);
  if (sl >= 1 && s.suspect[sl]) s.suspect[sl] = 0, s.dirty = true;
}
void fail_say(const char* line) {   // a job that did not follow the Rytm: loud (the log, the PROJECT page, /status)
  ++s.failures;
  say(line);
  log(CORE_STR("prjslot: NOT FOLLOWED: %s"), line);
}
void clean(const char* in, char* out, int cap) {   // printable, one line (info.txt, prjslot.txt)
  int k = 0;
  for (const char* c = in ? in : ""; *c && k < cap - 1; ++c) out[k++] = (*c < 0x20 || *c > 0x7e) ? '_' : *c;
  out[k] = 0;
}

// ---- marks ------------------------------------------------------------------------------------------------------------
void mark_renamed(int slot) {
  if (slot <= 0) return;
  for (uint8_t m : s.renamed)
    if (m == slot) return;
  for (uint8_t& m : s.renamed)
    if (!m) { m = (uint8_t)slot, s.dirty = true; return; }
  memmove(s.renamed, s.renamed + 1, kMarks - 1), s.renamed[kMarks - 1] = (uint8_t)slot, s.dirty = true;
}
bool take_renamed(int slot) {   // true (and the mark gone) when slot was marked
  for (uint8_t& m : s.renamed)
    if (m == slot && slot > 0) { m = 0, s.dirty = true; return true; }
  return false;
}
bool declined(int slot, const char* key) {
  for (auto& d : s.declined)
    if (d.slot == slot && !strcmp(d.key, key)) return true;
  return false;
}
void decline(int slot, const char* key) {
  if (declined(slot, key)) return;
  for (auto& d : s.declined)
    if (!d.slot) { d.slot = (uint8_t)slot, sniprintf(d.key, sizeof d.key, "%s", key), s.dirty = true; return; }
  memmove(s.declined, s.declined + 1, sizeof s.declined - sizeof s.declined[0]);
  s.declined[kDeclined - 1].slot = (uint8_t)slot;
  sniprintf(s.declined[kDeclined - 1].key, kKeyMax, "%s", key);
  s.dirty = true;
}
bool is_bad(uint32_t id) {
  for (uint32_t b : s.bad)
    if (b == id) return true;
  return false;
}

// ---- the queue --------------------------------------------------------------------------------------------------------
Job* push(uint8_t k, bool front = false) {
  if (s.qn >= kQueue) {
    fail_say("TOO MANY EVENTS: ONE NOT FOLLOWED");
    return nullptr;
  }
  int at;
  if (front) s.qh = (s.qh + kQueue - 1) % kQueue, at = s.qh;
  else at = (s.qh + s.qn) % kQueue;
  ++s.qn;
  Job& j = s.q[at];
  memset(&j, 0, sizeof j);
  j.k = k;
  return &j;
}
Job* job_trash(const char* key, const char* why, const char* name, int mark = 0, bool front = false) {
  Job* j = push(kJTrash, front);
  if (j) {
    sniprintf(j->a, sizeof j->a, "%s", key), sniprintf(j->why, sizeof j->why, "%s", why);
    sniprintf(j->name, sizeof j->name, "%s", name ? name : "");
    j->mark = mark;
  }
  return j;
}
Job* job_two(uint8_t k, const char* a, const char* b, const char* name, int mark = 0, bool front = false) {
  Job* j = push(k, front);
  if (j) {
    sniprintf(j->a, sizeof j->a, "%s", a), sniprintf(j->b, sizeof j->b, "%s", b);
    sniprintf(j->name, sizeof j->name, "%s", name ? name : "");
    j->mark = mark;
  }
  return j;
}
void job_switch(int slot, const char* name, bool reload) {
  if (Job* j = push(kJSwitch)) {
    j->slot = slot, j->reload = reload;
    slot_key(slot, j->b, sizeof j->b);
    sniprintf(j->name, sizeof j->name, "%s", name);
  }
}
void requeue_front(const Job& j) {   // the job running, again after what goes in front of it
  if (Job* p = push(j.k, true)) *p = j;
}
// the switch the queue ends on (the key the Sidecar will be on): its job, or none
Job* last_switch() {
  for (int k = s.qn - 1; k >= 0; --k) {
    Job& j = s.q[(s.qh + k) % kQueue];
    if (j.k == kJSwitch) return &j;
  }
  if (s.cur.k == kJSwitch) return &s.cur;
  return nullptr;
}
const char* name_of(const char* key) {   // the index's name for a key ("" none)
  const projects::Info* e = projects::find(key);
  return e ? e->rytm : "";
}
void drop_index(const char* key) {   // a key whose files went: its index entry too (a stale name asks nothing later)
  if (strcmp(key, project)) projects::drop(key);
}

// ---- the per-project modules' keys: every module that keeps unsaved edits keyed by the project in RAM, re-pointed at
// once (a SAVE AS copy placed, a move of the current project) - their held edits then save under the new key
void rekey_all(const char* from, const char* to) {
  lfo::rekey(to);
#if CORE_POLY
  ins::rekey(to);
  seq::rekey(to);
#endif
  pools::rekey(from, to);   // (the pool lists: their dirty lists saved under to)
  junolib.rekey(to);        // (the JUNO and FM lists: their next writes)
  dxlib.rekey(to);
  um::rekey(to);            // (the user machines' slot map)
  log(CORE_STR("prjslot: the project's modules now save under %s (was %s)"), to, from);
}

// ---- prjslot.txt (the settings and the journal) ----------------------------------------------------------------------
void save_file() {
  if (!s.dirty || !card_ok) return;
  char* t = fmt::post_begin();
  if (!t) return;   // another settings write in flight: at a later pass
  uint32_t n = (uint32_t)sniprintf(t, 160, "# Sidecar projects by the Rytm's slot (core/bundle/prjslot.h)\nmode=%s\ntrash_next=%lu\n",
                                   s.card_slot ? "slot" : "name", (unsigned long)s.next);
  if (s.jline[0]) n += (uint32_t)sniprintf(t + n, fmt::kPostMax - n, "job=%s\n", s.jline);
  for (uint8_t m : s.renamed)
    if (m) n += (uint32_t)sniprintf(t + n, fmt::kPostMax - n, "renamed=%u\n", (unsigned)m);
  for (auto& d : s.declined)
    if (d.slot) n += (uint32_t)sniprintf(t + n, fmt::kPostMax - n, "declined=%u %s\n", (unsigned)d.slot, d.key);
  for (int sl = 1; sl <= kSlots; ++sl)
    if (s.suspect[sl]) n += (uint32_t)sniprintf(t + n, fmt::kPostMax - n, "suspect=%d\n", sl);
  if (fmt::post_end(kFile, n)) s.dirty = false;
}
// the journal: line ("" clears it) on the card before the card operation it names starts (posted; kDone once written)
Res journal(const char* line) {
  if (!s.js) {
    sniprintf(s.jline, sizeof s.jline, "%s", line);
    s.dirty = true, s.js = 1;
  }
  if (!card_ok) { s.js = 0; return kDone; }   // (no card index up - a bundle tool, a test: nothing to journal into)
  save_file();
  if (s.dirty || fmt::pending(kFile)) return kWait;
  s.js = 0;
  return kDone;
}

// ---- one card-queue request at a time ---------------------------------------------------------------------------------
// true: done (R.got); false: waiting (posted, running, the queue full, or the streams need the card first)
bool req(cardq::Kind k, const char* path, uint64_t off = 0, uint8_t* buf = nullptr, uint32_t len = 0, File* f = nullptr) {
  if (cardq::idle(&R) && R.state != cardq::kDone && cardq::worker() && cardq::busy()) return false;   // (the streams first)
  return cardq::async(&R, k, f ? f : &g_file, path, off, buf, len);
}

// ---- the phases' parts ------------------------------------------------------------------------------------------------
// SONG RECORD writes its folder itself (not through idle_save): no move or copy of the project's songs while it runs
bool song_busy(const char* key) {
#if CORE_POLY
  if (strcmp(key, project)) return false;
  int st = song::status().state;
  return st == song::kArming || st == song::kArmed || st == song::kRec || st == song::kSaving || st == song::kReplayReady ||
         st == song::kReplaying;
#else
  (void)key;
  return false;
#endif
}
// quiet: the key's unsaved edits posted and on the card, nothing of it in flight. The current project: its edits saved
// (the rename's FLUSH) and their posted / queued writes landed; the one just left: its store fill done (until then its
// edits still save into its own files) and the writes landed. Never a card call nor a wait here: kWait until then (a
// log after kFlushMs). Edits still coming after kFlushMs stay in RAM and are saved after the job (to the key the job
// leaves them: the copy rekeys them to its target).
Res quiet(const char* key, uint32_t ms) {
  if (!s.qst) {
    s.t0 = ms, s.logged = false, s.qst = 1;
    if (!strcmp(key, project)) {
#if CORE_POLY
      seq::project_leaving();
      ins::settings_due();
#endif
      lfo::save_due();
    }
  }
  bool dirty = lfo::dirty();
#if CORE_POLY
  dirty = dirty || ins::settings_dirty(nullptr);
#endif
  bool songs = song_busy(key);
  if (songs && ms - s.t0 >= kFlushMs && !s.skip_songs) {   // (bounded: the job goes on without the songs folder)
    s.skip_songs = true;
    say("SONGS LEFT: SONG RECORD RUNS");
    log(CORE_STR("prjslot: %s: SONG RECORD runs: its songs folder is left out of this job"), key);
  }
  bool waiting = project_loading() || fmt::post_busy() || pools::saving() || (songs && !s.skip_songs) ||
                 (dirty && !strcmp(key, project) && ms - s.t0 < kFlushMs);
  if (waiting) {
    if (!s.logged && ms - s.t0 >= kFlushMs)
      s.logged = true, log(CORE_STR("prjslot: %s: still waiting for its saves (loading %d, posted %d, song %d)"), key,
                           (int)project_loading(), (int)fmt::post_busy(), (int)song_busy(key));
    return kWait;
  }
  s.qst = 2;
  return kDone;
}
// scan: which candidates of key exist (bit i), one look a request
Res scan(const char* key, uint32_t* mask) {
  if (s.i == 0) *mask = 0;
  if (s.i >= g_ncand) return kDone;
  cand_path(s.i, key, s.tmp, sizeof s.tmp);
  if (!req(cardq::kExistsReq, s.tmp)) return kWait;
  if (R.got == 1) *mask |= 1u << s.i;
  ++s.i;
  return kMore;
}
int bits(uint32_t m) {
  int n = 0;
  for (; m; m &= m - 1) ++n;
  return n;
}
// the renames src[] -> dst[] (s.nops). Live (fwd false): a refusal undoes the made ones (newest first; s.stuck the ones
// that could not) and sets s.failed. Forward (a journal's recovery after a cut): each op done when its source is there
// and its target is not, skipped otherwise; a refusal stops (the journal stays: tried again at the next boot).
Res renames(bool fwd) {
  if (s.failed && !fwd) {   // undoing
    if (s.done <= 0) return kDone;
    int k = s.done - 1;
    if (!req(cardq::kRenameReq, s.dst[k], 0, (uint8_t*)s.src[k], (uint32_t)strlen(s.src[k]) + 1)) return kWait;
    if (R.got != 1) ++s.stuck, log(CORE_STR("prjslot: %s could not go back to %s"), s.dst[k], s.src[k]);
    --s.done;
    return kMore;
  }
  if (s.done >= s.nops) return kDone;
  int k = s.done;
  if (fwd && s.sub == 0) {   // the source there?
    if (!req(cardq::kExistsReq, s.src[k])) return kWait;
    if (R.got != 1) { ++s.done; return kMore; }
    s.sub = 1;
  }
  if (fwd && s.sub == 1) {   // the target not?
    if (!req(cardq::kExistsReq, s.dst[k])) return kWait;
    s.sub = 0;
    if (R.got == 1) { log(CORE_STR("prjslot: recovery: %s and %s both there: %s left"), s.src[k], s.dst[k], s.src[k]); ++s.done; return kMore; }
    s.sub = 2;
  }
  if (!req(cardq::kRenameReq, s.src[k], 0, (uint8_t*)s.dst[k], (uint32_t)strlen(s.dst[k]) + 1)) return kWait;
  s.sub = 0;
  if (R.got == 1) { ++s.done; return kMore; }
  log(CORE_STR("prjslot: %s -> %s refused%s"), s.src[k], s.dst[k], fwd ? "" : ": the ones moved go back");
  s.failed = true;
  return fwd ? kDone : kMore;
}
// a tree removal (rm_begin, then rm_step until kDone; s.rm_failed when it stopped short). Only for what the user removes
// for good (the web page's remove / empty) and the cap's oldest entry
void rm_begin(const char* path) {
  s.rmd = 1, s.rm_failed = false;
  RmLevel& l = s.rm[0];
  sniprintf(l.path, sizeof l.path, "%s", path);
  l.st = 0, l.len = l.pos = 0;
}
Res rm_step() {
  if (s.rmd <= 0) return kDone;
  RmLevel& l = s.rm[s.rmd - 1];
  if (l.st == 0 || l.st == 3) {
    if (!req(cardq::kRemoveReq, l.path)) return kWait;
    if (R.got == 1) { --s.rmd; return kMore; }
    if (l.st == 3) { s.rm_failed = true, s.rmd = 0; log(CORE_STR("prjslot: %s could not be removed"), l.path); return kDone; }
    l.st = 1;
    return kMore;
  }
  if (l.st == 1) {
    if (!l.names) l.names = new char[kListMax];
    if (!req(cardq::kListReq, l.path, 0, (uint8_t*)l.names, kListMax)) return kWait;
    if (R.got == cardq::kListMissing) { --s.rmd; return kMore; }   // (gone meanwhile)
    if (R.got <= 0) { s.rm_failed = true, s.rmd = 0; log(CORE_STR("prjslot: %s could not be listed"), l.path); return kDone; }
    l.len = (uint32_t)R.got, l.pos = 0, l.st = 2;
    return kMore;
  }
  if (l.pos >= l.len) { l.st = 3; return kMore; }
  const char* nm = l.names + l.pos;
  l.pos += (uint32_t)strlen(nm) + 1;
  if (s.rmd >= kDepth) { s.rm_failed = true, s.rmd = 0; return kDone; }
  RmLevel& c = s.rm[s.rmd++];
  sniprintf(c.path, sizeof c.path, "%s/%s", l.path, nm);
  c.st = 0, c.len = c.pos = 0;
  return kMore;
}
uint32_t num(const char* p) {   // a trash entry's number (-1u: not one)
  uint32_t v = 0;
  if (!*p) return ~0u;
  for (; *p; ++p) {
    if (*p < '0' || *p > '9' || v > 100000000u) return ~0u;
    v = v * 10 + (uint32_t)(*p - '0');
  }
  return v;
}
void entry_dir(uint32_t id, char* out, int cap) { sniprintf(out, cap, "%s/%06lu", kTrash, (unsigned long)id); }
bool songs_take(const char* nm) {   // a song folder's file a copy takes: the take JSONs (not a journal, a .tmp, a dot file)
  size_t l = strlen(nm);
  return nm[0] != '.' && l > 5 && !strcmp(nm + l - 5, ".json");
}

// ---- the question -----------------------------------------------------------------------------------------------------
void fit(char* out, int cap, const char* n, int max) {   // a name cut to max characters (upper case)
  sniprintf(out, cap, "%.*s", max, n);
  upper(out);
}
void ask_mismatch(const Job& j, const char* was) {
  char n[24];
  s.ask = kAskMismatch, ++s.asks, s.ask_ms = now_ms();
  sniprintf(s.ask_l[0], sizeof s.ask_l[0], "SLOT %03d: A NEW NAME", j.slot);
  fit(n, sizeof n, j.name, 16), sniprintf(s.ask_l[1], sizeof s.ask_l[1], "NOW %s", n);
  fit(n, sizeof n, was[0] ? was : "?", 16), sniprintf(s.ask_l[2], sizeof s.ask_l[2], "WAS %s", n);
  sniprintf(s.ask_l[3], sizeof s.ask_l[3], "KEEP ITS SIDECAR FILES?");
  sniprintf(s.ask_l[4], sizeof s.ask_l[4], "NO: THEY GO TO THE TRASH");
  log(CORE_STR("prjslot: slot %d is %s now, its files are %s's: asking"), j.slot, j.name, was[0] ? was : "?");
}
void ask_legacy(const Job& j, const char* key, int files) {
  char n[24];
  s.ask = kAskLegacy, ++s.asks, s.ask_ms = now_ms();
  sniprintf(s.ask_key, sizeof s.ask_key, "%s", key);
  s.ask_files = files;
  sniprintf(s.ask_l[0], sizeof s.ask_l[0], "FILES FROM BEFORE SLOTS");
  fit(n, sizeof n, key, 18), sniprintf(s.ask_l[1], sizeof s.ask_l[1], "NAME %s", n);
  sniprintf(s.ask_l[2], sizeof s.ask_l[2], "FOR SLOT %03d?", j.slot);
  sniprintf(s.ask_l[3], sizeof s.ask_l[3], "%d SIDECAR FILE%s THERE", files, files == 1 ? "" : "S");
  sniprintf(s.ask_l[4], sizeof s.ask_l[4], "NO: ANOTHER SLOT MAY");
  log(CORE_STR("prjslot: slot %d (%s) has no files, %s from before slots has %d: asking"), j.slot, j.name, key, files);
}

// ---- the job steps ----------------------------------------------------------------------------------------------------
enum { kPhStart = 0, kPhQuiet, kPhScanA, kPhScanB, kPhScanS, kPhList, kPhPurge, kPhInfo, kPhJournal, kPhOps, kPhUndo,
       kPhClear, kPhCopySongs, kPhCopyQuiet, kPhCopyRest, kPhCopyDir, kPhCopyFile, kPhPlaceJ, kPhPlace, kPhAsk, kPhSwitch,
       kPhRead, kPhReadClose, kPhEntryList, kPhRmInfo, kPhRmDir, kPhRm, kPhFail };

// (the target of the switch the queue ends on is the Rytm's current project too)
bool current_or_next(const char* key) {
  Job* w = last_switch();
  return !strcmp(key, project) || (w && !strcmp(w->b, key));
}

Res step_trash(Job& j, uint32_t ms) {
  switch (s.ph) {
    case kPhStart:
      if (!strcmp(j.why, "web") && current_or_next(j.a)) {   // the web page's delete, checked again when it runs
        fail_say("DELETE REFUSED: THE CURRENT PROJECT");
        return kDone;
      }
      s.qst = 0, s.ph = kPhQuiet;
      return kMore;
    case kPhQuiet:
      if (quiet(j.a, ms) == kWait) return kWait;
      s.ph = kPhScanA, s.i = 0;
      return kMore;
    case kPhScanA: {
      Res r = scan(j.a, &s.mask_a);
      if (r != kDone) return r;
      if (s.skip_songs) s.mask_a &= ~songs_mask();
      if (!s.mask_a) {   // nothing of it on the card
        drop_index(j.a);
        if (j.mark) mark_renamed(j.mark);
        return kDone;
      }
      s.ph = kPhList;
      return kMore;
    }
    case kPhList: {   // the trash: how many, the oldest (not one the cap could not remove), the newest
      if (!req(cardq::kListReq, kTrash, 0, (uint8_t*)g_list, kListMax)) return kWait;
      s.tcount = 0, s.tmin = ~0u, s.tmax = 0;
      if (R.got > 0)
        for (uint32_t p = 0; p < (uint32_t)R.got;) {
          const char* nm = g_list + p;
          p += (uint32_t)strlen(nm) + 1;
          uint32_t v = num(nm);
          if (v == ~0u) continue;
          ++s.tcount;
          if (v < s.tmin && !is_bad(v)) s.tmin = v;
          if (v > s.tmax) s.tmax = v;
        }
      else if (R.got != cardq::kListMissing && R.got != 0)
        log(CORE_STR("prjslot: %s could not be listed: the cap not checked this time"), kTrash);
      if (s.tcount && s.tmax + 1 > s.next) s.next = s.tmax + 1, s.dirty = true;
      if (s.tcount >= (uint32_t)kTrashMax && s.tmin != ~0u) {   // the cap rule: the oldest entry for good
        entry_dir(s.tmin, s.tmp, sizeof s.tmp);
        log(CORE_STR("trash: %06lu removed for good: the trash keeps %d"), (unsigned long)s.tmin, kTrashMax);
        rm_begin(s.tmp);
        s.ph = kPhPurge;
        return kMore;
      }
      if (s.tcount >= (uint32_t)kTrashMax)
        log(CORE_STR("trash: over %d entries (%lu): the oldest could not be removed - see the web page"), kTrashMax,
            (unsigned long)s.tcount);
      s.ph = kPhInfo;
      return kMore;
    }
    case kPhPurge: {
      Res r = rm_step();
      if (r != kDone) return r;
      if (s.rm_failed) {   // never a stop: this one remembered (the next oldest the next time), this trash past the cap
        bool kept = false;
        for (uint32_t& b : s.bad)
          if (!b) { b = s.tmin, kept = true; break; }
        if (!kept) memmove(s.bad, s.bad + 1, sizeof s.bad - sizeof s.bad[0]), s.bad[kBadIds - 1] = s.tmin;
        log(CORE_STR("trash: %06lu could not be removed: kept; this entry goes past %d"), (unsigned long)s.tmin, kTrashMax);
        s.ph = kPhInfo;
        return kMore;
      }
      s.ph = kPhList;
      return kMore;
    }
    case kPhInfo: {
      int n = trash_info(j.a, j.name[0] ? j.name : name_of(j.a), j.why, s.info, sizeof s.info);
      entry_dir(s.next, s.tmp, sizeof s.tmp);
      char p[cardq::kPathMax];
      sniprintf(p, sizeof p, "%s/info.txt", s.tmp);
      if (!req(cardq::kWriteReq, p, 0, (uint8_t*)s.info, (uint32_t)n)) return kWait;
      if (R.got < 0) {
        fail_say("TRASH: CANNOT WRITE THE CARD");
        suspect(j.a);
        return kDone;
      }
      s.nops = 0, s.done = 0, s.stuck = 0, s.failed = false, s.sub = 0;
      for (int i = 0; i < g_ncand; ++i)
        if (s.mask_a >> i & 1) {
          char it[48];
          cand_item(i, it, sizeof it);
          cand_path(i, j.a, s.src[s.nops], cardq::kPathMax);
          sniprintf(s.dst[s.nops], cardq::kPathMax, "%s/%s", s.tmp, it);
          ++s.nops;
        }
      s.ph = kPhJournal;
      return kMore;
    }
    case kPhJournal: {
      char line[96];
      if (j.pair[0]) sniprintf(line, sizeof line, "trash\t%s\t%lu\t%s", j.a, (unsigned long)s.next, j.pair);
      else sniprintf(line, sizeof line, "trash\t%s\t%lu", j.a, (unsigned long)s.next);
      if (journal(line) == kWait) return kWait;
      s.ph = kPhOps, s.moving = true;
      return kMore;
    }
    case kPhOps: {
      Res r = renames(false);
      if (r != kDone) return r;
      s.moving = false;
      entry_dir(s.next, s.tmp, sizeof s.tmp);
      if (s.failed) {
        suspect(j.a);
        if (s.stuck) {
          ++s.next, s.dirty = true;
          fail_say("TRASH INCOMPLETE: SEE THE TRASH");
          s.ph = kPhClear;
          return kMore;
        }
        fail_say("TRASH FAILED: NOTHING MOVED");
        s.ph = kPhRmInfo;   // (our own info.txt and the empty folder, nothing of the user's)
        return kMore;
      }
      log(CORE_STR("trash: %s (%s, %s): %d files / folders -> %s"), j.a, j.name[0] ? j.name : name_of(j.a), j.why,
          s.nops, s.tmp);
      char b[48];
      sniprintf(b, sizeof b, "TRASHED %s (%s)", j.a, j.why);
      say(b);
      ++s.next, s.dirty = true;
      drop_index(j.a);
      if (j.mark) mark_renamed(j.mark);
      s.ph = kPhClear;
      return kMore;
    }
    case kPhRmInfo: {
      char p[cardq::kPathMax];
      sniprintf(p, sizeof p, "%s/info.txt", s.tmp);
      if (!req(cardq::kRemoveReq, p)) return kWait;
      s.ph = kPhRmDir;
      return kMore;
    }
    case kPhRmDir:
      if (!req(cardq::kRemoveReq, s.tmp)) return kWait;
      s.ph = kPhClear;
      return kMore;
    case kPhClear: {   // a SAVE TO / PASTE: the journal goes straight on to its copy (a cut between them: the copy again)
      char line[96] = "";
      if (j.pair[0] && !s.failed) sniprintf(line, sizeof line, "copy\t%s\t%s", j.pair, j.a);
      return journal(line) == kWait ? kWait : kDone;
    }
    default: return kDone;
  }
}

// move (a -> b, b empty: the migration, a take from before slots, the no-slot project saved)
Res step_move(Job& j, uint32_t ms) {
  switch (s.ph) {
    case kPhStart: s.qst = 0, s.ph = kPhQuiet; return kMore;
    case kPhQuiet:
      if (quiet(j.a, ms) == kWait) return kWait;
      s.ph = kPhScanA, s.i = 0;
      return kMore;
    case kPhScanA: {
      Res r = scan(j.a, &s.mask_a);
      if (r != kDone) return r;
      if (s.skip_songs) s.mask_a &= ~songs_mask();
      if (!s.mask_a) {
        if (j.mark) mark_renamed(j.mark);
        unsuspect(j.b);
        return kDone;
      }
      s.ph = kPhScanB, s.i = 0;
      return kMore;
    }
    case kPhScanB: {
      Res r = scan(j.b, &s.mask_b);
      if (r != kDone) return r;
      if (s.mask_b) {   // never over another project's files (the trash before it failed; something came since)
        char b[48];
        sniprintf(b, sizeof b, "NOT MOVED: %s HAS FILES", j.b);
        fail_say(b);
        suspect(j.b);
        return kDone;
      }
      s.nops = 0, s.done = 0, s.stuck = 0, s.failed = false, s.sub = 0;
      for (int i = 0; i < g_ncand; ++i)
        if (s.mask_a >> i & 1) cand_path(i, j.a, s.src[s.nops], cardq::kPathMax), cand_path(i, j.b, s.dst[s.nops], cardq::kPathMax), ++s.nops;
      s.ph = kPhJournal;
      return kMore;
    }
    case kPhJournal: {
      char line[96];
      sniprintf(line, sizeof line, "move\t%s\t%s", j.a, j.b);
      if (journal(line) == kWait) return kWait;
      s.ph = kPhOps, s.moving = true;
      return kMore;
    }
    case kPhOps: {
      Res r = renames(false);
      if (r != kDone) return r;
      s.moving = false;
      if (s.failed) {
        fail_say(s.stuck ? "MOVE INCOMPLETE" : "MOVE FAILED: NOTHING MOVED");
        s.ph = kPhClear;
        return kMore;
      }
      log(CORE_STR("prjslot: %d files / folders of %s moved to %s"), s.nops, j.a, j.b);
      if (!strcmp(j.a, project)) rekey_all(j.a, j.b);   // the project in memory is b's now (its files moved)
      if (!projects::rekey(j.a, j.b) && j.name[0]) projects::note(j.b, j.name);
      if (j.mark) mark_renamed(j.mark);
      unsuspect(j.b);
      char b[48];
      sniprintf(b, sizeof b, "MOVED %s TO %s", j.a, j.b);
      say(b);
      s.ph = kPhClear;
      return kMore;
    }
    case kPhClear: return journal("") == kWait ? kWait : kDone;
    default: return kDone;
  }
}

// copy (a -> b, b empty): into the staging key b~ (journal "copy a b"), the song takes first (long: idle_save runs on),
// then FLUSH and the rest (idle_save held), then renamed into place (journal "place a b"). A half copy is never under b.
Res step_copy(Job& j, uint32_t ms) {
  char stage[kKeyMax + 2];
  stage_of(j.b, stage, sizeof stage);
  switch (s.ph) {
    case kPhStart: s.ph = kPhScanA, s.i = 0; return kMore;
    case kPhScanA: {
      Res r = scan(j.a, &s.mask_a);
      if (r != kDone) return r;
      if (!s.mask_a) {   // an empty project copied: its name for the slot
        const char* nm = j.name[0] ? j.name : name_of(j.a);
        if (nm[0]) projects::note(j.b, nm);
        if (j.mark) mark_renamed(j.mark);
        unsuspect(j.b);
        return kDone;
      }
      s.ph = kPhScanB, s.i = 0;
      return kMore;
    }
    case kPhScanB: {
      Res r = scan(j.b, &s.mask_b);
      if (r != kDone) return r;
      if (s.mask_b) {   // the trash before it did not take b's files: never over them, and no mark (its name asks)
        char b[48];
        sniprintf(b, sizeof b, "NOT COPIED: %s HAS FILES", j.b);
        fail_say(b);
        suspect(j.b);
        return kDone;
      }
      s.ph = kPhScanS, s.i = 0;
      return kMore;
    }
    case kPhScanS: {
      Res r = scan(stage, &s.mask_s);
      if (r != kDone) return r;
      if (s.sub == 1) {   // (after the copy: the place's ops, what the stage has)
        s.sub = 0;
        s.nops = 0, s.done = 0, s.stuck = 0, s.failed = false;
        for (int i = 0; i < g_ncand; ++i)
          if (s.mask_s >> i & 1) cand_path(i, stage, s.src[s.nops], cardq::kPathMax), cand_path(i, j.b, s.dst[s.nops], cardq::kPathMax), ++s.nops;
        s.ph = kPhPlace;
        return kMore;
      }
      if (s.sub == 2) {   // (after the place: anything left in the stage goes to the trash)
        s.sub = 0;
        if (s.mask_s) job_trash(stage, "failed", "", 0, true), log(CORE_STR("prjslot: %s left after the place: to the trash"), stage);
        s.ph = kPhClear;
        return kMore;
      }
      if (s.mask_s) {   // a copy cut before: its leftovers to the trash first, then this one again
        if (j.again) { fail_say("NOT COPIED: A STAGED COPY IS STUCK"); return kDone; }
        Job c = j;
        c.again = true;
        s.cur.k = kJNone;
        requeue_front(c);
        job_trash(stage, "failed", "", 0, true);
        return kMore;
      }
      s.ph = kPhJournal;
      return kMore;
    }
    case kPhJournal: {
      char line[96];
      sniprintf(line, sizeof line, "copy\t%s\t%s", j.a, j.b);
      if (journal(line) == kWait) return kWait;
      s.cdone = 0, s.cfiles = 0, s.songs_phase = true, s.i = 0;
      s.ph = kPhCopySongs;
      return kMore;
    }
    case kPhCopySongs:   // the song takes first (not held): big and immutable once written
    case kPhCopyRest: {  // then the rest (held)
      bool songs = s.ph == kPhCopySongs;
      if (songs && song_busy(j.a) && !s.skip_songs) {   // (bounded: kFlushMs, then the copy goes on without them)
        if (!s.qst) s.qst = 1, s.t0 = ms;
        if (ms - s.t0 < kFlushMs) return kWait;
        s.skip_songs = true;
        say("SONGS NOT COPIED: SONG RECORD RUNS");
        log(CORE_STR("prjslot: %s: SONG RECORD runs: its songs not copied to %s"), j.a, j.b);
      }
      if (songs && s.skip_songs) { s.ph = kPhCopyQuiet, s.qst = 0, s.i = 0; return kMore; }
      while (s.i < g_ncand && (!(s.mask_a >> s.i & 1) || g_cand[s.i].songs != songs)) ++s.i;
      if (s.i >= g_ncand) {
        if (songs) { s.ph = kPhCopyQuiet, s.qst = 0, s.i = 0; return kMore; }
        s.ph = kPhPlaceJ;
        return kMore;
      }
      s.songs_phase = songs;
      if (g_cand[s.i].dir) { s.ph = kPhCopyDir; return kMore; }
      cand_path(s.i, j.a, s.cfrom, sizeof s.cfrom), cand_path(s.i, stage, s.cto, sizeof s.cto);
      s.off = 0, s.llen = 0, s.lpos = 0, s.ph = kPhCopyFile;
      return kMore;
    }
    case kPhCopyQuiet:
      if (quiet(j.a, ms) == kWait) return kWait;
      s.moving = true;
      s.ph = kPhCopyRest, s.i = 0;
      return kMore;
    case kPhCopyDir: {
      cand_path(s.i, j.a, s.tmp, sizeof s.tmp);
      if (!req(cardq::kListReq, s.tmp, 0, (uint8_t*)g_list, kListMax)) return kWait;
      if (R.got < 0 && R.got != cardq::kListMissing) { s.ph = kPhFail; return kMore; }
      s.llen = R.got > 0 ? (uint32_t)R.got : 0, s.lpos = 0;
      s.ph = kPhCopyFile, s.off = ~0ull;   // (the first name next)
      return kMore;
    }
    case kPhCopyFile: {
      if (s.off == ~0ull) {   // a folder's next file
        for (;;) {
          if (s.lpos >= s.llen) { ++s.i, s.ph = s.songs_phase ? kPhCopySongs : kPhCopyRest; return kMore; }
          const char* nm = g_list + s.lpos;
          s.lpos += (uint32_t)strlen(nm) + 1;
          if (g_cand[s.i].songs && !songs_take(nm)) continue;   // (a song journal, a .tmp: not a take)
          cand_path(s.i, j.a, s.tmp, sizeof s.tmp), sniprintf(s.cfrom, sizeof s.cfrom, "%s/%s", s.tmp, nm);
          cand_path(s.i, stage, s.tmp, sizeof s.tmp), sniprintf(s.cto, sizeof s.cto, "%s/%s", s.tmp, nm);
          break;
        }
        s.off = 0;
      }
      if (!req(cardq::kCopyReq, s.cfrom, s.off, (uint8_t*)s.cto, cardq::kCopyMax)) return kWait;
      if (R.got < 0) {
        log(CORE_STR("prjslot: copy %s -> %s failed"), s.cfrom, s.cto);
        s.ph = kPhFail;
        return kMore;
      }
      if (R.got > 0) { s.off += (uint64_t)R.got; return kMore; }
      ++s.cdone;
      if (s.cdone % 8 == 0) {
        char b[48];
        sniprintf(b, sizeof b, "COPYING %s TO %s: %lu FILES", j.a, j.b, (unsigned long)s.cdone);
        say(b);
      }
      if (g_cand[s.i].dir) s.off = ~0ull;   // (this file complete: the folder's next)
      else ++s.i, s.ph = s.songs_phase ? kPhCopySongs : kPhCopyRest;
      return kMore;
    }
    case kPhPlaceJ: {   // the staged copy whole: renamed into place (journal "place": a cut completes it forward)
      char line[96];
      sniprintf(line, sizeof line, "place\t%s\t%s", j.a, j.b);
      if (journal(line) == kWait) return kWait;
      s.ph = kPhScanS, s.i = 0, s.sub = 1;   // (what the stage holds: an empty folder copied is not there)
      return kMore;
    }
    case kPhPlace: {   // live: a target there or a refusal undoes the made ones back into the stage (never forward)
      Res r = renames(false);
      if (r != kDone) return r;
      if (s.failed) {
        s.moving = false;
        suspect(j.b);
        char b[48];
        sniprintf(b, sizeof b, "COPY NOT PLACED: %s ASKS", j.b);
        fail_say(b);
        if (!s.stuck) job_trash(stage, "failed", "", 0, true);   // (the staged copy kept in the trash)
        if (s.stuck) return kDone;   // (the card refuses: the journal stays - the next boot completes it)
        s.ph = kPhClear;
        return kMore;
      }
      s.moving = false;
      if (j.rekey && !strcmp(j.a, project)) rekey_all(j.a, j.b);   // SAVE AS from the current project: its held edits
      const char* nm = j.name[0] ? j.name : name_of(j.a);
      if (nm[0]) projects::note(j.b, nm);
      if (j.mark) mark_renamed(j.mark);
      unsuspect(j.b);
      log(CORE_STR("prjslot: %s copied to %s (%lu files)"), j.a, j.b, (unsigned long)s.cdone);
      char b[48];
      sniprintf(b, sizeof b, "COPIED %s TO %s", j.a, j.b);
      say(b);
      s.ph = kPhScanS, s.i = 0, s.sub = 2;   // (the stage checked empty)
      return kMore;
    }
    case kPhFail: {   // the staged copy to the trash (why failed: never deleted), nothing under b
      s.moving = false;
      char b[48];
      sniprintf(b, sizeof b, "COPY FAILED: %s NOT COPIED", j.a);
      fail_say(b);
      Job* t = job_trash(stage, "failed", "", 0, true);
      (void)t;
      s.ph = kPhClear;
      return kMore;
    }
    case kPhClear: return journal("") == kWait ? kWait : kDone;
    default: return kDone;
  }
}

Res step_switch(Job& j, uint32_t ms) {
  (void)ms;
  switch (s.ph) {
    case kPhStart:
      if (j.reload || j.checked) { s.ph = kPhSwitch; return kMore; }
      s.ph = kPhScanA, s.i = 0;
      return kMore;
    case kPhScanA: {   // the slot's own files
      Res r = scan(j.b, &s.mask_a);
      if (r != kDone) return r;
      if (s.mask_a) {
        const char* was = name_of(j.b);
        bool sus = j.slot >= 1 && s.suspect[j.slot];   // (a trash / copy that could not follow: always asked)
        if (sus) take_renamed(j.slot);
        else if (take_renamed(j.slot) || !was[0] || !strcmp(was, j.name)) { s.ph = kPhSwitch; return kMore; }
        ask_mismatch(j, was);
        s.ph = kPhAsk;
        return kWait;
      }
      take_renamed(j.slot);
      project_key(j.name, j.a, sizeof j.a);   // the name's key from before slots
      if (!j.name[0] || is_slot_key(j.a) || declined(j.slot, j.a)) { s.ph = kPhSwitch; return kMore; }
      s.ph = kPhScanB, s.i = 0;
      return kMore;
    }
    case kPhScanB: {
      Res r = scan(j.a, &s.mask_b);
      if (r != kDone) return r;
      if (!s.mask_b) { s.ph = kPhSwitch; return kMore; }
      if (!s.card_slot) {   // the migration: the active project by slot + name, without a question
        log(CORE_STR("prjslot: migration: %s (slot %d's name) -> %s"), j.a, j.slot, j.b);
        Job sw = j;
        sw.checked = true;
        s.cur.k = kJNone;
        requeue_front(sw);
        job_two(kJMove, sw.a, sw.b, sw.name, 0, true);
        return kMore;
      }
      ask_legacy(j, j.a, bits(s.mask_b));
      s.ph = kPhAsk;
      return kWait;
    }
    case kPhAsk: return kWait;   // (answer() moves it on)
    case kPhSwitch: {
      project_switch_named(j.b, j.name, j.reload);
      host_project_changed();
      if (!s.card_slot) {
        s.card_slot = true, s.dirty = true;
        log(CORE_STR("prjslot: this card is keyed by slot from now on"));
      }
      return kDone;
    }
    default: return kDone;
  }
}

Res step_restore(Job& j, uint32_t ms) {
  (void)ms;
  switch (s.ph) {
    case kPhStart: {
      entry_dir(j.id, s.tmp, sizeof s.tmp);
      char p[cardq::kPathMax];
      sniprintf(p, sizeof p, "%s/info.txt", s.tmp);
      if (!req(cardq::kOpenReq, p, 0, nullptr, 0, &g_file)) return kWait;
      if (R.got != 1) { fail_say("RESTORE: NO SUCH ENTRY"); return kDone; }
      s.ph = kPhRead;
      return kMore;
    }
    case kPhRead: {
      if (!req(cardq::kReadReq, "", 0, (uint8_t*)s.info, sizeof s.info - 1, &g_file)) return kWait;
      int n = R.got > 0 ? R.got : 0;
      s.info[n] = 0;
      s.ph = kPhReadClose;
      return kMore;
    }
    case kPhReadClose: {
      if (!req(cardq::kCloseReq, "", 0, nullptr, 0, &g_file)) return kWait;
      char key[kKeyMax] = "", name[24] = "";
      for (char* l = s.info; *l;) {
        char* nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (!strncmp(l, "key=", 4)) sniprintf(key, sizeof key, "%s", l + 4);
        if (!strncmp(l, "name=", 5)) sniprintf(name, sizeof name, "%s", l + 5);
        if (!nl) break;
        l = nl + 1;
      }
      if (!j.b[0]) sniprintf(j.b, sizeof j.b, "%s", key);
      if (!j.name[0]) sniprintf(j.name, sizeof j.name, "%s", name);
      char k[kKeyMax];
      project_key(j.b, k, sizeof k);
      if (!j.b[0] || strcmp(k, j.b)) { fail_say("RESTORE: NOT A PROJECT KEY"); return kDone; }
      if (current_or_next(j.b)) { fail_say("RESTORE: THE RYTM'S CURRENT ONE: LOAD ANOTHER"); return kDone; }
      s.ph = kPhScanB, s.i = 0;
      return kMore;
    }
    case kPhScanB: {
      Res r = scan(j.b, &s.mask_b);
      if (r != kDone) return r;
      if (s.mask_b) {
        if (!j.replace) {
          char b[48];
          sniprintf(b, sizeof b, "RESTORE: %s HAS FILES", j.b);
          say(b);
          return kDone;
        }
        Job rs = j;   // the target's files to the trash first, then this restore again
        rs.replace = false;
        s.cur.k = kJNone;
        requeue_front(rs);
        job_trash(rs.b, "restore", name_of(rs.b), 0, true);
        return kMore;
      }
      s.ph = kPhEntryList;
      return kMore;
    }
    case kPhEntryList: {
      entry_dir(j.id, s.tmp, sizeof s.tmp);
      if (!req(cardq::kListReq, s.tmp, 0, (uint8_t*)g_list, kListMax)) return kWait;
      if (R.got < 0) { fail_say("RESTORE: THE ENTRY CANNOT BE READ"); return kDone; }
      s.nops = 0, s.done = 0, s.stuck = 0, s.failed = false, s.sub = 0;
      for (uint32_t p = 0; p < (uint32_t)R.got;) {
        const char* nm = g_list + p;
        p += (uint32_t)strlen(nm) + 1;
        char it[48];
        for (int i = 0; i < g_ncand && s.nops < kMaxCand; ++i) {   // only the items this build knows: the rest stay
          cand_item(i, it, sizeof it);
          if (strcmp(it, nm)) continue;
          sniprintf(s.src[s.nops], cardq::kPathMax, "%s/%s", s.tmp, nm);
          cand_path(i, j.b, s.dst[s.nops], cardq::kPathMax);
          ++s.nops;
        }
      }
      s.ph = kPhJournal;
      return kMore;
    }
    case kPhJournal: {
      char line[96];
      sniprintf(line, sizeof line, "restore\t%lu\t%s", (unsigned long)j.id, j.b);
      if (journal(line) == kWait) return kWait;
      s.ph = kPhOps, s.moving = true;
      return kMore;
    }
    case kPhOps: {
      Res r = renames(false);
      if (r != kDone) return r;
      s.moving = false;
      if (s.failed) {
        fail_say(s.stuck ? "RESTORE INCOMPLETE" : "RESTORE FAILED: NOTHING MOVED");
        s.ph = kPhClear;
        return kMore;
      }
      if (j.name[0]) projects::note(j.b, j.name);
      int sl = slot_of(j.b);
      if (sl > 0) mark_renamed(sl);   // (the slot's next name taken without a question: the user chose this)
      log(CORE_STR("trash: %06lu restored to %s (%d files / folders)"), (unsigned long)j.id, j.b, s.nops);
      char b[48];
      sniprintf(b, sizeof b, "RESTORED %s TO %s", j.name[0] ? j.name : "", j.b);
      say(b);
      entry_dir(j.id, s.tmp, sizeof s.tmp);
      s.ph = kPhRmInfo;
      return kMore;
    }
    case kPhRmInfo: {   // the entry's own info.txt, then its folder - not recursive: anything left keeps the entry
      char p[cardq::kPathMax];
      sniprintf(p, sizeof p, "%s/info.txt", s.tmp);
      if (!req(cardq::kRemoveReq, p)) return kWait;
      s.ph = kPhRmDir;
      return kMore;
    }
    case kPhRmDir:
      if (!req(cardq::kRemoveReq, s.tmp)) return kWait;
      if (R.got != 1) {   // items this build does not know: kept in the entry (with a fresh info.txt)
        log(CORE_STR("trash: %s keeps items this build does not restore"), s.tmp);
        s.ph = kPhInfo;
        return kMore;
      }
      s.ph = kPhClear;
      return kMore;
    case kPhInfo: {
      int n = sniprintf(s.info, sizeof s.info, "# items left after a restore (core/bundle/prjslot.h)\nkey=%s\nname=%s\nwhy=left\nslot=%d\ndate=-\n",
                        j.b, j.name, slot_of(j.b));
      char p[cardq::kPathMax];
      sniprintf(p, sizeof p, "%s/info.txt", s.tmp);
      if (!req(cardq::kWriteReq, p, 0, (uint8_t*)s.info, (uint32_t)n)) return kWait;
      s.ph = kPhClear;
      return kMore;
    }
    case kPhClear: return journal("") == kWait ? kWait : kDone;
    default: return kDone;
  }
}

Res step_remove(Job& j, uint32_t ms) {
  (void)ms;
  switch (s.ph) {
    case kPhStart:
      if (j.k == kJRemove) {
        entry_dir(j.id, s.tmp, sizeof s.tmp);
        log(CORE_STR("trash: %06lu removed for good (the web page)"), (unsigned long)j.id);
        rm_begin(s.tmp);
        s.ph = kPhRm, s.moving = true;
        return kMore;
      }
      s.ph = kPhList;
      return kMore;
    case kPhList: {   // emptying: the first entry left (not one that failed), again until none
      if (!req(cardq::kListReq, kTrash, 0, (uint8_t*)g_list, kListMax)) return kWait;
      for (uint32_t p = 0; R.got > 0 && p < (uint32_t)R.got;) {
        const char* nm = g_list + p;
        p += (uint32_t)strlen(nm) + 1;
        uint32_t v = num(nm);
        if (v == ~0u || is_bad(v)) continue;
        sniprintf(s.tmp, sizeof s.tmp, "%s/%s", kTrash, nm);
        s.tmin = v;
        rm_begin(s.tmp);
        s.ph = kPhRm, s.moving = true;
        return kMore;
      }
      s.moving = false;
      log(CORE_STR("trash: emptied (the web page)"));
      say("TRASH EMPTIED");
      return kDone;
    }
    case kPhRm: {
      Res r = rm_step();
      if (r != kDone) return r;
      s.moving = false;
      if (s.rm_failed) {
        fail_say("TRASH: AN ENTRY COULD NOT BE REMOVED");
        if (j.k == kJEmpty) {
          for (uint32_t& b : s.bad)
            if (!b) { b = s.tmin; break; }
          s.ph = kPhList;
          return is_bad(s.tmin) ? kMore : kDone;
        }
        return kDone;
      }
      if (j.k == kJEmpty) { s.ph = kPhList; return kMore; }
      say("TRASH ENTRY REMOVED");
      return kDone;
    }
    default: return kDone;
  }
}

// a journal line found at boot: the card operation it names completed forward (a copy: its staging to the trash and
// the copy again; the renames: each one whose source is there and target is not)
Res step_recover(Job& j, uint32_t ms) {
  (void)ms;
  char kind[12] = "", a[kKeyMax + 2] = "", b[kKeyMax + 2] = "", c[kKeyMax + 2] = "";
  {   // "kind\ta\tb": a tab, which no key holds (a name key keeps spaces)
    const char* p = j.line;
    int k = 0;
    while (*p && *p != '\t' && k < 11) kind[k++] = *p++;
    kind[k] = 0;
    if (*p == '\t') ++p;
    k = 0;
    while (*p && *p != '\t' && k < kKeyMax + 1) a[k++] = *p++;
    a[k] = 0;
    if (*p == '\t') ++p;
    k = 0;
    while (*p && *p != '\t' && k < kKeyMax + 1) b[k++] = *p++;
    b[k] = 0;
    if (*p == '\t') ++p;
    k = 0;
    while (*p && *p != '\t' && k < kKeyMax + 1) c[k++] = *p++;
    c[k] = 0;
  }
  switch (s.ph) {
    case kPhStart: {
      log(CORE_STR("prjslot: recovery after a cut: %s"), j.line);
      s.nops = 0, s.done = 0, s.stuck = 0, s.failed = false, s.sub = 0;
      if (!strcmp(kind, "copy")) {   // the staged half copy to the trash, the copy again
        char stage[kKeyMax + 2];
        stage_of(b, stage, sizeof stage);
        s.cur.k = kJNone;
        if (!strcmp(b, project) && slot_of(b) >= 0)   // (the Sidecar booted on it: loaded again once it is whole)
          if (Job* w = push(kJSwitch, true)) w->slot = slot_of(b), w->reload = true, sniprintf(w->b, sizeof w->b, "%s", b),
                                             sniprintf(w->name, sizeof w->name, "%s", project_name);
        job_two(kJCopy, a, b, name_of(a), slot_of(b), true);
        job_trash(stage, "failed", "", 0, true);
        s.jline[0] = 0, s.dirty = true;
        return kMore;
      }
      if (!strcmp(kind, "move") || !strcmp(kind, "place")) {
        char from[kKeyMax + 2];
        if (!strcmp(kind, "place")) stage_of(b, from, sizeof from);
        else sniprintf(from, sizeof from, "%s", a);
        for (int i = 0; i < g_ncand; ++i)
          cand_path(i, from, s.src[s.nops], cardq::kPathMax), cand_path(i, b, s.dst[s.nops], cardq::kPathMax), ++s.nops;
      } else if (!strcmp(kind, "trash")) {
        uint32_t id = num(b);
        if (id == ~0u) { s.ph = kPhClear; return kMore; }
        if (id + 1 > s.next) s.next = id + 1, s.dirty = true;
        entry_dir(id, s.tmp, sizeof s.tmp);
        for (int i = 0; i < g_ncand; ++i) {
          char it[48];
          cand_item(i, it, sizeof it);
          cand_path(i, a, s.src[s.nops], cardq::kPathMax);
          sniprintf(s.dst[s.nops], cardq::kPathMax, "%s/%s", s.tmp, it);
          ++s.nops;
        }
      } else if (!strcmp(kind, "restore")) {
        uint32_t id = num(a);
        if (id == ~0u) { s.ph = kPhClear; return kMore; }
        entry_dir(id, s.tmp, sizeof s.tmp);
        for (int i = 0; i < g_ncand; ++i) {
          char it[48];
          cand_item(i, it, sizeof it);
          sniprintf(s.src[s.nops], cardq::kPathMax, "%s/%s", s.tmp, it);
          cand_path(i, b, s.dst[s.nops], cardq::kPathMax);
          ++s.nops;
        }
      }
      s.ph = kPhOps, s.moving = true;
      return kMore;
    }
    case kPhOps: {
      Res r = renames(true);
      if (r != kDone) return r;
      s.moving = false;
      if (s.failed) {   // the card still refuses: the journal kept, tried again at the next boot
        fail_say("RECOVERY STOPPED: THE CARD REFUSED");
        return kDone;
      }
      if (!strcmp(kind, "move")) projects::rekey(a, b);
      if (!strcmp(kind, "trash")) drop_index(a);
      say("RECOVERED AFTER A CUT");
      if (!strcmp(kind, "trash") && c[0]) {   // a SAVE TO / PASTE's trash: its copy next (journaled by the copy)
        if (Job* cp = job_two(kJCopy, c, a, name_of(c), slot_of(a), true)) (void)cp;
        s.jline[0] = 0, s.dirty = true;
      }
      // the project the Sidecar booted on was moved / placed / trashed / restored under it: loaded again (its RAM state
      // was filled from the card before the recovery)
      if (!strcmp(kind, "move") && !strcmp(a, project)) project_switch_named(b, project_name, true);
      else if (!strcmp(a, project) || !strcmp(b, project)) project_switch_named(project, project_name, true);
      if (!strcmp(kind, "restore")) { s.ph = kPhEntryList; return kMore; }
      s.ph = kPhClear;
      return kMore;
    }
    case kPhEntryList: {   // a restore: the entry removed only when nothing but its info.txt is left (else kept whole)
      if (!req(cardq::kListReq, s.tmp, 0, (uint8_t*)g_list, kListMax)) return kWait;
      bool only_info = R.got > 0 && !strcmp(g_list, "info.txt") && (uint32_t)R.got == 9;
      if (R.got == 0 || only_info) { s.ph = only_info ? kPhRmInfo : kPhRmDir; return kMore; }
      log(CORE_STR("trash: %s keeps items after the recovered restore"), s.tmp);
      s.ph = kPhClear;
      return kMore;
    }
    case kPhRmInfo: {
      char p[cardq::kPathMax];
      sniprintf(p, sizeof p, "%s/info.txt", s.tmp);
      if (!req(cardq::kRemoveReq, p)) return kWait;
      s.ph = kPhRmDir;
      return kMore;
    }
    case kPhRmDir:
      if (!req(cardq::kRemoveReq, s.tmp)) return kWait;
      s.ph = kPhClear;
      return kMore;
    case kPhClear: return journal("") == kWait ? kWait : kDone;
    default: return kDone;
  }
}

// ---- the events -------------------------------------------------------------------------------------------------------
void pend_resolve(bool reload, int slot, const char* name) {
  if (!s.pend.kind || s.pend.slot != slot) return;
  char k[kKeyMax], src[kKeyMax];
  slot_key(slot, k, sizeof k);
  if (s.pend.kind == 2 && s.pend.src > 0) {   // (as on_event's PASTE: suspect until placed, the trash + copy one journaled operation)
    slot_key(s.pend.src, src, sizeof src);
    suspect(k);
    if (Job* t = job_trash(k, s.pend.why, name_of(k))) sniprintf(t->pair, sizeof t->pair, "%s", src);
    job_two(kJCopy, src, k, name_of(src), slot);
  }
  else job_trash(k, s.pend.why, name_of(k));
  log(CORE_STR("prjslot: slot %d %s: its pending %s resolved"), slot, reload ? "reloaded" : "left", s.pend.why);
  s.pend.kind = 0;
  if (reload) job_switch(slot, name, true);
}
void on_active(int slot, const char* name) {
  int prev = s.rytm_slot;
  if (prev >= 0 && prev != slot) kits::project_changing();   // (kit reports waiting are of the slot before)
  s.rytm_slot = slot;
  sniprintf(s.rytm_name, sizeof s.rytm_name, "%s", name);
  char key[kKeyMax];
  slot_key(slot, key, sizeof key);
  Job* w = last_switch();
  const char* target = w ? w->b : project;
  const char* tname = w ? w->name : project_name;
  if (!strcmp(key, target)) {
    if (!strcmp(name, tname)) {   // (the Rytm's repeat; the boot's first: its tables announced, as a 10's)
      if (!w && !project_seen) project_switch_named(key, name, false), host_project_changed();
      return;
    }
    if (s.pend.kind && s.pend.slot == slot) { pend_resolve(true, slot, name); return; }   // reloaded from the card
    if (w && w != &s.cur) { sniprintf(w->name, sizeof w->name, "%s", name); return; }    // (renamed meanwhile)
    if (w) { job_switch(slot, name, false); return; }
    log(CORE_STR("prjslot: slot %d renamed %s -> %s"), slot, project_name, name);
    take_renamed(slot);
    project_switch_named(key, name, false);   // (the same key: the name and the index only)
    return;
  }
  if (slot == 0) job_trash(key, "new", name_of(key));   // (a new project starts empty: the no-slot one's leftovers go)
  job_switch(slot, name, false);
  if (prev >= 0 && prev != slot) pend_resolve(false, prev, "");
}
void on_event(int op, int a, int b) {
  char ka[kKeyMax], kb[kKeyMax];
  slot_key(a, ka, sizeof ka), slot_key(b, kb, sizeof kb);
  bool active = b > 0 && b == s.rytm_slot;
  switch (op) {
    case 1:   // SAVE a -> b
      if (b <= 0) return;
      if (a == b) {
        if (s.pend.kind && s.pend.slot == a) {
          log(CORE_STR("prjslot: slot %d saved: its pending %s cancelled (the Rytm's project is on the card again)"), a, s.pend.why);
          s.pend.kind = 0;
          say("KEPT: SAVED AGAIN");
        }
        return;
      }
      if (s.pend.kind && s.pend.slot == b) s.pend.kind = 0;   // (overwritten: its files go to the trash now)
      suspect(kb);   // (persisted until the copy is placed: a failure on the way asks at b's switch)
      if (Job* t = job_trash(kb, "saveto", name_of(kb)))
        if (a > 0) sniprintf(t->pair, sizeof t->pair, "%s", ka);   // (one journaled operation: trash, then copy)
      if (Job* j = job_two(a > 0 ? kJCopy : kJMove, ka, kb, a > 0 ? name_of(ka) : project_name, b))
        j->rekey = a == s.rytm_slot;   // (the edits held during the copy are b's: the Rytm is on b once it ends)
      return;
    case 2:   // PASTE a -> b
      if (b <= 0) return;
      if (active) {
        s.pend.kind = a > 0 ? 2 : 1, s.pend.slot = b, s.pend.src = a;
        sniprintf(s.pend.why, sizeof s.pend.why, "paste");
        log(CORE_STR("prjslot: slot %d (the active one) pasted over: its files go when it is left or reloaded"), b);
        return;
      }
      if (a > 0) {
        suspect(kb);
        if (Job* t = job_trash(kb, "paste", name_of(kb))) sniprintf(t->pair, sizeof t->pair, "%s", ka);
        job_two(kJCopy, ka, kb, name_of(ka), b);
      }
      else job_trash(kb, "paste", name_of(kb), b), log(CORE_STR("prjslot: slot %d pasted from an unknown slot: nothing copied"), b);
      return;
    case 3: case 4:   // CLEAR / DELETE b
      if (b <= 0) return;
      if (active) {
        s.pend.kind = 1, s.pend.slot = b, s.pend.src = 0;
        sniprintf(s.pend.why, sizeof s.pend.why, "%s", op == 3 ? "clear" : "delete");
        log(CORE_STR("prjslot: slot %d (the active one) %s: its files go when it is left or reloaded, a SAVE keeps them"),
            b, s.pend.why);
        return;
      }
      job_trash(kb, op == 3 ? "clear" : "delete", name_of(kb), op == 3 ? b : 0);
      return;
    case 5:   // RENAME b
      if (b > 0 && !active) mark_renamed(b);
      return;
    case 6:
      log(CORE_STR("prjslot: the Rytm dropped %d project events: resync from its slot list"), a);
      s.lost = true;
      kits::lost(a);
      return;
    case 7: log(CORE_STR("prjslot: slot %d written by a path not followed (Transfer, SysEx)"), b); return;
    case 10:   // LOAD b (the active-slot setter): a load of the active slot again resolves its pending clear / paste now
      if (b > 0 && b != s.rytm_slot) kits::project_changing();   // (another slot: kit reports waiting are stale)
      if (b > 0 && b == s.rytm_slot && s.pend.kind && s.pend.slot == b) pend_resolve(true, b, s.rytm_name);
      return;
    default: return;
  }
}
// the Rytm's slot list, whole (4B 09 after the 08s): slots the index knows that are empty now go to the trash (deleted
// while the Sidecar was away, or events lost: at most kGoneMax a list); name keys from before slots whose name is in
// exactly one slot move there (the migration; several: asked when one is opened)
void on_list() {
  int gone = 0, moved = 0;
  for (int i = 0; i < projects::count(); ++i) {
    const projects::Info* e = projects::at(i);
    if (!e) continue;
    int sl = slot_of(e->key);
    if (sl >= 1) {
      if (s.list_occ[sl] || sl == s.rytm_slot || current_or_next(e->key)) continue;
      if (++gone > kGoneMax) continue;
      job_trash(e->key, "gone", e->rytm);
      continue;
    }
    if (sl == 0 || !e->key[0]) continue;   // (the no-slot project; a name key below)
    int found = 0, at = 0;
    for (int k = 1; k <= kSlots; ++k) {
      if (!s.list_occ[k]) continue;
      char nk[kKeyMax];
      project_key(s.list_name[k], nk, sizeof nk);
      if (!strcmp(nk, e->key)) ++found, at = k;
    }
    if (found != 1 || declined(at, e->key)) continue;
    char sk[kKeyMax];
    slot_key(at, sk, sizeof sk);
    if (current_or_next(e->key) && at != s.rytm_slot) continue;   // (the Sidecar is on it: the switch takes it)
    job_two(kJMove, e->key, sk, s.list_name[at]);
    ++moved;
  }
  if (gone > kGoneMax)
    fail_say("SLOT LIST: TOO MANY GONE"), log(CORE_STR("prjslot: the slot list has %d known slots empty: %d trashed"), gone, kGoneMax);
  if (moved) log(CORE_STR("prjslot: migration from the slot list: %d projects from before slots to their slot"), moved);
  s.lost = false;
}

void apply_held() {
  if (!s.held[0]) return;
  uint8_t m[26];
  int n = 0;
  for (; s.held[n] && n < 23; ++n) m[n] = (uint8_t)s.held[n];
  m[n] = 0xF7;
  s.held[0] = 0;
  set_project(m, n + 1);
}
void enter_slot_mode() {
  if (s.mode != kSlot) {
    log(CORE_STR("prjslot: the Rytm reports project slots (rui1052): slot mode"));
    rename::back();   // (a name question from the 10s before is moot now)
  }
  s.mode = kSlot, s.held[0] = 0;
}

void drain_web() {
  while (ld(&g_wr_r) != ld(&g_wr_w)) {
    const WebReq& r = g_wr[g_wr_r % kWebReqs];
    Job* j = nullptr;
    if (r.op == 'T') {
      job_trash(r.key, r.why, r.name);
    } else if (r.op == 'R' && (j = push(kJRestore))) {
      j->id = r.id, j->replace = r.replace;
      sniprintf(j->b, sizeof j->b, "%s", r.key);
    } else if (r.op == 'X' && (j = push(kJRemove))) {
      j->id = r.id;
    } else if (r.op == 'E') {
      push(kJEmpty);
    }
    sto(&g_wr_r, g_wr_r + 1);
  }
}
bool web(char op, uint32_t id, const char* key, const char* name, const char* why, bool replace) {
  uint32_t w = ld(&g_wr_w);
  if (w - ld(&g_wr_r) >= kWebReqs) return false;
  WebReq& r = g_wr[w % kWebReqs];
  memset(&r, 0, sizeof r);
  r.op = op, r.id = id, r.replace = replace;
  sniprintf(r.key, sizeof r.key, "%s", key ? key : "");
  sniprintf(r.name, sizeof r.name, "%s", name ? name : "");
  sniprintf(r.why, sizeof r.why, "%s", why ? why : "");
  sto(&g_wr_w, w + 1);
  return true;
}

}  // namespace

void answer2(bool yes, bool remember);

// ---- the interface ----------------------------------------------------------------------------------------------------
CORE_COLD void load() {
  cands();
  s.rytm_slot = -1;
  if (!g_list) g_list = new char[kListMax];
  s.card_slot = false, s.next = 1, s.jline[0] = 0;
  memset(s.renamed, 0, sizeof s.renamed);
  memset(s.declined, 0, sizeof s.declined);
  memset(s.suspect, 0, sizeof s.suspect);
  fmt::recover(kFile);
  File f;
  if (!f.open(kFile, kRead)) return;
  static char b[2048];
  int n = f.read(b, sizeof b - 1);
  f.close();
  b[n > 0 ? n : 0] = 0;
  char job[96] = "";
  for (char* l = b; *l;) {
    char* nl = strchr(l, '\n');
    if (nl) *nl = 0;
    if (!strcmp(l, "mode=slot")) s.card_slot = true;
    else if (!strncmp(l, "trash_next=", 11)) s.next = (uint32_t)atoi(l + 11) > 0 ? (uint32_t)atoi(l + 11) : 1;
    else if (!strncmp(l, "job=", 4)) sniprintf(job, sizeof job, "%s", l + 4);
    else if (!strncmp(l, "renamed=", 8)) mark_renamed(atoi(l + 8));
    else if (!strncmp(l, "suspect=", 8)) { int sl = atoi(l + 8); if (sl >= 1 && sl <= kSlots) s.suspect[sl] = 1; }
    else if (!strncmp(l, "declined=", 9)) {
      char* sp = strchr(l + 9, ' ');
      if (sp) decline(atoi(l + 9), sp + 1);
    }
    if (!nl) break;
    l = nl + 1;
  }
  s.dirty = false;
  if (job[0]) {   // a card operation a cut stopped: completed forward first, before anything else
    sniprintf(s.jline, sizeof s.jline, "%s", job);
    if (Job* j = push(kJRecover, true)) sniprintf(j->line, sizeof j->line, "%s", job);
  }
  log(CORE_STR("prjslot: card keyed by %s, trash next %lu%s%s"), s.card_slot ? "slot" : "name", (unsigned long)s.next,
      job[0] ? ", recovering: " : "", job);
}

CORE_COLD void hello(int caps) {
  s.list_seen = 0, s.list_prev = 0;   // (a connect: lists from before it count for nothing)
  if (caps & 4) {
    enter_slot_mode();
    return;
  }
  if (s.mode != kName) log(CORE_STR("prjslot: the Rytm does not report project slots: name mode"));
  s.mode = kName;
  apply_held();
}

CORE_COLD bool take_10(const char* n) {
  if (s.mode == kSlot) return true;
  if (s.mode == kName || !s.card_slot) return false;
  if (!s.held[0]) s.held_ms = now_ms();
  sniprintf(s.held, sizeof s.held, "%s", n);
  return true;
}

CORE_COLD bool resolve(const char* n, char* key, int cap) {
  if (!s.card_slot || s.mode == kSlot) return false;
  char nk[kKeyMax];
  project_key(n, nk, sizeof nk);
  if (projects::find(nk)) return false;   // the name's own key is known: as today
  const projects::Info* best = nullptr;
  for (int i = 0; i < projects::count(); ++i) {
    const projects::Info* e = projects::at(i);
    if (e && is_slot_key(e->key) && !strcmp(e->rytm, n) && (!best || e->used > best->used)) best = e;
  }
  if (!best) return false;
  sniprintf(key, cap, "%s", best->key);
  return true;
}

CORE_COLD void sysex(const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1) return;
  enter_slot_mode();
  int op = d[6];
  if (op == 0) {
    int slot = d[7] << 7 | d[8];
    char nm[24];
    int k = 0;
    for (int i = 9; i < len && k < 23 && d[i] != 0xF7 && d[i]; ++i) nm[k++] = (char)(d[i] & 0x7F);
    nm[k] = 0;
    if (slot > kSlots) return;
    on_active(slot, nm);
    return;
  }
  if (op == 8) {   // LIST: <first hi lo> <n> {<occ> <name 15>} x n
    if (len < 11) return;
    int first = d[7] << 7 | d[8], n = d[9];
    if (first == 1) s.list_seen = 0, memset(s.list_occ, 0, sizeof s.list_occ);   // (a list starts: nothing from before)
    if (n == 8 && first >= 1 && first <= kSlots && (first - 1) % 8 == 0 && 10 + 16 * n < len) s.list_seen |= (uint16_t)(1u << ((first - 1) / 8));
    for (int i = 0; i < n && 10 + 16 * i + 16 < len; ++i) {
      int sl = first + i;
      if (sl < 1 || sl > kSlots) continue;
      const uint8_t* e = d + 10 + 16 * i;
      s.list_occ[sl] = e[0] ? 1 : 0;
      int k = 0;
      for (; k < 15 && e[1 + k] && e[1 + k] != 0xF7; ++k) s.list_name[sl][k] = (char)(e[1 + k] & 0x7F);
      s.list_name[sl][k] = 0;
    }
    return;
  }
  if (len < 12) return;
  int a = d[7] << 7 | d[8], b = d[9] << 7 | d[10];
  if (op == 9) {   // LIST END: <count> (0x3fff: no list). Acted on only when whole, plausible and seen twice the same
    if (a == 0x3FFF) { log(CORE_STR("prjslot: the Rytm could not list its slots")); return; }
    int occ = 0;
    uint32_t h = 2166136261u;
    for (int k = 1; k <= kSlots; ++k) {
      occ += s.list_occ[k];
      h = (h ^ (uint32_t)(s.list_occ[k] + 1)) * 16777619u;
      for (const char* c = s.list_occ[k] ? s.list_name[k] : ""; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
    }
    bool whole = s.list_seen == 0xFFFF, counted = occ == a && a > 0;
    bool active_ok = s.rytm_slot <= 0 || s.list_occ[s.rytm_slot];
    s.list_seen = 0;
    if (!whole || !counted || !active_ok) {
      log(CORE_STR("prjslot: a slot list not used (%s)"), !whole ? "messages missing" : !counted ? "its count does not match"
                                                                                        : "the active slot empty in it");
      s.list_prev = 0;
      return;
    }
    if (!h) h = 1;
    if (h != s.list_prev) { s.list_prev = h; return; }   // (the first of two: the next one, the same, acts)
    on_list();
    return;
  }
  if (op == 6) { on_event(op, a, b); return; }
  if (a > kSlots || b > kSlots) return;
  on_event(op, a, b);
}

CORE_COLD void tick(uint32_t ms) {
  if (!g_ncand) cands();
  if (!g_list) g_list = new char[kListMax];
  if (!s.next) s.next = 1;   // (no load() yet: entries from 000001)
  drain_web();
  if (s.mode == kUndecided && s.held[0] && ms - s.held_ms >= kHoldMs) {
    log(CORE_STR("prjslot: no hello, no slot report: name mode"));
    s.mode = kName;
    apply_held();
  }
  if (s.ask) {
    if (ms - s.ask_ms < kAskMs) return;
    log(CORE_STR("prjslot: no answer: %s"), s.ask == kAskMismatch ? "the files kept" : "nothing moved");
    answer2(s.ask == kAskMismatch, false);   // (no answer is not a NO to remember: asked again next time)
    return;
  }
  if (rename::busy()) return;   // (the name question's looks / moves first: never two movers at once)
  for (int guard = 0; guard < 256; ++guard) {
    if (!s.cur.k) {
      if (!s.qn) {
        save_file();
        return;
      }
      if (__atomic_load_n(&g_hold, __ATOMIC_SEQ_CST)) return;   // (an import commits: its trash entry, its slot)
      s.cur = s.q[s.qh], s.qh = (s.qh + 1) % kQueue, --s.qn;
      s.ph = 0, s.i = 0, s.moving = false, s.rmd = 0, s.qst = 0, s.js = 0, s.sub = 0, s.skip_songs = false;
    }
    Job& j = s.cur;
    uint8_t k = j.k;
    Res r;
    switch (k) {
      case kJTrash: r = step_trash(j, ms); break;
      case kJMove: r = step_move(j, ms); break;
      case kJCopy: r = step_copy(j, ms); break;
      case kJSwitch: r = step_switch(j, ms); break;
      case kJRestore: r = step_restore(j, ms); break;
      case kJRemove: case kJEmpty: r = step_remove(j, ms); break;
      case kJRecover: r = step_recover(j, ms); break;
      default: r = kDone; break;
    }
    if (!s.cur.k) continue;   // (the step put other jobs in front and itself behind them)
    if (r == kWait) return;
    if (r == kDone) s.cur.k = kJNone, s.moving = false, __atomic_store_n(&s.jobs, s.jobs + 1, __ATOMIC_RELEASE);
  }
}

CORE_COLD void answer(bool yes) { answer2(yes, true); }
CORE_COLD void answer2(bool yes, bool remember) {
  if (!s.ask || s.cur.k != kJSwitch) { s.ask = kNoAsk; return; }
  Job sw = s.cur;
  sw.checked = true;
  uint8_t kind = s.ask;
  s.ask = kNoAsk;
  s.cur.k = kJNone;
  // the switch again (checked), in front; what the answer moves goes in front of it
  requeue_front(sw);
  if (sw.slot >= 1) s.suspect[sw.slot] = 0;
  if (kind == kAskMismatch) {
    if (yes) {
      log(CORE_STR("prjslot: slot %d's files kept for %s"), sw.slot, sw.name);
      say("KEPT ITS SIDECAR FILES");
    } else {
      job_trash(sw.b, "mismatch", name_of(sw.b), 0, true);
    }
  } else {
    if (yes) {
      job_two(kJMove, s.ask_key, sw.b, sw.name, 0, true);
    } else {
      if (remember) decline(sw.slot, s.ask_key);
      log(CORE_STR("prjslot: %s's files not taken for slot %d"), s.ask_key, sw.slot);
      say("NOT TAKEN: A NEW PROJECT");
    }
  }
  tick(now_ms());
}

CORE_COLD bool moving() { return s.moving; }
CORE_COLD bool busy() { return s.cur.k || s.qn || s.ask || ld(&g_wr_r) != ld(&g_wr_w); }
CORE_COLD int mode() { return s.mode; }
CORE_COLD bool migrated() { return s.card_slot; }
CORE_COLD int ask() { return s.ask; }
CORE_COLD const char* ask_line(int i) { return i >= 0 && i < 5 && s.ask ? s.ask_l[i] : ""; }
CORE_COLD const char* ask_footer() {
  return s.ask == kAskMismatch ? "YES: KEEP  NO: TRASH" : s.ask == kAskLegacy ? "YES: TAKE  NO: NOT THIS" : "";
}
CORE_COLD uint32_t asks() { return s.asks; }
CORE_COLD const char* last() { return s.last; }
CORE_COLD uint32_t trash_next() { return s.next; }
CORE_COLD uint32_t failures() { return s.failures; }
CORE_COLD uint32_t jobs_done() { return __atomic_load_n(&s.jobs, __ATOMIC_ACQUIRE); }
CORE_COLD const char* journal_line() { return s.jline; }

CORE_COLD int rytm_slot(int slot, char* name, int cap, bool* occupied) {
  name[0] = 0, *occupied = false;
  if (slot < 1 || slot > kSlots) return 0;
  if (slot == s.rytm_slot) {   // (the active one: its ACTIVE report is the newest)
    sniprintf(name, cap, "%s", s.rytm_name), *occupied = true;
    return 1;
  }
  if (!s.list_prev) return 0;
  *occupied = s.list_occ[slot] != 0;
  if (*occupied) sniprintf(name, cap, "%.15s", s.list_name[slot]);
  return 1;
}
CORE_COLD bool trash_item(int i, const char* key, uint32_t id, char* from, char* to, int cap) {
  cands();
  if (i < 0 || i >= g_ncand) return false;
  char it[48], e[40];
  cand_item(i, it, sizeof it);
  entry_dir(id, e, sizeof e);
  cand_path(i, key, from, cap);
  sniprintf(to, cap, "%s/%s", e, it);
  return true;
}
CORE_COLD int trash_info(const char* key, const char* name, const char* why, char* out, int cap) {
  char date[projects::kDate] = "-", nm[24];
  uint32_t ep;
  int src;
  if (projects::clock_now(&ep, &src)) projects::iso_date(ep, date, sizeof date);
  clean(name, nm, sizeof nm);
  return sniprintf(out, cap, "# a project's Sidecar files, moved here (core/bundle/prjslot.h)\nkey=%s\nname=%s\nwhy=%s\nslot=%d\ndate=%s\n",
                   key, nm, why, slot_of(key), date);
}

CORE_COLD void hold_jobs(bool on) { __atomic_store_n(&g_hold, on ? 1u : 0u, __ATOMIC_SEQ_CST); }

CORE_COLD bool request_trash(const char* key, const char* why, const char* name) { return web('T', 0, key, name, why, false); }
CORE_COLD bool request_restore(uint32_t id, const char* to, bool replace) { return web('R', id, to, "", "", replace); }
CORE_COLD bool request_remove(uint32_t id) { return web('X', id, "", "", "", false); }
CORE_COLD bool request_empty() { return web('E', 0, "", "", "", false); }

CORE_COLD int trash_rows(TrashRow* e, int max) {
  int n = 0;
  File d, f;
  if (d.open(kTrash, kRead) && d.isDir()) {
    char nm[64];
    while (n < max && f.openNext(&d, kRead)) {
      f.getName(nm, sizeof nm);
      bool isd = f.isDir();
      f.close();
      uint32_t id = num(nm);
      if (!isd || id == ~0u) continue;
      TrashRow& x = e[n++];
      memset(&x, 0, sizeof x);
      x.id = id;
      char p[96], b[640];
      sniprintf(p, sizeof p, "%s/%s/info.txt", kTrash, nm);
      File in;
      int k = 0;
      if (in.open(p, kRead)) k = in.read(b, sizeof b - 1), in.close();
      b[k > 0 ? k : 0] = 0;
      for (char* l = b; *l;) {
        char* nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (!strncmp(l, "key=", 4)) sniprintf(x.key, sizeof x.key, "%s", l + 4);
        else if (!strncmp(l, "name=", 5)) sniprintf(x.name, sizeof x.name, "%s", l + 5);
        else if (!strncmp(l, "why=", 4)) sniprintf(x.why, sizeof x.why, "%s", l + 4);
        else if (!strncmp(l, "date=", 5) && strcmp(l + 5, "-")) sniprintf(x.date, sizeof x.date, "%s", l + 5);
        if (!nl) break;
        l = nl + 1;
      }
      File sub, g;
      sniprintf(p, sizeof p, "%s/%s", kTrash, nm);
      if (sub.open(p, kRead) && sub.isDir())
        while (g.openNext(&sub, kRead)) {
          g.getName(b, 64);
          g.close();
          if (strcmp(b, "info.txt")) ++x.items;
        }
      sub.close();
    }
    d.close();
  }
  for (int i = 1; i < n; ++i)   // the newest first
    for (int k = i; k > 0 && e[k].id > e[k - 1].id; --k) {
      TrashRow t = e[k];
      e[k] = e[k - 1], e[k - 1] = t;
    }
  return n;
}

CORE_COLD int trash_json(char* out, int cap) {
  static TrashRow e[kTrashMax + 16];
  int n = trash_rows(e, kTrashMax + 16);
  bundle::JOut o;
  o.begin(out, (uint32_t)cap);
  o.raw("[");
  for (int i = 0; i < n; ++i) {
    if (i) o.raw(",");
    o.raw("{\"id\":"), o.num(e[i].id), o.raw(",\"key\":"), o.str(e[i].key), o.raw(",\"name\":"), o.str(e[i].name);
    o.raw(",\"why\":"), o.str(e[i].why), o.raw(",\"date\":"), o.str(e[i].date), o.raw(",\"items\":"), o.num((uint64_t)e[i].items);
    o.raw("}");
  }
  o.raw("]");
  return o.over ? 0 : (int)o.n;
}

CORE_COLD int status_json(char* out, int cap) {
  bundle::JOut o;
  o.begin(out, (uint32_t)cap);
  o.raw("{\"mode\":"), o.str(s.mode == kSlot ? "slot" : s.mode == kName ? "name" : "undecided");
  o.raw(",\"migrated\":"), o.raw(s.card_slot ? "true" : "false");
  o.raw(",\"active\":"), o.str(project), o.raw(",\"name\":"), o.str(project_name);
  o.raw(",\"busy\":"), o.raw(busy() ? "true" : "false"), o.raw(",\"ask\":"), o.num((uint64_t)s.ask);
  o.raw(",\"last\":"), o.str(s.last), o.raw(",\"failures\":"), o.num(s.failures);
  o.raw(",\"journal\":"), o.str(s.jline), o.raw(",\"trash_next\":"), o.num(s.next), o.raw("}");
  return o.over ? 0 : (int)o.n;
}

CORE_COLD void reset() {
  char* l = g_list;
  memset(&s, 0, sizeof s);
  s.rytm_slot = -1, s.next = 1;
  g_list = l;
  memset(&R, 0, sizeof R);
  g_wr_w = g_wr_r = 0;
  g_hold = 0;
}

}  // namespace prjslot
}  // namespace core
#endif
