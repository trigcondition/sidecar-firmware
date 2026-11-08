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
static std::vector<uint8_t> report(int cmd) {
  float p[16] = {0, 64, 64, 64, 0, 0, 64, 100, 64, 64, g_smp, 0, g_sta, g_end, 0, 100};
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(kDspSample | 0)};
  if (cmd == 0x04) m.push_back(60), m.push_back(100);
  for (float x : p) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  m.push_back(0xf7);
  return m;
}
static void params() {
  auto m = report(0x03);
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
  project_switch_named("OTHER", "OTHER", false);
  CHECK(vw().st == sedit::kClosed);
  for (int i = 0; i < 10; ++i) pass();
  CHECK(pcache::stats().priv == g_priv0);
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
  t_project_closes();
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);
  printf("test_sedit_play: PASS\n");
  return 0;
}
