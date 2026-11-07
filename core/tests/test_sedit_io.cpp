// core/tests/test_sedit_io.cpp - the sample editor's card side (samp/sedit_io.h): the 32-bit WAV header, the save name,
// a whole load at 16 / 24 / 32 bits (frames split across 64 KB pieces), the files refused, and a saved copy that never
// touches the original - inline (no card core) and with a card core.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "lib/sha256.h"
#include "msamp/cardq.h"
#include "pool/pcache.h"
#include "samp/dsp.h"
#include "samp/sedit_io.h"

using namespace core;
using namespace core::sedit;

static std::string g_card;

static uint32_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// A WAV on the card with float samples F(frame, ch) in -1..1: PCM 8 / 16 / 24 / 32-bit, or (fmt 3) 32-bit float.
// (tests/wav.h's write_wav takes int16 samples and 16 / 24 bits only: these files need the full depth.)
template <class F>
static void write_wavf(const char* path, uint32_t frames, int ch, int bits, uint32_t rate, F s, int fmt = 1) {
  uint8_t h[samp::kWavHeaderMax];
  int hn = samp::wav_header(h, ch, bits, rate, frames);
  h[20] = (uint8_t)fmt;
  std::vector<uint8_t> b(h, h + hn);
  uint8_t o[4];
  for (uint32_t f = 0; f < frames; ++f)
    for (int c = 0; c < ch; ++c) {
      float v = s((int)f, c);
      if (fmt == 3) memcpy(o, &v, 4);
      else if (bits == 8) o[0] = (uint8_t)(128 + (int)(v * 127));
      else if (bits == 16) samp::put16(o, v);
      else if (bits == 24) samp::put24(o, v);
      else samp::put32(o, v);
      b.insert(b.end(), o, o + bits / 8);
    }
  if (b.size() & 1) b.push_back(0);
  FILE* fp = fopen(host_path(path).c_str(), "wb");
  CHECK(fp);
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
}

static std::vector<uint8_t> file(const char* path) {
  std::vector<uint8_t> b;
  FILE* fp = fopen(host_path(path).c_str(), "rb");
  if (!fp) return b;
  uint8_t t[4096];
  size_t n;
  while ((n = fread(t, 1, sizeof t, fp)) > 0) b.insert(b.end(), t, t + n);
  fclose(fp);
  return b;
}
static std::string sha(const char* path) {
  std::vector<uint8_t> b = file(path);
  uint8_t d[32];
  char x[65];
  lib::sha256(b.data(), b.size(), d);
  lib::hex(d, 32, x);
  return x;
}
static bool exists(const char* path) { return access(host_path(path).c_str(), F_OK) == 0; }

static bool g_worker = false;
static void card_core() {
  if (!g_worker) return;
  host_card_core(0);
  for (int i = 0; i < 64 && cardq::service(); ++i) {}
  host_card_core(3);
}
static int load(Loader& L, const char* path, int* steps = nullptr) {
  int r = L.begin(path), n = 0;
  if (r) return r;
  while ((r = L.step()) > 0) ++n, card_core();
  if (steps) *steps = n;
  return r;
}
static int save(const char* target, const float* x, int ch, int bits, uint32_t rate, uint32_t frames) {
  Saver S;
  int r = S.begin(target, x, ch, bits, rate, frames);
  if (r) return r;
  for (int i = 0; i < 100000 && (r = S.step()) > 0; ++i) card_core();
  return r;
}

static float ramp24(int f, int c) { return c ? -(f % 1000) / 1000.f : (f % 1000) / 1000.f; }

static void t_load_save(bool worker) {
  g_worker = worker;
  cardq::reset();
  cardq::set_worker(worker);
  host_card_core(3);
  uint64_t priv0 = pcache::stats().priv;

  // t_load_16m: mono 16-bit, one piece and a bit
  write_wavf("/Samples/t16.wav", 40000, 1, 16, 44100, [](int f, int) { return ((f % 200) - 100) / 128.f; });
  Loader L;
  CHECK(load(L, "/Samples/t16.wav") == 0);
  CHECK(L.frames == 40000 && L.ch == 1 && L.bits == 16 && L.rate == 44100 && L.blk >= 0 && L.x);
  for (int f : {0, 1, 32767, 32768, 39999}) CHECK(fabsf(L.x[f] - ((f % 200) - 100) / 128.f) < 1.f / 32768);
  pcache::release(L.blk);

  // t_load_24st_split: 180006 bytes of data in 64 KB pieces, frames (6 bytes) split at the piece ends
  write_wavf("/Samples/t24.wav", 30001, 2, 24, 48000, ramp24);
  int steps = 0;
  CHECK(load(L, "/Samples/t24.wav", &steps) == 0);
  CHECK(L.frames == 30001 && L.ch == 2 && L.bits == 24 && L.rate == 48000);
  CHECK(steps >= 3);
  for (int f : {0, 10922, 10923, 21845, 30000}) {
    CHECK(fabsf(L.x[2 * f] - ramp24(f, 0)) < 1.f / 8388608 * 2);
    CHECK(fabsf(L.x[2 * f + 1] - ramp24(f, 1)) < 1.f / 8388608 * 2);
  }
  for (uint32_t f = 0; f < L.frames; ++f) CHECK(fabsf(L.x[2 * f] - ramp24((int)f, 0)) < 1.f / 8388608 * 2);
  std::vector<float> x24(L.x, L.x + 2 * L.frames);
  pcache::release(L.blk);

  // t_load_32st_split: 8-byte frames, 3 pieces and a bit
  auto s32 = [](int f, int c) { return (c ? -1.f : 1.f) * ((f * 7919) % 65536 - 32768) / 32768.f * 0.999f; };
  write_wavf("/Samples/t32.wav", 25000, 2, 32, 96000, s32);
  CHECK(load(L, "/Samples/t32.wav") == 0);
  CHECK(L.frames == 25000 && L.ch == 2 && L.bits == 32 && L.rate == 96000);
  for (uint32_t f = 0; f < L.frames; ++f)
    for (int c = 0; c < 2; ++c) CHECK(fabsf(L.x[2 * f + c] - s32((int)f, c)) < 1.f / 8388608);
  pcache::release(L.blk);
  CHECK(pcache::stats().priv == priv0);

  // t_refuse
  write_wavf("/Samples/b8.wav", 100, 1, 8, 48000, [](int, int) { return 0.f; });
  CHECK(load(L, "/Samples/b8.wav") == kErrFormat);
  write_wavf("/Samples/bf.wav", 100, 1, 32, 48000, [](int, int) { return 0.f; }, 3);
  CHECK(load(L, "/Samples/bf.wav") == kErrFormat);
  CHECK(load(L, "/Samples/none.wav") == kErrOpen);
  write_wavf("/Samples/long.wav", (1u << 22) + 1, 1, 16, 48000, [](int, int) { return 0.f; });
  CHECK(load(L, "/Samples/long.wav") == kErrLong);
  CHECK(pcache::stats().priv == priv0);   // refused from the header: no RAM taken
  CHECK(!strcmp(err_text(kErrLong), "TOO LONG (MAX 87 S)") && !strcmp(err_text(kErrFormat), "NOT 16/24/32-BIT PCM WAV"));

  // t_save_names_and_original: the copy beside the original, the original's bytes unchanged
  write_wavf("/Samples/k/kick.wav", 30001, 2, 24, 48000, ramp24);
  write_wavf("/Samples/k/kick-ed1.wav", 10, 1, 16, 48000, [](int, int) { return 0.f; });
  FILE* fp = fopen(host_path("/Samples/k/kick-ed2.wav.tmp").c_str(), "wb");   // a stale .tmp: not a name, written over
  fputs("stale", fp);
  fclose(fp);
  char nm[cardq::kPathMax];
  CHECK(edit_name("/Samples/k/kick-ed1.wav", nm, sizeof nm) == 0);
  CHECK(!strcmp(nm, "/Samples/k/kick-ed2.wav"));
  CHECK(edit_name("/Samples/k/kick.wav", nm, sizeof nm) == 0);
  CHECK(!strcmp(nm, "/Samples/k/kick-ed2.wav"));
  CHECK(edit_name("/Samples/nofolder/kick.wav", nm, sizeof nm) == -2);
  CHECK(edit_name("/Samples/k/kick.wav", nm, 20) == -1);
  std::string sha_before = sha("/Samples/k/kick.wav");
  CHECK(save("/Samples/k/kick-ed2.wav", x24.data(), 2, 24, 48000, 30001) == 0);
  CHECK(sha("/Samples/k/kick.wav") == sha_before);
  CHECK(!exists("/Samples/k/kick-ed2.wav.tmp"));
  CHECK(load(L, "/Samples/k/kick-ed2.wav") == 0);
  CHECK(L.frames == 30001 && L.ch == 2 && L.bits == 24 && L.rate == 48000);
  for (uint32_t i = 0; i < 2 * L.frames; ++i) CHECK(fabsf(L.x[i] - x24[i]) <= 1.f / 8388608);
  pcache::release(L.blk);
  CHECK(file("/Samples/k/kick-ed2.wav").size() == 44 + 180006);
  CHECK(save("/Samples/k/kick.wav", x24.data(), 2, 24, 48000, 30001) == kErrExists);
  CHECK(sha("/Samples/k/kick.wav") == sha_before);
  std::string lng = "/Samples/k/" + std::string(200, 'a') + ".wav";
  CHECK(save(lng.c_str(), x24.data(), 2, 24, 48000, 30001) == kErrName);

  // 32-bit and 16-bit round trips (an odd 24-bit mono length: the pad byte)
  CHECK(save("/Samples/k/r32.wav", x24.data(), 2, 32, 48000, 30001) == 0);
  CHECK(load(L, "/Samples/k/r32.wav") == 0);
  CHECK(L.bits == 32 && L.frames == 30001);
  for (uint32_t i = 0; i < 2 * L.frames; ++i) CHECK(fabsf(L.x[i] - x24[i]) < 1.f / 8388608);
  pcache::release(L.blk);
  CHECK(save("/Samples/k/r16.wav", x24.data(), 2, 16, 48000, 30001) == 0);
  CHECK(load(L, "/Samples/k/r16.wav") == 0);
  for (uint32_t i = 0; i < 2 * L.frames; ++i) CHECK(fabsf(L.x[i] - x24[i]) <= 1.f / 32768);
  pcache::release(L.blk);
  CHECK(save("/Samples/k/m24.wav", x24.data(), 1, 24, 48000, 333) == 0);
  CHECK(file("/Samples/k/m24.wav").size() == 44 + 999 + 1);
  CHECK(load(L, "/Samples/k/m24.wav") == 0 && L.frames == 333);
  pcache::release(L.blk);
  CHECK(pcache::stats().priv == priv0);

  system(("rm -f '" + g_card + "/Samples/k/'*").c_str());
}

int main() {
  // t_header32
  uint8_t h[samp::kWavHeaderMax];
  int n = samp::wav_header(h, 2, 32, 44100, 10);
  CHECK(n == 44);
  CHECK(rd16(h + 34) == 32);
  CHECK(rd16(h + 32) == 8);
  CHECK(rd32(h + 28) == 44100 * 8);
  CHECK(rd32(h + 40) == 80);
  uint8_t p[4];
  samp::put32(p, 1.5f);
  CHECK(rd32(p) == 0x7FFFFFFFu);
  samp::put32(p, -1.0f);
  CHECK(rd32(p) == 0x80000000u);
  samp::put16(p, -2.0f);
  CHECK(rd16(p) == 0x8000);
  samp::put16(p, 0.5f);
  CHECK(rd16(p) == 0x4000);

  // t_name
  char out[64];
  int t1[] = {1, 3};
  edit_name_from("kick", t1, 2, out, 64);
  CHECK(!strcmp(out, "kick-ed4.wav"));
  edit_name_from("kick", nullptr, 0, out, 64);
  CHECK(!strcmp(out, "kick-ed1.wav"));
  edit_name_from("kick-ed1", nullptr, 0, out, 64);   // the stem's -ed1 dropped
  CHECK(!strcmp(out, "kick-ed1.wav"));
  edit_name_from("kick-ed", nullptr, 0, out, 64);    // no number: part of the name
  CHECK(!strcmp(out, "kick-ed-ed1.wav"));

  char tmpl[] = "/tmp/core_sedit_io_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/k'").c_str()) == 0);
  host_set_card(tmpl);
  pcache::begin(64u << 20);
  t_load_save(false);
  t_load_save(true);
  cardq::reset();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_sedit_io: PASS\n");
  return 0;
}
