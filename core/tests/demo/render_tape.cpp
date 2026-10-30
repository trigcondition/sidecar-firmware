// core/tests/demo/render_tape.cpp - SPACE (send FX TYPE 5, core/fx/tape_fx.h) demo: a 120 BPM drum loop (synthesised
// kick, snare, hats; MIDI clock running) through FX 1 = SPACE in a few settings, dry + wet, into OUT/space_demo.wav
// (48 kHz stereo 16-bit). Sections of 8 s: H1 slapback; H123 synced 1/8; H2+3 with spring, worn tape; H123S at full
// AGE (wow); the runaway (REPEAT 115 in H2+3 + spring, the drums' send cut), the dub siren (H1 at REPEAT 120, TIME
// swept), then REPEAT back down.
// Build and run: make -C core demo-tape [OUT=dir].
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fx/send_fx.h"

using namespace core::fx;

static const int kSr = 48000, kFpb = 24000;   // 120 BPM: a beat 24000 frames, a 16th 6000, a tick 1000
static uint32_t g_rng = 1;
static float noise() {
  g_rng = g_rng * 1664525u + 1013904223u;
  return (float)(g_rng >> 8) / 8388608.0f - 1.0f;
}
// the loop (one bar of 16ths): kick on 1, 2+, 3; snare on 2, 4; hats on the 8ths
static void drums(long i, float* kick, float* snare, float* hat) {
  const long bar = 4 * kFpb, t = i % bar, s16 = t / (kFpb / 4), p = t % (kFpb / 4);
  const bool k = s16 == 0 || s16 == 6 || s16 == 8, sn = s16 == 4 || s16 == 12, h = s16 % 2 == 0;
  const float tp = (float)p / kSr;
  *kick = k ? 0.8f * std::sin(6.2831853f * (50.0f * tp + 60.0f * (1.0f - std::exp(-tp * 30.0f)) / 30.0f)) *
                  std::exp(-tp * 9.0f)
            : 0.0f;
  float n = noise();
  *snare = sn ? (0.5f * n * std::exp(-tp * 18.0f) + 0.3f * std::sin(6.2831853f * 190.0f * tp) * std::exp(-tp * 25.0f)) : 0.0f;
  *hat = h ? 0.15f * (n - 0.6f * noise()) * std::exp(-tp * 90.0f) : 0.0f;
}
struct Section {
  float secs;
  uint8_t time, rpt, mode, age, send;   // P2..P5, the snare / kick send (0..127)
  const char* what;
};

int main(int argc, char** argv) {
  std::string out = std::string(argc > 1 ? argv[1] : "build") + "/space_demo.wav";
  auto fx = std::make_unique<SendFx>((float)kSr);
  memset((void*)&control, 0, sizeof control);
  const Section secs[] = {
      {8, 25, 35, 0, 10, 110, "H1 slapback, REPEAT 35, new tape"},
      {8, 96, 55, 80, 50, 100, "H123 synced 1/8, REPEAT 55"},
      {8, 112, 65, 96, 95, 100, "H2+3 + spring, synced 1/8D, worn tape"},
      {8, 40, 60, 112, 127, 100, "H123 + spring, AGE 127 (full wow and flutter)"},
      {8, 30, 115, 96, 80, 100, "runaway: H2+3 + spring, REPEAT 115, the send cut after 2 s"},
      {8, 30, 120, 0, 80, 0, "dub siren: H1, REPEAT 120, TIME swept"},
      {6, 30, 60, 96, 80, 0, "REPEAT back to 60: the runaway dies away"},
  };
  std::vector<int16_t> pcm;
  long frame = 0;
  clock_in(0xFA);
  double next_tick = 0;
  for (const Section& s : secs) {
    printf("render_tape: %5.1f s  %s\n", (double)frame / kSr, s.what);
    const long n = (long)(s.secs * kSr);
    for (long i = 0; i < n; i += 12) {
      while (next_tick < (double)(frame + 12)) clock_in(0xF8), next_tick += kSr * 60.0 / 120.0 / 24.0;
      uint8_t time = s.time, send = s.send;
      if (s.rpt >= 100 && i > 2 * kSr) send = 0;   // the runaway: the send cut after 2 s
      if (s.mode == 0 && s.rpt >= 100)   // the siren: TIME swept down and up (tape speed: pitch)
        time = (uint8_t)(30 + 20 * std::sin(6.2831853f * 0.2f * (float)i / kSr));
      control.knob[0][0] = kTape, control.knob[0][1] = 0;
      control.knob[0][2] = time, control.knob[0][3] = s.rpt, control.knob[0][4] = s.mode, control.knob[0][5] = s.age;
      control.send[0][0] = (uint8_t)(send / 2), control.send[1][0] = send, control.send[8][0] = (uint8_t)(send / 3);
      float st[12][kStems] = {}, o[12][2], dry[12];
      for (int j = 0; j < 12; ++j) {
        float k, sn, h;
        drums(frame + j, &k, &sn, &h);
        st[j][0] = k, st[j][1] = sn, st[j][6] = h;   // BD, SD, CH stems
        dry[j] = k + sn + h;
      }
      fx->Process(st, 12, o, control);
      for (int j = 0; j < 12; ++j) {
        const bool drums_on = !(send == 0);   // the dry stops with the send in the runaway
        for (int c = 0; c < 2; ++c) {
          float v = (drums_on ? 0.5f * dry[j] : 0.0f) + 1.6f * o[j][c];
          v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
          pcm.push_back((int16_t)std::lrint(v * 32767.0f));
        }
      }
      frame += 12;
    }
  }
  FILE* f = fopen(out.c_str(), "wb");
  if (!f) return 1;
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  const uint32_t bytes = (uint32_t)(pcm.size() * 2);
  fwrite("RIFF", 1, 4, f), u32(36 + bytes), fwrite("WAVEfmt ", 1, 8, f), u32(16), u16(1), u16(2), u32(kSr),
      u32(kSr * 4), u16(4), u16(16), fwrite("data", 1, 4, f), u32(bytes);
  fwrite(pcm.data(), 2, pcm.size(), f);
  fclose(f);
  printf("render_tape: %s (%.1f s), faults %u\n", out.c_str(), (double)frame / kSr, fx->Faults());
  return fx->Faults() ? 1 : 0;
}
