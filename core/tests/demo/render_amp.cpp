// core/tests/demo/render_amp.cpp - INSERT AMP's demos (make -C core demo-amp OUT=dir [MODEL=file.namb|.nam]), 48 kHz
// 16-bit. Unless MODEL names a real A2-Lite capture, capture A is SYNTHETIC (amp::synthetic_weights: random A2-Lite
// weights, not an amp: its colour says nothing about a real capture); capture B is always synthetic.
//   OUT/amp_demo.wav       (mono) 12 s of a plucked guitar-like line through voice 1's slot A: 0-3 s dry (AMP OFF),
//                          3-6 s capture A, 6-9 s GAIN +12 dB TONE brighter, 9-12 s MODL -> capture B (loaded while A
//                          plays, a 10 ms crossfade)
//   OUT/amp_kick_demo.wav  (stereo) a 909-style kick (synthesised: a sine swept 230 -> 50 Hz, a click), four on the
//                          floor at 120 BPM, 8 s a section: dry; -> AMP (capture A); -> AMP with GAIN up; -> AMP ->
//                          SPACE (send FX TYPE 5, core/fx/tape_fx.h) H1 slapback; H123 synced 1/8 with REPEAT and AGE;
//                          a dub throw (one beat sent at REPEAT 112, then the send cut: the echoes run on)
// The slot runs as on the Pi: the /NAM/ list and the loads on the control side between blocks, the chain a 12-frame
// block at a time; SPACE as render_tape.cpp runs it (its own SendFx, MIDI clock at 120 BPM).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "fx/amp.h"
#include "fx/insert.h"
#include "fx/send_fx.h"
#include <memory>

using namespace core;

static void put(std::vector<uint8_t>& b, uint32_t v, int n) {
  for (int i = 0; i < n; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}
static std::vector<uint8_t> namb(const std::vector<float>& w) {   // as test_amp.cpp's make_namb
  const size_t cfg = 68, woff = (84 + cfg + 3) & ~(size_t)3, size = woff + 4 * w.size();
  std::vector<uint8_t> b(size, 0);
  auto p16 = [&](size_t a, uint16_t v) { b[a] = (uint8_t)v, b[a + 1] = (uint8_t)(v >> 8); };
  auto p32 = [&](size_t a, uint32_t v) { for (int i = 0; i < 4; ++i) b[a + i] = (uint8_t)(v >> (8 * i)); };
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

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : "build";
  const char* model = argc > 2 && argv[2][0] ? argv[2] : nullptr;   // a real A2-Lite capture for A
  char tmpl[] = "/tmp/render_amp.XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  std::string card = tmpl;
  mkdir((card + "/NAM").c_str(), 0755);
  mkdir((card + "/Samples").c_str(), 0755);
  if (model) {
    FILE* f = fopen(model, "rb");
    if (!f) {
      fprintf(stderr, "render_amp: cannot read %s\n", model);
      return 1;
    }
    std::vector<uint8_t> d(1 << 20);
    d.resize(fread(d.data(), 1, d.size(), f));
    fclose(f);
    const char* ext = strrchr(model, '.');
    write(card + "/NAM/Model A" + (ext ? ext : ".namb"), d.data(), d.size());
  }
  for (int k = model ? 1 : 0; k < 2; ++k) {
    std::vector<float> w(amp::kWeights);
    amp::synthetic_weights(w.data(), 1 + k);
    auto b = namb(w);
    write(card + (k ? "/NAM/Synthetic B.namb" : "/NAM/Model A.namb"), b.data(), b.size());
  }
  host_set_card(tmpl);
  engine_begin();
  ins::reset();
  for (int i = 0; i < 100; ++i) amp::step();

  const int n = 48000 * 12;
  std::vector<float> x(n), y(n);
  const float notes[8] = {82.41f, 98.0f, 110.0f, 123.47f, 146.83f, 110.0f, 98.0f, 164.81f};
  for (int i = 0; i < n; ++i) {   // a plucked line: saw + octave, a fast decay, eighth notes at 120 BPM
    int k = (i / 12000) & 7;
    float t = (float)(i % 12000) / 48000.0f, ph = notes[k] * t;
    float saw = 2.0f * (ph - std::floor(ph)) - 1.0f;
    x[i] = 0.35f * std::exp(-5.0f * t) * (0.6f * saw + 0.4f * std::sin(2 * (float)M_PI * 2 * ph));
  }
  for (int i = 0; i < n; i += 12) {
    if (i == 48000 * 3) ins::set_type(0, 0, ins::kAmp), ins::set_param(0, 0, 0, amp::knob_for_model(0));
    if (i == 48000 * 6) ins::set_param(0, 0, 1, 106), ins::set_param(0, 0, 2, 96), ins::set_param(0, 0, 3, 90);
    if (i == 48000 * 9) ins::set_param(0, 0, 0, amp::knob_for_model(1));
    amp::step();
    amp::service(0);
    memcpy(&y[i], &x[i], 12 * sizeof(float));
    ins::engine().ProcessMono(0, &y[i], 12, ins::control);
  }
  float pk = 0;
  for (float v : y) pk = std::max(pk, std::fabs(v));
  float g = pk > 0 ? 0.89f / pk : 1.0f;   // -1 dBFS
  std::vector<uint8_t> b;
  b.insert(b.end(), {'R', 'I', 'F', 'F'}), put(b, 36 + 2 * n, 4), b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put(b, 16, 4), put(b, 1, 2), put(b, 1, 2), put(b, 48000, 4), put(b, 96000, 4), put(b, 2, 2), put(b, 16, 2);
  b.insert(b.end(), {'d', 'a', 't', 'a'}), put(b, 2 * n, 4);
  for (float v : y) {
    float q = std::round(v * g * 32767.0f);
    put(b, (uint32_t)(int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q), 2);
  }
  mkdir(out.c_str(), 0755);
  write(out + "/amp_demo.wav", b.data(), b.size());
  char st[512];
  amp::command("amp", st, sizeof st);
  printf("amp_demo.wav: 12 s, peak %.2f (x %.2f), %s captures; %s; faults %u\n", pk, g,
         model ? "A real, B SYNTHETIC" : "SYNTHETIC (random A2-Lite weights, not amps)", st, ins::engine().Faults());
  if (amp::status(0, 0) != amp::kPlaying) {
    fprintf(stderr, "render_amp: capture B does not play (%s)\n", amp::status_long(amp::status(0, 0)));
    return 1;
  }

  // ---- the kick through AMP and SPACE
  {
    using namespace core::fx;
    const int kFpb = 24000;   // 120 BPM
    auto fx = std::make_unique<SendFx>(48000.0f);
    memset((void*)&fx::control, 0, sizeof fx::control);
    ins::set_param(0, 0, 0, amp::knob_for_model(0));   // capture A
    ins::set_param(0, 0, 1, 64), ins::set_param(0, 0, 2, 64), ins::set_param(0, 0, 3, 109);
    ins::set_type(0, 0, ins::kOff);
    for (int i = 0; i < 400; ++i) amp::step(), amp::service(0);
    struct Sec {
      bool amp;
      uint8_t gain, time, rpt, mode, age, send;
      const char* what;
    };
    const Sec secs[] = {
        {false, 64, 0, 0, 0, 0, 0, "dry kick"},
        {true, 64, 0, 0, 0, 0, 0, "kick -> AMP (capture A, GAIN 0 dB)"},
        {true, 112, 0, 0, 0, 0, 0, "kick -> AMP, GAIN +15 dB"},
        {true, 112, 25, 35, 0, 10, 110, "kick -> AMP -> SPACE H1 slapback"},
        {true, 112, 96, 55, 80, 50, 100, "kick -> AMP -> SPACE H123 synced 1/8, REPEAT 55, AGE 50"},
        {true, 112, 96, 112, 80, 70, 120, "dub throw: one beat sent at REPEAT 112, then the send cut"},
    };
    std::vector<int16_t> pcm;
    long frame = 0;
    double next_tick = 0;
    fx::clock_in(0xFA);
    float pk = 0;
    std::vector<float> mix;
    for (const Sec& sc : secs) {
      printf("render_amp kick: %5.1f s  %s\n", frame / 48000.0, sc.what);
      if (sc.amp && ins::control.type[0][0] != ins::kAmp) ins::set_type(0, 0, ins::kAmp), ins::set_param(0, 0, 0, amp::knob_for_model(0));
      ins::set_param(0, 0, 1, sc.gain);
      ins::set_param(0, 0, 3, sc.gain > 64 ? 91 : 109);   // LEV: -6 dB with GAIN up (near the dry kick's level)
      for (long i = 0; i < 8L * 48000; i += 12) {
        while (next_tick < (double)(frame + 12)) fx::clock_in(0xF8), next_tick += 48000.0 * 60.0 / 120.0 / 24.0;
        uint8_t send = sc.send;
        if (sc.rpt >= 100 && i >= kFpb) send = 0;   // the throw: only the first beat goes to SPACE
        fx::control.knob[0][0] = kTape, fx::control.knob[0][1] = 0, fx::control.knob[0][2] = sc.time;
        fx::control.knob[0][3] = sc.rpt, fx::control.knob[0][4] = sc.mode, fx::control.knob[0][5] = sc.age;
        fx::control.send[0][0] = send;
        float k[12];
        for (int j = 0; j < 12; ++j) {   // the 909-style kick: a sine swept 230 -> 50 Hz, a click, on every beat
          const long p = (frame + j) % kFpb;
          const float t = (float)p / 48000.0f;
          const float ph = 50.0f * t + 180.0f * (1.0f - std::exp(-t * 28.0f)) / 28.0f;
          float v = std::sin(6.2831853f * ph) * std::exp(-t * 6.0f) * (t < 0.0008f ? t / 0.0008f : 1.0f);
          float click = p < 96 ? 0.5f * std::sin(6.2831853f * 3200.0f * t) * (1.0f - (float)p / 96.0f) : 0.0f;
          k[j] = 0.6f * (v + click);
        }
        amp::step();
        amp::service(0);
        ins::engine().ProcessMono(0, k, 12, ins::control);
        float st[12][kStems] = {}, o[12][2];
        for (int j = 0; j < 12; ++j) st[j][0] = k[j];
        fx->Process(st, 12, o, fx::control);
        for (int j = 0; j < 12; ++j)
          for (int c = 0; c < 2; ++c) {
            float v = 0.7f * k[j] + 1.4f * o[j][c];
            mix.push_back(v);
            pk = std::max(pk, std::fabs(v));
          }
        frame += 12;
      }
    }
    float g = pk > 0 ? 0.89f / pk : 1.0f;   // -1 dBFS
    std::vector<uint8_t> w;
    const uint32_t nb = (uint32_t)(mix.size() * 2);
    w.insert(w.end(), {'R', 'I', 'F', 'F'}), put(w, 36 + nb, 4), w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put(w, 16, 4), put(w, 1, 2), put(w, 2, 2), put(w, 48000, 4), put(w, 192000, 4), put(w, 4, 2), put(w, 16, 2);
    w.insert(w.end(), {'d', 'a', 't', 'a'}), put(w, nb, 4);
    for (float v : mix) put(w, (uint32_t)(int16_t)std::lrint(v * g * 32767.0f), 2);
    write(out + "/amp_kick_demo.wav", w.data(), w.size());
    printf("amp_kick_demo.wav: %.1f s, peak %.2f (x %.2f), capture A %s, AMP faults %u, SPACE faults %u\n", frame / 48000.0, pk, g,
           model ? model : "SYNTHETIC (not an amp)", ins::engine().Faults(), fx->Faults());
  }
  std::string rm = "rm -rf " + card;
  return system(rm.c_str());
}
