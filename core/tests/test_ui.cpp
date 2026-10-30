// core/tests/test_ui.cpp - core/ui: the canvas in the Rytm's framebuffer layout, the fonts, the tile diff, the pages and
// the remote UI host against a model of rytm-mods 0026's terminal (staging, seq-checked commit into the shadow): the
// whole frame on open, then only changed tiles, the keepalive, page keys, an encoder edit through voice_report, the
// held edit against the Rytm's reports, nothing once closed.
#include <cctype>
#include <cstring>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "rui_png.h"
#include "lib/pools.h"
#include "engine.h"
#include "version.h"
#include "fx/send_fx.h"
#include "ui/extra.h"
#include "ui/page_keys.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core::ui;

// ---- 0026's terminal, as rui.c does it (1021: text slots staged and committed with the pixels, echo bindings)
static uint8_t staging[1024], shadow[1024];
static TextItem txt_st[kTextSlots], txt_sh[kTextSlots];
static int bind_slot[16], bind_val[16], binds = 0, texts_in = 0;
static char bind_pre[16][12];
static int st_seq = -1, commits = 0, rects = 0, tiles_in = 0, alive = 0, bad = 0;
static std::vector<std::vector<uint8_t>> other;
// 1022: the catch mask (37) as rui.c decodes it: encoder i bit i % 7 of e(i / 7), key c bit c % 7 of k(c / 7)
static unsigned mask_enc = 0;
static bool mask_key[256];
static int masks_in = 0;
static int splash_msgs = 0, splash_last = -1;   // 1023: 36s and the last one's ticks
static uint8_t trig_led[16];                    // 1026: the trig LEDs the Rytm has from us (34), kLedOff when off
static bool leds_on = false;
static bool caught(int code) { return mask_key[code & 0xFF]; }
static void terminal(const uint8_t* d, int len) {
  if (len == 7 && d[4] == 0x26) return;   // the IN L/R source ask on the first message (host_link.cpp, fxin): not the remote UI's
  if (len < 7 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54 || d[5] != 1 || d[len - 1] != 0xF7) {
    other.emplace_back(d, d + len);
    return;
  }
  const uint8_t* b = d + 6;
  int n = len - 7;
  for (int i = 0; i < len - 1; ++i) CHECK(i == 0 || d[i] < 0x80);   // 7-bit body
  if (d[4] == 0x38) {
    int c0 = b[1], p0 = b[2], nc = b[3], np = b[4], left = nc * 8 * np;
    if (c0 + nc > 16 || p0 + np > 8 || n - 5 != left + (left + 6) / 7) { ++bad; st_seq = -1; return; }
    const uint8_t* p = b + 5;
    uint8_t* col = staging + c0 * 64 + p0;
    int pg = 0;
    while (left > 0) {
      int g = left < 7 ? left : 7;
      unsigned hi = *p++;
      left -= g;
      while (g-- > 0) {
        col[pg] = (uint8_t)(*p++ | (hi & 1) << 7);
        hi >>= 1;
        if (++pg == np) pg = 0, col += 8;
      }
    }
    st_seq = b[0];
    ++rects;
    tiles_in += nc * np;
  } else if (d[4] == 0x39) {
    if (st_seq == b[0]) memcpy(shadow, staging, 1024), memcpy(txt_sh, txt_st, sizeof txt_sh), ++commits;
    st_seq = -1;
  } else if (d[4] == 0x3D) {
    if (n < 6 || b[1] >= kTextSlots || n - 6 > kTextLen) { ++bad; return; }
    TextItem& t = txt_st[b[1]];
    t.x = b[2], t.y = b[3], t.font = b[4], t.flags = b[5];
    memcpy(t.s, b + 6, n - 6);
    t.s[n - 6] = 0;
    st_seq = b[0];
    ++texts_in;
  } else if (d[4] == 0x3E) {
    for (auto& t : txt_st) t.s[0] = 0;
    st_seq = b[0];
  } else if (d[4] == 0x3F) {
    if (n < 6 || b[0] >= 16 || n - 6 > 11) { ++bad; return; }
    bind_slot[b[0]] = b[1], bind_val[b[0]] = b[2];
    memcpy(bind_pre[b[0]], b + 6, n - 6);
    bind_pre[b[0]][n - 6] = 0;
    ++binds;
  } else if (d[4] == 0x37) {
    if (n < 3 || n > 40) { ++bad; return; }
    mask_enc = 0;
    memset(mask_key, 0, sizeof mask_key);
    for (int i = 0; i < 3; ++i) mask_enc |= (unsigned)b[i] << (7 * i);
    for (int j = 0; j < n - 3; ++j)
      for (int i = 0; i < 7; ++i)
        if (b[3 + j] >> i & 1) mask_key[7 * j + i] = true;
    ++masks_in;
  } else if (d[4] == 0x36) {
    if (n < 1) { ++bad; return; }
    splash_last = b[0];
    ++splash_msgs;
  } else if (d[4] == 0x3C) {
    ++alive;
  } else if (d[4] == 0x34) {   // 1026: the trig LEDs (mode 1 + c[0..15], or mode 0 alone: stock's again)
    leds_on = n >= 17 && b[0] == 1;
    if (leds_on) memcpy(trig_led, b + 1, 16);
    else memset(trig_led, kLedOff, 16);
  } else {
    other.emplace_back(d, d + len);
  }
}
static void reset_counts() { commits = rects = tiles_in = alive = texts_in = binds = 0; }
// the Rytm's text slots equal the host's (what it committed last)
static bool texts_equal() {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots), k = 0;
  for (int i = 0; i < kTextSlots; ++i) {
    if (!txt_sh[i].s[0]) continue;
    if (k >= n || strcmp((const char*)t + 36 * k + 4, txt_sh[i].s) || t[36 * k] != txt_sh[i].x || t[36 * k + 1] != txt_sh[i].y)
      return false;
    ++k;
  }
  return k == n;
}
static bool shown_text(const char* s) {
  for (auto& t : txt_sh)
    if (!strcmp(t.s, s)) return true;
  return false;
}
static bool tabs_shown() {   // rui1040: the bank list's tabs, a text item a letter
  for (char b = 'A'; b <= 'H'; ++b) {
    char l[2] = {b, 0};
    if (!shown_text(l)) return false;
  }
  return true;
}
static bool tab_inverted(char b) {
  for (auto& t : txt_sh)
    if (t.s[0] == b && !t.s[1] && (t.flags & core::ui::kTextInvert)) return true;
  return false;
}

static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  core::engine_sysex(m, 10);
}
static void report03(int vi, const uint8_t p[16]) {
  uint8_t m[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)vi};
  for (int k = 0; k < 16; ++k) m[6 + 2 * k] = p[k], m[7 + 2 * k] = 0;
  m[38] = 0xF7;
  core::engine_sysex(m, 39);
}
// The Rytm's report of voice vi on machine m (the voice byte's flags), its P1..P16 as they are: a 0x03, or a trig
static void report_machine(int vi, int m, bool trig = false) {
  uint8_t r[41] = {0xF0, 0x7D, 0x52, 0x54, (uint8_t)(trig ? 0x04 : 0x03), (uint8_t)(m | vi)};
  int n = 6;
  if (trig) r[n++] = 48, r[n++] = 100;
  for (int k = 0; k < 16; ++k) r[n++] = (uint8_t)core::v[vi].p[k] & 0x7F, r[n++] = 0;
  r[n++] = 0xF7;
  core::engine_sysex(r, n);
}

int main() {
  // ---- chrorange: a keyboard's window (widgets.h keys_window) - C3 first, kept while the notes fit, moved by the
  // fewest octaves (the Rytm's CHRO pads: 0x04 notes 36..83), from the lowest note's C when wider than 3 octaves
  CHECK(keys_window(-1, 127, -1) == 48 && keys_window(-1, 60, 60) == 48 && keys_window(-1, 83, 83) == 48);
  CHECK(keys_window(48, 36, 36) == 36 && keys_window(36, 71, 71) == 36 && keys_window(36, 83, 83) == 48);
  CHECK(keys_window(48, 90, 90) == 60 && keys_window(48, 30, 100) == 24 && keys_window(48, 127, 127) == 84);
  CHECK(keys_window(48, 0, 0) == 0 && keys_window(36, 127, -1) == 36);
  CHECK(keys_window(48, 49, 84) == 48 && keys_window(0, 1, 36) == 0 && keys_window(48, 47, 82) == 36);   // review
  {   // review (brute force): from any window, notes that fit in a C-aligned 3-octave window (C0 .. C6's) are all shown,
      // on a C; a window they already fit in stays
    int bad = 0;
    for (int c0 = -1; c0 <= 84; c0 += c0 < 0 ? 1 : 12)
      for (int lo = 0; lo < 128; ++lo)
        for (int hi = lo; hi < 128; ++hi) {
          int r = keys_window(c0, lo, hi);
          bool fits = hi - lo / 12 * 12 <= 35 && lo / 12 * 12 <= 84;
          if (r % 12 || r < 0 || r > 84 || (fits && (lo < r || hi > r + 35))) ++bad;
          if (c0 >= 0 && lo >= c0 && hi <= c0 + 35 && r != c0) ++bad;
        }
    CHECK(bad == 0);
  }
  // ---- canvas: the layout, fills, the diff
  Canvas c;
  c.pixel(0, 0);
  c.pixel(127, 63);
  CHECK(c.fb[7] == 0x01 && c.fb[8 * 127] == 0x80 && c.get(0, 0) && !c.get(1, 0));
  c.clear();
  c.fill(Rect{0, 0, 8, 8}, kOn);   // the top-left tile = page 7, columns 0..7
  for (int x = 0; x < 8; ++x) CHECK(c.fb[7 + 8 * x] == 0xFF);
  Canvas z;
  uint32_t m[4];
  CHECK(c.diff_tiles(z, m) == 1 && (m[3] >> 16 & 1) && !m[0] && !m[1] && m[3] == 1u << 16);   // tile 7 * 16 + 0 = 112
  c.clear();
  c.dither(Rect{0, 0, 128, 64});
  int on = 0;
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 128; ++x) {
      on += c.get(x, y);
      CHECK(c.get(x, y) == (((x + y) & 1) == 0));
    }
  CHECK(on == 4096);
  c.clear();
  c.clip(Rect{10, 10, 5, 5});
  c.line(0, 0, 127, 63);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 128; ++x) CHECK(!c.get(x, y) || (x >= 10 && x < 15 && y >= 10 && y < 15));
  c.unclip();
  c.clear();
  c.line(0, 0, 9, 3);
  CHECK(c.get(0, 0) && c.get(9, 3));
  c.clear();
  CHECK(text_width(kSmall, "ABC") == 11 && text_width(kMedium, "AB") == 11);
  text(c, kMedium, 0, 0, "A");
  CHECK(!c.get(0, 0) && c.get(1, 0) && c.get(0, 1) && c.get(0, 6) && c.get(4, 6));   // .###. / #...#
  CHECK(sin256(0) == 0 && sin256(64) == 127 && sin256(128) == 0 && sin256(192) == -127 && cos256(0) == 127);
  CHECK(list_count("A\0BB\0") == 2 && !strcmp(list_name("A\0BB\0", 1), "BB") && !*list_name("A\0BB\0", 2));

  // ---- the host against the terminal
  char tmpl[] = "/tmp/core_ui_XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::hooks.to_rytm = terminal;
  core::engine_begin();
  for (int i = 0; i < 2000 && !core::pools::ready(); ++i) core::pools::step();   // the pool lists (the template)
  // ---- every page draws; a list parameter steps one zone a detent
  for (int which = 0; which < 5; ++which) {
    uint8_t vals[16] = {40, 90, 1, 20, 60, 90, 40, 64, 3, 64, 1, 100, 10, 110, 0, 0};
    CHECK(remote_render(which, 0, vals, c));
    int px = 0;
    for (int i = 0; i < 1024; ++i) px += __builtin_popcount(c.fb[i]);
    CHECK(px > (which == 0 || which == 4 ? 200 : 300));   // the status pages: two bars and a few badges
  }
  const Page* plaits = page_for_machine(0x00);
  CHECK(plaits && !page_for_machine(0x10) && plaits->enc[0] == 0);   // DAISYSP: no page (JUNO, FM, DSP SAMPLE: page_*.cpp)
  CHECK(step_value(*plaits, 0, 0, 1, false) == (1 * 128 + 64) / 17 && core::eng_zone(step_value(*plaits, 0, 64, 3, false), 17) == 8 + 3);
  CHECK(step_value(*plaits, 1, 64, -3, false) == 61 && step_value(*plaits, 1, 64, 2, true) == 80 && step_value(*plaits, 1, 126, 5, false) == 127);

  remote_reset();
  report_machine(0, 0x00);   // track 1 reported on PLAITS (the only voice page: the others never reported)
  remote_tick(1000);
  CHECK(!rects && !alive && !remote_open());   // closed: nothing
  from_rytm(0x30, 0, 1, 0);                    // TRK + YES on the Rytm
  CHECK(remote_open());
  remote_tick(1000);
  CHECK(commits == 1 && tiles_in == 128 && rects == 8 && !memcmp(shadow, remote_frame(), 1024));   // the whole frame
  CHECK(remote_stats().page == 0 && texts_equal() && shown_text("SIDECAR") && !strcmp(kStatusTitle, "SIDECAR") && shown_text("FX1") && shown_text("OFF") &&
        !shown_text("NET -") && !shown_text("OB -"));   // SIDECAR: the tracks and the FX only
  CHECK(binds == 8);   // the SIDECAR page: every encoder's echo none
  // the SIDECAR page's mask: navigation and the stock-screen keys, all nine encoders (A..H, LEVEL) and their pushes;
  // transport, pads, FUNC, TEMPO, MUTE, the trig and bank keys pass
  CHECK(masks_in == 1 && mask_enc == 0x1FF && caught(0x70) && caught(0x77) && caught(0x78) &&
        caught(0x41) && caught(0x45) && caught(0x84) && caught(0x44) &&
        caught(0x34) && caught(0xB0) && caught(0x86) && !caught(0x51) && !caught(0x52) && !caught(0x50) &&
        !caught(0x90) && !caught(0x9B) && !caught(0x80) && !caught(0x83) && !caught(0x11) && !caught(0x00) &&
        !caught(0xC0));
  reset_counts();
  remote_tick(1020);   // within the tick: nothing
  remote_tick(1060);   // a tick, nothing changed: nothing
  CHECK(!rects && !commits && !alive);
  remote_tick(1260);
  CHECK(alive == 1 && !rects);   // 250 ms without a message: 3C
  reset_counts();
  from_rytm(0x31, 0, 0x41, 1);   // RIGHT: SYSTEM
  from_rytm(0x31, 0, 0x41, 0);
  remote_tick(1290);
  char sys_title[40] = "SYSTEM " SIDECAR_VERSION;   // as the Rytm shows it: upper case ("0.3.0-dev" -> "0.3.0-DEV")
  for (char* q = sys_title; *q; ++q) *q = (char)toupper((unsigned char)*q);
  CHECK(commits == 1 && remote_stats().page == 2 && texts_equal() && shown_text(sys_title) && shown_text("NET -") &&
        shown_text("OB -") && shown_text("U1 -") && !shown_text("FX1") && !memcmp(shadow, remote_frame(), 1024));
  reset_counts();
  from_rytm(0x31, 0, 0x84, 1);   // PAGE: voice 1's PLAITS page
  from_rytm(0x31, 0, 0x84, 0);
  remote_tick(1320);
  CHECK(commits == 1 && remote_stats().page == 1 && remote_stats().voice == 0 && !memcmp(shadow, remote_frame(), 1024));
  CHECK(texts_equal() && shown_text("PLAITS") && shown_text("HRM") && shown_text("BD"));
  // 1022: the PLAITS page takes its encoders (and their pushes) too, nothing else changed (SYSTEM had SIDECAR's mask:
  // not sent again)
  {
    unsigned want = 0;
    for (int e = 0; e < 8; ++e)
      if (plaits->enc[e] != kNone) want |= 1u << e;
    CHECK(masks_in == 2 && mask_enc == want && want && caught(0x70) && caught(0x41) && !caught(0x51) && !caught(0x90) &&
          !caught(0x78) && !(mask_enc & 0x100));
  }
  CHECK(binds == 7 && bind_slot[0] == 0x7F && bind_slot[1] == kSlotFoot && !strcmp(bind_pre[1], "BD HRM:") &&
        bind_slot[7] == kSlotFoot);   // ENG is a list: no echo (unchanged: not sent again); HRM .. LEV echo
  int page_tiles = tiles_in;
  CHECK(page_tiles > 0 && page_tiles <= 128);
  // an encoder: B (HRM, P2) +5 -> the voice's P2 through voice_report, the knob redrawn, a few tiles
  float before = core::v[0].p[1];
  reset_counts();
  from_rytm(0x32, 1, 64 + 5, 0);
  CHECK(core::v[0].p[1] == before + 5 && core::v[0].patch.harmonics > (before + 4) / 127.0f);
  remote_tick(1328);   // a turn: the frame at once (8 ms after the last), not at the next 25 ms tick
  char foot[16];
  snprintf(foot, sizeof foot, "BD HRM:%d", (int)before + 5);
  CHECK(commits >= 1 && commits <= 2 && tiles_in > 0 && tiles_in <= 24 && !memcmp(shadow, remote_frame(), 1024));
  CHECK(texts_equal() && shown_text(foot) && bind_val[1] == (int)before + 5);
  CHECK(commits == 1 || remote_stats().split == 1);   // the touched cell and the bottom bar first when more changed
  printf("ui: whole frame 128 tiles, page change %d tiles, one knob %d tiles in %d rects, %d commits\n", page_tiles,
         tiles_in, rects, commits);
  // the Rytm reports the voice again with its own unchanged P2: the edit holds; its P2 moved: the Rytm wins
  uint8_t p[16] = {45, (uint8_t)before, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100};
  report03(0, p);
  CHECK(core::v[0].p[1] == before + 5);
  p[1] = 30;
  report03(0, p);
  CHECK(core::v[0].p[1] == 30);
  // ENG: a detent is a model
  from_rytm(0x32, 0, 64 + 1, 0);
  CHECK(core::eng_zone(core::v[0].p[0], 17) == core::eng_zone(45, 17) + 1);
  // NO: back to SIDECAR; LEFT from SIDECAR wraps to the last voice page
  from_rytm(0x31, 0, 0x45, 1);
  remote_tick(1440);
  CHECK(remote_stats().page == 0);
  // NO on SIDECAR: the page leaves, as TRK + YES (3A 0 0; the Rytm answers 30 off), then TRK + YES opens it again
  other.clear();
  from_rytm(0x31, 0, 0x45, 1);
  CHECK(other.size() == 1 && other[0].size() == 9 && other[0][4] == 0x3A && other[0][6] == 0 && other[0][7] == 0);
  other.clear();
  from_rytm(0x30, 0, 0, 3);
  CHECK(!remote_open());
  from_rytm(0x30, 0, 1, 0);
  remote_tick(1442);
  CHECK(remote_open() && remote_stats().page == 0);
  from_rytm(0x31, 0, 0x41, 1);   // RIGHT: SYSTEM
  remote_tick(1450);
  CHECK(remote_stats().page == 2);
  // the SYSTEM page's numbers: the recorder, the network (never more than up / down and the address), U1..U5
  RemoteStatus rs = {};
  rs.ob = 1, rs.cpu[0] = 12, rs.cpu[1] = 34, rs.cpu[2] = -1, rs.rec = 3, rs.rec_secs = 75, rs.eth = 2;
  rs.ip[0] = 192, rs.ip[1] = 168, rs.ip[2] = 1, rs.ip[3] = 20;
  strcpy(rs.user[0], "CLUST1");
  strcpy(rs.user[1], "---");
  remote_status(rs);
  core::fx::control.knob[0][0] = 4, core::fx::control.knob[0][1] = 127;   // FX1 PINGPONG, P1 POST (route: per slot)
  remote_tick(1480);
  CHECK(shown_text("ETH UP 192.168.1.20") && shown_text("REC 1:15") && shown_text("U1 CLUST1") && shown_text("U2 ---") &&
        shown_text("12%") && shown_text("34%") && shown_text("OB OK"));
  // the whole of a long name; Wi-Fi up and its address
  strcpy(rs.user[4], "LONGNAM8");
  rs.eth = 1, rs.wifi = 2, rs.ip[2] = 100, rs.ip[3] = 200;
  remote_status(rs);
  remote_tick(1510);
  CHECK(shown_text("U5 LONGNAM8") && shown_text("WIFI UP 192.168.100.200"));
  // LEFT: back to SIDECAR. Every row fits, nothing cut (1021: "PLAIT" in the last column; "FX1 " + a name ran into POST)
  from_rytm(0x31, 0, 0x40, 1);
  remote_tick(1520);
  int plaits_shown = 0;
  for (auto& t : txt_sh) plaits_shown += !strcmp(t.s, "PLAITS");
  CHECK(remote_stats().page == 0 && plaits_shown == 1 && shown_text("PINGPONG") && shown_text("POST") &&
        shown_text("FX2") && shown_text("OFF") && !shown_text("PRE"));
  core::fx::control.knob[0][0] = 0, core::fx::control.knob[0][1] = 0;
  // fix1049: UP is the Sidecar's modifier (page_keys.h), no page arrow: UP alone stays on the page (before: the previous
  // page, as LEFT)
  from_rytm(0x31, 0, 0x42, 1), from_rytm(0x31, 0, 0x42, 0);
  remote_tick(1525);
  CHECK(remote_stats().page == 0);
  from_rytm(0x31, 0, 0x40, 1);
  remote_tick(1530);
  CHECK(remote_stats().page == 3 && !memcmp(shadow, remote_frame(), 1024));   // LEFT on SIDECAR: the last page, SCOPE
  from_rytm(0x31, 0, 0x40, 1);   // (ux1027: the LFO page is off the ring - UP + LFO's alone)
  remote_tick(1560);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 0 && !memcmp(shadow, remote_frame(), 1024));   // the only voice page
  // closed by the Rytm (timeout / escape): nothing more, not even keepalives
  from_rytm(0x30, 0, 0, 2);
  reset_counts();
  for (uint32_t t = 1540; t < 4000; t += 10) remote_tick(t);
  CHECK(!remote_open() && !rects && !alive && !commits && !bad);
  // reopened: the whole frame again (the shadow may hold anything)
  from_rytm(0x30, 0, 1, 1);
  remote_tick(5000);
  CHECK(tiles_in == 128 && commits == 1 && remote_stats().opens == 3 && remote_stats().closes == 2);
  CHECK(mask_enc == 0x1FF && caught(0x78) && caught(0x41) && remote_stats().masks == (uint32_t)masks_in);   // the mask again on reopening

  // ---- 1023: the connect splash. The Rytm's hello (35 <uptime>) when it was up >= 3 s: the splash frame, 36 64, the
  // line running down in small frames, 36 0 at 3 s; an early hello (the Rytm booting with the Sidecar), caps 0 or a
  // hello while the page is open: a 3C only. The user opening the page during the splash replaces it.
  from_rytm(0x30, 0, 0, 2);
  reset_counts();
  from_rytm(0x35, 0, 20, 1);   // up 1 s
  remote_tick(6000);
  remote_tick(6100);
  CHECK(!splash_msgs && alive == 1 && !commits);
  reset_counts();
  from_rytm(0x35, 1, 0, 1);    // up 128 ticks = 6.4 s
  remote_tick(7000);
  CHECK(commits == 1 && tiles_in == 128 && splash_msgs == 1 && splash_last == 64 && shown_text("CONNECTED") &&
        !remote_open() && !memcmp(shadow, remote_frame(), 1024));
  for (uint32_t t = 7010; t < 9990; t += 10) remote_tick(t);
  printf("ui: splash: %d commits, %d tiles after the first frame\n", commits, tiles_in - 128);
  CHECK(commits >= 25 && commits <= 40 && tiles_in - 128 <= 3 * commits && splash_msgs == 1 &&
        !memcmp(shadow, remote_frame(), 1024));
  remote_tick(10000);
  CHECK(splash_msgs == 2 && splash_last == 0 && remote_stats().splashes == 1 && remote_stats().hellos == 2);
  reset_counts();
  for (uint32_t t = 10010; t < 12000; t += 10) remote_tick(t);
  CHECK(!commits && !alive && !rects);   // over: nothing more while the page is closed
  from_rytm(0x35, 1, 0, 0);    // caps 0: a Rytm that cannot show it
  remote_tick(12010);
  CHECK(alive == 1 && !commits && splash_msgs == 2);
  reset_counts();
  from_rytm(0x35, 1, 0, 1);
  remote_tick(13000);
  CHECK(commits == 1 && splash_msgs == 3);
  from_rytm(0x30, 0, 1, 0);    // TRK + YES during the splash
  for (uint32_t t = 13010; t < 17000; t += 10) remote_tick(t);
  CHECK(remote_open() && splash_msgs == 3 && remote_stats().page == 0 && shown_text(kStatusTitle) && !shown_text("CONNECTED"));
  reset_counts();
  from_rytm(0x35, 1, 0, 1);    // a hello while the page is open: a new link (the Rytm's page left by its timeout, its 30
  remote_tick(17010);          // off lost with the cable): closed here too, the splash (0026 ignores a 36 while open)
  CHECK(!remote_open() && splash_msgs == 4);
  for (uint32_t t = 17020; t < 21000; t += 10) remote_tick(t);
  CHECK(splash_last == 0);
  // The page the user opened a moment before the first hello (0026 says hello 0.5 s after its USB port came up, and the
  // user's TRK + YES can fall in that half second): theirs, the Rytm still shows it - kept, no splash (the Rytm would
  // ignore the 36 and we would have stopped serving the page), and what the hello invalidates sent again.
  remote_tick(22000);
  from_rytm(0x30, 0, 1, 0);
  remote_tick(22100);
  reset_counts();
  uint32_t sm0 = splash_msgs, cl0 = remote_stats().closes, hl0 = remote_stats().hellos, ms0 = masks_in, sp_0 = remote_stats().splashes;
  remote_tick(22400);          // (the page's frames are going)
  from_rytm(0x35, 1, 0, 1);    // 300 ms after the open: up 6.4 s, caps 1
  for (uint32_t t = 22410; t < 23500; t += 10) remote_tick(t);
  CHECK(remote_open() && splash_msgs == sm0 && remote_stats().closes == cl0 && remote_stats().hellos == hl0 + 1 &&
        remote_stats().splashes == sp_0 && shown_text(kStatusTitle) && !shown_text("CONNECTED"));
  CHECK(commits >= 1 && tiles_in == 128 && masks_in > (int)ms0 && mask_enc == 0x1FF);   // the whole frame and the mask again
  // ... but an open from before the cable went (the page left by its timeout, its 30 off lost): a new link, as above
  remote_tick(24500);
  from_rytm(0x35, 1, 0, 1);
  remote_tick(24510);
  CHECK(!remote_open() && splash_msgs == sm0 + 1);
  for (uint32_t t = 24520; t < 28000; t += 10) remote_tick(t);
  CHECK(splash_last == 0);
  CHECK(other.empty() && !bad);   // nothing but remote-UI messages went out, all well formed

  // ---- the voice pages follow the machine the Rytm's track plays, as its reports name it (the voice byte), live:
  // nothing reported yet (an analog track, or one just set on the SRC page: 0021 reports a voice only while it sounds)
  // -> no page and "-" on SIDECAR, never a guessed PLAITS; each later report moves the page to its machine at once
  remote_reset();
  from_rytm(0x30, 0, 1, 0);
  remote_tick(20000);
  CHECK(remote_open() && remote_stats().page == 0 && !shown_text("PLAITS") && shown_text("-"));
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // PAGE: SYSTEM
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // PAGE again: no voice page, an extra page (SCOPE)
  remote_tick(20100);
  CHECK(remote_stats().page != 1);
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: back to SIDECAR
  report_machine(2, core::kJuno, true);                        // track 3 trigs on JUNO (mod 0021's EXT machine 41)
  remote_tick(20200);
  CHECK(remote_stats().page == 0 && shown_text("JUNO"));      // SIDECAR names it
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // SYSTEM, then the first voice page: track 3's JUNO
  remote_tick(20300);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 2 && shown_text("JUNO") && shown_text("CUT"));
  static const struct { int m; const char* title; } kFollow[] = {
      {core::kFm, "6-OP FM"}, {core::kPlaits, "PLAITS"}, {core::kDspSample, "DSP SAMPLE"}, {core::kJuno, "JUNO"}};
  uint32_t t = 20400;
  for (const auto& f : kFollow) {   // the machine changed on the Rytm, the page open: the next frame is the new one's
    report_machine(2, f.m, true);
    remote_tick(t += 100);
    CHECK(remote_stats().page == 1 && remote_stats().voice == 2 && shown_text(f.title));
  }
  report_machine(2, 0x50, true);    // U1, a user machine: no page here - the page leaves for SIDECAR
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1 && !shown_text("JUNO"));
  report_machine(2, core::kDaisy);  // DAISYSP: no page either
  report_machine(5, core::kFm);     // track 6 on 6-OP FM: its page
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 5 && shown_text("6-OP FM"));
  remote_forget_voices();   // the platform's boot bench reported voices itself: forgotten, no voice page left
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1);

  // ---- 1025: the Rytm's 33 names each track's machine at once (no trig needed) and decides alone from then on
  auto machines = [](int t0, int n, const uint8_t* mv) {   // F0 7D 52 54 33 01 <t0> <n> {<machine> <vb>} x n F7
    uint8_t m[40] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t0, (uint8_t)n};
    memcpy(m + 8, mv, 2 * n);
    m[8 + 2 * n] = 0xF7;
    core::engine_sysex(m, 9 + 2 * n);
  };
  // the PAT lists (16 JUNO / 17 FM) share one cache on the Rytm: a track that turns JUNO / 6-OP FM gets its list pushed
  auto sent = [](uint8_t cmd) {
    int k = 0;
    for (auto& m : other) k += m.size() > 5 && m[4] == cmd;
    return k;
  };
  uint8_t snap[24];
  for (int k = 0; k < 12; ++k) snap[2 * k] = (uint8_t)k, snap[2 * k + 1] = 0x7F;   // every track analog
  report_machine(1, core::kPlaits, true);   // an older build's path: a report alone ...
  other.clear();
  machines(0, 12, snap);                    // ... and then the snapshot, which decides
  CHECK(sent(0x16) == 0 && sent(0x17) == 0);   // a snapshot pushes no list
  CHECK(remote_track_machine(0) == 0x7F && remote_stats().machines == 1);
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: SIDECAR
  remote_tick(t += 100);
  CHECK(remote_stats().page == 0 && !shown_text("PLAITS") && !shown_text("FM"));
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // SYSTEM, then no voice page
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1);
  const uint8_t juno_bd[2] = {41, 0x08};   // BD picked JUNO in the SRC list (no trig)
  other.clear();
  machines(0, 1, juno_bd);
  CHECK(sent(0x16) >= 1 && sent(0x17) == 0);   // JUNO's list pushed (PAT names at once, not after the Rytm's re-ask)
  other.clear();
  machines(0, 1, juno_bd);
  CHECK(sent(0x16) == 0);                      // the same machine again: not pushed again
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 0 && shown_text("JUNO"));
  const uint8_t fm_cp[2] = {40, 0x48 | 2};   // CP (track 4) on 6-OP FM: 0026's 33 says voice 2, its 04s voice 7 (RS/CP)
  other.clear();
  machines(3, 1, fm_cp);
  CHECK(sent(0x17) >= 1 && sent(0x16) == 0);   // the FM list pushed
  const uint8_t analog_bd[2] = {22, 0x7F};   // BD back to an analog machine: its page leaves at once
  machines(0, 1, analog_bd);
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1 || remote_stats().voice != 0);
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 0 && shown_text("FM") && !shown_text("JUNO"));
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 7 && shown_text("6-OP FM"));
  // SD (track 2) on JUNO: 0026's 33 says voice 1, the Rytm plays it on voice 2 (its 04s): the page is V3's, not V2's
  const uint8_t juno_sd[2] = {41, 0x08 | 1};
  machines(1, 1, juno_sd);
  CHECK(remote_track_machine(1) == (0x08 | 2));
  uint8_t mixed[24];   // the Pi's live snapshot [measured 2026-10-19]: 08 09 72 42 7f 7f 75 15 4e 7f 0f 7f
  const uint8_t live[12] = {0x08, 0x09, 0x72, 0x42, 0x7F, 0x7F, 0x75, 0x15, 0x4E, 0x7F, 0x0F, 0x7F};
  for (int k = 0; k < 12; ++k) mixed[2 * k] = 0, mixed[2 * k + 1] = live[k];
  machines(0, 12, mixed);
  // voices by the 04s; CP's 0x42 was DIGIDRUM's, MD's (machines.h kMd) since 2026-10-29
  const int want[12] = {0x08, 0x0A, 0x77, 0x47, 0x7F, 0x7F, 0x75, 0x15, 0x4C, 0x7F, 0x0E, 0x7F};
  for (int k = 0; k < 12; ++k) CHECK(remote_track_machine(k) == want[k]);
  machines(3, 1, fm_cp);   // CP back on 6-OP FM for what follows
  const uint8_t past[4] = {0, 0x7F, 0, 0x7F};   // tracks 11 and 12: past track 11, refused, nothing changes
  machines(11, 2, past);
  CHECK(remote_track_machine(3) == (0x48 | 7) && remote_stats().machines == 8);

  // ---- 1027: the SRC view. With the page closed, the Rytm's stock SRC page of a track on a Sidecar machine shows the
  // host's frames (a whole frame answers each 29 t 1), drawn from the Rytm's own values (2A) and only from them:
  // no 37 mask, no 3F echo, no edit, never a 29 back; a track without a page gets nothing (3A 0 0 when its page went);
  // the TRK + YES page comes first.
  auto src_vals = [](int tr, int first, int n, const uint8_t* vals) {   // F0 7D 52 54 2A 01 <t> <first> <n> {int frac}
    uint8_t m[48] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)tr, (uint8_t)first, (uint8_t)n};
    for (int i = 0; i < n; ++i) m[9 + 2 * i] = vals[i], m[10 + 2 * i] = 0;
    m[9 + 2 * n] = 0xF7;
    core::engine_sysex(m, 10 + 2 * n);
  };
  auto src_view = [](int tr, int on) {
    const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)tr, (uint8_t)on, 0xF7};
    core::engine_sysex(m, 9);
  };
  auto sent_cmd = [](uint8_t cmd) {
    int k = 0;
    for (auto& m : other) k += m.size() > 5 && m[4] == cmd;
    return k;
  };
  auto shown_prefix = [](const char* pre) -> const char* {
    for (auto& tx : txt_sh)
      if (!strncmp(tx.s, pre, strlen(pre))) return tx.s;
    return nullptr;
  };
  from_rytm(0x30, 0, 0, 0);   // the page closed
  remote_tick(t += 100);
  float p0[16];
  for (int k = 0; k < 16; ++k) p0[k] = core::v[0].p[k];
  const uint8_t juno_vals[8] = {3, 100, 20, 90, 10, 60, 40, 127};   // BD's PAT CUT RES ENV ATK DEC REL LEN on its SRC page
  src_vals(0, 0, 8, juno_vals);
  other.clear(), reset_counts();
  int masks0 = masks_in, binds0 = binds, bad0 = remote_stats().bad;
  src_view(0, 1);   // BD (JUNO) shown
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(commits >= 1 && rects >= 1 && remote_stats().page == 4 && remote_stats().voice == 0 && sent_cmd(0x29) == 0);
  CHECK(shown_text("JUNO") && shown_text("BD") && texts_equal());
  CHECK(masks_in == masks0 && binds == binds0);   // every key and knob the Rytm's
  for (int k = 0; k < 16; ++k) CHECK(core::v[0].p[k] == p0[k]);   // the page only reads
  // a knob on the Rytm (2A n 1): the drawing follows, the bottom bar names it
  int c1 = commits, ti1 = tiles_in;
  const uint8_t cut[1] = {77};
  src_vals(0, 1, 1, cut);
  remote_tick(t += 10);
  CHECK(commits > c1 && tiles_in > ti1);
  const char* f = shown_prefix("BD CUT:");
  CHECK(f && !strcmp(f, "BD CUT:77"));
  for (int k = 0; k < 16; ++k) CHECK(core::v[0].p[k] == p0[k]);
  // nothing new: keepalives only
  c1 = commits;
  for (int i = 0; i < 12; ++i) remote_tick(t += 30);
  CHECK(commits == c1 && alive >= 1);
  // TRK + YES opens the remote page: the view waits, the page's own frames and mask; closed: the view's whole frame
  from_rytm(0x30, 0, 1, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page != 4 && masks_in > masks0);
  from_rytm(0x30, 0, 0, 0);
  other.clear();
  int r1 = rects;
  c1 = commits;
  remote_tick(t += 100);
  CHECK(commits > c1 && rects - r1 >= 8 && remote_stats().page == 4 && shown_text("BD CUT:77") && sent_cmd(0x3A) == 0);
  // the 29 again (after a hello): a whole frame again
  r1 = rects;
  src_view(0, 1);
  remote_tick(t += 100);
  CHECK(rects - r1 >= 8 && remote_stats().page == 4);
  // SD's view: SD is on JUNO too (voice 2); BT is analog: no page, nothing sent
  src_view(1, 1);
  remote_tick(t += 100);
  CHECK(shown_text("SD") && shown_text("JUNO"));
  other.clear(), c1 = commits;
  src_view(4, 1);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(commits == c1 && other.empty());
  // BD's view, then BD turns analog while it shows: 3A 0 0 (stock back), nothing drawn after
  src_view(0, 1);
  remote_tick(t += 100);
  CHECK(commits > c1);
  machines(0, 1, analog_bd);
  other.clear(), c1 = commits;
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(commits == c1 && sent_cmd(0x3A) == 1 && other.size() == 1 && other[0][6] == 0 && other[0][7] == 0);
  src_view(0, 0);
  machines(0, 1, juno_bd);
  src_view(4, 0);   // another track's 29 0: BD's view ends only by its own
  src_view(0, 1), remote_tick(t += 100), src_view(0, 0);   // left: nothing more is sent
  other.clear(), reset_counts();
  for (int i = 0; i < 20; ++i) remote_tick(t += 30);
  CHECK(other.empty() && commits == 0 && alive == 0);
  // a replug while the view is on (and while the TRK + YES page is open): the hello resets both, the splash shows, then
  // the view's 29 t 1 (repeated after the hello) gets a whole frame once the splash ended
  src_view(0, 1);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 4);
  from_rytm(0x30, 0, 1, 0);   // the page open when the cable goes
  for (int i = 0; i < 20; ++i) remote_tick(t += 100);   // (the cable out and in again: seconds)
  int sp0 = splash_msgs;
  from_rytm(0x35, 0, 100, 1);   // hello: up 5 s
  src_view(0, 1);
  CHECK(!remote_open());
  for (int i = 0; i < 10; ++i) remote_tick(t += 30);
  CHECK(splash_msgs == sp0 + 1 && splash_last == 64);   // the splash, not blocked by the stale page / view
  r1 = rects;
  for (int i = 0; i < 120; ++i) remote_tick(t += 30);   // past its 3 s: 36 0, then the view
  CHECK(splash_last == 0 && remote_stats().page == 4 && rects - r1 >= 8 && shown_text("JUNO") && shown_text("BD"));
  src_view(0, 0);
  // the Rytm has the SRC view (it sent 29 / 2A): TRK + YES lists SIDECAR, SYSTEM and the live pages, no voice page
  // (BD and SD are on JUNO, CP on 6-OP FM: an older build listed them)
  from_rytm(0x30, 0, 1, 0);
  for (int i = 0; i < 12; ++i) {
    from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);
    remote_tick(t += 100);
    CHECK(remote_stats().page != 1 && remote_stats().page != 4);
  }
  from_rytm(0x30, 0, 0, 0);
  remote_tick(t += 100);
  // malformed 2A / 29 are refused
  const uint8_t bad_m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 12, 0, 0xF7};
  core::engine_sysex(bad_m, 9);
  uint8_t bad_n[12] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 15, 2, 1, 0, 0xF7};   // P16 + 1 past the end
  core::engine_sysex(bad_n, 12);
  CHECK(remote_stats().bad == (uint32_t)bad0 + 2);

  // ---- rui1038: the bank list (4A). BD on GRANULAR (voice 0), its SRC view, P1 = F005 by the 2A (lo 0x50); the Rytm
  // opens the list (knob A turned): the tabs and rows over the view, RIGHT -> bank G, knob A +3 -> row 8 (the 47 of bank
  // G's names sent: the bank byte 6), YES (4A 0 .. 1) -> 48 0 0 8 0x60 1 (the Rytm locks held steps, else the kit),
  // the page again; P1's 14 bits known for the song recorder.
  const uint8_t gran_bd[2] = {47, 0x38};
  machines(0, 1, gran_bd);
  src_view(0, 1);
  const uint8_t p1f[12] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 0, 1, 5, 0x50, 0xF7};
  core::engine_sysex(p1f, 12);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(remote_track_src14(0) == (5 << 7 | 0x50));
  const uint8_t bl_open[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 1, 5, 0x50, 0, 0xF7};
  core::engine_sysex(bl_open, 12);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().on && src_bank_state().bank == 5 && src_bank_state().row == 5);
  CHECK(tabs_shown() && tab_inverted('F') && !tab_inverted('G') && shown_text("005 ---") && shown_text("SOURCE"));
  other.clear();
  from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);   // RIGHT: bank G
  from_rytm(0x32, 0, 64 + 3, 0);                              // knob A +3: row 8
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().bank == 6 && src_bank_state().row == 8 && shown_text("008 ---") && tab_inverted('G') && !tab_inverted('F'));
  bool g47 = false;
  for (auto& m : other) g47 = g47 || (m.size() >= 11 && m[4] == 0x47 && m[6] == 0 && m[7] == 1 && m[9] == 6);
  CHECK(g47);
  other.clear();
  const uint8_t bl_yes[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 0, 5, 0x50, 1, 0xF7};
  core::engine_sysex(bl_yes, 12);
  bool w48 = false;
  for (auto& m : other) w48 = w48 || (m.size() == 12 && m[4] == 0x48 && m[6] == 0 && m[7] == 0 && m[8] == 8 && m[9] == 0x60 && m[10] == 1);
  CHECK(w48 && !src_bank_state().on && remote_track_src14(0) == (8 << 7 | 0x60));
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(shown_text("GRANULAR") && !tabs_shown());
  // NO: closed, nothing written
  core::engine_sysex(bl_open, 12);
  remote_tick(t += 30);
  other.clear();
  const uint8_t bl_no[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 0, 5, 0x50, 2, 0xF7};
  core::engine_sysex(bl_no, 12);
  CHECK(sent_cmd(0x48) == 0 && !src_bank_state().on);
  // rui1040: FUNC + YES on a row (4A on 3): bank F row 5 -> the files (the host answers 4A 0 1, the page shows the
  // target and the browser), NO at the card's root -> back to the list (4A 0 0); on bank A -> refused (4A 0 0, a note)
  core::engine_sysex(bl_open, 12);
  remote_tick(t += 30);
  other.clear();
  const uint8_t bl_files[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 3, 5, 0x50, 0, 0xF7};
  core::engine_sysex(bl_files, 12);
  auto sent4a = [&](int v) {
    for (auto& m : other) if (m.size() == 9 && m[4] == 0x4A && m[6] == 0 && m[7] == v) return true;
    return false;
  };
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().on && src_bank_state().browse && sent4a(1) && !tabs_shown());
  bool hd = false;
  for (auto& x : txt_sh) hd = hd || !strncmp(x.s, "F005 ", 5);
  CHECK(hd);
  other.clear();
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO at the root: the list again
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().on && !src_bank_state().browse && sent4a(0) && tabs_shown());
  from_rytm(0x31, 0, 0x40, 1), from_rytm(0x31, 0, 0x40, 0);   // LEFT x 5: bank A
  from_rytm(0x31, 0, 0x40, 1), from_rytm(0x31, 0, 0x40, 0);
  from_rytm(0x31, 0, 0x40, 1), from_rytm(0x31, 0, 0x40, 0);
  from_rytm(0x31, 0, 0x40, 1), from_rytm(0x31, 0, 0x40, 0);
  from_rytm(0x31, 0, 0x40, 1), from_rytm(0x31, 0, 0x40, 0);
  // the pools design: bank A takes files too (A005: the files open)
  other.clear();
  int rf = src_bank_state().refusals;
  core::engine_sysex(bl_files, 12);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().bank == 0 && src_bank_state().browse && sent4a(1));
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: back to the list
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(!src_bank_state().browse);
  // H001 (the template's IN input): refused with a note (4A 0 0); so is row 000
  for (int i = 0; i < 7; ++i) from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);   // RIGHT x 7: bank H
  for (int i = 0; i < 4; ++i) from_rytm(0x31, 0, 0x42, 1), from_rytm(0x31, 0, 0x42, 0);   // UP x 4: row 001
  other.clear();
  core::engine_sysex(bl_files, 12);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().bank == 7 && src_bank_state().row == 1);
  CHECK(!src_bank_state().browse && src_bank_state().refusals == rf + 1);
  CHECK(sent4a(0));
  CHECK(shown_text("BUILT-IN: UNLOAD FIRST"));
  from_rytm(0x31, 0, 0x42, 1), from_rytm(0x31, 0, 0x42, 0);   // UP: H000 OFF
  other.clear();
  core::engine_sysex(bl_files, 12);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(!src_bank_state().browse && src_bank_state().refusals == rf + 2 && shown_text("000 IS OFF"));
  core::engine_sysex(bl_no, 12);
  printf("ui: rui1040 FUNC + YES on a bank row: the files (4A 0 1), NO back (4A 0 0), bank A open, built-in / OFF refused\n");
  src_view(0, 0);
  // ---- rui1045: DSP SAMPLE's SMP (P11) opens the same list from its SMPL page: the Rytm's 4A carries P11 (C005: lo
  // 0x20), then its 29 1 (the view over that page while the list is open); the SAMPLES pool's eight banks, no GRANULAR
  // preview on a DSP SAMPLE voice; YES -> 48 0 10 <row> <bank << 4> 1 (P11); P11's 14 bits for the song recorder;
  // FUNC + YES opens the files in every bank
  const uint8_t dsps_bd[2] = {45, 0x20};
  machines(0, 1, dsps_bd);
  const uint8_t ds_open[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 1, 5, 0x20, 0, 0xF7};
  core::engine_sysex(ds_open, 12);
  src_view(0, 1);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().on && src_bank_state().bank == 2 && src_bank_state().row == 5 && src_bank_state().machine == 0x20);
  CHECK(tabs_shown() && tab_inverted('C') && shown_text("005 ---") && shown_text("SAMPLE"));
  for (int i = 0; i < 5; ++i) from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);   // RIGHT x 5: bank H
  from_rytm(0x32, 0, 64 + 3, 0);                                                          // the list's knob +3: row 8
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(src_bank_state().bank == 7 && src_bank_state().row == 8 && tab_inverted('H') && !tab_inverted('C'));
  other.clear();
  const uint8_t ds_yes[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 0, 5, 0x20, 1, 0xF7};
  core::engine_sysex(ds_yes, 12);
  bool d48 = false;
  for (auto& m : other) d48 = d48 || (m.size() == 12 && m[4] == 0x48 && m[6] == 0 && m[7] == 10 && m[8] == 8 && m[9] == 0x70 && m[10] == 1);
  CHECK(d48 && !src_bank_state().on && remote_track_src14(0) == (8 << 7 | 0x70) && remote_track_src_param(0) == 10);
  src_view(0, 0);
  for (int b = 0; b < 8; ++b) {   // FUNC + YES on row 20 of each bank (H001..H012: the template's inputs / built-ins):
                                  // the files open (4A 0 1), NO back to the list
    const uint8_t o[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 1, 20, (uint8_t)(b << 4), 0, 0xF7};
    const uint8_t f3[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 3, 20, (uint8_t)(b << 4), 0, 0xF7};
    core::engine_sysex(o, 12);
    src_view(0, 1);
    remote_tick(t += 30);
    other.clear();
    core::engine_sysex(f3, 12);
    for (int i = 0; i < 3; ++i) remote_tick(t += 30);
    CHECK(src_bank_state().bank == b && src_bank_state().browse && sent4a(1));
    from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
    remote_tick(t += 30);
    core::engine_sysex(bl_no, 12);
    src_view(0, 0);
  }
  {   // fix1049: FUNC + YES on DSP SAMPLE's SRC view with SMP OFF (rytm-mods 0026 1049: 4A 1 why 0, then 4A 3 why 6):
      // the files at once, aimed at bank A's first free row (an empty pool's A001); the list's own FUNC + YES on OFF
      // (why 0) stays refused (000 IS OFF)
    const uint8_t o[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 1, 0, 0, 0, 0xF7};
    const uint8_t f6[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 3, 0, 0, 6, 0xF7};
    const uint8_t f0[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 3, 0, 0, 0, 0xF7};
    int free_a = 0;
    for (int r = 1; r < 128 && !free_a; ++r)
      if (core::pools::at(core::pools::kSamples, r).type == core::pools::kEmpty) free_a = r;
    core::engine_sysex(o, 12);
    src_view(0, 1);
    remote_tick(t += 30);
    other.clear();
    core::engine_sysex(f6, 12);
    for (int i = 0; i < 3; ++i) remote_tick(t += 30);
    printf("ui: fix1049 FUNC + YES on DSP SAMPLE's SRC (OFF): the files for A%03d\n", src_bank_state().row);
    CHECK(free_a > 0 && src_bank_state().on && src_bank_state().bank == 0 && src_bank_state().row == free_a &&
          src_bank_state().browse && sent4a(1));
    from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: back to the list, on OFF again (no file set)
    remote_tick(t += 30);
    CHECK(src_bank_state().on && !src_bank_state().browse && src_bank_state().row == 0);
    core::engine_sysex(bl_no, 12);
    src_view(0, 0);
    core::engine_sysex(o, 12);
    src_view(0, 1);
    remote_tick(t += 30);
    other.clear();
    core::engine_sysex(f0, 12);
    for (int i = 0; i < 3; ++i) remote_tick(t += 30);
    CHECK(src_bank_state().row == 0 && !src_bank_state().browse && !sent4a(1));
    core::engine_sysex(bl_no, 12);
    src_view(0, 0);
  }
  // the 2A's P11 (the view's values) is P11's 14 bits on DSP SAMPLE (P1's on the banked machines)
  src_view(0, 1);
  const uint8_t p11[12] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 10, 1, 7, 0x30, 0xF7};
  core::engine_sysex(p11, 12);
  CHECK(remote_track_src14(0) == (7 << 7 | 0x30));
  src_view(0, 0);
  printf("ui: rui1045 DSP SAMPLE's SMP: the bank list (4A with P11), banks A..H, YES -> 48 0 10, FUNC + YES in every bank\n");
  machines(0, 1, juno_bd);
  printf("ui: the bank list (4A): tabs, RIGHT / knob A, bank G's names (47 + bank), YES -> 48 with the bank and flags ok\n");

  // UX review R3 / N3: YES on SIDECAR opens the MENU (off the ring: LEFT / RIGHT never land on it), DOWN to KEYS, YES
  // its cheat sheet, NO back to SIDECAR; rui1044's 49 1 4 1 (UP held alone, the page closed) opens KEYS with T1..T10
  // lit, 49 1 4 0 closes the page again
  {
    remote_reset();
    menu_reset();
    uint32_t t = 50000;
    const std::string pd = rui_png_dir("RUI_PAGES", "build/rui_pages");   // rui1046: the docs' renders of these pages
    uint8_t fb[kBytes];
    auto png = [&](const char* n) { preview_sent(fb); CHECK(rui_png(pd, n, fb)); };
    // ---- the pairing code's modal (pi/engine/paircode.h): over a page that is open, and the page back afterwards.
    // Rendered, not just asserted: before / during / after go to build/rui_pages as PNGs to be looked at.
    {
      from_rytm(0x30, 0, 1, 0);                 // a Sidecar page open on the Rytm
      from_rytm(0x35, 1, 0, 1);                 // its hello: caps 1, so it can draw a 36 frame (no splash: page open)
      remote_tick(t += 30);
      CHECK(remote_open());
      png("pair_modal_0_page.png");
      uint8_t page_fb[kBytes];
      memcpy(page_fb, shadow, sizeof page_fb);
      auto page_txt = txt_sh;
      CHECK(remote_popup("SIDECAR PAIR CODE", "1234 5678", 12000, true));   // over: a page is open
      remote_tick(t += 30);
      png("pair_modal_1_over.png");
      CHECK(shown_text("SIDECAR PAIR CODE") && shown_text("1234 5678"));
      CHECK(memcmp(shadow, page_fb, sizeof page_fb) != 0);                  // it is on the screen
      // The Rytm draws each text item itself, in a box it clears, so nothing of the page may be left under the
      // modal or it would be drawn back over it. (remote.cpp's kModal* geometry.)
      for (auto& it : txt_sh) {
        if (!it.s[0] || !strcmp(it.s, "SIDECAR PAIR CODE") || !strcmp(it.s, "1234 5678")) continue;
        if (it.y + 6 <= 16 || it.y >= 16 + 32) continue;
        int w = (int)strlen(it.s) * 5, x = text_left(it, w);
        CHECK(x + w <= 24 || x >= 24 + 80);
      }
      int changed = 0;                          // and it is a small box, not the whole screen
      for (int i = 0; i < kBytes; ++i)
        if (shadow[i] != page_fb[i]) ++changed;
      CHECK(changed > 0 && changed < kBytes / 2);
      // it goes by itself, and the page comes back exactly as it was - pixel for pixel and slot for slot
      remote_tick(t += 12100);
      remote_tick(t += 30);
      png("pair_modal_2_back.png");
      CHECK(!memcmp(shadow, page_fb, sizeof page_fb));
      CHECK(txt_sh == page_txt);
      CHECK(!shown_text("1234 5678"));
      // and it can be taken off early, when a device pairs
      CHECK(remote_popup("SIDECAR PAIR CODE", "8765 4321", 12000, true));
      remote_tick(t += 30);
      CHECK(shown_text("8765 4321"));
      remote_popup_end();
      remote_tick(t += 30);
      remote_tick(t += 30);
      CHECK(!shown_text("8765 4321") && !memcmp(shadow, page_fb, sizeof page_fb));
      // a legacy Rytm cannot draw it at all: that is a capability, and it must still say no
      remote_legacy(true);
      CHECK(!remote_popup("SIDECAR PAIR CODE", "1111 2222", 12000, true));
      remote_legacy(false);
      from_rytm(0x30, 0, 0, 0);                 // the page closed again, for the menu renders below
      remote_tick(t += 30);
      remote_reset();
      menu_reset();
    }
    from_rytm(0x30, 0, 1, 0);
    remote_tick(t += 30);
    CHECK(remote_stats().page == 0 && shown_text("YES: MENU  NO: LEAVE"));
    from_rytm(0x31, 0, 0x44, 1), from_rytm(0x31, 0, 0x44, 0);   // YES
    remote_tick(t += 30);
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "MENU") && shown_text("SIDECAR MENU") && shown_text("FUNC+CHRO") &&
          shown_text("UP+T11") && shown_text("UP+T6") && shown_text("UP+T7") && shown_text("YES: OPEN  NO: BACK"));
    png("menu_1_top.png");
    // rui1051: the rows by rule, not by count - none is a page only its keys open (page_keys.h), KEYS the last
    CHECK(menu_rows() >= 2 && !menu_row_page(menu_rows() - 1));
    for (int i = 0; i < menu_rows(); ++i) CHECK(!menu_row_page(i) || !sc_shortcut_only(menu_row_page(i)));
    for (int i = 0; i < menu_rows() + 4; ++i) from_rytm(0x31, 0, 0x43, 1), from_rytm(0x31, 0, 0x43, 0);   // DOWN to the last row
    remote_tick(t += 30);
    CHECK(shown_text("KEYS (ALL COMBOS)") && shown_text("UP+T12") && shown_text("UP+T10") && !shown_text("UP+T13"));
    png("menu_2_end.png");
    from_rytm(0x31, 0, 0x44, 1), from_rytm(0x31, 0, 0x44, 0);   // YES: KEYS
    remote_tick(t += 30);
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "KEYS") && shown_text("UP+T1") && shown_text("TOUCH: HOME"));
    png("keys_1_pages.png");
    for (int i = 0; i < 6; ++i) from_rytm(0x31, 0, 0x43, 1), from_rytm(0x31, 0, 0x43, 0);   // DOWN x 6: T7..T12
    remote_tick(t += 30);
    CHECK(shown_text("UP+T11") && shown_text("SAMPLE, AGAIN: AUTO") && shown_text("UP+T8") && shown_text("TOUCH: SET"));
    for (int i = 0; i < 7; ++i) from_rytm(0x31, 0, 0x43, 1), from_rytm(0x31, 0, 0x43, 0);   // DOWN x 7: the UP combos
    remote_tick(t += 30);
    CHECK(shown_text("UP+TRIG") && shown_text("UP+SRC") && shown_text("UP+LFO"));
    png("keys_2_combos.png");
    for (int i = 0; i < 60; ++i) from_rytm(0x31, 0, 0x43, 1), from_rytm(0x31, 0, 0x43, 0);   // DOWN to the end: the legacy forms
    remote_tick(t += 30);
    CHECK(shown_text("TRK+TRIG") && shown_text("= UP+TRIG") && shown_text("T+FUNC+PLAY") && shown_text("= T+UP+PLAY"));
    png("keys_3_legacy.png");
    from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: SIDECAR
    remote_tick(t += 30);
    CHECK(remote_stats().page == 0 && !remote_page_title());
    from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);   // RIGHT: SYSTEM (MENU is not on the ring)
    remote_tick(t += 30);
    CHECK(remote_stats().page == 2);
    from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: SIDECAR, NO: leave
    from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
    remote_tick(t += 30);
    from_rytm(0x30, 0, 0, 1);   // the Rytm closed it
    CHECK(!remote_open());
    // rui1052: UP's map with no touchscreen up (remote_display_owns never given - a Sidecar Mini, or a Pi whose panel
    // was configured and did not initialise): the pages only the touchscreen has are off the map, their trigs unlit
    // (their keys show nothing on either screen), and the UP combos otherwise off the map take the rows they free
    CHECK(!remote_display_present());
    other.clear();
    from_rytm(0x49, 4, 1, 0);   // UP held alone: the page asked open
    CHECK(sent_cmd(0x3A) == 1);
    from_rytm(0x30, 0, 1, 1);   // opened (host)
    remote_tick(t += 30);
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "KEYS") && shown_text("UP + KEY") && keys_map_on() &&
          shown_text("T6 SONG") && shown_text("T7 REC") && shown_text("T11 SAMPLE") && shown_text("T13 POOL") &&
          shown_text("TRIG SEQ") && shown_text("FX INSERT FX") && shown_text("AMP OUT") &&
          shown_text("T+REC COPY") && shown_text("T+STOP PASTE") && shown_text("T+PLAY CLEAR"));
    CHECK(!shown_text("T1-5,8 TOUCH") && !shown_text("T1 HOME") && !shown_text("T5 INSERT FX") && !shown_text("T8 SET"));
    CHECK(leds_on && trig_led[5] != kLedOff && trig_led[12] != kLedOff &&                       // T6 SONG, T13 POOL
          trig_led[0] == kLedOff && trig_led[4] == kLedOff && trig_led[7] == kLedOff);           // T1, T5, T8: nothing there
    png("up_map_alone.png");
    other.clear();
    from_rytm(0x49, 4, 0, 0);   // let go: closed again
    CHECK(sent_cmd(0x3A) == 1 && !keys_map_on());
    // and with one up (the Pi's panel: voice_pi::OwnPages gave it its pages): as before - the pages only the glass has
    // are one row of their keys, their trigs lit, and the step-edit trio is off the map again
    remote_display_owns(1u << 6 | 1u << 7 | 1u << 9 | 1u << 10 | 1u << 12 | 1u << 13);
    CHECK(remote_display_present());
    from_rytm(0x30, 0, 0, 1);
    other.clear();
    from_rytm(0x49, 4, 1, 0);
    CHECK(sent_cmd(0x3A) == 1);
    from_rytm(0x30, 0, 1, 1);
    remote_tick(t += 30);
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "KEYS") && shown_text("UP + KEY") && keys_map_on() &&
          shown_text("T1-5,8 TOUCH") && shown_text("T6 SONG") && shown_text("T11 SAMPLE") && shown_text("T13 POOL") &&
          shown_text("TRIG SEQ") && shown_text("FX INSERT FX") && shown_text("AMP OUT"));
    CHECK(!shown_text("T+REC COPY") && !shown_text("T+STOP PASTE") && !shown_text("T+PLAY CLEAR"));
    CHECK(leds_on && trig_led[0] == kLedBlue && trig_led[4] == kLedBlue && trig_led[7] == kLedBlue);   // T1, T5, T8: the glass's
    png("up_map.png");
    other.clear();
    from_rytm(0x49, 4, 0, 0);
    CHECK(sent_cmd(0x3A) == 1 && !keys_map_on());
    remote_display_owns(0);
    // the map up, then UP + T9 (a slow combo): SCOPE shows, and UP's release leaves it open
    from_rytm(0x30, 0, 0, 1);
    from_rytm(0x49, 4, 1, 0);
    from_rytm(0x30, 0, 1, 1);
    remote_tick(t += 30);
    from_rytm(0x49, 1, 9, 0);
    remote_tick(t += 30);
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "SCOPE"));
    other.clear();
    from_rytm(0x49, 4, 0, 0);
    CHECK(sent_cmd(0x3A) == 0 && remote_open() && !keys_map_on());
    from_rytm(0x30, 0, 0, 1);
    printf("ui: MENU (YES on SIDECAR), KEYS, UP held's combo map (49 1 4), a slow UP + T9 keeps its page\n");
    // rui1051 (the user's report on rui1050): the ring never lands on a page only its keys open; NO goes back to where
    // a shortcut's page came from - the stock screen (the page asked closed: 3A), the Sidecar page under it, SIDECAR
    // when it came from the MENU
    {
      from_rytm(0x30, 0, 1, 0);
      remote_tick(t += 30);
      for (int i = 0; i < 2 * extra_pages() + 4; ++i) {
        from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);   // RIGHT round the ring
        remote_tick(t += 30);
        CHECK(!remote_page_title() || (!sc_shortcut_only(remote_page_title()) && strcmp(remote_page_title(), "LFO")));   // ux1027
      }
      from_rytm(0x30, 0, 0, 1);
      auto no = [&]() { from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0); remote_tick(t += 30); };
      auto is = [](const char* title) { return remote_page_title() && !strcmp(remote_page_title(), title); };
      // UP + LFO on the stock screen (30 1 1): NO asks the page closed at once
      from_rytm(0x30, 1, 1, 0);
      remote_tick(t += 30);
      CHECK(is("LFO"));
      other.clear();
      no();
      CHECK(sent_cmd(0x3A) == 1);
      from_rytm(0x30, 0, 0, 1);
      // UP + T9 on the stock screen (the page asked open, then opened): NO closes it again
      other.clear();
      from_rytm(0x49, 1, 9, 0);
      CHECK(sent_cmd(0x3A) == 1);
      from_rytm(0x30, 0, 1, 1);
      remote_tick(t += 30);
      CHECK(is("SCOPE"));
      other.clear();
      no();
      CHECK(sent_cmd(0x3A) == 1);
      from_rytm(0x30, 0, 0, 1);
      // SYSTEM, then UP + T9 over it: NO back to SYSTEM, NO again SIDECAR (the ring's page: as before)
      from_rytm(0x30, 0, 1, 0);
      remote_tick(t += 30);
      from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);
      remote_tick(t += 30);
      CHECK(remote_stats().page == 2);
      from_rytm(0x49, 1, 9, 0);
      remote_tick(t += 30);
      CHECK(is("SCOPE"));
      no();
      CHECK(remote_stats().page == 2);
      no();
      CHECK(remote_stats().page == 0 && !remote_page_title());
      // SCOPE, then UP + LFO over it (30 1 1 while open), UP + T11 over that: NO SONG -> LFO -> SCOPE -> SIDECAR
      from_rytm(0x49, 1, 9, 0);
      remote_tick(t += 30);
      from_rytm(0x30, 1, 1, 0);
      remote_tick(t += 30);
      CHECK(is("LFO"));
      from_rytm(0x49, 1, 12, 0);   // PROJECT (shown without the bundle feed? else NOT HERE NOW: the LFO stays)
      remote_tick(t += 30);
      bool proj = is("PROJECT");
      no();
      if (proj) {
        CHECK(is("LFO"));
        no();
      }
      CHECK(is("SCOPE"));
      no();
      CHECK(remote_stats().page == 0 && !remote_page_title());
      // the MENU's SCOPE row: NO goes to SIDECAR (the MENU counts as SIDECAR, as before)
      from_rytm(0x31, 0, 0x44, 1), from_rytm(0x31, 0, 0x44, 0);   // YES: the MENU
      remote_tick(t += 30);
      CHECK(is("MENU"));
      for (int i = 0; i < menu_rows() + 2; ++i) from_rytm(0x31, 0, 0x42, 1), from_rytm(0x31, 0, 0x42, 0);   // UP to the top
      for (int i = 0; i < menu_rows() && !(menu_row_page(i) && !strcmp(menu_row_page(i), "SCOPE")); ++i)
        from_rytm(0x31, 0, 0x43, 1), from_rytm(0x31, 0, 0x43, 0);
      from_rytm(0x31, 0, 0x44, 1), from_rytm(0x31, 0, 0x44, 0);   // YES: SCOPE
      remote_tick(t += 30);
      CHECK(is("SCOPE"));
      other.clear();
      no();
      CHECK(remote_stats().page == 0 && !remote_page_title() && sent_cmd(0x3A) == 0);
      from_rytm(0x30, 0, 0, 1);
      printf("ui: rui1051 the ring skips SEQ / INSERT / POOL / CHOP; NO back where a shortcut's page came from\n");
    }
    // F4 / F1: the page closed, a Rytm that understands 36 (its hello's caps, up 0.5 s: no splash): the session's first
    // underrun pops up once (a frame, 36 n, then 36 0); a second one does not
    from_rytm(0x35, 0, 10, 1);
    int sm0 = splash_msgs;
    RemoteStatus rs = {};
    rs.underruns = 3, rs.cpu[0] = 91;
    remote_status(rs);
    remote_tick(t += 30);
    CHECK(remote_popups() == 1 && splash_msgs == sm0 + 1 && splash_last > 0 && shown_text("SIDECAR AUDIO DROPOUT") &&
          shown_text("LOAD 91%  SEE STATUS"));
    remote_tick(t += 900);
    CHECK(splash_msgs == sm0 + 2 && splash_last == 0);
    rs.underruns = 5;
    remote_status(rs);
    remote_tick(t += 30);
    CHECK(remote_popups() == 1);
    printf("ui: F4 the first underrun's popup (36), once\n");
    // the temperature warning's popup (pi/engine/thermal.h's wording): the same call as F4's. Shown with the page closed;
    // with a page open it is dropped (a temperature warning does not override a page the user is looking at: the banner
    // and STATUS still say it, and the kernel offers it again each second until the Rytm can take it)
    const uint32_t pn = remote_popups();
    remote_tick(t += 3000);
    remote_popup("SIDECAR RUNNING HOT", "SEE STATUS (UP+T10)");
    remote_tick(t += 30);
    CHECK(remote_popups() == pn + 1 && shown_text("SIDECAR RUNNING HOT") && shown_text("SEE STATUS (UP+T10)"));
    {
      uint8_t pfb[kBytes];
      preview_sent(pfb);   // what the Rytm was sent: the popup's frame, rendered for a look
      CHECK(rui_png(rui_png_dir("RUI_PAGES", "build/rui_pages"), "popup_hot.png", pfb));
    }
    remote_tick(t += 900);
    from_rytm(0x30, 0, 1, 0);                    // TRK + YES: a page opens
    remote_tick(t += 30);
    CHECK(remote_open());
    remote_tick(t += 3000);
    remote_popup("SIDECAR RUNNING HOT", "SEE STATUS (UP+T10)");
    remote_tick(t += 30);
    CHECK(remote_popups() == pn + 1);            // dropped, not shown over the page
    from_rytm(0x30, 0, 0, 0);                    // the page closed
    CHECK(!remote_open());
    for (int i = 0; i < 20; ++i) remote_tick(t += 300);   // (the page's close settles: no splash left)
    usleep(2100000);   // (remote_popup's own 2 s window for the same text runs on the platform's clock)
    remote_popup("SIDECAR RUNNING HOT", "SEE STATUS (UP+T10)");
    remote_tick(t += 30);
    CHECK(remote_popups() == pn + 2 && shown_text("SIDECAR RUNNING HOT"));   // the offer after it: shown
    remote_tick(t += 900);
    printf("ui: the temperature popup (36): shown with the page closed, dropped over an open page, shown once it closes\n");
    // the pairing code wins (audit 2026-10-26): a status popup (the temperature warning, the first dropout) never
    // replaces it - not while it is owed (asked for, not drawn yet) and not while it is up - and once it has gone the
    // status popup can come again
    {
      for (int i = 0; i < 5; ++i) remote_tick(t += 300);
      usleep(2100000);   // (past remote_popup's 2 s same-text window)
      const uint32_t p0 = remote_popups(), sp0 = remote_status_popups();
      CHECK(remote_popup("SIDECAR PAIR CODE", "1234 5678", 12000, true));
      CHECK(!remote_popup("SIDECAR RUNNING HOT", "SEE STATUS (UP+T10)"));   // owed, not drawn yet: refused
      remote_tick(t += 30);
      CHECK(shown_text("SIDECAR PAIR CODE") && shown_text("1234 5678") && !shown_text("SIDECAR RUNNING HOT"));
      CHECK(!remote_popup("SIDECAR AUDIO DROPOUT", "SEE STATUS (UP+T10)"));   // up: refused
      remote_tick(t += 1000);
      CHECK(shown_text("1234 5678") && !shown_text("SIDECAR AUDIO DROPOUT"));
      CHECK(remote_popups() == p0 + 1);
      // and the status popups' own count did not move: the temperature warning's caller (thermal::popup_due) waits
      // for that count, so the pairing code being shown must not read as its warning having been seen
      CHECK(remote_status_popups() == sp0);
      remote_tick(t += 11100);                   // its 12 s are over
      remote_tick(t += 30);
      CHECK(remote_popup("SIDECAR RUNNING HOT", "SEE STATUS (UP+T10)"));    // and the status popup's turn
      remote_tick(t += 30);
      CHECK(shown_text("SIDECAR RUNNING HOT") && remote_popups() == p0 + 2 && remote_status_popups() == sp0 + 1);
      remote_tick(t += 900);
      CHECK(remote_popup("SIDECAR PAIR CODE", "1234 5678", 12000, true));   // the other way round: the code goes over it
      remote_tick(t += 30);
      CHECK(shown_text("1234 5678") && remote_status_popups() == sp0 + 1);
      remote_tick(t += 12100);
      remote_tick(t += 30);
      printf("ui: the pairing code's popup is never replaced by a status popup\n");
    }
    // A hello while the code's modal is owed or up (the Rytm restarting, or saying hello again after a silence)
    // dropped it, so the boot's code never appeared on the Rytm (audit 2026-10-26). It is kept and drawn on the new
    // link; a status popup still goes with the old one, and a Rytm that cannot draw it (no 36) does not keep it.
    {
      usleep(2100000);   // (past remote_popup's 2 s same-text window)
      const uint32_t p0 = remote_popups();
      CHECK(remote_popup("SIDECAR PAIR CODE", "2468 1357", 12000, true));
      from_rytm(0x35, 0, 2, 1);                  // a hello before it was drawn
      remote_tick(t += 30);
      CHECK(remote_popups() == p0 + 1 && shown_text("2468 1357"));
      remote_tick(t += 1000);
      from_rytm(0x35, 0, 2, 1);                  // and one while it is up: drawn again, on the new link
      remote_tick(t += 30);
      CHECK(remote_popups() == p0 + 2 && shown_text("2468 1357"));
      remote_tick(t += 12100);
      remote_tick(t += 30);
      usleep(2100000);
      CHECK(remote_popup("SIDECAR RUNNING HOT", "SEE STATUS (UP+T10)"));
      from_rytm(0x35, 0, 2, 1);                  // a status popup is dropped by a hello, as before
      remote_tick(t += 30);
      CHECK(remote_popups() == p0 + 2);
      CHECK(remote_popup("SIDECAR PAIR CODE", "2468 1357", 12000, true));
      from_rytm(0x35, 0, 2, 0);                  // a Rytm with no 36: it cannot be drawn, it is not kept
      remote_tick(t += 30);
      CHECK(remote_popups() == p0 + 2);
      from_rytm(0x35, 0, 2, 1);
      remote_tick(t += 30);
      CHECK(remote_popups() == p0 + 2);          // (and not brought back by a later hello either)
      printf("ui: the pairing code's popup survives a hello (the boot's code is shown once the Rytm is there)\n");
    }
    // The pairing code's popup with the page closed held 30 s (device 2026-10-29: it went after about a second, since
    // the Rytm ends a splash after 1 s without a host message, and the old 12 s asked for 244 ticks, sent as 116): a
    // host message at least every 0.5 s, every 36 within 7 bits and re-sent before its ticks run out, 36 0 at 30 s.
    {
      for (int i = 0; i < 5; ++i) remote_tick(t += 300);
      from_rytm(0x35, 1, 0, 1);                  // a hello from a Rytm up 6.4 s: the connect splash is owed too
      remote_tick(t += 30);
      const uint32_t p0 = remote_popups();
      CHECK(remote_popup("SIDECAR PAIR CODE", "1357 2468", kPopupMaxMs, true));
      const int s0 = splash_msgs;
      int a0 = alive, s_seen = splash_msgs, a_seen = alive, c_seen = commits;
      uint32_t last_msg = t, gap = 0, t36 = t, max36 = 0;
      bool ended = false, ticks_ok = true;
      for (uint32_t el = 0; el < 31000; el += 50) {
        remote_tick(t += 50);
        if (splash_msgs != s_seen) {
          if (splash_last == 0) ended = ended || el >= 29900;
          else {
            ticks_ok = ticks_ok && splash_last <= 127 && splash_last > 0;
            if (t - t36 > max36 && s_seen != s0) max36 = t - t36;
            t36 = t;
          }
        }
        if (splash_msgs != s_seen || alive != a_seen || commits != c_seen) {
          if (!ended && t - last_msg > gap) gap = t - last_msg;
          last_msg = t;
        }
        s_seen = splash_msgs, a_seen = alive, c_seen = commits;
        if (el < 29000) CHECK(shown_text("1357 2468"));
      }
      CHECK(ticks_ok && ended && gap <= 550 && alive > a0);
      CHECK(max36 > 0 && max36 * 1 <= 127u * 50u);   // re-sent while the last one still held
      CHECK(remote_popups() == p0 + 1);
      printf("ui: the pairing code's popup held 30 s with the page closed (3C every 0.5 s, 36 again every %u ms)\n",
             (unsigned)max36);
      for (int i = 0; i < 20; ++i) remote_tick(t += 300);   // (the connect splash it held back runs now)
    }
    // Headless: a key on the Rytm while the code's popup is up with the page closed ends its splash (the Rytm's rule);
    // as the Rytm reports it, the popup is over here too - no 36 brings it back over a user who has read it
    {
      for (int i = 0; i < 5; ++i) remote_tick(t += 300);
      CHECK(!remote_open());
      CHECK(remote_popup("SIDECAR PAIR CODE", "1122 3344", kPopupMaxMs, true));
      remote_tick(t += 50);
      CHECK(shown_text("1122 3344"));
      from_rytm(0x31, 0, 0x10, 1), from_rytm(0x31, 0, 0x10, 0);   // FX pressed on the stock screen
      const int s1 = splash_msgs;
      bool again = false;
      for (int i = 0; i < 200; ++i) {             // 10 s: past the next 36's window
        remote_tick(t += 50);
        if (splash_msgs != s1 && splash_last != 0) again = true;
      }
      CHECK(!again);
      CHECK(remote_popup("SIDECAR RUNNING HOT", "SEE STATUS (UP+T10)"));   // not "owed or up" any more: accepted
      for (int i = 0; i < 5; ++i) remote_tick(t += 300);
      from_rytm(0x32, 0, 65, 0);                  // and a turn ends it the same way
      CHECK(remote_popup("SIDECAR PAIR CODE", "5566 7788", kPopupMaxMs, true));
      remote_tick(t += 50);
      from_rytm(0x32, 0, 65, 0);
      const int s2 = splash_msgs;
      for (int i = 0; i < 200; ++i) remote_tick(t += 50);
      CHECK(splash_msgs == s2);
      printf("ui: a key or a turn on the Rytm ends the code's popup with the page closed (no 36 again)\n");
    }
    // Over an open page a key takes the code's modal off (the user has read it), and that key - its press and its
    // release - does nothing else: NO here would otherwise have left the page
    {
      from_rytm(0x30, 0, 1, 0);
      remote_tick(t += 30);
      CHECK(remote_open());
      CHECK(remote_popup("SIDECAR PAIR CODE", "9753 1864", kPopupMaxMs, true));
      remote_tick(t += 30);
      remote_tick(t += 30);
      CHECK(shown_text("9753 1864"));
      from_rytm(0x31, 0, 0x45, 1);               // NO pressed
      remote_tick(t += 30);
      remote_tick(t += 30);
      from_rytm(0x31, 0, 0x45, 0);               // and let go
      remote_tick(t += 30);
      CHECK(!shown_text("9753 1864") && remote_open() && remote_stats().page == 0);
      from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);   // the next key is the page's again: RIGHT, SYSTEM
      remote_tick(t += 30);
      CHECK(remote_stats().page == 2);
      from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
      remote_tick(t += 30);
      printf("ui: a key takes the code's modal off an open page, and is eaten\n");
    }
    // The code on the SIDECAR status page (UP + T10): readable on demand on a Sidecar with no touchscreen
    {
      CHECK(remote_stats().page == 0);
      remote_pair_code("2468 1357");
      remote_tick(t += 30);
      CHECK(shown_text("PAIR 2468 1357"));
      png("status_pair_code.png");
      remote_pair_code("");
      remote_tick(t += 30);
      CHECK(!shown_text("PAIR 2468 1357") && !shown_text("PAIR "));
      remote_pair_code("2468 1357");
      printf("ui: the SIDECAR page shows the pairing code (PAIR 1234 5678)\n");
    }
    // SIDECAR MENU > WEB UI: the web page on / off, the code, a new code, forget the devices (YES twice)
    {
      struct Fake { bool on = true; int gate = 1, devs = 3, news = 0, forgets = 0; };
      static Fake fk;
      static const RemoteWebUi hooks = {
          [] { return fk.on; }, [](bool on) { fk.on = on; }, [] { return fk.gate; }, [] { return fk.devs; },
          [] { if (fk.gate != 1) return false; ++fk.news; return true; }, [] { ++fk.forgets, fk.devs = 0; }};
      auto key = [&](int k) { from_rytm(0x31, 0, (uint8_t)k, 1), from_rytm(0x31, 0, (uint8_t)k, 0); remote_tick(t += 30); };
      auto is = [](const char* title) { return remote_page_title() && !strcmp(remote_page_title(), title); };
      int row = -1;
      for (int i = 0; i < menu_rows(); ++i) if (menu_row_page(i) && !strcmp(menu_row_page(i), "WEB UI")) row = i;
      CHECK(row >= 0);
      // with no platform hooks (the Teensy, the Mac) the row is there but says so
      remote_webui(nullptr);
      menu_reset();
      key(0x44);                                 // YES on SIDECAR: MENU
      CHECK(is("MENU"));
      for (int i = 0; i < row; ++i) from_rytm(0x31, 0, 0x43, 1), from_rytm(0x31, 0, 0x43, 0);
      key(0x44);
      CHECK(is("MENU"));   // (its note: "WEB UI: NOT HERE NOW")
      remote_webui(&hooks);
      key(0x44);
      CHECK(is("WEB UI") && shown_text("ON") && shown_text("2468 1357") && shown_text("3") && shown_text("NEW CODE") &&
            shown_text("FORGET DEVICES") && shown_text("YES: CHANGE  NO: BACK"));
      png("webui_1_on.png");
      key(0x44);                                 // YES on WEB UI: off
      CHECK(!fk.on && shown_text("OFF") && shown_text("OFF: PAGE + 7310 REFUSE"));
      png("webui_2_off.png");
      key(0x44);
      CHECK(fk.on && shown_text("WEB UI ON"));
      key(0x43), key(0x43), key(0x43);           // DOWN x 3: NEW CODE
      key(0x44);
      CHECK(fk.news == 1 && shown_text("A NEW CODE: SEE ABOVE"));
      fk.gate = 2;
      key(0x44);
      CHECK(fk.news == 1 && shown_text("NOT ON A FIXED CODE") && shown_text("FIXED"));
      fk.gate = 1;
      key(0x43);                                 // FORGET DEVICES: YES asks, NO keeps
      key(0x44);
      CHECK(fk.forgets == 0 && shown_text("YES: FORGET ALL  NO: KEEP"));
      png("webui_3_forget.png");
      key(0x45);
      CHECK(fk.forgets == 0 && is("WEB UI") && shown_text("KEPT"));
      key(0x44), key(0x44);                      // YES, YES: forgotten
      CHECK(fk.forgets == 1 && shown_text("ALL DEVICES FORGOTTEN") && shown_text("0"));
      fk.gate = 0;                               // cmdline.txt paircode=0: the code row says so (no gate toggle here)
      remote_tick(t += 30);
      CHECK(shown_text("OFF (CMDLINE)"));
      key(0x45);                                 // NO: SIDECAR
      CHECK(!is("WEB UI") && remote_stats().page == 0);
      remote_webui(nullptr);
      remote_pair_code("");
      menu_reset();
      printf("ui: SIDECAR MENU > WEB UI: on / off, the code, a new code, forget the devices (YES twice)\n");
    }
  }

  float full = 0, one = 0;
  for (int which = 0; which < 5; ++which) {
    remote_bench(which, 0, 2000, &full, &one);
    printf("ui: page %d: whole page drawn + diffed %.1f us, one change %.1f us (Mac)\n", which, full, one);
  }
  printf("test_ui: PASS\n");
  return 0;
}
