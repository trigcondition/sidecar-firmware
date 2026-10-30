// core/tests/test_fm_pat.cpp - a PAT change on the 6-OP FM plays the NEW voice (the user, 2026-10-26, after fm-smooth:
// "when i p-lock the fm synth preset now, it stays on the initial preset"). Each trig's note, well after its attack,
// must match a run that played that PAT from the start, not the PAT before.
#include <cmath>
#include <cstring>
#include <vector>

#include "check.h"
#include "voices.h"

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back(note); m.push_back(vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}
static void out(const uint8_t*, int) {}
static int16_t ring[core::kBlock * 400][core::kVoices];
static void render(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) core::render_voices(ring, core::kBlock * 400, b * core::kBlock, 0, core::kVoices, mach);
}
// trigs with these PATs, 2400 frames apart; frames 1200..2399 after the last trig
static float g_chd = 0;   // P13 (CHD): 0 the mono note, else a chord on the poly slots (poly.cpp take_trig)
static std::vector<double> run(std::vector<int> pats) {
  using namespace core;
  voices_init();
  render(10);
  for (int pat : pats) {
    auto t = report(0x04, kFm | 0, 48, 110, {(float)pat, 64, 64, 127, 64, 64, 110, 40, 0, 0, 0, 0, g_chd});
    voice_report(0x04, t.data(), (int)t.size());
    render(1200 / kBlock);
    if (&pat != &pats.back()) render(1200 / kBlock);
  }
  render(1200 / kBlock);
  std::vector<double> x;
  for (int i = 0; i < 1200; ++i) x.push_back(ring[i][0]);
  return x;
}
static double diff(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0, n = 0;
  for (size_t i = 0; i < a.size(); ++i) s += (a[i] - b[i]) * (a[i] - b[i]), n += b[i] * b[i];
  return std::sqrt(s / (n + 1));
}
int main() {
  core::hooks.to_rytm = out;
  // the same trigs, the last with a new PAT vs with the PAT before: if the new PAT is not played, they are equal
  std::vector<std::vector<int>> seqs = {{0, 9}, {0, 9, 5}, {0, 9, 5, 0}, {0, 9, 5, 0, 9}, {3, 3, 9}};
  for (float chd : {0.0f, 20.0f})
  for (auto& seq : seqs) {
    g_chd = chd;
    printf("%s ", chd ? "poly (a chord)" : "mono");
    std::vector<int> same = seq;
    same.back() = seq[seq.size() - 2];
    double d = diff(run(seq), run(same));
    printf("PATs");
    for (int p : seq) printf(" %d", p);
    printf(": the last trig with its new PAT vs with the PAT before: %.3f\n", d);
    CHECK(d > 0.3);
  }
  printf("test_fm_pat: PASS\n");
  return 0;
}
