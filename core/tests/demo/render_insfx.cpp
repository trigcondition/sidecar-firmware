// core/tests/demo/render_insfx.cpp - the INSERT effects' and FX buses' demo (make -C core demo-insfx OUT=dir): a
// WAVETABLE chord pad on voice 1 and a WAVETABLE arpeggio on voice 2 through the engine's own path (host_sysex reports,
// render_voices with the voices' INSERT chains), the voices' sends through send FX 1 (CLOUDSEED) and its bus chain and
// through bus 3 (dry, its own chain), mixed as the Rytm's MAIN would carry them (the voices panned a little apart, the
// returns in stereo). Sections of 8 s (one progression each): dry; FILTER under an LFO (I1) + TREMOLO; CRUSH with
// stepped BITS locks (as the sequencer's p-locks set them) + RINGMOD; DRIVE + EQ, a DELAY chorus; the buses (CLOUDSEED
// with an EQ on its return, bus 3 DELAY + auto-pan); everything, the TYPEs switched every bar (click-free). Writes
// OUT/insfx_demo.wav (48 kHz stereo 16-bit; afconvert -f m4af -d aac turns it into m4a), the remote INSERT page as
// OUT/rui_insert_*.ppm, and the chains' CPU on this Mac with the Pi 4 estimate (the boot bench's PLAITS ratio).
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "lfo.h"
#include "notes.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"

using namespace core;

namespace {
struct Ev {
  double t;
  std::function<void()> f;
};
std::vector<Ev> evs;
void at(double t, std::function<void()> f) { evs.push_back({t, std::move(f)}); }
std::string g_dir;

void cmd(const char* c) {
  char o[300];
  if (!ins::command(c, o, sizeof o) && !lfo::command(c, o, sizeof o) && !out::command(c, o, sizeof o)) printf("?? %s\n", c);
}

void ppm(const char* name) {   // the remote page as the Rytm would show it (text in the preview font), 4x
  ui::Canvas c;
  uint8_t fb[ui::kBytes];
  for (int k = 0; k < ui::extra_pages(); ++k)
    if (!strcmp(ui::extra_page(k).title, "INSERT")) ui::remote_render(6 + k, 0, nullptr, c);
  ui::preview(c, fb);
  std::string path = g_dir + "/" + name;
  FILE* f = fopen(path.c_str(), "wb");
  const int s = 4;
  fprintf(f, "P6 %d %d 255\n", 128 * s, 64 * s);
  for (int y = 0; y < 64 * s; ++y)
    for (int x = 0; x < 128 * s; ++x) {
      int X = x / s, Y = y / s;
      bool on = fb[(7 - Y / 8) + 8 * X] >> (Y % 8) & 1;
      bool grid = x % s == s - 1 || y % s == s - 1;
      uint8_t c3[3] = {(uint8_t)(on ? 232 : 12), (uint8_t)(on ? 238 : 14), (uint8_t)(on ? 255 : 22)};
      for (int k = 0; k < 3; ++k) fputc(grid ? c3[k] * 8 / 10 : c3[k], f);
    }
  fclose(f);
  printf("  %s\n", path.c_str());
}
}  // namespace

int main(int argc, char** argv) {
  g_dir = argc > 1 ? argv[1] : "build";
  voices_init();
  wt::begin(false);
  ins::reset();
  lfo::reset();
  memset((void*)&fx::control, 0, sizeof fx::control);
  static fx::SendFx sfx(48000.0f);
  const int F = kWaveMachine;
  const int kRoot[4] = {57, 53, 48, 55}, kChd[4] = {7, 6, 13, 4};
  const double kSec = 8.0;
  // P1..P16: TBL POS WARP AMT UNI SUB CUT RES ATK DEC SUS ENV CHD INV SPR VOI
  for (int sec = 0; sec < 6; ++sec)
    for (int c = 0; c < 4; ++c) {
      double t = sec * kSec + c * 2.0;
      std::vector<float> pad = {0, 40, 0, 0, 90, 30, 70, 10, 30, 90, 110, 64, (float)kChd[c], 1, 1, 0};
      at(t, [=] { wg::trig(F, 0, kRoot[c], 100, pad); });
      at(t + 1.95, [] { wg::off(0); });
      for (int n = 0; n < 16; ++n) {   // the arpeggio: 16ths at 120 BPM over the chord's tones
        const int iv[4] = {0, 7, 12, 15};
        int note = kRoot[c] + 12 + iv[n % 4] + (n >= 8 ? 12 : 0);
        std::vector<float> arp = {1, 20, 30, 0, 10, 0, 90, 30, 0, 50, 0, 80, 0, 0, 0, 0};
        at(t + n * 0.125, [=] { wg::trig(F, 1, note, 100, arp); });
        at(t + n * 0.125 + 0.09, [] { wg::off(1); });
      }
    }
  // the sections' settings (the console, as the Rytm's INSERT page / the touchscreen set them)
  at(1 * kSec, [] {
    cmd("ins=V1:A:type:filter"), cmd("ins=V1:A:p2:90"), cmd("ins=V1:A:p4:30");
    cmd("lfo=1:tgt:i1"), cmd("lfo=1:par:0"), cmd("lfo=1:dep:40"), cmd("lfo=1:wav:0"), cmd("lfo=1:rate:40");
    cmd("ins=V2:A:type:tremolo"), cmd("ins=V2:A:p1:100");
  });
  at(2 * kSec, [] {
    cmd("lfo=1:off"), cmd("ins=V1:A:type:crush"), cmd("ins=V1:A:p1:20");
    cmd("ins=V2:A:type:ringmod"), cmd("ins=V2:A:p1:80"), cmd("ins=V2:A:p4:90");
  });
  for (int b = 0; b < 16; ++b)   // stepped locks of CRUSH BITS, a beat each (as seq's lock trigs apply them)
    at(2 * kSec + b * 0.5, [b] { ins::lock(0, 0, 1, (b % 4) * 32 + 20); });
  at(3 * kSec, [] {
    ins::lock(0, 0, 1, -1);
    cmd("ins=V1:A:type:drive"), cmd("ins=V1:A:p1:70"), cmd("ins=V1:A:p3:40");
    cmd("ins=V1:B:type:eq"), cmd("ins=V1:B:p1:85"), cmd("ins=V1:B:p3:50");
    cmd("ins=V2:A:type:delay"), cmd("ins=V2:A:p1:75"), cmd("ins=V2:A:p3:90");
  });
  at(4 * kSec, [] {
    cmd("ins=V1:A:off"), cmd("ins=V1:B:off"), cmd("ins=V2:A:off");
    fx::control.knob[0][0] = fx::kCloudSeed;   // send FX 1 = bus 1
    for (int k = 2; k < 6; ++k) fx::control.knob[0][k] = 70;
    cmd("ins=B1:A:type:eq"), cmd("ins=B1:A:p1:40"), cmd("ins=B1:A:p3:90");
    cmd("ins=V1:send:1:100"), cmd("ins=V2:send:1:70");
    cmd("ins=V2:send:3:110"), cmd("ins=B3:A:type:delay"), cmd("ins=B3:A:p1:127"), cmd("ins=B3:A:p2:80"), cmd("ins=B3:A:p4:100");
    cmd("ins=B3:B:type:tremolo"), cmd("ins=B3:B:p1:70"), cmd("ins=B3:B:p4:127");
  });
  const char* kTypes5[] = {"filter", "comp", "drive", "eq"};
  for (int b = 0; b < 4; ++b)
    at(5 * kSec + b * 2.0, [b, kTypes5] {
      char c[48];
      snprintf(c, sizeof c, "ins=V1:A:type:%s", kTypes5[b]);
      cmd(c);
      cmd(b % 2 ? "ins=V2:A:type:delay" : "ins=V2:A:type:tremolo");
    });
  std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });

  static int16_t ring[kBlock][kVoices];
  static float stems[kBlock][8];
  uint64_t mach[kMachines] = {0};
  std::vector<float> L, R;
  size_t e = 0;
  uint64_t us = 1000000;
  const double secs = 6 * kSec + 2.0;
  for (double t = 0; t < secs; t += kBlock / 48000.0) {
    while (e < evs.size() && evs[e].t <= t) evs[e++].f();
    for (int i = 0; i < 2000 && wt::step(); ++i) {}
    us += 250;
    lfo::tick((uint32_t)us);
    render_voices(ring, kBlock, 0, 0, kVoices, mach);
    float vs[kBlock][3], pre[kBlock][2], post[kBlock][2], b3[kBlock];
    ins::voice_sends(ring, kBlock, vs);
    sfx.Process(stems, kBlock, pre, fx::control, post, nullptr, vs);
    for (int i = 0; i < kBlock; ++i) b3[i] = vs[i][2];
    ins::bus3(b3, kBlock, pre, post);
    for (int i = 0; i < kBlock; ++i) {
      float v0 = ring[i][0] / 32768.0f, v1 = ring[i][1] / 32768.0f;
      L.push_back(0.6f * v0 + 0.4f * v1 + pre[i][0] + post[i][0]);
      R.push_back(0.4f * v0 + 0.6f * v1 + pre[i][1] + post[i][1]);
    }
  }
  float pk = 0;
  for (size_t i = 0; i < L.size(); ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
  float g = pk > 0.0f ? 0.9f / pk : 1.0f;   // normalised to -1 dBFS
  wg::write_file(g_dir + "/insfx_demo.wav", wg::wav_bytes((int)L.size(), 2, 16, 48000, 1, "", [&](int i, int c) {
                   return (double)((c ? R[i] : L[i]) * g);
                 }));
  printf("insfx_demo.wav: %.1f s, peak %.2f (x %.2f), engine faults %u\n", L.size() / 48000.0, pk, g,
         ins::engine().Faults());

  // the remote INSERT page: the TRACK view of voice 1 (FILTER + EQ), and the BUS view of bus 3
  cmd("ins=V1:A:type:filter"), cmd("ins=V1:B:type:eq");
  ui::ins_page_set(false, 0, 0, 0);
  ppm("rui_insert_track.ppm");
  ui::ins_page_set(true, 0, 2, 0);
  ppm("rui_insert_bus3.ppm");
  // the OUT route (docs/STEREO.md): voice 1 on POST - the TRACK view's OUT box, the OUT view
  cmd("out=V1:post"), cmd("out=V1:pan:40"), cmd("out=V1:wid:90");
  ui::ins_page_set(false, 0, 0, 0);
  ppm("rui_insert_track_post.ppm");
  ui::ins_page_set(false, 0, 0, 0, true);
  ppm("rui_insert_out.ppm");
  cmd("out=V1:trk");
  ui::ins_page_set(false, 0, 0, 0, true);
  ppm("rui_insert_out_trk.ppm");

  // CPU: 8 voices x 2 slots, as render_voices runs them; the Pi 4 from PLAITS' boot-bench ratio (0.73 % a voice there)
  {
    NoteAdapter na([](const uint8_t* d, int n) { host_sysex(d, n); });
    voices_init();
    na.SetMachine(0, "plaits");
    na.Channel(0x90, 48, 100);
    static int16_t blk[kBlock][kVoices];
    uint64_t m2[kMachines] = {0}, sum = 0;
    for (int b = 0; b < 4000; ++b) sum += render_voices(blk, kBlock, 0, 0, 1, m2);
    double plaits = sum / 1e9 * 100, ratio = 0.73 / plaits;
    const int pairs[4][2] = {{ins::kFilter, ins::kDelay}, {ins::kComp, ins::kEq}, {ins::kDrive, ins::kTrem}, {ins::kCrush, ins::kRing}};
    for (auto& pr : pairs) {
      ins::Engine en(48000.0f);
      static float lines[ins::kLines][ins::kLine];
      en.Place(lines);
      en.Reset();
      ins::Control c = {};
      for (int v = 0; v < 8; ++v)
        for (int s = 0; s < 2; ++s) {
          c.type[v][s] = (uint8_t)pr[s];
          for (int k = 0; k < 4; ++k) c.play[v][s][k] = (uint8_t)(40 + 20 * k);
        }
      float x[kBlock];
      auto t0 = std::chrono::steady_clock::now();
      for (int n = 0; n < 4000; ++n)
        for (int v = 0; v < 8; ++v) {
          for (int s = 0; s < kBlock; ++s) x[s] = 0.3f * std::sin(0.05f * (n * kBlock + s) * (v + 1));
          en.ProcessMono(v, x, kBlock, c);
        }
      double pct = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 100;
      printf("CPU %s + %s, 8 voices: Mac %.2f %% of a core, Pi 4 estimate %.1f %% (x %.1f)\n", ins::type_name(pr[0]),
             ins::type_name(pr[1]), pct, pct * ratio, ratio);
    }
  }
  return 0;
}
