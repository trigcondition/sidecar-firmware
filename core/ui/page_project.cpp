// core/ui/page_project.cpp - see extra.h: the PROJECT page (project bundles, docs/BUNDLES.md "PROJECT pages") on the
// Rytm's screen, after SONG. Pi and Mac only (CORE_POLY && CORE_MSAMP). Cold.
//
// Everything it shows comes from core 0's published feed (core/bundle/feed.h); everything it does is a command line
// posted to core 0 (feed::post) - the control core never touches the card or a stick.
//
//   the list    the card's projects (the Rytm's current one marked *, imported ones +), the cursor on encoder A or
//               UP / DOWN; the destination / source (encoder H: CARD, or a mounted USB stick) with its free space.
//               T1 EXPORT the project there (YES / NO), T2 IMPORT (the bundles there: pick one, YES checks it),
//               T3 INFO, T4 DELETE (YES, then YES again; never the current project), T5 EJECT the stick.
//   a job       its phase, a bar over its bytes, MB done of MB, WAITING (PLAYING) while paused; NO cancels (not in
//               COMMIT). An import's summary (READY): the name it imports as, new / reused MB, the machines (T3 leaves
//               them out or puts them back), YES imports, NO cancels. Done: what happened and, after an import, the
//               name to save the Rytm project under. Failed: one line of what, one of what to do. YES / NO: back.
#if CORE_POLY && CORE_MSAMP
#include <string.h>

#include "bundle/feed.h"
#include "bundle/prjslot.h"
#include "bundle/rename.h"
#include "ui/extra.h"
#include "ui/page.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

using bundle::feed::Feed;
enum View : uint8_t { kVList, kVBundles, kVInfo, kVAskExport, kVAskDelete, kVSureDelete };
enum : int { kYes = 0x44, kNo = 0x45, kUp = 0x42, kDown = 0x43 };
constexpr int kRows = 5, kRowY = 11, kRowH = 7;

struct PageState {
  uint8_t view;
  int cur, top, bcur, btop;
  int dest;               // 0 the card, 1 + i the stick vol[i]
  char dest_drive[8];     // the stick picked (kept while sticks come and go)
  bool pk_off;            // READY: the bundle's machines left out
  char key[24];           // the project an ask is about
  char note[40];          // a one-line refusal ("THE CURRENT PROJECT: NOT DELETED")
  bool ejecting;          // T5 posted: the answer (F.eject_n moving) replaces "EJECTING..."
  uint32_t eject_n;
  uint32_t list_ms;       // when the page last asked for its source's list
};
PageState P;
Feed F, T;                // the last good copy (taken when the feed moved: by a draw, a key, a turn), a scratch
uint32_t F_at = ~0u;      // feed::published() when F was taken
CORE_COLD void fix_dest();
// F again when core 0 published since (a torn copy is dropped: F stays the last good one)
CORE_COLD void refresh() {
  uint32_t n = bundle::feed::published();
  if (n != F_at && bundle::feed::copy(&T)) F = T, F_at = n;
  fix_dest();
}

CORE_COLD void upper(char* s) {
  for (; *s; ++s)
    if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}
CORE_COLD void size_text(uint64_t b, char* out, int cap) {   // "12K" "412M" "1.2G"
  if (b >= (10ull << 30)) sniprintf(out, cap, "%uG", (unsigned)(b >> 30));
  else if (b >= (1ull << 30)) sniprintf(out, cap, "%u.%uG", (unsigned)(b >> 30), (unsigned)((b & ((1ull << 30) - 1)) * 10 >> 30));
  else if (b >= (1u << 20)) sniprintf(out, cap, "%uM", (unsigned)(b >> 20));
  else sniprintf(out, cap, "%uK", (unsigned)((b + 1023) >> 10));
}
CORE_COLD bool job_running(int p) {
  return !(p == bundle::kIdle || p == bundle::kDone || p == bundle::kFailed || p == bundle::kCancelled || p == bundle::kReady);
}
// the destination as F names it now (a stick that went: the card)
CORE_COLD void fix_dest() {
  if (P.dest > F.nvol) P.dest = 0;
  if (P.dest && strcmp(F.vol[P.dest - 1].drive, P.dest_drive)) {
    P.dest = 0;
    for (int i = 0; i < F.nvol; ++i)
      if (!strcmp(F.vol[i].drive, P.dest_drive)) P.dest = i + 1;
  }
  if (!P.dest) P.dest_drive[0] = 0;
}
CORE_COLD const char* dest_name() { return P.dest ? F.vol[P.dest - 1].name : "CARD"; }
// fix1049 (the hardware pass: "only USB offered, CARD missing"): with a stick the line names every destination H picks
// from, the one picked first ("H: CARD/USB" on the card, "H: CARD" on a stick: the way back), not only the others
CORE_COLD void dest_choices(char* out, int cap) {
  int n = sniprintf(out, cap, "%s", "CARD");
  for (int i = 0; i < F.nvol && n < cap; ++i) n += sniprintf(out + n, cap - n, "/%s", F.vol[i].name);
}
CORE_COLD void dest_token(char* out, int cap) { sniprintf(out, cap, "%s", P.dest ? F.vol[P.dest - 1].name : "card"); }
CORE_COLD void post(const char* fmt, const char* a = "", const char* b = "") {
  char l[200];
  sniprintf(l, sizeof l, fmt, a, b);
  bundle::feed::post(l);
}
CORE_COLD void ask_lists() {
  char t[8];
  dest_token(t, sizeof t);
  post("bundle=list:%s", t);
}
CORE_COLD void clamp(int* cur, int* top, int n) {
  if (*cur >= n) *cur = n - 1;
  if (*cur < 0) *cur = 0;
  if (*cur < *top) *top = *cur;
  if (*cur >= *top + kRows) *top = *cur - kRows + 1;
  if (*top < 0) *top = 0;
}
CORE_COLD void line(Canvas& c, int i, const char* s, bool inv = false, uint8_t align = kAlignLeft, int x = 1) {
  int y = kRowY + i * kRowH;
  if (inv) c.fill(Rect{0, (int16_t)(y - 1), 128, kRowH}, kOn);
  c.put_text(kSlotFree + i * 2, kRSmall, x, y, s, (uint8_t)(align | (inv ? kTextInvert : 0)));
}
CORE_COLD void line2(Canvas& c, int i, const char* right, bool inv) {   // a row's right part (its size)
  c.put_text(kSlotFree + i * 2 + 1, kRSmall, 126, kRowY + i * kRowH, right, (uint8_t)(kAlignRight | (inv ? kTextInvert : 0)));
}
// a long text over up to n lines of 25 characters (cut at spaces)
CORE_COLD int wrap(Canvas& c, int first, int n, const char* s) {
  char b[256];
  sniprintf(b, sizeof b, "%s", s);
  upper(b);
  const char* p = b;
  int i = 0;
  for (; *p && i < n; ++i) {
    int len = (int)strlen(p), k = len <= 25 ? len : 25;
    if (len > 25) {
      int sp = k;
      while (sp > 10 && p[sp] != ' ') --sp;
      if (p[sp] == ' ') k = sp;
    }
    char l[32];
    sniprintf(l, sizeof l, "%.*s", k, p);
    line(c, first + i, l);
    p += k;
    while (*p == ' ') ++p;
  }
  return i;
}

CORE_COLD void draw_progress(Canvas& c) {
  const bundle::Status& s = F.st;
  char b[48], d[16], t[16];
  sniprintf(b, sizeof b, "%s %s", s.job == bundle::kExport ? "EXPORT" : s.job == bundle::kImport ? "IMPORT" : "DELETE", s.key);
  upper(b);
  line(c, 0, b);
  if (s.phase == bundle::kServe) {
    line(c, 1, "A WEB DOWNLOAD RUNS");
    sniprintf(b, sizeof b, "%s SENT", (size_text(s.done, d, sizeof d), d));
    line(c, 2, b);
    footer(c, "NO: STOP IT");
    return;
  }
  sniprintf(b, sizeof b, "%s", bundle::phase_name(s.phase));
  line(c, 1, b);
  c.rect(Rect{0, 25, 128, 6}, kOn);
  if (s.total) {
    int w = (int)(s.done * 126 / s.total);
    if (w > 126) w = 126;
    if (w > 0) c.fill(Rect{1, 26, (int16_t)w, 4}, kOn);
  }
  if (s.total > 4096 && s.phase != bundle::kCommit && s.phase != bundle::kDelete) {
    size_text(s.done, d, sizeof d), size_text(s.total, t, sizeof t);
    sniprintf(b, sizeof b, "%s / %s", d, t);
  } else if (s.total) {
    sniprintf(b, sizeof b, "%u / %u", (unsigned)s.done, (unsigned)s.total);
  } else {
    b[0] = 0;
  }
  line(c, 3, b);
  if (s.waiting[0]) sniprintf(b, sizeof b, "WAITING (%s)", s.waiting), line(c, 4, b);
  bool can = s.phase != bundle::kCommit && s.phase != bundle::kLink && s.phase != bundle::kDelete;
  footer(c, can ? "NO: CANCEL" : "FINISHING...");
}

CORE_COLD void draw_ready(Canvas& c) {
  const bundle::PlanInfo& p = F.plan;
  char b[64], n[16], r[16];
  if (p.slot && p.pick) {   // every slot taken: no default - the web page picks one
    line(c, 0, "IMPORT: NO EMPTY SLOT");
    line(c, 2, "PICK THE SLOT ON THE");
    line(c, 3, "WEB PAGE (PROJECT TAB)");
    footer(c, "NO: BACK");
    return;
  }
  if (p.slot) sniprintf(b, sizeof b, "IMPORT TO SLOT %03d?", p.slot);   // slot mode (the plan's slot; others: the web page)
  else sniprintf(b, sizeof b, "IMPORT AS %s?", p.name);
  upper(b);
  line(c, 0, b);
  size_text(p.new_bytes, n, sizeof n), size_text(p.reused_bytes, r, sizeof r);
  sniprintf(b, sizeof b, "NEW %s  ON CARD %s", n, r);
  line(c, 1, b);
  int code = 0;
  for (int i = 0; i < p.nmach; ++i) code += p.mach[i].action == bundle::kPkgInstall || p.mach[i].action == bundle::kPkgSide;
  if (!p.nmach) sniprintf(b, sizeof b, "NO MACHINES IN IT");
  else if (P.pk_off) sniprintf(b, sizeof b, "MACHINES: %d LEFT OUT", p.nmach);
  else if (!code) sniprintf(b, sizeof b, "MACHINES: %d, ON THE CARD", p.nmach);
  else sniprintf(b, sizeof b, "MACHINES (CODE): %d NEW", code);
  line(c, 2, b);
  if (p.slot && p.clash) line(c, 3, "HAS FILES: USE WEB PAGE");
  else if (p.clash) sniprintf(b, sizeof b, "%s IS ON THE CARD", p.from), upper(b), line(c, 3, b);
  if (p.slot) sniprintf(b, sizeof b, p.rytm_file[0] ? "RYTM: TRANSFER TO %03d" : "RYTM: LOAD SLOT %03d", p.slot);
  else sniprintf(b, sizeof b, "RYTM: SAVE AS %s", p.name);
  upper(b);
  line(c, 4, b);
  footer(c, p.nmach ? "YES:IMP NO:BACK T3:MACH" : "YES: IMPORT   NO: BACK");
}

CORE_COLD void draw_end(Canvas& c) {
  const bundle::Status& s = F.st;
  if (s.phase == bundle::kDone && s.job == bundle::kImport && F.have_plan) {
    char b[48];
    if (F.plan.slot) {   // slot mode: the Sidecar's side is in the slot; the Rytm's project goes there
      sniprintf(b, sizeof b, "IMPORTED TO SLOT %03d", F.plan.slot);
      line(c, 0, b);
      line(c, 1, "ON THE RYTM:");
      wrap(c, 2, 3, F.plan.rytm_file[0] ? "send its file into that slot with Transfer, then load it"
                                        : "load that slot (the Rytm project itself must be in it)");
      footer(c, "YES / NO: BACK");
      return;
    }
    line(c, 0, "IMPORTED. ON THE RYTM:");
    line(c, 1, "SAVE THE RYTM PROJECT AS");
    sniprintf(b, sizeof b, "%s", F.plan.name);
    upper(b);
    line(c, 2, b, true, kAlignCentre, 64);
    wrap(c, 3, 2, F.plan.rytm_file[0] ? "(after sending its file with Transfer)" : "(load it on the Rytm first)");
  } else {
    line(c, 0, s.phase == bundle::kDone ? "DONE" : s.phase == bundle::kFailed ? "FAILED" : "CANCELLED");
    wrap(c, 1, 4, s.msg);
  }
  footer(c, "YES / NO: BACK");
}

// the bundles shown are this page's source's (the touchscreen may have asked for another one since)
CORE_COLD bool src_ok() { return !strcmp(F.src, P.dest ? F.vol[P.dest - 1].drive : ""); }
CORE_COLD void draw_list(Canvas& c, uint32_t ms) {
  char b[48], sz[16];
  if (P.ejecting && F.eject_n != P.eject_n) {
    P.ejecting = false;
    sniprintf(P.note, sizeof P.note, "%s", F.eject_msg);
    upper(P.note);
  }
  if (P.view == kVBundles) {
    if (!src_ok() && !F.reading && ms - P.list_ms > 1000) P.list_ms = ms, ask_lists();
    bool reading = F.reading || !src_ok();
    clamp(&P.bcur, &P.btop, F.nbund);
    if (reading) line(c, 0, "READING...");
    else if (!F.nbund) sniprintf(b, sizeof b, "NO BUNDLES ON %s", dest_name()), line(c, 0, b);
    for (int i = 0; i < kRows && P.btop + i < F.nbund && !reading; ++i) {
      const bundle::BundleRow& r = F.bund[P.btop + i];
      bool sel = P.btop + i == P.bcur;
      sniprintf(b, sizeof b, "%s", r.name);
      char* z = strstr(b, ".sidecar.zip");
      if (z) *z = 0;
      upper(b);
      if (strlen(b) > 19) b[19] = 0;
      line(c, i, b, sel);
      size_text(r.size, sz, sizeof sz);
      line2(c, i, r.incomplete ? "PART" : sz, sel);
    }
    footer(c, "YES: CHECK IT  NO: BACK");
    return;
  }
  clamp(&P.cur, &P.top, F.nproj);
  if (!F.nproj) line(c, 0, F.reading ? "READING..." : "NO PROJECTS ON THE CARD");
  for (int i = 0; i < kRows && P.top + i < F.nproj; ++i) {
    const bundle::ProjectRow& r = F.proj[P.top + i];
    bool sel = P.top + i == P.cur;
    sniprintf(b, sizeof b, "%s%s", r.rytm, r.current ? "*" : r.imported ? "+" : "");
    upper(b);
    if (strlen(b) > 19) b[19] = 0;
    line(c, i, b, sel);
    size_text(r.bytes, sz, sizeof sz);
    line2(c, i, sz, sel);
  }
  // the destination's line: its free space; a stick's eject
  uint64_t fr = P.dest ? F.vol[P.dest - 1].free : F.card_free;
  size_text(fr, sz, sizeof sz);
  if (P.note[0]) sniprintf(b, sizeof b, "%s", P.note);
  else if (P.dest) sniprintf(b, sizeof b, "%s %s H:CARD T5:EJECT", dest_name(), F.vol[P.dest - 1].free_known ? sz : "?");
  else if (F.nvol) {   // (the Rytm's line holds 25: more sticks than fit, the way to them named shorter)
    char ch[24];
    dest_choices(ch, sizeof ch);
    int n = sniprintf(b, sizeof b, "CARD %s FREE H:%s", fr ? sz : "?", ch);
    if (n > 25) sniprintf(b, sizeof b, "CARD %s FREE H:STICKS", fr ? sz : "?");
  } else sniprintf(b, sizeof b, "CARD %s FREE", fr ? sz : "?");
  c.put_text(kSlotFree + 12, kRSmall, 1, 47, b);
  footer(c, "T1 EXP T2 IMP T4 DEL");
}

CORE_COLD void draw_info(Canvas& c) {
  if (P.cur >= F.nproj) { P.view = kVList; return; }
  const bundle::ProjectRow& r = F.proj[P.cur];
  char b[48], sz[16];
  sniprintf(b, sizeof b, "%s", r.rytm);
  upper(b);
  line(c, 0, b);
  if (strcmp(r.key, r.rytm)) sniprintf(b, sizeof b, "CARD NAME %s", r.key), upper(b), line(c, 1, b);
  size_text(r.bytes, sz, sizeof sz);
  sniprintf(b, sizeof b, "%u SIDECAR FILES, %s", (unsigned)r.files, sz);
  line(c, 2, b);
  if (r.date[0]) sniprintf(b, sizeof b, "LAST USED %.16s", r.date);
  else sniprintf(b, sizeof b, r.used ? "LAST USED: #%u" : "NOT USED YET", (unsigned)r.used);
  char* t = r.date[0] ? strchr(b + 10, 'T') : nullptr;   // "2026-10-22T14:03"
  if (t) *t = ' ';
  line(c, 3, b);
  line(c, 4, r.current ? "THE RYTM'S CURRENT ONE" : r.imported ? "IMPORTED (RYTM: NOT YET)" : "");
  footer(c, "SAMPLES: SEE THE WEB PAGE");
}

// UX review B1 (core/bundle/rename.h): the Rytm named a project with no Sidecar files right after one with some
CORE_COLD void draw_rename(Canvas& c) {
  char b[48], n[32];
  line(c, 0, "RENAMED ON THE RYTM?");
  name_fit(n, sizeof n, rename::from(), 20);
  sniprintf(b, sizeof b, "FROM %s", n), upper(b);
  line(c, 1, b);
  name_fit(n, sizeof n, rename::to(), 20);
  sniprintf(b, sizeof b, "TO   %s", n), upper(b);
  line(c, 2, b, true);
  sniprintf(b, sizeof b, "%d SIDECAR FILE%s THERE:", rename::files(), rename::files() == 1 ? "" : "S");
  line(c, 3, b);
  line(c, 4, "YES MOVES THEM ACROSS");
  footer(c, "YES: MOVE  NO: NEW PROJ");
}

// 2026-10-27 (core/bundle/prjslot.h): a slot whose name changed without an event, or files from before slots
CORE_COLD void draw_prjslot(Canvas& c) {
  for (int i = 0; i < 5; ++i) line(c, i, prjslot::ask_line(i), i == 2);
  footer(c, prjslot::ask_footer());
}

CORE_COLD void project_draw(Canvas& c, int page, int npages, uint32_t ms) {
  refresh();
  c.clear();
  int ph = F.st.phase;
  char right[12];
  sniprintf(right, sizeof right, "%s", dest_name());
  header(c, "PROJECT", page, npages, right);
  if (rename::state() == rename::kAsk) return draw_rename(c);
  if (prjslot::ask()) return draw_prjslot(c);
  if (job_running(ph) || ph == bundle::kServe) return draw_progress(c);
  if (ph == bundle::kReady && F.have_plan) return draw_ready(c);
  if (ph == bundle::kDone || ph == bundle::kFailed || ph == bundle::kCancelled) return draw_end(c);
  char b[64], sz[16];
  switch (P.view) {
    case kVAskExport: {
      sniprintf(b, sizeof b, "EXPORT %s", P.key), upper(b);
      line(c, 0, b);
      uint64_t fr = P.dest ? F.vol[P.dest - 1].free : F.card_free;
      size_text(fr, sz, sizeof sz);
      sniprintf(b, sizeof b, "TO %s?  (%s FREE)", dest_name(), fr ? sz : "?");
      line(c, 1, b);
      line(c, 3, P.dest ? "INTO ITS /SIDECAR FOLDER" : "INTO /BUNDLES ON THE CARD");
      if (F.nvol) {   // fix1049: H picks the card or a stick here too
        char ch[24];
        dest_choices(ch, sizeof ch);
        sniprintf(b, sizeof b, "H: %s", ch);
        line(c, 4, b);
      }
      footer(c, "YES: EXPORT   NO: BACK");
      return;
    }
    case kVAskDelete:
    case kVSureDelete:
      sniprintf(b, sizeof b, "DELETE %s?", P.key), upper(b);
      line(c, 0, b);
      line(c, 1, "ITS SIDECAR FILES GO TO");
      line(c, 2, "THE TRASH, SAMPLES STAY");
      if (P.view == kVSureDelete) line(c, 4, "RESTORE: THE WEB PAGE", true, kAlignCentre, 64);
      footer(c, P.view == kVSureDelete ? "YES: DELETE   NO: BACK" : "YES: GO ON   NO: BACK");
      return;
    case kVInfo: return draw_info(c);
    default: return draw_list(c, ms);
  }
}

CORE_COLD bool project_shown() {   // (B1: its question; the slot reports' questions)
  return bundle::feed::available() || rename::state() == rename::kAsk || prjslot::ask();
}

CORE_COLD void move(int d) {
  if (P.view == kVBundles) P.bcur += d, clamp(&P.bcur, &P.btop, F.nbund);
  else if (P.view == kVList || P.view == kVInfo) P.cur += d, clamp(&P.cur, &P.top, F.nproj);
}

CORE_COLD void project_enc(int e, int delta, bool func) {
  refresh();   // (the state now: a job may have started since the last frame)
  if (job_running(F.st.phase) || F.st.phase == bundle::kReady) return;
  if (e == 0) move(delta * (func ? 5 : 1));
  if (e == 7 && delta) {   // H: the card or a stick
    int n = F.nvol + 1;
    P.dest = ((P.dest + (delta > 0 ? 1 : -1)) % n + n) % n;
    sniprintf(P.dest_drive, sizeof P.dest_drive, "%s", P.dest ? F.vol[P.dest - 1].drive : "");
    P.note[0] = 0;
    if (P.view == kVBundles) ask_lists();
  }
}

CORE_COLD bool project_key(int code) {
  refresh();
  if (rename::state() == rename::kAsk) {   // B1: YES moves the old name's files, NO starts the new name empty
    if (code == kYes || code == kNo) {
      rename::answer(code == kYes);
      sniprintf(P.note, sizeof P.note, "%s", rename::last());
      P.view = kVList;
    }
    return code == kYes || code == kNo || (code >= 0 && code < 16) || code == kUp || code == kDown;
  }
  if (prjslot::ask()) {   // 2026-10-27: keep / trash a slot's files, take / leave files from before slots
    if (code == kYes || code == kNo) {
      prjslot::answer(code == kYes);
      sniprintf(P.note, sizeof P.note, "%s", prjslot::last());
      P.view = kVList;
    }
    return code == kYes || code == kNo || (code >= 0 && code < 16) || code == kUp || code == kDown;
  }
  int ph = F.st.phase;
  if (job_running(ph) || ph == bundle::kServe) {
    if (code == kNo) post("bundle=cancel");
    return code == kNo || code == kYes || (code >= 0 && code < 16) || code == kUp || code == kDown;
  }
  if (ph == bundle::kReady && F.have_plan) {
    if (code == kYes && F.plan.slot && (F.plan.clash || F.plan.pick)) {
      // slot mode: a slot with Sidecar files is replaced from the web page only (it asks; they go to the trash)
    } else if (code == kYes) {
      unsigned skip = 0;
      if (P.pk_off)
        for (int i = 0; i < F.plan.nmach; ++i)
          if (F.plan.mach[i].slot) skip |= 1u << (F.plan.mach[i].slot - 1);
      char a[16], b[4];
      sniprintf(a, sizeof a, "%u", skip), sniprintf(b, sizeof b, "%d", P.pk_off ? 0 : 1);
      post("bundle=import:%s:%s", a, b);
    } else if (code == kNo) {
      post("bundle=cancel"), post("bundle=finish");
      P.view = kVList, P.pk_off = false;
    } else if (code == 2 && F.plan.nmach) {
      P.pk_off = !P.pk_off;
    }
    return code == kYes || code == kNo || (code >= 0 && code < 16) || code == kUp || code == kDown;
  }
  if (ph == bundle::kDone || ph == bundle::kFailed || ph == bundle::kCancelled) {
    if (code == kYes || code == kNo) post("bundle=finish"), P.view = kVList, P.pk_off = false;
    return code == kYes || code == kNo || (code >= 0 && code < 16) || code == kUp || code == kDown;
  }
  P.note[0] = 0;
  const bundle::ProjectRow* r = P.cur < F.nproj ? &F.proj[P.cur] : nullptr;
  switch (P.view) {
    case kVAskExport:
      if (code == kYes && P.key[0]) {
        char t[8];
        dest_token(t, sizeof t);
        post("bundle=export:%s:%s", t, P.key);
      }
      if (code == kYes || code == kNo) P.view = kVList;
      return code == kYes || code == kNo || (code >= 0 && code < 16);
    case kVAskDelete:
      if (code == kYes) P.view = kVSureDelete;
      else if (code == kNo) P.view = kVList;
      return code == kYes || code == kNo || (code >= 0 && code < 16);
    case kVSureDelete:
      if (code == kYes) post("bundle=delete:%s", P.key);
      if (code == kYes || code == kNo) P.view = kVList;
      return code == kYes || code == kNo || (code >= 0 && code < 16);
    case kVBundles:
      if (code == kUp || code == kDown) return move(code == kUp ? -1 : 1), true;
      if (code == kYes && P.bcur < F.nbund && !F.reading && src_ok()) {
        const bundle::BundleRow& b = F.bund[P.bcur];
        if (b.incomplete) sniprintf(P.note, sizeof P.note, "INCOMPLETE: EXPORT AGAIN"), P.view = kVList;
        else post("bundle=verify:%s", b.path), P.view = kVList;
        return true;
      }
      if (code == kNo || code == 1) P.view = kVList;
      return code == kYes || code == kNo || (code >= 0 && code < 16);
    case kVInfo:
      if (code == kUp || code == kDown) return move(code == kUp ? -1 : 1), true;
      if (code == kYes || code == kNo || code == 2) P.view = kVList;
      return code == kYes || code == kNo || (code >= 0 && code < 16);
    default:
      break;
  }
  if (code == kUp || code == kDown) return move(code == kUp ? -1 : 1), true;
  if (code == 0 && r) sniprintf(P.key, sizeof P.key, "%s", r->key), P.view = kVAskExport;
  if (code == 1) P.view = kVBundles, P.bcur = P.btop = 0, ask_lists();
  if (code == 2 && r) P.view = kVInfo;
  if (code == 3 && r) {
    if (r->current) sniprintf(P.note, sizeof P.note, "CURRENT ONE: NOT DELETED");
    else sniprintf(P.key, sizeof P.key, "%s", r->key), P.view = kVAskDelete;
  }
  if (code == 4) {
    if (P.dest) {
      char t[8];
      dest_token(t, sizeof t);
      post("bundle=eject:%s", t);
      sniprintf(P.note, sizeof P.note, "EJECTING %s...", t);
      upper(P.note);
      P.ejecting = true, P.eject_n = F.eject_n;
      P.dest = 0, P.dest_drive[0] = 0;
    } else {
      sniprintf(P.note, sizeof P.note, "H: PICK A USB STICK FIRST");
    }
  }
  return code >= 0 && code < 16;   // the trig keys are the page's (T1..T5 act, the rest do nothing)
}
CORE_COLD uint16_t project_trigs() { return 0x001F; }
CORE_COLD bool project_leds(uint8_t c[16]) {
  memset(c, kLedOff, 16);
  if (job_running(F.st.phase)) return true;   // off while a job runs
  c[0] = kLedBlue;           // T1 EXPORT
  c[1] = 31;                 // T2 IMPORT: green
  c[2] = kLedBlueDim;        // T3 INFO
  c[3] = 15;                 // T4 DELETE: red
  c[4] = P.dest ? 63 : kLedOff;   // T5 EJECT: amber with a stick picked
  return true;
}
CORE_COLD uint8_t project_encs() { return 0x81; }   // A the cursor, H the card / a stick

}  // namespace

// tests and the renderer: the page's state cleared
CORE_COLD void project_page_reset() { memset(&P, 0, sizeof P), F_at = ~0u; }

UI_ROM const ExtraPage kProjectPage = {"PROJECT", project_shown, project_draw, project_enc, project_key, 0x81, false,
                                       project_encs, project_trigs, project_leds};

}  // namespace ui
}  // namespace core
#endif
