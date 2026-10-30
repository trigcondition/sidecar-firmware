// core/tests/demo/render_livein.cpp - LIVE IN's demo (make -C core demo-livein OUT=dir [MODEL=file.nam|.namb]), 48 kHz
// 16-bit stereo: a synthetic guitar (Karplus-Strong plucks, a DI's level) as the Rytm's AUDIO IN -> LIVE IN on voice 1
// (the real machine path: render_voices, the voice's INSERT chain, its send) -> INSERT AMP (the capture MODEL names,
// else a SYNTHETIC one: random A2-Lite weights, not an amp) -> SPACE (send FX 1 TYPE 5 through bus 1), mixed as the
// Rytm would return them (the voice on TRK, SPACE's pair).
//   0-4 s    the DI alone (LIVE IN, no insert)
//   4-9 s    -> AMP (GAIN +9 dB)
//   9-15 s   -> AMP -> SPACE (H1 + H2, REPEAT 45 %, a little AGE)
//   15-19 s  MON TRIG: the same playing gated by 16th-note trigs (ATK 0.5 ms, DEC 90 ms) -> AMP -> SPACE
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "fx/amp.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "livein/livein.h"
#include "voices.h"

using namespace core;

static void put(std::vector<uint8_t>& b, uint32_t v, int n) {
  for (int i = 0; i < n; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}
static std::vector<uint8_t> namb(const std::vector<float>& w) {   // as render_amp.cpp's
  const size_t cfg = 68, woff = (84 + cfg + 3) & ~(size_t)3, size = woff + 4 * w.size();
  std::vector<uint8_t> b(size, 0);
  auto p16 = [&](size_t a, uint16_t x) { b[a] = (uint8_t)x, b[a + 1] = (uint8_t)(x >> 8); };
  auto p32 = [&](size_t a, uint32_t x) { for (int i = 0; i < 4; ++i) b[a + i] = (uint8_t)(x >> (8 * i)); };
  double rate = 48000.0;
  p32(0, 0x4E414D42u), p16(4, 1), p32(8, (uint32_t)size), p32(12, (uint32_t)woff), p32(16, (uint32_t)w.size()), p32(20, 4 + cfg);
  memcpy(&b[36], &rate, 8);
  b[80] = 3, p16(82, (uint16_t)cfg), b[84] = 1, b[86] = 1;
  p16(88, 1), p16(90, 1), p16(92, 1), p16(94, 3), p16(96, 3), p16(98, 16), b[101] = 23;
  memcpy(&b[woff], w.data(), 4 * w.size());
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    if (i >= 24 && i < 28) continue;
    c ^= b[i];
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
  }
  p32(24, c ^ 0xFFFFFFFFu);
  return b;
}
static void write(const std::string& p, const void* d, size_t n) {
  FILE* f = fopen(p.c_str(), "wb");
  if (!f || fwrite(d, 1, n, f) != n) {
    fprintf(stderr, "cannot write %s\n", p.c_str());
    exit(1);
  }
  fclose(f);
}

// A plucked string (Karplus-Strong with a pick position and a body low-pass): a DI-level guitar
struct Pluck {
  std::vector<float> line;
  size_t at = 0;
  float lp = 0, damp = 0.996f;
  uint32_t seed = 12345;
  void start(float hz, float amp) {
    size_t n = (size_t)(48000.0f / hz);
    line.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
      seed = seed * 1664525u + 1013904223u;
      float r = (float)(seed >> 8) / 8388608.0f - 1.0f;
      line[i] = amp * r;
    }
    for (size_t i = n - 1; i > n / 5; --i) line[i] -= 0.6f * line[i - n / 5];   // the pick a fifth along the string
    at = 0;
  }
  float tick() {
    if (line.empty()) return 0.0f;
    size_t n = line.size(), j = (at + 1) % n;
    float y = line[at];
    line[at] = damp * 0.5f * (y + line[j]);
    at = j;
    lp += 0.45f * (y - lp);
    return lp;
  }
};

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : "build";
  const char* model = argc > 2 && argv[2][0] ? argv[2] : nullptr;
  char tmpl[] = "/tmp/render_livein.XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  std::string card = tmpl;
  mkdir((card + "/NAM").c_str(), 0755);
  mkdir((card + "/Samples").c_str(), 0755);
  if (model) {
    FILE* f = fopen(model, "rb");
    if (!f) {
      fprintf(stderr, "render_livein: cannot read %s\n", model);
      return 1;
    }
    std::vector<uint8_t> d(1 << 20);
    d.resize(fread(d.data(), 1, d.size(), f));
    fclose(f);
    const char* ext = strrchr(model, '.');
    write(card + "/NAM/Amp" + (ext ? ext : ".namb"), d.data(), d.size());
  } else {
    std::vector<float> w(amp::kWeights);
    amp::synthetic_weights(w.data(), 1);
    auto b = namb(w);
    write(card + "/NAM/Amp.namb", b.data(), b.size());
  }
  host_set_card(tmpl);
  engine_begin();
  ins::reset();
  for (int i = 0; i < 100; ++i) amp::step();
  if (amp::models() != 1) {
    fprintf(stderr, "render_livein: the capture is not listed\n");
    return 1;
  }

  const int kV = 0;   // voice 1 (BD's)
  char b[600];
  livein::command("livein=voice:0", b, sizeof b);
  const float knobs[9] = {80, 64, 14, 30, 0, 0, 77, 0, 77};   // L+R, 0 dB, the gate at -73 dBFS, ALW
  for (int k = 0; k < 9; ++k) v[kV].p[k] = knobs[k];
  auto fxp = std::make_unique<fx::SendFx>(48000.0f);
  memset((void*)&fx::control, 0, sizeof fx::control);
  fx::control.knob[0][0] = fx::kTape, fx::control.knob[0][1] = 0;
  fx::control.knob[0][2] = 30, fx::control.knob[0][3] = 45, fx::control.knob[0][4] = 40, fx::control.knob[0][5] = 25;
  fx::clock_in(0xFA);

  // the line: E2 A2 D3 G3 B3 E4 arpeggios and a riff, 8ths at 110 BPM (a pluck every 13091 frames)
  const float notes[16] = {82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f, 246.94f, 196.0f,
                           98.0f,  146.83f, 196.0f,  246.94f, 293.66f, 392.0f,  293.66f, 246.94f};
  Pluck str[2];
  const int kStep = 13091, kSixteenth = kStep / 2;
  static int16_t ring[kBlock][kVoices], ring_r[kBlock][kVoices];
  static float stems[kBlock][fx::kStems];
  uint8_t st[kVoices];
  uint64_t mach[kMachines] = {};
  std::vector<float> mix;
  const long total = 19L * 48000;
  float pk = 0, in_pk = 0;
  double next_tick = 0;
  int section = -1;
  for (long f = 0; f < total; f += kBlock) {
    int sec = f < 4 * 48000 ? 0 : f < 9 * 48000 ? 1 : f < 15 * 48000 ? 2 : 3;
    if (sec != section) {
      section = sec;
      if (sec == 1) {
        ins::set_type(kV, 0, ins::kAmp), ins::set_param(kV, 0, 0, amp::knob_for_model(0));
        ins::set_param(kV, 0, 1, 88), ins::set_param(kV, 0, 2, 70), ins::set_param(kV, 0, 3, 62);   // GAIN +9 dB, TONE a little up, LEV -16 dB
      }
      if (sec == 2) ins::set_send(kV, 0, 100);
      if (sec == 3) v[kV].p[4] = 100, v[kV].p[5] = 0, v[kV].p[6] = 40, v[kV].p[7] = 0;   // MON TRIG, DEC ~90 ms
      const char* what[4] = {"the DI alone (LIVE IN, no insert)", "-> AMP (GAIN +9 dB)", "-> AMP -> SPACE",
                             "MON TRIG: 16th-note trigs gate it -> AMP -> SPACE"};
      printf("render_livein: %5.1f s  %s\n", f / 48000.0, what[sec]);
    }
    while (next_tick < (double)(f + kBlock)) fx::clock_in(0xF8), next_tick += 48000.0 * 60.0 / 110.0 / 24.0;
    float in[kBlock];
    for (int s = 0; s < kBlock; ++s) {
      long g = f + s;
      if (g % kStep == 0) {
        int k = (int)((g / kStep) % 16);
        str[k & 1].start(notes[k], k % 4 == 0 ? 0.30f : 0.20f);
      }
      if (sec == 3 && g % kSixteenth == 0) voice_play(kV, 60, 100);   // MON TRIG: a trig every 16th
      in[s] = 0.6f * (str[0].tick() + str[1].tick());
      in_pk = std::max(in_pk, std::fabs(in[s]));
    }
    amp::step();
    livein::in_frames(in, in, kBlock);
    render_voices(ring, kBlock, 0, 0, kVoices, mach, ring_r, st);
    float vs[kBlock][ins::kBuses], o[kBlock][2], post[kBlock][2];
    ins::voice_sends(ring, kBlock, vs);
    fxp->Process(stems, kBlock, o, fx::control, post, nullptr, vs);
    for (int s = 0; s < kBlock; ++s)
      for (int c = 0; c < 2; ++c) {
        float x = ring[s][kV] / 32767.0f + 1.2f * o[s][c];
        mix.push_back(x);
        pk = std::max(pk, std::fabs(x));
      }
  }
  float g = pk > 0 ? 0.89f / pk : 1.0f;   // -1 dBFS
  std::vector<uint8_t> w;
  const uint32_t nb = (uint32_t)(mix.size() * 2);
  w.insert(w.end(), {'R', 'I', 'F', 'F'}), put(w, 36 + nb, 4), w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put(w, 16, 4), put(w, 1, 2), put(w, 2, 2), put(w, 48000, 4), put(w, 192000, 4), put(w, 4, 2), put(w, 16, 2);
  w.insert(w.end(), {'d', 'a', 't', 'a'}), put(w, nb, 4);
  for (float x : mix) put(w, (uint32_t)(int16_t)std::lrint(x * g * 32767.0f), 2);
  mkdir(out.c_str(), 0755);
  write(out + "/livein_demo.wav", w.data(), w.size());
  livein::command("livein", b, sizeof b);
  printf("livein_demo.wav: %.1f s, the DI's peak %.2f, the mix's %.2f (x %.2f), capture %s, AMP %s, faults %u / %u\n%s\n",
         total / 48000.0, in_pk, pk, g, model ? model : "SYNTHETIC (not an amp)", amp::status_long(amp::status(kV, 0)),
         ins::engine().Faults(), fxp->Faults(), b);
  std::string rm = "rm -rf " + card;
  return system(rm.c_str());
}
