// core/tests/test_song_slots.cpp - SONG RECORD per slot (3(c) step 10c, core/song/song.h "Per slot"): the Rytm on slot 0
// and the A4 on slot 1 each record their own take from their own stream, at the same time, into their own folder
// (/.rytm/songs, /.a4/songs), with their own clock, transport, tempo, mutes, CC / NRPN and program changes.
// The A4's stock snapshot, MIDI CONFIG, replay and UNDO REPLAY as the Rytm's (its traced dumps, device 0x06: a fake A4
// here). The Rytm's stays as it always was: its arm asks its dumps, its MIDI CONFIG is midi_config() whatever the slot in hand.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "song/song.h"

using namespace core;


static int64_t g_frame = 0;
static uint64_t g_us = 1000000;
static int64_t frames() { return g_frame; }
static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> blk4(int64_t frame) {
  uint32_t b = (uint32_t)(frame / 32) & ((1u << 28) - 1);
  return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)};
}
static std::vector<uint8_t> with_blk(std::vector<uint8_t> v) {
  auto b = blk4(g_frame);
  v.insert(v.end(), b.begin(), b.end());
  return v;
}
// a slot's stream, byte by byte, as the platform's tap hands it over (pi/engine/bridge.cpp link_byte: the slot's Scope)
static void feed(int slot, const std::vector<uint8_t>& m) {
  device::Scope in(device::g_links[slot]);
  for (uint8_t b : m) song::byte(b);
}
static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
// ---- a fake A4 MKII (a4-song-dumps.md): its working kit / pattern / settings / global, answered for 0x68..0x6D (device id
// 0x06), written by 0x58 / 0x5A / 0x5C with no answer; a program change selects a pattern (each pattern its own kit and
// pattern here) and its mod reports it (2D 02); CC / NRPN logged
static std::vector<uint8_t> dump6(uint8_t id, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> m = {0xF0, 0x00, 0x20, 0x3C, 0x06, 0x00, id, 0x01, 0x01, 0x00};
  m.insert(m.end(), payload.begin(), payload.end());
  uint32_t sum = 0;
  for (size_t i = 10; i < m.size(); ++i) sum += m[i];   // (the object number left out)
  int len = (int)m.size() - 5;
  m.push_back((uint8_t)(sum >> 7 & 0x7F)), m.push_back((uint8_t)(sum & 0x7F));
  m.push_back((uint8_t)(len >> 7 & 0x7F)), m.push_back((uint8_t)(len & 0x7F));
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> payload(int n, int seed) {
  std::vector<uint8_t> p(n);
  for (int i = 0; i < n; ++i) p[i] = (uint8_t)((i * 5 + seed) & 0x7F);
  return p;
}
static std::vector<uint8_t> pack7(const std::vector<uint8_t>& u) {
  std::vector<uint8_t> p;
  for (size_t i = 0; i < u.size(); i += 7) {
    uint8_t hi = 0;
    for (size_t k = 0; k < 7 && i + k < u.size(); ++k) hi |= (uint8_t)((u[i + k] >> 7) << (6 - k));
    p.push_back(hi);
    for (size_t k = 0; k < 7 && i + k < u.size(); ++k) p.push_back(u[i + k] & 0x7F);
  }
  return p;
}
// its GLOBAL: AUTO CHANNEL 10, tracks 1..4 on 1..4, FX 5, CV 6, PROG CH IN / OUT CH OFF (the auto channel), PARAM OUTPUT
// NRPN, the sends and receives on
static std::vector<uint8_t> a4_global() {
  std::vector<uint8_t> u(0x30, 0);
  u[0] = 0x0B;   // (its version)
  u[song::kA4GlobalAuto] = 9;
  for (int t = 0; t < 6; ++t) u[song::kA4GlobalTrack + t] = (uint8_t)t;
  u[song::kA4GlobalPcIn] = u[song::kA4GlobalPcOut] = 0xFF, u[song::kA4GlobalPerf] = 0xFF;
  u[song::kA4GlobalParam] = 0;
  u[song::kA4GlobalClockSend] = u[song::kA4GlobalTrRecv] = u[song::kA4GlobalTrSend] = u[song::kA4GlobalPcRecv] = u[song::kA4GlobalPcSend] = 1;
  return dump6(0x5D, pack7(u));
}
struct FakeA4 {
  std::vector<uint8_t> kit, pat, settings, global;
  int cur = 0;
  std::vector<uint8_t> kits[128], pats[128];   // each pattern's own (selected by a PC)
  std::vector<std::vector<uint8_t>> pending;   // answers, fed back on its link next step
  std::vector<std::string> order;
  int reqs = 0, writes = 0, pcs = 0, wrong_dev = 0;
  std::vector<std::vector<uint8_t>> cc;       // channel messages
  void select(int p) {
    kits[cur] = kit, pats[cur] = pat;
    cur = p;
    if (kits[p].empty()) kits[p] = dump6(0x58, payload(2755, 40 + p)), pats[p] = dump6(0x5A, payload(14828, 60 + p));
    kit = kits[p], pat = pats[p];
    pending.push_back(msg(0x2D, {2, (uint8_t)p, 0x7F}));   // its mod: PATTERN now p, nothing queued
  }
} g_a4;
static void a4_in(const uint8_t* m, int n);
static void out1(const uint8_t* m, int n) {   // (a long dump comes in pieces: song.cpp kPiece)
  static std::vector<uint8_t> sx;
  g_out1.emplace_back(m, m + n);
  if (!sx.empty() || (n > 3 && m[0] == 0xF0 && m[n - 1] != 0xF7)) {
    sx.insert(sx.end(), m, m + n);
    if (sx.back() != 0xF7) return;
    std::vector<uint8_t> w;
    w.swap(sx);
    a4_in(w.data(), (int)w.size());
    return;
  }
  a4_in(m, n);
}
static void a4_in(const uint8_t* m, int n) {
  FakeA4& a = g_a4;
  if (n <= 3 && (m[0] & 0xF0) == 0xC0) {
    ++a.pcs, a.order.push_back("pc " + std::to_string(m[1]));
    a.select(m[1]);
    return;
  }
  if (n <= 3) { a.cc.emplace_back(m, m + n); return; }
  if (m[0] != 0xF0 || n < 15 || m[1] != 0 || m[2] != 0x20 || m[3] != 0x3C) return;   // (the Sidecar's own commands)
  if (m[4] != 0x06) { ++a.wrong_dev; return; }
  if (n == 15) {   // a request
    ++a.reqs, a.order.push_back("req " + std::to_string(m[6]));
    const uint8_t id = m[6];
    a.pending.push_back(id == 0x68 ? a.kit : id == 0x6A ? a.pat : id == 0x6C ? a.settings : a.global);
    return;
  }
  ++a.writes, a.order.push_back("write " + std::to_string(m[6]));   // a working dump: at once, no answer
  std::vector<uint8_t> d(m, m + n);
  if (m[6] == 0x58) a.kit = d;
  else if (m[6] == 0x5A) a.pat = d;
  else if (m[6] == 0x5C) a.settings = d;
}


static void put(const char* path, const char* text) {
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  fputs(text, f);
  fclose(f);
}
static std::string get(const char* path) {
  FILE* f = fopen(host_path(path).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static bool exists(const char* path) {
  struct stat sb;
  return stat(host_path(path).c_str(), &sb) == 0;
}
static void settle() {
  idle_flush(true);
  for (int i = 0; i < 4000; ++i) {
    device::step();
    idle_prepare();
    idle_save();
    idle_stream();
    bool loading = project_loading();
    for (int k = 1; k < kVoiceSlots; ++k) {
      device::Scope in(device::g_links[k]);
      loading = loading || project_loading();
    }
    if (i > 50 && idle_settled() && !loading) break;
  }
  idle_flush(false);
}

static std::string cmd(int slot, const char* c) {
  device::Scope in(device::g_links[slot]);
  char b[400];
  CHECK(song::command(c, b, sizeof b));
  return b;
}
static song::Status status(int slot) {
  device::Scope in(device::g_links[slot]);
  return song::status();
}
static void step(int ms) {   // time on, the fake A4's answers, the song's loop and card jobs (every slot's), the posted writes
  for (int i = 0; i < ms; ++i) {
    g_us += 1000, g_frame += 48;
    host_fake_time(true, g_us);
    std::vector<std::vector<uint8_t>> p;
    p.swap(g_a4.pending);
    for (auto& m : p) feed(1, m);
    song::tick(now_ms());
    fmt::post_step();
    for (int k = 0; k < 16 && song::idle(); ++k) {}
  }
}
static int count(const std::string& s, const char* what) {
  int n = 0;
  for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
  return n;
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_song_slots: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-songslots-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  settle();
  // the A4 on the second transport, beside the Rytm: slot 1
  device::rytm_on_usb(true);
  {
    const uint8_t h[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                         (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
    engine_sysex_link(1, h, (int)sizeof h);
  }
  settle();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[0]);
  CHECK(device::route_of(0) == 0 && device::route_of(1) == 1);   // (the platform's taps: each transport's slot)
  CHECK(!strcmp(prj(0).key, "RYP") && !strcmp(prj(1).key, "A4P"));

  FakeA4& A = g_a4;
  A.kit = dump6(0x58, payload(2755, 11)), A.pat = dump6(0x5A, payload(14828, 12)), A.settings = dump6(0x5C, payload(419, 13));
  A.global = a4_global();
  song::test_frames(frames);
  song::reset();
  step(20);
  CHECK(status(0).state == song::kOff && status(1).state == song::kOff);

  // ---- arm both. Each asks its own box's stock dumps: the Rytm's (device 0x07) to the Rytm, the A4's (0x06) to the A4
  g_out.clear(), g_out1.clear();
  feed(0, msg(0x2D, {7, 120 * 120 >> 14, (120 * 120 >> 7) & 127, 120 * 120 & 127}));   // the Rytm at 120 BPM
  feed(1, msg(0x2D, {7, 93 * 120 >> 14, (93 * 120 >> 7) & 127, 93 * 120 & 127}));      // the A4 at 93
  feed(1, msg(0x2D, {2, 0, 0x7F}));                                                    // the A4 on A01
  cmd(0, "song=arm");
  cmd(1, "song=arm");
  step(400);
  int req0 = 0;
  for (auto& m : g_out) req0 += m.size() == 15 && m[1] == 0 && m[2] == 0x20 && m[3] == 0x3C && m[4] == 0x07;
  printf("  armed: slot 0 %s (%d dump requests to the Rytm), slot 1 %s (%d to the A4: %s): %s\n", song::state_name(status(0).state),
         req0, song::state_name(status(1).state), A.reqs, A.order.empty() ? "" : A.order[0].c_str(), status(1).msg);
  CHECK(req0 >= 1 && A.reqs == 4 && A.wrong_dev == 0 && A.order[0] == "req 109");   // (0x6D, its GLOBAL, first)
  CHECK(status(0).state == song::kArming && status(1).state == song::kArmed);   // (the Rytm here never answers)
  CHECK(status(1).tempo == 93 * 120 && status(0).tempo == 120 * 120);   // (each box's own tempo)
  {
    device::Scope in(device::g_links[1]);
    const song::Take& t = song::current();
    CHECK(t.kit.n == (int)A.kit.size() && t.pat.n == (int)A.pat.size() && t.settings.n == (int)A.settings.size() && t.global.n > 0);
    // its MIDI CONFIG, read from its GLOBAL (a4-song-dumps.md 3): tracks 1..6, PCs on the auto channel 10, NRPN
    song::Status s = song::status();
    CHECK(s.cfg_valid && s.problems == 0);
    // midi_config() is the Rytm's whatever the slot in hand (core/mod/rytm_out.cpp sends to the Rytm with it)
    CHECK(song::midi_config().track_ch[0] == 1 && song::midi_config().fx_ch == 13);
  }

  // ---- PLAY on both; each its own clock: the Rytm's 4-byte 2C records, the A4's 5-byte ones (a flags byte)
  feed(0, msg(0x2D, with_blk({1, 1})));
  feed(1, msg(0x2D, with_blk({1, 1})));
  CHECK(status(0).state == song::kRec && status(1).state == song::kRec);
  const int P0 = 6000, P1 = 7742;   // frames a step: 120 and 93 BPM
  int64_t next0 = 0, next1 = 0;
  int st0 = 0, st1 = 0;
  bool knob0 = false;
  for (int ms = 0; ms < 4000; ++ms) {
    if (g_frame >= next0 && st0 < 32) {
      feed(0, msg(0x2C, [&] { auto v = blk4(g_frame); v.push_back(1); v.insert(v.end(), {0, (uint8_t)(st0 % 16), 16, 2}); return v; }()));
      next0 += P0, ++st0;
    }
    if (g_frame >= next1 && st1 < 24) {
      feed(1, msg(0x2C, [&] { auto v = blk4(g_frame); v.push_back(1); v.insert(v.end(), {0, (uint8_t)(st1 % 16), 16, 2, 0}); return v; }()));
      next1 += P1, ++st1;
      if (st1 == 3) feed(1, {0xB0, 74, 33});                                         // the A4's track 1: a CC
      if (st1 == 5) feed(1, {0xB1, 99, 2, 0xB1, 98, 9, 0xB1, 6, 90, 0xB1, 38, 0});   // track 2: an NRPN
      if (st1 == 7) feed(1, {0xB5, 92, 3});                                          // CV track: CC 92 (no Rytm scene)
      if (st1 == 8) feed(1, msg(0x2D, {5, 2, 0}));                                   // its mutes: track 2
      if (st1 == 12) feed(1, {0xC9, 5}), feed(1, msg(0x2D, {2, 0, 5}));              // a pattern queued
    }
    if (st0 == 6 && !knob0) knob0 = true, feed(0, {0xB2, 99, 1, 0xB2, 98, 3, 0xB2, 6, 64, 0xB2, 38, 0});   // the Rytm's knob
    step(1);
  }
  song::Status s0 = status(0), s1 = status(1);
  printf("  recording: slot 0 bar %u, %u events; slot 1 bar %u, %u events\n", (unsigned)(s0.t_now / song::kBar + 1),
         (unsigned)s0.events, (unsigned)(s1.t_now / song::kBar + 1), (unsigned)s1.events);
  CHECK(s0.state == song::kRec && s1.state == song::kRec);
  CHECK(s1.t_now >= 20 * 96 && s1.t_now < 25 * 96);   // (the A4's own steps: 24 by now, not the Rytm's 32)
  CHECK(s0.t_now >= 31 * 96);
  CHECK(exists("/.a4/songs/A4P/take-001.jsonl") && exists("/.rytm/songs/RYP/take-001.jsonl"));
  CHECK(!exists("/.rytm/songs/A4P") && !exists("/.a4/songs/RYP"));

  // ---- STOP: each its own file, in its own folder
  feed(1, msg(0x2D, with_blk({1, 0})));
  step(300);
  CHECK(status(1).state == song::kOff && status(0).state == song::kRec);   // (the Rytm still records)
  feed(0, msg(0x2D, with_blk({1, 0})));
  step(300);
  CHECK(status(0).state == song::kOff);
  const std::string a4 = get("/.a4/songs/A4P/take-001.json"), ry = get("/.rytm/songs/RYP/take-001.json");
  printf("  saved: /.a4/songs/A4P/take-001.json %d bytes, /.rytm/songs/RYP/take-001.json %d bytes\n", (int)a4.size(), (int)ry.size());
  CHECK(!a4.empty() && !ry.empty());
  CHECK(!exists("/.a4/songs/A4P/take-001.jsonl") && !exists("/.rytm/songs/RYP/take-001.jsonl"));
  CHECK(a4.find("\"box\":\"a4\"") != std::string::npos && ry.find("\"box\"") == std::string::npos);
  CHECK(a4.find("\"project\":\"A4P\"") != std::string::npos && ry.find("\"project\":\"RYP\"") != std::string::npos);
  CHECK(a4.find("\"tempo\":93") != std::string::npos && ry.find("\"tempo\":120") != std::string::npos);
  CHECK(a4.find("kit_x") != std::string::npos && a4.find("\"tracks\":6") != std::string::npos);   // (its start snapshot)
  CHECK(ry.find("\"tracks\"") == std::string::npos && ry.find("\"pc_in_ch\"") == std::string::npos);   // (the Rytm's as before)
  printf("  A4 take: %d cc, %d p, %d mute, %d pc, %d scene\n", count(a4, "\"cc\""), count(a4, "\"p\""), count(a4, "\"mute\""),
         count(a4, "\"pc\""), count(a4, "\"scene\","));
  CHECK(count(a4, "\"mute\"") >= 1 && count(a4, "\"pc\"") >= 1 && count(a4, "\"scene\",") == 0);
  CHECK(count(ry, "\"p\"") >= 1 && count(ry, "\"mute\"") == 0);
  {   // the take read back: the A4's box, its config, its events on its tracks
    song::Take t;
    char err[48];
    CHECK(song::read_json(a4.data(), (int)a4.size(), &t, err, sizeof err));
    CHECK(!strcmp(t.box, "a4") && !strcmp(t.project, "A4P") && t.n == (int)status(1).events);
    CHECK(t.cfg.valid && t.cfg.ntracks == 6 && t.cfg.track_ch[5] == 6 && t.cfg.auto_ch == 10 && t.cfg.track_ch[6] == 0);
    CHECK(t.kit.n == (int)A.kit.size() && t.pattern == 0);
    bool cc = false, nrpn = false, cv92 = false;
    for (int i = 0; i < t.n; ++i) {
      cc |= t.ev[i].type == song::kCc && t.ev[i].a == 1 && t.ev[i].b == 74 && t.ev[i].v == 33;
      nrpn |= t.ev[i].type == song::kParam && t.ev[i].a == 1 && t.ev[i].b == (2 << 7 | 9) && t.ev[i].v == 90 << 7;
      cv92 |= t.ev[i].type == song::kCc && t.ev[i].a == 6 && t.ev[i].b == 92;
    }
    CHECK(cc && nrpn && cv92);
    t.free();
  }

  // ---- the A4's replay, as the Rytm's: confirm -> backup (its working kit / pattern / settings read) -> PC to the start
  // pattern (reported by its mod) -> pattern X, kit X written -> its 6 tracks' mutes -> verified
  const std::vector<uint8_t> take_kit = A.kit, take_pat = A.pat;
  A.kit = dump6(0x58, payload(2755, 91)), A.pat = dump6(0x5A, payload(14828, 92));   // edited since
  const std::vector<uint8_t> edited_kit = A.kit, edited_pat = A.pat;
  A.order.clear(), A.cc.clear(), A.writes = 0;
  cmd(1, "song=replay");
  CHECK(status(1).state == song::kConfirm && A.writes == 0);
  cmd(1, "song=confirm");
  for (int i = 0; i < 3000 && status(1).state == song::kRestoring; ++i) step(1);
  printf("  slot 1 restore: %s (%s)\n", song::state_name(status(1).state), status(1).msg);
  CHECK(status(1).state == song::kReplayReady);
  CHECK(A.kit == take_kit && A.pat == take_pat && A.cur == 0);
  {
    std::string o;
    for (auto& x : A.order) o += x + ", ";
    printf("  the A4 saw: %s\n", o.c_str());
    // the backup's reads before any write, then the PC, pattern X, kit X, then the verify
    CHECK(o.find("req 104, req 106, req 108, pc 0, write 90, req 104, write 88, req 104, req 106") == 0);
  }
  int mutes = 0;
  for (auto& m : A.cc)
    if ((m[0] & 0xF0) == 0xB0 && m[1] == 94) {
      ++mutes;
      CHECK((m[0] & 15) < 6);
    }
  CHECK(mutes == 6);   // (its 6 tracks, on their own channels)
  CHECK(exists("/.a4/songs/A4P/.backups.txt") && !exists("/.rytm/songs/RYP/.backups.txt"));
  // PLAY: the take's events into the A4 on their channels, then its STOP
  A.cc.clear();
  feed(1, msg(0x2D, with_blk({1, 1})));
  CHECK(status(1).state == song::kReplaying);
  st1 = 0, next1 = g_frame;
  for (int ms = 0; ms < 4000 && status(1).state == song::kReplaying; ++ms) {
    if (g_frame >= next1 && st1 < 30) {
      feed(1, msg(0x2C, [&] { auto v = blk4(g_frame); v.push_back(1); v.insert(v.end(), {0, (uint8_t)(st1 % 16), 16, 2, 0}); return v; }()));
      next1 += P1, ++st1;
    }
    step(1);
  }
  bool cc74 = false, nrpn2 = false, stop = false;
  for (auto& m : A.cc) {
    cc74 |= m.size() == 3 && m[0] == 0xB0 && m[1] == 74 && m[2] == 33;
    nrpn2 |= m.size() == 3 && m[0] == 0xB1 && m[1] == 98 && m[2] == 9;
    stop |= m.size() == 1 && m[0] == 0xFC;
  }
  printf("  replayed into the A4: %d channel messages (cc 74 %d, nrpn %d, stop %d): %s\n", (int)A.cc.size(), cc74, nrpn2, stop,
         status(1).msg);
  CHECK(cc74 && nrpn2 && stop && status(1).state == song::kOff);
  feed(1, msg(0x2D, with_blk({1, 0})));   // (its STOP reported)
  step(20);
  // ---- UNDO REPLAY: the edited kit and pattern back, verified
  CHECK(status(1).undo);
  A.order.clear();
  cmd(1, "song=undo");
  for (int i = 0; i < 3000 && status(1).state == song::kUndoing; ++i) step(1);
  printf("  slot 1 undo: %s\n", status(1).msg);
  CHECK(status(1).state == song::kOff && strstr(status(1).msg, "replay undone"));
  CHECK(A.kit == edited_kit && A.pat == edited_pat);
  CHECK(A.wrong_dev == 0);
  // the Rytm saw none of the A4's
  for (auto& m : g_out) CHECK(!(m.size() > 4 && m[0] == 0xF0 && m[4] == 0x06));

  // ---- a PC the A4 does not act on (its stopped PC is not traced): the restore stops before anything is written
  {
    struct Mute {
      static void out(const uint8_t* m, int n) {
        g_out1.emplace_back(m, m + n);
        if (n == 15 && m[0] == 0xF0) a4_in(m, n);   // (answers its requests, ignores its PCs and writes)
      }
    };
    A.cur = 3;   // (it is on A04; the take starts on A01)
    feed(1, msg(0x2D, {2, 3, 0x7F}));
    device::set_out(1, Mute::out);
    A.writes = 0;
    cmd(1, "song=replay"), cmd(1, "song=confirm");
    for (int i = 0; i < 12000 && status(1).state == song::kRestoring; ++i) step(1);
    printf("  slot 1 restore with no PC acted on: %s (%s)\n", song::state_name(status(1).state), status(1).msg);
    CHECK(status(1).state == song::kOff && strstr(status(1).msg, "did not select") && A.writes == 0);
    device::set_out(1, out1);
  }

  // ---- each slot's take list its own
  cmd(0, "song=list"), cmd(1, "song=list");
  step(50);
  CHECK(status(0).takes == 1 && status(1).takes == 1);
  put("/.a4/songs/A4P/take-007.json", a4.c_str());
  cmd(1, "song=list");
  step(50);
  CHECK(status(0).takes == 1 && status(1).takes == 2);

  printf("test_song_slots: PASS (each box its own take, clock, tempo and folder; the A4's snapshot, replay and undo as the Rytm's)\n");
  return 0;
}
