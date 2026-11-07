// core/samp/sedit.cpp - see sedit.h.
#include "sedit.h"

#if CORE_MSAMP

#include <string.h>
#include <strings.h>

#include "../lib/pools.h"
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
// A save's
enum SavePass : uint8_t { kName, kWrite };

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
  char target[cardq::kPathMax] = "";
  // a close while a card job runs: the job finishes out of sight, then everything goes
  bool drain = false;
  // the outline: built into wlo / whi a few columns a step, published to lo / hi when whole
  bool ol = false;
  int col = 0;
  int16_t lo[kCols] = {}, hi[kCols] = {}, wlo[kCols] = {}, whi[kCols] = {};
  uint32_t gen = 0, step_frames = 0;
};
Ed e;
Loader g_load;
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
// The private seam after every change of the edit (apply, undo, crop): the outline rebuilt. (Task 4: the int16 refill of
// the play buffer DSP SAMPLE reads, then the outline.)
CORE_COLD void play_changed() { ol_start(); }

CORE_COLD void clamp() {   // the region within the sample (sedit_dsp.h clamp_region)
  int64_t a = e.a, b = e.b;
  clamp_region(e.frames, &a, &b);
  e.a = (uint32_t)a, e.b = (uint32_t)b;
}
CORE_COLD void whole_window() {
  e.wa = 0, e.wb = e.frames, e.zoom = 0;
}

CORE_COLD void release_all() {
  if (e.bx >= 0) pcache::release(e.bx);
  if (e.by >= 0) pcache::release(e.by);
  e.bx = e.by = -1;
  e.x = e.y = nullptr;
}
CORE_COLD void fail(const char* m) {   // the open or the job ends here: nothing left open
  release_all();
  e.job = kNoJob;
  e.ol = false;
  e.st = kError;
  e.frames = 0;
  say(m);
}
// The job ends: ready once the outline is whole
CORE_COLD void job_done() {
  e.job = kNoJob;
  e.progress = 100;
  if (!e.ol) e.st = kReady;
  touch();
}
CORE_COLD bool refuse_busy() {
  if (e.st == kReady && !e.drain) return false;
  say("BUSY");
  return true;
}

// ---- the load

CORE_COLD bool step_load() {
  uint32_t before = g_load.loaded();
  int r = g_load.step();
  e.step_frames = g_load.loaded() - before;
  if (r > 0) {
    if (g_load.frames) set_progress(g_load.loaded(), g_load.frames);
    return true;
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
  ol_start();
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
        uint32_t pre = samp::kRate / 500;   // 2 ms before it (samp::trim's)
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
        uint32_t last = s + l, post = samp::kRate / 100;   // 10 ms after it (samp::trim's)
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
  int code = -1;
  int r = pools::add_file(pools::kSamples, pools::bank_of(e.code < 0 ? 0 : e.code), e.target, &code);
  if (r == pools::kErrFull) r = pools::add_take(pools::kSamples, e.target, &code);
  if (e.drain) return;
  sniprintf(e.src, sizeof e.src, "%s", e.target);
  const char* sl = strrchr(e.src, '/');
  sniprintf(e.name, sizeof e.name, "%s", sl ? sl + 1 : e.src);
  e.undo = e.dirty = false;
  if (r == pools::kOk || r == pools::kExisting) {
    e.code = code;
    say("SAVED");
    if (e.use && e.track >= 0) use_saved(e.track, code);
  } else {
    e.code = -1;
    say(pools::result_text(r));
  }
}

CORE_COLD bool step_save() {
  if (e.spass == kName) {
    e.step_frames = 0;
    int r = g_name.step();
    if (r > 0) return true;
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
    r = g_save.begin(e.target, e.x, e.ch, e.bits, e.rate, e.frames);
    if (r < 0) {
      e.job = kNoJob, e.st = kReady;
      say(err_text(r));
      return true;
    }
    e.spass = kWrite;
    return true;
  }
  uint32_t before = g_save.written(), y0 = g_save.yields;
  int r = g_save.step();
  e.step_frames = g_save.written() - before;
  if (r > 0) {
    set_progress(g_save.written(), e.frames);
    return g_save.yields == y0;   // (held back for a stream: no work done)
  }
  e.job = kNoJob;
  if (r == 0) saved();
  if (e.drain) {
    e.drain = false;
    release_all();
    return true;
  }
  e.st = kReady;
  if (r < 0) say(err_text(r));
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
    e.wlo[e.col] = pcm16(mn), e.whi[e.col] = pcm16(mx);
    n += t - s;
    ++e.col;
  }
  e.step_frames = n;
  if (e.col >= kCols) {
    memcpy(e.lo, e.wlo, sizeof e.lo);
    memcpy(e.hi, e.whi, sizeof e.hi);
    e.ol = false;
    if (e.st == kLoading || e.st == kBusy) e.st = kReady;
    touch();
  }
  return true;
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
  if (e.drain || (e.st != kReady && e.st != kClosed && e.st != kError)) {
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
    say("NOT A DSP SAMPLE TRACK");
    return -1;
  }
  int vi = ui::remote_track_voice(track);
  int slot = vi < 0 ? 0 : pool_slot(v[vi].p[10]);   // SMP (P11)
  if (slot <= 0) {
    say("NO SAMPLE");
    return -1;
  }
  int code = pools::code((slot - 1) / kPool, (slot - 1) % kPool + 1);
  return start_open(code, track, v[vi].p[12], v[vi].p[13]);
}

CORE_COLD int open_code(int code) { return start_open(code, -1, 0, 120); }

CORE_COLD void close() {
  // a card job in flight (the load, the name's listing, the save) is finished by step() first; then the buffers go
  if (e.job == kLoad || e.job == kSave) e.drain = true;
  else {
    release_all();
    e.job = kNoJob;
  }
  e.ol = false;
  e.st = kClosed;
  e.frames = 0, e.a = e.b = e.wa = e.wb = 0;
  e.undo = e.dirty = false;
  e.track = -1, e.code = -1;
  e.progress = 0;
  touch();
}

CORE_COLD void set_start(int64_t f) {
  if (!e.x || e.drain) return;
  e.a = f < 0 ? 0 : f > e.frames ? e.frames : (uint32_t)f;
  if (e.a >= e.b) e.a = e.b ? e.b - 1 : 0;
  clamp();
  e.moved = 0;
  touch();
}
CORE_COLD void set_end(int64_t f) {
  if (!e.x || e.drain) return;
  e.b = f < 0 ? 0 : f > e.frames ? e.frames : (uint32_t)f;
  if (e.b <= e.a) e.b = e.a + 1;
  clamp();
  e.moved = 1;
  touch();
}
CORE_COLD void nudge(int which, int detents, bool coarse) {
  uint32_t step = (e.wb - e.wa) / 128;
  int64_t d = (int64_t)(step ? step : 1) * detents * (coarse ? 8 : 1);
  if (which) set_end((int64_t)e.b + d);
  else set_start((int64_t)e.a + d);
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
  e.spass = kName;
  e.job = kSave;
  e.st = kSaving;
  e.progress = 0;
  e.msg[0] = 0;
  touch();
  return 0;
}

CORE_COLD int preview() {
  if (refuse_busy()) return -1;
  if (e.track < 0) {
    say("NO TRACK");
    return -1;
  }
  return 0;   // (the region played on the track's voice: Task 4's dsps_preview)
}

CORE_COLD int use_saved(int track, int code) {
  // Task 4 writes the track's SMP to the saved row (and STA / END when the length changed); here the request is noted
  char row[8];
  pools::row_text(code, row);
  char m[32];
  sniprintf(m, sizeof m, "SAVED, T%d USES %s", track + 1, row);
  say(m);
  return 0;
}

CORE_COLD bool step() {
  e.step_frames = 0;
  switch (e.job) {
    case kLoad: return step_load();
    case kApply: return step_apply();
    case kSave: return step_save();
    default: break;
  }
  if (e.ol && e.x) return step_outline();
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
      if (ok && track) open_track((int)x);
      else if (ok) open_code((int)x);
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
      if (ok) nudge(which, (int)x, coarse);
    } else if (word("zoom")) {
      if ((ok = num(&x) && !*a)) zoom((int)x);
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
      sniprintf(out, cap, "sedit=open:track:T|open:code:C|close|start:F|end:F|region:A:B|nudge:S|E:N[:c]|zoom:L|"
                          "window:A:B|fn:N|NAME|apply[:NAME]|undo|save|save:use|preview");
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
