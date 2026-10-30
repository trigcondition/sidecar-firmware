// core/tests/test_traffic.cpp - what the host sends the Rytm (to_rytm) per second while it plays: idle, SONG RECORD
// armed and recording, with the Rytm's remote SONG page closed and open, and after STOP. The Rytm's side is a stream as
// a rui1036 Rytm sends it at 120 BPM: BD's 2C ticks (the clock mask), 2D 09 once a second, an NRPN knob turned 25
// times a second on SD's channel with its 49 3 echo, 2D 05 mute changes. The Pi's control loop (bridge::tick) is run
// every millisecond: the remote UI, the LFOs, poly, seq, song, the card jobs. Prints the rates (docs/SONG.md
// "Traffic to the Rytm") and checks the bounds: recording must not cost the Rytm more messages than playing; the
// touchscreen's PAT / VOICE pages' pattern dumps paced while the pattern is edited (6.).
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "lfo.h"
#include "poly.h"
#include "seq/rytm_dump.h"
#include "seq/seq.h"
#include "song/song.h"
#include "ui/remote.h"

using namespace core;

namespace core {
void host_set_card(const char* dir);
void host_fake_time(bool on, uint64_t us);
}

namespace {

uint64_t g_us = 1000000;
int64_t g_frame = 0;
int64_t frames() { return g_frame; }

struct Count {
  std::map<int, int> msgs;   // per command (the 7D 52 54 cmd; 0x100 + id a stock dump request; 0x200 + status channel)
  int total = 0, bytes = 0;
};
Count g_c;
std::vector<std::vector<uint8_t>> g_answers;   // the fake Rytm's dump answers, fed back on the next millisecond
bool g_answer = true;                          // false: a Rytm that leaves every request unanswered
std::vector<uint32_t> g_req_ms;                // when each dump request went

std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
std::vector<uint8_t> blk4(int64_t frame) {
  uint32_t b = (uint32_t)(frame / 32) & ((1u << 28) - 1);
  return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)};
}
std::vector<uint8_t> dump(uint8_t id, int n) {
  std::vector<uint8_t> m = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, id, 0x01, 0x01, 0x00};
  for (int i = 0; i < n; ++i) m.push_back((uint8_t)((i * 7 + id) & 0x7F));
  uint32_t sum = 0;
  for (size_t i = 10; i < m.size(); ++i) sum += m[i];
  int len = (int)m.size() - 5;
  m.push_back((uint8_t)(sum >> 7 & 0x7F)), m.push_back((uint8_t)(sum & 0x7F));
  m.push_back((uint8_t)(len >> 7 & 0x7F)), m.push_back((uint8_t)(len & 0x7F));
  m.push_back(0xF7);
  return m;
}

void to_rytm_count(const uint8_t* m, int n) {
  g_c.total++, g_c.bytes += n;
  if (n == 15 && m[0] == 0xF0 && m[1] == 0 && m[2] == 0x20) {   // a stock dump request: answered
    g_c.msgs[0x100 | m[6]]++;
    g_req_ms.push_back(now_ms());
    if (!g_answer) return;
    uint8_t id = m[6];
    int sz = id == 0x68 ? 2988 : id == 0x6A ? 14978 : id == 0x6C ? 2738 : id == 0x6D ? 97 : id == 0x64 ? 14973 : 190;
    g_answers.push_back(dump((uint8_t)(id - 0x10), sz));
    return;
  }
  if (m[0] == 0xF0 && n > 5 && m[1] == 0x7D) g_c.msgs[m[4]]++;
  else if (m[0] >= 0x80 && m[0] < 0xF0) g_c.msgs[0x200 | (m[0] & 0xF0)]++;
  else g_c.msgs[0x300]++;   // a piece of a long SysEx, other
}

// a whole message from the Rytm as the Pi takes it: the byte tap (rytm_dump, song) and, a SysEx, the engine
void from_rytm(const std::vector<uint8_t>& m) {
  for (uint8_t b : m) rytm_dump::byte(b), song::byte(b);
  if (m[0] == 0xF0 && m.size() > 5 && m[1] == 0x7D) engine_sysex(m.data(), (int)m.size());
}

// one millisecond of the Pi's control loop (bridge::tick + the card jobs)
void ms_step() {
  g_us += 1000, g_frame += 48;
  host_fake_time(true, g_us);
  std::vector<std::vector<uint8_t>> a;
  a.swap(g_answers);
  for (auto& m : a) from_rytm(m);
  uint32_t ms = now_ms();
  ui::remote_tick(ms);
  lfo::tick(now_us());
  poly::tick(ms);
  seq::tick(ms);
  song::tick(ms);
  for (int k = 0; k < 8 && idle_save(); ++k) {}
}

// The Rytm playing at 120 BPM for `secs`: a 1x step is 6000 frames (125 ms); BD's 2C when its clock is asked (song
// armed / recording) - sent anyway here, a Rytm with a seq track sends it too; the knob 25 times a second (an NRPN of 4
// CCs on SD's channel 2, ENCODER DEST INT+EXT) and rui1036b's 49 3 for it (the selected track SD's P1..P8 with labels)
// at the Rytm's 20 Hz UI tick; 2D 09 POS every second; a mute toggled every 2 s.
int g_step = 0;
uint8_t g_knob = 0;
// the pattern edited (core/seq/rytm_dump.h): 2D 0B DIRTY every 200 ms (the mod's most: every 4 UI ticks) while a
// gesture lasts - g_edit 1: knob gestures, 0.6 s of edits (4 DIRTYs) and 0.4 s still, every second (a live-recorded lock,
// trig keys); 2: without a pause. g_switch: a run of 3 pattern changes 100 ms apart every 4 s.
int g_edit = 0, g_switch = 0, g_pat = 0;
void edits(int i) {
  if (g_edit && i % 200 == 0 && (g_edit == 2 || i % 1000 < 800)) from_rytm(msg(0x2D, {11, 2}));
  if (g_switch && i % 4000 < 300 && i % 100 == 0) g_pat = (g_pat + 1) & 15, from_rytm(msg(0x2D, {2, (uint8_t)g_pat, 0x7F}));
}
void play(double secs) {
  int n = (int)(secs * 1000);
  for (int i = 0; i < n; ++i) {
    edits(i);
    if (g_frame % 6000 < 48) {
      from_rytm(msg(0x2C, [] { auto v = blk4(g_frame); v.push_back(1); v.insert(v.end(), {0, (uint8_t)(g_step % 16), 16, 2}); return v; }()));
      ++g_step;
    }
    if (i % 40 == 0) {
      g_knob = (uint8_t)((g_knob + 1) & 0x7F);
      from_rytm({0xB1, 99, 1, 0xB1, 98, 2, 0xB1, 6, g_knob, 0xB1, 38, 0});
    }
    if (i % 50 == 0) {
      std::vector<uint8_t> b = {3, 1};
      for (int k = 0; k < 8; ++k) b.insert(b.end(), {(uint8_t)(k == 2 ? g_knob : 64), 'L', 'A', 'B', (uint8_t)('1' + k)});
      from_rytm(msg(0x49, b));
    }
    if (i % 1000 == 0) from_rytm(msg(0x2D, [] { std::vector<uint8_t> v = {9}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); v.insert(v.end(), {1, (uint8_t)(g_step & 0x7F), 0}); return v; }()));
    if (i % 2000 == 1000) from_rytm(msg(0x2D, {5, (uint8_t)((i / 2000) & 1 ? 4 : 0), 0}));
    ms_step();
  }
}
void stopped(double secs) {
  for (int i = 0; i < (int)(secs * 1000); ++i) edits(i), ms_step();
}

struct Rate {
  std::string name;
  double secs;
  Count c;
};
std::vector<Rate> g_rates;

const char* cmd_name(int k) {
  static char b[24];
  switch (k) {
    case 0x2E: return "2E seq mask";
    case 0x2F: return "2F page LEDs";
    case 0x34: return "34 trig LEDs";
    case 0x37: return "37 catch mask";
    case 0x38: return "38 rect";
    case 0x39: return "39 commit";
    case 0x3C: return "3C alive";
    case 0x3D: return "3D text";
    case 0x3E: return "3E text clear";
    case 0x3F: return "3F echo";
    case 0x47: return "47 names";
    case 0x168: return "dump req kit";
    case 0x16A: return "dump req pattern";
    case 0x16C: return "dump req settings";
    case 0x16D: return "dump req global";
    case 0x164: return "dump req PAT view";
    default: break;
  }
  if (k >= 0x200 && k < 0x300) snprintf(b, sizeof b, "channel %02X", k & 0xFF);
  else if (k >= 0x100 && k < 0x200) snprintf(b, sizeof b, "dump req %02X", k & 0xFF);
  else snprintf(b, sizeof b, "%02X", k);
  return b;
}

Rate measure(const char* name, double secs, void (*fn)(double)) {
  g_c = Count();
  fn(secs);
  Rate r = {name, secs, g_c};
  g_rates.push_back(r);
  printf("traffic %-34s %6.1f msg/s %7.0f B/s:", name, r.c.total / secs, r.c.bytes / secs);
  for (auto& kv : r.c.msgs) printf("  %s %.1f", cmd_name(kv.first), kv.second / secs);
  printf("\n");
  return r;
}

double per_s(const Rate& r, int k) {
  auto it = r.c.msgs.find(k);
  return it == r.c.msgs.end() ? 0 : it->second / r.secs;
}

std::string cmd(const char* c) {
  char b[400];
  CHECK(song::command(c, b, sizeof b));
  return b;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/core_traffic_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = to_rytm_count;
  host_fake_time(true, g_us);
  song::test_frames(frames);
  CHECK(engine_begin() && card_ok);
  from_rytm(msg(0x35, {0, 10, 1}));   // the hello (up 0.5 s: no splash)
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'T', 'R', 'A', 'F', 'F', 'I', 'C', 0xF7};
  engine_sysex(proj, sizeof proj);
  {
    std::vector<uint8_t> b = {0, 12};
    for (int t = 0; t < 12; ++t) b.push_back(t ? 0 : 21), b.push_back(0x7F);
    from_rytm(msg(0x33, b));   // every track analog
  }
  from_rytm(msg(0x49, {2, 1, 2, 'S', 'D', ' ', 'H', 'A', 'R', 'D', 0, 0, 0, 0, 0}));   // SD selected
  from_rytm(msg(0x2D, {7, 120 * 120 >> 14, (120 * 120 >> 7) & 127, 120 * 120 & 127}));   // TEMPO 120
  from_rytm(msg(0x2D, {2, 0, 0x7F}));   // pattern A01
  stopped(2);
  auto play_msg = [] { std::vector<uint8_t> v = {1, 1}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return msg(0x2D, v); };
  auto stop_msg = [] { std::vector<uint8_t> v = {1, 0}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return msg(0x2D, v); };

  // 1. the Rytm playing, nothing armed, the remote page closed
  from_rytm(play_msg());
  Rate idle = measure("playing, page closed", 10, play);
  from_rytm(stop_msg());
  stopped(1);
  // 2. armed (the start snapshot: four dump requests), then recording
  cmd("song=arm");
  Rate arm = measure("arming (snapshot), stopped", 3, stopped);
  CHECK(song::status().state == song::kArmed);
  from_rytm(play_msg());
  CHECK(song::status().state == song::kRec);
  Rate rec = measure("recording, page closed", 10, play);
  // 3. recording with the Rytm's remote SONG page open (TRK + YES, then the SONG page)
  const uint8_t open[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 1, 0, 0xF7};
  engine_sysex(open, sizeof open);
  ui::remote_show_page("SONG");
  play(0.5);
  CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "SONG"));
  Rate rec_open = measure("recording, SONG page open", 10, play);
  from_rytm(stop_msg());
  Rate after = measure("after STOP (saving), page open", 3, stopped);
  CHECK(song::status().state == song::kOff);
  from_rytm(play_msg());
  Rate idle_open = measure("playing, not armed, SONG page open", 10, play);
  from_rytm(stop_msg());
  stopped(1);

  // 4. a Rytm that does not answer the snapshot's requests: asked again with a growing wait, never a storm
  g_answer = false;
  g_req_ms.clear();
  cmd("song=arm");
  Rate deaf = measure("arming, the Rytm silent", 45, stopped);
  uint32_t gap = 0xFFFFFFFFu;
  for (size_t i = 1; i < g_req_ms.size(); ++i) gap = g_req_ms[i] - g_req_ms[i - 1] < gap ? g_req_ms[i] - g_req_ms[i - 1] : gap;
  printf("traffic: the silent Rytm got %d dump requests in 45 s, the closest %u ms apart\n", (int)g_req_ms.size(), (unsigned)gap);
  CHECK(g_req_ms.size() == 16 && gap >= 1500);   // 4 objects x 4 tries (1.5 s, then 3 s apart)
  cmd("song=disarm");
  g_answer = true;

  // the bounds: recording costs the Rytm no more than playing does (the clock mask's one 2E aside); the open SONG page
  // redraws at most what changes visibly (the record dot, the bar), never per recorded event
  CHECK(per_s(arm, 0x168) + per_s(arm, 0x16A) + per_s(arm, 0x16C) + per_s(arm, 0x16D) <= 4.0 / 3 + 0.01);   // once each
  CHECK(rec.c.total / rec.secs <= idle.c.total / idle.secs + 0.2);
  CHECK(rec_open.c.total / rec_open.secs <= idle_open.c.total / idle_open.secs + 2);
  CHECK(per_s(rec_open, 0x39) <= 1.5 && per_s(rec_open, 0x3D) <= 2);
  // 5. the page requests (49 1 <code>, the device's table ui/page_keys.h; rui1052's keys): T5 the touchscreen's INSERT
  // FX (a display request only); T9 SCOPE and T10 SIDECAR on the Rytm's own screen: the page asked open (3A 0 1) when
  // closed, then shown; switched when open
  {
    const uint8_t close[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 0, 0, 0xF7};
    engine_sysex(close, sizeof close);
    stopped(0.1);
    CHECK(!ui::remote_open());
    int pg = 0;
    uint32_t g0 = ui::remote_display_request(&pg);
    g_c = Count();
    from_rytm(msg(0x49, {1, 5}));
    CHECK(ui::remote_display_request(&pg) == g0 + 1 && pg == 5 && !g_c.msgs[0x3A] && !ui::remote_open());
    from_rytm(msg(0x49, {1, 9}));
    CHECK(ui::remote_display_request(&pg) == g0 + 2 && pg == 9 && g_c.msgs[0x3A] == 1);
    engine_sysex(open, sizeof open);   // the Rytm opens the page
    stopped(0.1);
    CHECK(ui::remote_open() && ui::remote_page_title() && !strcmp(ui::remote_page_title(), "SCOPE"));
    from_rytm(msg(0x49, {1, 10}));
    stopped(0.1);
    CHECK(ui::remote_open() && !ui::remote_page_title());   // SIDECAR: the status page (no code-drawn title)
    from_rytm(msg(0x49, {1, 9}));
    stopped(0.1);
    CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "SCOPE") && g_c.msgs[0x3A] == 1);
    // rui1046 / rui1052: SONG, PROJECT, POOL on the Rytm's screen (UP + T6, T12, T13); a touchscreen that has them
    // (the Pi with a display: voice_pi::OwnPages) takes them, the Rytm's page stays
    const uint8_t code[3] = {6, 12, 13};
    const char* const want[3] = {"SONG", "PROJECT", "POOL"};
    for (int k = 0; k < 3; ++k) {
      from_rytm(msg(0x49, {1, code[k]}));
      stopped(0.1);
      CHECK(ui::remote_display_request(&pg) == g0 + 5 + k && pg == code[k]);
      // SONG is asked to show (song=show); PROJECT / POOL only when they are in the list (here: no bundle feed, no
      // recorder: not, so SONG stays and a closed page would get the "NOT HERE NOW" popup)
      CHECK(ui::remote_page_title() && (!strcmp(ui::remote_page_title(), want[k]) || (k && !strcmp(ui::remote_page_title(), "SONG"))));
    }
    ui::remote_display_owns(1u << 6 | 1u << 12 | 1u << 13);
    from_rytm(msg(0x49, {1, 6}));
    stopped(0.1);
    CHECK(ui::remote_display_request(&pg) == g0 + 8 && pg == 6 && ui::remote_page_title());
    ui::remote_display_owns(0);
    from_rytm(msg(0x49, {1, 14}));   // past T13: ignored
    CHECK(ui::remote_display_request(&pg) == g0 + 8 && pg == 6);
    printf("display pages: UP + T5 the touchscreen's, T9 SCOPE / T10 SIDECAR / T6 T12 T13 SONG PROJECT POOL on the Rytm "
           "(opened, switched; a touchscreen's own) ok\n");
  }
  // 6. the touchscreen's PAT / VOICE page open (core/seq/rytm_dump.h): the pattern dump (14988 B a request) per minute
  // while the pattern is edited - playing, recording a take, stopped; the bounds: none while a take records (one after
  // STOP), at most one every 4 s while playing or editing stopped, a run of pattern changes one request
  {
    const int kReq = 0x164;
    auto per_min = [&](const Rate& r) { return per_s(r, kReq) * 60; };
    stopped(1);
    rytm_dump::set_page(rytm_dump::kPagePattern);
    stopped(1);   // the fetch on open
    CHECK(rytm_dump::stats().answers >= 1);
    g_edit = 1;
    from_rytm(play_msg());
    Rate pat_play = measure("PAT page, playing, edits", 60, play);
    from_rytm(stop_msg());
    g_edit = 2;
    Rate pat_cont = measure("PAT page, stopped, edits nonstop", 60, stopped);
    g_edit = 1;
    Rate pat_stop = measure("PAT page, stopped, edits", 60, stopped);
    g_edit = 0, g_switch = 1;
    Rate pat_sw = measure("PAT page, stopped, pattern runs", 60, stopped);
    g_switch = 0;
    stopped(2);
    cmd("song=arm");
    stopped(3);
    CHECK(song::status().state == song::kArmed);
    g_edit = 1;
    from_rytm(play_msg());
    CHECK(song::status().state == song::kRec);
    Rate pat_rec = measure("PAT page, recording, edits", 60, play);
    rytm_dump::set_page(rytm_dump::kPageVoice);
    Rate voice_rec = measure("VOICE page, recording, edits", 60, play);
    g_switch = 1;
    Rate voice_rec_sw = measure("VOICE page, recording, edits + runs", 60, play);
    rytm_dump::set_page(rytm_dump::kPagePattern);
    Rate pat_rec_sw = measure("PAT page, recording, edits + runs", 60, play);
    g_edit = 0, g_switch = 0;
    from_rytm(stop_msg());
    Rate pat_after = measure("PAT page, after the take's STOP", 3, stopped);
    CHECK(song::status().state == song::kOff);
    rytm_dump::set_page(rytm_dump::kPageVoice);
    g_edit = 1;
    from_rytm(play_msg());
    Rate voice_play = measure("VOICE page, playing, edits", 60, play);
    from_rytm(stop_msg());
    Rate voice_after = measure("VOICE page, stopped, edits", 60, stopped);
    g_edit = 0;
    stopped(2);
    rytm_dump::set_page(rytm_dump::kOff);
    printf("traffic: pattern dumps per minute: PAT playing %.0f, PAT stopped %.0f (nonstop %.0f), PAT pattern runs %.0f,"
           " PAT recording %.0f (+ runs %.0f), VOICE recording %.0f (+ runs %.0f), VOICE playing %.0f, VOICE stopped %.0f;"
           " after the take's STOP %d; timeouts %u bad %u\n",
           per_min(pat_play), per_min(pat_stop), per_min(pat_cont), per_min(pat_sw), per_min(pat_rec), per_min(pat_rec_sw),
           per_min(voice_rec), per_min(voice_rec_sw), per_min(voice_play), per_min(voice_after),
           pat_after.c.msgs[kReq], (unsigned)rytm_dump::stats().timeouts, (unsigned)rytm_dump::stats().bad);
    CHECK(per_min(pat_play) <= 16 && per_min(pat_stop) <= 16 && per_min(pat_cont) <= 16 && per_min(voice_after) <= 16);
    CHECK(per_min(pat_sw) == 15);                    // a run of three: one fetch
    CHECK(per_min(pat_rec) == 0 && per_min(voice_rec) == 0 && per_min(voice_rec_sw) == 0 && per_min(voice_play) == 0);
    CHECK(per_min(pat_rec_sw) == 15);                // recording on the PAT page: one per run of switches
    CHECK(pat_after.c.msgs[kReq] == 1);              // the take's edits: once after STOP
    CHECK(rytm_dump::stats().timeouts == 0 && rytm_dump::stats().bad == 0);
  }
  // 7. the Sidecar sequencer: BD a seq track with notes, GRID REC on it (seq mode, 2D 08): the host opens its SEQ page
  // on the Rytm; playing at 120 BPM (BD's 2C every step) the page's playhead, the trig LEDs (34) and the PAGE LEDs (2F)
  // move every step. Each commit (39) makes the Rytm redraw its whole stock screen under the page (rytm-mods 0026,
  // ev_tick), so the rate of commits is the Rytm's UI cost; the LEDs are cheap (the panel stream rewritten).
  {
    char b[200];
    const char* sc[] = {"seq=track:1:on", "seq=len:1:16", "seq=step:1:1:60:100", "seq=step:1:5:64:100",
                        "seq=step:1:9:67:100", "seq=step:1:13:72:100", "seq=step:1:13:76:100"};
    for (const char* c : sc) CHECK(seq::command(c, b, sizeof b));
    const uint8_t close0[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 0, 3, 0xF7};
    engine_sysex(close0, sizeof close0);   // (5. left the page open)
    stopped(0.5);
    from_rytm(msg(0x2D, {6, 0, 1}));   // GRID REC
    from_rytm(msg(0x2D, {8, 1, 0}));   // seq mode on BD: the host asks the page open (3A 0 1)
    stopped(0.1);
    engine_sysex(open, sizeof open);   // the Rytm opens it
    stopped(0.5);
    CHECK(ui::remote_open() && ui::remote_page_title() && !strcmp(ui::remote_page_title(), "SEQ"));
    Rate seq_still = measure("SEQ page open, stopped", 5, stopped);
    from_rytm(play_msg());
    Rate seq_open = measure("SEQ page open, playing", 10, play);
    const uint8_t close7[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 0, 3, 0xF7};
    engine_sysex(close7, sizeof close7);   // TRK + TRIG: the page closed, seq mode stays (the host's LEDs, 34 / 2F)
    play(0.2);
    CHECK(!ui::remote_open());
    Rate seq_closed = measure("seq mode, page closed, playing", 10, play);
    from_rytm(stop_msg());
    from_rytm(msg(0x2D, {8, 0, 0x7F}));
    from_rytm(msg(0x2D, {6, 0, 0}));
    stopped(1);
    printf("traffic: seq page open playing: commits %.1f/s, texts %.1f/s, rects %.1f/s, 34 %.1f/s, 2F %.1f/s; page "
           "closed: 34 %.1f/s, 2F %.1f/s; stopped: commits %.1f/s\n", per_s(seq_open, 0x39), per_s(seq_open, 0x3D),
           per_s(seq_open, 0x38), per_s(seq_open, 0x34), per_s(seq_open, 0x2F), per_s(seq_closed, 0x34),
           per_s(seq_closed, 0x2F), per_s(seq_still, 0x39));
    // the bounds (dbg, 2026-10-21): the playhead redraws the Rytm's screen at most 4 times a second; the LEDs at most
    // once a step (8 a second at 120 BPM)
    CHECK(per_s(seq_open, 0x39) <= 4.5);
    CHECK(per_s(seq_open, 0x34) <= 8.5 && per_s(seq_closed, 0x34) <= 8.5);
    CHECK(per_s(seq_open, 0x2F) <= 2.5 && per_s(seq_closed, 0x2F) <= 2.5);
  }
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_traffic: ALL PASS\n");
  return 0;
}
