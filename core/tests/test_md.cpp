// core/tests/test_md.cpp - the built-in MD on voice byte 0x40 (core/md/md.h; the interface: docs/MD-BUILTIN.md): the
// DIGIDRUM migration (its 11 ENG zones -> ENGINE / TYPE, the controls kept where they are the same), MD's own layout
// (the 0x224D tag: P9 ENGINE, P10 TYPE, P11 LEV), every one of the 30 TYPEs sounds, a TYPE change between hits, the
// sequencer's note, the labels the Rytm asks for (19 / 1A / 1B on slots 0x40..0x42), and the cost per voice (printed:
// the render core's per block, the control core's for a TYPE change).
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "glue/md_tables.h"   // (core/build/engines/MDKit: plugins/mdkit/md_tables.h)
#include "host/core_port.h"
#include "inst.h"
#include "md/md.h"
#include "voices.h"

extern "C" int mdk_builtin_default(int t, int k);

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static std::string g_log;
static void sink(const char* l, int n) { g_log.append(l, n).append("\n"); }

// a 04 (note >= 0) or 03 with P1..P16 and, if tag >= 0, the sound tag (43 / 41 bytes)
static std::vector<uint8_t> report(int vb, int note, int vel, std::vector<float> p, int tag) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)(note >= 0 ? 0x04 : 0x03), (uint8_t)vb};
  if (note >= 0) m.push_back((uint8_t)note), m.push_back((uint8_t)vel);
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back((uint8_t)i); m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f)); }
  if (tag >= 0) m.push_back((uint8_t)(tag >> 7 & 0x7f)), m.push_back((uint8_t)(tag & 0x7f));
  m.push_back(0xf7);
  return m;
}
static void send(const std::vector<uint8_t>& m) { voice_report(m[4], m.data(), (int)m.size()); }

static int16_t ring[kBlock * 400][kVoices];
static uint64_t mach[kMachines];
static double rms_of(int vi, int blocks) {   // render blocks (<= 400), voice vi's RMS
  for (int b = 0; b < blocks; ++b) render_voices(ring, kBlock * 400, b * kBlock, 0, kVoices, mach);
  double s = 0;
  for (int i = 0; i < blocks * kBlock; ++i) s += (double)ring[i][vi] * ring[i][vi];
  return std::sqrt(s / (blocks * kBlock));
}
static int lo(int z, int n) { return (z * 128 + n - 1) / n; }

static void test_legacy_map() {
  // DIGIDRUM's ENG zones (11) -> the set; P2..P8 kept as P1..P7 where the controls are the same, the rest defaults
  const int set[11] = {0, 1, 2, 3, 4, 5, 6, 7, 21, 22, 27}, keep[11] = {7, 7, 7, 7, 7, 7, 7, 7, 3, 3, 6};
  for (int z = 0; z < 11; ++z) {
    float p[16] = {(float)lo(z, 11), 10, 20, 30, 40, 50, 60, 70, 99, 99, 99, 99, 99, 99, 99, 99};
    float q[16];
    md::knobs_of(p, 16, false, q);
    CHECK(md::set_of(q) == set[z]);
    for (int k = 0; k < 8; ++k)
      CHECK(q[k] == (k < keep[z] ? 10.0f * (k + 1) : (float)mdk_builtin_default(set[z], k)));
    CHECK(q[10] == 100 && q[11] == 0 && q[15] == 0);   // LEV at its default; DIGIDRUM's sample page (P9..P16) ignored
    printf("  DIGIDRUM zone %2d -> MD %s (P1..P%d kept)\n", z, md::set_name(set[z]), keep[z]);
  }
  // the defaults table the spec and the mod use (gen_tables.py from voices.json) is mdkit's own
  for (int s = 0; s < md::kSets; ++s)
    for (int k = 0; k < 8; ++k) CHECK(mdt_default[s][k] == mdk_builtin_default(s, k));
  // an old 8-param report (P1..P8 only): the same
  float p8[8] = {(float)lo(9, 11), 1, 2, 3, 4, 5, 6, 7}, q[16];
  md::knobs_of(p8, 8, false, q);
  CHECK(md::set_of(q) == 22 && q[0] == 1 && q[2] == 3 && q[3] == (float)mdk_builtin_default(22, 3));
  // MD's layout: ENGINE / TYPE by zones, every set reachable at its zone's first value
  for (int e = 0, s = 0; e < 3; ++e) {
    int n = e == 0 ? 8 : e == 1 ? 9 : 13;
    for (int t = 0; t < n; ++t, ++s) {
      float m[16] = {0, 0, 0, 0, 0, 0, 0, 0, (float)lo(e, 3), (float)lo(t, n), 100};
      md::knobs_of(m, 16, true, q);
      CHECK(md::set_of(q) == s);
      m[9] = (float)(t + 1 < n ? lo(t + 1, n) - 1 : 127);   // the zone's last value
      md::knobs_of(m, 16, true, q);
      CHECK(md::set_of(q) == s);
    }
  }
}

static void test_play() {
  // every TYPE in MD's layout sounds on voice 4, alone (no NaN, not clipped hard); the render cost per block
  double worst_us = 0;
  int worst = -1;
  for (int e = 0, s = 0; e < 3; ++e) {
    int n = e == 0 ? 8 : e == 1 ? 9 : 13;
    for (int t = 0; t < n; ++t, ++s) {
      std::vector<float> p = {64, 64, 64, 64, 64, 64, 64, 64, (float)lo(e, 3), (float)lo(t, n), 100};
      send(report(kMd | 4, 60, 127, p, md::kLayoutTag));
      CHECK(v[4].md && v[4].live && md::voice_set(4) == s);
      double r = rms_of(4, 200);   // 50 ms
      float o[kBlock];
      auto t0 = std::chrono::steady_clock::now();
      for (int b = 0; b < 4000; ++b) md::render(4, o, kBlock);   // 1 s more of it
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 4000;
      bool finite = true;
      for (int i = 0; i < 200 * kBlock; ++i) finite = finite && ring[i][4] > -32768 && ring[i][4] < 32767;
      printf("  %-7s rms %6.0f  %.2f us a block\n", md::set_name(s), r, us);
      CHECK(r > 30 && finite);
      if (us > worst_us) worst_us = us, worst = s;
      for (int vi = 0; vi < kVoices; ++vi)
        if (vi != 4) CHECK(ring[0][vi] == 0 && ring[100][vi] == 0);
    }
  }
  // a block is 250 us of audio: one voice of MD's worst TYPE as % of a Mac core
  printf("  worst: %s %.2f us a 12-sample block = %.2f %% of a core per voice (this Mac)\n", md::set_name(worst), worst_us,
         worst_us / 250.0 * 100);
  // a TYPE change on the control core (a report, then the hit): its cost
  double change_us = 0;
  for (int k = 0; k < 60; ++k) {
    int e = k % 3, n = e == 0 ? 8 : e == 1 ? 9 : 13;
    auto m = report(kMd | 4, 60, 100, {64, 64, 64, 64, 64, 64, 64, 64, (float)lo(e, 3), (float)lo(k % n, n), 100},
                    md::kLayoutTag);
    auto t0 = std::chrono::steady_clock::now();
    send(m);
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    change_us = us > change_us ? us : change_us;
    rms_of(4, 4);
  }
  printf("  a report that changes ENGINE / TYPE with its trig: at most %.1f us on the control core (budget 2000)\n",
         change_us);
  CHECK(change_us < 2000);
  md::Cost mc = md::cost();   // /status "md": the TYPE changes counted (each: params' set-up, the trig's tail + set-up)
  printf("  md::cost: %u changes, at most %u us (other calls %u us)\n", (unsigned)mc.changes, (unsigned)mc.change_max_us,
         (unsigned)mc.call_max_us);
  CHECK(mc.changes >= 60 && mc.change_max_us < 2000);   // (its most: also the first, cold set-ups above)
  char js[200];
  CHECK(md::json(js, sizeof js) > 0 && strstr(js, "\"md\":{\"ok\":true,\"stateBytes\":") && strstr(js, "changeMaxUs"));
  // DIGIDRUM's page (no tag) on voice 2: EFM-SD's zone -> FM SD, and it sounds
  send(report(kMd | 2, 60, 100, {(float)lo(1, 11), 64, 64, 64, 64, 64, 64, 64, 30, 40, 50}, 0));
  CHECK(v[2].md && md::voice_set(2) == 1 && rms_of(2, 100) > 30);
  // its knobs move (a 03): the voice follows (zone 10: TRX CL)
  send(report(kMd | 2, -1, 0, {(float)lo(10, 11), 64, 64, 64, 64, 64, 64, 64}, -1));
  CHECK(md::voice_set(2) == 27);
  // the sequencer's note on an MD voice: mono, a hit
  seq::PolyCfg c = {};
  rms_of(2, 400), rms_of(2, 400), rms_of(2, 400);   // let it ring out
  CHECK(inst::seq_on(2, 48, 100, c) && inst::stat(2).path == 2 && rms_of(2, 100) > 30);
  inst::seq_off(2, -1);
  // the machine stats: MD's column
  for (int k = 0; k < kMachines; ++k) mach[k] = 0;
  send(report(kMd | 4, 60, 100, {64, 64, 64, 64, 64, 64, 64, 64, 0, 0, 100}, md::kLayoutTag));
  for (int b = 0; b < 100; ++b) render_voices(ring, kBlock * 400, b * kBlock, 0, kVoices, mach);
  CHECK(mach[kMMd] > 0);
}

static int u14(const std::vector<uint8_t>& m, int at) { return m[at] << 7 | m[at + 1]; }
static void test_labels() {
  auto ask = [](int slot, int gen, int what, int arg) {
    g_out.clear();
    uint8_t m[11] = {0xF0, 0x7D, 0x52, 0x54, 0x19, 0x01, (uint8_t)slot, (uint8_t)gen, (uint8_t)what, (uint8_t)arg, 0xF7};
    return md::on_ask(m, 11);
  };
  // ENGINE FM (slot 0x40), TYPE value 0: FM BD's set (generation 1): the head, then 4 parts of 4 knobs
  CHECK(ask(0x40, 0, 0, 0) && g_out.size() == 5);
  const auto& h = g_out[0];
  CHECK(h.size() == 43 && h[4] == 0x1A && h[5] == 1 && h[6] == 0x40 && h[7] == 1 && h[8] == 0 && h[9] == 1 && h[10] == 1 &&
        h[11] == 16 && h[12] == 5 && !memcmp(&h[13], "MD\0", 3) && !memcmp(&h[27], "MD ENGINE", 9) && u14(h, 40) == 0);   // fp 0: no fingerprint
  for (int part = 1; part <= 4; ++part) CHECK(g_out[part].size() == 106 && g_out[part][8] == part && g_out[part][6] == 0x40);
  const auto& p1 = g_out[1];   // P1: Pitch / PTCH, dial, its default
  CHECK(p1[9] == 1 && p1[10] == 0 && p1[14] == 127 && p1[15] == mdk_builtin_default(0, 0) && !memcmp(&p1[17], "Pitch", 5) &&
        !memcmp(&p1[29], "PTCH", 4));
  const auto& p3 = g_out[3];   // P9 ENGINE (list, 3 names), P10 TYPE (list, 8 for FM), P11 LEV (fader, 100), P12 unused
  CHECK(p3[9] == 9 && p3[10] == 3 && p3[11] == 2 && p3[16] == 3 && !memcmp(&p3[17], "Engine", 6));
  CHECK(p3[33] == 10 && p3[34] == 3 && p3[40] == 8 && p3[57] == 11 && p3[58] == 2 && p3[63] == 100 && p3[81] == 12 &&
        p3[82] == 0 && p3[97] == 0);
  // the same set again with its generation: unchanged
  CHECK(ask(0x40, 1, 0, 15) && g_out.size() == 1 && g_out[0].size() == 10 && g_out[0][8] == 0x7F);
  // FM CB (value 80): set 5, generation 6; P8 unused (FM CB has 7 controls)
  CHECK(ask(0x40, 1, 0, 80) && g_out.size() == 5 && g_out[0][7] == 6 && g_out[2][9 + 3 * 24] == 8 && g_out[2][17 + 3 * 24] == 0);
  // TRX (slot 0x42), value 99: TRX CL, set 27, generation 28; TYPE's enum count 13
  CHECK(ask(0x42, 0, 0, 99) && g_out[0][6] == 0x42 && g_out[0][7] == 28 && g_out[3][40] == 13);
  // the lists: ENGINE's 3 names; TRX's 13 TYPE names in two 1Bs (replace, then not)
  CHECK(ask(0x41, 0, 1, 8) && g_out.size() == 1 && g_out[0][4] == 0x1B && g_out[0][7] == 8 && g_out[0][9] == 1 &&
        g_out[0][10] == 3 && !memcmp(&g_out[0][12], "FM", 2) && !memcmp(&g_out[0][26], "PI", 2) && !memcmp(&g_out[0][40], "TRX", 3));
  CHECK(ask(0x42, 0, 1, 9) && g_out.size() == 2 && g_out[0][9] == 1 && g_out[0][10] == 13 && g_out[1][9] == 0 &&
        g_out[1][11] == 8 && !memcmp(&g_out[1][12], "CY", 2) && !memcmp(&g_out[0][12], "BD", 2));
  CHECK(g_out[0][8] == 18 && g_out[1][8] == 18);   // gen 0 asked: the ENGINE's first set's (TRX BD, set 17)
  CHECK(ask(0x42, 28, 1, 9) && g_out[0][8] == 28 && g_out[1][8] == 28);   // the page's 1A gen (TRX CL): echoed
  // not MD's: a user slot's 19 and a malformed one are left to the user slots
  CHECK(!ask(1, 0, 0, 0) && !ask(0x43, 0, 0, 0));
}

int main() {
  hooks.to_rytm = out;
  host_set_log_sink(sink);
  voices_init();
  printf("  MD state: %u B a voice\n", (unsigned)md::state_bytes());
  test_legacy_map();
  test_play();
  test_labels();
  printf("test_md: PASS\n");
  return 0;
}
