// core/tests/test_host_link.cpp - host_sysex on whole messages (sim/protocol.py's): 10 sets the project and,
// without slot hooks, a 1D; 19 / 1E answered as empty slots; 0x04 / 0x11 reach the voices; others ignored.
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include <cmath>

#include "check.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "rec/rec_names.h"

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static std::vector<uint8_t> msg(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static void send(const std::vector<uint8_t>& m) { core::host_sysex(m.data(), (int)m.size()); }
static int count(uint8_t cmd) {
  int n = 0;
  for (auto& m : g_out) n += m[4] == cmd;
  return n;
}

static int16_t ring[core::kBlock * 400][core::kVoices];
static double rms(int voice, int frames, int from = 0) {   // frames from ring row from
  double s = 0;
  for (int i = from; i < from + frames; ++i) s += (double)ring[i][voice] * ring[i][voice];
  return std::sqrt(s / frames);
}
static void render(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) core::render_voices(ring, core::kBlock * 400, b * core::kBlock, 0, core::kVoices, mach);
}

int main() {
  using namespace core;
  char tmpl[] = "/tmp/core-link-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples"));
  hooks.to_rytm = out;
  card_ok = cards.begin(log, shared, sizeof shared);
  CHECK(card_ok);
  junolib.begin(&cards.sd(), log);
  dxlib.begin(&cards.sd(), log);
  voices_init();
  send(msg({0x10, 'A', 'B'}));
  CHECK(!strcmp(prj().key, "AB") && count(0x14) == 1 && count(0x16) == 1 && count(0x17) >= 1 && count(0x1d) == 1);
  g_out.clear();
  send(msg({0x10, 'A', 'B'}));   // the Rytm's repeat (every few seconds while it plays): no tables again
  CHECK(!strcmp(prj().key, "AB") && count(0x14) == 0 && count(0x16) == 0);
  send(msg({0x10, 'C', 'D'}));   // another project: its pool, the tables again
  CHECK(!strcmp(prj().key, "CD") && count(0x14) == 1 && count(0x16) == 1);
  g_out.clear();
  prj().seen = false;          // a fresh start whose stored project is the Rytm's: the first 10 still answers
  send(msg({0x10, 'C', 'D'}));
  CHECK(!strcmp(prj().key, "CD") && count(0x14) == 1 && count(0x16) == 1);
  g_out.clear();
  send(msg({0x19, 1, 2, 0, 0, 0}));   // ask slot 2's metadata at generation 0: EMPTY
  CHECK(g_out.size() == 1 && g_out[0].size() == 43 && g_out[0][4] == 0x1a && g_out[0][6] == 2);
  send(msg({0x19, 1, 2, 1, 0, 0}));   // at generation 1: unchanged
  CHECK(g_out.size() == 2 && g_out[1].size() == 10);
  send(msg({0x1e, 1, 3, 0, 0, 1}));   // assign U3: NOT INSTALLED
  CHECK(g_out.size() == 3 && g_out[2].size() == 15 && g_out[2][12] == 3 && g_out[2][13] == 1);
  send(msg({0x19, 1, 0, 0, 3, 0}));   // the slot menu's YES asks the installed list: empty (1B 0x21), maybe a 1D first
  CHECK(g_out.back().size() == 12 && g_out.back()[4] == 0x1b && g_out.back()[7] == 0x21 && g_out.back()[10] == 0);
  CHECK(!slots_1d(5000));             // the platform loop's 5 s 1D (pi bridge::tick): not again so soon
  g_out.clear();
  std::vector<uint8_t> t = {0x04, 0x00, 48, 100};
  for (float p : {124.f, 64.f, 64.f, 64.f, 64.f, 64.f, 64.f, 100.f, 64.f, 64.f, 0.f, 0.f, 0.f, 120.f, 0.f, 100.f}) {
    t.push_back((uint8_t)p);
    t.push_back(0);
  }
  send(msg(t));
  CHECK(v[0].live);
  send(msg({0x11, 0}));   // a PLAITS voice ignores the note-off (its LPG decays on its own)
  CHECK(v[0].live);
  size_t before = g_out.size();
  send(msg({0x09, 0, 0}));   // the retired folder walk
  send(msg({0x7f, 0, 0}));   // unknown
  CHECK(g_out.size() == before);
  send(msg({0x0d, 0, 0}));   // listing of /Samples
  CHECK(g_out.size() == before + 1 && g_out.back()[4] == 0x0e);
  // ---- IN L/R source (26 / 27)
  CHECK(!fxin::wanted() && fxin::rytm() == -1 && (!strcmp(fxin::state(), "unknown") || !strcmp(fxin::state(), "unsupported")));   // (3 s unanswered: unsupported)
  CHECK(count(0x26) == 0);   // (the first message of the run asked, before the clear above)
  g_out.clear();
  send(msg({0x27, 0}));      // the Rytm reports off: nothing wanted, nothing sent
  CHECK(fxin::rytm() == 0 && !fxin::active() && g_out.empty() && !rec::fxin());
  char b[160];
  CHECK(fxin::command("fxin=1", b, sizeof b) && fxin::wanted() && g_out.size() == 1);
  CHECK(g_out[0] == msg({0x26, 1}));
  send(msg({0x27, 1}));      // confirmed
  CHECK(fxin::active() && !strcmp(fxin::state(), "on") && rec::fxin() && g_out.size() == 1);
  CHECK(!strcmp(rec::source(11).name, "FX_L") && !strcmp(rec::source(12).name, "FX_R") && !strcmp(rec::source(10).name, "CY-CB"));
  char fp[64];
  rec::file_path(fp, sizeof fp, "t_001", 12);
  CHECK(!strcmp(fp, "/Recordings/t_001/12_FX_R.wav"));
  uint32_t mk = 0;
  char err[40];
  CHECK(rec::parse_list("fx_l,in_r", &mk, err, sizeof err) && mk == (1u << 11 | 1u << 12));
  CHECK(fxin::json(b, sizeof b) > 0 && strstr(b, "\"want\":1") && strstr(b, "\"state\":\"on\""));
  send(msg({0x27, 0}));      // the Rytm's port came up again (unasked 00): the setting goes out again, once
  CHECK(!fxin::active() && !rec::fxin() && g_out.size() == 2 && g_out[1] == msg({0x26, 1}));
  CHECK(!strcmp(rec::source(11).name, "IN_L"));
  send(msg({0x27, 0}));      // its answer to that, if it refused: not sent again (no loop)
  CHECK(g_out.size() == 2);
  CHECK(fxin::command("fxin=0", b, sizeof b) && !fxin::wanted() && g_out.back() == msg({0x26, 0}));
  CHECK(fxin::command("fxin=?", b, sizeof b) && g_out.back() == msg({0x26, 0x7F}));
  CHECK(!fxin::command("fx", b, sizeof b) && fxin::command("fxin=x", b, sizeof b) && strstr(b, "not"));
  // persistence: the setting kept on the card, read back
  fxin::set(true);
  CHECK(fxin::save() && !fxin::save());
  fxin::set(false);
  fxin::load();
  CHECK(fxin::wanted());
  fxin::set(false);
  CHECK(fxin::save());
  fxin::load();
  CHECK(!fxin::wanted());
  // ---- a (re)connect: the Rytm's hello (35) silences every voice and clears the send FX (a note-off or STOP lost
  // across the gap must not leave a voice sounding; 2026-10-19, a constant tone after reconnects)
  std::vector<uint8_t> j = {0x04, (uint8_t)(kJuno | 1), 60, 100};
  for (int p : {0, 80, 20, 64, 10, 64, 64, 127, 0, 0, 0, 0, 0, 0, 0, 0}) { j.push_back((uint8_t)p); j.push_back(0); }
  send(msg(j));
  render(400);
  CHECK(v[1].juno && rms(1, 4800) > 300);   // held: no note-off comes
  usleep(150000);                           // past all_sound_off's 100 ms window
  uint8_t kill = fx::control.kill;
  send(msg({0x35, 0, 100, 1}));             // hello: up 5 s, caps 1
  CHECK(fx::control.kill == (uint8_t)(kill + 1));
  render(400);
  CHECK(rms(1, 2400, 2400) == 0);           // past the 5 ms ramp: silent until its next trig
  send(msg(j));
  render(400);
  CHECK(rms(1, 4800) > 300);                // a trig brings it back
  link_reset("test");                       // the platform's edge (USB MIDI gone / attached)
  render(400);
  CHECK(rms(1, 2400, 2400) == 0);
  printf("test_host_link: PASS\n");
  return 0;
}
