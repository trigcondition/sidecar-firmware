// core/ui/page_midi.cpp - see extra.h: the MIDI page on the Rytm's screen (0.5.0, the owner: a user could not find the
// keyboards' routing modes). Cold. CORE_POLY builds only. Opened from the SIDECAR MENU's MIDI row (no key of its own:
// no rytm-mods change); off the LEFT / RIGHT ring. One row a setting, as WEB UI and USB-C (page_menu.cpp):
//   KEYBOARD ROUTE  SELECTED / CHANNEL   core/keys.h: the USB-A keyboards' (and the computer's) notes play the Rytm's
//                                        selected track, or channel N plays track N (1..12)
//   AUTO CHANNEL    1..16                the Rytm's auto channel as set on the Rytm (its MIDI CONFIG > CHANNELS)
//   COMPUTER NOTES  ON / OFF             the computer's notes (USB-C) routed here, or straight to the Rytm
//   MIDI LEARN      N ROUTES / NONE      core/mod/learn.h: YES twice clears them all (WAITING: a learn armed - YES stops
//                                        it; LOCKED: learn.txt has lines of a newer build, nothing is cleared)
//   USB-A           the devices in the Pi's USB-A ports (the first by name, +N more); read only
//   LAST            the last key's way ("MPKMINI > SD POLY", keys::line); read only
// Knob A or UP / DOWN pick a row, YES or knob B change it. Each change is the console's own command (keys=...), takes
// effect at once and is saved by idle_save (/.rytm/keys.txt, /.rytm/learn.txt). Arming a learn stays the console's
// (learn=fader|a|b|NAME[:DEPTH]): its targets are a long list.
#if CORE_POLY
#include <stdio.h>
#include <string.h>

#include "keys.h"
#include "mod/learn.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

enum : int { kYes = 0x44, kNo = 0x45, kUp = 0x42, kDown = 0x43 };
enum : int { kRoute, kAuto, kMac, kLearn, kUsbA, kLast, kRows };
constexpr int kRowY = 11, kRowH = 7;

struct MidiPage {
  bool on;            // shown (the MENU's row) and not left
  int cur;            // the row under the cursor
  bool confirm;       // MIDI LEARN's YES asked once: YES again clears
  bool note_new;      // a note set by a key or a turn: timed from the next frame (the frame's clock)
  uint32_t note_ms;
  char note[26];
};
MidiPage P;

CORE_COLD void note(const char* t) {
  sniprintf(P.note, sizeof P.note, "%s", t);
  P.note_new = true;
}
CORE_COLD void keys_cmd(const char* c) {
  char o[96];
  keys::command(c, o, sizeof o);
}
CORE_COLD int learn_routes() {
  const learn::Route* r = nullptr;
  return learn::routes(&r);
}

// a change of the row's setting: dir +1 / -1 (knob B, clamped), 0 the next value (YES, wrapping)
CORE_COLD void change(int dir) {
  char b[24];
  if (P.cur == kRoute) {
    const bool ch = keys::view().mode == keys::kChannel;
    const bool want = dir > 0 ? true : dir < 0 ? false : !ch;
    if (want == ch) return;
    keys_cmd(want ? "keys=channel" : "keys=selected");
    note(want ? "CH 1-12 PLAY TRACKS 1-12" : "NOTES: THE SELECTED TRACK");
  } else if (P.cur == kAuto) {
    int a = keys::auto_channel(), n = dir ? a + dir : a % 16 + 1;
    n = n < 1 ? 1 : n > 16 ? 16 : n;
    if (n == a) return;
    sniprintf(b, sizeof b, "keys=auto:%d", n);
    keys_cmd(b);
    note("SET THE RYTM'S TO MATCH");
  } else if (P.cur == kMac) {
    const bool on = keys::computer_on();
    const bool want = dir > 0 ? true : dir < 0 ? false : !on;
    if (want == on) return;
    keys_cmd(want ? "keys=mac:on" : "keys=mac:off");
    note(want ? "COMPUTER: SIDECAR ROUTES" : "COMPUTER: TO THE RYTM");
  }
}

CORE_COLD void learn_yes() {
  if (learn::armed() >= 0) {   // a learn waits for its CC (the console's learn=...): YES stops it
    learn::disarm();
    P.confirm = false;
    note("LEARN STOPPED");
    return;
  }
  const int n = learn_routes();
  if (!n) { P.confirm = false; note("ARM IT: CONSOLE LEARN="); return; }   // (the routes went meanwhile: no question left)
  if (learn::locked()) { P.confirm = false; note("LEARN FILE IS NEWER"); return; }
  if (!P.confirm) { P.confirm = true; return; }
  P.confirm = false;
  while (learn::clear(0)) {}
  note("LEARN ROUTES CLEARED");
}

CORE_COLD void row_text(int i, const char** name, char* v, int cap) {
  v[0] = 0;
  switch (i) {
    case kRoute:
      *name = "KEYBOARD ROUTE";
      sniprintf(v, cap, "%s", keys::view().mode == keys::kChannel ? "CHANNEL" : "SELECTED");
      break;
    case kAuto: *name = "AUTO CHANNEL"; sniprintf(v, cap, "%d", keys::auto_channel()); break;
    case kMac: *name = "COMPUTER NOTES"; sniprintf(v, cap, "%s", keys::computer_on() ? "ON" : "OFF"); break;
    case kLearn: {
      *name = "MIDI LEARN";
      const int n = learn_routes();
      if (learn::armed() >= 0) sniprintf(v, cap, "WAITING");
      else if (!n) sniprintf(v, cap, "NONE");
      else if (learn::locked()) sniprintf(v, cap, "%d LOCKED", n);
      else sniprintf(v, cap, "%d ROUTE%s", n, n == 1 ? "" : "S");
      break;
    }
    case kUsbA: {
      *name = "USB-A";
      int first = -1, more = 0;
      for (int s = 0; s < keys::kComputer; ++s)
        if (keys::source_name(s)[0]) first < 0 ? (void)(first = s) : (void)++more;
      if (first < 0) sniprintf(v, cap, "NONE");
      else if (more) sniprintf(v, cap, "%.8s +%d", keys::source_name(first), more);
      else sniprintf(v, cap, "%.8s", keys::source_name(first));
      break;
    }
    case kLast: {
      *name = "LAST";
      char l[40];
      const char* ln = keys::view().src >= 0 ? keys::line(l, sizeof l) : nullptr;
      sniprintf(v, cap, "%s", ln ? (strncmp(ln, "KEYS ", 5) ? ln : ln + 5) : "-");
      break;
    }
  }
}

CORE_COLD void midi_draw(Canvas& c, int page, int npages, uint32_t ms) {
  c.clear();
  header(c, "MIDI", page, npages, nullptr);
  if (P.cur < 0) P.cur = 0;
  if (P.cur >= kRows) P.cur = kRows - 1;
  for (int i = 0; i < kRows; ++i) {
    const char* name = "";
    char v[24];
    row_text(i, &name, v, sizeof v);
    const bool on = i == P.cur;
    const int y = kRowY + i * kRowH;
    if (on) c.fill(Rect{0, (int16_t)(y - 1), 128, kRowH}, kOn);
    const uint8_t inv = on ? kTextInvert : 0;
    c.put_text(kSlotFree + 2 * i, kRSmall, 2, y, name, (uint8_t)(kAlignLeft | inv));
    c.put_text(kSlotFree + 2 * i + 1, kRSmall, 126, y, v[0] ? v : nullptr, (uint8_t)(kAlignRight | inv));
  }
  if (P.confirm) footer(c, "YES: CLEAR ALL  NO: KEEP");
  else if (P.note_new || (P.note_ms && ms - P.note_ms < 2000)) footer(c, P.note);
  else if (P.cur <= kMac) footer(c, "YES / B: SET  NO: BACK");
  else if (P.cur == kLearn && learn::armed() >= 0) footer(c, "YES: STOP LEARN  NO: BACK");
  else if (P.cur == kLearn && learn_routes() && !learn::locked()) footer(c, "YES TWICE: CLEAR ALL");
  else footer(c, "NO: BACK");
  if (P.note_new) P.note_new = false, P.note_ms = ms | 1;
}

CORE_COLD void move(int d) {
  if (!d) return;
  P.cur += d, P.confirm = false;
  if (P.cur < 0) P.cur = 0;
  if (P.cur >= kRows) P.cur = kRows - 1;
}
CORE_COLD void midi_enc(int e, int delta, bool) {
  const int l = e <= 1 ? list_turn(e, delta) : 0;
  if (e == 0) move(l);
  else if (e == 1 && l) change(l > 0 ? 1 : -1);   // a step a report, by its sign (unpaced reports carry the counts)
}
CORE_COLD bool midi_key(int code) {
  if (code == kUp || code == kDown) { move(code == kUp ? -1 : 1); return true; }
  if (code == kNo) {
    if (P.confirm) { P.confirm = false; note("KEPT"); return true; }
    P.on = false;
    return false;   // (remote.cpp: NO goes to SIDECAR)
  }
  if (code != kYes) return false;
  if (P.cur == kLearn) learn_yes();
  else change(0);
  return true;
}
CORE_COLD bool midi_shown() { return P.on; }

}  // namespace

UI_ROM const ExtraPage kMidiPage = {"MIDI", midi_shown, midi_draw, midi_enc, midi_key, 0x03, false, nullptr, nullptr,
                                    nullptr, true, nullptr, nullptr, true};

CORE_COLD void midi_page_open() {
  P.on = true, P.cur = 0, P.confirm = false, P.note_new = false, P.note_ms = 0;
  remote_show_page("MIDI");
}
CORE_COLD void midi_page_reset() { P = MidiPage(); }

}  // namespace ui
}  // namespace core
#endif
