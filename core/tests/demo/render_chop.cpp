// core/tests/demo/render_chop.cpp - CHOP's demo (make -C core demo-chop OUT=dir): chop_demo.wav, 48 kHz 16-bit mono,
// a synthetic two-bar break at 120 BPM (kicks, snares, hats, an open hat, a ghost note) chopped by LAZY and played back:
//   A  0-4 s     LAZY: the break plays once from its start while taps land on its hits (each 7 ms late: the latency the
//                engine subtracts, with up to 1 ms of jitter) - what the player hears while chopping
//   B  4-8 s     the slices in a new order at 16ths (the sequencer's notes: note = slice)
//   C  8-12 s    the straight order through SHUFFLE (SEED 7), then SEED 19
//   D  12-16 s   STUTTER: slice 2 x4 (25 %), slice 6 x8 (12 %, decaying), the rest as they are
//   E  16-20 s   TAPE STOP (600 ms) on the bar's last hit, SPIN-UP (250 ms) on its first
//   F  20-24 s   MORPH swept from 0 to fully up and back (each slice into its right-hand neighbour)
// The machine's own render (chop::render_add), the voice's soft limiter left out: the slices as they play.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "chop/chop.h"
#include "core.h"
#include "host/core_port.h"
#include "lib/pools.h"
#include "nmach.h"
#include "poly.h"
#include "voices.h"

using namespace core;

static const int kSr = 48000, kStep = 6000;   // a 16th at 120 BPM
static std::vector<float> g_out;
static int16_t g_brk[2 * 16 * kStep];        // two bars
static std::vector<uint32_t> g_hits;         // the break's hits (frames)

static void make_break() {
  std::vector<double> x(sizeof g_brk / sizeof g_brk[0], 0.0);
  uint32_t rnd = 777;
  auto noise = [&]() { rnd = rnd * 1664525u + 1013904223u; return (double)(rnd >> 8) / 8388608.0 - 1.0; };
  // the pattern (16ths over two bars): K kick, S snare, H hat, O open hat, g ghost snare
  const char* pat = "K.H.S.HgK.KHS.HO" "K.H.S.H.K.KHSgOH";
  for (int st = 0; st < 32; ++st) {
    char c = pat[st];
    if (c == '.') continue;
    uint32_t a = (uint32_t)st * kStep;
    g_hits.push_back(a);
    double ph = 0, lp = 0;
    for (uint32_t i = a; i < x.size() && i < a + 20000; ++i) {
      double t = (i - a) / (double)kSr, v = 0;
      if (c == 'K') { ph += 2 * M_PI * (48 + 90 * std::exp(-t * 35)) / kSr; v = 0.85 * std::exp(-t * 8) * std::sin(ph); }
      else if (c == 'S' || c == 'g') {
        double amp = c == 'S' ? 0.6 : 0.18;
        lp += 0.35 * (noise() - lp);
        v = amp * (std::exp(-t * 16) * (noise() * 0.7 + lp * 0.6) + 0.5 * std::exp(-t * 30) * std::sin(2 * M_PI * 190 * t));
      } else if (c == 'H') v = 0.22 * std::exp(-t * 70) * (noise() - lp), lp += 0.5 * (noise() - lp);
      else v = 0.2 * std::exp(-t * 9) * (noise() - lp), lp += 0.5 * (noise() - lp);
      uint32_t left = a + 20000 - i;
      x[i] += left < 2000 ? v * left / 2000.0 : v;
    }
  }
  for (size_t i = 0; i < x.size(); ++i) {
    double v = std::tanh(x[i] * 1.1) * 0.95;
    g_brk[i] = (int16_t)std::lround(v * 32767);
  }
}
static void render(int frames) {
  size_t at = g_out.size();
  g_out.resize(at + frames, 0.0f);
  for (int s = 0; s < frames; s += kBlock) chop::render_add(0, g_out.data() + at + s, frames - s < kBlock ? frames - s : kBlock);
}
static std::vector<float> P(float seed = 0, float mrph = 0) {
  return {10, 0, 64, mrph, seed, 0, 40, 110, 0, 0, 0, 0, 0, 0, 0, 0};
}
static void knobs(const std::vector<float>& p) { chop::report(0, -1, 0, p.data()); }
// one 16th step: the slice (note = slice; -1 rest), held for the step
static void step(int slice, int vel = 110) {
  if (slice >= 0) chop::live_on(0, slice, vel, poly::kSeqCh, slice);
  render(kStep);
  if (slice >= 0) chop::live_off(0, poly::kSeqCh, slice);
}

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : "build";
  mkdir(out.c_str(), 0755);
  host_fake_time(true, 1000000);
  voices_init();
  nm::begin(false);
  make_break();
  v[0].cp = true;
  if (!chop::use_buffer(0, g_brk, sizeof g_brk / sizeof g_brk[0], 1, kSr)) { fprintf(stderr, "no buffer\n"); return 1; }
  knobs(P());
  // A: LAZY - the break plays from its start; a tap 7 ms after each hit (+-1 ms), the engine takes the latency off
  chop::set_latency(336);
  chop::lazy_arm(0);
  size_t ti = 0;
  int total = (int)(sizeof g_brk / sizeof g_brk[0]);
  for (int f = 0; f < total + 2400; f += kBlock) {
    while (ti < g_hits.size() && f >= (int)g_hits[ti] + 336 + (int)((ti * 37) % 97) - 48) {
      chop::lazy_tap(0);
      ++ti;
    }
    render(kBlock);
  }
  chop::lazy_stop(0, true);
  const chop::Map* m = chop::map_of(0);
  int n = m ? m->n : 0;
  printf("render_chop: LAZY: %d taps -> %d slices; markers vs hits:", (int)g_hits.size(), n);
  int worst = 0;
  for (int k = 0; k < n && k < (int)g_hits.size(); ++k) {
    int d = (int)m->s[k].start - (int)g_hits[k];
    worst = std::abs(d) > worst ? std::abs(d) : worst;
  }
  printf(" worst %d frames (%.2f ms)\n", worst, worst * 1000.0 / kSr);
  g_out.resize((size_t)4 * kSr, 0.0f);   // (section A ends at 4 s)
  // B: a new order at 16ths (slice numbers modulo the map's: the sequencer's note is the slice)
  const int order[32] = {0, 3, 2, 1, 0, 0, 5, 3, 6, 2, 7, 1, 4, 4, 5, 7, 0, 8, 2, 9, 10, 3, 11, 1, 0, 12, 13, 2, 14, 6, 15, 16};
  for (int s = 0; s < 32; ++s) step(order[s] % (n ? n : 1));
  // C: SHUFFLE - the straight order through SEED 7, then SEED 19
  knobs(P(7));
  for (int s = 0; s < 16; ++s) step(s % (n ? n : 1));
  knobs(P(19));
  for (int s = 0; s < 16; ++s) step(s % (n ? n : 1));
  knobs(P());
  // D: STUTTER on slices 2 and 6
  chop::edit_field(0, 2, chop::kFStut, 4), chop::edit_field(0, 2, chop::kFSlen, 25), chop::edit_field(0, 2, chop::kFSdec, 0);
  chop::edit_field(0, 6, chop::kFStut, 8), chop::edit_field(0, 6, chop::kFSlen, 12), chop::edit_field(0, 6, chop::kFSdec, 15);
  for (int bar = 0; bar < 2; ++bar)
    for (int s = 0; s < 16; ++s) step(s % 4 == 0 ? (s / 4 == 1 ? 2 : s / 4 == 3 ? 6 : 0) : s % 2 ? -1 : (s + bar) % (n ? n : 1));
  chop::edit_field(0, 2, chop::kFStut, 1), chop::edit_field(0, 6, chop::kFStut, 1);
  // E: TAPE STOP on the bar's last hit, SPIN-UP on its first
  int last = n > 1 ? n - 1 : 0;
  chop::edit_field(0, last, chop::kFTape, chop::kTapeStop), chop::edit_field(0, last, chop::kFTms, 600);
  chop::edit_field(0, 0, chop::kFTape, chop::kTapeSpin), chop::edit_field(0, 0, chop::kFTms, 250);
  for (int bar = 0; bar < 2; ++bar) {
    for (int s = 0; s < 12; ++s) step(s % 3 ? -1 : (s / 3) % (n ? n : 1));
    step(last);
    for (int s = 0; s < 3; ++s) step(-1);
  }
  chop::edit_field(0, last, chop::kFTape, chop::kTapeOff), chop::edit_field(0, 0, chop::kFTape, chop::kTapeOff);
  // F: MORPH swept up and back over two bars
  for (int s = 0; s < 32; ++s) {
    float mv = s < 16 ? 127.0f * s / 15 : 127.0f * (31 - s) / 15;
    knobs(P(0, mv));
    step(s % (n ? n : 1));
  }
  render(kSr / 2);
  // the WAV: 16-bit mono
  float pk = 0;
  for (float x : g_out) pk = std::fabs(x) > pk ? std::fabs(x) : pk;
  float g = pk > 0.98f ? 0.98f / pk : 1.0f;
  std::vector<uint8_t> b;
  auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(x >> (8 * i))); };
  auto u16 = [&](uint16_t x) { b.push_back((uint8_t)x), b.push_back((uint8_t)(x >> 8)); };
  b.insert(b.end(), {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(16), u16(1), u16(1), u32(kSr), u32(kSr * 2), u16(2), u16(16);
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  u32((uint32_t)g_out.size() * 2);
  for (float x : g_out) {
    int v = (int)std::lround(x * g * 32767.0f);
    u16((uint16_t)(int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
  }
  uint32_t riff = (uint32_t)b.size() - 8;
  for (int i = 0; i < 4; ++i) b[4 + i] = (uint8_t)(riff >> (8 * i));
  std::string p = out + "/chop_demo.wav";
  FILE* f = fopen(p.c_str(), "wb");
  if (!f || fwrite(b.data(), 1, b.size(), f) != b.size()) { fprintf(stderr, "cannot write %s\n", p.c_str()); return 1; }
  fclose(f);
  printf("render_chop: %s (%.1f s, %d slices, peak %.2f)\n", p.c_str(), g_out.size() / (double)kSr, n, pk);
  return 0;
}
