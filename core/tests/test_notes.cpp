// core/tests/test_notes.cpp - MIDI channel 1..8 notes -> the mods' reports for voices 0..7 (sim/protocol.py's
// layout: 0x04 41 bytes, 0x03 39 bytes, 0x11 7 bytes), the machine per voice, CC 16..31 as P1..P16; then the
// adapter into the engine (as the boards wire it): channel 1's note 36 plays voice 0 on every machine but DSP
// SAMPLE (no card pool here) and leaves the other voices silent.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "engine.h"
#include "fx/send_fx.h"
#include "notes.h"

using core::NoteAdapter;

static std::vector<std::vector<uint8_t>> out;
static void on(const uint8_t* d, int n) { out.emplace_back(d, d + n); }

int main() {
  NoteAdapter a(on);
  CHECK(a.Machine(0) == 0x00 && !strcmp(NoteAdapter::Name(0x48), "fm"));
  CHECK(a.SetMachine(1, "juno") && !a.SetMachine(1, "nope") && a.Machine(1) == 0x08);
  a.Channel(0x91, 60, 100);                 // channel 2: voice 1
  CHECK(out.size() == 1 && out[0].size() == 41 && out[0][4] == 0x04 && out[0][5] == (0x08 | 1));
  CHECK(out[0][6] == 60 && out[0][7] == 100 && out[0][8] == 0 && out[0][22] == 127 && out[0][40] == 0xF7);   // PAT 0, LEN 127
  a.Channel(0xB1, 17, 33);                  // CC 17 = P2 (CUT)
  CHECK(out.size() == 2 && out[1].size() == 39 && out[1][4] == 0x03 && out[1][8] == 33 && out[1][9] == 0);
  a.Channel(0x91, 60, 0);                   // note-on velocity 0: off
  CHECK(out.size() == 3 && out[2] == (std::vector<uint8_t>{0xF0, 0x7D, 0x52, 0x54, 0x11, 1, 0xF7}));
  a.Channel(0x81, 60, 64);                  // nothing sounding: no second note-off
  CHECK(out.size() == 3);
  a.Channel(0x91, 60, 100);                 // A on, B on, A off: B keeps playing
  a.Channel(0x91, 62, 100);
  a.Channel(0x81, 60, 64);
  CHECK(out.size() == 5 && out[4][6] == 62);
  a.Channel(0x91, 62, 0);                   // B off (velocity 0): the voice closes
  CHECK(out.size() == 6 && out[5] == out[2]);
  a.Channel(0x99, 60, 100);                 // channel 10: no voice
  a.Channel(0xE0, 0, 64);                   // pitch bend: ignored
  CHECK(out.size() == 6);
  CHECK(a.SetMachine(0, "dsps"));
  a.Channel(0x90, 36, 90);
  CHECK(out[6][5] == 0x20 && out[6][8 + 2 * 10] == 1);   // DSP SAMPLE: SMP (P11) = pool slot 1
  CHECK(a.SetMachine(0, "grain") && a.Machine(0) == 0x20 && !strcmp(NoteAdapter::Name(0x20), "dsps"));
  a.Channel(0x90, 37, 90);
  CHECK(out[7][5] == 0x20 && out[7][8 + 2 * 10] == 1 && out[7][8 + 2 * 11] == 56);   // GRAIN: MOD (P12) 56
  CHECK(!strcmp(NoteAdapter::Names(), "plaits juno daisy dsps grain fm md wt gr livein chop") && !a.SetMachine(0, "dd"));   // (DIGIDRUM: removed)
  {   // CC 123 closes the sounding note; CC 120 on a voice channel silences it (no report), on any channel clears the FX
    size_t n0 = out.size();
    a.Channel(0xB3, 123, 0);                // channel 4, nothing sounding: no note-off
    CHECK(out.size() == n0);
    a.Channel(0x91, 62, 100);
    a.Channel(0xB1, 123, 0);
    CHECK(out.size() == n0 + 2 && out[n0 + 1][4] == 0x11 && out[n0 + 1][5] == 1);
    a.Channel(0xB1, 123, 0);
    CHECK(out.size() == n0 + 2);
    uint8_t k0 = core::fx::control.kill;
    a.Channel(0xBD, 120, 0);                // channel 14 (no voice): the FX only
    CHECK((uint8_t)(core::fx::control.kill - k0) <= 1);
  }
  CHECK(!a.SetMachine(8, "plaits") && !a.SetMachine(-1, "plaits") && a.Machine(0) == 0x20);

  // Into the engine: each machine on voice 0, channel 1 note 36 (a stock Rytm's BD track), then its note-off.
  char tmpl[] = "/tmp/core-notes-XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples"));
  core::hooks.to_rytm = [](const uint8_t*, int) {};
  CHECK(core::engine_begin());
  NoteAdapter e(core::engine_sysex);
  static int16_t ring[core::kBlock * 200][core::kVoices];
  const char* sounding[] = {"plaits", "juno", "daisy", "md", "fm", "wt", "gr"};
  for (const char* m : sounding) {
    CHECK(e.SetMachine(0, m));
    e.Channel(0x90, 36, 100);
    while (core::idle_prepare()) {}
    uint64_t mach[core::kMachines] = {0};
    for (int b = 0; b < 200; ++b) {
      core::idle_stream();
      core::render_voices(ring, core::kBlock * 200, b * core::kBlock, 0, core::kVoices, mach);
    }
    double s0 = 0, rest = 0;
    for (int i = 0; i < core::kBlock * 200; ++i) {
      s0 += (double)ring[i][0] * ring[i][0];
      for (int v = 1; v < core::kVoices; ++v) rest += std::abs(ring[i][v]);
    }
    double rms = std::sqrt(s0 / (core::kBlock * 200));
    printf("  %s: voice 0 rms %.0f\n", m, rms);
    CHECK(rms > 100 && rest == 0);
    // CC 120 on the voice's channel: silent within 5 ms (+ a block), the ramp without a step beyond the signal's own
    e.Channel(0xB0, 120, 0);
    int last = 0, maxstep = 0, natural = 0, pk = 0;   // natural: the voice's own largest step in the last 20 ms before
    for (int i = core::kBlock * 160; i < core::kBlock * 200; ++i)
      natural = std::max(natural, std::abs(ring[i][0] - ring[i - 1][0])), pk = std::max(pk, std::abs((int)ring[i][0]));
    for (int b = 0; b < 40; ++b) {
      core::render_voices(ring, core::kBlock * 200, b * core::kBlock, 0, core::kVoices, mach);
      for (int i = b * core::kBlock; i < (b + 1) * core::kBlock; ++i) {
        if (b >= 21) CHECK(ring[i][0] == 0);
        if (b > 0 && b < 21) maxstep = std::max(maxstep, std::abs(ring[i][0] - last));
        last = ring[i][0];
      }
    }
    printf("  %s: silenced, max step in the ramp %d (own %d)\n", m, maxstep, natural);
    CHECK(maxstep <= natural + pk / 240 + 1);
    e.Channel(0x80, 36, 0);
  }
  printf("test_notes: PASS\n");
  return 0;
}
