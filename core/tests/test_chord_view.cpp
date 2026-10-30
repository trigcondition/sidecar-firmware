// core/tests/test_chord_view.cpp - test-1031: the SRC view's chord (core/ui/page_gfx.cpp chord_view_page) for 6-OP FM
// and MULTISAMPLE: the chord's name, INV SPR VOI, its notes on the keyboard from the last trig's root, the voice's
// notes sounding now; the plain page with CHD OFF; MULTISAMPLE's page (INST's name from the card's list); MULTISAMPLE's
// live pad chords (poly's MIDI binding: the pads play msamp's live notes, the chord written back to the MULTISAMPLE
// track, the view says LIVE). Writes the frames as PNGs (the Rytm's text in the host's font) to $RUI_OUT, default
// build/rui_chord: chord_fm.png, chord_msamp.png, chord_msamp_live.png, fm_no_chord.png.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "pool_util.h"
#include "engine.h"
#include "msamp/msamp.h"
#include "poly.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;
static uint32_t g_ms = 1000;
static void advance_ms(int ms) {
  g_us += (uint64_t)ms * 1000;
  host_fake_time(true, g_us);
  poly::tick(now_ms());
}
static void tick(int n) {
  for (int i = 0; i < n; ++i) remote_tick(g_ms += 30);
}

static void report(int cmd, int vb, int note, int vel, const std::vector<int>& p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) m.push_back((uint8_t)note), m.push_back((uint8_t)vel);
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(k < (int)p.size() ? p[k] : 0)), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void src_vals(int tr, const std::vector<int>& p) {   // 2A: the Rytm's P1..P16 of track tr's SRC page
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)tr, 0, 16};
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)p[k]), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void src_view(int tr, int on) {
  const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)tr, (uint8_t)on, 0xF7};
  engine_sysex(m, 9);
}
static bool shown(const char* s) {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  for (int i = 0; i < n; ++i)
    if (!strcmp((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
static void dump_texts() {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  for (int i = 0; i < n; ++i) printf("  text [%s]\n", (const char*)t + 36 * i + 4);
}
static int px(int x, int y) { return remote_frame()[(7 - y / 8) + 8 * x] >> (y % 8) & 1; }

// ---- a PNG of the sent frame with its text (sim/rui_render.py's colours, scale 4, the pixel grid)
static uint32_t crc32_of(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
static std::vector<uint8_t> zstore(const std::vector<uint8_t>& raw) {   // a zlib stream of stored deflate blocks
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t at = 0; at < raw.size() || at == 0;) {
    size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8)), z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (!n) break;
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

// The keyboard (page_gfx.cpp): the bottom row's cell, 5 px white keys from x 1, rows 38..46, from the window's C (base;
// chrorange: the voice's window, C3 48 until a note falls outside it)
static bool key_lit(int base, int note) {
  static const int white[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6}, after[12] = {-1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1};
  int k = note - base, o = k / 12, pc = k % 12, kx = 1, ky = 9 + 22 + 7;
  if (white[pc] >= 0) return px(kx + (o * 7 + white[pc]) * 5 + 2, ky + 6);
  int sep = kx + (o * 7 + after[pc] + 1) * 5;   // a lit black key is solid: both pixels of a row
  return px(sep, ky + 2) && px(sep, ky + 3);
}

int main() {
  const char* env = getenv("RUI_OUT");
  g_dir = env && *env ? env : "build/rui_chord";
  mkdir("build", 0755);
  mkdir(g_dir.c_str(), 0755);
  char tmpl[] = "/tmp/core_chord_view_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples' '" + card + "/MULTISAMPLE' && cp -R ../content/MULTISAMPLE/SidecarTest '" + card +
                "/MULTISAMPLE/'").c_str()) == 0);
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  for (int i = 0; i < 4000 && msamp::step(); ++i) {}
  CHECK(pu::put(pools::kInstr, "A001", "/MULTISAMPLE/SidecarTest/SidecarTest.sfz") == pools::kOk);   // INST A001
  CHECK(msamp::stats().listed == 1 && !strcmp(msamp::entry_name(1), "SidecarTest"));

  // the Rytm's 33: BD on 6-OP FM (voice 0), SD on MULTISAMPLE (0x30, voice 2)
  const uint8_t mach[9] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 2, 0};
  std::vector<uint8_t> m33(mach, mach + 8);
  m33.insert(m33.end(), {49, (uint8_t)(kFm | 0), 50, (uint8_t)(kMultiMachine | 2), 0xF7});
  engine_sysex(m33.data(), (int)m33.size());
  CHECK(remote_track_machine(0) == (kFm | 0) && remote_track_machine(1) == (kMultiMachine | 2));

  // ---- 6-OP FM, D MAJ7 INV1 (F# A C# D): four notes in the pool
  std::vector<int> fm = {0, 64, 64, 127, 64, 64, 100, 127, 0, 0, 0, 0, 6, 1, 0, 0};
  report(0x04, kFm | 0, 62, 100, fm);
  CHECK(poly::view(0).root == 62 && !poly::view(0).live && poly::voice_notes(0) == 4);
  src_vals(0, fm);
  src_view(0, 1);
  tick(4);
  CHECK(remote_stats().page == 4);
  CHECK(shown("6-OP FM") && !shown("LPG") && shown("D4 MAJ7") && shown("INV1 SPR0 AUTO") && shown("4") && shown("ON"));
  CHECK(shown("C3") && shown("C4") && shown("C5"));   // chrorange: each octave's C named
  for (int n : {66, 69, 73, 74}) CHECK(key_lit(48, n));
  for (int n : {60, 62, 64, 67, 71, 72}) CHECK(!key_lit(48, n));
  png("chord_fm.png");
  // CHD OFF on the Rytm: the plain page
  std::vector<int> fm0 = fm;
  fm0[12] = 0;
  src_vals(0, fm0);
  tick(2);
  CHECK(shown("6-OP FM") && !shown("D4 MAJ7") && shown("LPG"));
  png("fm_no_chord.png");
  src_vals(0, fm);
  tick(2);
  CHECK(shown("D4 MAJ7"));
  src_view(0, 0);
  tick(1);

  // ---- MULTISAMPLE: SidecarTest, A MIN SPR1 (A3 C5 E5: the spread opens it), three notes sounding
  std::vector<int> ms = {1, 127, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 2, 0, 1, 0};
  report(0x03, kMultiMachine | 2, -1, 0, ms);
  for (int i = 0; i < 4000 && msamp::step(); ++i) {}
  CHECK(msamp::voice_slot(2) >= 0);
  report(0x04, kMultiMachine | 2, 57, 100, ms);
  CHECK(v[2].ms && poly::view(2).root == 57 && poly::voice_notes(2) == 3);
  src_vals(1, ms);
  src_view(1, 1);
  tick(4);
  CHECK(shown("MULTISAMPLE") && shown("SD") && shown("SIDECARTEST") && shown("A3 MIN") && shown("INV0 SPR1 AUTO") &&
        shown("3") && shown("RTRG"));
  uint8_t ex[poly::kMaxNotes];
  int ne = poly::expand(57, {2, 0, 1, 0}, ex);
  CHECK(ne == 3);
  for (int i = 0; i < ne; ++i) CHECK(key_lit(48, ex[i]));
  png("chord_msamp.png");

  // ---- MULTISAMPLE live pad chords: channel 14 bound to voice 2 by its 0x04s, the pads' notes are msamp's live
  // notes, the chord (C E G) written back to SD (track 1, the MULTISAMPLE track on voice 2), the view LIVE
  std::vector<int> ms0 = ms;
  ms0[12] = 0, ms0[14] = 0;
  report(0x04, kMultiMachine | 2, 60, 127, ms0);
  advance_ms(1);
  poly::midi(0x9D, 36, 127);
  advance_ms(40);
  poly::midi(0x9D, 40, 127);
  advance_ms(1);
  report(0x04, kMultiMachine | 2, 64, 127, ms0);
  CHECK(poly::stats().bound[13] == 2);
  advance_ms(1);
  uint32_t live0 = poly::stats().live_notes;
  poly::midi(0x8D, 36, 0), poly::midi(0x8D, 40, 0);
  advance_ms(500);   // (the binding's two notes were a chord too: written back as the window closed)
  g_out.clear();
  poly::midi(0x9D, 36, 127);   // a chord on the bound channel: C E G at once
  poly::midi(0x9D, 40, 127);
  poly::midi(0x9D, 43, 127);
  CHECK(poly::stats().live_notes == live0 + 3);
  report(0x04, kMultiMachine | 2, 60, 127, ms0);   // the Rytm's own trig for the first pad: swallowed
  advance_ms(poly::kGestureMs + 2);   // (the window closes kGestureMs after its last note-on)
  int writes = 0;
  for (auto& w : g_out)
    if (w.size() == 13 && w[4] == 0x28 && w[5] == 0x01) {
      ++writes;
      CHECK(w[6] == 1 && w[7] == 60 && w[8] == 1 && w[11] == 3);   // SD, root C, MAJ, 3 notes
    }
  CHECK(writes == 1);
  CHECK(poly::view(2).live && poly::view(2).root == 60 && poly::view(2).c.chd == 1);
  src_vals(1, ms0);
  tick(3);
  CHECK(shown("C4 MAJ") && shown("LIVE"));
  for (int n : {60, 64, 67}) CHECK(key_lit(48, n));
  png("chord_msamp_live.png");
  // the pads let go: the live notes released (the trig's 0x11 does not touch them while held)
  int before = msamp::voice_notes(2);
  CHECK(before >= 3);
  poly::midi(0x8D, 36, 0), poly::midi(0x8D, 40, 0), poly::midi(0x8D, 43, 0);
  src_view(1, 0);
  tick(1);

  // ---- 6-OP FM live pad chords (CHRO: four pads together, C MAJ7 on track 0 / voice 0, CHD OFF on the Rytm): the SRC
  // view shows the DETECTED chord, root, INV SPR and LIVE - not the voice's CHD setting or the last pad's note (the
  // Rytm's own 0x04 for a pad must not clear it). Each case: pad note-on times, the 0x04's delay after its note-on.
  {
    std::vector<int> f0 = fm;
    f0[12] = f0[13] = 0;   // CHD OFF
    src_vals(0, f0);
    src_view(0, 1);
    const int pads[4] = {36, 40, 43, 47}, off = 24;   // C E G B at 60..71: C MAJ7
    struct Case {
      const char* name;
      int gap, trig_delay;   // ms between pad note-ons; the 0x04 after its note-on (negative: before)
    } cases[] = {{"together, trigs at once", 5, 2}, {"spread over 120 ms", 40, 2}, {"trigs 80 ms late", 5, 80},
                 {"trigs before the pads", 5, -3}};
    int ci = 0;
    for (const Case& cs : cases) {
      advance_ms(3000);
      poly::reset();
      for (int k = 0; k < 2; ++k) {   // channel 1 bound to voice 0 by two note-on / 0x04 pairs, then released
        poly::midi(0x90, 30 + k, 100);
        advance_ms(1);
        report(0x04, kFm | 0, 30 + k + off, 100, f0);
        advance_ms(1);
      }
      CHECK(poly::stats().bound[0] == 0);
      poly::midi(0x80, 30, 0), poly::midi(0x80, 31, 0);
      advance_ms(500);
      printf("  FM live, %s\n", cs.name);
      struct Ev {
        int t, kind, i;   // kind 0 note-on, 1 0x04
      };
      std::vector<Ev> ev;
      for (int i = 0; i < 4; ++i) {
        ev.push_back({i * cs.gap, 0, i});
        ev.push_back({i * cs.gap + cs.trig_delay, 1, i});
      }
      int t0 = 0, t1 = 0;
      for (Ev& e : ev) t0 = e.t < t0 ? e.t : t0, t1 = e.t > t1 ? e.t : t1;
      for (int t = t0; t <= t1; ++t) {
        for (Ev& e : ev)
          if (e.t == t) {
            if (e.kind == 0) poly::midi(0x90, pads[e.i], 110);
            else report(0x04, kFm | 0, pads[e.i] + off, 110, f0);
          }
        advance_ms(1);
      }
      advance_ms(200);   // the window closed, the unmatched trigs timed out; the pads still held
      tick(3);
      if (!(shown("C4 MAJ7") && shown("LIVE"))) dump_texts();
      CHECK(poly::view(0).live && poly::view(0).root == 60 && poly::view(0).c.chd == 6);
      CHECK(shown("C4 MAJ7") && shown("LIVE") && !shown("B4 NOTE"));
      for (int n : {60, 64, 67, 71}) CHECK(key_lit(48, n));
      if (!ci++) png("live_fixed.png");
      for (int i = 0; i < 4; ++i) poly::midi(0x80, pads[i], 0);
      advance_ms(100);
      tick(2);
      CHECK(shown("C4 MAJ7"));   // briefly after: still the detected chord
    }
    src_view(0, 0);
    tick(1);
  }
  printf("test_chord_view: PASS\n");
  system(("rm -rf '" + card + "'").c_str());
  return 0;
}
