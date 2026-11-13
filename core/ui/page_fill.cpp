// core/ui/page_fill.cpp - see extra.h: the FILL page on the Rytm's screen (0.5.0, the owner: "p-lock all the trigs of
// that track with those samples, in order or at random, like the fill functions in Polyend Tracker"; notes/design
// sampler-lock-fill, OWNER DECISIONS). Cold. CORE_POLY builds only. Opened by UP + RIGHT on the Rytm's stock screen
// (49 1 1 16, the 0.5.0 0026 with hello caps2 bit 0 and the host's claim) and by the SIDECAR MENU's FILL row; UP +
// RIGHT again (UP then RIGHT, both the page's) or NO closes it. Off the LEFT / RIGHT ring. Each label on the page
// starts with its encoder's letter ("A TGT SAMPLE", "E TRG ALL"; the owner's test, 2026-11-13).
//
//   A TARGET  the track's targets (core/seq/filltargets.h): the machine's main one first, VELOCITY last
//   B FROM    a value of the target (a pool row, an engine, a preset, 1..127); FUNC: a bank (pool) / 16 at a time
//   C TO      idem; below FROM fills downwards
//   D ORDER   UP DOWN PING RND SHUF SPRD (lockfill.h)
//   E TRIGS   ALL / EMPTY (no value of the target yet) / LOCKED (only those)
//   F EVERY   1..16, counted over the trigs TRIGS leaves; G OFFSET 0..EVERY-1
//   H SEED    0..999 (RND, SHUF): new each time the page opens
//   LEFT / RIGHT  the target before / after (as A)
//   YES fills, FUNC + YES clears the target on those trigs, FUNC + NO undoes the last fill or clear (one level; gone
//   after a pattern change or a project load; refused once the filled target changed since: CHANGED SINCE), NO
//   leaves. docs/FILL.md.
// Defaults (owner): the machine's main target, ORDER UP (VELOCITY RND), TRIGS ALL, empty / OFF rows skipped, a fresh
// seed. SAMPLE's FROM / TO: the track's last quick-load batch (RIGHT in its list), else the sample the kit plays.
// The values become the Rytm's own p-locks in its working pattern (core/seq/patwrite.h: read, patch, write, verify);
// a fill while the Rytm plays answers STOP TO FILL until emulator check E4 allows it (fill=playing:on).
#if CORE_POLY
#include <stdio.h>
#include <string.h>

#include "lib/srclib.h"
#include "machines.h"
#include "seq/filltargets.h"
#include "seq/lockfill.h"
#include "seq/patwrite.h"
#include "seq/rytm_dump.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"
#include "pool/pool.h"
#if CORE_MSAMP
#include "lib/pools.h"
#include "pool/pcache.h"
#endif

namespace core {
namespace ui {

namespace {

namespace ft = filltargets;
namespace lf = lockfill;
enum : int { kLeft = 0x40, kRight = 0x41, kUp = 0x42, kDown = 0x43, kYes = 0x44, kNo = 0x45, kFunc = 0x80 };
enum Job : uint8_t { kJobNone = 0, kJobFill, kJobClear, kJobUndo };
constexpr int kRowY = 11, kRowH = 7, kColX = 66;
constexpr uint32_t kNoteMs = 2500;
constexpr int kSetMax = 1024;

struct FillPage {
  bool on = false;
  int track = -1;
  ft::Def defs[ft::kMax];
  int nd = 0, cur = 0;
  int from[ft::kMax] = {}, to[ft::kMax] = {};
  uint8_t order[ft::kMax] = {};
  lf::Options o;
  bool func = false, up = false;
  char note[28] = "";
  bool note_new = false;
  uint32_t note_ms = 0;
  bool note_stick = false;        // a job's result: on the footer until the next key (the Rytm's popup may cover it)
  // the job under way and what it leaves
  uint8_t job = kJobNone;
  uint32_t jobs_seen = 0;
  int filled = 0;                 // the trigs the job wrote
  lf::Saved pending, undo;        // the target as it was before the job's patch; the undo a done job leaves
  lf::Saved pafter, after;        // the target as the patch left it: the undo refuses once it changed (CHANGED SINCE)
  int undo_pattern = -1;          // the pattern the job read (patwrite::read_pattern): another active pattern ends the undo
  uint32_t jproject = 0, undo_project = 0;   // core::project_gen() at the job's start: a project load ends it too
  // the job's own copy of what it writes (the page may change while the job runs)
  ft::Def jd;
  int jtrack = -1, jfrom = 0, jto = 0;
  lf::Options jo;
};
FillPage P;
uint16_t g_set[kSetMax], g_seq[lf::kSteps];

CORE_COLD void note(const char* t) {
  sniprintf(P.note, sizeof P.note, "%s", t);
  P.note_new = true;
}
CORE_COLD ft::Def* def() { return P.nd ? &P.defs[P.cur] : nullptr; }

// ---- the patches (core/seq/patwrite.h PatchFn), on the record as read
CORE_COLD int patch_fill(uint8_t* rec, void*) {
  uint8_t steps[lf::kSteps];
  int n = lf::steps_for(rec, P.jtrack, P.jd.g, P.jo, steps);
  if (!n) return lf::kNoTrigs;
  int nv = ft::value_set(P.jd, P.jfrom, P.jto, g_set, kSetMax);
  if (!nv) return lf::kNoValues;
  lf::sequence(g_set, nv, n, P.jo, g_seq);
  lf::save(rec, P.jtrack, P.jd.g, &P.pending);
  int r = lf::fill(rec, P.jtrack, P.jd.g, steps, g_seq, n);
  lf::save(rec, P.jtrack, P.jd.g, &P.pafter);
  P.filled = n;
  return r == lf::kOk ? 0 : r;
}
CORE_COLD int patch_clear(uint8_t* rec, void*) {
  uint8_t steps[lf::kSteps];
  int n = lf::steps_for(rec, P.jtrack, P.jd.g, P.jo, steps);
  if (!n) return lf::kNoTrigs;
  lf::save(rec, P.jtrack, P.jd.g, &P.pending);
  int r = lf::clear(rec, P.jtrack, P.jd.g, steps, n);
  lf::save(rec, P.jtrack, P.jd.g, &P.pafter);
  P.filled = n;
  return r == lf::kOk ? 0 : r;
}
CORE_COLD int patch_undo(uint8_t* rec, void*) {
  // N1 (re-review): the read may have restarted on another pattern (a switch before its answer) or after a project load:
  // bound to the fill's own pattern and project, not to the content alone (a copy of the pattern holds the same)
  // (owner 2026-11-13: by the pattern's number, not the switch count: a Rytm hello's resync to the same pattern counted
  // as a switch and expired the undo right after a fill; the content check below guards a changed track)
  if (patwrite::read_pattern() != P.undo_pattern || project_gen() != P.undo_project) return lf::kChanged;
  lf::Saved now;   // M2 (fix round): the target edited since the fill (on the Rytm, by a take): not put back over that
  lf::save(rec, P.undo.track, P.undo.g, &now);
  if (!lf::equal(now, P.after)) return lf::kChanged;
  int r = lf::restore(rec, P.undo);
  return r == lf::kOk ? 0 : r;
}

// a job's end (patwrite's job count moved): what it leaves
CORE_COLD void poll() {
  if (!P.job || patwrite::busy() || patwrite::jobs() == P.jobs_seen) return;
  P.jobs_seen = patwrite::jobs();
  const uint8_t job = P.job;
  P.job = kJobNone;
  P.note_stick = true;
  char b[28];
  if (patwrite::state() != patwrite::kDone) {
    int w = patwrite::why();
    note(w == patwrite::kWhyPatch ? lf::result_text(patwrite::patch_result()) : patwrite::why_text(w));
    if (job == kJobUndo && w == patwrite::kWhyPatch && patwrite::patch_result() == lf::kChanged) P.undo = lf::Saved();
    return;
  }
  if (job == kJobUndo) {
    P.undo = lf::Saved();
    note("UNDONE");
    return;
  }
  if (!patwrite::changed()) { note(job == kJobFill ? "NO CHANGE" : "NOTHING TO CLEAR"); return; }
  P.undo = P.pending, P.after = P.pafter, P.undo_pattern = patwrite::read_pattern(), P.undo_project = P.jproject;
  sniprintf(b, sizeof b, "%s %d: FN+NO UNDO", job == kJobFill ? "FILLED" : "CLEARED", P.filled);
  note(b);
#if CORE_MSAMP
  if (job == kJobFill && P.jd.values == ft::kPool && P.jd.arg == pools::kSamples) {   // the filled rows wanted at once
    for (int i = 0, nv = ft::value_set(P.jd, P.jfrom, P.jto, g_set, kSetMax); i < nv; ++i) {
      int code = ft::index_of(P.jd, g_set[i]);
      const pools::Entry& e = pools::at(pools::kSamples, code);
      if (pools::has_file(e) && e.path[0]) pcache::want(e.ref, e.path, true);
    }
  }
#endif
}

CORE_COLD bool begin(uint8_t job) {
  poll();
  if (patwrite::busy()) { note("A FILL IS RUNNING"); return false; }
  if (P.track < 0) { note("SELECT A TRACK FIRST"); return false; }
  if (job == kJobUndo) {
    if (!P.undo.valid) { note("NOTHING TO UNDO"); return false; }
    int ap = rytm_dump::active_pattern();
    if (ap >= 0 && ap != P.undo_pattern) { P.undo = lf::Saved(); note("UNDO EXPIRED (PATTERN)"); return false; }
    if (project_gen() != P.undo_project) { P.undo = lf::Saved(); note("UNDO EXPIRED (PROJECT)"); return false; }
  } else if (!def()) {
    return false;
  }
  if (job != kJobUndo) {
    P.jd = *def(), P.jtrack = P.track, P.jfrom = P.from[P.cur], P.jto = P.to[P.cur], P.jo = P.o;
    P.jo.order = P.order[P.cur];
  }
  P.filled = 0, P.jproject = project_gen();
  patwrite::PatchFn fn = job == kJobFill ? patch_fill : job == kJobClear ? patch_clear : patch_undo;
  if (!patwrite::start(fn, nullptr)) { note(patwrite::why_text(patwrite::why())); return false; }
  P.job = job, P.jobs_seen = patwrite::jobs();
  note(job == kJobFill ? "FILLING.." : job == kJobClear ? "CLEARING.." : "UNDOING..");
  return true;
}

// FROM / TO defaults for target k
CORE_COLD void defaults(int k) {
  const ft::Def& d = P.defs[k];
  int first = ft::step(d, -1, 1), last = ft::step(d, d.n, -1);
  if (first < 0 || first >= d.n) first = 0;
  if (last < 0 || last >= d.n) last = d.n - 1;
  P.from[k] = first, P.to[k] = last, P.order[k] = d.order;
  if (d.values == ft::kRange) {   // VELOCITY: a humanised 60..127
    P.from[k] = 60 - d.lo, P.to[k] = d.n - 1;
    return;
  }
  if (d.values != ft::kPool) return;
  int a = -1, b = -1;
  if (d.n > 128 && fill_batch(P.track, d.arg, &a, &b)) {   // the last quick-load batch
    P.from[k] = a, P.to[k] = b;
    return;
  }
  int v = d.n > 128 ? remote_track_src14(P.track) : -1;   // the sample the kit plays
  int i = v >= 0 ? srclib::code_of14(v) : -1;
  if (i > 0 && i < d.n && !ft::skip(d, i)) P.from[k] = P.to[k] = i;
  else P.to[k] = P.from[k];
}

CORE_COLD void load_targets() {
  P.nd = P.track >= 0 ? ft::for_track(P.track, P.defs, ft::kMax) : 0;
  if (P.cur >= P.nd) P.cur = 0;
  for (int k = 0; k < P.nd; ++k) defaults(k);
}

CORE_COLD void fit_name(char* out, int cap, const ft::Def& d, int i, int room) {
  char nm[48];
  ft::name(d, i, nm, sizeof nm);
  name_fit(out, cap, nm, room);
}

CORE_COLD void fill_draw(Canvas& c, int page, int npages, uint32_t ms) {
  poll();
  c.clear();
  char t[24], r[16] = "";
  static const char* const kTrack[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
  sniprintf(t, sizeof t, "FILL %s", P.track >= 0 && P.track < 12 ? kTrack[P.track] : "-");
  int pat = -1;
  const uint8_t* rec = rytm_dump::record(&pat, nullptr);
  const ft::Def* d = def();
  if (rec && d && P.track >= 0) sniprintf(r, sizeof r, "LOCK TRK %d/72", lf::slots_after(rec, P.track, d->g));
  header(c, t, page, npages, r[0] ? r : nullptr);
  if (!d) {
    c.put_text(kSlotFree, kRSmall, 64, 30, P.track < 0 ? "SELECT A TRACK" : "NOTHING TO FILL", kAlignCentre);
    footer(c, "NO: BACK");
    return;
  }
  // the owner's test (2026-11-13): each label starts with its encoder's letter (A..D the left column, E..H the right, as
  // the Rytm's knobs). At kRSmall's widest 5 px the left column holds 13 characters (x 2..65, the right starts at 66),
  // the right 12: an 8-letter target ("VELOCITY") drops the TGT word ("A VELOCITY")
  char b[8][20];
  char nm[16];
  if (strlen(d->label) + 6 <= 13) sniprintf(b[0], 20, "A TGT %s", d->label);
  else sniprintf(b[0], 20, "A %.11s", d->label);
  fit_name(nm, sizeof nm, *d, P.from[P.cur], 8);
  sniprintf(b[1], 20, "B FR %s", nm);
  fit_name(nm, sizeof nm, *d, P.to[P.cur], 8);
  sniprintf(b[2], 20, "C TO %s", nm);
  sniprintf(b[3], 20, "D ORD %s", lf::order_name(P.order[P.cur]));
  sniprintf(b[4], 20, "E TRG %s", lf::trigs_name(P.o.trigs));
  sniprintf(b[5], 20, "F EVRY %d", P.o.every);
  sniprintf(b[6], 20, "G OFS %d", P.o.offset);
  sniprintf(b[7], 20, "H SEED %d", P.o.seed);
  for (int i = 0; i < 4; ++i) {
    c.put_text(kSlotFree + i, kRSmall, 2, kRowY + i * kRowH, b[i]);
    c.put_text(kSlotFree + 4 + i, kRSmall, kColX, kRowY + i * kRowH, b[4 + i]);
  }
  char line[32], full[40];   // FROM and TO by their whole names
  fit_name(line, sizeof line, *d, P.from[P.cur], 19);
  sniprintf(full, sizeof full, "FROM %s", line);
  c.put_text(kSlotFree + 8, kRSmall, 2, kRowY + 4 * kRowH, full);
  fit_name(line, sizeof line, *d, P.to[P.cur], 19);
  sniprintf(full, sizeof full, "TO   %s", line);
  c.put_text(kSlotFree + 9, kRSmall, 2, kRowY + 5 * kRowH, full);
  if (P.note_new || P.note_stick || (P.note_ms && ms - P.note_ms < kNoteMs) || P.job) footer(c, P.note);
  else footer(c, P.func ? "YES: CLEAR  NO: UNDO" : "YES: FILL  FN: CLR/UNDO");
  if (P.note_new) P.note_new = false, P.note_ms = ms | 1;
}

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
CORE_COLD void move_value(int* v, int l, bool func) {
  const ft::Def* d = def();
  if (!d || !l) return;
  int jump = func ? (d->values == ft::kPool && d->n > 128 ? 128 : 16) : 1;
  int dir = l > 0 ? 1 : -1, steps = (l > 0 ? l : -l) * jump;
  if (func && d->values == ft::kPool && d->n > 128) {   // a bank: the same row in the next bank, else its first row
    int to = clampi(*v + dir * 128, 0, d->n - 1);
    *v = ft::skip(*d, to) ? ft::step(*d, to, dir) : to;
    if (ft::skip(*d, *v)) *v = ft::step(*d, *v, -dir);
    return;
  }
  for (int k = 0; k < steps; ++k) *v = ft::step(*d, *v, dir);
}
CORE_COLD void set_target(int k) {
  if (!P.nd) return;
  P.cur = clampi(k, 0, P.nd - 1);
}
CORE_COLD void fill_enc(int e, int delta, bool func) {
  if (!delta) return;
  const int l = e == 1 || e == 2 ? list_turn_long(e, delta) : list_turn(e, delta);
  if (!l) return;
  switch (e) {
    case 0: set_target(P.cur + l); break;
    case 1: move_value(&P.from[P.cur], l, func || P.func); break;
    case 2: move_value(&P.to[P.cur], l, func || P.func); break;
    case 3: P.order[P.cur] = (uint8_t)clampi(P.order[P.cur] + l, 0, lf::kOrders - 1); break;
    case 4: P.o.trigs = (uint8_t)clampi(P.o.trigs + l, 0, lf::kTrigsModes - 1); break;
    case 5:
      P.o.every = (uint8_t)clampi(P.o.every + l, 1, 16);
      if (P.o.offset >= P.o.every) P.o.offset = (uint8_t)(P.o.every - 1);
      break;
    case 6: P.o.offset = (uint8_t)clampi(P.o.offset + l, 0, P.o.every - 1); break;
    case 7: P.o.seed = (uint16_t)((P.o.seed + l * ((func || P.func) ? 100 : 1) + 1000) % 1000); break;
    default: break;
  }
}

CORE_COLD void close_page() {
  P.on = false, P.up = false, P.func = false;
  remote_request(false);
}

CORE_COLD bool fill_key(int code) {
  P.note_stick = false;
  poll();
  if (code == kFunc) { P.func = true; return true; }
  if (code == kUp) { P.up = true; return true; }
  if (code == kDown) return true;
  if (code == kRight && P.up) { close_page(); return true; }   // UP + RIGHT again: the page closes (the combo toggles)
  if (code == kLeft || code == kRight) { set_target(P.cur + (code == kRight ? 1 : -1)); return true; }
  if (code == kYes) {
    begin(P.func ? kJobClear : kJobFill);
    return true;
  }
  if (code == kNo) {
    if (P.func) { begin(kJobUndo); return true; }
    P.on = false, P.up = false;
    return false;   // (remote.cpp: NO leaves - a shortcut's page closes, from the MENU back to SIDECAR)
  }
  return false;
}
CORE_COLD bool fill_key_up(int code) {
  if (code == kFunc) P.func = false;
  else if (code == kUp) P.up = false;
  return true;
}
CORE_COLD bool fill_shown() { return P.on; }

}  // namespace

UI_ROM const ExtraPage kFillPage = {"FILL", fill_shown, fill_draw, fill_enc, fill_key, 0xFF, false, nullptr, nullptr,
                                    nullptr, true, fill_key_up, nullptr, true};

CORE_COLD void fill_page_open(int track) {
  int t = track >= 0 ? track : remote_selected_track(nullptr);
  bool same = P.on && t == P.track;
  if (!same) {
    uint32_t u = now_ms() * 2654435761u;
    P.track = t >= 0 && t < 12 ? t : -1, P.cur = 0;
    P.o = lf::Options();
    P.o.seed = (uint16_t)((u >> 8) % 1000);   // a fresh seed each open (owner)
    load_targets();
  }
  P.on = true, P.func = false, P.up = false, P.note_stick = false;   // (an old result not shown again)
  remote_show_page("FILL");
}
CORE_COLD void fill_page_reset() {
  P = FillPage();
}
CORE_COLD FillView fill_page_view() {
  poll();   // a job's end seen (as a draw or a key would)
  FillView v = {};
  v.on = P.on, v.track = P.track, v.target = P.cur, v.ntargets = P.nd, v.from = P.nd ? P.from[P.cur] : -1, v.to = P.nd ? P.to[P.cur] : -1;
  v.order = P.nd ? P.order[P.cur] : 0, v.trigs = P.o.trigs, v.every = P.o.every, v.offset = P.o.offset, v.seed = P.o.seed;
  v.undo = P.undo.valid, v.note = P.note;
  return v;
}

CORE_COLD bool fill_command(const char* p, char* out, int cap) {
  if (strncmp(p, "fill", 4) || (p[4] && p[4] != '=')) return false;
  poll();
  const char* a = p[4] ? p + 5 : "";
  if (!strcmp(a, "yes")) begin(kJobFill);
  else if (!strcmp(a, "clear")) begin(kJobClear);
  else if (!strcmp(a, "undo")) begin(kJobUndo);
  else if (!strcmp(a, "playing:on")) patwrite::set_play_ok(true);
  else if (!strcmp(a, "playing:off")) patwrite::set_play_ok(false);
  else if (!strcmp(a, "keys:on")) remote_fill_keys(true);     // UP + RIGHT claimed on the Rytm (the default)
  else if (!strcmp(a, "keys:off")) remote_fill_keys(false);   // the claim dropped: UP + RIGHT stock's (4A 7E 0)
  else if (!strncmp(a, "open", 4)) fill_page_open(a[4] == ':' && a[5] >= '0' && a[5] <= '9' ? (a[6] >= '0' && a[6] <= '9' ? (a[5] - '0') * 10 + a[6] - '0' : a[5] - '0') : -1);
  else if (a[0]) return sniprintf(out, cap, "fill | fill=open[:T]|yes|clear|undo|playing:on|off|keys:on|off"), true;
  const ft::Def* d = def();
  const patwrite::Stats& st = patwrite::stats();
  sniprintf(out, cap, "fill: track %d target %s from %d to %d order %s trigs %s every %d offset %d seed %d%s, playing %s; "
                      "writes %lu verifies %lu restarts %lu mismatches %lu; %s",
            P.track, d ? d->label : "-", P.nd ? P.from[P.cur] : -1, P.nd ? P.to[P.cur] : -1, lf::order_name(P.nd ? P.order[P.cur] : 0),
            lf::trigs_name(P.o.trigs), P.o.every, P.o.offset, P.o.seed, P.undo.valid ? ", undo" : "",
            patwrite::play_ok() ? "allowed" : "STOP TO FILL", (unsigned long)st.writes, (unsigned long)st.verifies,
            (unsigned long)st.restarts, (unsigned long)st.mismatches, P.note[0] ? P.note : "-");
  return true;
}

}  // namespace ui
}  // namespace core
#endif
