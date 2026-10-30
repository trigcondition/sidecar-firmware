// core/bundle/rename.cpp - see rename.h. Every card operation goes through the card queue (core/msamp/cardq.h: on the Pi
// the card core runs it, the control core takes the answer on a later pass and never waits; without a card core it
// runs at once): kExistsReq for the looks, kRenameReq for the moves - one request at a time, a step a tick.
#include "rename.h"

#if CORE_MSAMP
#include <string.h>

#include "bundle_int.h"
#include "../fmt/fmt.h"
#include "../msamp/cardq.h"
#include "../pool/pool.h"
#include "../lfo.h"
#include "../lib/pools.h"
#if CORE_POLY
#include "../fx/insert.h"
#include "../seq/seq.h"
#include "../song/song.h"
#endif

namespace core {
namespace rename {

namespace {

constexpr int kMaxOps = 32;
constexpr char kProjDir[] = "/.rytm/projects/%s";   // the pool lists' folder: moved whole
constexpr uint32_t kCheckMs = 3000;                  // the looks not done by then (a card core held up): no question
enum Phase : uint8_t { kNone, kCheckTo, kCheckFrom, kWait, kFlush, kMove, kUndo };
constexpr uint32_t kFlushMs = 5000, kStallMs = 2000;   // FLUSH at most; a card request waiting that long: run here
struct Op {
  char src[cardq::kPathMax], dst[cardq::kPathMax];
};
struct S {
  uint8_t state, phase;
  char from[24], to[24], kf[kKeyMax], kt[kKeyMax];
  int nfiles;
  uint32_t asked_ms, asks, began_ms;
  char last[48];
  int ncand, i, nops, done;   // the paths to look at (ncand), the one now (i); the moves (nops), made (done)
  int stuck;                  // undo moves that failed (files left under the new key)
  bool relook;                // after FLUSH: the old key looked at again (a save may have made a file), then the moves
  uint32_t flush_t0, op_t0;
  bool clash;
};
S s;
Op ops[kMaxOps];
char cand[kMaxOps][cardq::kPathMax];   // the per-project paths of one key
cardq::Req req;
File dummy;

CORE_COLD void upper(char* b) {
  for (; *b; ++b)
    if (*b >= 'a' && *b <= 'z') *b = (char)(*b - 32);
}

// the per-project paths for key (the pool lists' files go with their folder)
CORE_COLD int candidates(const char* key, char (*out)[cardq::kPathMax]) {
  int n = 0;
  for (int i = 0; i < bundle::kNumProjectFiles && n < kMaxOps - 1; ++i) {
    const char* pat = bundle::kProjectFiles[i].card;
    if (!strncmp(pat, "/.rytm/projects/", 16)) continue;   // (in the folder)
    sniprintf(out[n++], cardq::kPathMax, pat, key);
  }
  sniprintf(out[n++], cardq::kPathMax, kProjDir, key);
  return n;
}
// path for key -> the same path for key2 (the key is the pattern's only %s: rebuilt from the table)
CORE_COLD void target_of(int ci, char* out) {
  int n = 0;
  for (int i = 0; i < bundle::kNumProjectFiles; ++i) {
    const char* pat = bundle::kProjectFiles[i].card;
    if (!strncmp(pat, "/.rytm/projects/", 16)) continue;
    if (n++ == ci) { sniprintf(out, cardq::kPathMax, pat, s.kt); return; }
  }
  sniprintf(out, cardq::kPathMax, kProjDir, s.kt);
}

CORE_COLD void finish() {   // the switch the Rytm's 10 asked for, whatever came of it
  char to[24];
  sniprintf(to, sizeof to, "%s", s.to);
  s.state = kIdle, s.phase = kNone;
  s.from[0] = s.to[0] = 0;
  upper(s.last);
  project_switch(to);   // (pool.cpp: the new name's files now)
}

}  // namespace

CORE_COLD bool intercept(const char* from, const char* to) {
  if (s.phase != kNone) {
    if (!strcmp(to, s.to)) return true;   // the Rytm's repeat of the same name: still looking / asking / moving
    if (s.phase == kFlush || s.phase == kMove || s.phase == kUndo) return true;   // (a move runs to its end; the Rytm's 10 repeats)
    s.state = kIdle, s.phase = kNone;     // another name meanwhile: the question is moot
    if (cardq::idle(&req) || cardq::cancel(&req)) {}   // (a look in flight: its answer is not read)
    log(CORE_STR("rename: %s named before an answer: %s not moved"), to, s.from);
  }
  if (!card_ok || !from || !from[0] || !to || !to[0]) return false;
  project_key(from, s.kf, sizeof s.kf);
  project_key(to, s.kt, sizeof s.kt);
  if (!strcmp(s.kf, s.kt)) return false;   // the same files either way
  if (!cardq::idle(&req)) return false;    // (a cancelled look still running: this switch goes as before)
  if (req.state == cardq::kDone) req.state = cardq::kFree;   // (a cancelled look's late answer: never this one's)
#if CORE_POLY
  { int ss = song::status().state;            // SONG RECORD's journal is open in the songs folder: no move now
    if (ss == song::kArming || ss == song::kArmed || ss == song::kRec || ss == song::kReplayReady || ss == song::kReplaying) {
      log(CORE_STR("rename: SONG RECORD runs: %s switched to without a question"), to);
      return false;
    } }
#endif
  sniprintf(s.from, sizeof s.from, "%s", from);
  sniprintf(s.to, sizeof s.to, "%s", to);
  s.ncand = candidates(s.kt, cand), s.i = 0, s.nops = 0, s.nfiles = 0, s.clash = false;
  s.phase = kCheckTo, s.began_ms = now_ms();
  tick(s.began_ms);   // (no card core: the looks run here at once, and the switch may be made already)
  return true;        // the switch is this module's now (made, or waiting for the looks / the answer)
}

CORE_COLD int answer(bool yes) {
  if (s.state != kAsk || s.phase != kWait) return 0;
  s.state = kIdle;   // (the question is answered: the page shows the outcome once the move ends)
  if (!yes) {
    sniprintf(s.last, sizeof s.last, "%s: A NEW PROJECT", s.to);
    log(CORE_STR("rename: no - %s starts without %s's Sidecar files"), s.to, s.from);
    finish();
    return 0;
  }
  // FLUSH: every unsaved edit of the old project into its files first (they move with them)
#if CORE_POLY
  seq::project_leaving();
  ins::settings_due();
#endif
  lfo::save_due();
  lfo::save();   // (as a project switch saves them: at once, its own small file)
  s.phase = kFlush, s.done = 0, s.stuck = 0, s.flush_t0 = now_ms();
  tick(now_ms());
  return s.phase == kNone && !strncmp(s.last, "MOVED", 5) ? s.nops : 0;
}

CORE_COLD void tick(uint32_t ms) {
  for (int guard = 0; guard < kMaxOps * 2 + 4; ++guard) {   // (no card core: every step at once; with one: one a pass)
    switch (s.phase) {
      case kCheckTo:
      case kCheckFrom: {
        if (ms - s.began_ms > kCheckMs) {   // the card core held up: no question, the switch as before
          if (!cardq::idle(&req)) cardq::cancel(&req);
          log(CORE_STR("rename: the card did not answer: %s switched to without a question"), s.to);
          s.last[0] = 0;
          finish();
          return;
        }
        if (s.i >= s.ncand) {
          if (s.phase == kCheckTo) {   // nothing under the new key: what does the old one have?
            s.phase = kCheckFrom, s.ncand = candidates(s.kf, cand), s.i = 0;
            continue;
          }
          if (s.relook) {   // after FLUSH: the moves, with every file the saves made
            s.relook = false;
            s.phase = kMove, s.op_t0 = ms;
            continue;
          }
          if (!s.nops) {   // nothing to keep
            s.last[0] = 0;
            finish();
            return;
          }
          s.state = kAsk, s.phase = kWait, s.nfiles = s.nops, s.asked_ms = ms, ++s.asks;
          log(CORE_STR("rename: the Rytm names %s, which has no Sidecar files; %s has %d: asking"), s.to, s.from, s.nops);
          return;
        }
        if (!cardq::async(&req, cardq::kExistsReq, &dummy, cand[s.i], 0, nullptr, 0)) return;   // (its answer later)
        if (req.got == 1) {
          if (s.phase == kCheckTo) {   // the new name has files of its own: a project of its own, no question
            s.last[0] = 0;
            finish();
            return;
          }
          if (s.nops < kMaxOps) {
            strcpy(ops[s.nops].src, cand[s.i]);
            target_of(s.i, ops[s.nops].dst);
            ++s.nops;
          }
        }
        ++s.i;
        continue;
      }
      case kWait:
        if (ms - s.asked_ms >= kAskMs) {
          log(CORE_STR("rename: no answer"));
          s.phase = kWait;
          answer(false);
        }
        return;
      case kFlush: {   // the saves idle_save makes meanwhile; then the moves (idle_save holds the saves from here on)
        // 0.3.0: fmt::post_busy() counts too - a settings save the control core has handed to the card core is no
        // longer dirty but is not on the card yet, and a write landing while these very files move would recreate the
        // old project's folder (or write into one that has gone).
        bool pending = lfo::dirty() || pools::saving() || fmt::post_busy();
#if CORE_POLY
        pending = pending || ins::settings_dirty(nullptr);
#endif
        if (pending && ms - s.flush_t0 < kFlushMs) return;
        if (pending)
          log(CORE_STR("rename: unsaved edits still waiting after %u ms (lfo %d pools %d posted %d): moving anyway"),
              (unsigned)kFlushMs, (int)lfo::dirty(), (int)pools::saving(), (int)fmt::post_busy());
        // whatever is still in flight lands (or is run here) before anything moves. After a long hold of the card core
        // (phase 3: nothing is taken back on the way) that is the whole queue built meanwhile, possibly several
        // pattern-sized writes, run here on the control core: allowed - a rename is a project operation the user waits
        // for, and moving files with writes of them still posted would lose or misplace them
        fmt::post_wait();
        s.phase = kCheckFrom, s.ncand = candidates(s.kf, cand), s.i = 0, s.nops = 0, s.relook = true, s.began_ms = ms;
        continue;
      }
      case kMove: {
        if (s.done >= s.nops) {
          sniprintf(s.last, sizeof s.last, "MOVED %d TO %s", s.nops, s.to);
          log(CORE_STR("rename: %d Sidecar files / folders of %s moved to %s"), s.nops, s.kf, s.kt);
          // the LFOs and inserts in memory are the new name's now (their files moved with the rest): an edit while
          // its store fills is saved under the new name and put into its store - never a file under the old one
          lfo::rekey(s.to);
#if CORE_POLY
          ins::rekey(s.to);
          seq::rekey(s.to);   // (its patterns too: phase 2)
#endif
          finish();
          return;
        }
        Op& o = ops[s.done];
        if (!cardq::async(&req, cardq::kRenameReq, &dummy, o.src, 0, (uint8_t*)o.dst, (uint32_t)strlen(o.dst) + 1)) {
          if (ms - s.op_t0 > kStallMs) cardq::take_back(&req);   // (the card core held up: run here)
          return;
        }
        s.op_t0 = ms;
        if (req.got == 1) { ++s.done; continue; }
        log(CORE_STR("rename: %s -> %s refused: the ones moved go back"), o.src, o.dst);
        s.phase = kUndo;
        continue;
      }
      case kUndo: {   // the moved ones back, newest first; the switch then goes on as NO
        if (s.done <= 0) {
          if (s.stuck) sniprintf(s.last, sizeof s.last, "MOVE INCOMPLETE: %d UNDER %s", s.stuck, s.to);
          else sniprintf(s.last, sizeof s.last, "MOVE FAILED: NOTHING MOVED");
          finish();
          return;
        }
        Op& o = ops[s.done - 1];
        if (!cardq::async(&req, cardq::kRenameReq, &dummy, o.dst, 0, (uint8_t*)o.src, (uint32_t)strlen(o.src) + 1)) {
          if (ms - s.op_t0 > kStallMs) cardq::take_back(&req);
          return;
        }
        s.op_t0 = ms;
        if (req.got != 1) ++s.stuck, log(CORE_STR("rename: %s could not go back to %s (left under the new key)"), o.dst, o.src);
        --s.done;
        continue;
      }
      default:
        return;
    }
  }
}

int state() { return s.state; }
const char* from() { return s.from; }
const char* to() { return s.to; }
int files() { return s.nfiles; }
const char* last() { return s.last; }
uint32_t asks() { return s.asks; }
bool busy() { return s.phase != kNone; }
// (from FLUSH's end on: the old key's second look - a save landing then could make a file the moves leave behind -,
// the moves, an undo)
bool moving() { return s.phase == kMove || s.phase == kUndo || (s.phase == kCheckFrom && s.relook); }
CORE_COLD void back() {
  if (s.phase == kNone || s.phase == kFlush || s.phase == kMove || s.phase == kUndo) return;   // (a move runs to its end)
  if (!cardq::idle(&req)) cardq::cancel(&req);
  log(CORE_STR("rename: the Rytm is back on %s: no question about %s"), project, s.to);
  s.state = kIdle, s.phase = kNone, s.from[0] = s.to[0] = 0;
}
CORE_COLD void reset() {
  memset(&s, 0, sizeof s);
  memset(&req, 0, sizeof req);
}

}  // namespace rename
}  // namespace core
#endif
