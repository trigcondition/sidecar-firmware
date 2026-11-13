// core/samp/sedit.cpp - see sedit.h.
#include "sedit.h"

#if CORE_MSAMP

#include <string.h>
#include <strings.h>

#include "../lib/library.h"
#include "../lib/pools.h"
#include "../lib/srclib.h"
#include "../machines.h"
#include "../pool/pcache.h"
#include "../pool/pool.h"
#include "../ui/remote.h"
#include "../ui/ui.h"
#include "../voices.h"
#include "dsp.h"
#include "sedit_dsp.h"
#include "sedit_io.h"

namespace core {
namespace sedit {

namespace {

// The job in hand, stepped by step(): a load, an apply's passes, a save. The outline is rebuilt after any of them.
enum Job : uint8_t { kNoJob, kLoad, kApply, kSave };
// An apply's passes, in order (each in pieces of kEditPiece frames; the ones a function does not need are skipped)
enum Pass : uint8_t { kPeak, kFloor, kFirst, kLast, kCopy, kOp, kFade, kSwap };
// A save's: the name, the write, then the pool once the library's published index has the file
enum SavePass : uint8_t { kName, kWrite, kPool };
// The pool's wait for the library to publish the saved file (lib::touched works on the card core's index; the snapshot
// pools read follows at its next publish, 250 ms or more apart on a big index)
constexpr uint32_t kPublishWaitMs = 5000;
constexpr int kJumpMax = 64;   // jump(): detents a call at most

struct Ed {
  State st = kClosed;
  Job job = kNoJob;
  char src[cardq::kPathMax] = "";   // the source file
  char name[48] = "", msg[32] = "";
  int code = -1;                    // its SAMPLES pool row (-1 none)
  int8_t track = -1;
  float sta = 0, end = 120;         // a track's STA / END at the open (0..120), for the region once the length is known
  float* x = nullptr;               // the edit
  float* y = nullptr;               // the spare: the edit before the last apply (undo), and the passes' target
  int bx = -1, by = -1;             // their pcache blocks
  uint32_t frames = 0, rate = 0;
  uint8_t ch = 0, bits = 0;
  uint32_t a = 0, b = 0, wa = 0, wb = 0;
  uint8_t zoom = 0, fn = kCrop, progress = 0;
  bool undo = false, dirty = false;
  uint8_t moved = 0;                // the marker moved last (0 start, 1 end): zoom's centre
  uint32_t u_frames = 0, u_a = 0, u_b = 0;   // the length and region before the last apply
  bool u_dirty = false;
  // the apply in hand
  uint8_t pass = kPeak, jfn = kCrop;
  uint32_t ja = 0, jb = 0, pos = 0;          // its region, the place in the pass
  uint32_t ka = 0, kb = 0;                   // trim's bounds; crop's source range
  float g = 1.0f, floor = 0.0f;
  double sq = 0;                             // trim's floor: the sum of squares so far
  // the save in hand
  uint8_t spass = kName;
  bool use = false;
  int scode = -1;                   // the source's row when the save began (kept if the editor closes meanwhile)
  char target[cardq::kPathMax] = "";
  // the copy's format and length, taken when the save begins (fix round 1, H1: a close during the save zeroes frames;
  // the card index must get the file's own)
  uint32_t s_frames = 0, s_rate = 0;
  uint8_t s_ch = 0, s_bits = 0;
  // fix round 2 (owner): SAVE writes START..END only: the region at the save's start; once saved, the edit becomes it
  // (a crop run as the save's last part: post_save, no undo, nothing unsaved)
  uint32_t s_a = 0, s_b = 0;
  bool post_save = false;
  uint32_t lgen = 0, pool_ms = 0;   // kPool: the library's gen() and the time when the file was whole
  bool pool_off = false;            // the project changed during the save: the copy stays out of the new one's pool
  // a close while a card job runs: the job finishes out of sight, then everything goes
  bool drain = false;
  // live play: the edit as int16 in two play buffers that take turns, the one not in the voices' override refilled
  // after each change; a buffer a voice still reads is never written or freed. sedit-050 #1: every open (a track's or a
  // pool row's), and every voice: whichever voice triggers the edited row (its pool slot) plays the edit
  int16_t* p[2] = {nullptr, nullptr};
  int bp[2] = {-1, -1};             // their pcache blocks
  uint32_t pcap = 0;                // their frames (the loaded length; + a guard frame)
  int cur = -1;                     // the one in the override (-1 none)
  int8_t vi = -1;                   // the track's voice (-1: opened from the pool)
  int slot = 0;                     // the pool slot the override plays for (pool_slot)
  uint32_t tlen = 0;                // the length the track's STA / END are for
  bool rf = false;                  // a refill in hand (into p[rt] from rpos)
  uint8_t rt = 0;
  uint32_t rpos = 0;
  bool nolive = false;              // the play buffers could not be had: open without live play (fix round 1, M3)
  // the outline: built into wlo / whi (float: a gain over 1.0 kept) a few columns a step, published to lo / hi when
  // whole, scaled so the window's peak fits (sedit-050 #3: unity is where 1.0, 0 dBFS, is drawn then)
  bool ol = false;
  int col = 0;
  int16_t lo[kCols] = {}, hi[kCols] = {};
  float wlo[kCols] = {}, whi[kCols] = {};
  int16_t unity = 32767;
  // sedit-050 #8: the transients (sedit_dsp.h Onsets), found after every change in pieces; on_ok when whole
  Onsets on;
  bool on_run = false, on_ok = false;
  uint32_t on_pos = 0;
  uint32_t gen = 0, step_frames = 0;
};
Ed e;
Loader g_load;
// Play buffers let go (a close) while a voice still read them: released by step() once none does
struct Retired {
  int blk;
  const int16_t *lo, *hi;
};
constexpr int kRetired = 8;
Retired g_ret[kRetired];
int g_nret = 0;
Saver g_save;
Namer g_name;

UI_ROM const char* const kStNames[] = {"closed", "loading", "ready", "busy", "saving", "error"};
// The console's short names for the functions (sedit_dsp.h Fn order); kFnNames' are taken too
UI_ROM const char* const kFnShort[kFns] = {"crop", "norm", "trim", "fadein", "fadeout", "rev", "gain+", "gain-"};

CORE_COLD void touch() { ++e.gen; }
CORE_COLD void say(const char* m) {
  sniprintf(e.msg, sizeof e.msg, "%s", m);
  touch();
}
CORE_COLD uint32_t min32(uint32_t p, uint32_t q) { return p < q ? p : q; }
// A sample as the int16 play buffer holds it (samp::put16's rounding and clip)
CORE_COLD int16_t pcm16(float v) {
  float s = v * 32768.0f;
  return (int16_t)(s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : (int32_t)(s < 0 ? s - 0.5f : s + 0.5f));
}
CORE_COLD void set_progress(uint32_t done, uint32_t of) {
  uint8_t p = (uint8_t)(of ? (uint64_t)done * 100 / of : 100);
  if (p != e.progress) e.progress = p, touch();
}

CORE_COLD void ol_start() {   // the outline of the window, from its first column at the next steps
  e.ol = true;
  e.col = 0;
}
CORE_COLD void on_start() {   // the transients found again, from the first frame at the next steps
  e.on.begin(e.rate);
  e.on_run = true, e.on_ok = false, e.on_pos = 0;
}
// After every change of the edit (the load, apply, undo, crop): the play buffer not in the override refilled (a track's
// open), the outline rebuilt
CORE_COLD void play_changed() {
  if (e.p[0]) {
    e.rf = true;
    e.rt = e.cur == 0 ? 1 : 0;
    e.rpos = 0;
  }
  ol_start();
  on_start();
}
// The pool slot a SAMPLES row plays as (pool_slot of its SMP value)
CORE_COLD int slot_of(int code) {
  return pool_slot((float)(code % srclib::kBankRows) + (float)srclib::lo_of(code) / 128.0f);
}
CORE_COLD const int16_t* play_end(int k) { return e.p[k] + (size_t)(e.pcap + 1) * e.ch; }
// Every voice on play buffer cur from its next trig of the edited row (sedit-050 #1: not only the track's: a pool open
// has none, and another track on the same row plays the edit too): the whole edit when its length is not the one STA /
// END are for
CORE_COLD void publish() {
  bool whole = e.frames != e.tlen;
  DspOverride o = {e.p[e.cur], e.frames, (float)e.rate, e.ch, e.slot, whole ? 0 : -1, whole ? (int32_t)e.frames : -1};
  for (int i = 0; i < kVoices; ++i) dsps_override(i, &o);
}
// A play buffer's block given back now if no voice reads it, else onto the retired list
CORE_COLD void retire(int blk, const int16_t* lo, const int16_t* hi) {
  dsps_detach_idle(lo, hi);
  if (!dsps_reads(lo, hi)) pcache::release(blk);
  else if (g_nret < kRetired) g_ret[g_nret++] = {blk, lo, hi};
  // (full: kept - never freed under a voice; start_open refuses an open that could fill it)
}
CORE_COLD void retired_step() {
  for (int i = 0; i < g_nret;) {
    dsps_detach_idle(g_ret[i].lo, g_ret[i].hi);
    if (dsps_reads(g_ret[i].lo, g_ret[i].hi)) {
      ++i;
      continue;
    }
    pcache::release(g_ret[i].blk);
    g_ret[i] = g_ret[--g_nret];
  }
}
// The track back on its own sample (the override cleared first), the play buffers let go
CORE_COLD void release_play() {
  if (e.cur >= 0)
    for (int i = 0; i < kVoices; ++i) dsps_override(i, nullptr);
  for (int k = 0; k < 2; ++k) {
    if (e.bp[k] >= 0) retire(e.bp[k], e.p[k], play_end(k));
    e.bp[k] = -1, e.p[k] = nullptr;
  }
  e.cur = -1, e.vi = -1, e.rf = false;
}

CORE_COLD void clamp() {   // the region within the sample (sedit_dsp.h clamp_region)
  int64_t a = e.a, b = e.b;
  clamp_region(e.frames, &a, &b);
  e.a = (uint32_t)a, e.b = (uint32_t)b;
}
CORE_COLD void whole_window() {
  e.wa = 0, e.wb = e.frames, e.zoom = 0;
}

CORE_COLD void release_all() {
  release_play();
  if (e.bx >= 0) pcache::release(e.bx);
  if (e.by >= 0) pcache::release(e.by);
  e.bx = e.by = -1;
  e.x = e.y = nullptr;
}
CORE_COLD void fail(const char* m) {   // the open or the job ends here: nothing left open
  release_all();
  e.job = kNoJob;
  e.ol = false;
  e.on_run = e.on_ok = false;
  e.st = kError;
  e.frames = 0;
  say(m);
}
// The job ends: ready once the outline is whole
CORE_COLD void job_done() {
  e.job = kNoJob;
  e.progress = 100;
  if (!e.ol && !e.rf) e.st = kReady;
  touch();
}
CORE_COLD bool refuse_busy() {
  if (e.st == kReady && !e.drain) return false;
  say("BUSY");
  return true;
}

// ---- the load

CORE_COLD bool step_load() {
  uint32_t before = g_load.loaded(), w0 = g_load.waits;
  int r = g_load.step();
  e.step_frames = g_load.loaded() - before;
  if (r > 0) {
    if (g_load.frames) set_progress(g_load.loaded(), g_load.frames);
    return g_load.waits == w0;   // (a card request still running: no work done)
  }
  if (e.drain) {   // closed meanwhile
    if (r == 0) pcache::release(g_load.blk);
    e.job = kNoJob;
    e.drain = false;
    return true;
  }
  if (r < 0) {
    fail(err_text(r));
    return true;
  }
  e.x = g_load.x, e.bx = g_load.blk;
  e.frames = g_load.frames, e.rate = g_load.rate, e.ch = g_load.ch, e.bits = g_load.bits;
  void* p = nullptr;
  e.by = pcache::alloc_private((uint64_t)e.frames * e.ch * 4, &p);
  if (e.by < 0) {
    fail(err_text(kErrRam));
    return true;
  }
  e.y = (float*)p;
  {   // the play buffers: (frames + a guard frame) x ch int16 each (every open: sedit-050 #1). Not to be had: the edit
      // opens anyway, without live play (fix round 1, M3; the footer says so once)
    e.pcap = e.frames;
    e.nolive = false;
    for (int k = 0; k < 2 && !e.nolive; ++k) {
      void* q = nullptr;
      e.bp[k] = pcache::alloc_private((uint64_t)(e.frames + 1) * e.ch * 2, &q);
      e.p[k] = (int16_t*)q;
      if (e.bp[k] < 0) e.nolive = true;
    }
    if (e.nolive)
      for (int k = 0; k < 2; ++k) {
        if (e.bp[k] >= 0) pcache::release(e.bp[k]);   // (no voice has seen it)
        e.bp[k] = -1, e.p[k] = nullptr;
      }
    e.tlen = e.frames;
    e.vi = (int8_t)(e.track >= 0 ? ui::remote_track_voice(e.track) : -1);
    e.slot = slot_of(e.code);
  }
  if (e.track >= 0) {   // STA..END (0..120 onto frames - 1, either way round, END inclusive)
    float lo = e.sta < e.end ? e.sta : e.end, hi = e.sta < e.end ? e.end : e.sta;
    if (lo < 0) lo = 0;
    if (hi > 120) hi = 120;
    e.a = (uint32_t)(lo / 120.0f * (float)(e.frames - 1));
    e.b = (uint32_t)(hi / 120.0f * (float)(e.frames - 1)) + 1;
  } else {
    e.a = 0, e.b = e.frames;
  }
  clamp();
  whole_window();
  e.undo = e.dirty = false;
  e.msg[0] = 0;
  if (e.nolive) say("NO LIVE PLAY: RAM");
  play_changed();
  job_done();
  return true;
}

// ---- an apply

CORE_COLD void apply_swap() {   // the spare is the new edit; the old edit the undo
  float* t = e.x;
  e.x = e.y, e.y = t;
  int tb = e.bx;
  e.bx = e.by, e.by = tb;
  e.u_frames = e.frames, e.u_a = e.ja, e.u_b = e.jb, e.u_dirty = e.dirty;   // (the region the apply began with)
  if (e.jfn == kCrop || e.jfn == kTrim) {
    e.frames = e.kb - e.ka;
    e.a = 0, e.b = e.frames;
    whole_window();
  }
  e.undo = e.dirty = true;
  if (e.post_save) e.undo = e.dirty = false, e.post_save = false;   // (the save's crop: the copy as saved)
  play_changed();
  job_done();
}

// The next pass after the one done (the passes a function needs, in order)
CORE_COLD void next_pass() {
  e.pos = 0;
  switch (e.pass) {
    case kPeak: e.pass = kCopy; break;
    case kFloor: e.pass = kFirst, e.pos = 0; break;
    case kFirst: e.pass = kLast, e.pos = e.frames; break;
    case kLast: e.pass = kCopy; break;
    case kCopy: e.pass = e.jfn == kCrop ? kSwap : e.jfn == kTrim ? kFade : kOp; break;
    default: e.pass = kSwap; break;
  }
}

CORE_COLD bool step_apply() {
  const int ch = e.ch;
  uint32_t n = 0;   // frames this step
  switch (e.pass) {
    case kPeak: {   // normalise: the region's peak (on the edit: a silent region leaves everything as it was)
      uint32_t s = e.ja + e.pos, t = min32(s + kEditPiece, e.jb);
      float p = peak_range(e.x, ch, s, t);
      if (p > e.g) e.g = p;
      n = t - s, e.pos += n;
      set_progress(e.pos, e.jb - e.ja);
      if (s + n < e.jb) break;
      if (e.g < 1e-6f) {
        e.job = kNoJob, e.st = kReady;
        say("SILENT");
        break;
      }
      e.g = kNormPeak / e.g;
      next_pass();
      break;
    }
    case kFloor: {   // trim: samp::trim's floor, the RMS of the first rate / 20 frames + 12 dB, at least -50 dBFS
      uint32_t fl_n = min32(e.rate / 20, e.frames), s = e.pos, t = min32(s + kEditPiece, fl_n);
      for (size_t i = (size_t)s * ch; i < (size_t)t * ch; ++i) e.sq += (double)e.x[i] * e.x[i];
      n = t - s, e.pos += n;
      if (t < fl_n) break;
      e.floor = samp::db_to_lin(-50.0f);
      if (fl_n) {
        float r = (float)sqrt(e.sq / ((double)fl_n * ch)) * 3.98f;
        if (r > e.floor) e.floor = r;
      }
      next_pass();
      break;
    }
    case kFirst: {   // the first frame over the floor
      uint32_t s = e.pos, t = min32(s + kEditPiece, e.frames);
      uint32_t f = samp::first_above(e.x, ch, t, e.floor, s);
      n = (f < t ? f + 1 : t) - s, e.pos += n;
      set_progress(e.pos, e.frames);
      if (f < t) {
        uint32_t pre = e.rate / 500;   // 2 ms before it (samp::trim's, at the file's own rate)
        e.ka = f > pre ? f - pre : 0;
        next_pass();
      } else if (t >= e.frames) {
        e.job = kNoJob, e.st = kReady;
        say("ALL SILENT");
      }
      break;
    }
    case kLast: {   // one past the last frame over the floor, from the end down
      uint32_t t = e.pos, s = t > kEditPiece ? t - kEditPiece : 0;
      uint32_t l = samp::last_above(e.x + (size_t)s * ch, ch, t - s, e.floor);
      n = t - s, e.pos = s;
      if (l || !s) {
        uint32_t last = s + l, post = e.rate / 100;   // 10 ms after it (as the fade: the file's own rate)
        e.kb = last + post < e.frames ? last + post : e.frames;
        next_pass();
      }
      break;
    }
    case kCopy: {   // the edit (crop / trim: [ka, kb)) into the spare from 0
      uint32_t len = e.kb - e.ka, s = e.pos, t = min32(s + kEditPiece, len);
      copy_frames(e.y, e.x, ch, e.ka + s, e.ka + t, s);
      n = t - s, e.pos += n;
      set_progress(e.pos, len);
      if (t >= len) next_pass();
      break;
    }
    case kOp: {   // the function on the spare's region
      uint32_t len = e.jfn == kReverse ? (e.jb - e.ja) / 2 : e.jb - e.ja;   // reverse: pairs
      uint32_t s = e.pos, t = min32(s + (e.jfn == kReverse ? kEditPiece / 2 : kEditPiece), len);
      switch (e.jfn) {
        case kNorm:
        case kGainUp:
        case kGainDown: gain_range(e.y, ch, e.ja + s, e.ja + t, e.g); break;
        case kFadeIn:
        case kFadeOut: fade_range(e.y, ch, e.ja + s, e.ja + t, e.ja, e.jb, e.jfn == kFadeIn); break;
        case kReverse: reverse_pairs(e.y, ch, e.ja, e.jb, s, t); break;
        default: break;
      }
      n = (t - s) * (e.jfn == kReverse ? 2 : 1), e.pos = t;
      set_progress(t, len);
      if (t >= len) next_pass();
      break;
    }
    case kFade: {   // trim: a 10 ms linear fade-out at the new end
      uint32_t len = e.kb - e.ka, f = min32(min32(e.rate / 100, len), kEditPiece);
      if (f) fade_range(e.y, ch, len - f, len, len - f, len, false);
      n = f;
      next_pass();
      break;
    }
    default:
      apply_swap();
      break;
  }
  e.step_frames = n;
  return true;
}

// ---- the save

// The copy saved: into the SAMPLES pool (the source's bank, else the next with room), and the source from now on (not
// when the editor was closed meanwhile: then only the pool)
CORE_COLD void saved() {
  if (e.pool_off) return;   // (closed by a project switch: e.drain too)
  // sedit-050 #5: the engine's card index (built at boot) learns the new file first, so its row resolves to it
  // (pool_sync), plays and has a waveform; before, the row stayed an empty slot until the next boot
  uint8_t hdr[samp::kWavHeaderMax];
  const int known = pool_card_add(e.target, e.s_ch, e.s_bits, e.s_rate, e.s_frames,
                                  (uint32_t)samp::wav_header(hdr, e.s_ch, e.s_bits, e.s_rate, e.s_frames));
  int code = -1;   // (no source row: a take, the shown bank or the next with room)
  int r = e.scode >= 0 ? pools::add_file(pools::kSamples, pools::bank_of(e.scode), e.target, &code) : pools::kErrFull;
  if (r == pools::kErrFull) r = pools::add_take(pools::kSamples, e.target, &code);
  if (e.drain) return;
  sniprintf(e.src, sizeof e.src, "%s", e.target);
  const char* sl = strrchr(e.src, '/');
  sniprintf(e.name, sizeof e.name, "%s", sl ? sl + 1 : e.src);
  e.undo = e.dirty = false;
  if (r == pools::kExisting) {   // the copy's audio is a row's already (e.g. the whole saved with no function run):
    e.code = code;                 // said plainly, and SAVE + USE points the track at that row
    if (e.use && e.track >= 0) use_saved(e.track, code);
    char row[8], m[32];
    pools::row_text(code, row);
    sniprintf(m, sizeof m, "SAVED: SAME AUDIO AS %s", row);
    say(m);
  } else if (r == pools::kOk) {
    e.code = code;
    if (known == -2) {   // a name the card index cannot hold: the row never resolves (not after a reboot either), so
      say("SAVED: NAME TOO LONG");   // SAVE + USE leaves the track on its sample
      return;
    }
    say(known < 0 ? "SAVED: REBOOT TO PLAY" : "SAVED");   // (the index's table full: the row plays after a reboot)
    if (e.use && e.track >= 0) use_saved(e.track, code);
  } else {
    e.code = -1;
    say(r == pools::kErrPath ? "SAVED, NOT IN POOL" : pools::result_text(r));   // (the file is there either way)
  }
}

// The save wrote a region short of the edit: the edit is cropped to it next (save_done)
CORE_COLD bool crop_follows() {
  return e.x && !e.drain && (e.s_a != 0 || e.s_b != e.frames) && e.s_b <= e.frames && e.s_a < e.s_b;
}

// The save's end: the copy pooled, the job over (a close meanwhile: everything goes now)
CORE_COLD bool save_done() {
  e.job = kNoJob;
  saved();
  if (e.drain) {
    e.drain = false;
    release_all();
    return true;
  }
  if (crop_follows()) {
    // the edit becomes what was saved (START..END): crop's passes (kCopy, kSwap), without an undo
    e.jfn = kCrop, e.ja = e.ka = e.s_a, e.jb = e.kb = e.s_b;
    e.pos = 0, e.g = 0, e.sq = 0;
    e.pass = kCopy;
    e.post_save = true;
    e.job = kApply;
    e.st = kBusy;
    touch();
    return true;
  }
  e.st = kReady;
  touch();
  return true;
}
// kPool: the file is whole and the card core's library index has it, the published one perhaps not yet (pools read
// that): pooled once it knows the file, or its gen() moved (a publish after lib::touched has it), or kPublishWaitMs
CORE_COLD bool step_pool() {
  e.step_frames = 0;
  if (lib::gen() == e.lgen && now_ms() - e.pool_ms < kPublishWaitMs) return false;   // (waiting: no work done)
  return save_done();
}

CORE_COLD bool step_save() {
  if (e.spass == kPool) return step_pool();
  if (e.spass == kName) {
    e.step_frames = 0;
    uint32_t w0 = g_name.waits;
    int r = g_name.step();
    if (r > 0) return g_name.waits == w0;   // (the listing still running: no work done)
    if (e.drain) {   // closed meanwhile: nothing written
      e.job = kNoJob, e.drain = false;
      release_all();
      return true;
    }
    if (r < 0) {
      e.job = kNoJob, e.st = kReady;
      say(r == -1 ? err_text(kErrName) : "NO FOLDER");
      return true;
    }
    r = g_save.begin(e.target, e.x + (size_t)e.s_a * e.ch, e.ch, e.bits, e.rate, e.s_frames);   // START..END only
    if (r < 0) {
      e.job = kNoJob, e.st = kReady;
      say(err_text(r));
      return true;
    }
    e.spass = kWrite;
    return true;
  }
  uint32_t before = g_save.written(), w0 = g_save.waits;
  int r = g_save.step();
  e.step_frames = g_save.written() - before;
  if (r > 0) {
    set_progress(g_save.written(), e.s_frames);   // (the region written)
    return g_save.waits == w0;   // (a card request running, or held back for a stream: no work done)
  }
  if (r == 0) {   // written: into the pool once the library's published index has it
    lib::Ref ref;
    if (lib::ref_for_path(e.target, &ref)) return save_done();
    e.spass = kPool, e.lgen = lib::gen(), e.pool_ms = now_ms();
    return true;
  }
  e.job = kNoJob;
  if (e.drain) {
    e.drain = false;
    release_all();
    return true;
  }
  e.st = kReady;
  say(err_text(r));
  touch();
  return true;
}

// ---- the refill: the edit as int16 into the play buffer not in the override, at most kEditPiece frames a step

// Before the first piece: a voice still reading the buffer holds the refill (a note keeps its buffer to its end or the
// voice's next trig); a stopped one lets it go
CORE_COLD bool refill_waits() {
  if (e.rpos) return false;
  const int16_t *lo = e.p[e.rt], *hi = play_end(e.rt);
  dsps_detach_idle(lo, hi);
  return dsps_reads(lo, hi);
}
CORE_COLD bool step_refill() {
  const uint32_t ch = e.ch, s = e.rpos, t = min32(s + kEditPiece, e.frames);
  int16_t* d = e.p[e.rt];
  for (size_t i = (size_t)s * ch; i < (size_t)t * ch; ++i) d[i] = pcm16(e.x[i]);
  e.rpos = t;
  e.step_frames = t - s;
  if (t < e.frames) return true;
  if (e.frames)   // the guard frame (the engine's interpolation reads one past the last): the last again
    for (uint32_t c = 0; c < ch; ++c) d[(size_t)e.frames * ch + c] = d[(size_t)(e.frames - 1) * ch + c];
  e.cur = e.rt;
  e.rf = false;
  publish();
  if (e.job == kNoJob && !e.ol && (e.st == kLoading || e.st == kBusy)) e.st = kReady;
  touch();
  return true;
}

// ---- the outline: whole columns, at most kEditPiece frames a step

CORE_COLD bool step_outline() {
  const uint32_t span = e.wb - e.wa;
  uint32_t n = 0;
  while (e.col < kCols) {
    uint32_t s = e.wa + (uint32_t)((uint64_t)span * e.col / kCols), t = e.wa + (uint32_t)((uint64_t)span * (e.col + 1) / kCols);
    if (t <= s) t = s + 1;
    if (t > e.frames) t = e.frames, s = s < t ? s : t - 1;
    if (n && n + (t - s) > kEditPiece) break;
    float mn = 0, mx = 0;
    if (e.x && t > s) {
      mn = mx = e.x[(size_t)s * e.ch];
      for (size_t i = (size_t)s * e.ch, z = (size_t)t * e.ch; i < z; ++i) {
        float q = e.x[i];
        if (q < mn) mn = q;
        if (q > mx) mx = q;
      }
    }
    e.wlo[e.col] = mn, e.whi[e.col] = mx;
    n += t - s;
    ++e.col;
  }
  e.step_frames = n;
  if (e.col >= kCols) {   // published: over 0 dBFS (a gain the file will clip) the window's peak is the full height
    float pk = 1.0f;
    for (int c = 0; c < kCols; ++c) {
      if (-e.wlo[c] > pk) pk = -e.wlo[c];
      if (e.whi[c] > pk) pk = e.whi[c];
    }
    const float k = 1.0f / pk;
    for (int c = 0; c < kCols; ++c) e.lo[c] = pcm16(e.wlo[c] * k), e.hi[c] = pcm16(e.whi[c] * k);
    e.unity = pcm16(k);
    e.ol = false;
    if (!e.rf && (e.st == kLoading || e.st == kBusy)) e.st = kReady;
    touch();
  }
  return true;
}

// ---- the transients: at most kEditPiece frames a step (they do not hold the editor busy: FUNC + A / B waits for them)
CORE_COLD bool step_onsets() {
  uint32_t s = e.on_pos, t = min32(s + kEditPiece, e.frames);
  e.on.feed(e.x, e.ch, s, t);
  e.on_pos = t;
  e.step_frames = t - s;
  if (t >= e.frames) {
    e.on.end(e.x, e.ch);
    e.on_run = false, e.on_ok = true;
  }
  return true;
}

// ---- the markers (sedit-050 #6, #7)

// f snapped to the nearest zero crossing within kSnapReach (sedit_dsp.h zero_near); dir > 0 only past `from`, dir < 0
// only before it (a turn always moves on). None found: f itself. The sample's ends are not moved.
CORE_COLD uint32_t snapped(int64_t f, int dir, uint32_t from) {
  if (f <= 0) return 0;
  if (f >= e.frames) return e.frames;
  uint32_t t = (uint32_t)f, lo = t > kSnapReach ? t - kSnapReach : 0, hi = t + kSnapReach, o = t;
  if (dir > 0 && lo <= from) lo = from + 1;
  if (dir < 0) {
    if (!from) return t;
    if (hi >= from) hi = from - 1;
  }
  return lo <= hi && zero_near(e.x, e.ch, e.frames, t, lo, hi, &o) ? o : t;
}
// Zoomed in, the window follows the marker (0 start, 1 end) when it leaves it: scrolled so it is an eighth inside
CORE_COLD void follow(int which) {
  const uint32_t span = e.wb - e.wa;
  if (!e.x || span >= e.frames) return;
  uint32_t m = which ? e.b : e.a, wa = e.wa, margin = span / 8;
  bool before = which ? m <= e.wa : m < e.wa, after = which ? m > e.wb : m >= e.wb;
  if (before) wa = m > margin ? m - margin : 0;
  else if (after) wa = m + margin > span ? m + margin - span : 0;
  else return;
  if (wa + span > e.frames) wa = e.frames - span;
  e.wa = wa, e.wb = wa + span;
  ol_start();
}
CORE_COLD void put_start(int64_t f) {
  e.a = f < 0 ? 0 : f > e.frames ? e.frames : (uint32_t)f;
  if (e.a >= e.b) e.a = e.b ? e.b - 1 : 0;
  clamp();
  e.moved = 0;
  follow(0);
  touch();
}
CORE_COLD void put_end(int64_t f) {
  e.b = f < 0 ? 0 : f > e.frames ? e.frames : (uint32_t)f;
  if (e.b <= e.a) e.b = e.a + 1;
  clamp();
  e.moved = 1;
  follow(1);
  touch();
}
// A transient's landing: the crossing at or before it, else the nearest, else the transient itself
CORE_COLD uint32_t onset_landing(uint32_t o) {
  uint32_t l = snapped(o, -1, o + 1);
  if (l == o) l = snapped(o, 0, 0);
  return l;
}

CORE_COLD int start_open(int code, int track, float sta, float end) {
  if (code < 0 || code >= pools::kCodes || !pools::row_of(code)) {
    say("NO SAMPLE");
    return -1;
  }
  const pools::Entry& en = pools::at(pools::kSamples, code);
  if (en.type != pools::kSample || !en.path[0]) {
    say("NO SAMPLE");
    return -1;
  }
  // (BUSY too while the retired list could not take this open's play buffers at its close)
  if (e.drain || (e.st != kReady && e.st != kClosed && e.st != kError) || g_nret > kRetired - 2) {
    say("BUSY");
    return -1;
  }
  close();
  sniprintf(e.src, sizeof e.src, "%s", en.path);
  const char* sl = strrchr(e.src, '/');
  sniprintf(e.name, sizeof e.name, "%s", sl ? sl + 1 : e.src);
  e.code = code, e.track = (int8_t)track, e.sta = sta, e.end = end;
  e.frames = e.rate = 0, e.ch = e.bits = 0, e.a = e.b = e.wa = e.wb = 0;
  e.undo = e.dirty = false;
  e.msg[0] = 0;
  e.progress = 0;
  int r = g_load.begin(e.src);
  if (r < 0) {
    fail(err_text(r));
    return -1;
  }
  e.job = kLoad;
  e.st = kLoading;
  touch();
  return 0;
}

CORE_COLD bool parse_i64(const char** p, int64_t* v) {   // [-] digits
  const char* s = *p;
  bool neg = *s == '-';
  if (neg) ++s;
  if (*s < '0' || *s > '9') return false;
  int64_t x = 0;
  while (*s >= '0' && *s <= '9' && x < ((int64_t)1 << 40)) x = x * 10 + (*s++ - '0');
  *v = neg ? -x : x;
  *p = s;
  return true;
}
CORE_COLD int fn_of(const char* s) {   // a function by number, short name or kFnNames' (any case); -1 none
  if (*s >= '0' && *s <= '9') {
    int n = *s - '0';
    return !s[1] && n < kFns ? n : -1;
  }
  for (int i = 0; i < kFns; ++i)
    if (!strcasecmp(s, kFnShort[i]) || !strcasecmp(s, kFnNames[i])) return i;
  return -1;
}

}  // namespace

// ---- the state

CORE_COLD void begin(bool card) {
  (void)card;
  close();
  e.msg[0] = 0;
  e.name[0] = 0;
  e.drain = false;
}

CORE_COLD int open_track(int track) {
  if ((ui::remote_track_machine(track) & 0x78) != kDspSample) {
    say("NOT A SAMPLE TRACK");
    return -1;
  }
  int vi = ui::remote_track_voice(track);
  int slot = vi < 0 ? 0 : pool_slot(v[vi].p[10]);   // SMP (P11)
  if (slot <= 0) {
    say("NO SAMPLE");
    return -1;
  }
  int code = pools::code((slot - 1) / core::kPool, (slot - 1) % core::kPool + 1);   // (core::: SavePass has a kPool)
  return start_open(code, track, v[vi].p[12], v[vi].p[13]);
}

CORE_COLD int open_code(int code) { return start_open(code, -1, 0, 120); }

CORE_COLD void close() {
  release_play();   // the track on its own sample first (the save in flight reads the float edit only)
  // a card job in flight (the load, the name's listing, the save) is finished by step() first; then the buffers go
  if (e.job == kLoad || e.job == kSave) e.drain = true;
  else {
    release_all();
    e.job = kNoJob;
  }
  e.ol = false;
  e.on_run = e.on_ok = false;
  e.post_save = false;
  e.unity = 32767;
  e.st = kClosed;
  e.frames = 0, e.a = e.b = e.wa = e.wb = 0;
  e.undo = e.dirty = false;
  e.track = -1, e.code = -1;
  e.progress = 0;
  touch();
}

CORE_COLD void use_reset();
CORE_COLD void project_changed() {
  use_reset();   // (the old project's SAVE + USE writes: not sent again, nothing said)
  if (e.job == kSave) e.pool_off = true;
  close();
}

CORE_COLD void set_start(int64_t f) {
  if (!e.x || e.drain) return;
  put_start(snapped(f, 0, 0));
}
CORE_COLD void set_end(int64_t f) {
  if (!e.x || e.drain) return;
  put_end(snapped(f, 0, 0));
}
CORE_COLD void nudge(int which, int detents, bool coarse) {
  if (!e.x || e.drain || !detents) return;
  uint32_t step = (e.wb - e.wa) / 128, from = which ? e.b : e.a;
  int64_t d = (int64_t)(step ? step : 1) * detents * (coarse ? 8 : 1);
  uint32_t to = snapped((int64_t)from + d, detents > 0 ? 1 : -1, from);
  if (which) put_end(to);
  else put_start(to);
}
// The first onset at or past f (Onsets::at is in frame order)
CORE_COLD int onset_at_or_after(uint32_t f) {
  int lo = 0, hi = e.on.n;
  while (lo < hi) {
    int m = (lo + hi) / 2;
    if (e.on.at[m] < f) lo = m + 1;
    else hi = m;
  }
  return lo;
}
CORE_COLD int jump(int which, int detents) {
  if (!e.x || e.drain || !detents) return -1;
  if (!e.on_ok) {
    say("FINDING TRANSIENTS");
    return -1;
  }
  // (at most kJumpMax detents a call: each is a binary search and a few landings, the control core's time bounded)
  const int n = detents < 0 ? (-detents > kJumpMax ? kJumpMax : -detents) : (detents > kJumpMax ? kJumpMax : detents);
  for (int k = 0; k < n; ++k) {
    const uint32_t from = which ? e.b : e.a;
    bool got = false;
    uint32_t to = 0;
    // a landing is within 2 x kSnapReach of its onset (sedit_dsp.h zero_near): only those onsets are looked at first
    const uint32_t reach = 2 * kSnapReach;
    if (detents > 0) {
      for (int i = onset_at_or_after(from > reach ? from - reach : 0); i < e.on.n && !got; ++i) {
        uint32_t l = onset_landing(e.on.at[i]);
        if (l > from && (which || l < e.b)) to = l, got = true;
      }
    } else {
      for (int i = onset_at_or_after(from + reach + 1) - 1; i >= 0 && !got; --i) {
        uint32_t l = onset_landing(e.on.at[i]);
        if (l < from && (!which || l > e.a)) to = l, got = true;
      }
    }
    if (!got) {
      say("NO MORE TRANSIENTS");
      return k ? 0 : -1;
    }
    if (which) put_end(to);
    else put_start(to);
  }
  return 0;
}
CORE_COLD void pan(int detents, bool coarse) {
  if (!e.x || e.drain || !detents) return;
  const uint32_t span = e.wb - e.wa;
  if (span >= e.frames) return;   // (the whole sample shows: nowhere to go)
  int64_t step = (int64_t)(span / 16 ? span / 16 : 1) * (coarse ? 8 : 1), wa = (int64_t)e.wa + step * detents;
  if (wa < 0) wa = 0;
  if (wa + span > e.frames) wa = e.frames - span;
  if ((uint32_t)wa == e.wa) return;
  e.wa = (uint32_t)wa, e.wb = (uint32_t)wa + span;
  ol_start();
  touch();
}
CORE_COLD void zoom(int level) {
  if (!e.x || e.drain) return;
  level = level < 0 ? 0 : level > 8 ? 8 : level;
  uint32_t span = e.frames >> level;
  if (!span) span = 1;
  uint32_t c = e.moved ? e.b : e.a;
  e.wa = c > span / 2 ? c - span / 2 : 0;
  if (e.wa + span > e.frames) e.wa = e.frames - span;
  e.wb = e.wa + span;
  e.zoom = (uint8_t)level;
  ol_start();
  touch();
}
CORE_COLD void window(uint32_t wa, uint32_t wb) {
  if (!e.x || e.drain) return;
  if (wb > e.frames) wb = e.frames;
  if (wb < 1) wb = 1;
  if (wa >= wb) wa = wb - 1;
  e.wa = wa, e.wb = wb;
  uint8_t l = 0;
  while (l < 8 && (e.frames >> (l + 1)) >= wb - wa) ++l;
  e.zoom = l;
  ol_start();
  touch();
}

CORE_COLD void set_fn(int fn) {
  if (fn < 0 || fn >= kFns) return;
  e.fn = (uint8_t)fn;
  touch();
}

CORE_COLD int apply() {
  if (refuse_busy()) return -1;
  e.jfn = e.fn, e.ja = e.a, e.jb = e.b;
  e.pos = 0, e.g = 0, e.sq = 0;
  e.ka = 0, e.kb = e.frames;   // (the copy: the whole edit)
  switch (e.jfn) {
    case kCrop: e.pass = kCopy, e.ka = e.a, e.kb = e.b; break;
    case kNorm: e.pass = kPeak; break;
    case kTrim: e.pass = kFloor; break;
    case kGainUp: e.pass = kCopy, e.g = kGainUpF; break;
    case kGainDown: e.pass = kCopy, e.g = kGainDownF; break;
    default: e.pass = kCopy; break;
  }
  e.job = kApply;
  e.st = kBusy;
  e.progress = 0;
  e.msg[0] = 0;
  touch();
  return 0;
}

CORE_COLD int undo() {
  if (refuse_busy()) return -1;
  if (!e.undo) {
    say("NOTHING TO UNDO");
    return -1;
  }
  float* t = e.x;
  e.x = e.y, e.y = t;
  int tb = e.bx;
  e.bx = e.by, e.by = tb;
  bool len = e.frames != e.u_frames;
  e.frames = e.u_frames, e.a = e.u_a, e.b = e.u_b, e.dirty = e.u_dirty;
  if (len) whole_window();
  e.undo = false;
  e.msg[0] = 0;
  e.st = kBusy;
  play_changed();
  touch();
  return 0;
}

CORE_COLD int save(bool use) {
  if (refuse_busy()) return -1;
  int r = g_name.begin(e.src, e.target, sizeof e.target);
  if (r < 0) {
    say(r == -1 ? err_text(kErrName) : r == kErrBusy ? "BUSY" : "NO FOLDER");
    return -1;
  }
  e.use = use;
  e.scode = e.code;
  e.s_a = e.a, e.s_b = e.b;
  e.s_frames = e.b - e.a, e.s_rate = e.rate, e.s_ch = e.ch, e.s_bits = e.bits;
  e.spass = kName, e.pool_off = false;
  e.job = kSave;
  e.st = kSaving;
  e.progress = 0;
  e.msg[0] = 0;
  touch();
  return 0;
}

// sedit-050 #2: the voice a preview plays on: the track's; opened from the pool, the Rytm's selected track when it is a
// DSP SAMPLE or SAMPLER track (voice byte 0x20 both), else the first such track; -1 none
CORE_COLD int preview_voice() {
  if (e.vi >= 0) return e.vi;
  int sel = ui::remote_selected_track(nullptr);
  if (sel >= 0 && (ui::remote_track_machine(sel) & 0x78) == kDspSample && ui::remote_track_machine(sel) != 0x7F)
    return ui::remote_track_voice(sel);
  for (int t = 0; t < 12; ++t)
    if (ui::remote_track_machine(t) >= 0 && ui::remote_track_machine(t) != 0x7F && (ui::remote_track_machine(t) & 0x78) == kDspSample)
      return ui::remote_track_voice(t);
  return -1;
}
CORE_COLD int preview() {
  if (refuse_busy()) return -1;
  int vi = preview_voice();
  if (vi < 0) {
    say("NO SAMPLE TRACK");
    return -1;
  }
  if (e.cur < 0) {
    say(e.nolive ? "NO LIVE PLAY: RAM" : "NOT READY");
    return -1;
  }
  dsps_preview(vi, e.a, e.b);
  return 0;
}

// Rytm mods review H3: SAVE + USE's 48 writes (P11 SMP, P13 STA, P14 END) answered busy (1) are sent again every
// kUseRetryMs for up to kUseRetryForMs; still busy then: "USE: RYTM BUSY". Another status (4 locked on the steps held,
// or any refusal): "USE: RYTM REFUSED" at once. No answer (an older build): nothing said. One retry per track (a
// SAVE + USE on another track keeps the first's); a project change drops them all (use_reset).
constexpr uint32_t kUseRetryMs = 20, kUseRetryForMs = 1000;
constexpr int kUseTracks = 12;
struct UseWrites {
  uint8_t mask = 0, busy = 0;   // the writes still followed (bit i: kUseK[i]), the ones last answered busy
  uint8_t v[3] = {}, lo[3] = {};
  uint32_t t0 = 0, next = 0;
};
constexpr uint8_t kUseK[3] = {10, 12, 13};
UseWrites g_use[kUseTracks];
uint16_t g_use_on = 0;   // the tracks with writes followed

CORE_COLD void use_reset() {
  for (auto& u : g_use) u = UseWrites();
  g_use_on = 0;
}

CORE_COLD void use_write(int t, int i, int value, int lo) {
  UseWrites& u = g_use[t];
  u.v[i] = (uint8_t)value, u.lo[i] = (uint8_t)lo;
  if (ui::remote_track_write(t, kUseK[i], value, lo, 0)) u.mask |= (uint8_t)(1u << i), g_use_on |= (uint16_t)(1u << t);
}

CORE_COLD void use_poll() {
  const uint32_t now = now_ms();
  for (int t = 0; t < kUseTracks; ++t) {
    if (!(g_use_on >> t & 1)) continue;
    UseWrites& u = g_use[t];
    const bool resend = (int32_t)(now - u.next) >= 0;
    for (int i = 0; i < 3; ++i) {
      if (!(u.mask >> i & 1)) continue;
      const int a = ui::remote_track_write_answer(t, kUseK[i]);
      if (a == -1) continue;   // (owed)
      if (a == 1) {
        u.busy |= (uint8_t)(1u << i);
        if (resend) ui::remote_track_write(t, kUseK[i], u.v[i], u.lo[i], 0);
        continue;
      }
      u.mask &= (uint8_t)~(1u << i), u.busy &= (uint8_t)~(1u << i);   // done, refused or no answer (-2)
      if (a > 0) say("USE: RYTM REFUSED");   // (locked on the steps held, or refused: the track not on the row)
    }
    if (resend) u.next = now + kUseRetryMs;
    if (u.mask && now - u.t0 >= kUseRetryForMs) {
      if (u.busy & u.mask) say("USE: RYTM BUSY");
      u.mask = u.busy = 0;
    }
    if (!u.mask) g_use_on &= (uint16_t)~(1u << t);
  }
}

CORE_COLD int use_saved(int track, int code) {
  if (track < 0 || track >= kUseTracks) return -1;
  UseWrites& u = g_use[track];
  u.mask = u.busy = 0, u.t0 = now_ms(), u.next = u.t0 + kUseRetryMs;
  use_write(track, 0, code % srclib::kBankRows, srclib::lo_of(code));   // SMP: the saved row
  if (track == e.track) {
    // the saved copy's length (START..END): another than the one STA / END were for (a region short of the whole
    // always is): STA 0, END 120, the whole of the new file
    const uint32_t len = e.s_frames ? e.s_frames : e.frames;
    if (len != e.tlen) {
      use_write(track, 1, 0, 0);
      use_write(track, 2, 120, 0);
      e.tlen = len;
    }
    e.slot = slot_of(code);   // the override follows the track to the new row
    // (re-review 2: not before the save's crop: the edit is still the uncropped one; the crop's refill publishes)
    if (e.cur >= 0 && !crop_follows()) publish();
  }
  char row[8];
  pools::row_text(code, row);
  char m[32];
  sniprintf(m, sizeof m, "SAVED, T%d USES %s", track + 1, row);
  say(m);
  return 0;
}

CORE_COLD bool step() {
  e.step_frames = 0;
  if (g_use_on) use_poll();   // (no work done: a few answers looked at)
  if (g_nret) retired_step();
  switch (e.job) {
    case kLoad: return step_load();
    case kApply: return step_apply();
    case kSave: return step_save();
    default: break;
  }
  if (e.rf && e.x) {
    if (!refill_waits()) return step_refill();
    if (e.ol) return step_outline();
    if (e.on_run) return step_onsets();
    return false;   // (waiting for the voice: no work done, the engine's other work runs)
  }
  if (e.ol && e.x) return step_outline();
  if (e.on_run && e.x) return step_onsets();
  return false;
}

CORE_COLD uint32_t last_step_frames() { return e.step_frames; }

CORE_COLD void view(View* o) {
  o->st = e.st;
  sniprintf(o->name, sizeof o->name, "%s", e.name);
  sniprintf(o->msg, sizeof o->msg, "%s", e.msg);
  o->frames = e.frames, o->rate = e.rate, o->a = e.a, o->b = e.b, o->wa = e.wa, o->wb = e.wb;
  o->ch = e.ch, o->bits = e.bits, o->fn = e.fn, o->zoom = e.zoom;
  o->undo = e.undo, o->dirty = e.dirty, o->has_track = e.track >= 0;
  o->track = e.track;
  o->progress = e.progress;
  memcpy(o->lo, e.lo, sizeof o->lo);
  memcpy(o->hi, e.hi, sizeof o->hi);
  o->playhead = -1;
  o->unity = e.unity;
  o->gen = e.gen;
}

// ---- the console

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "sedit", 5) || (p[5] && p[5] != '=')) return false;
  if (p[5]) {
    const char* a = p + 6;
    auto word = [&](const char* w) {
      size_t l = strlen(w);
      if (strncmp(a, w, l) || (a[l] && a[l] != ':')) return false;
      a += l + (a[l] == ':');
      return true;
    };
    auto num = [&](int64_t* o) {
      if (!parse_i64(&a, o)) return false;
      if (*a == ':') ++a;
      return true;
    };
    int64_t x = 0, y = 0;
    bool ok = true;
    if (word("open")) {
      bool track = word("track"), code = !track && word("code");
      ok = (track || code) && num(&x) && !*a;
      // (out of range: -1, refused as such - never cast into a valid track or row)
      int k = x < 0 || x >= pools::kCodes ? -1 : (int)x;
      if (ok && track) open_track(k);
      else if (ok) open_code(k);
    } else if (word("close")) {
      close();
    } else if (word("start")) {
      if ((ok = num(&x) && !*a)) set_start(x);
    } else if (word("end")) {
      if ((ok = num(&x) && !*a)) set_end(x);
    } else if (word("region")) {
      if ((ok = num(&x) && num(&y) && !*a) && e.x && !e.drain) {
        int64_t ra = x, rb = y;
        clamp_region(e.frames, &ra, &rb);
        e.a = (uint32_t)ra, e.b = (uint32_t)rb;
        touch();
      }
    } else if (word("nudge")) {
      char m = *a;
      int which = m == 'S' || m == 's' ? 0 : m == 'E' || m == 'e' ? 1 : -1;
      if (which >= 0 && a[1] == ':') a += 2;
      else which = -1;
      bool coarse = false;
      ok = which >= 0 && num(&x);
      if (ok && *a) ok = !strcmp(a, "c"), coarse = true;
      if (ok) nudge(which, x < -1000000 ? -1000000 : x > 1000000 ? 1000000 : (int)x, coarse);
    } else if (word("jump")) {   // sedit-050 #8: FUNC + A / B, the transients
      char m = *a;
      int which = m == 'S' || m == 's' ? 0 : m == 'E' || m == 'e' ? 1 : -1;
      if (which >= 0 && a[1] == ':') a += 2;
      else which = -1;
      ok = which >= 0 && num(&x) && !*a;
      if (ok) jump(which, x < -kJumpMax ? -kJumpMax : x > kJumpMax ? kJumpMax : (int)x);
    } else if (word("pan")) {   // sedit-050 #4: knob D
      ok = num(&x);
      bool coarse = false;
      if (ok && *a) ok = !strcmp(a, "c"), coarse = true;
      if (ok) pan(x < -1000000 ? -1000000 : x > 1000000 ? 1000000 : (int)x, coarse);
    } else if (word("zoom")) {
      if ((ok = num(&x) && !*a)) zoom(x < 0 ? 0 : x > 8 ? 8 : (int)x);
    } else if (word("window")) {
      if ((ok = num(&x) && num(&y) && !*a))
        window(x < 0 ? 0 : x > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)x, y < 0 ? 0 : y > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)y);
    } else if (word("fn")) {
      int f = fn_of(a);
      if ((ok = f >= 0)) set_fn(f);
    } else if (word("apply")) {
      int f = *a ? fn_of(a) : e.fn;
      if ((ok = f >= 0) && e.st == kReady) set_fn(f);
      if (ok) apply();
    } else if (word("undo")) {
      undo();
    } else if (word("save")) {
      if (!*a) save(false);
      else if (!strcmp(a, "use")) save(true);
      else ok = false;
    } else if (word("preview")) {
      preview();
    } else {
      ok = false;
    }
    if (!ok) {
      sniprintf(out, cap, "sedit=open:track:T|open:code:C|close|start:F|end:F|region:A:B|nudge:S|E:N[:c]|jump:S|E:N|"
                          "zoom:L|pan:N[:c]|window:A:B|fn:N|NAME|apply[:NAME]|undo|save|save:use|preview");
      return true;
    }
  }
  sniprintf(out, cap, "sedit: st=%s name=%s frames=%lu ch=%u bits=%u rate=%lu a=%lu b=%lu wa=%lu wb=%lu fn=%s undo=%d "
                      "dirty=%d track=%d msg=%s",
            kStNames[e.st], e.name, (unsigned long)e.frames, e.ch, e.bits, (unsigned long)e.rate, (unsigned long)e.a,
            (unsigned long)e.b, (unsigned long)e.wa, (unsigned long)e.wb, kFnNames[e.fn], e.undo, e.dirty, e.track, e.msg);
  return true;
}

}  // namespace sedit
}  // namespace core

#endif  // CORE_MSAMP
