// core/ui/page_song.cpp - see extra.h: the SONG page (SONG RECORD, core/song/song.h) on the Rytm's screen. Cold;
// CORE_POLY builds only.
//
// The title bar: SIDECAR SONG and the state (OFF / ARMED / REC / REPLAY ...). Under it the take (name, events), the
// position (bar.step) and the pattern / tempo, a position bar over the take's length, the MIDI CONFIG's problems (what
// stands in the way of recording or replay, never changed by the Sidecar), the last message. Keys: T1 ARM / DISARM,
// T2 REPLAY (asks: YES restores the start state and arms the replay, NO cancels) / replay off, T3 MARK, T4 the MIDI
// CONFIG how-to (YES / NO back), T5 UNDO REPLAY (the replay's backup put back; lit and in the footer only while
// offered: song.h). Start / stop is always the Rytm's own PLAY / STOP.
#if CORE_POLY
#include <string.h>

#include "platform.h"
#include "song/song.h"
#include "ui/extra.h"
#include "ui/page.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

CORE_COLD const char* short_state(int s) {
  static const char* const k[] = {"IDLE", "ARMING", "ARMED", "REC", "SAVING", "REPLAY?", "RESTORE", "READY", "REPLAY", "LOADING",
                                  "UNDO"};
  return s >= 0 && s <= song::kUndoing ? k[s] : "?";
}

// song::status() once a frame (review 12: draw, trigs and LEDs each asked it; it formats and looks in the RAM store)
CORE_COLD const song::Status& st() {
  static song::Status s;
  static uint32_t at = 0;
  static bool have = false;
  uint32_t ms = now_ms();
  if (!have || ms != at) s = song::status(), at = ms, have = true;
  return s;
}

CORE_COLD void upper(char* s) {
  for (; *s; ++s)
    if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

CORE_COLD void song_draw(Canvas& c, int page, int npages, uint32_t ms) {
  c.clear();
  const song::Status& s = st();
  header(c, "SIDECAR SONG", page, npages, short_state(s.state));
  char b[kTextLen + 1];
  if (song::ui_question() == 2) {   // the how-to
    for (int i = 0; i < 6; ++i) {
      const char* l = song::config_explain(i + 2);
      if (l) c.put_text(kSlotFree + i, kRSmall, 1, 10 + 7 * i, l);
    }
    footer(c, "YES / NO: BACK");
    return;
  }
  if (s.state == song::kConfirm) {
    c.put_text(kSlotFree, kRSmall, 64, 12, "REPLAY THIS TAKE?", kAlignCentre);
    sniprintf(b, sizeof b, "%s", s.name);
    upper(b);
    c.put_text(kSlotFree + 1, kRSmall, 64, 20, b, kAlignCentre);
    c.put_text(kSlotFree + 2, kRSmall, 64, 30, "OVERWRITES THE WORKING", kAlignCentre);
    c.put_text(kSlotFree + 3, kRSmall, 64, 37, s.settings ? "KIT, PATTERN, SETTINGS" : "KIT AND PATTERN", kAlignCentre);
    c.put_text(kSlotFree + 4, kRSmall, 64, 44, "(BACKED UP FIRST)", kAlignCentre);
    footer(c, "YES: RESTORE   NO: CANCEL");
    return;
  }
  // the take. While it records or replays the page changes at most about once a second: every committed frame makes
  // the Rytm run its whole stock draw and panel diff under the page (rytm-mods 0026 design.md, ev_tick), so a page
  // redrawn per recorded event (a knob turned: 25 frames a second) made its own menus (MUTE, the pattern list) slow
  // [measured: core/tests/test_traffic.cpp, docs/SONG.md "Traffic to the Rytm"]. So: the event count at most once a
  // second, the bar without its beat (and the position bar by whole bars), the record dot steady.
  bool live = s.state == song::kRec || s.state == song::kReplaying;
  static uint32_t ev_shown = 0, ev_ms = 0;
  if (!live || ms - ev_ms >= 1000 || s.events < ev_shown) ev_shown = s.events, ev_ms = ms;
  if (s.name[0]) sniprintf(b, sizeof b, "%s %u EV%s", s.name, (unsigned)ev_shown, s.recovered ? " REC'D" : "");
  else sniprintf(b, sizeof b, "NO TAKE (%d ON CARD)", s.takes);
  upper(b);
  c.put_text(kSlotFree, kRSmall, 1, 11, b);
  uint32_t t = live ? s.t_now / song::kBar * song::kBar : s.length;
  int pat = s.pattern < 0 ? 0 : s.pattern;
  uint32_t h = (s.tempo * 10 + 60) / 120;
  if (live)
    sniprintf(b, sizeof b, "BAR %u %c%02d %u.%u BPM", (unsigned)(t / song::kBar + 1), 'A' + pat / 16, pat % 16 + 1,
              (unsigned)(h / 10), (unsigned)(h % 10));
  else
    sniprintf(b, sizeof b, "BAR %u.%u %c%02d %u.%u BPM"   /* one space: bar 100+ fits */, (unsigned)(t / song::kBar + 1),
              (unsigned)(t % song::kBar / song::kPpq + 1), 'A' + pat / 16, pat % 16 + 1, (unsigned)(h / 10), (unsigned)(h % 10));
  c.put_text(kSlotFree + 1, kRSmall, 1, 19, b);
  // the position bar
  c.rect(Rect{0, 27, 128, 6}, kOn);
  uint32_t len = s.state == song::kRec || s.length < t ? t : s.length;   // (recording: the whole bar, the take is as long as t)
  if (len) {
    int w = (int)((uint64_t)t * 126 / len);
    if (w > 0) c.fill(Rect{1, 28, (int16_t)w, 4}, s.state == song::kRec ? kOn : kDither);
  }
  if (s.state == song::kRec) c.fill(Rect{122, 11, 5, 5}, kOn);   // the record dot (steady: a blink is 2 frames a second)
  // the MIDI CONFIG
  char p[64];
  song::config_text(s.problems, p, sizeof p);
  if (s.undo) {   // UNDO REPLAY offered (30 s at most): named in the MIDI CONFIG's row (T4 still lit when it is wrong)
    c.put_text(kSlotFree + 2, kRSmall, 1, 36, "T5: UNDO REPLAY");
  } else if (p[0]) {
    sniprintf(b, sizeof b, "! %s", p);
    c.put_text(kSlotFree + 2, kRSmall, 1, 36, b);
  } else {
    c.put_text(kSlotFree + 2, kRSmall, 1, 36, "MIDI CONFIG OK");
  }
  char m[sizeof s.msg];   // the last message in two lines (rows 43 / 49: clear of the bottom bar)
  sniprintf(m, sizeof m, "%s", s.msg);
  upper(m);
  text_lines(c, kSlotFree + 3, 2, 1, 43, 6, m);
  const char* f = s.state == song::kOff ? "T1:ARM T2:REPLAY T4:CFG"
                  : s.state == song::kArming || s.state == song::kArmed ? "PLAY: REC  T1: DISARM"
                  : s.state == song::kRec ? "STOP: SAVE  T3: MARK"
                  : s.state == song::kReplayReady ? "PLAY: REPLAY  T2: OFF"
                  : s.state == song::kReplaying ? "STOP: END  T2: OFF" : "...";
  footer(c, f);
}

CORE_COLD bool song_shown() { return song::page_shown(); }
CORE_COLD void song_enc(int, int, bool) {}
CORE_COLD bool song_key(int code) { return song::ui_key(code); }
CORE_COLD uint16_t song_trigs() { return st().undo_t5 ? 0x001F : 0x000F; }   // (T5: and a second after the undo ended)
CORE_COLD bool song_leds(uint8_t c[16]) {
  memset(c, kLedOff, 16);
  int s = st().state;
  c[0] = s == song::kRec ? 15 : s == song::kArmed || s == song::kArming ? 8 : 3;          // red ramp
  c[1] = s == song::kReplaying ? 31 : s == song::kReplayReady || s == song::kConfirm ? 24 : 19;   // green
  c[2] = kLedBlueDim;
  c[3] = st().problems ? kLedOrange : kLedBlueDim;
  if (st().undo) c[4] = 24;   // T5 UNDO REPLAY: green while offered (dark otherwise)
  return true;
}

}  // namespace

UI_ROM const ExtraPage kSongPage = {"SONG", song_shown, song_draw, song_enc, song_key, 0, false, nullptr, song_trigs, song_leds};

}  // namespace ui
}  // namespace core
#endif
