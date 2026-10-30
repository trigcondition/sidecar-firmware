// core/lib/pools.cpp - see pools.h. The control core's (core 3 on the Pi): every card operation is posted to the card
// core (cardq: kOpenReq / kReadReq / kCloseReq for a read, kAtomicReq for a write) and taken back at a later step, so
// a pass never waits on the card. One card job at a time (a list's read or write, the used set's, an .sfz's read).
#include "../platform.h"

#if CORE_MSAMP
#include "pools.h"
#include "../bundle/prjslot.h"

#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include "../fmt/fmt.h"
#include "../msamp/cardq.h"
#include "../msamp/sfz.h"
#include "../granular/granular.h"
#include "../msamp/msamp.h"
#include "../pool/pcache.h"
#include "../pool/pool.h"
#include "../voices.h"
#include "../wavetable/wavetable.h"
#include "../project_key.h"
#include "pools_fmt.h"
#include "srclib.h"

namespace core {
namespace pools {

const char* const kUnusedWarning = "NOT SEEN PLAYING - A RYTM LOCK MAY STILL USE IT";

namespace {

constexpr uint32_t kSaveDelayMs = 200;     // a list written this long after its last edit (edits coalesce)
constexpr uint32_t kUsedDelayMs = 1000;    // the used set a second after a change
constexpr int kResolvePer = 128;           // refs resolved a step
constexpr uint32_t kPieceRead = 65536;     // a read request's bytes (cardq::kPiece)
constexpr uint32_t kSfzMax = 1u << 20;     // an .sfz's bytes at most
constexpr int kSfzRegions = 4096;
constexpr int kVoicesMax = 8;

struct Lst {
  Entry* e;              // [kCodes]
  Member* m;             // [kMembers] (the instruments list; others none)
  int nm;                // members used (an instrument's are contiguous; unloads leave holes until compacted)
  volatile uint32_t seq; // odd while an entry changes (refs() reads under it)
  volatile uint32_t sig;
  bool locked, ro, dirty, from_template;
  bool ram_all;          // "always in RAM": the whole list (pool-used.txt "ram K *")
  bool info_dirty;       // only resolution's changes (a hash filled in, a moved hint): written when the Rytm stops
  uint8_t fails;         // writes failed in a row (given up after kMaxFails: kept in RAM)
  uint32_t dirty_ms;
  char why[160];
  // resolution
  uint32_t rgen;         // lib::gen() of the last full pass (~0u: start one)
  int rpos;              // the next ref of the pass (code * kMembersPer... flattened: entries then members)
  bool rrun;
  // one-level undo
  bool undo_ok;
  int undo_code;
  Entry undo_e;
  Member* undo_m;        // [kMembersPer]
  int16_t voice[kVoicesMax];   // the code each voice plays now (-1 none)
};
Lst L[kLists];
struct UsedSet {
  uint32_t* n;           // [kLists * kCodes]
  bool dirty;
  uint32_t dirty_ms;
} U = {nullptr, false, 0};

enum Io : uint8_t { kIoIdle, kIoOpen, kIoExists, kIoRead, kIoClose, kIoParse, kIoFormat, kIoWrite };
constexpr int kParseLines = 64;   // lines parsed a step (the control core renders between steps)
constexpr int kFormatRows = 64;   // rows formatted a step
constexpr int kFillItems = 16;    // a fill's files a step
constexpr int kMaxFails = 3;
enum What : uint8_t { kWList = 0, kWUsed, kWSfz };
struct Job {
  Io io;
  What what;
  int list;
  bool prev;             // reading <path>.prev (a cut between write_atomic's renames)
  cardq::Req rq;
  File f;
  uint32_t got;
  char path[200];
  bool too_big, read_err;
  // a list's parse / format over several steps
  uint32_t pos, body;    // the parse's next byte; the text's body
  text::Parser ps;
  int fc;                // the format's next code
  uint8_t fst;           // the load's fmt verdict (fmt::Status)
};
Job J = {};
char* text = nullptr;    // [kFileMax + 1] one card job's bytes (a read, or a write's text until it is done)
// the project the lists are of, and the load's progress
char cur_key[kKeyMax] = {1, 0};   // "\1": none yet
char cur_name[24] = {};
int load_next = -1;      // 0..2 the lists, 3 the used set, 4 done; -1 not loading
bool g_ready = false, card_on = false;
volatile bool began = false;
bool (*play_fn)() = nullptr;
Stats st;
uint8_t pre_done[kCodes / 8];   // the preload's rows done (preload_step)
bool pre_on = false;
// an instrument being added: its .sfz read first
struct SfzJob {
  bool on, take;
  int list, code, bank;  // code >= 0: into that row (set), else the bank's first free row (add)
  int confirm;
  char path[lib::kPath];
  int result, out;       // the last one's outcome (UI)
} SJ = {};
msamp::Region* sfz_regions = nullptr;
char (*sfz_samples)[msamp::kPath] = nullptr;
// a fill: the folder listed by srclib's browser (the list's kind), then its files into the bank
struct FillJob {
  bool on;
  int list, bank, i, filled;   // i: the next item of its listing
  char folder[lib::kPath];
  uint32_t t0;
} FJ = {};
// instruments waiting for the card job (a take's slice map while a list is written): added in turn
struct PendAdd {
  int list, code, bank, confirm;
  bool take;
  char path[lib::kPath];
};
constexpr int kPend = 8;
PendAdd PQ[kPend];
int npq = 0;

CORE_COLD void begin_change(Lst& l) {
  __atomic_store_n(&l.seq, l.seq + 1, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
CORE_COLD void end_change(Lst& l, bool bump = true) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  __atomic_store_n(&l.seq, l.seq + 1, __ATOMIC_RELEASE);
  if (bump) __atomic_store_n(&l.sig, l.sig + 1, __ATOMIC_RELEASE);
}
CORE_COLD void ensure() {
  if (began) return;   // (set at the end: another core reading at() / refs() sees the lists only once they exist)
  for (int l = 0; l < kLists; ++l) {
    L[l].e = new Entry[kCodes];
    memset(L[l].e, 0, sizeof(Entry) * kCodes);
    L[l].m = l == kInstr ? new Member[kMembers] : nullptr;
    L[l].undo_m = l == kInstr ? new Member[kMembersPer] : nullptr;
    L[l].nm = 0, L[l].seq = 0, L[l].sig = 1, L[l].rgen = ~0u;
    for (int b = 0; b < kBanks; ++b) L[l].e[code(b, 0)].type = kOff;
    for (int v = 0; v < kVoicesMax; ++v) L[l].voice[v] = -1;
  }
  U.n = new uint32_t[kLists * kCodes];
  memset(U.n, 0, sizeof(uint32_t) * kLists * kCodes);
  text = new char[kFileMax + 1];
  __atomic_store_n(&began, true, __ATOMIC_RELEASE);
}
CORE_COLD const char* base_of(const char* p) {
  const char* b = strrchr(p, '/');
  return b ? b + 1 : p;
}
CORE_COLD void name_of(const char* path, char* out) {
  const char* b = base_of(path);
  const char* dot = strrchr(b, '.');
  int n = dot && dot != b ? (int)(dot - b) : (int)strlen(b);
  if (n > kName - 1) n = kName - 1;
  memcpy(out, b, n);
  out[n] = 0;
}
CORE_COLD bool same_ref(const lib::Ref& a, const lib::Ref& b) {
  if (a.has_sha && b.has_sha) return !memcmp(a.sha, b.sha, 32);
  return !strcasecmp(a.hint, b.hint);
}
CORE_COLD void clear_entry(Entry& e, int c) {
  if (e.ram) U.dirty = true, U.dirty_ms = now_ms();   // its "always in RAM" line goes with it
  memset(&e, 0, sizeof e);
  e.type = row_of(c) ? kEmpty : kOff;
}
CORE_COLD void clear_list(int l) {
  Lst& x = L[l];
  begin_change(x);
  for (int c = 0; c < kCodes; ++c) clear_entry(x.e[c], c);
  x.nm = 0;
  x.locked = x.ro = x.dirty = x.undo_ok = false;
  x.ram_all = false;
  x.why[0] = 0;
  x.rgen = ~0u, x.rrun = false;
  end_change(x);
}
CORE_COLD void template_of(int l) {
  Lst& x = L[l];
  begin_change(x);
  if (l == kSamples) {
    for (int i = 0; i < kInputs; ++i) {
      Entry& e = x.e[code(7, 1 + i)];
      e.type = kInput, e.id = (uint8_t)i, e.st = lib::kExact;
      sniprintf(e.name, sizeof e.name, "%s", input_name(i));
    }
    for (int i = 0; i < kSmpBuiltins; ++i) {
      Entry& e = x.e[code(7, 11 + i)];
      e.type = kBuiltin, e.id = (uint8_t)i, e.st = lib::kExact;
      sniprintf(e.name, sizeof e.name, "%s", builtin_name(l, i));
    }
  } else if (l == kTables) {
    for (int i = 0; i < kWtBuiltins; ++i) {
      Entry& e = x.e[code(0, 1 + i)];
      e.type = kBuiltin, e.id = (uint8_t)i, e.st = lib::kExact;
      sniprintf(e.name, sizeof e.name, "%s", builtin_name(l, i));
    }
  }
  x.from_template = true;
  end_change(x);
  ++st.templates;
}
CORE_COLD void mark(int l) {
  L[l].dirty = true;
  L[l].dirty_ms = now_ms();
  L[l].from_template = false;
}
CORE_COLD void mark_info(int l) {   // a resolution change only: written later, never while the Rytm plays
  if (!L[l].info_dirty) L[l].info_dirty = true, L[l].dirty_ms = now_ms();
}
// one ref resolved into its state (and an entry's path); a "-" ref takes its hash once the library has it (the list
// written at the next save). True when something shown changed.
CORE_COLD bool resolve_ref(int l, lib::Ref& r, uint8_t* st_out, char* path_out) {
  bool ch = false;
  if (!r.has_sha) {
    lib::Ref k;
    if (lib::ref_for_path(r.hint, &k) && k.has_sha && (!r.size || k.size == r.size)) {
      memcpy(r.sha, k.sha, 32), r.has_sha = true, r.size = k.size;
      mark_info(l);
      ch = true;
    } else if (!r.size && k.size) {
      r.size = k.size;
    }
  }
  lib::Resolved v;
  lib::State s = lib::resolve(r, &v);
  if (*st_out != (uint8_t)s) *st_out = (uint8_t)s, ch = true;
  if (path_out) {
    const char* p = s == lib::kMissing ? "" : v.path;
    if (strcmp(path_out, p)) sniprintf(path_out, lib::kPath, "%s", p), ch = true;
  }
  return ch;
}
// an instrument's state: the worst of its .sfz and its members (MISSING .sfz: missing; a missing member: CHANGED-like
// flag shown by the views from the members, the instrument still plays)
CORE_COLD void inst_state(Lst& x, Entry& e, uint8_t sfz_st) {
  e.st = sfz_st;
}
CORE_COLD bool resolve_entry(int l, int c) {
  Lst& x = L[l];
  Entry& e = x.e[c];
  if (!has_file(e)) return false;
  begin_change(x);
  uint8_t s = e.st;
  bool ch = resolve_ref(l, e.ref, &s, e.path);
  if (e.type == kInstrument) {
    for (int i = 0; i < e.nm; ++i) {
      Member& m = x.m[e.m0 + i];
      ch |= resolve_ref(l, m.ref, &m.st, nullptr);
    }
    inst_state(x, e, s);
  } else {
    e.st = s;
  }
  end_change(x);
  return ch;
}
// the resolution pass: kResolvePer refs a step; restarted when lib::gen() moves
CORE_COLD bool resolve_step() {
  uint32_t g = lib::gen();
  for (int l = 0; l < kLists; ++l) {
    Lst& x = L[l];
    if (!x.rrun && x.rgen == g) continue;
    if (!x.rrun) x.rrun = true, x.rpos = 0, x.rgen = g;
    int done = 0;
    bool ch = false;
    while (x.rpos < kCodes && done < kResolvePer) {
      Entry& e = x.e[x.rpos];
      if (has_file(e)) {
        ch |= resolve_entry(l, x.rpos);
        done += 1 + (e.type == kInstrument ? e.nm : 0);
        ++st.resolves;
      }
      ++x.rpos;
    }
    if (ch) __atomic_store_n(&x.sig, x.sig + 1, __ATOMIC_RELEASE);
    if (x.rpos >= kCodes) x.rrun = false;
    return true;
  }
  return false;
}

// ---- the list text (the save; refs moved: their hints rewritten now)
// up to max rows of list l from *fc into out at *n (refs moved: their hints rewritten now, under the sequence counter);
// 1 done, 0 more to do, -1 does not fit
CORE_COLD int format_rows(int l, int* fc, char* out, uint32_t cap, uint32_t* n, int max) {
  Lst& x = L[l];
  int rows = 0;
  for (; *fc < kCodes && rows < max; ++*fc) {
    int c = *fc;
    Entry& e = x.e[c];
    if (e.type == kEmpty || e.type == kOff) continue;
    ++rows;
    begin_change(x);
    if (has_file(e) && e.st == lib::kMoved && e.path[0]) sniprintf(e.ref.hint, sizeof e.ref.hint, "%s", e.path);
    for (int i = 0; e.type == kInstrument && i < e.nm; ++i) {
      Member& m = x.m[e.m0 + i];
      lib::Resolved v;
      if (m.st == lib::kMoved && lib::resolve(m.ref, &v) == lib::kMoved && v.path[0])
        sniprintf(m.ref.hint, sizeof m.ref.hint, "%s", v.path);
    }
    end_change(x, false);
    text::Line t;
    memset(&t, 0, sizeof t);
    t.code = c, t.type = e.type, t.id = e.id, t.ref = e.ref, t.frames = e.frames, t.rate = e.rate, t.ch = e.ch;
    sniprintf(t.name, sizeof t.name, "%s", e.name);
    int k = text::put(l, t, out + *n, (int)(cap - *n));
    if (!k) return -1;
    *n += (uint32_t)k;
    for (int i = 0; e.type == kInstrument && i < e.nm; ++i) {
      text::Line ml;
      memset(&ml, 0, sizeof ml);
      ml.member = true, ml.code = c, ml.ref = x.m[e.m0 + i].ref;
      k = text::put(l, ml, out + *n, (int)(cap - *n));
      if (!k) return -1;
      *n += (uint32_t)k;
    }
  }
  return *fc >= kCodes ? 1 : 0;
}
CORE_COLD uint32_t format_used(char* out, uint32_t cap) {
  uint32_t n = (uint32_t)fmt::put_text_header(out, (int)cap, "poolused", 1, 1);
  for (int l = 0; l < kLists; ++l)
    for (int c = 0; c < kCodes; ++c) {
      uint32_t k = U.n[l * kCodes + c];
      if (!k) continue;
      text::Used u = {l, c, k, 0};
      int w = text::put_used(u, out + n, (int)(cap - n));
      if (!w) return n;
      n += (uint32_t)w;
    }
  for (int l = 0; l < kLists; ++l) {   // "always in RAM" (docs/STEREO.md), after the counts: an older reader keeps those
    if (L[l].ram_all) {
      text::Used u = {l, -1, 0, 2};
      int w = text::put_used(u, out + n, (int)(cap - n));
      if (!w) return n;
      n += (uint32_t)w;
    }
    for (int c = 0; L[l].e && c < kCodes; ++c) {
      if (!L[l].e[c].ram) continue;
      text::Used u = {l, c, 0, 1};
      int w = text::put_used(u, out + n, (int)(cap - n));
      if (!w) return n;
      n += (uint32_t)w;
    }
  }
  return n;
}

// ---- reading a list (the parse into the list)
struct Fill {
  int l;
  int cur;   // the instrument being filled (-1 none)
  bool over;
};
CORE_COLD bool take_line(void* c, const text::Line& t) {
  Fill* f = (Fill*)c;
  Lst& x = L[f->l];
  if (t.member) {
    if (f->cur < 0) return true;
    Entry& e = x.e[f->cur];
    if (x.nm >= kMembers || e.nm >= kMembersPer) {
      f->over = true;
      return false;
    }
    if (!e.nm) e.m0 = (uint16_t)x.nm;
    Member& m = x.m[x.nm++];
    m.ref = t.ref, m.st = lib::kPending;
    ++e.nm;
    return true;
  }
  Entry& e = x.e[t.code];
  memset(&e, 0, sizeof e);
  e.type = t.type, e.id = t.id, e.frames = t.frames, e.rate = t.rate, e.ch = t.ch;
  e.st = has_file(e) ? (uint8_t)lib::kPending : (uint8_t)lib::kExact;
  e.ref = t.ref;
  if (t.name[0]) sniprintf(e.name, sizeof e.name, "%s", t.name);
  else if (has_file(e)) name_of(t.ref.hint, e.name);
  if (has_file(e)) sniprintf(e.path, sizeof e.path, "%s", t.ref.hint);   // by its hint until resolved
  f->cur = t.type == kInstrument ? t.code : -1;
  return true;
}
// A list's bytes are in (or absent: the template): the verdict from its header (newer: locked; foreign-newer: read only;
// damaged, too big, a read error: locked), then the parse in pieces (parse_step). True: a parse follows.
CORE_COLD bool loaded_list(int l, uint32_t n, bool absent) {
  clear_list(l);
  Lst& x = L[l];
  if (absent) {
    template_of(l);
    log(CORE_STR("pools: %s: no list for %s: the template"), list_name(l), cur_key);
    return false;
  }
  ++st.reads;
  const fmt::Format* f = fmt::find(format_of(l));
  fmt::Info info;
  memset(&info, 0, sizeof info);
  fmt::Bytes in = {(const uint8_t*)text, n};
  fmt::Sniffed sn = {};
  bool hdr = f->sniff(in, &sn);
  fmt::Status s = fmt::kOk;
  if (J.read_err) s = fmt::kUnreadable, sniprintf(info.why, sizeof info.why, "a card error while reading");
  else if (J.too_big || n > f->max_bytes) s = fmt::kUnreadable, sniprintf(info.why, sizeof info.why, "too big");
  else if (!hdr) s = fmt::kUnreadable, sniprintf(info.why, sizeof info.why, "not this format");
  else if (sn.version > f->current) s = sn.min_reader > f->current ? fmt::kNewer : fmt::kForeignNewer;
  info.st = s, info.version = sn.version, info.min_reader = sn.min_reader;
  if (s == fmt::kNewer || s == fmt::kUnreadable) {
    begin_change(x);
    x.locked = true;
    info.locked = true;
    fmt::refusal(*f, info, J.path, x.why, sizeof x.why);
    end_change(x);
    ++st.refused;
    log(CORE_STR("pools: %s"), x.why);
    return false;
  }
  x.ro = s == fmt::kForeignNewer;
  J.pos = J.body = sn.body;
  text::parser_init(&J.ps);
  if (J.prev) mark(l);   // (read from its .prev: written in place at the next save)
  return true;
}
// kParseLines lines of the list's text into it (its sig bumped only at the end: the machines compose once)
CORE_COLD bool parse_step(int l, uint32_t n) {
  Lst& x = L[l];
  uint32_t a = J.pos, b = a;
  for (int k = 0; k < kParseLines && b < n; ++k) {
    while (b < n && text[b] != '\n') ++b;
    if (b < n) ++b;
  }
  Fill fl = {l, J.fc, false};   // (J.fc: the instrument being filled across pieces)
  char why[80];
  begin_change(x);
  bool ok = text::parse_more(l, &J.ps, text + a, b - a, take_line, &fl, why, sizeof why);
  J.fc = fl.cur;
  J.pos = b;
  if (!ok) {   // the module's own limits (members past kMembers) or a damaged line: locked, nothing of it plays
    for (int c = 0; c < kCodes; ++c) clear_entry(x.e[c], c);
    x.nm = 0;
    x.locked = true, x.ro = false;
    sniprintf(x.why, sizeof x.why, "%s: not a %s file this Sidecar can read (%s). Not loaded, not changed.", J.path,
              format_of(l), why);
    end_change(x);
    ++st.refused;
    log(CORE_STR("pools: %s"), x.why);
    return true;
  }
  bool done = b >= n;
  if (done) x.rgen = ~0u;
  end_change(x, done);
  if (!done) return false;
  if (x.ro) sniprintf(x.why, sizeof x.why, "%s: made by a newer Sidecar: read, not written back", J.path);
  log(CORE_STR("pools: %s: %d entries%s"), J.path, count(l), x.ro ? " (read only)" : "");
  return true;
}
CORE_COLD bool take_used(void* c, const text::Used& u) {
  if (u.ram == 2) return L[u.list].ram_all = true, true;
  if (u.ram == 1) return L[u.list].e[u.code].ram = 1, true;
  uint32_t& k = U.n[u.list * kCodes + u.code];
  k = u.count ? u.count : 1;
  return true;
}
CORE_COLD void loaded_used(uint32_t n, bool absent) {
  memset(U.n, 0, sizeof(uint32_t) * kLists * kCodes);
  U.dirty = false;
  for (int l = 0; l < kLists; ++l) {   // the "always in RAM" flags come with the file
    L[l].ram_all = false;
    for (int c = 0; L[l].e && c < kCodes; ++c) L[l].e[c].ram = 0;
  }
  if (absent) return;
  const fmt::Format* f = fmt::find("poolused");
  fmt::Info info;
  fmt::Bytes in = {(const uint8_t*)text, n};
  if (J.too_big || fmt::load(*f, in, nullptr, &info) != fmt::kOk) {   // class D: losing it costs only preloading
    log(CORE_STR("pools: %s not read (rebuilt as rows play)"), J.path);
    return;
  }
  fmt::Sniffed sn;
  fmt::text_header(in, "poolused", &sn);
  char why[60];
  text::parse_used(text + sn.body, n - sn.body, take_used, nullptr, why, sizeof why);
}

// ---- the card job (one at a time): a read is open, reads of kPieceRead, close; a write is one kAtomicReq
CORE_COLD bool path_of(int l, What w, char* out, int cap) {
  return fmt::path(w == kWUsed ? "poolused" : format_of(l), cur_key, 0, out, cap) != nullptr;
}
CORE_COLD void start_read(What w, int l, const char* path) {
  J.what = w, J.list = l, J.prev = false, J.got = 0, J.too_big = false, J.read_err = false;
  sniprintf(J.path, sizeof J.path, "%s", path);
  J.rq.state = cardq::kFree;
  J.io = kIoOpen;
}
CORE_COLD void sfz_done(uint32_t n);
// the read finished (absent: neither the file nor its .prev could be opened)
CORE_COLD void list_loaded() {   // a list (or the used set) of the project's load done: the next one
  if (load_next < 0) return;
  ++load_next;
  if (load_next > kLists) {
    load_next = -1, g_ready = true;
    memset(pre_done, 0, sizeof pre_done), pre_on = true;
    log(CORE_STR("pools: %s: samples %d, wavetables %d, instruments %d"), cur_key, count(kSamples), count(kTables),
        count(kInstr));
  }
}
CORE_COLD void read_done(bool absent) {
  uint32_t n = absent ? 0 : J.got;
  text[n] = 0;
  if (J.what == kWList) {
    if (loaded_list(J.list, n, absent)) {   // its parse over the next steps
      J.fc = -1;
      J.io = kIoParse;
      return;
    }
  } else if (J.what == kWUsed) {
    loaded_used(n, absent);
  } else {
    sfz_done(absent ? 0 : n);
    return;
  }
  list_loaded();
}
// one step of the job; true while it holds the card queue's attention (did or waits)
CORE_COLD bool io_step() {
  switch (J.io) {
    case kIoIdle: return false;
    case kIoOpen: {
      char p[208];
      sniprintf(p, sizeof p, "%s%s", J.path, J.prev ? ".prev" : "");
      if (!cardq::async(&J.rq, cardq::kOpenReq, &J.f, p, 0, nullptr, 0)) return true;
      if (J.rq.got != 1) {
        if (J.what == kWSfz) {
          J.io = kIoIdle;
          read_done(true);
          return true;
        }
        J.io = kIoExists, J.rq.state = cardq::kFree;   // absent, or there and not opened (a card error)?
        return true;
      }
      if (J.prev) log(CORE_STR("pools: %s missing: its .prev read (written at the next save)"), J.path);
      J.io = kIoRead, J.rq.state = cardq::kFree;
      return true;
    }
    case kIoExists: {
      char p[208];
      sniprintf(p, sizeof p, "%s%s", J.path, J.prev ? ".prev" : "");
      if (!cardq::async(&J.rq, cardq::kExistsReq, &J.f, p, 0, nullptr, 0)) return true;
      J.rq.state = cardq::kFree;
      if (J.rq.got == 1) {   // there but not opened: a card error - locked, never written over (not the template)
        J.read_err = true;
        J.io = kIoIdle;
        read_done(false);
        return true;
      }
      if (!J.prev) {   // a cut between write_atomic's renames: the old file is the .prev
        J.prev = true;
        J.io = kIoOpen;
        return true;
      }
      J.io = kIoIdle;
      read_done(true);
      return true;
    }
    case kIoRead: {
      uint32_t cap = J.what == kWSfz ? kSfzMax : kFileMax;
      uint32_t want = cap + 1 - J.got < kPieceRead ? cap + 1 - J.got : kPieceRead;   // (one past the cap: too big)
      if (!cardq::async(&J.rq, cardq::kReadReq, &J.f, nullptr, J.got, (uint8_t*)text + J.got, want)) return true;
      int r = J.rq.got;
      J.rq.state = cardq::kFree;
      if (r < 0) J.read_err = true, J.io = kIoClose;   // a card error: never taken as the file's end
      else {
        J.got += (uint32_t)r;
        if (J.got > cap) J.too_big = true, J.got = cap, J.io = kIoClose;
        else if (r < (int)want) J.io = kIoClose;
      }
      return true;
    }
    case kIoClose: {
      if (!cardq::async(&J.rq, cardq::kCloseReq, &J.f, nullptr, 0, nullptr, 0)) return true;
      J.io = kIoIdle;
      if (J.what == kWSfz && (J.read_err || J.too_big)) {
        sfz_done(0);
        return true;
      }
      read_done(false);
      return true;
    }
    case kIoParse:
      if (parse_step(J.list, J.got)) J.io = kIoIdle, list_loaded();
      return true;
    case kIoFormat: {
      int r = format_rows(J.list, &J.fc, text, kFileMax, &J.got, kFormatRows);
      if (r < 0) {
        ++st.save_fails;
        log(CORE_STR("pools: %s does not fit in %lu bytes: not written"), J.path, (unsigned long)kFileMax);
        J.io = kIoIdle;
      } else if (r > 0) {
        J.rq.state = cardq::kFree;
        J.io = kIoWrite;
      }
      return true;
    }
    case kIoWrite: {
      if (!cardq::async(&J.rq, cardq::kAtomicReq, &J.f, J.path, 0, (uint8_t*)text, J.got)) return true;
      if (J.rq.got < 0) {
        ++st.save_fails;
        bool give_up = J.what != kWUsed && ++L[J.list].fails >= kMaxFails;
        log(CORE_STR("pools: cannot write %s (%s)"), J.path, give_up ? "given up: the list stays in RAM" : "kept for the next save");
        if (J.what == kWUsed) U.dirty = true, U.dirty_ms = now_ms();
        else if (!give_up) mark(J.list);
      } else if (J.what == kWUsed) {
        ++st.used_saves;
      } else {
        L[J.list].fails = 0;
        ++st.saves;
      }
      J.io = kIoIdle;
      return true;
    }
  }
  return false;
}
CORE_COLD bool start_write(What w, int l) {
  char p[200];
  if (!path_of(l, w, p, sizeof p)) return false;
  J.what = w, J.list = l;
  sniprintf(J.path, sizeof J.path, "%s", p);
  J.rq.state = cardq::kFree;
  if (w == kWUsed) {   // (small: at once)
    J.got = format_used(text, kFileMax);
    J.io = kIoWrite;
    return true;
  }
  J.got = (uint32_t)fmt::put_text_header(text, (int)kFileMax, format_of(l), 1, 1);
  J.fc = 0;
  J.io = kIoFormat;    // the rows over the next steps, then the write
  return true;
}
// the next write due (force: no delay - a project switch flushes); true when one was started. A resolution-only change
// waits while the Rytm plays (unless forced).
CORE_COLD bool save_due(bool force) {
  if (!card_on || J.io != kIoIdle) return false;
  uint32_t now = now_ms();
  for (int l = 0; l < kLists; ++l) {
    Lst& x = L[l];
    bool info = x.info_dirty && (force || !playing());
    if ((!x.dirty && !info) || (!force && (int32_t)(now - x.dirty_ms) < (int32_t)kSaveDelayMs)) continue;
    x.dirty = x.info_dirty = false;
    if (x.locked || x.ro) continue;   // never written
    if (start_write(kWList, l)) return true;
  }
  if (U.dirty && (force || ((int32_t)(now - U.dirty_ms) >= (int32_t)kUsedDelayMs && !playing()))) {   // (not while it plays)
    U.dirty = false;
    if (start_write(kWUsed, 0)) return true;
  }
  return false;
}
CORE_COLD bool any_dirty() {
  for (int l = 0; l < kLists; ++l)
    if ((L[l].dirty || L[l].info_dirty) && !L[l].locked && !L[l].ro) return true;
  return U.dirty;
}

// a content still held by a row of the SAMPLES list or an instrument's member (the pool RAM's users)
CORE_COLD bool referenced(const lib::Ref& r) {
  for (int c = 0; c < kCodes; ++c) {
    const Entry& e = L[kSamples].e[c];
    if (e.type == kSample && same_ref(e.ref, r)) return true;
    const Entry& f = L[kInstr].e[c];
    for (int i = 0; f.type == kInstrument && i < f.nm; ++i)
      if (same_ref(L[kInstr].m[f.m0 + i].ref, r)) return true;
  }
  return false;
}
// ---- the preload (the design's 4.2): after a project's lists are read, the SAMPLES rows of its used set, the most
// seen first, into the pool RAM as prefetches (free space only: up to the budget, nothing evicted)
CORE_COLD bool preload_step() {
  if (!pre_on || !pcache::ok()) return false;
  int best = -1;
  uint32_t bc = 0;
  for (int c = 0; c < kCodes; ++c) {
    if (pre_done[c >> 3] >> (c & 7) & 1) continue;
    uint32_t k = U.n[kSamples * kCodes + c];
    const Entry& e = L[kSamples].e[c];
    if (!k || e.type != kSample || !playable(e)) continue;
    if (k > bc) bc = k, best = c;
  }
  if (best < 0) {
    pre_on = false;
    return false;
  }
  pre_done[best >> 3] |= (uint8_t)(1 << (best & 7));
  const Entry& e = L[kSamples].e[best];
  pcache::want(e.ref, e.path, true);
  return true;
}

// ---- edits
CORE_COLD bool can_edit(int l) { return l >= 0 && l < kLists && !L[l].locked && g_ready; }
CORE_COLD int row_check(int c) {
  if (c < 0 || c >= kCodes) return kErrRow;
  if (!row_of(c)) return kErrOff;
  return kOk;
}
// P3 (the UX review): a row in use is not changed without a confirm, and not while the Rytm plays unless forced
CORE_COLD int guard(int l, int c, int confirm) {
  const Entry& e = L[l].e[c];
  if (e.type == kEmpty) return kOk;
  if (!in_use(l, c)) return kOk;
  if (playing() && confirm < kForced) return kErrPlaying;
  if (confirm < kConfirmed) return kErrUsed;
  return kOk;
}
CORE_COLD void keep_undo(int l, int c) {
  Lst& x = L[l];
  x.undo_ok = true, x.undo_code = c, x.undo_e = x.e[c];
  if (x.undo_e.type == kInstrument && x.undo_m) {
    int n = x.undo_e.nm < kMembersPer ? x.undo_e.nm : kMembersPer;
    memcpy(x.undo_m, x.m + x.undo_e.m0, sizeof(Member) * n);
    x.undo_e.nm = (uint16_t)n;
  }
}
// room for n members at the table's end (compacted when full: the live instruments' members moved down)
CORE_COLD int member_room(int l, int n) {
  Lst& x = L[l];
  if (x.nm + n <= kMembers) return x.nm;
  // compact: the instruments in the order of their members' place (m0), each moved down (never over one not moved yet)
  static int16_t order[kCodes];
  int no = 0;
  for (int c = 0; c < kCodes; ++c)
    if (x.e[c].type == kInstrument && x.e[c].nm) {
      int k = no++;
      while (k > 0 && x.e[order[k - 1]].m0 > x.e[c].m0) order[k] = order[k - 1], --k;
      order[k] = (int16_t)c;
    }
  int w = 0;
  begin_change(x);
  for (int i = 0; i < no; ++i) {
    Entry& e = x.e[order[i]];
    if (e.m0 != w) memmove(x.m + w, x.m + e.m0, sizeof(Member) * e.nm);
    e.m0 = (uint16_t)w;
    w += e.nm;
  }
  x.nm = w;
  end_change(x);
  return x.nm + n <= kMembers ? x.nm : -1;
}
CORE_COLD void put_entry(int l, int c, const Entry& src, const Member* mem, int nm) {
  Lst& x = L[l];
  int at = 0;
  if (src.type == kInstrument && nm) {
    at = member_room(l, nm);
    if (at < 0) nm = 0, at = 0, log(CORE_STR("pools: the instruments' member table is full: %s has no members"), src.name);
  }
  begin_change(x);
  Entry& e = x.e[c];
  e = src;
  e.m0 = (uint16_t)at, e.nm = (uint16_t)nm;
  if (nm) memcpy(x.m + at, mem, sizeof(Member) * nm), x.nm = at + nm;
  end_change(x);
  if (has_file(e)) resolve_entry(l, c);
  mark(l);
}
CORE_COLD int make_file_entry(int l, const char* path, Entry* out) {
  if (!path || path[0] != '/' || strlen(path) >= (size_t)lib::kPath) return kErrPath;
  if (!accepts_file(l, path)) return kErrType;
  lib::Ref r;
  bool known = lib::ref_for_path(path, &r);
  if (!known && lib::stats().state == lib::kReady) return kErrPath;   // the walk is done and it is not on the card
  memset(out, 0, sizeof *out);
  out->type = l == kInstr ? kInstrument : kSample;
  out->ref = r;
  out->st = lib::kPending;
  name_of(path, out->name);
  sniprintf(out->path, sizeof out->path, "%s", path);
  return kOk;
}
CORE_COLD int place(int l, int c, const Entry& ne, const Member* mem, int nm, int* out, int confirm) {
  int ex = find_hash(l, ne.ref);
  if (ex >= 0) {
    if (out) *out = ex;
    return kExisting;
  }
  const Entry& e = L[l].e[c];
  if (e.type == kInput || e.type == kBuiltin) return kErrBuiltin;
  int g = guard(l, c, confirm);
  if (g != kOk) return g;
  if (e.type != kEmpty) keep_undo(l, c);
  put_entry(l, c, ne, mem, nm);
  if (out) *out = c;
  log(CORE_STR("pools: %s %c%03d = %s"), list_name(l), 'A' + bank_of(c), row_of(c), ne.ref.hint);
  return kOk;
}
CORE_COLD int start_sfz(int l, int c, int bank, const char* path, int confirm, bool take = false) {
  if (!card_on) return kErrPath;
  if (SJ.on || J.io != kIoIdle) {   // the card job is busy (a list written): queued, started in turn
    if (npq >= kPend) return kErrBusy;
    PendAdd& q = PQ[npq++];
    q.list = l, q.code = c, q.bank = bank, q.confirm = confirm, q.take = take;
    sniprintf(q.path, sizeof q.path, "%s", path);
    return kPending;
  }
  SJ.take = take;
  SJ.on = true, SJ.list = l, SJ.code = c, SJ.bank = bank, SJ.confirm = confirm;
  sniprintf(SJ.path, sizeof SJ.path, "%s", path);
  SJ.result = kPending, SJ.out = -1;
  start_read(kWSfz, l, path);
  return kPending;
}
// the .sfz is read: its distinct samples (in mapping order) become the members, the entry placed
CORE_COLD void sfz_done(uint32_t n) {
  SJ.on = false;
  int l = SJ.list;
  if (!n || J.too_big) {
    SJ.result = kErrPath;
    log(CORE_STR("pools: %s: cannot read the instrument"), SJ.path);
    return;
  }
  if (!sfz_regions) sfz_regions = new msamp::Region[kSfzRegions];
  if (!sfz_samples) sfz_samples = new char[kMembersPer][msamp::kPath];
  msamp::Desc d;
  memset(&d, 0, sizeof d);
  d.regions = sfz_regions, d.max_regions = kSfzRegions, d.samples = sfz_samples, d.max_samples = kMembersPer;
  char dir[lib::kPath];
  sniprintf(dir, sizeof dir, "%s", SJ.path);
  char* sl = strrchr(dir, '/');
  if (sl) *sl = 0;
  if (!msamp::parse(text, (int)n, dir, &d)) {
    SJ.result = kErrType;
    log(CORE_STR("pools: %s: not an instrument this reads (%s)"), SJ.path, d.err);
    return;
  }
  Entry ne;
  int r = make_file_entry(l, SJ.path, &ne);
  if (r != kOk) {
    SJ.result = r;
    return;
  }
  static Member mem[kMembersPer];
  int nm = 0;
  for (int i = 0; i < d.nsamples && nm < kMembersPer; ++i) {
    if (strlen(sfz_samples[i]) >= (size_t)lib::kPath) continue;
    lib::ref_for_path(sfz_samples[i], &mem[nm].ref);   // (unknown to the library: by its path, hashed when it is)
    mem[nm].st = lib::kPending;
    ++nm;
  }
  int c = SJ.code;
  if (c < 0) {
    int ex = find_hash(l, ne.ref);
    if (ex >= 0) {
      SJ.result = kExisting, SJ.out = ex;
      return;
    }
    c = first_free(l, SJ.bank);
    for (int k = 1; c < 0 && SJ.take && k < kBanks; ++k) c = first_free(l, (SJ.bank + k) % kBanks);   // a take: the next bank
    if (c < 0) {
      SJ.result = kErrFull;
      return;
    }
  }
  SJ.result = place(l, c, ne, mem, nm, &SJ.out, SJ.confirm);
  log(CORE_STR("pools: %s: %d regions, %d samples (%s)"), SJ.path, d.nregions, nm, result_text(SJ.result));
}
// a fill: its own browser (srclib::kFill: never a user's), kFillItems files a step
CORE_COLD bool fill_step() {
  if (!FJ.on) return false;
  int k = srclib::kFill;
  if (strcmp(srclib::cwd(k), FJ.folder)) srclib::cd(k, FJ.folder);
  if (!srclib::listing_done(k)) return false;
  int n = srclib::count(k);
  for (int done = 0; FJ.i < n && done < kFillItems; ++FJ.i) {
    const char* nm;
    bool dir;
    if (!srclib::item(k, FJ.i, &nm, &dir, nullptr) || dir) continue;
    char path[lib::kPath];
    if (!srclib::item_path(k, FJ.i, path, sizeof path) || !accepts_file(FJ.list, path) || FJ.list == kInstr) continue;
    ++done;
    Entry ne;
    if (make_file_entry(FJ.list, path, &ne) != kOk) continue;
    if (find_hash(FJ.list, ne.ref) >= 0) continue;   // in the list already: it keeps its row
    int c = first_free(FJ.list, FJ.bank);
    if (c < 0) {
      FJ.i = n;
      break;
    }
    put_entry(FJ.list, c, ne, nullptr, 0);
    ++FJ.filled;
  }
  if (FJ.i < n) return true;
  ++st.fills;
  log(CORE_STR("pools: %s bank %c: %d rows filled from %s"), list_name(FJ.list), 'A' + FJ.bank, FJ.filled, FJ.folder);
  FJ.on = false;
  return true;
}
CORE_COLD void project_now(char* key, int cap) {
  char p[24];
  memcpy(p, (const char*)core::project, sizeof p);
  p[23] = 0;
  project_key(p, key, cap);
  sniprintf(cur_name, sizeof cur_name, "%s", p);
}
}  // namespace

// ---- lifecycle
CORE_COLD void begin(bool card) {
  ensure();
  card_on = card;
  cur_key[0] = 1, cur_key[1] = 0;
  g_ready = false, load_next = -1;
  J.io = kIoIdle;
  SJ.on = false, FJ.on = false;
}
CORE_COLD bool step() {
  ensure();
  if (io_step()) return true;
  char key[kKeyMax];
  project_now(key, sizeof key);
  if (prjslot::moving()) return false;   // (a slot job moves / copies files: no list written meanwhile)
  if (strcmp(key, cur_key)) {   // another project: the old one's writes first, then its lists one by one
    if (any_dirty() && cur_key[0] != 1) return save_due(true);
    sniprintf(cur_key, sizeof cur_key, "%s", key);
    g_ready = false;
    for (int l = 0; l < kLists; ++l) L[l].undo_ok = false;
    if (!card_on) {
      for (int l = 0; l < kLists; ++l) clear_list(l), template_of(l);
      memset(U.n, 0, sizeof(uint32_t) * kLists * kCodes);
      g_ready = true;
      return true;
    }
    load_next = 0;
  }
  if (load_next >= 0) {
    char p[200];
    if (load_next < kLists) {
      if (!path_of(load_next, kWList, p, sizeof p)) {   // (a key too long: cannot happen with project_key)
        clear_list(load_next), template_of(load_next), ++load_next;
        return true;
      }
      start_read(kWList, load_next, p);
    } else {
      path_of(0, kWUsed, p, sizeof p);
      start_read(kWUsed, 0, p);
    }
    return io_step();
  }
  if (save_due(false)) return io_step();
  if (npq && !SJ.on && J.io == kIoIdle) {   // an instrument that waited for the card job
    PendAdd q = PQ[0];
    memmove(PQ, PQ + 1, sizeof(PendAdd) * (npq - 1));
    --npq;
    start_sfz(q.list, q.code, q.bank, q.path, q.confirm, q.take);
    return true;
  }
  if (fill_step()) return true;
  if (resolve_step()) return true;
  return preload_step();
}
bool ready() { return g_ready; }
bool saving() { return J.io == kIoWrite || any_dirty(); }
const char* project() { return cur_key[0] == 1 ? "" : cur_key; }
CORE_COLD void rekey(const char* from, const char* to) {
  if (cur_key[0] != 1 && !strcmp(cur_key, from)) sniprintf(cur_key, sizeof cur_key, "%s", to);
}
uint32_t sig(int l) { return l >= 0 && l < kLists && began ? __atomic_load_n(&L[l].sig, __ATOMIC_ACQUIRE) : 0; }
const Entry& at(int l, int c) {
  static Entry none = {};
  if (!began || l < 0 || l >= kLists || c < 0 || c >= kCodes) return none;
  return L[l].e[c];
}
const Member* members(int l, int c, int* n) {
  *n = 0;
  if (!began || l != kInstr || c < 0 || c >= kCodes || L[l].e[c].type != kInstrument) return nullptr;
  *n = L[l].e[c].nm;
  return L[l].m + L[l].e[c].m0;
}
bool locked(int l) { return began && l >= 0 && l < kLists && L[l].locked; }
CORE_COLD void set_ram(int l, int c, bool on) {
  if (!began || l < 0 || l >= kLists) return;
  if (c < 0) L[l].ram_all = on;
  else if (c < kCodes && L[l].e) L[l].e[c].ram = on ? 1 : 0;
  U.dirty = true, U.dirty_ms = now_ms();
}
CORE_COLD bool ram_all(int l) { return began && l >= 0 && l < kLists && L[l].ram_all; }
CORE_COLD bool always_ram(int l, int c) {
  if (!began || l < 0 || l >= kLists) return false;
  return L[l].ram_all || (c >= 0 && c < kCodes && L[l].e && L[l].e[c].ram);
}
bool read_only(int l) { return began && l >= 0 && l < kLists && L[l].ro; }
const char* lock_why(int l) { return began && l >= 0 && l < kLists ? L[l].why : ""; }
int count(int l) {
  if (!began || l < 0 || l >= kLists) return 0;
  int n = 0;
  for (int c = 0; c < kCodes; ++c) n += L[l].e[c].type != kEmpty && L[l].e[c].type != kOff;
  return n;
}
int first_free(int l, int bank) {
  if (!began || l < 0 || l >= kLists || bank < 0 || bank >= kBanks) return -1;
  for (int r = 1; r < kRows; ++r)
    if (L[l].e[code(bank, r)].type == kEmpty) return code(bank, r);
  return -1;
}
int find_hash(int l, const lib::Ref& r) {
  if (!began || l < 0 || l >= kLists) return -1;
  for (int c = 0; c < kCodes; ++c) {
    const Entry& e = L[l].e[c];
    if (!has_file(e)) continue;
    if (same_ref(e.ref, r)) return c;
    if (!r.has_sha && e.path[0] && !strcasecmp(e.path, r.hint)) return c;
  }
  return -1;
}

// ---- edits
const char* result_text(int r) {
  switch (r) {
    case kOk: return "SET";
    case kExisting: return "IN POOL";
    case kPending: return "READING";
    case kErrOff: return "000 IS OFF";
    case kErrBuiltin: return "BUILT-IN";
    case kErrType: return "NOT FOR THIS POOL";
    case kErrFull: return "BANK FULL";
    case kErrLocked: return "POOL LOCKED";
    case kErrPath: return "NO SUCH FILE";
    case kErrRow: return "NO SUCH ROW";
    case kErrUsed: return "IN USE: REPLACE?";
    case kErrPlaying: return "PLAYING: STOP FIRST";
    case kErrBusy: return "BUSY";
    case kErrEmpty: return "EMPTY";
    default: return "?";
  }
}
CORE_COLD int set_file(int l, int c, const char* path, int* out, int confirm) {
  ensure();
  if (l < 0 || l >= kLists) return kErrRow;
  int r = row_check(c);
  if (r != kOk) return r;
  if (!can_edit(l)) return kErrLocked;
  if (!accepts_file(l, path)) return path && path[0] == '/' ? kErrType : kErrPath;
  const Entry& e = L[l].e[c];
  if (e.type == kInput || e.type == kBuiltin) return kErrBuiltin;
  if (l == kInstr) {
    int g = guard(l, c, confirm);
    if (g != kOk) return g;
    return start_sfz(l, c, 0, path, confirm);
  }
  Entry ne;
  r = make_file_entry(l, path, &ne);
  if (r != kOk) return r;
  return place(l, c, ne, nullptr, 0, out, confirm);
}
CORE_COLD int add_file(int l, int bank, const char* path, int* out) {
  ensure();
  if (l < 0 || l >= kLists || bank < 0 || bank >= kBanks) return kErrRow;
  if (!can_edit(l)) return kErrLocked;
  if (!accepts_file(l, path)) return path && path[0] == '/' ? kErrType : kErrPath;
  if (l == kInstr) return start_sfz(l, -1, bank, path, kAsk);
  Entry ne;
  int r = make_file_entry(l, path, &ne);
  if (r != kOk) return r;
  int ex = find_hash(l, ne.ref);
  if (ex >= 0) {
    if (out) *out = ex;
    return kExisting;
  }
  int c = first_free(l, bank);
  if (c < 0) return kErrFull;
  return place(l, c, ne, nullptr, 0, out, kAsk);
}
int g_shown[kLists] = {0, 0, 0};
void shown(int l, int bank) {
  if (l >= 0 && l < kLists && bank >= 0 && bank < kBanks) g_shown[l] = bank;
}
int shown_bank(int l) { return l >= 0 && l < kLists ? g_shown[l] : 0; }
CORE_COLD int add_take(int l, const char* path, int* out) {
  int b0 = shown_bank(l), r = kErrFull;
  if (l == kInstr) {   // (its .sfz read first; a full bank: the next ones)
    if (!can_edit(l)) return kErrLocked;
    if (!accepts_file(l, path)) return kErrType;
    return start_sfz(l, -1, b0, path, kAsk, true);
  }
  for (int k = 0; k < kBanks && r == kErrFull; ++k) r = add_file(l, (b0 + k) % kBanks, path, out);
  return r;
}
CORE_COLD int set_special(int l, int c, int type, int id, int confirm) {
  ensure();
  if (l < 0 || l >= kLists) return kErrRow;
  int r = row_check(c);
  if (r != kOk) return r;
  if (!can_edit(l)) return kErrLocked;
  if (!takes_type(l, type) || (type != kInput && type != kBuiltin)) return kErrType;
  if ((type == kInput && (id < 0 || id >= kInputs)) || (type == kBuiltin && (id < 0 || id >= builtins(l)))) return kErrType;
  const Entry& e = L[l].e[c];
  if (e.type == kInput || e.type == kBuiltin) return kErrBuiltin;
  int g = guard(l, c, confirm);
  if (g != kOk) return g;
  if (e.type != kEmpty) keep_undo(l, c);
  Entry ne;
  memset(&ne, 0, sizeof ne);
  ne.type = (uint8_t)type, ne.id = (uint8_t)id, ne.st = lib::kExact;
  sniprintf(ne.name, sizeof ne.name, "%s", type == kInput ? input_name(id) : builtin_name(l, id));
  put_entry(l, c, ne, nullptr, 0);
  return kOk;
}
CORE_COLD int unload(int l, int c, int confirm) {
  ensure();
  if (l < 0 || l >= kLists) return kErrRow;
  int r = row_check(c);
  if (r != kOk) return r;
  if (!can_edit(l)) return kErrLocked;
  Lst& x = L[l];
  if (x.e[c].type == kEmpty) return kErrEmpty;
  int g = guard(l, c, confirm);
  if (g != kOk) return g;
  keep_undo(l, c);
  Entry old = x.e[c];
  begin_change(x);
  clear_entry(x.e[c], c);
  end_change(x);
  mark(l);
  // its audio leaves the pool RAM when no other row (a sample, an instrument's member) holds the same content
  if (old.type == kSample && l == kSamples && !referenced(old.ref)) pcache::forget(old.ref);
  for (int i = 0; old.type == kInstrument && i < old.nm; ++i)
    if (!referenced(x.m[old.m0 + i].ref)) pcache::forget(x.m[old.m0 + i].ref);
  log(CORE_STR("pools: %s %c%03d unloaded"), list_name(l), 'A' + bank_of(c), row_of(c));
  return kOk;
}
CORE_COLD bool undo(int l) {
  ensure();
  if (l < 0 || l >= kLists || !L[l].undo_ok || !can_edit(l)) return false;
  Lst& x = L[l];
  Entry e = x.undo_e;
  if (has_file(e)) {   // one entry per hash: not when the content went into another row since
    int ex = find_hash(l, e.ref);
    if (ex >= 0 && ex != x.undo_code) return false;
  }
  x.undo_ok = false;
  put_entry(l, x.undo_code, e, x.undo_m, e.type == kInstrument ? e.nm : 0);
  log(CORE_STR("pools: %s %c%03d: undone"), list_name(l), 'A' + bank_of(x.undo_code), row_of(x.undo_code));
  return true;
}
CORE_COLD int fill(int l, int bank, const char* folder) {
  ensure();
  if (l < 0 || l >= kLists || l == kInstr || bank < 0 || bank >= kBanks || !folder || folder[0] != '/' || !can_edit(l))
    return -1;
  if (FJ.on) return -1;
  if (!srclib::cd(srclib::kFill, folder)) return -1;   // (its own browser: nothing opened here; a missing folder lists empty)
  FJ.on = true, FJ.list = l, FJ.bank = bank, FJ.i = 0, FJ.filled = 0, FJ.t0 = now_ms();
  sniprintf(FJ.folder, sizeof FJ.folder, "%s", srclib::cwd(srclib::kFill));
  return 0;
}
bool filling() { return FJ.on; }
bool pending() { return SJ.on || npq > 0; }
int last_result(int* out) {
  if (out) *out = SJ.out;
  return SJ.on || npq ? (int)kPending : SJ.result;
}
CORE_COLD void note_info(int l, int c, uint32_t frames, uint8_t ch, uint32_t rate) {
  if (!began || l < 0 || l >= kLists || c < 0 || c >= kCodes) return;
  Entry& e = L[l].e[c];
  if (e.type != kSample || (e.frames == frames && e.ch == ch && e.rate == rate)) return;
  e.frames = frames, e.ch = ch, e.rate = rate;   // (no sig change: the machines' lists do not show them; written
                                                 // with the list's next save, no write of its own)
}
CORE_COLD void accept_changed(int l, int c) {
  if (!began || l < 0 || l >= kLists || c < 0 || c >= kCodes || !can_edit(l)) return;
  Entry& e = L[l].e[c];
  if (!has_file(e) || e.st != lib::kChanged) return;
  lib::Ref r;
  if (!lib::ref_for_path(e.ref.hint, &r) || !r.has_sha) return;
  begin_change(L[l]);
  e.ref = r;
  end_change(L[l]);
  resolve_entry(l, c);
  mark(l);
}

// ---- used
CORE_COLD void seen(int l, int c) {
  if (!began || l < 0 || l >= kLists || c < 0 || c >= kCodes || !row_of(c)) return;
  uint32_t& k = U.n[l * kCodes + c];
  if (!k) U.dirty = true, U.dirty_ms = now_ms();   // a new code: written a second later (a count alone waits for it)
  if (k < 0xFFFFFFF0u) ++k;
}
bool used(int l, int c) { return began && l >= 0 && l < kLists && c >= 0 && c < kCodes && U.n[l * kCodes + c]; }
uint32_t used_count(int l, int c) { return used(l, c) ? U.n[l * kCodes + c] : 0; }
CORE_COLD void voice_on(int l, int vi, int c) {
  ensure();
  if (l < 0 || l >= kLists || vi < 0 || vi >= kVoicesMax) return;
  for (int k = 0; k < kLists; ++k)
    if (k != l && L[k].voice[vi] >= 0 && c >= 0) L[k].voice[vi] = -1;   // a voice plays one machine at a time
  L[l].voice[vi] = (int16_t)c;
}
int users(int l, int c) {
  if (!began || l < 0 || l >= kLists) return 0;
  int n = 0;
  for (int vi = 0; vi < kVoicesMax; ++vi) {   // (a voice that plays another machine now holds no row of this list)
    const V& x = v[vi];
    bool on = l == kSamples ? x.gr || x.cp : l == kTables ? x.wt : x.ms;
    n += on && L[l].voice[vi] == c;
  }
  return n;
}
bool in_use(int l, int c) { return users(l, c) > 0 || used(l, c); }
void set_playing_fn(bool (*fn)()) { play_fn = fn; }
bool playing() { return play_fn && play_fn(); }
CORE_COLD int select_unused(int l, int16_t* out, int max) {
  if (!began || l < 0 || l >= kLists) return 0;
  int n = 0;
  for (int c = 0; c < kCodes && n < max; ++c) {
    const Entry& e = L[l].e[c];
    if (!has_file(e) || in_use(l, c)) continue;   // (inputs / built-ins hold no RAM: never "unused")
    out[n++] = (int16_t)c;
  }
  return n;
}

// ---- refs (any core)
CORE_COLD int refs(int l, RefFn fn, void* ctx) {
  if (!began || l < 0 || l >= kLists || !fn) return 0;
  Lst& x = L[l];
  static lib::Ref mem[kMembersPer];   // an instrument's members, copied in the same window as its entry (core 0's)
  int n = 0;
  bool all = true;
  for (int c = 0; c < kCodes; ++c) {
    lib::Ref r;
    uint8_t type = kEmpty;
    int nm = 0;
    bool got = false;
    for (int tries = 0; tries < 4096 && !got; ++tries) {
      uint32_t s0 = __atomic_load_n(&x.seq, __ATOMIC_ACQUIRE);
      if (s0 & 1) continue;
      const Entry& e = x.e[c];
      type = e.type, nm = e.nm;
      int m0 = e.m0;
      if (has_file(e)) memcpy(&r, &e.ref, sizeof r);
      if (type == kInstrument && x.m) {
        if (nm > kMembersPer) nm = kMembersPer;
        if (m0 + nm > kMembers) nm = m0 < kMembers ? kMembers - m0 : 0;
        for (int i = 0; i < nm; ++i) memcpy(&mem[i], &x.m[m0 + i].ref, sizeof(lib::Ref));
      }
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      got = __atomic_load_n(&x.seq, __ATOMIC_ACQUIRE) == s0;
    }
    if (!got) {
      all = false;   // (a row changing all the while: the caller reads the lists again or the files)
      continue;
    }
    if (type != kSample && type != kInstrument) continue;
    fn(ctx, l, c, -1, r), ++n;
    for (int i = 0; type == kInstrument && i < nm; ++i) fn(ctx, l, c, i, mem[i]), ++n;
  }
  return all ? n : -1;
}

// ---- the console and the web
namespace {
CORE_COLD const char* state_text(const Entry& e) {
  if (e.type == kOff) return "OFF";
  if (e.type == kEmpty) return "EMPTY";
  if (e.type == kInput) return "INPUT";
  if (e.type == kBuiltin) return "BUILT-IN";
  return lib::state_name(e.st);
}
// appends, never past cap (sniprintf's return is the length it wanted)
CORE_COLD void app(char* out, int cap, int* n, const char* f, ...) {
  if (*n >= cap - 1) return;
  va_list ap;
  va_start(ap, f);
  int k = vsniprintf(out + *n, (size_t)(cap - *n), f, ap);
  va_end(ap);
  if (k > 0) *n += k < cap - *n ? k : cap - 1 - *n;
}
CORE_COLD void json_str(char* out, int cap, int* n, const char* s) {
  if (*n < cap) out[(*n)++] = '"';
  for (; *s && *n < cap - 2; ++s) {
    char c = *s;
    if (c == '"' || c == '\\') out[(*n)++] = '\\', out[(*n)++] = c;
    else if ((uint8_t)c < 0x20) out[(*n)++] = ' ';
    else out[(*n)++] = c;
  }
  if (*n < cap) out[(*n)++] = '"';
}
}  // namespace

CORE_COLD int bank_json(int l, int bank, char* out, int cap) {
  if (!began || l < 0 || l >= kLists || bank < 0 || bank >= kBanks || cap < 64) return 0;
  int n = 0;
  app(out, cap, &n, "{\"list\":\"%s\",\"bank\":%d,\"locked\":%s,\"ro\":%s,\"count\":%d,\"rows\":[", list_tag(l),
      bank, L[l].locked ? "true" : "false", L[l].ro ? "true" : "false", count(l));
  bool first = true;
  for (int r = 1; r < kRows && n < cap - 600; ++r) {
    const Entry& e = L[l].e[code(bank, r)];
    if (e.type == kEmpty) continue;
    app(out, cap, &n, "%s[%d,\"%s\",", first ? "" : ",", r,
        e.type == kSample ? "sample" : e.type == kInput ? "input" : e.type == kBuiltin ? "builtin" : "instrument");
    first = false;
    json_str(out, cap, &n, e.name);
    app(out, cap, &n, ",\"%s\",", state_text(e));
    json_str(out, cap, &n, has_file(e) ? (e.path[0] ? e.path : e.ref.hint) : "");
    app(out, cap, &n, ",%lu,%d,%lu,%d,%d,%d]", (unsigned long)used_count(l, code(bank, r)), users(l, code(bank, r)),
        (unsigned long)(has_file(e) ? e.ref.size : 0), (int)e.nm, (int)e.ch, always_ram(l, code(bank, r)) ? 1 : 0);
  }
  app(out, cap, &n, "]}");
  return n < cap - 1 ? n : 0;
}
CORE_COLD int status_json(char* out, int cap) {
  if (!began) return sniprintf(out, cap, "{}");
  int n = 0;
  app(out, cap, &n, "{\"project\":");
  json_str(out, cap, &n, project());
  app(out, cap, &n, ",\"ready\":%s", g_ready ? "true" : "false");
  for (int l = 0; l < kLists; ++l) {
    app(out, cap, &n, ",\"%s\":{\"n\":%d,\"locked\":%s,\"ro\":%s,\"why\":", list_tag(l), count(l),
        L[l].locked ? "true" : "false", L[l].ro ? "true" : "false");
    json_str(out, cap, &n, L[l].why);
    app(out, cap, &n, "}");
  }
  app(out, cap, &n, "}");
  return n < cap - 1 ? n : 0;
}

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "pool", 4) != 0 || (p[4] && p[4] != '=')) return false;
  ensure();
  const char* a = p[4] ? p + 5 : "";
  if (!*a || !strcmp(a, "status")) {
    sniprintf(out, cap, "pool: project %s%s; samples %d, wavetables %d, instruments %d; %u reads, %u saves (%u failed), "
                        "%u templates, %u fills%s%s",
              project()[0] ? project() : "-", g_ready ? "" : " (loading)", count(kSamples), count(kTables), count(kInstr),
              (unsigned)st.reads, (unsigned)st.saves, (unsigned)st.save_fails, (unsigned)st.templates, (unsigned)st.fills,
              L[kSamples].locked || L[kTables].locked || L[kInstr].locked ? "; LOCKED: " : "",
              L[kSamples].locked ? L[kSamples].why : L[kTables].locked ? L[kTables].why : L[kInstr].locked ? L[kInstr].why : "");
    return true;
  }
  char tag[8];
  const char* c1 = strchr(a, ':');
  int tl = c1 ? (int)(c1 - a) : (int)strlen(a);
  if (tl >= (int)sizeof tag) tl = (int)sizeof tag - 1;
  memcpy(tag, a, tl);
  tag[tl] = 0;
  int l = list_of_tag(tag);
  if (l < 0 || !c1) {
    sniprintf(out, cap, "pool: pool=smp|wt|inst:set:A010:PATH|replace:A010:PATH[:yes|:force]|add:B:PATH|clear:A010|"
                        "unload:A010[,..]|fill:B:FOLDER|input:H001:IN|builtin:H011:PAD|assign:A010:I|accept:A010|shown:B|"
                        "unused|list[:B]|undo");
    return true;
  }
  const char* r = c1 + 1;
  auto res = [&](int code_, int rr, const char* what) {
    char row[5] = "-";
    if (code_ >= 0) row_text(code_, row);
    sniprintf(out, cap, "pool: %s %s %s: %s", list_tag(l), row, what, result_text(rr));
  };
  int c = -1;
  if (!strncmp(r, "set:", 4) || !strncmp(r, "replace:", 8)) {
    const char* q = strchr(r, ':') + 1;
    const char* e;
    if (!parse_row(q, &c, &e) || *e != ':') return sniprintf(out, cap, "pool: a row A000..H127"), true;
    char path[lib::kPath + 8];
    sniprintf(path, sizeof path, "%s", e + 1);
    int conf = kAsk;
    int pl = (int)strlen(path);
    if (pl > 4 && !strcmp(path + pl - 4, ":yes")) path[pl - 4] = 0, conf = kConfirmed;
    else if (pl > 6 && !strcmp(path + pl - 6, ":force")) path[pl - 6] = 0, conf = kForced;
    int o = -1;
    int rr = set_file(l, c, path, &o, conf);
    res(rr == kExisting ? o : c, rr, path);
    return true;
  }
  if (!strncmp(r, "add:", 4)) {
    const char* q = r + 4;
    int b = *q >= 'A' && *q <= 'H' ? *q - 'A' : *q >= 'a' && *q <= 'h' ? *q - 'a' : -1;
    if (b < 0 || q[1] != ':') return sniprintf(out, cap, "pool: a bank A..H"), true;
    int o = -1;
    int rr = add_file(l, b, q + 2, &o);
    res(o, rr, q + 2);
    return true;
  }
  if (!strncmp(r, "clear:", 6) || !strncmp(r, "unload:", 7)) {
    const char* q = strchr(r, ':') + 1;
    int done = 0, refused = 0, last = kOk;
    for (;;) {
      const char* e;
      if (!parse_row(q, &c, &e)) break;
      bool yes = !strncmp(e, ":yes", 4);
      int rr = unload(l, c, yes ? kConfirmed : kAsk);
      if (rr == kOk) ++done;
      else ++refused, last = rr;
      q = yes ? e + 4 : e;
      if (*q != ',') break;
      ++q;
    }
    sniprintf(out, cap, "pool: %s: %d unloaded%s%s", list_tag(l), done, refused ? ", refused: " : "", refused ? result_text(last) : "");
    return true;
  }
  if (!strncmp(r, "input:", 6) || !strncmp(r, "builtin:", 8)) {
    bool in = r[0] == 'i';
    const char* q = strchr(r, ':') + 1;
    const char* e;
    if (!parse_row(q, &c, &e) || *e != ':') return sniprintf(out, cap, "pool: a row A000..H127"), true;
    int id = in ? input_of(e + 1) : builtin_of(l, e + 1);
    int rr = id < 0 ? (int)kErrType : set_special(l, c, in ? kInput : kBuiltin, id);
    res(c, rr, e + 1);
    return true;
  }
  if (!strncmp(r, "fill:", 5)) {
    const char* q = r + 5;
    int b = *q >= 'A' && *q <= 'H' ? *q - 'A' : *q >= 'a' && *q <= 'h' ? *q - 'a' : -1;
    if (b < 0 || q[1] != ':') return sniprintf(out, cap, "pool: a bank A..H"), true;
    int rr = fill(l, b, q + 2);
    sniprintf(out, cap, rr < 0 ? "pool: %s bank %c: cannot fill from %s" : "pool: %s bank %c: filling from %s",
              list_tag(l), 'A' + b, q + 2);
    return true;
  }
  if (!strcmp(r, "unused")) {
    static int16_t un[kCodes];
    int n = select_unused(l, un, kCodes);
    int k = sniprintf(out, cap, "pool: %s: %d unused (%s):", list_tag(l), n, kUnusedWarning);
    for (int i = 0; i < n && k < cap - 8; ++i) {
      char row[5];
      row_text(un[i], row);
      k += sniprintf(out + k, cap - k, " %s", row);
    }
    return true;
  }
  if (!strncmp(r, "assign:", 7)) {   // assign:ROW:I[:yes] - item I of the list's browser (srclib; the touchscreen's FILES)
    const char* e;
    if (!parse_row(r + 7, &c, &e) || *e != ':') return sniprintf(out, cap, "pool: assign:ROW:ITEM"), true;
    int i = 0;
    const char* q = e + 1;
    for (; *q >= '0' && *q <= '9'; ++q) i = i * 10 + (*q - '0');
    int conf = !strcmp(q, ":yes") ? kConfirmed : !strcmp(q, ":force") ? kForced : kAsk;
    char path[lib::kPath];
    bool dir = false;
    int type = 0, id = 0, o = -1, rr;
    if (!srclib::item(l, i, nullptr, &dir, nullptr) || dir || !srclib::item_path(l, i, path, sizeof path)) rr = kErrPath;
    else if (srclib::special(l, path, &type, &id)) rr = set_special(l, c, type, id, conf);
    else rr = set_file(l, c, path, &o, conf);
    res(rr == kExisting ? o : c, rr, "item");
    return true;
  }
  if (!strncmp(r, "accept:", 7)) {   // a CHANGED row takes its file's new content
    if (!parse_row(r + 7, &c)) return sniprintf(out, cap, "pool: accept:ROW"), true;
    accept_changed(l, c);
    res(c, kOk, lib::state_name(L[l].e[c].st));
    return true;
  }
  if (!strncmp(r, "ram:", 4)) {   // ram:ROW|all:on|off - always in RAM (docs/STEREO.md)
    const char* q = r + 4;
    const char* e = q;
    bool all = !strncmp(q, "all", 3);
    if (all) e = q + 3;
    else if (!parse_row(q, &c, &e)) return sniprintf(out, cap, "pool: ram:ROW|all:on|off"), true;
    bool on = !strcmp(e, ":on") || !strcmp(e, ":1");
    if (!on && strcmp(e, ":off") && strcmp(e, ":0")) return sniprintf(out, cap, "pool: ram:ROW|all:on|off"), true;
    set_ram(l, all ? -1 : c, on);
    if (all) sniprintf(out, cap, "pool: %s: every row %s", list_tag(l), on ? "always in RAM" : "streamed as the budget allows");
    else res(c, kOk, on ? "always in RAM" : "streamed as the budget allows");
    return true;
  }
  if (!strncmp(r, "shown:", 6)) {   // the bank a view shows (takes go there)
    char b = r[6];
    int bk = b >= 'A' && b <= 'H' ? b - 'A' : b >= 'a' && b <= 'h' ? b - 'a' : -1;
    if (bk >= 0) shown(l, bk);
    sniprintf(out, cap, "pool: %s: bank %c shown", list_tag(l), 'A' + shown_bank(l));
    return true;
  }
  if (!strcmp(r, "undo")) {
    sniprintf(out, cap, undo(l) ? "pool: %s: undone" : "pool: %s: nothing to undo", list_tag(l));
    return true;
  }
  if (!strncmp(r, "list", 4)) {
    int b0 = 0, b1 = kBanks;
    if (r[4] == ':' && r[5] >= 'A' && r[5] <= 'H') b0 = r[5] - 'A', b1 = b0 + 1;
    int n = 0;
    for (int b = b0; b < b1; ++b)
      for (int rr = 1; rr < kRows; ++rr) {
        const Entry& e = L[l].e[code(b, rr)];
        if (e.type == kEmpty) continue;
        char row[5];
        row_text(code(b, rr), row);
        log(CORE_STR("pool: %s %s %s %s %s"), list_tag(l), row, state_text(e), e.name, has_file(e) ? e.path : "");
        ++n;
      }
    sniprintf(out, cap, "pool: %s: %d rows (the list in the log)", list_tag(l), n);
    return true;
  }
  sniprintf(out, cap, "pool: %s: set|replace|add|clear|unload|fill|input|builtin|unused|list|undo", list_tag(l));
  return true;
}

CORE_COLD Ram ram() {
  Ram r = {};
  pcache::Stats c = pcache::stats();
  uint64_t cap = 0;
  r.tables = wt::ram_bytes(&cap);
  r.budget = c.budget + cap;   // (the tables' memory is set aside at boot: counted in the budget, shown apart)
  r.used = c.used;
  r.entries = c.ready;
  r.pct = r.budget ? (uint8_t)((r.used + r.tables) * 100 / r.budget) : 0;
  return r;
}
CORE_COLD bool resident(int l, int c) {
  if (!began || l < 0 || l >= kLists || c < 0 || c >= kCodes) return false;
  const Entry& e = L[l].e[c];
  switch (l) {
    case kSamples:
      if (e.type == kInput || e.type == kBuiltin) return true;
      return e.type == kSample && pcache::find(e.ref, pcache::kWhole) >= 0;
    case kTables: return e.type == kBuiltin || (e.type == kSample && wt::entry_state(c) == 3);
    default: return e.type == kInstrument && msamp::entry_state(c) == 3;
  }
}

const Stats& stats() { return st; }
CORE_COLD void compact_for_tests(int l) {
  if (began && l >= 0 && l < kLists && L[l].m) {
    int keep = L[l].nm;
    L[l].nm = kMembers;   // (as if full: member_room compacts)
    (void)keep;
    member_room(l, 1);
  }
}
CORE_COLD void reset_for_tests() {
  ensure();
  for (int l = 0; l < kLists; ++l) clear_list(l);
  cur_key[0] = 1, cur_key[1] = 0;
  g_ready = false, load_next = -1;
  J.io = kIoIdle;
  SJ.on = FJ.on = false;
}

}  // namespace pools
}  // namespace core
#endif
