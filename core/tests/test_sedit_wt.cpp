// core/tests/test_sedit_wt.cpp - MAKE WT through the editor (samp/sedit.h make_wt, docs/SAMPLE-EDIT.md): a pooled sample's
// region becomes /WAVETABLES/<stem>-wt<N>.wav (float32, `clm ` 2048, N x 2048 samples) that WAVETABLE's reader parses, a row
// of the WAVETABLES pool (kExisting on the same content), never over a file that is there, the folder made when
// missing; a table file opens in the editor (float read), a refused make says why, the RAM is given back.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "lib/library.h"
#include "lib/sha256.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "nmach.h"
#include "pool/pcache.h"
#include "samp/dsp.h"
#include "samp/sedit.h"
#include "ui/remote.h"
#include "voices.h"
#include "wavetable/tables.h"
#include "wavetable/wav_read.h"

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

static std::vector<uint8_t> slurp(const char* path) {
  FILE* fp = fopen(host_path(path).c_str(), "rb");
  CHECK(fp);
  std::vector<uint8_t> b;
  uint8_t t[4096];
  size_t n;
  while ((n = fread(t, 1, sizeof t, fp)) > 0) b.insert(b.end(), t, t + n);
  fclose(fp);
  return b;
}
static float saw(int f, int) {   // a band-limited saw, 220 Hz at 48 kHz: its harmonics to 109
  double a = 0;
  for (int h = 1; h <= 108; ++h) a += sin(2 * M_PI * h * 220.0 * f / 48000.0) / h;
  return (float)(0.3 * a);
}

int main() {
  char tmpl[] = "/tmp/sedit-wt-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/k' '" + g_card + "/.rytm'").c_str()) == 0);   // (no /WAVETABLES: made)
  setenv("CORE_POOL_RAM_MB", "160", 1);
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  hooks.to_rytm = [](const uint8_t*, int) {};
  write_wavf("/Samples/k/saw.wav", 48000, 1, 16, saw);
  write_wavf("/Samples/k/tiny.wav", 40, 1, 16, saw);
  cardq::reset();
  cardq::set_worker(true);
  CHECK(engine_begin() && card_ok);
  snprintf(core::prj().key, sizeof core::prj().key, "LIVE");
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) pass();
  for (int i = 0; i < 4000 && !(pools::ready() && !strcmp(pools::project(), "LIVE")); ++i) pass();
  CHECK(pools::ready());
  put_row("A001", "/Samples/k/saw.wav");
  put_row("A002", "/Samples/k/tiny.wav");

  // a refused make: nothing open; too short a region
  CHECK(sedit::make_wt() < 0);
  CHECK(starts(cmd("sedit=open:code:2"), "sedit: st=loading"));
  run();
  CHECK(sedit::make_wt() < 0 && !strcmp(vw().msg, "TOO SHORT FOR A TABLE"));
  cmd("sedit=close");

  // a region of the sample as a table of 16 frames
  CHECK(starts(cmd("sedit=open:code:1"), "sedit: st=loading"));
  run();
  const uint64_t priv0 = pcache::stats().priv;
  CHECK(sedit::wt_frames() == 64);
  cmd("sedit=wtn:7");   // (not a choice: ignored)
  CHECK(sedit::wt_frames() == 64);
  cmd("sedit=wtn:16");
  CHECK(sedit::wt_frames() == 16 && vw().wt_frames == 16 && vw().wt_voice == -1 && !vw().file);
  cmd("sedit=region:12000:36000");
  CHECK(sedit::make_wt() == 0 && vw().st == sedit::kBusy && vw().making);
  CHECK(sedit::make_wt() < 0 && !strcmp(vw().msg, "BUSY"));   // (one at a time)
  int last = -1;
  bool saw_saving = false;
  for (int i = 0; i < 400000 && !settled(vw().st); ++i) {
    pass();
    if (vw().st == sedit::kBusy) {
      CHECK(vw().progress >= last);
      last = vw().progress;
    }
    saw_saving |= vw().st == sedit::kSaving;
  }
  CHECK(saw_saving && vw().st == sedit::kReady && !vw().making);
  printf("made: %s\n", vw().msg);
  CHECK(starts(vw().msg, "WT A007 PITCHED 220HZ"));
  CHECK(exists("/WAVETABLES/saw-wt1.wav") && !exists("/WAVETABLES/saw-wt1.wav.tmp"));
  CHECK(vw().dirty == false && vw().frames == 48000);   // (the edit itself untouched)
  {
    std::vector<uint8_t> w = slurp("/WAVETABLES/saw-wt1.wav");
    wavread::Info in;
    CHECK(wavread::parse(w.data(), (uint32_t)w.size(), &in));
    CHECK(in.format == 3 && in.ch == 1 && in.bits == 32 && in.frames == 16u * 2048 && in.clm_frame == 2048);
    CHECK(w.size() == in.data_off + in.data_len + (in.data_len & 1));
    CHECK(wt::detect_frame_size(in.frames, in.clm_frame) == 2048 && wt::frame_count(in.frames, 2048) == 16);
    float pk = 0;
    for (uint32_t i = 0; i < in.frames; ++i) pk = fmaxf(pk, fabsf(wavread::sample(in, &w[in.data_off + 4 * i])));
    CHECK(fabsf(pk - 0.95f) < 1e-3f);
  }
  {
    int c7 = code("A007");
    const pools::Entry& en = pools::at(pools::kTables, c7);
    CHECK(en.type == pools::kSample && !strcmp(en.path, "/WAVETABLES/saw-wt1.wav"));
    CHECK(pools::at(pools::kTables, code("A008")).type == pools::kEmpty);
  }
  // the same region again: a new file (never over one that is there), the same audio: the pool says the same row
  cmd("sedit=makewt");
  run();
  CHECK(starts(vw().msg, "WT A007 PITCHED"));
  CHECK(exists("/WAVETABLES/saw-wt2.wav") && pools::at(pools::kTables, code("A008")).type == pools::kEmpty);
  // another choice of frames: 32
  cmd("sedit=wtn:32");
  cmd("sedit=region:0:48000");
  cmd("sedit=makewt");
  run();
  CHECK(starts(vw().msg, "WT A008 PITCHED") && exists("/WAVETABLES/saw-wt3.wav"));
  {
    std::vector<uint8_t> w = slurp("/WAVETABLES/saw-wt3.wav");
    wavread::Info in;
    CHECK(wavread::parse(w.data(), (uint32_t)w.size(), &in) && in.frames == 32u * 2048);
  }
  cmd("sedit=close");
  run();
  CHECK(pcache::stats().priv < priv0);   // (the edit's blocks and the table's: let go)

  // a table's file opens as an edit: float read, no live play, no preview; its table: /WAVETABLES/saw-wt<N+1>
  CHECK(starts(cmd("sedit=open:file:/WAVETABLES/saw-wt1.wav"), "sedit: st=loading"));
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == 16u * 2048 && vw().ch == 1 && vw().bits == 32 && vw().file && !vw().has_track);
  CHECK(!strcmp(vw().name, "saw-wt1.wav"));
  CHECK(sedit::preview() < 0 && !strcmp(vw().msg, "NO PLAY FOR A FILE"));
  cmd("sedit=region:0:16384");   // the first 8 frames' worth of samples: a short cycle count, still pitched
  cmd("sedit=wtn:16");
  cmd("sedit=makewt");
  run();
  CHECK(exists("/WAVETABLES/saw-wt4.wav"));
  CHECK(strstr(vw().msg, "WT A0"));
  cmd("sedit=close");
  run();

  // silence: refused as such, nothing written, the RAM given back
  write_wavf("/Samples/k/quiet.wav", 24000, 1, 16, [](int, int) { return 0.f; });
  put_row("A003", "/Samples/k/quiet.wav");
  const uint64_t p1 = pcache::stats().priv;
  cmd("sedit=open:code:3");
  run();
  cmd("sedit=makewt");
  run();
  CHECK(!strcmp(vw().msg, "SILENT") && !exists("/WAVETABLES/quiet-wt1.wav"));
  cmd("sedit=close");
  run();
  CHECK(pcache::stats().priv == p1);

  // closed while the table is made: everything let go, nothing written
  cmd("sedit=open:code:1");
  run();
  cmd("sedit=makewt");
  for (int i = 0; i < 3; ++i) pass();
  CHECK(vw().making);
  cmd("sedit=close");
  for (int i = 0; i < 400; ++i) pass();
  CHECK(vw().st == sedit::kClosed && pcache::stats().priv == p1);
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_sedit_wt: PASS\n");
  return 0;
}
