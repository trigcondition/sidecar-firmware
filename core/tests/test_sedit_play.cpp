// core/tests/test_sedit_play.cpp - the sample editor's live play (samp/sedit.h, voices.h dsps_override): while the
// editor is open, the DSP SAMPLE track it was opened from plays the edit from RAM at its next trig; a note already
// sounding keeps the buffer it started on, which is never rewritten under it (the refill waits); a stopped voice's
// buffer is let go, so the wait ends; a crop plays whole; close gives the original back and the RAM; SAVE + USE writes
// the track's SMP (and STA / END after a length change) to the Rytm; an SMP p-lock to another sample plays that sample;
// the render allocates nothing; preview plays the region once. Track 0 (voice 0) is DSP SAMPLE, SMP A001 = kick.wav, a
// constant 0.25 (48000 frames, mono, 16-bit), MOD ONE.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <functional>
#include <new>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "nmach.h"
#include "pool/pcache.h"
#include "pool/pool.h"
#include "samp/dsp.h"
#include "samp/sedit.h"
#include "samp/sedit_dsp.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using SV = sedit::View;

// ---- the allocation counter (the render path must not allocate)
static long g_news = 0;
void* operator new(size_t n) {
  ++g_news;
  void* p = malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) {
  ++g_news;
  void* p = malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

static std::string g_card;
static uint64_t g_us = 1000000;
static constexpr uint32_t kN = 48000;

template <class F>
static void write_wav16(const char* path, uint32_t frames, F s) {
  uint8_t h[samp::kWavHeaderMax];
  int hn = samp::wav_header(h, 1, 16, 48000, frames);
  std::vector<uint8_t> b(h, h + hn);
  uint8_t o[2];
  for (uint32_t f = 0; f < frames; ++f) samp::put16(o, s((int)f)), b.insert(b.end(), o, o + 2);
  FILE* fp = fopen(host_path(path).c_str(), "wb");
  CHECK(fp);
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
}

// ---- the Rytm's messages out (48 1 <track> <param> <value> [<fraction>]: a track's knob written)
static std::vector<std::vector<uint8_t>> g_sent;
static void out(const uint8_t* m, int n) { g_sent.emplace_back(m, m + n); }
static bool sent48(int t, int k, int value, int lo) {
  for (auto& m : g_sent)
    if (m.size() >= 10 && m[4] == 0x48 && m[6] == t && m[7] == k && m[8] == value && (m.size() > 10 ? m[9] : 0) == lo)
      return true;
  return false;
}
static int count48() {
  int n = 0;
  for (auto& m : g_sent) n += m.size() >= 10 && m[4] == 0x48;
  return n;
}

// ---- the control core's pass and the card core's turn; 1 ms each
static void pass() {
  host_card_core(3);
  for (int k = 0; k < 4; ++k) idle_stream();
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
static char g_out[512];
static const char* cmd(const std::string& c) {
  g_out[0] = 0;
  CHECK(nm::command(c.c_str(), g_out, sizeof g_out));
  return g_out;
}
static int code(const char* row) {
  int c = -1;
  CHECK(pools::parse_row(row, &c));
  return c;
}
static void put_row(const char* row, const char* path) {
  char c[256], o[256];
  snprintf(c, sizeof c, "pool=smp:set:%s:%s", row, path);
  host_card_core(0);
  lib::touched(path, nullptr, 0);
  host_card_core(3);
  for (int i = 0; i < 200000 && (i < 20 || lib::busy()); ++i) pass();
  CHECK(pools::command(c, o, sizeof o));
  for (int i = 0; i < 4000 && (pools::pending() || strcmp(pools::at(pools::kSamples, code(row)).path, path)); ++i) pass();
  CHECK(!strcmp(pools::at(pools::kSamples, code(row)).path, path));
}

// ---- voice 0: DSP SAMPLE, P9..P16 TUN FIN SMP MOD STA END LOP LEV; MIX 0 (the sample alone)
static float g_smp = 1, g_sta = 0, g_end = 120;
static int g_smpl = 0;   // 1 SAMPLER: the reports carry its sound tag (53 41; P12 PLAY 0 FWD, P15 LOOP 0); 2 DSP SAMPLE's (00 00)
static std::vector<uint8_t> report(int cmd) {
  float p[16] = {0, 64, 64, 64, 0, 0, 64, 100, 64, 64, g_smp, 0, g_sta, g_end, 0, 100};
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(kDspSample | 0)};
  if (cmd == 0x04) m.push_back(60), m.push_back(100);
  for (float x : p) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  if (g_smpl) m.push_back(g_smpl == 1 ? 0x53 : 0), m.push_back(g_smpl == 1 ? 0x41 : 0);
  m.push_back(0xf7);
  return m;
}
static void params() {
  auto m = report(0x03);
  engine_sysex(m.data(), (int)m.size());
}
// voice vi's DSP SAMPLE trig on SMP smp (STA 0, END 120)
static void trig_voice(int vi, float smp) {
  float p[16] = {0, 64, 64, 64, 0, 0, 64, 100, 64, 64, smp, 0, 0, 120, 0, 100};
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x04, (uint8_t)(kDspSample | vi), 60, 100};
  for (float x : p) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void trig() {
  auto m = report(0x04);
  engine_sysex(m.data(), (int)m.size());
}

constexpr int kRows = kBlock * 400;
static int16_t ring[kRows][kVoices];
static void render(int blocks) {   // from row 0, wrapping
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < blocks; ++b) render_voices(ring, kRows, (b % 400) * kBlock, 0, kVoices, mach);
}
// The sample's own level over the first frames rows after a trig (MIX 0: tanh(x) * 0.7 full scale, inverted)
static double level(int frames) {
  double s = 0;
  for (int i = 0; i < frames; ++i) s += atanh(ring[i][0] / (0.7 * 32767.0));
  return s / frames;
}
// Frames voice 0 sounds from a trig on (rendered in blocks of kBlock until it stops)
static uint32_t sounding() {
  uint64_t mach[kMachines] = {0};
  uint32_t n = 0;
  for (int b = 0; b < 10000; ++b) {
    render_voices(ring, kRows, 0, 0, kVoices, mach);
    int z = 0;
    for (int i = 0; i < kBlock; ++i) z += ring[i][0] != 0;
    if (!z) break;
    n += z;
  }
  return n;
}
static void until_end() {   // the ONE note played out
  for (int i = 0; i < 10000 && engines[0].playing(); ++i) render(1);
  CHECK(!engines[0].playing());
}
static uint64_t checksum(const int16_t* p, uint32_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; ++i) h = (h ^ (uint16_t)p[i]) * 1099511628211ull;
  return h;
}
// voice 0 plays sample smp (its P11) from the sample RAM, its psram copy loaded first
static void warm(float smp) {
  g_smp = smp;
  params();
  for (int i = 0; i < 4000; ++i) {
    trig();
    pass();
    if (from_ram[0]) break;
  }
  CHECK(from_ram[0]);
}

static uint64_t g_priv0;
static const int16_t* g_orig;   // A001's sample RAM copy

static void t_plays_edit() {
  warm(1);
  g_orig = engines[0].pcm();
  trig();
  render(32);
  CHECK(fabs(level(256) - 0.25) < 0.01);
  g_priv0 = pcache::stats().priv;
  CHECK(!strncmp(cmd("sedit=open:track:0"), "sedit: st=loading", 17));
  run();
  CHECK(vw().st == sedit::kReady && vw().track == 0 && vw().frames == kN);
  // the play buffers: 2 x (frames + 1) x 2 bytes over the edit and spare
  CHECK(pcache::stats().priv >= g_priv0 + 2ull * kN * 4 + 2ull * (kN + 1) * 2);
  trig();
  render(32);
  CHECK(from_ram[0] && engines[0].pcm() != g_orig);   // the editor's play buffer
  CHECK(fabs(level(256) - 0.25) < 0.01);
  cmd("sedit=apply:gain+");
  run();
  render(32);   // (the note playing on is the old buffer)
  CHECK(fabs(level(256) - 0.25) < 0.01);
  trig();      // heard at the next trig
  render(32);
  CHECK(fabs(level(256) - 0.25 * 1.41254) < 0.01);
}

static void t_swap_busy_note() {
  trig();
  render(2);
  const int16_t* p0 = engines[0].pcm();
  const uint64_t h0 = checksum(p0, kN + 1);
  cmd("sedit=region:0:48000");
  cmd("sedit=apply:norm");   // the note still sounding, no new trig
  for (int i = 0; i < 100; ++i) pass();
  CHECK(vw().st == sedit::kReady);
  CHECK(engines[0].pcm() == p0 && engines[0].playing());   // the note keeps its buffer
  CHECK(dsps_reads(p0, p0 + kN + 1) && !dsps_reads(p0 + kN + 1, p0 + kN + 2));
  CHECK(checksum(p0, kN + 1) == h0);                       // nothing written into it
  render(32);
  CHECK(fabs(level(256) - 0.25 * 1.41254) < 0.01);
  // the next refill's buffer is the one the note reads: it waits
  cmd("sedit=apply:rev");
  for (int i = 0; i < 100; ++i) pass();
  CHECK(vw().st == sedit::kBusy && engines[0].pcm() == p0 && checksum(p0, kN + 1) == h0);
  CHECK(!sedit::step());   // (the wait is no work: the engine's AMP and stream top-ups run meanwhile)
  CHECK(!strncmp(cmd("sedit=apply:gain-"), "sedit: st=busy", 14));   // (refused while it waits: BUSY)
  CHECK(!strcmp(vw().msg, "BUSY"));
  trig();   // the next trig moves the voice to the normalised buffer: the wait ends
  CHECK(engines[0].pcm() != p0);
  run();
  CHECK(vw().st == sedit::kReady);
  render(32);
  CHECK(fabs(level(256) - sedit::kNormPeak) < 0.01);
  trig();   // the reversed edit (a constant: the same level)
  render(32);
  CHECK(engines[0].pcm() == p0 && fabs(level(256) - sedit::kNormPeak) < 0.01);
}

static void t_stopped_voice_detached() {
  trig();
  until_end();
  const int16_t* held = engines[0].pcm();
  cmd("sedit=apply:gain-");   // into the other buffer
  run();
  CHECK(vw().st == sedit::kReady);
  CHECK(engines[0].pcm() == held);
  cmd("sedit=apply:gain+");   // into the one the stopped voice still holds: let go, no wait
  run();
  CHECK(vw().st == sedit::kReady && engines[0].pcm() == nullptr);
  trig();
  render(32);
  CHECK(fabs(level(256) - sedit::kNormPeak * 0.70795 * 1.41254) < 0.01);
}

static void t_close_reverts() {
  trig();
  render(2);   // (a note on the editor's buffer while it closes)
  cmd("sedit=close");
  CHECK(vw().st == sedit::kClosed);
  CHECK(pcache::stats().priv > g_priv0);   // the buffer the note reads: kept until it is let go
  trig();
  render(32);
  CHECK(engines[0].pcm() == g_orig && fabs(level(256) - 0.25) < 0.01);
  for (int i = 0; i < 10; ++i) pass();
  CHECK(pcache::stats().priv == g_priv0);
}

static void t_crop_plays_whole() {
  g_sta = 30, g_end = 90;
  params();
  cmd("sedit=open:track:0");
  run();
  const uint32_t a = (uint32_t)(30 / 120.0f * (kN - 1)), b = (uint32_t)(90 / 120.0f * (kN - 1)) + 1;
  CHECK(vw().a == a && vw().b == b);
  trig();   // the edit, before a crop: STA..END as the track's
  uint32_t n0 = sounding();
  CHECK(n0 + 2 >= b - a - 1 && n0 <= b - a + 2);
  cmd("sedit=apply:crop");
  run();
  CHECK(vw().frames == b - a);
  trig();   // the cropped buffer whole, STA / END not applied
  uint32_t n = sounding();
  CHECK(n + 2 >= b - a && n <= b - a + 2);
  cmd("sedit=undo");   // the old length again: STA..END
  run();
  trig();
  n = sounding();
  CHECK(n + 2 >= b - a - 1 && n <= b - a + 2);
  cmd("sedit=apply:crop");
  run();
}

static void t_save_use() {
  g_sent.clear();
  cmd("sedit=save");   // SAVE alone: the track stays on the original
  run();
  CHECK(!strcmp(vw().name, "kick-ed1.wav") && count48() == 0);
  cmd("sedit=apply:gain-");
  run();
  trig();
  const int16_t* ed = engines[0].pcm();   // the editor's buffer the track plays
  CHECK(ed != g_orig);
  cmd("sedit=save:use");
  run();
  CHECK(!strcmp(vw().name, "kick-ed2.wav"));
  int c = -1;
  for (int r = 1; r < 128; ++r)
    if (!strcmp(pools::at(pools::kSamples, pools::code(0, r)).path, "/Samples/k/kick-ed2.wav")) c = pools::code(0, r);
  CHECK(c > 0);
  CHECK(sent48(0, 10, c % srclib::kBankRows, srclib::lo_of(c)));
  CHECK(sent48(0, 12, 0, 0) && sent48(0, 13, 120, 0));   // the length changed (the crop): STA 0, END 120
  CHECK(count48() == 3);
  // the override follows to the new row: the track on it (as the Rytm's answer sets it) plays the edit whole
  g_smp = (float)(c % srclib::kBankRows) + srclib::lo_of(c) / 128.0f;
  g_sta = 0, g_end = 120;
  params();
  trig();
  render(32);
  CHECK(engines[0].pcm() == ed && fabs(level(256) - 0.25 * 0.70795) < 0.01);
  g_smp = 1;   // A001, the row the track left: the original, unchanged
  params();
  trig();
  render(32);
  CHECK(engines[0].pcm() == g_orig && fabs(level(256) - 0.25) < 0.01);
  g_smp = (float)(c % srclib::kBankRows) + srclib::lo_of(c) / 128.0f;
  params();
  // a save + use with the length as the track's: SMP only
  g_sent.clear();
  cmd("sedit=region:0:" + std::to_string(vw().frames));
  cmd("sedit=apply:norm");   // (content the pool does not hold yet: a new row)
  run();
  cmd("sedit=save:use");
  run();
  CHECK(!strcmp(vw().name, "kick-ed3.wav") && count48() == 1);
  int c3 = -1;
  for (int r = 1; r < 128; ++r)
    if (!strcmp(pools::at(pools::kSamples, pools::code(0, r)).path, "/Samples/k/kick-ed3.wav")) c3 = pools::code(0, r);
  CHECK(c3 > 0 && sent48(0, 10, c3 % srclib::kBankRows, srclib::lo_of(c3)));
  g_smp = (float)(c3 % srclib::kBankRows) + srclib::lo_of(c3) / 128.0f;   // (the Rytm's answer)
  params();
}

static void t_plock_other_smp() {
  float own = g_smp;
  trig();
  const int16_t* ed = engines[0].pcm();   // the edit's buffer
  warm(2);   // SMP p-locked to A002 (a constant -0.5): that sample, unchanged
  trig();
  render(32);
  CHECK(fabs(level(256) + 0.5) < 0.01);
  g_smp = own;
  params();
  trig();
  render(32);
  CHECK(engines[0].pcm() == ed && fabs(level(256) - sedit::kNormPeak) < 0.01);   // back on the edit
}

static void t_render_no_alloc() {   // the apply, its refill, a trig (as the SysEx handler makes it) and the render
  long n0 = g_news;
  cmd("sedit=apply:gain-");
  run();
  trigger_stream(0, 60);   // (the SysEx path itself: the test host's log allocates)
  render(40);
  CHECK(g_news - n0 == 0);
}

static void t_preview() {
  cmd("sedit=region:12000:24000");
  CHECK(!strncmp(cmd("sedit=preview"), "sedit: st=ready", 15));
  CHECK(from_ram[0] && engines[0].playing());
  uint32_t n = sounding();
  CHECK(n >= 12000 - 2 && n <= 12000 + 2);
  CHECK(!engines[0].playing());
  // the next trig: the track's own STA..END again, whole
  trig();
  n = sounding();
  CHECK(n + 2 >= vw().frames && n <= vw().frames + 2);
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
  CHECK(pcache::stats().priv == g_priv0);
}

// The new row's checks (#5): it resolves, the WAVE page has an overview, a trig plays its file at level
static void check_row_plays(int c, double lvl) {
  float smp = (float)(c % srclib::kBankRows) + srclib::lo_of(c) / 128.0f;
  const int slot = pool_slot(smp);
  CHECK(slot > 0 && slot <= npool);
  int st = 0, pr = 0;
  CHECK(ui::wave_of_slot(slot, &st, &pr) != nullptr);   // (null: the WAVE page's EMPTY SLOT)
  card::FileRec r;   // the index's record: the file's own length (the WAVE page reads frames = data_len / bpf)
  CHECK(pool[slot - 1].folder >= 0 && cards.file(pool[slot - 1].folder, pool[slot - 1].file, &r));
  CHECK(r.data_len == kN * 2 && r.channels == 1 && r.bits == 16 && r.rate == 48000);
  uint8_t h[samp::kWavHeaderMax];
  CHECK(r.data_offset == (uint32_t)samp::wav_header(h, 1, 16, 48000, kN));
  warm(smp);
  trig();
  render(32);
  CHECK(fabs(level(256) - lvl) < 0.01);
  g_smp = 1;
  params();
}

// sedit-050 #5: a copy saved into the pool (a file the boot's card index does not have) plays from its row once the
// editor is closed, and the WAVE page has an overview for it (not EMPTY SLOT)
static void t_saved_row_plays() {
  g_smp = 1, g_sta = 0, g_end = 120;
  warm(1);
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=apply:gain-");
  run();
  cmd("sedit=save");
  run();
  CHECK(!strncmp(vw().msg, "SAVED", 5));
  int c = -1;
  for (int r = 1; r < 128; ++r)
    if (!strcmp(pools::at(pools::kSamples, pools::code(0, r)).path, ("/Samples/k/" + std::string(vw().name)).c_str()))
      c = pools::code(0, r);
  CHECK(c > 0);
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
  check_row_plays(c, 0.25 * 0.70795);
}

// sedit-050 #1 / #2: an edit opened from the POOL page (no track) plays on every track on its row from their next trig,
// and PLAY (preview) plays it on a DSP SAMPLE track; close gives the original back
static void t_pool_open_plays() {
  g_smp = 1, g_sta = 0, g_end = 120;
  warm(1);
  CHECK(engines[0].pcm() == g_orig);
  cmd("sedit=open:code:" + std::to_string(code("A001")));
  run();
  CHECK(vw().st == sedit::kReady && !vw().has_track);
  cmd("sedit=apply:gain-");
  run();
  trig();
  render(32);
  CHECK(engines[0].pcm() != g_orig && fabs(level(256) - 0.25 * 0.70795) < 0.01);   // the edit, not the file
  until_end();
  // fix round 1: voice 2 (track 1) on the same row plays the edit too; on another row (A002) its own file
  trig_voice(2, 1);
  CHECK(from_ram[2] && engines[2].pcm() == engines[0].pcm());
  for (int i = 0; i < 4000 && !(from_ram[2] && engines[2].pcm() != engines[0].pcm()); ++i) trig_voice(2, 2), pass();
  CHECK(from_ram[2] && engines[2].pcm() != engines[0].pcm() && engines[2].pcm()[100] == (int16_t)(-0.5 * 32768));
  { uint64_t mach[kMachines] = {0}; for (int i = 0; i < 10000 && engines[2].playing(); ++i) render_voices(ring, kRows, 0, 0, kVoices, mach); }
  CHECK(!strncmp(cmd("sedit=preview"), "sedit: st=ready", 15) && !vw().msg[0]);   // PLAY: on a DSP SAMPLE track
  CHECK(from_ram[0] && engines[0].playing() && engines[0].pcm() != g_orig);
  until_end();
  cmd("sedit=close");
  trig();
  render(32);
  CHECK(engines[0].pcm() == g_orig && fabs(level(256) - 0.25) < 0.01);
  for (int i = 0; i < 10; ++i) pass();
  CHECK(pcache::stats().priv == g_priv0);
}

// Fix round 1 (H1): SAVE, then CLOSE at once (before the write ends): the copy is still registered with its own length
static void t_save_close_at_once() {
  g_smp = 1, g_sta = 0, g_end = 120;
  warm(1);
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=apply:gain-");   // twice: content no row has yet (the pool would answer with that row)
  run();
  cmd("sedit=apply:gain-");
  run();
  int before[128];
  for (int r = 1; r < 128; ++r) before[r] = pools::at(pools::kSamples, pools::code(0, r)).path[0] != 0;
  cmd("sedit=save");
  for (int i = 0; i < 20000 && !(vw().st == sedit::kSaving && vw().progress > 0); ++i) pass();   // (writing: a close
  CHECK(vw().st == sedit::kSaving && vw().progress > 0);                                             // before it writes nothing)
  cmd("sedit=close");   // (the save finishes out of sight)
  CHECK(vw().st == sedit::kClosed);
  int c = -1;
  for (int i = 0; i < 20000 && c < 0; ++i) {
    pass();
    for (int r = 1; r < 128 && c < 0; ++r)
      if (!before[r] && pools::at(pools::kSamples, pools::code(0, r)).path[0]) c = pools::code(0, r);
  }
  CHECK(c > 0);
  for (int i = 0; i < 20; ++i) pass();
  check_row_plays(c, 0.25 * 0.70795 * 0.70795);
}

// Fix round 1 (M3): the two play buffers cannot be had: the editor opens anyway, without live play, and says so once
static void t_no_live_play_ram() {
  g_smp = 1, g_sta = 0, g_end = 120;
  warm(1);
  // the pool RAM filled (what nothing pins evicted), then 448 KB given back: the edit and the spare (2 x 192 KB) fit,
  // the play buffers (2 x 96 KB) do not
  std::vector<int> fill;
  void* fp = nullptr;
  for (int b; fill.size() < 4096 && (b = pcache::alloc_private(64 * 1024, &fp)) >= 0;) fill.push_back(b);
  CHECK(fill.size() > 7 && fill.size() < 4096);
  for (int k = 0; k < 7; ++k) pcache::release(fill.back()), fill.pop_back();
  cmd("sedit=open:track:0");
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == kN && !strcmp(vw().msg, "NO LIVE PLAY: RAM"));
  cmd("sedit=apply:gain-");
  run();
  CHECK(vw().st == sedit::kReady && vw().dirty);
  trig();
  render(32);
  CHECK(engines[0].pcm() == g_orig && fabs(level(256) - 0.25) < 0.01);   // the file: no live play
  CHECK(!strncmp(cmd("sedit=preview"), "sedit: st=ready", 15) && !strcmp(vw().msg, "NO LIVE PLAY: RAM"));
  until_end();
  cmd("sedit=close");
  for (int b : fill) pcache::release(b);
  for (int i = 0; i < 10; ++i) pass();
  CHECK(pcache::stats().priv == g_priv0);
}

// Fix round 1: a copy whose audio a row already holds (only START / END moved: SAVE writes the whole edit, so it is the
// original's audio) is said plainly - SAVED: SAME AUDIO AS A001 - and SAVE + USE points the track at that row
static void t_same_audio() {
  g_smp = 1, g_sta = 0, g_end = 120;
  params();
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=save");   // the whole, unchanged: a copy identical to A001
  run();
  CHECK(!strcmp(vw().msg, "SAVED: SAME AUDIO AS A001"));
  g_sent.clear();
  cmd("sedit=save:use");
  run();
  CHECK(!strcmp(vw().msg, "SAVED: SAME AUDIO AS A001") && sent48(0, 10, 1, 0));
  CHECK(!sent48(0, 12, 0, 0) && !sent48(0, 13, 120, 0));   // the same length: STA / END kept
  // fix round 2: the markers only moved, SAVE+USE: a new, shorter file and row; STA 0, END 120 (the whole of it)
  cmd("sedit=region:1000:20000");
  g_sent.clear();
  cmd("sedit=save:use");
  run();
  CHECK(!strncmp(vw().msg, "SAVED", 5) && !strstr(vw().msg, "SAME"));
  CHECK(vw().frames == 19000 && vw().a == 0 && vw().b == 19000 && !vw().dirty);
  CHECK(!sent48(0, 10, 1, 0) && sent48(0, 12, 0, 0) && sent48(0, 13, 120, 0));
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// Fix round 1 (M2): the card index's added-files table full: the copy is saved and pooled, but says it plays after a
// reboot (instead of SAVED)
static void t_added_full() {
  card::FileRec r;
  memset(&r, 0, sizeof r);
  r.format = 1, r.channels = 1, r.bits = 16, r.rate = 48000, r.data_offset = 44, r.data_len = 2;
  for (int i = 0; i < 1000; ++i) {
    snprintf(r.name, sizeof r.name, "filler%d.wav", i);
    if (cards.add_file(0, r) < 0) break;
  }
  g_smp = 1;
  warm(1);
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=apply:gain+");
  run();
  cmd("sedit=save");
  run();
  CHECK(!strcmp(vw().msg, "SAVED: REBOOT TO PLAY"));
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// Rytm mods review H3: SAVE + USE's 48 writes answered busy (status 1) are sent again (every 20 ms, up to 1 s); still
// busy then: "USE: RYTM BUSY". Busy once, then done: the save's own words stay.
static int count48k(int k) {
  int n = 0;
  for (auto& m : g_sent) n += m.size() >= 10 && m[4] == 0x48 && m[6] == 0 && m[7] == k;
  return n;
}
static void answer48(int t, int k, int status) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x48, 1, (uint8_t)t, (uint8_t)k, (uint8_t)status, 0xF7};
  engine_sysex(m, sizeof m);
}
static void t_use_busy() {
  g_smp = 1, g_sta = 0, g_end = 120;
  params();
  for (int busy_for : {2000, 30}) {   // (ms: past the 1 s, then once and done)
    cmd("sedit=open:track:0");
    run();
    cmd("sedit=apply:gain+");
    run();
    cmd(busy_for > 1000 ? "sedit=region:1000:20000" : "sedit=region:2000:20000");   // (a new row each)
    g_sent.clear();
    cmd("sedit=save:use");
    run();
    CHECK(count48k(10) == 1 && count48k(12) == 1 && count48k(13) == 1);
    size_t seen = 0;
    for (int t = 0; t < 1500; ++t) {   // the fake Rytm: each write answered, busy for busy_for ms
      for (const size_t n = g_sent.size(); seen < n; ++seen)
        if (g_sent[seen].size() >= 10 && g_sent[seen][4] == 0x48) answer48(0, g_sent[seen][7], t < busy_for ? 1 : 0);
      pass();
    }
    if (busy_for > 1000) {
      CHECK(!strcmp(vw().msg, "USE: RYTM BUSY"));
      CHECK(count48k(10) > 20 && count48k(10) < 70);   // (sent again every 20 ms for 1 s, then left)
    } else {
      CHECK(!strncmp(vw().msg, "SAVED, T1 USES ", 15));
      CHECK(count48k(10) >= 2 && count48k(10) <= 4);
    }
    cmd("sedit=close");
    for (int i = 0; i < 10; ++i) pass();
  }
}

// Re-review 2: the fake Rytm for ms passes (1 ms each): each 48 write answered with status(track, param, ms) (< 0: no
// answer, an older build)
static size_t g_seen = 0;
static void fake_rytm(int ms, const std::function<int(int, int, int)>& status) {
  for (int t = 0; t < ms; ++t) {
    for (const size_t n = g_sent.size(); g_seen < n; ++g_seen) {
      const auto& m = g_sent[g_seen];
      if (m.size() < 10 || m[4] != 0x48) continue;
      const int st = status(m[6], m[7], t);
      if (st >= 0) answer48(m[6], m[7], st);
    }
    pass();
  }
}
static void save_use_from(int a) {   // track 0 opened, gain +3, START a, END 20000, SAVE + USE (a new row each a)
  g_smp = 1, g_sta = 0, g_end = 120;
  params();
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=apply:gain+");
  run();
  cmd("sedit=region:" + std::to_string(a) + ":20000");
  g_sent.clear(), g_seen = 0;
  cmd("sedit=save:use");
  run();
}
static int count48t(int t, int k) {
  int n = 0;
  for (auto& m : g_sent) n += m.size() >= 10 && m[4] == 0x48 && m[6] == t && m[7] == k;
  return n;
}
static int row_of(const char* name) {
  for (int r = 1; r < 128; ++r) {
    const char* p = pools::at(pools::kSamples, pools::code(0, r)).path;
    const char* sl = strrchr(p, '/');
    if (sl && !strcmp(sl + 1, name)) return pools::code(0, r);
  }
  return -1;
}

// (1) one retry per track: a SAVE + USE on track 2 (here use_saved on its own) keeps track 1's busy writes going
static void t_use_per_track() {
  save_use_from(3000);
  fake_rytm(100, [](int t, int, int) { return t == 0 ? 1 : 0; });   // track 1 busy throughout, track 2 done
  const int before = count48t(0, 10);
  CHECK(before >= 3);
  sedit::use_saved(1, code("A002"));
  fake_rytm(1400, [](int t, int, int) { return t == 0 ? 1 : 0; });
  CHECK(count48t(1, 10) == 1 && count48t(0, 10) > before + 20);
  CHECK(!strcmp(vw().msg, "USE: RYTM BUSY"));
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// (4) a refusal (4: locked on the steps held, or any other non-0 but busy): said, not "SAVED, T1 USES"
static void t_use_refused() {
  save_use_from(4000);
  fake_rytm(100, [](int, int k, int) { return k == 10 ? 4 : 0; });
  CHECK(!strcmp(vw().msg, "USE: RYTM REFUSED") && count48t(0, 10) == 1);
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
  save_use_from(4500);
  fake_rytm(100, [](int, int k, int) { return k == 13 ? 3 : 0; });   // (END refused, status 3)
  CHECK(!strcmp(vw().msg, "USE: RYTM REFUSED"));
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// (5) an older Rytm build never answers 48: each write sent once, the save's own words stay, nothing more said
static void t_use_no_answer() {
  save_use_from(5000);
  CHECK(!strncmp(vw().msg, "SAVED, T1 USES ", 15));
  fake_rytm(1500, [](int, int, int) { return -1; });
  CHECK(!strncmp(vw().msg, "SAVED, T1 USES ", 15));
  CHECK(count48t(0, 10) == 1 && count48t(0, 12) == 1 && count48t(0, 13) == 1);
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// (2) SAVE + USE of a region: until the save's crop is done a trig on the new row never plays the uncropped edit
static void t_use_before_crop() {
  g_smp = 1, g_sta = 0, g_end = 120;
  params();
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=apply:gain+");
  run();
  cmd("sedit=region:6000:20000");
  cmd("sedit=save:use");
  for (int i = 0; i < 200000 && strncmp(vw().msg, "SAVED, T1 USES ", 15); ++i) pass();
  CHECK(vw().st == sedit::kBusy);   // (the crop still to come)
  const int c = row_of(vw().name);
  CHECK(c > 0);
  g_smp = (float)(c % srclib::kBankRows) + srclib::lo_of(c) / 128.0f;
  params();
  trig();
  uint32_t n = sounding();
  CHECK(n > 13500 && n < 14500);   // the new row's 14000 frames, not the edit's 48000
  run();
  CHECK(vw().frames == 14000);
  trig();
  n = sounding();
  CHECK(n > 13500 && n < 14500 && engines[0].pcm() != g_orig);   // the edit, cropped, on the new row
  g_smp = 1;
  params();
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// (3) the save's progress is of the region written (s_frames), not of the whole edit
static void t_save_progress() {
  g_smp = 1, g_sta = 0, g_end = 120;
  params();
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=apply:gain-");
  run();
  cmd("sedit=apply:gain-");
  run();
  cmd("sedit=region:0:40000");
  cmd("sedit=save");
  int maxp = 0;
  for (int i = 0; i < 200000 && !(settled(vw().st) && vw().st != sedit::kBusy); ++i) {
    pass();
    if (vw().st == sedit::kSaving && vw().progress > maxp) maxp = vw().progress;
  }
  CHECK(maxp >= 95);   // (all 40000 frames written before the file's close: 100; of the whole 48000 it said 83)
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// Re-review: the copy's name too long for the card index (40 with its 0): pooled, but it never plays, so the footer
// says so instead of REBOOT TO PLAY, and SAVE + USE leaves the track on its sample
static void t_name_too_long() {
  put_row("A100", "/Samples/k/a_rather_long_sample_name_123456789.wav");   // (39 characters: in the index; its -ed1 copy, 43, is not)
  cmd("sedit=open:code:" + std::to_string(code("A100")));
  run();
  CHECK(vw().st == sedit::kReady);
  cmd("sedit=apply:gain+");
  run();
  cmd("sedit=save");
  run();
  CHECK(!strcmp(vw().msg, "SAVED: NAME TOO LONG") && !strcmp(vw().name, "a_rather_long_sample_name_123456789-ed1.wav"));
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
}

// Re-review (L7): a file written again after boot: its added record wins over the scanned one of the same name, in
// find_path (the pool rows' lookup)
static void t_added_wins() {
  int f = -1, i = -1;
  CHECK(find_path("k/other.wav", &f, &i) && i < cards.folder(f).nfiles);   // the scanned one
  card::FileRec r;
  CHECK(cards.file(f, i, &r));
  CHECK(cards.add_file(f, r) >= 0);   // (the same record: the rows on it play as before)
  int f2 = -1, i2 = -1;
  CHECK(find_path("k/other.wav", &f2, &i2) && f2 == f && i2 >= cards.folder(f).nfiles);
  card::FileRec r2;
  CHECK(cards.file(f, i2, &r2) && !strcmp(r2.name, "other.wav"));
}

// Re-review (L9): the folder listing (0D -> 0E, the Rytm's file browser) has the files added since boot, after the
// scanned ones, and counts them in its total
static void t_added_listed() {
  int f = -1, i = -1;
  CHECK(find_path("k/kick.wav", &f, &i));
  const int all = cards.files_in(f);
  CHECK(all > cards.folder(f).nfiles);   // (the copies saved above)
  g_sent.clear();
  send_listing(f);
  bool found = false;
  int total = -1;
  for (auto& m : g_sent) {
    if (m.size() < 14 || m[4] != 0x0e) continue;
    total = m[11] << 7 | m[12];
    for (size_t k = 0; k + 8 <= m.size(); ++k) found |= !memcmp(&m[k], "kick-ed1", 8);
  }
  uint16_t sub[8];
  CHECK(found && total == all + cards.subfolders(f, sub, 8));
}

// A project switch closes the edit: the new project's sample in the same slot never plays the old edit
static void t_project_closes() {
  warm(1);
  cmd("sedit=open:track:0");
  run();
  cmd("sedit=apply:gain+");
  run();
  CHECK(vw().st == sedit::kReady);
  trig();
  CHECK(engines[0].pcm() != g_orig);   // the edit plays
  until_end();
  // re-review 2 (1): a retry pending (track 2's SMP answered busy) when the project changes: dropped, nothing said
  g_sent.clear(), g_seen = 0;
  sedit::use_saved(1, code("A002"));
  fake_rytm(60, [](int, int, int) { return 1; });
  CHECK(count48t(1, 10) >= 2);
  project_switch_named("OTHER", "OTHER", false);
  CHECK(vw().st == sedit::kClosed);
  const int sent = count48t(1, 10);
  fake_rytm(1500, [](int, int, int) { return 1; });
  CHECK(count48t(1, 10) == sent && strcmp(vw().msg, "USE: RYTM BUSY"));
  for (int i = 0; i < 10; ++i) pass();
  CHECK(pcache::stats().priv == g_priv0);
}

// 0.5.0: SAMPLE EDIT on a SAMPLER track (the Rytm's machine 43 on DSP SAMPLE's voice byte, its tagged reports): opened
// from the track, the track plays the edit while it is open (live play), PLAY, SAVE + USE writes SAMPLER's SMP (P11)
// and, after a crop, STRT (P13) 0 / END (P14) 120; close gives the original back
static void t_sampler() {
  const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, kRytmSampler, (uint8_t)(kDspSample | 0), 0xF7};
  engine_sysex(m33, sizeof m33);
  g_smpl = 1;
  CHECK(ui::remote_track_smpl(0));
  g_smp = 1, g_sta = 0, g_end = 120;
  warm(1);
  CHECK(v[0].smpl && engines[0].pcm() == g_orig);
  trig();
  render(32);
  const double l0 = level(256);
  CHECK(l0 > 0.1);
  until_end();
  CHECK(!strncmp(cmd("sedit=open:track:0"), "sedit: st=loading", 17));
  run();
  CHECK(vw().st == sedit::kReady && vw().track == 0 && !vw().msg[0]);
  cmd("sedit=apply:gain+");
  run();
  trig();
  render(32);
  CHECK(v[0].smpl && from_ram[0] && engines[0].pcm() != g_orig && fabs(level(256) - l0 * 1.41254) < 0.01);   // the edit
  until_end();
  CHECK(!strncmp(cmd("sedit=preview"), "sedit: st=ready", 15) && !vw().msg[0]);   // PLAY on a SAMPLER track
  CHECK(from_ram[0] && engines[0].playing() && engines[0].pcm() != g_orig);
  until_end();
  cmd("sedit=region:0:24000");
  cmd("sedit=apply:crop");
  run();
  g_sent.clear();
  cmd("sedit=save:use");
  run();
  int c = -1;
  for (int r = 1; r < 128; ++r)
    if (!strcmp(pools::at(pools::kSamples, pools::code(0, r)).path, ("/Samples/k/" + std::string(vw().name)).c_str())) c = pools::code(0, r);
  CHECK(c > 0 && sent48(0, 10, c % srclib::kBankRows, srclib::lo_of(c)));   // SMP (P11)
  CHECK(sent48(0, 12, 0, 0) && sent48(0, 13, 120, 0) && count48() == 3);   // STRT 0, END 120 (the crop)
  g_smp = (float)(c % srclib::kBankRows) + srclib::lo_of(c) / 128.0f;   // (the Rytm's answer)
  params();
  trig();
  render(32);
  CHECK(v[0].smpl && engines[0].pcm() != g_orig && fabs(level(256) - l0 * 1.41254) < 0.01);   // the new row: the edit
  until_end();
  cmd("sedit=close");
  for (int i = 0; i < 10; ++i) pass();
  g_smp = 1;
  params();
  trig();
  render(32);
  CHECK(v[0].smpl && engines[0].pcm() == g_orig && fabs(level(256) - l0) < 0.01);   // closed: the original
  until_end();
  for (int i = 0; i < 10; ++i) pass();
  CHECK(pcache::stats().priv == g_priv0);
  g_smpl = 2;   // (a tagless report keeps SAMPLER: DSP SAMPLE's own tag ends it)
  const uint8_t back[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 49, (uint8_t)(kDspSample | 0), 0xF7};   // DSP SAMPLE again
  engine_sysex(back, sizeof back);
  params();
  CHECK(!ui::remote_track_smpl(0) && !v[0].smpl);
  g_smpl = 0;
  printf("sedit play: SAMPLER - open from the track, live play, PLAY, SAVE + USE (SMP, STRT, END), close\n");
}

int main() {
  char tmpl[] = "/tmp/sedit-play-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/k' '" + g_card + "/.rytm'").c_str()) == 0);
  setenv("CORE_POOL_RAM_MB", "64", 1);
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);   // the Pi: DSP SAMPLE plays from the sample RAM
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  hooks.to_rytm = out;
  write_wav16("/Samples/k/kick.wav", kN, [](int) { return 0.25f; });
  write_wav16("/Samples/k/other.wav", kN, [](int) { return -0.5f; });
  write_wav16("/Samples/k/a_rather_long_sample_name_123456789.wav", kN, [](int) { return 0.3f; });
  cardq::reset();
  cardq::set_worker(true);
  CHECK(engine_begin() && card_ok && psram && engines);
  snprintf(core::project, sizeof core::project, "LIVE");
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) pass();
  for (int i = 0; i < 4000 && !(pools::ready() && !strcmp(pools::project(), "LIVE")); ++i) pass();
  CHECK(pools::ready());
  put_row("A001", "/Samples/k/kick.wav");
  put_row("A002", "/Samples/k/other.wav");
  pool_sync();
  // track 0 (voice 0) and track 1 (voice 2) DSP SAMPLE
  const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 2, 49, (uint8_t)(kDspSample | 0), 49, (uint8_t)(kDspSample | 2), 0xF7};
  engine_sysex(m33, sizeof m33);
  CHECK((ui::remote_track_machine(0) & 0x78) == kDspSample && ui::remote_track_voice(0) == 0);

  t_plays_edit();
  t_swap_busy_note();
  t_stopped_voice_detached();
  t_close_reverts();
  t_crop_plays_whole();
  t_save_use();
  t_plock_other_smp();
  t_render_no_alloc();
  t_preview();
  t_saved_row_plays();
  t_pool_open_plays();
  t_save_close_at_once();
  t_no_live_play_ram();
  t_same_audio();
  t_use_busy();
  t_use_per_track();
  t_use_refused();
  t_use_no_answer();
  t_use_before_crop();
  t_save_progress();
  t_name_too_long();
  t_added_wins();
  t_added_listed();
  t_added_full();
  t_sampler();
  t_project_closes();
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);
  printf("test_sedit_play: PASS\n");
  return 0;
}
