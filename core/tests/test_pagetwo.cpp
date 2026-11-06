// core/tests/test_pagetwo.cpp - a machine chosen in the Rytm's machine list starts with its page 2's (P9..P16) defaults.
//
// Hardware report (0.4.1 candidate): a DSP SAMPLE voice sometimes made no sound; each time its TUN was off 0, its STA
// off 0 or its END not at the end. Stock's set_machine writes the chosen machine's P1..P8 defaults and leaves P9..P16
// (the stock sample words 17..24) as the sound had them [measured in rytm-emu gate040 (OS 1.73) and o174 (OS 1.74): BD
// LIVE IN -> DSP SAMPLE kept LIVE IN's page 2, 77 0 0 0 0 0 0 0: TUN +13, END 0, LEV 0; fresh from the base: 64 64 0 0 0
// 120 0 100]. The Pi now writes page 2's defaults (48) after the list closes (2B: MachineListView off the top) and the
// choice has settled, for a track the list moved to another machine, in two groups of 4 (0026's 48 queue is 8 deep);
// a kit / sound load, NO, or a choice of the machine it had keeps the sound's words.
//
// The messages in the order the emulator showed them (both OSes, LIVE IN -> list -> UP x3 -> YES / NO): 2B list on top,
// 33 GRANULAR, 29, 33 WAVETABLE, 29, 33 DSP SAMPLE, 29, [NO: 33 LIVE IN, 29,] 2B the SRC page. No snapshot 33 and no
// other view over the list in either.
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "engine.h"
#include "machines.h"
#include "platform.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint32_t g_ms = 10000;

static void from_rytm(uint8_t cmd, std::vector<uint8_t> b) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), b.begin(), b.end());
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void machine(int t, int rytm_machine, int vb) {   // 33 for one track, then the SRC view's 29 (as measured)
  from_rytm(0x33, {(uint8_t)t, 1, (uint8_t)rytm_machine, (uint8_t)vb});
  from_rytm(0x29, {(uint8_t)t, 1});
}
static void top(uint32_t vt) {   // 2B: the Rytm's top view (state, views, vt in five 7-bit groups, age)
  from_rytm(0x2B, {0x0B, 0x0A, (uint8_t)(vt >> 28 & 0x7F), (uint8_t)(vt >> 21 & 0x7F), (uint8_t)(vt >> 14 & 0x7F),
                   (uint8_t)(vt >> 7 & 0x7F), (uint8_t)(vt & 0x7F), 0});
}
// MachineListView and the SRC page's view, measured: OS 1.73 (gate040) and OS 1.74 (o174)
static uint32_t g_list = 0x401E5BF4u, g_src = 0x40234CFCu;
static void os174(bool on) { g_list = on ? 0x401E5EA4u : 0x401E5BF4u, g_src = on ? 0x4023502Cu : 0x40234CFCu; }
static void list_open() { top(g_list); }
static void list_close() { top(g_src); }
static void tick(int ms) { g_ms += (uint32_t)ms, ui::remote_tick(g_ms); }
static int writes() {
  int n = 0;
  for (auto& m : g_out) n += m.size() >= 10 && m[4] == 0x48;
  return n;
}
static void answer_all() {   // the Rytm's 48 answers (written) for every write so far
  std::vector<std::vector<uint8_t>> w;
  for (auto& m : g_out)
    if (m.size() >= 10 && m[4] == 0x48) w.push_back(m);
  for (auto& m : w) from_rytm(0x48, {m[6], m[7], 0});
}
static void settle() {   // the wait, both groups, their answers
  tick(200), tick(100), answer_all(), tick(100);
}
// 48 <track> <param> <value>: our writes for track t, P9..P16 (-1 not written)
static std::vector<int> page_two(int t) {
  std::vector<int> w(8, -1);
  for (auto& m : g_out)
    if (m.size() >= 10 && m[4] == 0x48 && m[6] == t && m[7] >= 8 && m[7] < 16) w[m[7] - 8] = m[8];
  return w;
}
static bool none(int t) { return page_two(t) == std::vector<int>(8, -1); }
// a 0x04 (note 60) for voice vi on DSP SAMPLE with these P9..P16 (P1..P8 its SRC defaults, MIX 0)
static void dsps_trig(int vi, const std::vector<int>& p2) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x04, (uint8_t)(kDspSample | vi), 60, 100};
  const int p1[8] = {80, 64, 64, 40, 0, 0, 64, 100};
  for (int k = 0; k < 8; ++k) m.push_back((uint8_t)p1[k]), m.push_back(0);
  for (int k = 0; k < 8; ++k) m.push_back((uint8_t)p2[k]), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void snapshot(int t0_machine, int t0_vb) {   // 33 of all 12 tracks: track 0 as given, the rest analog
  std::vector<uint8_t> b = {0, 12};
  for (int t = 0; t < 12; ++t) b.push_back((uint8_t)(t == 0 ? t0_machine : t)), b.push_back((uint8_t)(t == 0 ? t0_vb : 0x7F));
  from_rytm(0x33, b);
}

int main() {
  host_fake_time(true, 1000000);
  hooks.to_rytm = out;
  engine_begin();
  ui::remote_tick(g_ms);
  const std::vector<int> kDspsP2 = {64, 64, 0, 0, 0, 120, 0, 100};   // TUN FIN SMP MOD STA END LOP LEV
  const std::vector<int> kLiveP2 = {77, 0, 0, 0, 0, 0, 0, 0};          // what LIVE IN left (its own page-2 defaults)

  for (int os = 0; os < 2; ++os) {
    os174(os == 1);
    snapshot(48, kLiveMachine);   // BD (track 0) LIVE IN, the rest analog
    // ---- the owner's case: BD from LIVE IN to DSP SAMPLE (YES); before the fix the words stayed LIVE IN's: silent
    g_out.clear();
    list_open();
    machine(0, 47, kGranMachine);
    machine(0, 46, kWaveMachine);
    machine(0, 45, kDspSample);
    CHECK(none(0));   // nothing while the list is open (its cursor's previews)
    list_close();
    tick(50);
    CHECK(writes() == 0);   // the choice settles first (>= 2 UI ticks)
    tick(150);
    CHECK(writes() == 4 && page_two(0) == std::vector<int>({64, 64, 0, 0, -1, -1, -1, -1}));   // the first group of 4
    tick(20);
    CHECK(writes() == 4);   // the second a UI tick later (0026 queues 8)
    tick(50);
    CHECK(writes() == 8 && page_two(0) == kDspsP2);
    answer_all();
    tick(2000);
    CHECK(writes() == 8);   // all answered: nothing sent again
    // the Rytm's answer comes as the next report: the voice plays TUN 0, STA 0, END the end, LEV 100
    dsps_trig(0, kDspsP2);
    CHECK(v[0].dsps && v[0].p[8] == 64 && v[0].p[12] == 0 && v[0].p[13] == 120 && v[0].p[15] == 100);
    dsps_trig(0, kLiveP2);   // (what the voice got before the fix: LIVE IN's words)
    CHECK(v[0].p[13] == 0 && v[0].p[15] == 0);

    // ---- NO on the DSP SAMPLE preview: the restoring 33 before the close (measured): LIVE IN's words stay
    snapshot(48, kLiveMachine);
    g_out.clear();
    list_open();
    machine(0, 47, kGranMachine);
    machine(0, 46, kWaveMachine);
    machine(0, 45, kDspSample);
    machine(0, 48, kLiveMachine);
    list_close();
    settle();
    CHECK(none(0));
    // ... and a restoring 33 later than the close (not measured; the wait re-checks): nothing either
    list_open();
    machine(0, 45, kDspSample);
    list_close();
    tick(60);
    machine(0, 48, kLiveMachine);
    settle();
    CHECK(none(0));
    // no 33 while the list was open, one after: no list choice
    list_open();
    list_close();
    machine(0, 45, kDspSample);
    settle();
    CHECK(none(0));
    // YES's set_machine landing after the close (within the wait), the cursor having moved while open: written
    snapshot(48, kLiveMachine);
    g_out.clear();
    list_open();
    machine(0, 46, kWaveMachine);
    list_close();
    tick(60);
    machine(0, 45, kDspSample);
    settle();
    CHECK(page_two(0) == kDspsP2);
    // the list opened again within the settle wait, the cursor on another machine at the wait's end: nothing for the
    // preview; that list's YES writes the final choice's defaults, its NO the first choice's
    snapshot(48, kLiveMachine);
    g_out.clear();
    list_open();
    machine(0, 45, kDspSample);
    list_close();
    tick(60);
    list_open();
    machine(0, 46, kWaveMachine);   // (a preview)
    tick(200), tick(100);
    CHECK(writes() == 0);
    machine(0, 47, kGranMachine);
    list_close();
    settle();
    CHECK(page_two(0) == std::vector<int>({10, 60, 0, 0, 0, 0, 0, 0}));
    snapshot(48, kLiveMachine);
    g_out.clear();
    list_open();
    machine(0, 45, kDspSample);
    list_close();
    tick(60);
    list_open();
    machine(0, 46, kWaveMachine);
    machine(0, 45, kDspSample);   // NO: back to DSP SAMPLE, the first list's choice
    list_close();
    settle();
    CHECK(page_two(0) == kDspsP2);
    // the track moved on between the groups: the rest not sent
    snapshot(48, kLiveMachine);
    g_out.clear();
    list_open();
    machine(0, 45, kDspSample);
    list_close();
    tick(200);
    CHECK(writes() == 4);
    machine(0, 44, kPlaits);
    tick(100);
    CHECK(writes() == 4);
  }
  os174(false);

  // ---- from an analog machine (its sample layer's words are no DSP SAMPLE settings): SD to DSP SAMPLE
  g_out.clear();
  list_open();
  machine(1, 45, kDspSample);
  list_close();
  settle();
  CHECK(page_two(1) == kDspsP2 && none(0));
  list_close();   // a second 2B off the list (once a second on the SRC page): nothing again
  settle();
  CHECK(writes() == 8);

  // ---- writes the Rytm never answered (its 48 queue full): sent once more after a second, in groups, then given up
  g_out.clear();
  list_open();
  machine(2, 45, kDspSample);
  list_close();
  tick(200), tick(100);
  CHECK(writes() == 8);
  from_rytm(0x48, {2, 8, 0}), from_rytm(0x48, {2, 9, 0});   // only P9, P10 answered
  tick(1000);
  CHECK(writes() == 10);   // P11, P12 again (the first group's rest)
  tick(100);
  CHECK(writes() == 14);   // P13..P16
  tick(1100), tick(1100), tick(1100);
  CHECK(writes() == 14);   // no third try

  // ---- the cursor back to the machine it had: the sound's words stay
  g_out.clear();
  list_open();
  machine(1, 0, 0x7F);   // (passing an analog machine)
  machine(1, 45, kDspSample);
  list_close();
  settle();
  CHECK(none(1));

  // ---- a kit load: one track's 33 with no list open, a snapshot while it is open, a kit change around a choice
  g_out.clear();
  machine(3, 45, kDspSample);
  list_close();
  settle();
  CHECK(none(3));
  list_open();
  {
    std::vector<uint8_t> b = {0, 12};
    for (int t = 0; t < 12; ++t) b.push_back((uint8_t)(t == 3 ? 44 : t)), b.push_back(t == 3 ? 0 : 0x7F);
    from_rytm(0x33, b);
  }
  list_close();
  settle();
  CHECK(none(3));
  list_open();
  machine(4, 45, kDspSample);
  from_rytm(0x3B, {5});
  list_close();
  settle();
  CHECK(none(4));
  list_open();
  machine(4, 44, kPlaits);
  list_close();
  tick(60);
  from_rytm(0x3B, {6});   // a pattern change to another kit within the wait
  settle();
  CHECK(none(4));

  // ---- the other machines: PLAITS, WAVETABLE (its records' defaults), 6-OP FM's E..H only; LIVE IN / CHOP / MIDI /
  // MD / the user slots are the Rytm's own (0026 cu_defs_tick, 0021): none from the Pi
  g_out.clear();
  list_open();
  machine(5, 44, kPlaits);
  list_close();
  settle();
  CHECK(page_two(5) == kDspsP2);
  list_open();
  machine(6, 46, kWaveMachine);
  list_close();
  settle();
  CHECK(page_two(6) == std::vector<int>({0, 64, 100, 64, 0, 0, 0, 0}));
  list_open();
  machine(7, 40, kFm);
  list_close();
  settle();
  CHECK(page_two(7) == std::vector<int>({-1, -1, -1, -1, 0, 0, 0, 0}));
  list_open();
  machine(8, 48, kLiveMachine);
  list_close();
  settle();
  CHECK(none(8));
  list_open();
  machine(9, 35, 0x50);   // U1
  list_close();
  settle();
  CHECK(none(9));

  printf("test_pagetwo: PASS\n");
  return 0;
}
