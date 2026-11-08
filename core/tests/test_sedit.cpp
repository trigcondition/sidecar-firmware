// core/tests/test_sedit.cpp - the sample editor's state (samp/sedit.h) through its console command, sedit=: open from a
// pool row and from a DSP SAMPLE track, the outline, crop / normalise / trim and undo, the region kept inside the sample
// after a length change, commands while busy, every step within kEditPiece frames (an 87 s stereo file), the save (a
// new kick-ed1.wav in the pool, the original untouched, the name listed without a wait), the files refused and the RAM
// given back. A card core runs the card requests (cardq::set_worker), as on the Pi.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/sha256.h"
#include "lib/srclib.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "nmach.h"
#include "pool/pcache.h"
#include "samp/dsp.h"
#include "samp/sedit.h"
#include "samp/sedit_dsp.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using SV = sedit::View;

static std::string g_card;
static uint64_t g_us = 1000000;
static constexpr uint32_t kN = 48000;

// kick.wav: 0.5 s of silence, then a 110 Hz sine decaying from 0.8 (right channel 0.9 of the left)
static float kick(int f, int c) {
  if (f < 24000) return 0.f;
  double t = (f - 24000) / 48000.0;
  return (float)(0.8 * exp(-t / 0.08) * sin(2 * M_PI * 110 * t) * (c ? 0.9 : 1.0));
}
// the value a 24-bit file holds for v, as the loader reads it back
static float q24(float v) {
  uint8_t b[3];
  samp::put24(b, v);
  int32_t s = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) >> 8;
  return s / 8388608.f;
}
static int16_t i16(float v) {   // the int16 rendering (put16's rounding and clip)
  uint8_t b[2];
  samp::put16(b, v);
  return (int16_t)(b[0] | b[1] << 8);
}

template <class F>
static void write_wavf(const char* path, uint32_t frames, int ch, int bits, F s) {
  uint8_t h[samp::kWavHeaderMax];
  int hn = samp::wav_header(h, ch, bits, 48000, frames);
  std::vector<uint8_t> b(h, h + hn);
  b.reserve(hn + (size_t)frames * ch * (bits / 8) + 1);
  uint8_t o[4];
  for (uint32_t f = 0; f < frames; ++f)
    for (int c = 0; c < ch; ++c) {
      float v = s((int)f, c);
      if (bits == 16) samp::put16(o, v);
      else samp::put24(o, v);
      b.insert(b.end(), o, o + bits / 8);
    }
  if (b.size() & 1) b.push_back(0);
  FILE* fp = fopen(host_path(path).c_str(), "wb");
  CHECK(fp);
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
}
static std::string sha(const char* path) {
  FILE* fp = fopen(host_path(path).c_str(), "rb");
  CHECK(fp);
  std::vector<uint8_t> b;
  uint8_t t[4096];
  size_t n;
  while ((n = fread(t, 1, sizeof t, fp)) > 0) b.insert(b.end(), t, t + n);
  fclose(fp);
  uint8_t d[32];
  char x[65];
  lib::sha256(b.data(), b.size(), d);
  lib::hex(d, 32, x);
  return x;
}
static bool exists(const char* path) { return access(host_path(path).c_str(), F_OK) == 0; }

// ---- the control core's pass and the card core's turn; 1 ms each
static void pass() {
  host_card_core(3);
  for (int k = 0; k < 4 && nm::step(); ++k) {}
  host_card_core(0);
  for (int k = 0; k < 16 && cardq::service(); ++k) {}
  host_card_core(3);
  for (int k = 0; k < 4 && lib::step(); ++k) {}
  g_us += 1000;
  host_fake_time(true, g_us);
}
static SV g_v;
static const SV& vw() {
  sedit::view(&g_v);
  return g_v;
}
static bool settled(sedit::State s) { return s == sedit::kReady || s == sedit::kError || s == sedit::kClosed; }
static void run() {
  for (int i = 0; i < 200000 && !settled(vw().st); ++i) pass();
  CHECK(settled(vw().st));
}
static void run_outline() {   // the outline rebuilt after a zoom / window (gen moves when it is whole)
  uint32_t g = vw().gen;
  for (int i = 0; i < 2000 && vw().gen == g; ++i) pass();
  CHECK(vw().gen != g);
}
static char g_out[512];
static const char* cmd(const std::string& c) {
  char* out = g_out;
  out[0] = 0;
  CHECK(nm::command(c.c_str(), out, sizeof g_out));
  return out;
}
static bool starts(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }
static int code(const char* row) {
  int c = -1;
  CHECK(pools::parse_row(row, &c));
  return c;
}
static void put_row(const char* row, const char* path) {
  char c[256], out[256];
  snprintf(c, sizeof c, "pool=smp:set:%s:%s", row, path);
  host_card_core(0);
  lib::touched(path, nullptr, 0);   // (a file the test wrote after the library's walk: the card core's writers say so)
  host_card_core(3);
  for (int i = 0; i < 200000 && (i < 20 || lib::busy()); ++i) pass();
  CHECK(pools::command(c, out, sizeof out));
  for (int i = 0; i < 4000 && (pools::pending() || strcmp(pools::at(pools::kSamples, code(row)).path, path)); ++i) pass();
  CHECK(!strcmp(pools::at(pools::kSamples, code(row)).path, path));
}
// the outline against ref (frames x ch floats) over the window: each column's min / max within 1 LSB (columns that
// reach into [skip_a, skip_b) left out)
static void check_outline(const std::vector<float>& ref, int ch, uint32_t skip_a = 0, uint32_t skip_b = 0) {
  const SV& v = vw();
  uint32_t span = v.wb - v.wa;
  for (int c = 0; c < sedit::kCols; ++c) {
    uint32_t s = v.wa + (uint32_t)((uint64_t)span * c / sedit::kCols), e = v.wa + (uint32_t)((uint64_t)span * (c + 1) / sedit::kCols);
    if (e <= s) e = s + 1;
    if (s < skip_b && e > skip_a) continue;
    int lo = 32767, hi = -32768;
    for (uint32_t f = s; f < e; ++f)
      for (int k = 0; k < ch; ++k) {
        int x = i16(ref[(size_t)f * ch + k]);
        if (x < lo) lo = x;
        if (x > hi) hi = x;
      }
    if (abs(v.lo[c] - lo) > 1 || abs(v.hi[c] - hi) > 1) {
      fprintf(stderr, "col %d [%u, %u): lo %d/%d hi %d/%d\n", c, s, e, v.lo[c], lo, v.hi[c], hi);
      CHECK(false);
    }
  }
}
static std::vector<float> kick_ref() {
  std::vector<float> r(kN * 2);
  for (uint32_t f = 0; f < kN; ++f)
    for (int c = 0; c < 2; ++c) r[f * 2 + c] = q24(kick((int)f, c));
  return r;
}
static int peak16() {   // the outline's largest |sample|
  int p = 0;
  for (int c = 0; c < sedit::kCols; ++c) p = std::max(p, std::max(abs(vw().lo[c]), abs(vw().hi[c])));
  return p;
}

static void t_open_code() {
  uint32_t g0 = vw().gen;
  CHECK(starts(cmd("sedit=open:code:1"), "sedit: st=loading"));
  // the open posted to the card core: a step that only waits for it says so (no work), and nm::step lets the engine's
  // other work run (core/engine.cpp idle_stream: AMP and the streams' top-up after it)
  CHECK(!sedit::step() && cardq::pending() >= 1);
  CHECK(!sedit::step());
  bool idle = false;   // (the other machines' own turns done, nm::step says no work while the open still waits)
  for (int i = 0; i < 50 && !idle; ++i) idle = !nm::step();
  CHECK(idle && vw().st == sedit::kLoading);
  run();
  const SV& v = vw();
  CHECK(v.st == sedit::kReady && v.frames == kN && v.a == 0 && v.b == kN && !v.has_track && v.track == -1);
  CHECK(!strcmp(v.name, "kick.wav") && v.ch == 2 && v.bits == 24 && v.rate == 48000 && !v.undo && !v.dirty);
  CHECK(v.wa == 0 && v.wb == kN && v.zoom == 0 && v.gen != g0 && v.playhead == -1);
  // the status line
  const char* s = cmd("sedit");
  CHECK(starts(s, "sedit: st=ready name=kick.wav frames=48000 ch=2 bits=24 rate=48000 a=0 b=48000 wa=0 wb=48000 fn=CROP"));
  CHECK(strstr(s, "undo=0 dirty=0 track=-1 msg="));
  CHECK(strstr(cmd("sedit=bogus"), "sedit=open:"));   // the usage
}

static void t_outline() {
  std::vector<float> ref = kick_ref();
  check_outline(ref, 2);
  // zoomed: 48000 >> 3 = 6000 frames centred on the marker moved last (the end, at 30000)
  cmd("sedit=end:30000");
  cmd("sedit=zoom:3");
  run_outline();
  CHECK(vw().wa == 27000 && vw().wb == 33000 && vw().zoom == 3);
  check_outline(ref, 2);
  cmd("sedit=window:47990:60000");   // past the end: clamped
  run_outline();
  CHECK(vw().wa == 47990 && vw().wb == kN);
  check_outline(ref, 2);   // (10 frames in 1024 columns)
  cmd("sedit=zoom:0");
  run_outline();
  CHECK(vw().wa == 0 && vw().wb == kN);
  cmd("sedit=region:0:48000");
}

static void t_crop_undo() {
  std::vector<float> ref = kick_ref();
  cmd("sedit=region:12000:36000");
  CHECK(starts(cmd("sedit=apply:crop"), "sedit: st=busy"));
  run();
  const SV& v = vw();
  CHECK(v.frames == 24000 && v.a == 0 && v.b == 24000 && v.undo && v.dirty && v.wa == 0 && v.wb == 24000);
  std::vector<float> cut(ref.begin() + 12000 * 2, ref.begin() + 36000 * 2);
  check_outline(cut, 2);
  cmd("sedit=undo");
  run();
  CHECK(vw().frames == kN && !vw().undo && !vw().dirty && vw().a == 12000 && vw().b == 36000);
  check_outline(ref, 2);
  CHECK(strstr(cmd("sedit=undo"), "NOTHING TO UNDO") && vw().frames == kN);
}

static void t_norm() {
  const int target = i16(sedit::kNormPeak);
  std::vector<float> ref = kick_ref();
  // a region: its peak to -0.1 dBFS, the rest as it was
  cmd("sedit=region:24000:36000");
  cmd("sedit=apply:norm");
  run();
  CHECK(vw().undo && vw().dirty && vw().frames == kN);
  CHECK(abs(peak16() - target) <= 1);
  check_outline(ref, 2, 24000, 36000);
  cmd("sedit=undo");
  run();
  check_outline(ref, 2);
  // the whole sample
  cmd("sedit=region:0:48000");
  cmd("sedit=apply:normalise");
  run();
  CHECK(abs(peak16() - target) <= 1 && vw().lo[0] == 0 && vw().hi[0] == 0);
  cmd("sedit=undo");
  run();
  // silence: nothing changes, "SILENT"
  cmd("sedit=region:0:12000");
  cmd("sedit=apply:norm");
  run();
  CHECK(!strcmp(vw().msg, "SILENT") && !vw().undo && !vw().dirty);
  check_outline(ref, 2);
}

static void t_trim() {
  std::vector<float> ref = kick_ref();
  samp::Trim t;
  CHECK(samp::trim(ref.data(), 2, kN, 48000 / 20, &t));
  uint32_t first = samp::first_above(ref.data(), 2, kN, t.floor), last = samp::last_above(ref.data(), 2, kN, t.floor);
  CHECK(t.end - t.start == (last + 480 < kN ? last + 480 : kN) - (first - 96));
  cmd("sedit=fn:trim");
  CHECK(vw().fn == sedit::kTrim);
  cmd("sedit=apply");
  run();
  const SV& v = vw();
  CHECK(v.frames == t.end - t.start && v.a == 0 && v.b == v.frames && v.undo);
  // the sample's own frames from t.start, the last 480 ramped from 1 down to 0
  uint32_t n = t.end - t.start;
  std::vector<float> cut(ref.begin() + t.start * 2, ref.begin() + t.end * 2);
  for (uint32_t f = n - 480; f < n; ++f)
    for (int c = 0; c < 2; ++c) cut[f * 2 + c] *= 1.0f - (float)(f - (n - 480)) / 479.0f;
  check_outline(cut, 2);
  cmd("sedit=window:0:4000");   // (the start, closer)
  run_outline();
  check_outline(cut, 2);
  cmd("sedit=window:" + std::to_string(n - 600) + ":" + std::to_string(n));   // (the fade, closer)
  run_outline();
  check_outline(cut, 2);
  CHECK(vw().lo[1023] == 0 && vw().hi[1023] == 0);
  cmd("sedit=undo");
  run();
  CHECK(vw().frames == kN);
  cmd("sedit=zoom:0");
  run_outline();
}

static void t_silent_file() {
  CHECK(starts(cmd("sedit=open:code:2"), "sedit: st=loading"));
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == 4800 && vw().ch == 1 && vw().bits == 16);
  cmd("sedit=apply:trim");
  run();
  CHECK(!strcmp(vw().msg, "ALL SILENT") && vw().frames == 4800 && !vw().undo);
  cmd("sedit=apply:norm");
  run();
  CHECK(!strcmp(vw().msg, "SILENT") && !vw().undo);
  cmd("sedit=apply:fadein");   // (a fade of silence: no NaN, nothing to see)
  run();
  CHECK(vw().undo && vw().lo[0] == 0 && vw().hi[1023] == 0);
}

static void t_clamp_after_length_change() {
  cmd("sedit=open:code:1");
  run();
  cmd("sedit=region:12000:36000");
  cmd("sedit=apply:crop");
  run();
  CHECK(vw().frames == 24000);
  cmd("sedit=end:40000");   // (a touch drag computed on the old length)
  CHECK(vw().b == 24000);
  cmd("sedit=start:30000");
  CHECK(vw().a == 23999 && vw().b == 24000);
  cmd("sedit=region:30000:20000");   // b < a: swapped, then clamped
  CHECK(vw().a == 20000 && vw().b == 24000);
  cmd("sedit=start:-5");
  CHECK(vw().a == 0);
  cmd("sedit=region:5:5");   // empty: one frame
  CHECK(vw().a == 5 && vw().b == 6);
  cmd("sedit=window:30000:40000");   // past the end
  run_outline();
  CHECK(vw().wb == 24000 && vw().wa < vw().wb);
  // nudges: a detent is (wb - wa) / 128 frames, coarse 8 of them
  cmd("sedit=zoom:0");
  run_outline();
  cmd("sedit=region:1000:20000");
  cmd("sedit=nudge:S:3");
  CHECK(vw().a == 1000 + 3 * (24000 / 128));
  cmd("sedit=nudge:E:-1:c");
  CHECK(vw().b == 20000 - 8 * (24000 / 128));
  cmd("sedit=nudge:E:100:c");
  CHECK(vw().b == 24000);
  cmd("sedit=undo");
  run();
  CHECK(vw().frames == kN && vw().a == 12000 && vw().b == 36000);
}

static void t_busy_and_stale() {
  cmd("sedit=region:0:48000");
  CHECK(starts(cmd("sedit=apply:norm"), "sedit: st=busy"));   // (no steps yet)
  CHECK(strstr(cmd("sedit=apply:rev"), "BUSY"));
  CHECK(strstr(cmd("sedit=save"), "BUSY"));
  CHECK(strstr(cmd("sedit=open:code:1"), "BUSY"));
  CHECK(strstr(cmd("sedit=undo"), "BUSY"));
  CHECK(vw().st == sedit::kBusy);
  run();
  CHECK(vw().st == sedit::kReady && vw().undo);
  // a track that is not DSP SAMPLE: refused, the editor as it was
  const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 2, 49, (uint8_t)(kFm | 0), 49, (uint8_t)(kDspSample | 0), 0xF7};
  engine_sysex(m33, sizeof m33);
  CHECK((ui::remote_track_machine(0) & 0x78) == kFm && (ui::remote_track_machine(1) & 0x78) == kDspSample);
  CHECK(strstr(cmd("sedit=open:track:0"), "NOT A DSP SAMPLE TRACK"));
  CHECK(vw().st == sedit::kReady && vw().undo && !vw().has_track && vw().frames == kN);
  CHECK(strstr(cmd("sedit=open:track:5"), "NOT A DSP SAMPLE TRACK"));   // (no 33 named it)
  // track 1 (SD, voice 2) on DSP SAMPLE, SMP A001, STA 0 END 60: the region is the first half
  int vi = ui::remote_track_voice(1);
  v[vi].p[10] = 1.0f;
  v[vi].p[12] = 0, v[vi].p[13] = 60;
  CHECK(starts(cmd("sedit=open:track:1"), "sedit: st=loading"));
  run();
  CHECK(vw().st == sedit::kReady && vw().has_track && vw().track == 1 && !vw().undo && !strcmp(vw().name, "kick.wav"));
  CHECK(vw().a == 0 && vw().b == 24000);
  CHECK(strstr(cmd("sedit"), "track=1"));
}

static void t_save() {
  const std::string k0 = sha("/Samples/k/kick.wav");
  cmd("sedit=open:code:1");
  run();
  cmd("sedit=region:0:48000");
  cmd("sedit=apply:gain-");   // (an unchanged copy would be kick.wav's own content: the pool's row A001)
  run();
  CHECK(starts(cmd("sedit=save"), "sedit: st=saving"));
  // the name's listing is posted, not waited for: the step comes back with it still on the card core's queue (and says
  // it did no work)
  CHECK(!sedit::step());
  CHECK(vw().st == sedit::kSaving && cardq::pending() >= 1);
  CHECK(!sedit::step());
  CHECK(vw().st == sedit::kSaving && cardq::pending() >= 1);
  run();
  CHECK(vw().st == sedit::kReady);
  CHECK(exists("/Samples/k/kick-ed1.wav") && !exists("/Samples/k/kick-ed1.wav.tmp"));
  CHECK(sha("/Samples/k/kick.wav") == k0);
  int row = -1;
  for (int r = 1; r < 128; ++r)
    if (!strcmp(pools::at(pools::kSamples, pools::code(0, r)).path, "/Samples/k/kick-ed1.wav")) row = r;
  CHECK(row > 0);
  CHECK(!strcmp(vw().name, "kick-ed1.wav") && !vw().undo && !vw().dirty && vw().frames == kN);
  // a second edit of the copy saves as kick-ed2.wav
  cmd("sedit=apply:rev");
  run();
  cmd("sedit=save");
  run();
  CHECK(exists("/Samples/k/kick-ed2.wav") && !strcmp(vw().name, "kick-ed2.wav"));
  CHECK(sha("/Samples/k/kick.wav") == k0);
  // SAVE + USE from a track: saved, in the pool, the editor on the copy (the track's own write is Task 4's)
  cmd("sedit=open:track:1");
  run();
  cmd("sedit=region:24000:48000");   // (the track's region, 0..24000, is the silence)
  cmd("sedit=apply:fadeout");
  run();
  CHECK(starts(cmd("sedit=save:use"), "sedit: st=saving"));
  run();
  CHECK(exists("/Samples/k/kick-ed3.wav") && !strcmp(vw().name, "kick-ed3.wav") && vw().has_track && vw().track == 1);
  CHECK(!vw().undo && !vw().dirty);
  row = -1;
  for (int r = 1; r < 128; ++r)
    if (!strcmp(pools::at(pools::kSamples, pools::code(0, r)).path, "/Samples/k/kick-ed3.wav")) row = r;
  CHECK(row > 0);
  CHECK(sha("/Samples/k/kick.wav") == k0);
  // closed while saving: the save finishes out of sight and the copy goes into the source's bank (B), not bank A
  write_wavf("/Samples/k/snare.wav", 9600, 1, 16, [](int f, int) { return (f % 96 - 48) / 100.f; });
  put_row("B001", "/Samples/k/snare.wav");
  cmd("sedit=open:code:" + std::to_string(code("B001")));
  run();
  cmd("sedit=apply:gain-");
  run();
  CHECK(starts(cmd("sedit=save"), "sedit: st=saving"));
  for (int i = 0; i < 100 && !exists("/Samples/k/snare-ed1.wav.tmp"); ++i) {   // until the copy is being written
    sedit::step();
    host_card_core(0);
    while (cardq::service()) {}
    host_card_core(3);
  }
  CHECK(exists("/Samples/k/snare-ed1.wav.tmp") && vw().st == sedit::kSaving);
  cmd("sedit=close");
  CHECK(vw().st == sedit::kClosed);
  for (int i = 0; i < 2000; ++i) pass();
  CHECK(exists("/Samples/k/snare-ed1.wav"));
  row = -1;
  for (int c = 0; c < pools::kCodes; ++c)
    if (!strcmp(pools::at(pools::kSamples, c).path, "/Samples/k/snare-ed1.wav")) row = c;
  CHECK(row >= 0 && pools::bank_of(row) == 1);
  CHECK(vw().st == sedit::kClosed);
}

// The library publishes its index later than the card core's lib::touched (on the Pi from lib_pi::poll, 250 ms or more
// apart on a big index): a save whose steps run before that publish waits for it (saying no work), then pools the copy
static void t_save_before_publish() {
  cmd("sedit=open:code:1");
  run();
  cmd("sedit=apply:gain+");
  run();
  CHECK(starts(cmd("sedit=save"), "sedit: st=saving"));
  std::string want;
  for (int k = 1; k < 20 && want.empty(); ++k) {   // the name it will take: the first kick-ed<k>.wav not there
    std::string p = "/Samples/k/kick-ed" + std::to_string(k) + ".wav";
    if (!exists(p.c_str())) want = p;
  }
  // the control core's steps and the card core's turns, the library's step never: the file written and reported
  for (int i = 0; i < 2000; ++i) {
    sedit::step();
    host_card_core(0);
    while (cardq::service()) {}
    host_card_core(3);
    g_us += 1000;
    host_fake_time(true, g_us);
  }
  CHECK(exists(want.c_str()));
  CHECK(vw().st == sedit::kSaving && !sedit::step());   // waiting for the publish, not pooled as unknown
  CHECK(strcmp(vw().msg, pools::result_text(pools::kErrPath)));
  run();
  CHECK(vw().st == sedit::kReady && !strcmp(vw().msg, "SAVED"));
  int row = -1;
  for (int c = 0; c < pools::kCodes; ++c)
    if (!strcmp(pools::at(pools::kSamples, c).path, want.c_str())) row = c;
  CHECK(row >= 0);
  cmd("sedit=close");
  run();
}

static void t_step_bound() {
  // 87 s of stereo (kMaxFrames): every step of the load, the outline and an apply within kEditPiece frames
  write_wavf("/Samples/k/long.wav", pcache::kMaxFrames, 2, 16, [](int f, int c) { return ((f % 4800) - 2400) / 4800.f * (c ? -1 : 1); });
  put_row("A010", "/Samples/k/long.wav");
  cmd("sedit=open:code:10");
  int steps = 0;
  for (int i = 0; i < 400000 && !settled(vw().st); ++i, ++steps) {
    pass();
    CHECK(sedit::last_step_frames() <= sedit::kEditPiece);
  }
  CHECK(vw().st == sedit::kReady && vw().frames == pcache::kMaxFrames);
  CHECK(steps > (int)(pcache::kMaxFrames / sedit::kEditPiece));
  cmd("sedit=apply:gain+");
  // the other machines keep their turns while the edit runs: a browser's listing is read meanwhile
  uint32_t ls0 = srclib::stats().steps;
  CHECK(srclib::command("browse=gr:cd:/Samples/k", g_out, sizeof g_out));
  for (int i = 0; i < 10; ++i) nm::step();
  CHECK(vw().st == sedit::kBusy && srclib::stats().steps > ls0);
  int busy = 0;
  uint32_t most = 0;
  for (int i = 0; i < 400000 && vw().st == sedit::kBusy; ++i) {
    CHECK(sedit::step());
    CHECK(sedit::last_step_frames() <= sedit::kEditPiece);
    if (sedit::last_step_frames() > most) most = sedit::last_step_frames();
    ++busy;
  }
  CHECK(vw().st == sedit::kReady && vw().undo);
  CHECK(most == sedit::kEditPiece && busy >= 2 * (int)(pcache::kMaxFrames / sedit::kEditPiece));   // copy + gain at least
  cmd("sedit=close");
  run();
  unlink(host_path("/Samples/k/long.wav").c_str());
}

static void t_refused() {
  // longer than kMaxFrames
  write_wavf("/Samples/k/toolong.wav", pcache::kMaxFrames + 1, 1, 16, [](int, int) { return 0.f; });
  put_row("A011", "/Samples/k/toolong.wav");
  uint64_t priv0 = pcache::stats().priv;
  cmd("sedit=open:code:11");
  run();
  CHECK(vw().st == sedit::kError && !strcmp(vw().msg, "TOO LONG (MAX 87 S)") && pcache::stats().priv == priv0);
  unlink(host_path("/Samples/k/toolong.wav").c_str());
  // the pool RAM too full for the edit and its spare
  pcache::Stats s = pcache::stats();
  void* p = nullptr;
  int hog = pcache::alloc_private(s.budget - s.used - kN * 2 * 4 - 65536, &p);
  CHECK(hog >= 0);
  uint64_t priv1 = pcache::stats().priv;
  cmd("sedit=open:code:1");
  run();
  CHECK(vw().st == sedit::kError && !strcmp(vw().msg, "NO RAM") && pcache::stats().priv == priv1);
  pcache::release(hog);
  CHECK(pcache::stats().priv == priv0);
  // an empty row, OFF
  CHECK(strstr(cmd("sedit=open:code:100"), "NO SAMPLE"));
  CHECK(strstr(cmd("sedit=open:code:128"), "NO SAMPLE"));
  CHECK(strstr(cmd("sedit=open:code:4294967297"), "NO SAMPLE"));   // (2^32 + 1: not row 1)
  CHECK(strstr(cmd("sedit=open:track:4294967297"), "NOT A DSP SAMPLE TRACK"));   // (not track 1)
  CHECK(strstr(cmd("sedit=open:code:99999"), "NO SAMPLE") && strstr(cmd("sedit=open:code:-3"), "NO SAMPLE"));
}

static void t_close_frees() {
  cmd("sedit=close");
  run();
  uint64_t priv0 = pcache::stats().priv;
  cmd("sedit=open:code:1");
  run();
  CHECK(pcache::stats().priv > priv0);
  cmd("sedit=apply:rev");
  run();
  CHECK(starts(cmd("sedit=close"), "sedit: st=closed"));
  CHECK(pcache::stats().priv == priv0);
  // closed mid-load: the load finishes out of sight, then its block goes too
  cmd("sedit=open:code:1");
  pass();
  cmd("sedit=close");
  CHECK(vw().st == sedit::kClosed);
  for (int i = 0; i < 2000; ++i) pass();
  CHECK(pcache::stats().priv == priv0 && vw().st == sedit::kClosed);
  CHECK(strstr(cmd("sedit=apply"), "BUSY"));   // (nothing open)
}

int main() {
  char tmpl[] = "/tmp/sedit-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/k' '" + g_card + "/.rytm'").c_str()) == 0);
  setenv("CORE_POOL_RAM_MB", "160", 1);   // (the 87 s stereo edit and its spare: 64 MB)
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  hooks.to_rytm = [](const uint8_t*, int) {};
  write_wavf("/Samples/k/kick.wav", kN, 2, 24, kick);
  write_wavf("/Samples/k/silent.wav", 4800, 1, 16, [](int, int) { return 0.f; });
  cardq::reset();
  cardq::set_worker(true);   // the Pi: a card core runs every card request (pass() is its turn)
  CHECK(engine_begin() && card_ok);
  snprintf(core::project, sizeof core::project, "LIVE");
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) pass();
  for (int i = 0; i < 4000 && !(pools::ready() && !strcmp(pools::project(), "LIVE")); ++i) pass();
  CHECK(pools::ready());
  put_row("A001", "/Samples/k/kick.wav");
  put_row("A002", "/Samples/k/silent.wav");
  CHECK(vw().st == sedit::kClosed);

  t_open_code();
  t_outline();
  t_crop_undo();
  t_norm();
  t_trim();
  t_silent_file();
  t_clamp_after_length_change();
  t_busy_and_stale();
  t_save();
  t_save_before_publish();
  t_step_bound();
  t_refused();
  t_close_frees();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_sedit: PASS\n");
  return 0;
}
