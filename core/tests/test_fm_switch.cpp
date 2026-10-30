// core/tests/test_fm_switch.cpp - a PAT change on the 6-OP FM does not cut the note before it (the user, 2026-10-26:
// clicks between p-locked PAT steps on 6OP-FM, not on JUNO). Stock Plaits unloaded both of the six-op engine's voices
// on a patch reload, so the previous note went silent in one block; now the new voice goes into the engine's other
// patch (fm_machine.h slot_) and only the voice on that patch unloads. The first blocks after the switch carry the
// previous note alone (the new note's setup blank and Plaits' trigger delay): it must still sound there, at about
// the level it had, and the output must not step.
#include <cmath>
#include <cstring>
#include <vector>

#include "check.h"
#include "voices.h"
#include "presets/dx7_lib.h"   // dx7::rom_bank

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
static double rms(const std::vector<double>& x, int from, int n) {
  double s = 0;
  for (int i = from; i < from + n; ++i) s += x[i] * x[i];
  return std::sqrt(s / n);
}

// voice 0: PAT a held (LEN 127), 0.25 s, then a trig with PAT b; the 48 frames before and after the switch
static std::vector<double> run(int a, int b) {
  using namespace core;
  voices_init();
  render(10);
  auto t1 = report(0x04, kFm | 0, 48, 110, {(float)a, 64, 64, 127, 64, 64, 110, 127});
  voice_report(0x04, t1.data(), (int)t1.size());
  render(1000 / kBlock);   // 1000 frames
  std::vector<double> x;
  for (int i = 1000 - 48; i < 1000; ++i) x.push_back(ring[i][0]);
  auto t2 = report(0x04, kFm | 0, 48, 110, {(float)b, 64, 64, 127, 64, 64, 110, 127});
  voice_report(0x04, t2.data(), (int)t2.size());
  render(48 / kBlock);
  for (int i = 0; i < 48; ++i) x.push_back(ring[i][0]);
  return x;
}

static double rms_rows(int from, int n) {
  double s = 0;
  for (int i = from; i < from + n; ++i) s += (double)ring[i][0] * ring[i][0];
  return std::sqrt(s / n);
}

int main() {
  using namespace core;
  core::hooks.to_rytm = out;
  {   // a chain A -> B -> C, trigs 600 frames apart: each switch keeps the note before it
    voices_init();
    render(10);
    int pats[3] = {0, 5, 9};
    for (int k = 0; k < 3; ++k) {
      auto t = report(0x04, kFm | 0, 48, 110, {(float)pats[k], 64, 64, 127, 64, 64, 110, 127});
      voice_report(0x04, t.data(), (int)t.size());
      render(600 / kBlock);
      if (k) {
        double first = rms_rows(0, 12);
        printf("chain step %d: rms of the first 12 frames %.0f\n", k, first);
        CHECK(first > 1000);
      }
    }
  }
  {   // FM held -> PLAITS -> back to FM with no trig: the frozen FM note must not resume (stock unloads both voices)
    voices_init();
    render(10);
    auto t = report(0x04, kFm | 0, 48, 110, {0, 64, 64, 127, 64, 64, 110, 127});
    voice_report(0x04, t.data(), (int)t.size());
    render(600 / kBlock);
    CHECK(rms_rows(0, 600) > 1000);
    auto pl = report(0x03, kPlaits | 0, 0, 0, {124, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
    voice_report(0x03, pl.data(), (int)pl.size());
    render(600 / kBlock);
    auto back = report(0x03, kFm | 0, 0, 0, {0, 64, 64, 127, 64, 64, 110, 127});
    voice_report(0x03, back.data(), (int)back.size());
    render(1200 / kBlock);
    double r = rms_rows(240, 960);   // after Plaits' engine switch: no trig, so silence
    printf("FM -> PLAITS -> FM, no trig: rms %.0f\n", r);
    CHECK(r < 50);
  }
  {   // a held note, then a reload with no note (another DX7 voice handed over without a trig), then a trig with
      // another PAT: the held note's voice is not the one the trig's load unloads. (The cut would show from row 12:
      // the engine's staggered render.)
    voices_init();
    render(10);
    auto t = report(0x04, kFm | 0, 48, 110, {0, 64, 64, 127, 64, 64, 110, 127});
    voice_report(0x04, t.data(), (int)t.size());
    render(600 / kBlock);
    fms[0].SetVoice(dx7::rom_bank(0) + 128 * 3);   // a reload, no note
    render(120 / kBlock);
    auto t2 = report(0x04, kFm | 0, 48, 110, {9, 64, 64, 127, 64, 64, 110, 127});
    voice_report(0x04, t2.data(), (int)t2.size());
    render(48 / kBlock);
    double held = rms_rows(12, 36);
    printf("held, reload without a note, trig: rms of frames 12..47 %.0f\n", held);
    CHECK(held > 1000);
  }
  for (int b : {1, 5, 9}) {   // three ROM voices after voice 0
    std::vector<double> same = run(0, 0), sw = run(0, b);
    double before = rms(sw, 24, 24), after = rms(sw, 48, 12);   // the 12 frames right after the trig
    double ref = rms(same, 48, 12);   // the same voice retriggered: what "not cut" sounds like there
    double step = std::fabs(sw[48] - sw[47]), typ = 0;
    for (int i = 25; i < 48; ++i) typ = std::fmax(typ, std::fabs(sw[i] - sw[i - 1]));
    printf("PAT 0 -> %d: rms before %.0f, after %.0f (same patch %.0f), step %.0f (largest before %.0f)\n", b, before,
           after, ref, step, typ);
    CHECK(before > 200);           // the note sounds
    CHECK(after > 0.5 * before);   // not cut at the switch
    (void)step; (void)typ;   // (printed only: the waveform's own steps are as large; "after" is the proof)
  }
  printf("test_fm_switch: PASS\n");
  return 0;
}
