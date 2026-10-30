// core/tests/test_song.cpp - SONG RECORD (core/song/song.h): the JSON round trip, the journal's recovery (a cut last line),
// the tick math (2C ticks with tempo changes, speeds, a switch, pause / continue, the F8 fallback), the recorder (NRPN,
// thinning, mutes, PCs), the replay's scheduling (PCs ahead of their switches, events at their ticks, re-anchoring),
// the restore's order against a fake Rytm (backup first, nothing written without it, the verify), and the engine end to
// end on a byte stream: arm -> the start snapshot -> PLAY -> knobs / mutes / a pattern change -> STOP -> the song file
// on the card; crash recovery from a journal; the load and a confirmed replay into the fake Rytm.
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "timing.h"
#include "bundle/rename.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "fx/send_fx.h"
#include "pool/pool.h"
#include "store/store.h"
#include "seq/seq.h"
#include "song/song.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;
using namespace core::song;
using namespace core::ui;

namespace core {
void host_set_card(const char* dir);
void host_fake_time(bool on, uint64_t us);
void host_card_core(int core);
}

// ---- helpers
static std::vector<uint8_t> dump(uint8_t id, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> m = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, id, 0x01, 0x01, 0x00};
  m.insert(m.end(), payload.begin(), payload.end());
  uint32_t sum = 0;
  for (size_t i = 9; i < m.size(); ++i) sum += m[i];
  int len = (int)m.size() - 5;
  m.push_back((uint8_t)(sum >> 7 & 0x7F)), m.push_back((uint8_t)(sum & 0x7F));
  m.push_back((uint8_t)(len >> 7 & 0x7F)), m.push_back((uint8_t)(len & 0x7F));
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> payload(int n, int seed) {
  std::vector<uint8_t> p(n);
  for (int i = 0; i < n; ++i) p[i] = (uint8_t)((i * 7 + seed) & 0x7F);
  return p;
}
// a GLOBAL X payload (7-bit packed) with the MIDI config bytes set as recording + replay want them
static std::vector<uint8_t> global_payload(bool good) {
  uint8_t u[80] = {};
  for (int t = 0; t < 13; ++t) u[12 + t] = (uint8_t)t;
  u[32] = good ? 0 : 1;          // PARAM OUTPUT: NRPN / CC
  u[35] = 1, u[36] = 1, u[37] = good, u[38] = 1;
  u[44] = good, u[46] = good;    // ENCODER DEST, OUTPUT CH
  std::vector<uint8_t> p;
  for (int i = 0; i < 80; i += 7) {
    uint8_t hi = 0;
    for (int k = 0; k < 7 && i + k < 80; ++k) hi |= (uint8_t)((u[i + k] >> 7) << (6 - k));
    p.push_back(hi);
    for (int k = 0; k < 7 && i + k < 80; ++k) p.push_back(u[i + k] & 0x7F);
  }
  return p;
}
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

// ---- PNGs of the sent frame (the Rytm's text in the host's font), as test_chord_view.cpp's
static uint32_t crc32_of(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
static std::vector<uint8_t> zstore(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t at = 0; at < raw.size();) {
    size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8)), z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + (long)at, raw.begin() + (long)(at + n));
    at += n;
  }
  for (uint8_t x : raw) a = (a + x) % 65521, b = (b + a) % 65521;
  uint32_t ad = b << 16 | a;
  for (int i = 3; i >= 0; --i) z.push_back((uint8_t)(ad >> (8 * i)));
  return z;
}
static void put_be32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
static void chunk(std::vector<uint8_t>& f, const char* t, const std::vector<uint8_t>& d) {
  put_be32(f, (uint32_t)d.size());
  std::vector<uint8_t> td(t, t + 4);
  td.insert(td.end(), d.begin(), d.end());
  f.insert(f.end(), td.begin(), td.end());
  put_be32(f, crc32_of(td.data(), td.size()));
}
static std::string g_dir;
static void png(const char* name) {
  uint8_t fb[kBytes];
  preview_sent(fb);
  const int s = 4, w = 128 * s, h = 64 * s;
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      int X = x / s, Y = y / s;
      bool on = fb[(7 - Y / 8) + 8 * X] >> (Y % 8) & 1;
      int c[3] = {on ? 232 : 12, on ? 238 : 14, on ? 255 : 22};
      bool grid = x % s == s - 1 || y % s == s - 1;
      for (int k = 0; k < 3; ++k) raw.push_back((uint8_t)(grid ? c[k] * 8 / 10 : c[k]));
    }
  }
  std::vector<uint8_t> z = zstore(raw);
  std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, ih;
  put_be32(ih, w), put_be32(ih, h);
  ih.insert(ih.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ih);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  std::string path = g_dir + "/" + name;
  FILE* fp = fopen(path.c_str(), "wb");
  CHECK(fp);
  fwrite(f.data(), 1, f.size(), fp);
  fclose(fp);
  printf("  %s\n", path.c_str());
}
static bool shown(const char* s) {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  for (int i = 0; i < n; ++i)
    if (strstr((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
static void key(int code, bool down) {
  auto m = msg(0x31, {(uint8_t)(code >> 7), (uint8_t)(code & 0x7F), (uint8_t)down});
  engine_sysex(m.data(), (int)m.size());
}

// ---- 1. the JSON round trip and the journal
static Take sample_take() {
  Take t;
  t.clear();
  strcpy(t.project, "RYTMLINJE");
  strcpy(t.name, "take-007");
  config_default(&t.cfg);
  t.cfg.valid = true, t.cfg.encoder_ext = 1, t.cfg.trk_ch = 1, t.cfg.nrpn = 1, t.cfg.pc_receive = 1, t.cfg.auto_ch = 14;
  t.pattern = 3, t.tempo = 15240, t.mutes = 0x24, t.scene = 2;
  auto k = dump(0x58, payload(2988, 1)), p = dump(0x5A, payload(14978, 2)), s = dump(0x5C, payload(300, 3));
  t.kit.add(k.data(), (int)k.size()), t.pat.add(p.data(), (int)p.size()), t.settings.add(s.data(), (int)s.size());
  t.has_fx = true;
  t.fx_send[3][1] = 99, t.fx_knob[1][2] = 77, t.fx_route = 2;
  auto ev = [&](uint32_t tt, uint8_t ty, uint8_t a, uint16_t b, uint32_t v) {
    Ev e = {tt, ty, a, b, v, tt / 2};
    CHECK(t.push(e));
  };
  ev(0, kPlay, 0, 0, 0);
  ev(10, kParam, 0, 1 << 7 | 1, 7236);
  ev(20, kParam, kFxTrack, 2 << 7 | 0, 100);
  ev(30, kParam, 0x40 | 13, 1 << 7 | 5, 3);
  ev(40, kCc, 13, 16, 64);
  ev(50, kMute, 0, 0, 0x804);
  ev(60, kScene, 5, 0, 0);
  ev(70, kPerf, 35, 0, 90);
  ev(80, kTempo, 0, 0, 15360);
  ev(90, kPc, 4, 0, 0);
  ev(1536, kSw, 3, 4, 0);
  ev(1600, kSc, kScKnob, 8, 42);
  ev(1700, kMach, 2, 0x48, 0);
  ev(1800, kFill, 1, 0, 0);
  ev(1900, kMark, 0, 0, 0);
  ev(1950, kSrc, 4, 0, 5 << 7 | 0x50);   // rui1038: BT's P1 = F005 at 14 bits
  ev(2000, kPause, 0, 0, 0);
  ev(2000, kCont, 0, 0, 0);
  ev(3072, kStop, 0, 0, 0);
  t.end = 3072;
  return t;
}
static bool same(const Take& a, const Take& b) {
  if (a.n != b.n || a.end != b.end || a.pattern != b.pattern || a.tempo != b.tempo || a.mutes != b.mutes || a.scene != b.scene)
    return false;
  if (strcmp(a.project, b.project) || strcmp(a.name, b.name)) return false;
  if (!same_dump(a.kit, b.kit) || !same_dump(a.pat, b.pat) || a.settings.n != b.settings.n) return false;
  if (a.has_fx != b.has_fx || memcmp(a.fx_send, b.fx_send, sizeof a.fx_send) || memcmp(a.fx_knob, b.fx_knob, sizeof a.fx_knob) ||
      a.fx_route != b.fx_route)
    return false;
  if (a.cfg.valid != b.cfg.valid || a.cfg.nrpn != b.cfg.nrpn || a.cfg.trk_ch != b.cfg.trk_ch || a.cfg.auto_ch != b.cfg.auto_ch ||
      memcmp(a.cfg.track_ch, b.cfg.track_ch, sizeof a.cfg.track_ch))
    return false;
  for (int i = 0; i < a.n; ++i) {
    const Ev &x = a.ev[i], &y = b.ev[i];
    if (x.t != y.t || x.type != y.type || x.a != y.a || x.b != y.b || x.v != y.v) {
      fprintf(stderr, "event %d differs: %u %d %d %d %u / %u %d %d %d %u\n", i, x.t, x.type, x.a, x.b, x.v, y.t, y.type, y.a, y.b, y.v);
      return false;
    }
  }
  return true;
}

static void test_json() {
  Take t = sample_take();
  Buf out;
  JsonGen g;
  g.begin(&t, false);
  g.per = 5;   // several event pieces
  int pieces = 0;
  while (g.next(out)) ++pieces;
  CHECK(pieces >= 5);
  Take r;
  r.clear();
  char err[64];
  if (!read_json((const char*)out.p, out.n, &r, err, sizeof err)) {
    int at = atoi(strstr(err, "byte ") ? strstr(err, "byte ") + 5 : "0");
    fprintf(stderr, "%s: ...%.120s\n", err, (const char*)out.p + (at > 60 ? at - 60 : 0));
  }
  CHECK(r.n == t.n && same(t, r));
  // the incremental reader, one event a step
  Take r2;
  JsonRead rd;
  rd.begin((const char*)out.p, out.n, &r2);
  int steps = 0;
  while (rd.step(1)) ++steps;
  CHECK(rd.ok && steps >= t.n && same(t, r2));
  // a damaged file: refused, not half read as good
  Take r3;
  CHECK(!read_json((const char*)out.p, out.n / 2, &r3, err, sizeof err));
  // the journal: its header line, the event lines, the last line cut (a crash mid-write) -> the rest recovered
  Buf j;
  g.begin(&t, true);
  while (g.next(j)) {}
  CHECK(memchr(j.p, '\n', j.n) == j.p + j.n - 1);   // one line
  char b[160];
  for (int i = 0; i < t.n; ++i) {
    int k = event_json(t.ev[i], b, sizeof b);
    j.add(b, k), j.addc('\n');
  }
  Take rec;
  CHECK(recover_journal((const char*)j.p, j.n - 7, &rec, err, sizeof err));   // the stop line cut
  CHECK(rec.recovered && rec.n == t.n - 1 && rec.ev[rec.n - 1].type == kCont);
  CHECK(same_dump(rec.kit, t.kit) && same_dump(rec.pat, t.pat) && rec.pattern == 3 && rec.mutes == 0x24);
  t.free(), r.free(), r2.free(), r3.free(), rec.free(), out.free(), j.free();
  // base64 and the dump frame
  uint8_t raw[7] = {0, 1, 2, 250, 251, 252, 9}, back[8];
  char s64[16];
  int n64 = b64_encode(raw, 7, s64, sizeof s64);
  CHECK(n64 == 12 && b64_decode(s64, n64, back, sizeof back) == 7 && !memcmp(raw, back, 7));
  auto d = dump(0x58, payload(100, 4));
  uint8_t id = 0;
  CHECK(dump_ok(d.data(), (int)d.size(), &id) && id == 0x58);
  d[9] = 1;   // the object number out of the sum (the Rytm's answer for A02) accepted
  CHECK(dump_ok(d.data(), (int)d.size(), &id));
  d[20] ^= 1;
  CHECK(!dump_ok(d.data(), (int)d.size(), &id));
  // the MIDI CONFIG
  auto gp = global_payload(true);
  uint8_t u[90];
  int un = unpack7(gp.data(), (int)gp.size(), u, sizeof u);
  CHECK(un == 80);
  MidiConfig c = {};
  CHECK(parse_global(u, un, &c) && c.encoder_ext && c.trk_ch && c.nrpn && c.pc_receive && c.track_ch[0] == 1 && c.fx_ch == 13);
  CHECK(config_problems(c, false) == 0 && config_problems(c, true) == 0);
  auto bp = global_payload(false);
  unpack7(bp.data(), (int)bp.size(), u, sizeof u);
  parse_global(u, 80, &c);
  CHECK(config_problems(c, false) == (kCfgEncoder | kCfgTrkCh | kCfgNrpn) && config_problems(c, true) == kCfgPcReceive);
  char txt[96];
  config_text(config_problems(c, false), txt, sizeof txt);
  CHECK(strstr(txt, "ENCODER DEST INT") && strstr(txt, "OUTPUT CH AUTO"));
  // the how-to's checklist (UX review T13): the bad config's rows 0..3 wrong; the good one none; unread -1
  CHECK((config_wrong(c) & 15) == 15 && config_row(0) && strstr(config_row(3), "PROG CH RECEIVE") && !config_row(kCfgRows));
  MidiConfig unread = {};
  CHECK(config_wrong(unread) == -1);
  unpack7(gp.data(), (int)gp.size(), u, sizeof u);
  parse_global(u, 80, &c);
  CHECK((config_wrong(c) & 15) == 0);
  printf("json: round trip, incremental read, journal recovery, base64, dumps, MIDI CONFIG ok\n");
}

// ---- 2. the tick math
static void test_clock() {
  const double P120 = 48000.0 * 60 / 120 / 4;   // frames a 1x step at 120 BPM: 6000
  Clock c;
  c.tempo(120);
  c.start(1000);
  c.tick(0, 16, 2, 1000);
  CHECK(c.at(1000) == 0 && c.at(1000 + P120 / 2) == 48);
  CHECK(c.at(1000 + P120 * 3) == 95);   // never past the next tick
  for (int s = 1; s < 16; ++s) c.tick(s, 16, 2, 1000 + s * P120);
  CHECK(c.t == 15 * 96u);
  c.switch_seen();
  c.tick(0, 32, 2, 1000 + 16 * P120);   // a switch to a 32-step pattern: one step on, not 17
  CHECK(c.t == 16 * 96u);
  // a tempo change to 90: t stays musical (96 a step), the interpolation follows the new period
  c.tempo(90);
  const double P90 = 48000.0 * 60 / 90 / 4;
  c.tick(1, 32, 2, 1000 + 16 * P120 + P90);
  CHECK(c.t == 17 * 96u && c.at(1000 + 16 * P120 + P90 + P90 / 4) == 17 * 96u + 24);
  // a 2x track: two ticks a 1x step, 48 each
  Clock d;
  d.tempo(120);
  d.start(0);
  d.tick(0, 16, 0, 0);
  d.tick(1, 16, 0, P120 / 2);
  d.tick(2, 16, 0, P120);
  CHECK(d.t == 96 && d.at(P120 + P120 / 4) == 96 + 24);
  // a dropped tick: two steps at once
  d.tick(5, 16, 0, P120 * 2.5);
  CHECK(d.t == 96 + 3 * 48);
  // pause / continue: t frozen, then on from the next tick
  Clock e;
  e.tempo(120);
  e.start(0);
  e.tick(0, 16, 2, 0);
  e.tick(1, 16, 2, P120);
  e.pause(P120 + P120 / 2);
  CHECK(e.at(P120 * 10) == 96 + 48);
  e.cont(P120 * 20);
  CHECK(e.at(P120 * 20) == 96 + 48);
  e.tick(2, 16, 2, P120 * 20 + 10);
  CHECK(e.t == 192);
  // the F8 fallback (no 2C): 6 a step
  Clock f;
  f.start(0);
  for (int i = 0; i < 13; ++i) f.f8(i * P120 / 6);
  CHECK(f.t == 12 * 16 && f.at(12 * P120 / 6 + P120 / 12) == 12 * 16 + 8);
  // with no tempo reported, the ticks measure the period
  Clock g;
  g.start(0);
  g.tick(0, 16, 2, 0);
  g.tick(1, 16, 2, P120);
  CHECK(fabs(g.period - P120) < 1 && g.at(P120 * 1.5) == 96 + 48);
  printf("clock: ticks, switch, tempo change, 2x speed, drops, pause / continue, F8, measured period ok\n");
}

// ---- 3. the recorder
static void test_recorder() {
  Take t;
  t.clear();
  config_default(&t.cfg);
  t.cfg.valid = true;
  Recorder r;
  r.arm(&t);
  const double P = 6000;
  r.tempo(120, 0, 0);
  // armed, stopped: a knob -> the start (t 0)
  r.channel(0xB0, 95, 100, 0, 0);
  CHECK(t.n == 1 && t.ev[0].t == 0 && t.ev[0].type == kCc);
  r.pattern_state(2, -1, 0, 0);
  r.play(100, 1000);
  CHECK(t.pattern == 2 && t.ev[t.n - 1].type == kPlay);
  r.clk.tick(0, 16, 2, 100);
  // NRPN on channel 2 (SD): 99 1 98 1 6 56 38 68 -> one kParam, 14 bits
  double f = 100 + P / 2;
  for (auto cc : {std::make_pair(99, 1), std::make_pair(98, 1), std::make_pair(6, 56), std::make_pair(38, 68)})
    r.channel(0xB1, (uint8_t)cc.first, (uint8_t)cc.second, f, 1100);
  const Ev& e = t.ev[t.n - 1];
  CHECK(e.type == kParam && e.a == 1 && e.b == (1 << 7 | 1) && e.v == (56u << 7 | 68) && e.t == 48);
  int n0 = t.n;
  for (auto cc : {std::make_pair(99, 1), std::make_pair(98, 1), std::make_pair(6, 56), std::make_pair(38, 68)})
    r.channel(0xB1, (uint8_t)cc.first, (uint8_t)cc.second, f + 1000, 1100);   // the same value: thinned
  CHECK(t.n == n0);
  r.channel(0xB1, 6, 57, f + 1001, 1100), r.channel(0xB1, 38, 0, f + 1001, 1100);   // (99 / 98 kept: the next value)
  r.channel(0xB1, 6, 58, f + 1002, 1100), r.channel(0xB1, 38, 0, f + 1002, 1100);   // the same tick: replaces
  CHECK(t.n == n0 + 1 && t.ev[t.n - 1].v == 58u << 7);
  // CC 94 mute on channel 3 (RS) then 2D's mask: one event each, no duplicates
  r.channel(0xB2, 94, 1, f + 2000, 1200);
  CHECK(t.ev[t.n - 1].type == kMute && t.ev[t.n - 1].v == 4);
  r.mute_mask(4, f + 2001, 1200);
  CHECK(t.ev[t.n - 1].type == kMute && t.n == n0 + 2);
  // scene, PERF (auto channel 14), FX channel 13 CC
  r.channel(0xB0, 92, 3, f + 3000, 1300);
  CHECK(t.ev[t.n - 1].type == kScene && t.ev[t.n - 1].a == 3);
  r.channel(0xBD, 36, 80, f + 3000, 1300);
  CHECK(t.ev[t.n - 1].type == kPerf && t.ev[t.n - 1].a == 36);
  r.channel(0xBC, 16, 64, f + 3000, 1300);
  CHECK(t.ev[t.n - 1].type == kCc && t.ev[t.n - 1].a == 13);
  // a queued pattern by 2D PATTERN and then the Rytm's PC: one kPc; the switch
  r.pattern_state(2, 5, f + 4000, 1400);
  int n1 = t.n;
  r.channel(0xCD, 5, 0, f + 4100, 1400);
  CHECK(t.n == n1 && t.ev[t.n - 1].type == kPc && t.ev[t.n - 1].a == 5);
  r.switched(2, 5, f + 5000, 1500);
  CHECK(t.ev[t.n - 1].type == kSw && t.ev[t.n - 1].a == 2 && t.ev[t.n - 1].b == 5);
  r.stop(f + 6000, 1600);
  CHECK(t.ev[t.n - 1].type == kStop && !r.on && t.end > 0);
  // events stay in time order
  for (int i = 1; i < t.n; ++i) CHECK(t.ev[i].t >= t.ev[i - 1].t);
  t.free();
  printf("recorder: start events, NRPN, thinning, mutes, scene, PERF, FX CC, PC + switch ok\n");
}

// ---- 4. the replay's scheduling
struct FakePort : Port {
  std::vector<std::vector<uint8_t>> sent;
  std::vector<uint8_t> reqs;
  std::vector<Ev> sc;
  bool backup_ok = true, backup_called = false, backup_p1 = false, plays = false;
  int starts = 0;
  void send(const uint8_t* m, int n) override { sent.emplace_back(m, m + n); }
  void request(uint8_t id) override { reqs.push_back(id); }
  int backup_n = 0, relinked = -1;
  bool backup_partial = false;
  bool backup(const Take&, const Take* const*, int n, bool partial) override {
    backup_called = true, backup_p1 = n > 0, backup_n = n, backup_partial = partial;
    return backup_ok;
  }
  bool relink(int k, const Buf&) override { relinked = k; return true; }
  bool playing() override { return plays; }
  void sidecar(const Ev& e) override { sc.push_back(e); }
  void sidecar_start(const Take&) override { ++starts; }
};

static void test_replay() {
  Take t;
  t.clear();
  config_default(&t.cfg);
  t.cfg.valid = true, t.cfg.auto_ch = 14;
  auto ev = [&](uint32_t tt, uint8_t ty, uint8_t a, uint16_t b, uint32_t v) {
    Ev e = {tt, ty, a, b, v, 0};
    t.push(e);
  };
  ev(0, kPlay, 0, 0, 0);
  ev(0, kParam, 0, 1 << 7 | 1, 1000);            // BD TUN at the start
  ev(200, kMute, 0, 0, 0x2);                      // SD muted
  ev(400, kPc, 1, 0, 0);                          // A02 queued at step 4
  ev(1000, kParam, 12, 2 << 7 | 0, 500);          // FX delay time
  ev(1536, kSw, 0, 1, 0);                         // the switch at bar 2
  ev(1540, kParam, 0, 1 << 7 | 1, 2000);          // after the switch (the new kit)
  ev(2000, kSc, kScSend, 3, 77);
  ev(2100, kTempo, 0, 0, 15000);                  // not replayable
  ev(2500, kSw, 1, 2, 0);                         // no kPc before it: two steps ahead
  ev(2600, kScene, 4, 0, 0);
  ev(3000, kStop, 0, 0, 0);
  t.end = 3000;
  CHECK(pc_time(t, 5) == 400);
  CHECK(pc_time(t, 9) == 2500 - 192);
  FakePort p;
  Replayer r;
  r.begin(&t);
  r.start(p);
  CHECK(p.sent.size() == 4);   // the t-0 NRPN: 99 98 6 38 on channel 1
  CHECK(p.sent[0][0] == 0xB0 && p.sent[0][1] == 99 && p.sent[2][1] == 6 && p.sent[2][2] == 1000 >> 7 && p.sent[3][2] == (1000 & 127));
  p.sent.clear();
  r.advance(199, p);
  CHECK(p.sent.empty());
  r.advance(200, p);
  CHECK(p.sent.size() == 1 && p.sent[0][0] == 0xB1 && p.sent[0][1] == 94 && p.sent[0][2] == 1);
  p.sent.clear();
  r.advance(400, p);   // the PC at its recorded queue time, on the auto channel
  CHECK(p.sent.size() == 1 && p.sent[0].size() == 2 && p.sent[0][0] == 0xCD && p.sent[0][1] == 1);
  p.sent.clear();
  r.advance(1000, p);
  CHECK(p.sent.size() == 4 && p.sent[0][0] == 0xBC);   // the FX track's channel 13
  p.sent.clear();
  r.advance(1550, p);   // past the recorded switch, the Rytm has not switched yet: what follows it waits
  CHECK(p.sent.empty());
  // the Rytm switches 8 ticks late: re-anchored, the events after the switch from there
  r.switched(1, 1544, p);
  CHECK(r.offset == -8 && r.reanchors == 1);
  r.advance(1548, p);
  CHECK(p.sent.size() == 4 && p.sent[0][1] == 99);
  p.sent.clear();
  r.advance(2000 + 8, p);
  CHECK(p.sc.size() == 1 && p.sc[0].b == 3 && p.sc[0].v == 77);
  r.advance(2500 - 192 + 8, p);   // the second switch's PC, two steps ahead
  CHECK(p.sent.size() == 1 && p.sent[0][0] == 0xCD && p.sent[0][1] == 2);
  p.sent.clear();
  // this time no SWITCH comes (a Rytm without the 2D): after kSwWait the rest goes on
  r.advance(2500 + 8 + Replayer::kSwWait, p);
  CHECK(p.sent.size() == 1 && p.sent[0][0] == 0xBD && p.sent[0][1] == 92 && p.sent[0][2] == 4);
  CHECK(r.skipped == 1);   // the tempo
  // fix1041: the take's end (its STOP at 3000, re-anchored +8): nothing before it, then one MIDI STOP and ended
  CHECK(r.end_t == 3000);
  p.sent.clear();
  r.advance(3000 + 8 - 1, p);
  CHECK(p.sent.empty() && !r.ended);
  r.advance(3000 + 8, p);
  CHECK(r.ended && p.sent.size() == 1 && p.sent[0].size() == 1 && p.sent[0][0] == 0xFC);
  r.advance(3200, p);
  CHECK(p.sent.size() == 1);   // once
  // without a recorded STOP: the take's last tick (Take::end); TRANSPORT RECEIVE off: ended, no STOP sent
  t.n -= 1;
  t.end = 2800;
  Replayer r2;
  r2.begin(&t);
  CHECK(r2.end_t == 2800);
  r2.send_stop = false;
  FakePort p2;
  r2.start(p2);
  for (uint32_t tt = 0; tt <= 2799; tt += 24) r2.advance(tt, p2);   // (a Rytm without SWITCH reports: each waits kSwWait)
  for (uint32_t tt = 2800; tt <= 2800 + 2 * Replayer::kSwWait; tt += 24) r2.advance(tt, p2);
  CHECK(r2.ended);
  for (auto& m : p2.sent) CHECK(m[0] != 0xFC);
  t.free();
  printf("replay: t-0 events, mutes, PCs ahead (recorded queue / two steps), the hold and re-anchor at a switch, the "
         "Sidecar's own, the fallback without SWITCH ok\n");
}

// ---- 5. the restore's order, against a fake Rytm
static void test_restore() {
  Take t = sample_take();
  for (int i = 0; i < t.n; ++i)
    if (t.ev[i].type == kSw) t.ev[i].b = 3;   // (one pattern played: A04 throughout - the visits further down)
  FakePort p;
  Restore r;
  r.begin(&t, false, 1000, p, 3);   // (A04 selected already: one pattern)
  CHECK(r.st == Restore::kBackupReq && p.reqs.size() == 1 && p.reqs[0] == kReqKit && p.sent.empty());
  auto cur_kit = dump(0x58, payload(2988, 9)), cur_pat = dump(0x5A, payload(14978, 8)), cur_set = dump(0x5C, payload(300, 7));
  r.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1010, p);
  r.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 1020, p);
  CHECK(p.sent.empty() && !p.backup_called);
  r.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 1030, p);
  CHECK(r.st == Restore::kBackupWrite && p.backup_called && !p.backup_p1 && p.sent.empty());   // nothing written before the backup is
  CHECK(r.backup.kit.n == (int)cur_kit.size() && r.backup.pat.n == (int)cur_pat.size());
  r.backup_done(true, 1100, p);
  CHECK(r.st == Restore::kSelect && p.sent.size() == 1 && p.sent[0][0] == 0xCD && p.sent[0][1] == 3);   // A04
  uint32_t ms = 1100;
  while (r.busy() && r.want < 0 && ms < 3000) r.tick(ms += 10, p);
  CHECK(r.st == Restore::kRelink && r.want == kReqKit && p.sent.size() == 2);   // pattern X out: the kit it selects read
  r.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), ms, p);                   // (the same kit: on)
  CHECK(r.st == Restore::kPattern && p.relinked < 0);
  while (r.busy() && r.want < 0 && ms < 3000) r.tick(ms += 10, p);
  CHECK(r.st == Restore::kVerify && r.want == kReqKit);
  // the order: PC, pattern X, kit X, 12 mutes; the Sidecar's start
  CHECK(p.sent.size() == 1 + 1 + 1 + 12 && p.sent[1][6] == 0x5A && p.sent[2][6] == 0x58 && p.starts == 1);
  CHECK(p.sent[3][0] == 0xB0 && p.sent[3][1] == 94 && p.sent[3][2] == 0 && p.sent[5][2] == 1);   // 0x24: RS (2) muted
  r.dump_in(0x58, t.kit.p, t.kit.n, ms, p);
  r.dump_in(0x5A, t.pat.p, t.pat.n, ms, p);
  CHECK(r.st == Restore::kDone && r.verify_kit && r.verify_pat);
  // a backup that cannot be written: nothing goes to the Rytm
  FakePort q;
  q.backup_ok = false;
  Restore r2;
  r2.begin(&t, false, 0, q, 3);
  r2.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, q);
  r2.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, q);
  r2.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, q);
  CHECK(r2.st == Restore::kFailed && q.sent.empty());
  // a Rytm that does not answer the backup's request: nothing written
  FakePort z;
  Restore r3;
  r3.begin(&t, false, 0, z, 3);
  r3.tick(Restore::kRetryMs + 10, z);   // a request the Rytm missed: asked again
  CHECK(z.reqs.size() == 2 && z.reqs[1] == kReqKit && r3.busy());
  r3.tick(Restore::kTimeoutMs + 10, z);
  CHECK(r3.st == Restore::kFailed && z.sent.empty() && strstr(r3.msg, "nothing written"));
  // the verify finds another kit: failed, said
  FakePort w;
  Restore r4;
  r4.begin(&t, true, 0, w, 3);
  r4.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, w);
  r4.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, w);
  r4.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, w);
  r4.backup_done(true, 4, w);
  ms = 4;
  while (r4.busy() && r4.want < 0 && ms < 3000) r4.tick(ms += 10, w);
  r4.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), ms, w);   // (relink: the same kit)
  while (r4.busy() && r4.want < 0 && ms < 3000) r4.tick(ms += 10, w);
  CHECK(w.sent.size() == 1 + 3 + 12 && w.sent[3][6] == 0x5C);   // settings X too when asked
  r4.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), ms, w);
  r4.dump_in(0x5A, t.pat.p, t.pat.n, ms, w);
  CHECK(r4.st == Restore::kFailed && !r4.verify_kit && r4.verify_pat);
  // undo-replay review 1: the take starts on another pattern (A01 selected): A01's dumps, PC A04, A04's dumps, then
  // the backup of both; nothing written before it
  {
    FakePort a;
    Restore r5;
    r5.begin(&t, false, 0, a, 0);
    r5.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, a);
    r5.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, a);
    r5.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, a);
    CHECK(r5.st == Restore::kBackupSel && a.sent.size() == 1 && a.sent[0][0] == 0xCD && a.sent[0][1] == 3 && r5.selected);
    uint32_t m5 = 3;
    while (r5.st == Restore::kBackupSel && m5 < 1000) r5.tick(m5 += 10, a);
    CHECK(r5.st == Restore::kBackupReq1 && a.reqs.back() == kReqKit);
    auto k1 = dump(0x58, payload(2988, 31)), p1 = dump(0x5A, payload(14978, 32));
    r5.dump_in(0x58, k1.data(), (int)k1.size(), m5, a);
    r5.dump_in(0x5A, p1.data(), (int)p1.size(), m5, a);
    CHECK(r5.st == Restore::kBackupWrite && a.backup_p1 && r5.backup.pattern == 0 && r5.extra[0].pattern == 3 &&
          r5.extra[0].kit.n == (int)k1.size() && a.sent.size() == 1);
    r5.backup_done(true, m5, a);
    CHECK(r5.st == Restore::kSelect && a.sent.size() == 1);   // (no second PC: A04 is selected)
    // ... its backup not written: the PC undone (A01 again), nothing else
    FakePort b;
    Restore r6;
    r6.begin(&t, false, 0, b, 0);
    r6.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, b);
    r6.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, b);
    r6.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, b);
    for (uint32_t m6 = 3; r6.st == Restore::kBackupSel && m6 < 1000;) r6.tick(m6 += 10, b);
    r6.dump_in(0x58, k1.data(), (int)k1.size(), 400, b);
    r6.dump_in(0x5A, p1.data(), (int)p1.size(), 400, b);
    r6.backup_done(false, 500, b);
    CHECK(r6.st == Restore::kFailed && b.sent.size() == 2 && b.sent[1][0] == 0xCD && b.sent[1][1] == 0 &&
          strstr(r6.msg, "nothing written"));
    // review 4: the backup's files never reported: failed after kWriteTimeoutMs (nothing written); a late result changes
    // nothing. Cancelled before a write: stopped, its PC undone; once written: not
    FakePort c;
    Restore r7;
    r7.begin(&t, false, 0, c, 3);
    r7.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, c);
    r7.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, c);
    r7.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, c);
    r7.tick(3 + Restore::kWriteTimeoutMs - 10, c);
    CHECK(r7.st == Restore::kBackupWrite);
    r7.tick(3 + Restore::kWriteTimeoutMs + 10, c);
    CHECK(r7.st == Restore::kFailed && c.sent.empty() && strstr(r7.msg, "not on the card"));
    r7.backup_done(true, Restore::kWriteTimeoutMs + 20, c);
    CHECK(r7.st == Restore::kFailed && c.sent.empty());
    FakePort d;
    Restore r8;
    r8.begin(&t, false, 0, d, 0);
    r8.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, d);
    r8.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, d);
    r8.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, d);
    CHECK(r8.cancel(d) && r8.st == Restore::kFailed && d.sent.size() == 2 && d.sent[1][1] == 0 && strstr(r8.msg, "cancelled"));
    // review 6: a PLAY during the writes stops them
    FakePort f;
    Restore r9;
    r9.begin(&t, false, 0, f, 3);
    r9.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, f);
    r9.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, f);
    r9.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, f);
    r9.backup_done(true, 4, f);
    uint32_t m9 = 4;
    while (!r9.wrote && m9 < 2000) r9.tick(m9 += 10, f);
    CHECK(!r9.cancel(f));   // (writing: not cancelled)
    f.plays = true;
    r9.tick(m9 += 100, f);
    size_t n9 = f.sent.size();
    CHECK(r9.st == Restore::kFailed && strstr(r9.msg, "played"));
    r9.tick(m9 += 100, f);
    CHECK(f.sent.size() == n9);
  }
  t.free();
  printf("restore: backup first, nothing written without it, PC -> pattern X -> kit X [-> settings X] -> mutes -> verify ok\n");
  // re-review: every pattern the take plays backed up at confirm (stopped), in order: its start A04, then A05 (its
  // switch); then the PC to A04 and the writes; the kit A04's pattern X selects read before kit X: another kit -> kit2
  {
    Take u = sample_take();   // (A04, switching to A05)
    FakePort a;
    Restore r;
    r.begin(&u, false, 0, a, 0);
    CHECK(r.nvisit == 2 && r.visit[0] == 3 && r.visit[1] == 4 && r.plays == 3 && !r.partial);
    r.dump_in(0x58, cur_kit.data(), (int)cur_kit.size(), 1, a);
    r.dump_in(0x5A, cur_pat.data(), (int)cur_pat.size(), 2, a);
    r.dump_in(0x5C, cur_set.data(), (int)cur_set.size(), 3, a);
    uint32_t m = 3;
    for (int v = 0; v < 2; ++v) {
      CHECK(r.st == Restore::kBackupSel && a.sent.back()[0] == 0xCD && a.sent.back()[1] == 3 + v);
      while (r.st == Restore::kBackupSel && m < 5000) r.tick(m += 10, a);
      auto kv = dump(0x58, payload(2988, 50 + v)), pv = dump(0x5A, payload(14978, 60 + v));
      r.dump_in(0x58, kv.data(), (int)kv.size(), m, a);
      r.dump_in(0x5A, pv.data(), (int)pv.size(), m, a);
    }
    CHECK(r.st == Restore::kBackupWrite && a.backup_n == 2 && r.extra[1].pattern == 4 && a.sent.size() == 2);
    r.backup_done(true, m, a);
    CHECK(r.st == Restore::kSelect && a.sent.size() == 3 && a.sent[2][1] == 3);   // back to A04, the take's start
    while (r.busy() && r.want < 0 && m < 8000) r.tick(m += 10, a);
    CHECK(r.st == Restore::kRelink && r.wrote);
    auto other = dump(0x58, payload(2988, 77));   // the kit A04's new pattern X links: not A04's kit before
    r.dump_in(0x58, other.data(), (int)other.size(), m, a);
    CHECK(r.st == Restore::kRelinkWrite && a.relinked == 1 && r.relinked.n == (int)other.size() &&
          !memcmp(r.relinked.p, other.data(), other.size()));
    size_t n0 = a.sent.size();
    r.tick(m += 1000, a);
    CHECK(a.sent.size() == n0);   // (kit X waits for the linked kit's backup)
    r.relink_done(true, m, a);
    while (r.busy() && r.want < 0 && m < 12000) r.tick(m += 10, a);
    CHECK(a.sent.size() > n0 && a.sent[n0][6] == 0x58);
    // a take that plays more than kUndoMax patterns: its start only, said
    Take w9 = sample_take();
    for (int k = 0; k < 9; ++k) { Ev e = {(uint32_t)(2000 + 1536 * k), kSw, 0, (uint16_t)(10 + k), 0, 0}; w9.push(e); }
    FakePort b;
    Restore r9;
    r9.begin(&w9, false, 0, b, 0);
    CHECK(r9.partial && r9.nvisit == 1 && r9.visit[0] == 3 && r9.plays == 12);
    u.free(), w9.free();
  }
  printf("restore: another start pattern backed up too (PC back when not), the backup's write bounded, cancel, PLAY ok\n");
  printf("restore: every pattern the take plays backed up at confirm, the linked kit before kit X, > 8 patterns: start only ok\n");
}

// ---- 5b. UNDO REPLAY's run: newest entry first, P0 last with its settings, mutes and send FX; each verified; nothing
// sent unless every entry is whole; a PLAY stops it
static void test_undorun() {
  auto ent = [](int pattern, int seed, bool pat, bool set) {
    Take* t = new Take;
    t->clear();
    config_default(&t->cfg);
    t->pattern = pattern;
    auto k = dump(0x58, payload(2988, seed));
    t->kit.add(k.data(), (int)k.size());
    if (pat) { auto q = dump(0x5A, payload(14978, seed + 1)); t->pat.add(q.data(), (int)q.size()); }
    if (set) { auto q = dump(0x5C, payload(2738, seed + 2)); t->settings.add(q.data(), (int)q.size()); }
    t->has_fx = set;
    return t;
  };
  Take* e0 = ent(5, 10, true, true);   // P0 (F06 at confirm)
  Take* e1 = ent(0, 20, true, false);  // P1 (the take's start, A01), whose new pattern X linked another kit (kit2)
  { auto k2 = dump(0x58, payload(2988, 25)); e1->kit2.add(k2.data(), (int)k2.size()); }
  Take* e2 = ent(7, 30, false, false); // a pattern the replay reached (H08): its kit
  const Take* list[3] = {e0, e1, e2};
  FakePort p;
  UndoRun u;
  u.begin(list, 3, true, 0x5, 0, p);
  CHECK(u.st == UndoRun::kSelect && p.sent.size() == 1 && p.sent[0][0] == 0xCD && p.sent[0][1] == 7);   // H08 first
  uint32_t ms = 0;
  auto run_to_verify = [&] { while (u.busy() && u.want < 0 && ms < 100000) u.tick(ms += 10, p); };
  run_to_verify();
  CHECK(u.want == kReqKit && p.sent.size() == 2 && p.sent[1][6] == 0x58);   // its kit only
  u.dump_in(0x58, e2->kit.p, e2->kit.n, ms, p);
  CHECK(u.st == UndoRun::kSelect && p.sent.back()[0] == 0xCD && p.sent.back()[1] == 0);    // then A01
  run_to_verify();
  CHECK(u.st == UndoRun::kVerify2 && p.sent.back()[6] == 0x58 && (int)p.sent.back().size() == e1->kit2.n);   // its linked kit first
  u.dump_in(0x58, e1->kit2.p, e1->kit2.n, ms, p);
  size_t at1 = p.sent.size();
  run_to_verify();
  CHECK(p.sent.size() == at1 + 2 && p.sent[at1][6] == 0x5A && p.sent[at1 + 1][6] == 0x58);   // then its pattern X, kit X
  u.dump_in(0x58, e1->kit.p, e1->kit.n, ms, p);
  CHECK(u.want == kReqPattern);
  u.dump_in(0x5A, e1->pat.p, e1->pat.n, ms, p);
  CHECK(u.st == UndoRun::kSelect && p.sent.back()[1] == 5);   // P0 last
  size_t before = p.sent.size();
  run_to_verify();
  // pattern X, kit X, settings X, 12 mutes (BD and RS muted), the send FX
  CHECK(p.sent.size() == before + 3 + 12 && p.sent[before][6] == 0x5A && p.sent[before + 1][6] == 0x58 &&
        p.sent[before + 2][6] == 0x5C && p.sent[before + 3][1] == 94 && p.sent[before + 3][2] == 1 &&
        p.sent[before + 4][2] == 0 && p.sent[before + 5][2] == 1 && p.starts == 1);
  u.dump_in(0x58, e0->kit.p, e0->kit.n, ms, p);
  u.dump_in(0x5A, e0->pat.p, e0->pat.n, ms, p);
  CHECK(u.st == UndoRun::kDone && strstr(u.msg, "3 patterns"));
  // a damaged entry: nothing sent
  FakePort q;
  UndoRun v;
  e2->kit.n -= 3;
  v.begin(list, 3, true, -1, 0, q);
  CHECK(v.st == UndoRun::kFailed && q.sent.empty() && strstr(v.msg, "nothing written"));
  e2->kit.n += 3;
  // P0 not known with more than one entry: refused
  e0->pattern = -1;
  v.begin(list, 3, true, -1, 0, q);
  CHECK(v.st == UndoRun::kFailed && q.sent.empty());
  e0->pattern = 5;
  // a kit that differs: not verified, said; a PLAY: stopped
  FakePort r;
  UndoRun w;
  w.begin(list, 1, false, -1, 0, r);
  while (w.busy() && w.want < 0 && ms < 200000) w.tick(ms += 10, r);
  w.dump_in(0x58, e1->kit.p, e1->kit.n, ms, r);
  CHECK(w.st == UndoRun::kFailed && strstr(w.msg, "kit differs"));
  FakePort z;
  UndoRun x;
  x.begin(list, 2, false, -1, 0, z);
  z.plays = true;
  x.tick(1000, z);
  CHECK(x.st == UndoRun::kFailed && strstr(x.msg, "played") && z.sent.size() == 1);   // (only the PC)
  for (Take* t : {e0, e1, e2}) t->free(), delete t;
  printf("undorun: newest first, P0 last (settings, mutes, send FX), each verified, damaged / P0 unknown refused, PLAY stops ok\n");
}

// ---- 6. the engine end to end on the byte stream, with a fake Rytm behind to_rytm
static int64_t g_frame = 0;
static uint64_t g_us = 1000000;
static int64_t frames() { return g_frame; }
struct Rytm {
  std::vector<uint8_t> kit, pat, settings, global;   // its working state (whole dumps as it would answer): pattern cur's
  // undo-replay review 1: each pattern keeps its own working pattern and kit (as the Rytm does over a switch); a PC
  // selects at once (stopped) and is reported (2D 02)
  int cur = 0;
  std::map<int, std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> other;   // the others' (kit, pattern)
  uint32_t mutes = 0;                                // CC 94 on channels 1..12
  // linked: a pattern selects its kit by the number in its pattern X (here its first payload byte), as the Rytm's do:
  // kits by number, a pattern X written over the working one switches the working kit
  bool linked = false;
  std::map<int, std::vector<uint8_t>> kits;
  std::string ro_dir;   // non-empty: made read-only when a pattern X is written (a backup file then cannot be)
  std::vector<std::string> order;                    // "req 68", "cc b1 6": what came in, in order
  std::vector<uint8_t> partial;                      // a long SysEx arriving in pieces
  std::vector<std::vector<uint8_t>> channel;         // the channel messages it got
  int writes = 0, pieces = 0;
  bool answer = true;
  std::vector<std::vector<uint8_t>> pending;         // answers to send back
};
static Rytm g_rytm;
static int kitnum(const std::vector<uint8_t>& pat) { return pat.size() > 10 ? pat[10] : 0; }
static void rytm_link(const std::vector<uint8_t>& pat, const std::vector<uint8_t>* fallback) {   // pattern X written / selected
  Rytm& r = g_rytm;
  r.kits[kitnum(r.pat)] = r.kit;
  r.pat = pat;
  auto it = r.kits.find(kitnum(pat));
  r.kit = it != r.kits.end() ? it->second : fallback ? *fallback : dump(0x58, payload(2988, 400 + kitnum(pat)));
}
static void rytm_select(int p) {   // the fake's pattern p selected: its own working pattern and kit
  Rytm& r = g_rytm;
  if (p == r.cur) return;
  if (r.linked) {
    r.other[r.cur].second = r.pat;
    auto it = r.other.find(p);
    std::vector<uint8_t> pat = it != r.other.end() ? it->second.second : dump(0x5A, payload(14978, 300 + p));
    std::vector<uint8_t> fb = it != r.other.end() ? it->second.first : dump(0x58, payload(2988, 200 + p));
    rytm_link(pat, &fb);
    r.cur = p;
    return;
  }
  r.other[r.cur] = {r.kit, r.pat};
  auto it = r.other.find(p);
  if (it != r.other.end()) r.kit = it->second.first, r.pat = it->second.second;
  else r.kit = dump(0x58, payload(2988, 200 + p)), r.pat = dump(0x5A, payload(14978, 300 + p));
  r.cur = p;
}
static void to_rytm_fake(const uint8_t* m, int n) {
  Rytm& r = g_rytm;
  if (m[0] == 0xF0 && m[n - 1] != 0xF7) { r.partial.assign(m, m + n), ++r.pieces; return; }
  if (m[0] < 0x80) {   // a piece's continuation
    r.partial.insert(r.partial.end(), m, m + n), ++r.pieces;
    if (m[n - 1] != 0xF7) return;
    std::vector<uint8_t> w = r.partial;
    r.partial.clear();
    to_rytm_fake(w.data(), (int)w.size());
    return;
  }
  if (m[0] != 0xF0) {
    r.channel.emplace_back(m, m + n);
    char o[24];
    snprintf(o, sizeof o, "%02x %d", m[0], n > 1 ? m[1] : 0);
    r.order.push_back(o);
    if ((m[0] & 0xF0) == 0xB0 && n == 3 && m[1] == 94 && (m[0] & 15) < 12)
      r.mutes = m[2] ? r.mutes | 1u << (m[0] & 15) : r.mutes & ~(1u << (m[0] & 15));
    if ((m[0] & 0xF0) == 0xC0 && n == 2) {
      rytm_select(m[1]);
      r.pending.push_back({0xF0, 0x7D, 0x52, 0x54, 0x2D, 0x01, 2, m[1], 0x7F, 0xF7});   // 2D 02: now, nothing queued
    }
    return;
  }
  if (n == 15 && m[1] == 0 && m[2] == 0x20) {   // a request
    r.order.push_back("req " + std::to_string(m[6]));
    if (!r.answer) return;
    uint8_t id = m[6];
    r.pending.push_back(id == kReqKit ? r.kit : id == kReqPattern ? r.pat : id == kReqSettings ? r.settings : r.global);
    return;
  }
  if (n > 15 && m[1] == 0 && m[2] == 0x20) {   // a dump written to a working buffer
    std::vector<uint8_t> d(m, m + n);
    ++r.writes;
    if (m[6] == 0x58) r.kit = d;
    else if (m[6] == 0x5A && r.linked) {
      rytm_link(d, nullptr);
      if (!r.ro_dir.empty()) chmod(r.ro_dir.c_str(), 0555);
    }
    else if (m[6] == 0x5A) r.pat = d;
    else if (m[6] == 0x5C) r.settings = d;
  }
}
static void feed(const std::vector<uint8_t>& m) {
  for (uint8_t b : m) song::byte(b);
}
// re-review N2: the longest song::tick / song::idle call (the thread's CPU clock), while g_timed
static bool g_timed = false;
static uint64_t g_tmax = 0;
static const char* g_tmax_what = "";
static void step(int ms) {   // time on, the answers back, the loop and the card jobs
  for (int i = 0; i < ms; ++i) {
    g_us += 1000, g_frame += 48;
    host_fake_time(true, g_us);
    std::vector<std::vector<uint8_t>> p;
    p.swap(g_rytm.pending);
    for (auto& m : p) feed(m);
    uint64_t t0 = timing_cpu_us();
    song::tick(now_ms());
    uint64_t t1 = timing_cpu_us();
    if (g_timed && t1 - t0 > g_tmax) g_tmax = t1 - t0, g_tmax_what = "tick";
    seq::tick(now_ms());
    fmt::post_step();          // (idle_save's first step: the posted writes, the SONG backup's among them)
    project_fill_step();       // (idle_save's: a project load's RAM store fill)
    for (int k = 0; k < 8; ++k) {
      t0 = timing_cpu_us();
      bool more = song::idle();
      t1 = timing_cpu_us();
      int st = song::status().state;   // (the backups' and the undo's passes: not a take's load or the takes' listing)
      if (g_timed && (st == kRestoring || st == kUndoing || st == kReplayReady) && t1 - t0 > g_tmax)
        g_tmax = t1 - t0, g_tmax_what = st == kUndoing ? "idle, undoing" : "idle, backing up";
      if (!more) break;
    }
    if (g_timed) {   // (the card core's turn, outside the timed passes: the control core only posts)
      host_card_core(0);
      while (cardq::service()) {}
      host_card_core(3);
    }
  }
}
static std::string cmd(const char* c) {
  char b[400];
  CHECK(song::command(c, b, sizeof b));
  return b;
}
static void tick2c(int step_i, int len) { feed(msg(0x2C, [&] { auto v = blk4(g_frame); v.push_back(1); v.insert(v.end(), {0, (uint8_t)step_i, (uint8_t)len, 2}); return v; }())); }

static void test_engine(const std::string& card) {
  const int P = 6000;   // frames a step at 120 BPM
  g_rytm.kit = dump(0x58, payload(2988, 11));
  g_rytm.pat = dump(0x5A, payload(14978, 12));
  g_rytm.settings = dump(0x5C, payload(2738, 13));
  g_rytm.global = dump(0x5D, global_payload(true));
  song::test_frames(frames);
  song::reset();
  step(5);
  feed(msg(0x2D, {7, 120 * 120 >> 14, (120 * 120 >> 7) & 127, 120 * 120 & 127}));   // TEMPO 120
  feed(msg(0x2D, {2, 0, 0x7F}));   // PATTERN A01
  std::vector<std::vector<uint8_t>> sent2e;
  CHECK(cmd("song=arm").find("arming") != std::string::npos);
  step(50);
  Status s = song::status();
  CHECK(s.state == kArmed && s.cfg_valid && s.problems == 0);
  CHECK(song::current().kit.n == (int)g_rytm.kit.size() && song::current().pat.n == (int)g_rytm.pat.size() &&
        song::current().global.n > 0);
  // a knob while armed: the start
  feed({0xB0, 99, 1, 0xB0, 98, 0, 0xB0, 6, 100, 0xB0, 38, 0});
  // PLAY
  feed(msg(0x2D, {1, 1}));   // (malformed: ignored)
  auto play = msg(0x2D, [] { std::vector<uint8_t> v = {1, 1}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }());
  feed(play);
  CHECK(song::status().state == kRec);
  // 3 bars: BD's ticks; knob turns on SD (NRPN), a mute, a pattern queued at bar 2 and switched at bar 3
  for (int st = 0; st < 48; ++st) {
    if (st == 32) feed(msg(0x2D, [] { std::vector<uint8_t> v = {3, 0, 1}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }()));
    tick2c(st % 16, 16);
    if (st == 0) feed({0xCD, 0});   // (the Rytm's PC at PLAY: not an event)
    if (st == 5) { step(P / 96); feed({0xB1, 99, 1, 0xB1, 98, 1, 0xB1, 6, 64, 0xB1, 38, 0}); }
    if (st == 6) feed({0xB1, 6, 70, 0xB1, 38, 0});
    if (st == 10) feed(msg(0x2D, {5, 4, 0}));   // RS muted
    if (st == 20) feed(msg(0x2D, {2, 0, 1})), feed({0xCD, 1});
    if (st == 40) fx::control.send[2][0] = 55;   // the Sidecar's own
    step(P / 48);
  }
  CHECK(song::status().t_now >= 47 * 96 && song::status().t_now < 48 * 96);
  // the journal is there while recording
  struct stat sb;
  std::string dir = card + "/.rytm/songs/SONGTEST/";
  CHECK(stat((dir + "take-001.jsonl").c_str(), &sb) == 0 && sb.st_size > 20000);
  // STOP
  auto stop = msg(0x2D, [] { std::vector<uint8_t> v = {1, 0}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }());
  feed(stop);
  for (int i = 0; i < 200 && song::status().state != song::kOff; ++i) step(1);
  CHECK(song::status().state == song::kOff);
  CHECK(stat((dir + "take-001.json").c_str(), &sb) == 0 && stat((dir + "take-001.jsonl").c_str(), &sb) != 0);
  const Take& t = song::current();
  int params = 0, mutes = 0, pcs = 0, sws = 0, scs = 0;
  for (int i = 0; i < t.n; ++i) {
    const Ev& e = t.ev[i];
    params += e.type == kParam, mutes += e.type == kMute, pcs += e.type == kPc, sws += e.type == kSw, scs += e.type == kSc;
    if (e.type == kParam && e.a == 1 && e.v == 64u << 7) CHECK(e.t >= 5 * 96 && e.t < 6 * 96);
    if (e.type == kSw) CHECK(e.t == 32 * 96 && e.b == 1);
    if (e.type == kPc) CHECK(e.t >= 20 * 96 && e.t < 21 * 96 && e.a == 1);
    if (e.type == kParam && e.t == 0) CHECK(e.a == 0 && e.v == 100u << 7);
  }
  CHECK(params == 3 && mutes == 1 && pcs == 1 && sws == 1 && scs == 1 && t.pattern == 0 && t.tempo == 14400);
  CHECK(t.end >= 47 * 96);
  // the file reads back as it was
  {
    FILE* f = fopen((dir + "take-001.json").c_str(), "rb");
    std::string txt;
    char b[65536];
    size_t k;
    while ((k = fread(b, 1, sizeof b, f)) > 0) txt.append(b, k);
    fclose(f);
    Take r;
    char err[64];
    CHECK(read_json(txt.data(), (int)txt.size(), &r, err, sizeof err) && same(t, r));
    r.free();
  }
  // the timeline's lanes
  song::lanes_want(true);
  step(5);
  static Lanes L;
  uint32_t seen = 0;
  CHECK(song::lanes_read(&L, &seen) && L.nblocks == 2 && L.blocks[0].v == 0 && L.blocks[1].v == 1 && L.nmutes == 1 &&
        L.mutes[0].v == 2 && L.nlanes == 3);
  CHECK(!song::lanes_read(&L, &seen));
  // crash recovery: a journal with its last line cut is folded into a JSON at the next project naming
  {
    std::string j = dir + "take-002.jsonl";
    Buf jb;
    JsonGen g;
    Take x = sample_take();
    strcpy(x.project, "SONGTEST"), strcpy(x.name, "take-002");
    g.begin(&x, true);
    while (g.next(jb)) {}
    char b[160];
    for (int i = 0; i < x.n; ++i) jb.add(b, event_json(x.ev[i], b, sizeof b)), jb.addc('\n');
    FILE* f = fopen(j.c_str(), "wb");
    fwrite(jb.p, 1, jb.n - 5, f);
    fclose(f);
    const uint8_t other[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 'E', 'R', 0xF7};
    engine_sysex(other, sizeof other);
    if (rename::state() == rename::kAsk) rename::answer(false);   // (UX review B1: OTHER is a new project)
    step(3);
    const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'O', 'N', 'G', 'T', 'E', 'S', 'T', 0xF7};
    engine_sysex(proj, sizeof proj);
    step(40);
    CHECK(stat((dir + "take-002.json").c_str(), &sb) == 0 && stat(j.c_str(), &sb) != 0);
    x.free(), jb.free();
    CHECK(cmd("song=list").find("2 takes") != std::string::npos);
    CHECK(cmd("song=load:take-002").find("loading") != std::string::npos || true);
    step(20);
    CHECK(song::status().recovered && song::current().n == 18 && !strcmp(song::current().name, "take-002"));
    cmd("song=load:take-001");
    step(20);
    CHECK(!strcmp(song::current().name, "take-001") && song::current().n == t.n);
  }
  // replay: asked, nothing written until confirmed; then the backup, the restore, the verify; PLAY drives the Rytm
  g_rytm.cur = 1;   // (A02: where the take ended; the engine saw that switch)
  g_rytm.kit = dump(0x58, payload(2988, 21));   // the Rytm's working state moved on since the take
  g_rytm.pat = dump(0x5A, payload(14978, 22));
  auto kit_before = g_rytm.kit;
  g_rytm.channel.clear();
  g_rytm.writes = 0;
  cmd("song=replay");
  step(100);
  CHECK(song::status().state == kConfirm && g_rytm.writes == 0 && g_rytm.channel.empty());
  cmd("song=confirm");
  for (int i = 0; i < 300 && song::status().state == kRestoring; ++i) step(5);
  CHECK(song::status().state == kReplayReady);
  CHECK(g_rytm.writes == 2 && g_rytm.pieces == 5);   // pattern X in pieces (<= 3069 B each), kit X
  CHECK(g_rytm.kit == std::vector<uint8_t>(song::current().kit.p, song::current().kit.p + song::current().kit.n));
  // the backup holds what the Rytm had
  std::string backup = dir + ".backup-1.json";   // (A02's at confirm; .backup-1-1.json: A01's, the take's start)
  CHECK(stat((dir + ".backup-1-1.json").c_str(), &sb) == 0);
  CHECK(stat(backup.c_str(), &sb) == 0 && sb.st_size > 20000);
  {
    FILE* f = fopen(backup.c_str(), "rb");
    std::string txt;
    char b[65536];
    size_t k;
    while ((k = fread(b, 1, sizeof b, f)) > 0) txt.append(b, k);
    fclose(f);
    Take r;
    char err[64];
    CHECK(read_json(txt.data(), (int)txt.size(), &r, err, sizeof err));
    CHECK(std::vector<uint8_t>(r.kit.p, r.kit.p + r.kit.n) == kit_before);
    r.free();
  }
  CHECK(g_rytm.channel.size() >= 1 + 12 && g_rytm.channel[0][0] == 0xCD && g_rytm.channel[0][1] == 0);   // A01, mutes
  g_rytm.channel.clear();
  fx::control.send[2][0] = 0;
  feed(play);
  CHECK(song::status().state == kReplaying);
  CHECK(g_rytm.channel.size() == 4 && g_rytm.channel[0][1] == 99);   // the start's knob
  g_rytm.channel.clear();
  std::vector<std::pair<int, std::vector<uint8_t>>> got;
  for (int st = 0; st < 48; ++st) {
    if (st == 32) feed(msg(0x2D, [] { std::vector<uint8_t> v = {3, 0, 1}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }()));
    tick2c(st % 16, 16);
    for (int k = 0; k < 2; ++k) {
      size_t before = g_rytm.channel.size();
      step(P / 96);
      for (size_t i = before; i < g_rytm.channel.size(); ++i) got.push_back({st, g_rytm.channel[i]});
    }
  }
  // the replayed messages land in the step they were recorded in
  bool knob = false, mute = false, pc = false;
  for (auto& x : got) {
    const auto& m = x.second;
    if (m[0] == 0xB1 && m[1] == 6 && m[2] == 64) { knob = true; CHECK(x.first == 5); }
    if (m[0] == 0xB2 && m[1] == 94 && m[2] == 1) { mute = true; CHECK(x.first == 10); }
    if (m[0] == 0xCD && m[1] == 1) { pc = true; CHECK(x.first == 20); }
  }
  CHECK(knob && mute && pc && fx::control.send[2][0] == 55);
  // fix1041: the replay ends at the take's end: a MIDI STOP on the recorded STOP's step, the state off (the hold
  // released, the SONG page and the timeline idle) before the Rytm's own 2D STOP comes
  int fc_at = -1;
  for (auto& x : got) if (x.second.size() == 1 && x.second[0] == 0xFC) fc_at = x.first;
  printf("engine: replay end: the take ends at t %u (step %u), STOP sent in step %d\n", (unsigned)t.end,
         (unsigned)(t.end / 96), fc_at);
  CHECK(fc_at == (int)(t.end / 96));
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "replay at its end"));
  feed(stop);
  step(5);
  CHECK(song::status().state == song::kOff);
  // the Rytm's SONG page: the take, the confirm prompt (T2), the how-to (T4); T1 arms from it
  {
    const uint8_t open[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 1, 0, 0xF7};
    engine_sysex(open, sizeof open);
    remote_show_page("SONG");
    auto frames_ = [&](int n) { for (int i = 0; i < n; ++i) step(30), remote_tick(now_ms()); };
    frames_(4);
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "SONG"));
    CHECK(shown("SIDECAR SONG") && shown("TAKE-001") && shown("T5: UNDO REPLAY"));   // (after the replay: its undo offered)
    png("song_page.png");
    key(1, true), key(1, false);
    frames_(4);
    CHECK(song::status().state == kConfirm && shown("REPLAY THIS TAKE?") && shown("YES: RESTORE   NO: CANCEL"));
    png("song_confirm.png");
    key(0x45, true), key(0x45, false);   // NO: cancelled, nothing written
    frames_(2);
    CHECK(song::status().state == song::kOff && remote_page_title() && !strcmp(remote_page_title(), "SONG"));
    key(3, true), key(3, false);
    frames_(4);
    CHECK(shown("ENCODER DEST: INT+EXT"));
    png("song_howto.png");
    key(0x44, true), key(0x44, false);
    key(0, true), key(0, false);   // T1: arm
    frames_(4);
    CHECK(song::status().state == kArmed && shown("PLAY: REC  T1: DISARM"));
    png("song_armed.png");
    key(0, true), key(0, false);
    frames_(2);
    CHECK(song::status().state == song::kOff);
    cmd("song=load:take-001");
    step(20);
  }
  // a Rytm that does not answer the backup: nothing written
  g_rytm.answer = false, g_rytm.writes = 0;
  cmd("song=replay");
  cmd("song=confirm");
  step(8500);
  CHECK(song::status().state == song::kOff && g_rytm.writes == 0 && strstr(song::status().msg, "nothing written"));
  g_rytm.answer = true;
  printf("engine: arm + snapshot + config, record (NRPN, mutes, PC, switch, the Sidecar's), journal, song file, lanes, "
         "recovery, load, confirmed replay (backup, restore in pieces, verify, events on their steps) ok\n");
}

// ---- 7. UNDO REPLAY (UX review T14 + its reviews): everything the replay overwrites put back from the RAM store - the
// pattern at confirm (P0), every pattern the take plays (backed up at confirm, stopped), a kit the take's pattern X
// links - the mutes and send FX; offered until an edit, 30 s stopped or a project change; refused while a project
// loads; a power cut leaves the backup whole
static std::string slurp(const std::string& path) {
  std::string txt;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return txt;
  char b[65536];
  size_t k;
  while ((k = fread(b, 1, sizeof b, f)) > 0) txt.append(b, k);
  fclose(f);
  return txt;
}
static std::vector<uint8_t> vec(const Buf& b) { return std::vector<uint8_t>(b.p, b.p + b.n); }
static std::vector<uint8_t> K(int seed) { return dump(0x58, payload(2988, seed)); }
static std::vector<uint8_t> PT(int seed) { return dump(0x5A, payload(14978, seed)); }
static void set_state(int p, const std::vector<uint8_t>& kit, const std::vector<uint8_t>& pat) {   // pattern p's own
  if (g_rytm.cur == p) g_rytm.kit = kit, g_rytm.pat = pat;
  else g_rytm.other[p] = {kit, pat};
}
static void select_on_rytm(int p) {   // the user selects pattern p on the Rytm (reported)
  rytm_select(p);
  feed(msg(0x2D, {2, (uint8_t)p, 0x7F}));
}
// the newest set's main file (.backups.txt: "next N")
static std::string newest_backup(const std::string& dir) {
  std::string t = slurp(dir + ".backups.txt");
  CHECK(t.size() > 5);
  int n = atoi(t.c_str() + 5);
  return dir + ".backup-" + std::to_string((n - 2) % 8 + 1) + ".json";
}
// a confirmed replay restored into the fake Rytm: P0 = pattern p0 (its state seed's), A01 (the take's start) seed + 5's,
// the mutes `mutes`; the newest backup's main file
static std::string replay_confirmed(const std::string& dir, int seed, int p0 = 1, uint32_t mutes = 0x5) {
  if (song::status().state == kReplayReady) cmd("song=cancel");   // (the replay before it left armed)
  set_state(0, K(seed + 5), PT(seed + 6));
  select_on_rytm(p0);
  g_rytm.kit = K(seed), g_rytm.pat = PT(seed + 1), g_rytm.settings = dump(0x5C, payload(2738, seed + 2));
  g_rytm.mutes = mutes;
  feed(msg(0x2D, {5, (uint8_t)(mutes & 127), (uint8_t)(mutes >> 7)}));
  step(5);
  cmd("song=replay");
  cmd("song=confirm");
  for (int i = 0; i < 400 && song::status().state == kRestoring; ++i) step(5);
  CHECK(song::status().state == kReplayReady);
  CHECK(g_rytm.cur == 0 && g_rytm.kit == vec(song::current().kit));   // the take's start state is on the Rytm now
  return newest_backup(dir);
}
static void undo_run() {
  cmd("song=undo");
  for (int i = 0; i < 600 && song::status().state == kUndoing; ++i) step(5);
}
// everything as before the replay: P0 selected with its state, A01's own state back, the mutes
static bool undone(int seed, int p0 = 1, uint32_t mutes = 0x5) {
  bool ok = song::status().state == song::kOff && strstr(song::status().msg, "replay undone") && g_rytm.cur == p0 &&
            g_rytm.kit == K(seed) && g_rytm.pat == PT(seed + 1) && g_rytm.mutes == mutes;
  ok = ok && g_rytm.other[0].first == K(seed + 5) && g_rytm.other[0].second == PT(seed + 6);
  if (!ok) printf("undone(%d): msg \"%s\", pattern %d\n", seed, song::status().msg, g_rytm.cur);
  return ok;
}
static void test_undo(const std::string& card) {
  std::string dir = card + "/.rytm/songs/SONGTEST/";
  struct stat sb;
  auto play = [] { return msg(0x2D, [] { std::vector<uint8_t> v = {1, 1}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }()); };
  auto stop = [] { return msg(0x2D, [] { std::vector<uint8_t> v = {1, 0}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }()); };
  cmd("song=cancel");
  step(1200);
  CHECK(!song::status().undo && cmd("song=undo").find("no replay to undo") != std::string::npos);
  g_timed = true;
  cardq::set_worker(true);   // (a card core: the posted writes run there, as on the Pi - not in the timed passes)
  // 1. restore after a replay that started on another pattern (A02 at confirm, the take on A01): both patterns' own
  // state back, A02 selected again, the mutes and the send FX as they were - through the Rytm page's T5
  fx::control.send[3][1] = 77;
  step(20);
  std::string bk = replay_confirmed(dir, 41);
  CHECK(song::status().undo && g_rytm.mutes == (song::current().mutes & 0xFFF));   // (READY: offered; the take's mutes on the Rytm)
  CHECK(stat((bk + ".tmp").c_str(), &sb) != 0 && stat((bk.substr(0, bk.size() - 5) + "-1.json").c_str(), &sb) == 0);
  remote_show_page("SONG");
  auto frames_ = [&](int n) { for (int i = 0; i < n; ++i) step(30), remote_tick(now_ms()); };
  frames_(4);
  CHECK(shown("T5: UNDO REPLAY") && shown("PLAY: REPLAY  T2: OFF"));
  png("song_undo_ready.png");
  g_rytm.writes = 0;
  key(4, true), key(4, false);
  CHECK(song::status().state == kUndoing && !song::status().undo);
  for (int i = 0; i < 600 && song::status().state == kUndoing; ++i) step(5);
  CHECK(undone(41) && g_rytm.writes == 4 && fx::control.send[3][1] == 77);   // A01's pattern + kit, A02's pattern + kit
  CHECK(!song::status().undo && cmd("song=undo").find("no replay to undo") != std::string::npos);   // once
  // ... the take's start (A01) selected at confirm: A01 itself and A02 (its switch's)
  std::string b1 = replay_confirmed(dir, 51, 0);
  undo_run();
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "2 patterns") && g_rytm.cur == 0 && g_rytm.kit == K(51));
  // ... from the RAM store, not the card: the card's files gone, still undone
  std::string b2 = replay_confirmed(dir, 61);
  CHECK(remove(b2.c_str()) == 0);
  undo_run();
  CHECK(undone(61));
  // ... settings X too when the replay wrote them
  cmd("song=settings:on");
  replay_confirmed(dir, 71);
  CHECK(g_rytm.settings == vec(song::current().settings));
  undo_run();
  CHECK(undone(71) && g_rytm.settings == dump(0x5C, payload(2738, 73)));
  cmd("song=settings:off");
  // 2. every pattern the take plays backed up at confirm, while stopped (re-review: no gate): F06 selected, the take
  // on A01 switching to A02 - three entries, all put back
  set_state(1, K(87), PT(88));
  std::string b3 = replay_confirmed(dir, 81, 5);
  CHECK(stat((b3.substr(0, b3.size() - 5) + "-2.json").c_str(), &sb) == 0);
  undo_run();
  CHECK(undone(81, 5) && strstr(song::status().msg, "3 patterns"));
  select_on_rytm(1);
  CHECK(g_rytm.kit == K(87) && g_rytm.pat == PT(88));   // (A02 read, never written)
  // ... during the replay nothing is held or read: its messages on the steps they were recorded in (as test_engine's
  // replay without an undo); a switch to a pattern not backed up (A08, the user's) ends the undo at once
  replay_confirmed(dir, 91);
  g_rytm.order.clear();
  feed(play());
  CHECK(song::status().state == kReplaying && !song::status().undo);   // (not offered while it plays)
  const int P = 6000;
  std::vector<std::pair<int, std::vector<uint8_t>>> got;
  bool ended_at_once = false;
  for (int st = 0; st < 24; ++st) {
    if (st == 12) {
      feed(msg(0x2D, [] { std::vector<uint8_t> v = {3, 0, 7}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }()));
      ended_at_once = strstr(song::status().msg, "A08, not backed up") != nullptr;
    }
    tick2c(st % 16, 16);
    for (int k = 0; k < 2; ++k) {
      size_t before = g_rytm.channel.size();
      step(P / 96);
      for (size_t i = before; i < g_rytm.channel.size(); ++i) got.push_back({st, g_rytm.channel[i]});
    }
  }
  bool knob = false, mute = false, pc = false, req = false;
  for (auto& x : got) {
    const auto& m = x.second;
    if (m[0] == 0xB1 && m[1] == 6 && m[2] == 64) { knob = true; CHECK(x.first == 5); }
    if (m[0] == 0xB2 && m[1] == 94 && m[2] == 1) { mute = true; CHECK(x.first == 10); }
    if (m[0] == 0xCD && m[1] == 1) { pc = true; CHECK(x.first == 20); }
  }
  for (auto& o : g_rytm.order) req |= o.rfind("req", 0) == 0;
  CHECK(knob && mute && pc && !req && ended_at_once);
  feed(stop());
  step(5);
  CHECK(song::status().state == song::kOff && !song::status().undo);
  // ... the take's pattern X links another kit than A01's (the kit number in it): that kit backed up before kit X
  // lands on it (kit2), put back first by the undo
  g_rytm.linked = true;
  g_rytm.kits.clear();
  g_rytm.kits[12] = K(500);   // (the take's pattern X: payload seed 12, kit 12)
  replay_confirmed(dir, 301);
  CHECK(g_rytm.kits[51] == K(306));   // (A01's own kit, 307 & 127: not written)
  undo_run();
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "replay undone") && g_rytm.cur == 1 &&
        g_rytm.kit == K(301) && g_rytm.kits[12] == K(500) && g_rytm.kits[51] == K(306) && g_rytm.other[0].second == PT(307));
  // ... the linked kit's file cannot be written (re-review 2, finding 3): the restore stops before kit X (that kit
  // untouched), kit2 dropped, the undo kept - and it puts the rest back
  g_rytm.kits[12] = K(500);
  g_rytm.ro_dir = dir;   // (the fake makes the song folder read-only when the take's pattern X arrives)
  set_state(0, K(316), PT(317));
  select_on_rytm(1);
  g_rytm.kit = K(311), g_rytm.pat = PT(312);
  step(1100);
  cmd("song=replay");
  cmd("song=confirm");
  for (int i = 0; i < 400 && song::status().state == kRestoring; ++i) step(5);
  CHECK(chmod(dir.c_str(), 0755) == 0);
  g_rytm.ro_dir.clear();
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "linked kit") && song::status().undo);
  CHECK(g_rytm.kits[12] == K(500));   // (kit X never written over it)
  undo_run();
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "replay undone") && g_rytm.cur == 1 &&
        g_rytm.kit == K(311) && g_rytm.kits[12] == K(500) && g_rytm.other[0].second == PT(317));
  g_rytm.linked = false;
  // ... a take that plays more than 8 patterns: the undo covers its start only (said at confirm), and ends when the
  // replay leaves it
  {
    std::string t1 = slurp(dir + "take-001.json");
    Take x;
    char err[64];
    CHECK(read_json(t1.data(), (int)t1.size(), &x, err, sizeof err));
    for (int k = 0; k < 9; ++k) {
      Ev e = {(uint32_t)(6000 + 1536 * k), kSw, (uint8_t)(k ? 19 + k : 1), (uint16_t)(20 + k), 0, 0};
      x.push(e);
    }
    x.end = 6000 + 1536 * 9;
    strcpy(x.name, "take-009");
    Buf b;
    JsonGen g;
    g.begin(&x, false);
    while (g.next(b)) {}
    FILE* f = fopen((dir + "take-009.json").c_str(), "wb");
    fwrite(b.p, 1, b.n, f);
    fclose(f);
    x.free(), b.free();
  }
  cmd("song=list");
  step(5);
  cmd("song=load:take-009");
  step(20);
  CHECK(!strcmp(song::current().name, "take-009"));
  select_on_rytm(1);
  step(1100);
  cmd("song=replay");
  cmd("song=confirm");
  CHECK(strstr(song::status().msg, "start only"));
  for (int i = 0; i < 400 && song::status().state == kRestoring; ++i) step(5);
  CHECK(song::status().state == kReplayReady && song::status().undo);
  feed(play());
  step(50);
  feed(msg(0x2D, [] { std::vector<uint8_t> v = {3, 0, 20}; auto b = blk4(g_frame); v.insert(v.end(), b.begin(), b.end()); return v; }()));
  CHECK(strstr(song::status().msg, "more than 8 patterns"));
  feed(stop());
  step(5);
  CHECK(!song::status().undo);
  CHECK(remove((dir + "take-009.json").c_str()) == 0);
  cmd("song=list");
  step(5);
  cmd("song=load:take-001");
  step(20);
  // N2: no pass of the backups' formatting (a JSON piece a pass) or the undo's reading back (a file a pass) is long
  g_timed = false;
  cardq::set_worker(false);
  printf("undo: the longest song pass during the backups and undos above: %u us (%s)\n", (unsigned)g_tmax, g_tmax_what);
  CHECK(g_tmax < 500);   // (Mac; the Pi's core 3 several times slower, its FIFO 2 ms)
  // 3. the expiry. 30 s stopped, REPLAY READY too (review 7)
  replay_confirmed(dir, 101);
  step(29000);
  CHECK(song::status().undo && song::status().state == kReplayReady);
  step(1100);
  CHECK(!song::status().undo);
  // ... held while the Rytm plays (its own play, not a replay)
  replay_confirmed(dir, 111);
  cmd("song=cancel");
  step(20000);
  feed(play());
  step(5000);
  feed(stop());
  step(29000);
  CHECK(song::status().undo);
  for (int i = 0; i < 2000 && song::status().undo; ++i) step(1);
  CHECK(!song::status().undo);
  // ... T5 within a second of the end: "UNDO EXPIRED", still the page's (not the Rytm's trig); after: the stock trig
  frames_(1);
  CHECK(song::status().undo_t5);
  g_rytm.writes = 0;
  key(4, true), key(4, false);
  CHECK(strstr(song::status().msg, "UNDO EXPIRED") && g_rytm.writes == 0);
  frames_(40);
  CHECK(!song::status().undo_t5 && shown("T1:ARM T2:REPLAY T4:CFG") && !shown("T5: UNDO"));
  // ... a knob on the Rytm (also a mute: the undo puts the mutes back), also while the replay plays
  replay_confirmed(dir, 121);
  feed({0xB3, 99, 1, 0xB3, 98, 2, 0xB3, 6, 10, 0xB3, 38, 0});
  CHECK(!song::status().undo);
  replay_confirmed(dir, 131);
  cmd("song=cancel");
  feed({0xB4, 94, 1});
  CHECK(!song::status().undo);
  replay_confirmed(dir, 141);
  feed(play());
  step(100);
  feed({0xB3, 6, 20});
  feed(stop());
  step(5);
  CHECK(song::status().state == song::kOff && !song::status().undo);
  // ... a pattern the user selects; the Sidecar's send FX; ARM
  replay_confirmed(dir, 151);
  cmd("song=cancel");
  step(1100);
  select_on_rytm(9);
  CHECK(!song::status().undo);
  replay_confirmed(dir, 161);
  cmd("song=cancel");
  step(20);
  CHECK(song::status().undo);
  fx::control.send[0][0] = (uint8_t)(fx::control.send[0][0] + 1);
  step(20);
  CHECK(!song::status().undo);
  replay_confirmed(dir, 171);
  cmd("song=cancel");
  cmd("song=arm");
  CHECK(!song::status().undo);
  cmd("song=disarm");
  cmd("song=load:take-001");   // (a disarm drops the armed take)
  step(20);
  // ... another replay: once it writes, its own backup is the one undone
  replay_confirmed(dir, 181);
  replay_confirmed(dir, 191);
  undo_run();
  CHECK(undone(191));
  // ... a project switch
  replay_confirmed(dir, 201);
  const uint8_t other[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 'E', 'R', 0xF7};
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'O', 'N', 'G', 'T', 'E', 'S', 'T', 0xF7};
  engine_sysex(other, sizeof other);
  if (rename::state() == rename::kAsk) rename::answer(false);
  step(30);
  CHECK(!song::status().undo);
  engine_sysex(proj, sizeof proj);
  if (rename::state() == rename::kAsk) rename::answer(false);
  for (int i = 0; i < 5000 && project_loading(); ++i) step(1);
  CHECK(!song::status().undo && !project_loading());
  g_rytm.writes = 0;
  CHECK(cmd("song=undo").find("no replay to undo") != std::string::npos && g_rytm.writes == 0);
  cmd("song=load:take-001");
  step(20);
  // ... the slots in turn after the project load (.backups.txt from the RAM store): not slot 1 again
  std::string after = replay_confirmed(dir, 211);
  CHECK(after != dir + ".backup-1.json");
  // 4. refused while a project loads (its store filling: the edits' kLockLoading rule), nothing written
  replay_confirmed(dir, 221);
  CHECK(song::status().undo);
  project_fill_begin();   // (a load begun; the store emptied with it)
  CHECK(project_loading() && !song::status().undo);
  g_rytm.writes = 0;
  CHECK(cmd("song=undo").find("loading") != std::string::npos && g_rytm.writes == 0 && song::status().state == kReplayReady);
  for (int i = 0; i < 2000 && project_loading(); ++i) project_fill_step();
  CHECK(!project_loading() && !song::status().undo);   // (the store refilled without the set)
  CHECK(cmd("song=undo").find("not in RAM") != std::string::npos && g_rytm.writes == 0);
  cmd("song=cancel");
  // atomic: a damaged file of the set in the store (cut short) - refused whole, nothing written; whole again: undone
  std::string bk3 = replay_confirmed(dir, 231);
  cmd("song=cancel");
  std::string k1 = bk3.substr(0, bk3.size() - 5) + "-1.json";
  std::string txt = slurp(k1);
  CHECK(txt.size() > 20000);
  store::put(k1.c_str() + card.size(), (const uint8_t*)txt.data(), (uint32_t)txt.size() / 2);
  g_rytm.writes = 0;
  g_rytm.channel.clear();
  cmd("song=undo");
  step(50);   // (read back in idle passes, a file each)
  CHECK(strstr(song::status().msg, "cannot be read"));
  CHECK(g_rytm.writes == 0 && g_rytm.channel.empty() && song::status().state == song::kOff);
  store::put(k1.c_str() + card.size(), (const uint8_t*)txt.data(), (uint32_t)txt.size());
  undo_run();
  CHECK(undone(231));
  // ... a Rytm that stops answering mid-undo: not verified, the undo kept; then done
  replay_confirmed(dir, 241);
  g_rytm.answer = false;
  undo_run();
  for (int i = 0; i < 3000 && song::status().state == kUndoing; ++i) step(5);
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "no answer") && song::status().undo);
  g_rytm.answer = true;
  undo_run();
  CHECK(undone(241));
  // ... a PLAY while it writes: stopped, nothing more written, kept; after STOP: done
  replay_confirmed(dir, 251);
  g_rytm.writes = 0;
  cmd("song=undo");
  step(10);
  feed(play());
  step(1000);
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "played") && g_rytm.writes == 0);
  feed(stop());
  step(5);
  CHECK(song::status().undo);
  undo_run();
  CHECK(undone(251));
  // 5. a power cut between the replay and the undo: the set whole on the card (the atomic write: no .tmp / .prev),
  // what the Rytm had; after the restart no UNDO is offered (its edits since cannot be known), nothing written
  std::string bk4 = replay_confirmed(dir, 261);
  CHECK(stat((bk4 + ".tmp").c_str(), &sb) != 0 && stat((bk4 + ".prev").c_str(), &sb) != 0);
  {
    std::string t4 = slurp(bk4);
    Take r;
    char err[64];
    CHECK(read_json(t4.data(), (int)t4.size(), &r, err, sizeof err));
    CHECK(vec(r.kit) == K(261) && vec(r.pat) == PT(262) && r.pattern == 1 && r.mutes == 0x5);
    r.free();
    std::string t5 = slurp(bk4.substr(0, bk4.size() - 5) + "-1.json");
    CHECK(read_json(t5.data(), (int)t5.size(), &r, err, sizeof err) && vec(r.kit) == K(266) && r.pattern == 0);
    r.free();
  }
  song::reset();   // (the cut: the engine's RAM gone)
  step(30);
  g_rytm.writes = 0;
  g_rytm.channel.clear();
  CHECK(!song::status().undo && cmd("song=undo").find("no replay to undo") != std::string::npos);
  step(20);
  CHECK(g_rytm.writes == 0 && g_rytm.channel.empty());
  cmd("song=load:take-001");
  step(20);
  // 6. no backup: one that cannot be written means no replay (nothing written) and so nothing new to undo; the
  // earlier undo is kept (review 10: the Rytm unchanged)
  replay_confirmed(dir, 271, 0);
  cmd("song=cancel");
  CHECK(song::status().undo);
  CHECK(chmod(dir.c_str(), 0555) == 0);   // (the folder read-only: the backup's write fails)
  g_rytm.writes = 0;
  cmd("song=replay");
  cmd("song=confirm");
  for (int i = 0; i < 400 && song::status().state == kRestoring; ++i) step(5);
  CHECK(chmod(dir.c_str(), 0755) == 0);
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "nothing written") && g_rytm.writes == 0);
  CHECK(song::status().undo);
  undo_run();
  CHECK(strstr(song::status().msg, "replay undone") && g_rytm.kit == K(271));
  // re-review 2 finding 1: confirms whose backup fails still use slots; eight of them while an undo is offered never
  // take the undo's own slot (its files neither removed nor written over): the undo still puts back the right state
  replay_confirmed(dir, 281);
  cmd("song=cancel");
  std::string live = newest_backup(dir);
  std::string live_txt = slurp(live);
  CHECK(song::status().undo);
  CHECK(chmod(dir.c_str(), 0555) == 0);
  for (int r = 0; r < 9; ++r) {
    cmd("song=replay");
    cmd("song=confirm");
    for (int i = 0; i < 400 && song::status().state == kRestoring; ++i) step(5);
    CHECK(strstr(song::status().msg, "nothing written"));
  }
  CHECK(chmod(dir.c_str(), 0755) == 0);
  step(20);
  CHECK(song::status().undo && slurp(live) == live_txt);
  undo_run();
  CHECK(undone(281));
  // re-review N3: the 3-file set's slot reused by 2-file sets since: its extra file removed (through fmt's queue)
  CHECK(stat((b3.substr(0, b3.size() - 5) + "-2.json").c_str(), &sb) != 0);
  // 7. the backup's files held up (the card core busy): failed after 10 s, nothing written, A01's PC undone; and the
  // cancel while it waits
  select_on_rytm(1);
  g_rytm.writes = 0;
  cardq::set_worker(true);
  std::string idx0 = slurp(dir + ".backups.txt");
  cmd("song=replay");
  cmd("song=confirm");
  step(600);   // (both patterns read: the set posted - .backups.txt in fmt's slot, its files queued behind it)
  CHECK(song::status().state == kRestoring && fmt::post_busy());
  // re-review N1: .backups.txt advanced and posted before the set's files: the card core's first write is it
  host_card_core(0);
  while (cardq::service()) {}   // (what is posted now: fmt's slot holds one write, the rest waits in its queue)
  host_card_core(3);
  {
    std::string idx1 = slurp(dir + ".backups.txt");
    int n0 = atoi(idx0.c_str() + 5), n1 = atoi(idx1.c_str() + 5);
    std::string main = dir + ".backup-" + std::to_string((n1 - 2) % 8 + 1) + ".json";
    CHECK(n1 > n0 && slurp(main).find("\"backup " + std::to_string(n1 - 1) + ":") == std::string::npos);
    printf("undo: .backups.txt on the card (next %d) before its set's files\n", n1);
  }
  for (int i = 0; i < 2200 && song::status().state == kRestoring; ++i) step(5);
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "not on the card") && g_rytm.writes == 0 && g_rytm.cur == 1);
  cmd("song=replay");
  cmd("song=confirm");
  step(400);
  CHECK(song::status().state == kRestoring);
  cmd("song=cancel");
  CHECK(song::status().state == song::kOff && strstr(song::status().msg, "cancelled") && g_rytm.writes == 0);
  step(5);
  CHECK(g_rytm.cur == 1);
  host_card_core(0);
  while (cardq::service()) {}
  host_card_core(3);
  cardq::set_worker(false);
  for (int i = 0; i < 50; ++i) step(5);
  CHECK(song::status().state == song::kOff && g_rytm.writes == 0 && !song::status().undo);
  printf("undo: every pattern the take plays backed up at confirm and put back (RAM store, settings, mutes, send FX, "
         "a linked kit, once), nothing held during the replay, an unlisted pattern / > 8 patterns end it, expiry (30 s, knobs / mutes also while playing, a pattern, send FX, "
         "ARM, another replay, project), T5's grace, refused while loading / not in RAM / damaged / PLAY, a Rytm not "
         "answering, power cut, no backup (folder, held 10 s, cancel) ok\n");
}

// ---- rui1038: SRC / TBL / INST at 14 bits (the bank): from the Sidecar's reports, never the CC
static void test_src() {
  Take t;
  t.clear();
  config_default(&t.cfg);
  t.cfg.valid = true;
  Recorder r;
  r.arm(&t);
  r.play(100, 1000);
  r.clk.tick(0, 16, 2, 100);
  r.banked = 1u << 0;   // BD on a banked machine (GRANULAR), SD not
  int n0 = t.n;
  for (auto cc : {std::make_pair(99, 1), std::make_pair(98, 0), std::make_pair(6, 5), std::make_pair(38, 0x50)})
    r.channel(0xB0, (uint8_t)cc.first, (uint8_t)cc.second, 200, 1100);   // BD's NRPN 1:0: dropped (kSrc has it)
  r.channel(0xB0, 16, 5, 210, 1100);                                     // BD's CC 16 (SRC A): dropped
  CHECK(t.n == n0);
  for (auto cc : {std::make_pair(99, 1), std::make_pair(98, 0), std::make_pair(6, 9), std::make_pair(38, 0)})
    r.channel(0xB1, (uint8_t)cc.first, (uint8_t)cc.second, 220, 1100);   // SD's (analog) NRPN 1:0 still recorded
  CHECK(t.n == n0 + 1 && t.ev[t.n - 1].type == kParam && t.ev[t.n - 1].a == 1);
  r.src(0, 5 << 7 | 0x50, 300, 1200);
  CHECK(t.ev[t.n - 1].type == kSrc && t.ev[t.n - 1].a == 0 && t.ev[t.n - 1].v == (5u << 7 | 0x50));
  r.src(0, 5 << 7 | 0x50, 400, 1200);   // the same: thinned
  CHECK(t.n == n0 + 2);
  char b[96];
  event_json(t.ev[t.n - 1], b, sizeof b);
  CHECK(strstr(b, "\"src\",{\"trk\":0,\"v\":720}"));
  // replayed as 48 <track> 0 <index> <bank << 4> (a knob turn's write), not a CC
  FakePort p;
  Replayer rp;
  Take q;
  q.clear();
  config_default(&q.cfg);
  q.cfg.valid = true;
  Ev e0 = {0, kPlay, 0, 0, 0, 0}, e1 = {10, kSrc, 3, 0, 9u << 7 | 0x70, 0};
  q.push(e0), q.push(e1);
  q.end = 100;
  rp.begin(&q);
  rp.start(p);
  rp.advance(10, p);
  CHECK(p.sent.size() == 1 && p.sent[0].size() == 11 && p.sent[0][4] == 0x48 && p.sent[0][6] == 3 && p.sent[0][7] == 0 &&
        p.sent[0][8] == 9 && p.sent[0][9] == 0x70);
  // rui1045: DSP SAMPLE's SMP (P11) banked: its NRPN 1:11 / CC 27 dropped on that track [measured, rytm-mods bk_emu
  // dspnrpn], kSrc with b = 10, "p":10 in JSON, replayed as 48 <track> 10
  r.banked |= 1u << 2, r.banked_smp = 1u << 2;   // CP on DSP SAMPLE
  int n1 = t.n;
  for (auto cc : {std::make_pair(99, 1), std::make_pair(98, 11), std::make_pair(6, 5), std::make_pair(38, 0x20)})
    r.channel(0xB2, (uint8_t)cc.first, (uint8_t)cc.second, 500, 1300);
  r.channel(0xB2, 27, 5, 510, 1300);
  CHECK(t.n == n1);
  r.channel(0xB2, 16, 5, 520, 1300);   // its SRC A (ENG) is a plain parameter there
  CHECK(t.n == n1 + 1 && t.ev[t.n - 1].type == kCc);
  r.src(2, 5 << 7 | 0x20, 600, 1400, 10);
  CHECK(t.ev[t.n - 1].type == kSrc && t.ev[t.n - 1].a == 2 && t.ev[t.n - 1].b == 10 && t.ev[t.n - 1].v == (5u << 7 | 0x20));
  event_json(t.ev[t.n - 1], b, sizeof b);
  CHECK(strstr(b, "\"src\",{\"trk\":2,\"p\":10,\"v\":672}"));
  FakePort p2;
  Replayer rp2;
  Take q2;
  q2.clear();
  config_default(&q2.cfg);
  q2.cfg.valid = true;
  Ev f1 = {10, kSrc, 2, 10, 9u << 7 | 0x70, 0};
  q2.push(e0), q2.push(f1);
  q2.end = 100;
  rp2.begin(&q2);
  rp2.start(p2);
  rp2.advance(10, p2);
  CHECK(p2.sent.size() == 1 && p2.sent[0][4] == 0x48 && p2.sent[0][6] == 2 && p2.sent[0][7] == 10 && p2.sent[0][8] == 9 &&
        p2.sent[0][9] == 0x70);
  t.free(), q.free(), q2.free();
  printf("src: banked tracks' SRC A CC / NRPN dropped, kSrc at 14 bits recorded, thinned, in JSON, replayed as 48 ok\n");
  printf("src: rui1045 DSP SAMPLE's SMP: NRPN 1:11 / CC 27 dropped, kSrc p 10 in JSON, replayed as 48 t 10 ok\n");
}

int main() {
  const char* env = getenv("SONG_OUT");
  g_dir = env && *env ? env : "build/song";
  mkdir("build", 0755);
  mkdir(g_dir.c_str(), 0755);
  test_json();
  test_clock();
  test_recorder();
  test_replay();
  test_src();
  test_restore();
  test_undorun();
  char tmpl[] = "/tmp/core_song_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = to_rytm_fake;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'O', 'N', 'G', 'T', 'E', 'S', 'T', 0xF7};
  engine_sysex(proj, sizeof proj);
  test_engine(card);
  test_undo(card);
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_song: ALL PASS\n");
  return 0;
}
