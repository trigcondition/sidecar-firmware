// core/seq/patwrite.cpp - see patwrite.h. Control side; cold.
#include "seq/patwrite.h"

#include <string.h>

#include "hooks.h"
#include "platform.h"
#include "pool/pool.h"
#include "seq/lockfill.h"
#include "seq/rytm_dump.h"

namespace core {
namespace patwrite {

CORE_COLD const char* why_text(int w) {
  switch (w) {
    case kWhyNone: return "DONE";
    case kWhyPlaying: return "STOP TO FILL";
    case kWhyBusy: return "PATTERN BUSY";
    case kWhySong: return "SONG BUSY";
    case kWhyNoAnswer: return "NO ANSWER FROM RYTM";
    case kWhyVerify: return "VERIFY FAILED: RESTORED";
    case kWhyPatch: return "NOT WRITTEN";
    case kWhyPlayed: return "PLAY PRESSED: NOT WRITTEN";
    case kWhyRunning: return "A FILL IS RUNNING";
    case kWhyChanged: return "CHANGED MEANWHILE: CHECK";
    case kWhyUnconfirmed: return "NOT CONFIRMED: CHECK IT";
    case kWhyNotTaken: return "NOT TAKEN: UNCHANGED";
    case kWhyNoPattern: return "PATTERN UNKNOWN";
    case kWhyProject: return "PROJECT LOADED: STOPPED";
    default: return "FAILED";
  }
}

#if CORE_POLY
namespace {
// pattern n, by number: the read (0x64 n; its answer 0x54 n) and the write (0x54 n) name the same pattern
constexpr uint8_t kReqN = 0x64, kAnsN = 0x54;
constexpr int kAnswer = rytm_dump::kAnswer, kRecord = rytm_dump::kRecord;
constexpr int kPiece = 3069;   // a long SysEx in pieces (3-byte aligned: the Pi's records <= 4096 B; song.cpp out())
struct Job {
  State st = kIdle;
  PatchFn fn = nullptr;
  void* ctx = nullptr;
  int why = kWhyNone, presult = 0, reads = 0, tries = 0;
  bool changed = false, play_ok = false, have_before = false;
  uint32_t at = 0, now = 0, dirty0 = 0, switch0 = 0, jobs = 0;
  uint32_t dirtyw = 0, switchw = 0;   // the counters when the write went out (after it, only these tell a change)
  uint32_t read_switch = 0;           // switch0 of the read the patch was made from
  bool reverified = false;            // a verify answer as before the write: asked once more
  bool now_valid = false;
  int nbuf = -1;
  int pat = -1;                       // the pattern read and written (rytm_dump::active_pattern() at the read)
  uint32_t project = 0;               // project_gen() at the start: a project load since ends the job unwritten
  Stats st_ = {};
};
Job J;
uint8_t* g_in = nullptr;        // the answer being read
uint8_t* g_orig = nullptr;      // the answer as read: a 0x54 to its pattern (the restore's bytes)
uint8_t* g_out = nullptr;       // the message written
uint8_t* g_rec = nullptr;       // the record as read, then patched
uint8_t* g_before = nullptr;    // the record as read (before())
uint8_t* g_tmp = nullptr;       // a verify's record

void alloc() {
  if (g_in) return;
  g_in = new uint8_t[kAnswer], g_orig = new uint8_t[kAnswer], g_out = new uint8_t[kAnswer];
  g_rec = new uint8_t[kRecord], g_before = new uint8_t[kRecord], g_tmp = new uint8_t[kRecord];
}
void send_long(const uint8_t* m, int n) {
  for (int i = 0; i < n; i += kPiece) to_rytm(m + i, n - i < kPiece ? n - i : kPiece);
}
void request() {
  uint8_t k[15] = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, kReqN, 0x01, 0x01, (uint8_t)J.pat, 0x00, 0x00, 0x00, 0x05, 0xF7};
  to_rytm(k, 15);
  J.at = J.now, ++J.tries;
}
void end(State s, int why) {
  J.st = s, J.why = why, ++J.jobs;
  rytm_dump::set_fill_hold(false);
  if (s == kDone || J.changed) rytm_dump::want(rytm_dump::kDirtyMs);   // the pattern view: the new pattern
  log(CORE_STR("fill: pattern write %s (%s; reads %d)"), s == kDone ? "done" : "failed", why_text(why), J.reads);
}
// After the write: the pattern switched, or the DIRTYs are not exactly the two a received 0x54 makes of its own
// (kOwnDirties): more = the user edited, fewer = the write was not (fully) taken and may have been edited over - the
// user's pattern moved under us: never write anything back then.
bool moved_since_write() {
  return rytm_dump::switch_count() != J.switchw || rytm_dump::dirty_count() - J.dirtyw != kOwnDirties ||
         project_gen() != J.project;   // (a project load: pattern n is another project's now)
}
void restore_or_leave(int why_restored, int why_left) {
  if (moved_since_write()) {
    log(CORE_STR("fill: the pattern changed meanwhile: nothing written back (check it)"));
    end(kFailed, why_left);
    return;
  }
  send_long(g_orig, kAnswer);
  ++J.st_.restores;
  end(kFailed, why_restored);
}
void read_again() {
  J.st = kReading, J.tries = 0;
  J.dirty0 = rytm_dump::dirty_count(), J.switch0 = rytm_dump::switch_count();
  J.pat = rytm_dump::active_pattern();
  if (J.pat < 0) { end(kFailed, kWhyNoPattern); return; }   // a hello forgot it (start refuses before): never 0xFF
  ++J.reads, ++J.st_.reads;
  request();
}
void on_answer(const uint8_t* m) {
  if (J.st == kReading) {
    if (rytm_dump::dirty_count() != J.dirty0 || rytm_dump::switch_count() != J.switch0) {   // an edit meanwhile
      ++J.st_.restarts;
      if (J.reads >= kReads) { end(kFailed, kWhyBusy); return; }
      read_again();
      return;
    }
    if (rytm_dump::is_playing() && !J.play_ok) { end(kFailed, kWhyPlayed); return; }
    if (rytm_dump::hold_level() >= 2) { end(kFailed, kWhySong); return; }
    if (!rytm_dump::parse_id(m, kAnswer, kAnsN, g_rec, nullptr)) return;   // (checked by the caller: byte 9 = J.pat)
    memcpy(g_orig, m, kAnswer);   // 0x54, byte 9 = the pattern read: written back as it is (the write, the restore)
    memcpy(g_before, g_rec, kRecord);
    J.have_before = true, J.read_switch = J.switch0;
    int r = J.fn ? J.fn(g_rec, J.ctx) : 0;
    if (r) { J.presult = r; end(kFailed, kWhyPatch); return; }
    if (!memcmp(g_rec, g_before, kRecord)) { J.changed = false; end(kDone, kWhyNone); return; }   // nothing to write
    // a project load since the start (it keeps the pattern number, so the guard above does not see it): the pattern
    // read may be the old project's; written now it would land in the new project's stored pattern n
    if (project_gen() != J.project) { end(kFailed, kWhyProject); return; }
    memcpy(g_out, g_orig, kAnswer);
    rytm_dump::repack(g_out, g_rec);
    J.dirtyw = rytm_dump::dirty_count(), J.switchw = rytm_dump::switch_count();   // (= the read's: checked above)
    send_long(g_out, kAnswer);
    J.changed = true, ++J.st_.writes, J.reverified = false;
    J.st = kSettle, J.at = J.now;
    return;
  }
  if (J.st == kVerifying) {
    // the song's dumps began (hold 2) before a tick saw it: this answer may be the song's; out of its way (M1)
    if (rytm_dump::hold_level() >= 2) { end(kFailed, kWhySong); return; }
    if (!rytm_dump::parse_id(m, kAnswer, kAnsN, g_tmp, nullptr)) return;
    ++J.st_.verifies;
    // another pattern's answer, or another project's pattern n now (a project load keeps the number)
    if (rytm_dump::switch_count() != J.switchw || project_gen() != J.project) { end(kFailed, kWhyChanged); return; }
    if (lockfill::same(g_tmp, g_rec)) { end(kDone, kWhyNone); return; }   // (the Rytm re-sorts its lock list)
    // the pattern as it was before the write: a late answer to a read request, or the write not taken (yet): once more
    if (!J.reverified && !memcmp(g_tmp, g_before, kRecord)) {
      J.reverified = true, J.tries = 0;
      request();
      return;
    }
    // ... again: the Rytm did not take the write; it holds the pattern as read, so there is nothing to put back (a
    // restore could only overwrite an edit made meanwhile)
    if (!memcmp(g_tmp, g_before, kRecord)) {
      log(CORE_STR("fill: the Rytm did not take the write: its pattern is as before"));
      end(kFailed, kWhyNotTaken);
      return;
    }
    ++J.st_.mismatches;
    log(CORE_STR("fill: the pattern read back differs from the one written"));
    restore_or_leave(kWhyVerify, kWhyChanged);   // the read one back - unless the user's pattern moved meanwhile
  }
}
}  // namespace

CORE_COLD bool start(PatchFn fn, void* ctx) {
  if (busy()) { J.why = kWhyRunning; return false; }
  if (rytm_dump::hold_level() >= 2) { J.why = kWhySong; return false; }
  if (rytm_dump::is_playing() && !J.play_ok) { J.why = kWhyPlaying; return false; }
  // the write goes to the active pattern by number (0x54): not known yet (no 2D 02), nothing is read or written - a
  // 0x5A instead would reach only the working copy, lost at a power-off
  if (rytm_dump::active_pattern() < 0) { J.why = kWhyNoPattern; return false; }
  alloc();
  J.fn = fn, J.ctx = ctx, J.why = kWhyNone, J.presult = 0, J.reads = 0, J.changed = false, J.have_before = false;
  J.nbuf = -1, J.project = project_gen();
  J.now = now_ms();
  rytm_dump::set_fill_hold(true);
  read_again();
  return true;
}

CORE_COLD void tick(uint32_t ms) {
  J.now = ms;
  if (!busy()) return;
  // the song's own dumps or writes began meanwhile (M1): out of their way, nothing written back
  if (rytm_dump::hold_level() >= 2) { end(kFailed, kWhySong); return; }
  // H1: another pattern now, or a project load (pattern n is another project's): nothing written back, no DONE
  if ((J.st == kSettle || J.st == kVerifying) && (rytm_dump::switch_count() != J.switchw || project_gen() != J.project)) {
    log(CORE_STR("fill: the pattern changed after the write: nothing written back (check it)"));
    end(kFailed, kWhyChanged);
    return;
  }
  if (J.st == kSettle) {
    if (ms - J.at < kSettleMs) return;
    J.st = kVerifying, J.tries = 0;
    request();
    return;
  }
  if (J.st == kReading && rytm_dump::is_playing() && !J.play_ok) { end(kFailed, kWhyPlayed); return; }
  // another pattern active since the request: it named the old one; read the new one (as an answer would, the guard)
  if (J.st == kReading && rytm_dump::switch_count() != J.switch0) {
    ++J.st_.restarts;
    if (J.reads >= kReads) { end(kFailed, kWhyBusy); return; }
    read_again();
    return;
  }
  if (ms - J.at < kRetryMs) return;
  if (J.tries < kTries) { ++J.st_.timeouts; request(); return; }
  if (J.st == kVerifying) {   // written, never confirmed: the read one back only if nothing moved meanwhile
    restore_or_leave(kWhyVerify, kWhyUnconfirmed);
    return;
  }
  end(kFailed, kWhyNoAnswer);
}

CORE_COLD void byte(uint8_t b) {
  if (b >= 0xF8) return;   // realtime
  if (!busy() || J.st == kSettle) { J.nbuf = -1; return; }
  if (b == 0xF0) J.nbuf = 0;
  else if (J.nbuf < 0) return;
  static const uint8_t head[9] = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, kAnsN, 0x01, 0x01};
  if (J.nbuf < 9 && b != head[J.nbuf]) { J.nbuf = -1; return; }
  if (J.nbuf >= kAnswer) { J.nbuf = -1; return; }
  g_in[J.nbuf++] = b;
  if (b != 0xF7) return;
  int n = J.nbuf;
  J.nbuf = -1;
  int pat = -1;
  if (n != kAnswer || !rytm_dump::parse_id(g_in, n, kAnsN, g_tmp, &pat)) return;
  // another pattern's dump (the Rytm answered another number, or another client asked for one): never ours - a write
  // built from it would put that pattern's content into ours
  if (pat != J.pat) { log(CORE_STR("fill: a dump of pattern %d, not %d: not taken"), pat, J.pat); return; }
  on_answer(g_in);
}

CORE_COLD State state() { return J.st; }
CORE_COLD bool busy() { return J.st == kReading || J.st == kSettle || J.st == kVerifying; }
CORE_COLD int why() { return J.why; }
CORE_COLD int patch_result() { return J.presult; }
CORE_COLD bool changed() { return J.changed; }
CORE_COLD uint32_t jobs() { return J.jobs; }
CORE_COLD uint32_t read_switch() { return J.read_switch; }
CORE_COLD int read_pattern() { return J.have_before ? J.pat : -1; }
CORE_COLD const uint8_t* before() { return J.have_before ? g_before : nullptr; }
CORE_COLD void set_play_ok(bool on) { J.play_ok = on; }
CORE_COLD bool play_ok() { return J.play_ok; }
CORE_COLD const Stats& stats() { return J.st_; }
CORE_COLD void reset() {
  bool ok = false;
  J = Job();
  J.play_ok = ok;
  rytm_dump::set_fill_hold(false);
}
#endif

}  // namespace patwrite
}  // namespace core
