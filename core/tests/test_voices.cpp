// core/tests/test_voices.cpp - the voices on the Mac: a PLAITS trig sounds on its voice only, a JUNO note held
// until 0x11 then released, DAISYSP sounds, a user slot without plugins is silent, a kit's DIGIDRUM track (0x40,
// DIGIDRUM's old page: removed 2026-10-29) plays as the built-in MD (FM BD for its EFM-BD), the machine stats count the
// block. Reports as sim/protocol.py builds them (core_golden.py's helpers in C++).
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "host/core_port.h"
#include "md/md.h"
#include "voices.h"

static std::string g_log;   // every line since the last clear
static void sink(const char* l, int n) { g_log.append(l, n).append("\n"); }
static int count(const std::string& h, const char* n) {
  int c = 0;
  for (size_t at = h.find(n); at != std::string::npos; at = h.find(n, at + 1)) ++c;
  return c;
}

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back(note); m.push_back(vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}

static int16_t ring[core::kBlock * 400][core::kVoices];

static double rms(int voice, int frames) {   // frames from ring row 0
  double s = 0;
  for (int i = 0; i < frames; ++i) s += (double)ring[i][voice] * ring[i][voice];
  return std::sqrt(s / frames);
}

static void render(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) core::render_voices(ring, core::kBlock * 400, b * core::kBlock, 0, core::kVoices, mach);
}

int main() {
  using namespace core;
  hooks.to_rytm = out;
  host_set_log_sink(sink);
  voices_init();
  render(10);
  for (int v0 = 0; v0 < kVoices; ++v0) CHECK(rms(v0, 120) == 0);   // nothing live: silence
  auto t = report(0x04, kPlaits | 0, 48, 100, {124, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  voice_report(0x04, t.data(), (int)t.size());
  CHECK(v[0].live && !v[0].juno && v[0].patch.engine == kPlaitsModels[16]);
  render(400);
  CHECK(rms(0, 4800) > 300 && rms(1, 4800) == 0);
  auto j = report(0x04, kJuno | 1, 60, 100, {0, 80, 20, 64, 10, 64, 64, 127});
  voice_report(0x04, j.data(), (int)j.size());
  CHECK(v[1].juno && juno_pat[1] == 0);
  CHECK(!g_out.empty() && g_out.back()[4] == 0x15 && g_out.back()[5] == 1);   // the patch's knobs to the Rytm
  render(400);
  double held = rms(1, 4800);
  CHECK(held > 300);
  voice_note_off(1);
  for (int k = 0; k < 50; ++k) render(400);   // 5 s: any factory patch's release is over by then
  CHECK(rms(1, 4800) < held / 4);   // released
  // voice 4 sounding PLAITS, then a kit's DIGIDRUM track (0x40 with DIGIDRUM's page, no MD tag): MD at once (EFM-BD's
  // ENG zone -> FM BD), said once
  auto p4 = report(0x04, kPlaits | 4, 48, 100, {124, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  voice_report(0x04, p4.data(), (int)p4.size());
  CHECK(v[4].live && !v[4].md);
  g_log.clear();
  auto d = report(0x04, kMd | 4, 60, 100, {6, 64, 64, 64, 64, 64, 64, 64});
  voice_report(0x04, d.data(), (int)d.size());
  CHECK(v[4].md && v[4].live && !v[4].daisy && !v[4].juno && !v[4].fm && v[4].user < 0 && md::voice_set(4) == 0);
  auto d3 = report(0x03, kMd | 4, -1, 0, {100, 64, 64, 64, 64, 64, 64, 64});   // ENG zone 8: TRX-RS -> MD TRX RS
  voice_report(0x03, d3.data(), (int)d3.size());
  CHECK(md::voice_set(4) == 21 && count(g_log, "DIGIDRUM's page") == 1);
  voice_report(0x04, d.data(), (int)d.size());
  CHECK(count(g_log, "trig voice 4 MD FM BD") == 2);
  CHECK(!strcmp(machine_short(kMd), "MD") && machine_of(0x40 | 4) == kMd);
  auto s = report(0x04, kDaisy | 3, 57, 100, {0, 64, 64, 64, 0, 64, 64, 100});
  voice_report(0x04, s.data(), (int)s.size());
  auto u = report(0x04, 0x50 | 6, 60, 100, {64});
  voice_report(0x04, u.data(), (int)u.size());
  CHECK(v[6].user == 0 && !v[6].live);   // no plugins: silent
  render(400);
  CHECK(rms(4, 4800) > 100 && rms(3, 4800) > 100 && rms(6, 4800) == 0);
  uint64_t mach[kMachines] = {0};
  // 100 blocks: host cycles() are nanoseconds (core/host/platform_host.cpp), but macOS's CLOCK_MONOTONIC steps
  // in whole microseconds, so one block's voice may read 0
  uint32_t dt = 0;
  for (int b = 0; b < 100; ++b) dt += render_voices(ring, kBlock * 400, b * kBlock, 0, kVoices, mach);
  CHECK(dt > 0 && mach[kMPlaits] > 0 && mach[kMDaisy] > 0 && mach[kMMd] > 0 && mach[kMUser] == 0);
  // back to PLAITS and to MD again: a fresh MD voice (logged again)
  voice_report(0x04, p4.data(), (int)p4.size());
  CHECK(v[4].live && !v[4].md);
  voice_report(0x04, d.data(), (int)d.size());
  CHECK(v[4].md && count(g_log, "DIGIDRUM's page") == 2);
  printf("test_voices: PASS\n");
  return 0;
}
