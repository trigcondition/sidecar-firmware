// core/fx/knob_labels.h - what the Rytm's SEND FX pages show for an effect's P2..P5 (rytm-mods 0029-sendfx-ui): four
// short labels and, for a knob whose value picks a zone (a model, a character, a note length), the zones' names.
// Sent as 0x24 SENDFX LABELS (sim/protocol.py sendfx_labels), one message a TYPE, on 0x23 ASK and on a project change.
// Each effect keeps its table next to its knob documentation (send_fx.h Chorus / Comb, rings_fx.h, cloudseed_fx.h,
// stutter_fx.h). Data only: no code here.
#pragma once
#include <stdint.h>

namespace core {
namespace fx {

// A zone: knob k (0..3 = P2..P5) from value lo up to the next zone's lo - 1 of the same knob (or 127). With show, the
// Rytm prints the name and v - lo + base (RINGS P3 "MODAL 12"); without, the name alone (STUTTER P3 "1/16").
// step (2026-10-18, 0x24 bit 3): the knob is a pure zone selector - the Rytm steps it one zone per detent like its own
// enum knobs (LOOP, filter TYPE) and stores each zone's lo. Set it on every zone of such a knob; a knob with a number
// inside its zones (show) stays continuous. A Rytm without it (0029 before sfx-enum-knobs) ignores the bit.
struct Zone {
  uint8_t knob, lo, base, show, step;
  const char* name;   // <= 5 characters
};
// long: the full names (0x51 SENDFX FULL NAMES, rytm-mods 0029 sendfx-full-names): what the Rytm shows in the upper left while
// a knob turns, as the stock DELAY / REVERB pages do ("Feedback"). Null: the Rytm keeps "Send FX n" there.
struct KnobLabels {
  const char* label[4];   // P2..P5, <= 4 characters (as the stock pages' labels)
  const Zone* zones;      // ascending lo within each knob
  uint8_t nzones;
  const char* long_name[4];   // P2..P5, <= kLongChars characters; null or "" for none
};
constexpr int kMaxZones = 12;    // a message's cap (the Rytm's table holds as many per TYPE)
constexpr int kLabelChars = 4, kZoneChars = 5, kLongChars = 16;

}  // namespace fx
}  // namespace core
