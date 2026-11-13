// core/mod/xypad.h - the XY pad (0.5.0): the touchscreen's XY page as a "Kaoss pad" controller. Each axis moves up to
// kSlots targets, each with its own depth -100..+100 % (negative: inverted): any modulation target (core/mod/target.h,
// as an LFO's) or the scenes' morph fader (kMorph, "MORPH"). A slot is a source of the layer (kSource), as an LFO:
//   a target with a base the layer reads (kV kFx kSnd, the offset kinds): offset = depth x position x its range's span
//     (an offset kind: 127), added to the base, summed with the LFOs' and MIDI learn's, clamped by the layer
//   a value-only target (kSeq kArp kLoop kRytm: no base the Sidecar reads): the pad sets it from its range's low end
//     (depth > 0) or its high end (depth < 0): lo + depth x position x span, or hi + depth x position x span
//   MORPH: the fader set directly (scene::set_fader, kTouch): its position before the touch + depth x position x 16383
// with position 0..1 (0..16383 as the commands give it). The release (`up`) with HOLD off: every offset back to 0 (the
// base is never touched), a RYTM target once to its zero point (the value at position 0: the Rytm keeps what was sent
// last), MORPH back to its position before the touch (unless another source moved the fader since). HOLD on: the
// values stay where the finger lifted, until the next touch or HOLD off. A kit or project change while touching
// releases first (as an `up`, then a held state dropped: the old kit's slots); the finger's next positions are ignored
// until its `up`. Control side (core 3 on the Pi), cold. The UI (core 0) only ever sends the console commands below.
#pragma once
#include <stdint.h>

#include "mod/target.h"

namespace core {
namespace xypad {

constexpr int kSlots = 4;
constexpr int kAxes = 2;                  // 0 X, 1 Y
constexpr uint16_t kMax = 16383;          // a position's top
constexpr int kSource = 58;               // the layer's source id (mod.h: 58 the XY pad)
constexpr mod::Tid kMorph = 0xFFFF;       // the scenes' fader: not a layer target (mod::valid is false for it)
struct Slot {
  mod::Tid t;      // 0 none
  int8_t depth;    // -100..100 %
};
struct Cfg {
  Slot s[kAxes][kSlots];
  bool hold;
};

const Cfg& cfg();
bool set(int axis, int slot, mod::Tid t, int depth);   // false: bad axis / slot / depth, or no such target
bool clear(int axis, int slot);
void set_hold(bool on);                                 // off while a held state plays: released (as an `up`)
void pos(uint16_t x, uint16_t y);                       // a touch or a drag (each 0..kMax, clamped)
void up();                                              // the finger lifted (or the press was lost)
bool touching();
bool active();                                          // touching, or HOLD's held state
uint16_t x();
uint16_t y();
void tick();                                            // mod::tick: the slots' offsets
const char* target_name(mod::Tid t, char* b, int cap);  // "MORPH", "RYTM:PERF1"; "" when none / not valid
bool parse_target(const char* s, mod::Tid* out);        // "MORPH" or a target name, any case

// ---- the console (the UI's only way in): "xy" (the state), "xy=pos:X:Y" (0..16383 each), "xy=up",
// "xy=set:x|y:SLOT:TARGET:DEPTH" (SLOT 1..4, DEPTH -100..100), "xy=clear:x|y:SLOT", "xy=hold:0|1". pos and up answer
// nothing (out ""; they come every 30 ms while a finger drags), the others the state. False: not "xy..."
bool command(const char* p, char* out, int cap);

// ---- the files, as the scenes' (core/mod/scene.h): per kit /.rytm/xy/<project>/kNNN.txt (kit 1..128), the project's
// default /.rytm/xy/<project>.txt (a kit without its own file; the setup while the Rytm has named no kit). Read from
// the RAM store only, written by idle_save a second after the last edit (a position is no edit). Text, '#' comments:
//   #sidecar xy 1
//   H 1                  HOLD on
//   X 1 RYTM:PERF1 100   X's slot 1: PERF macro 1 at +100 %
//   Y 2 MORPH -50        Y's slot 2: the morph fader at -50 %
// A line this build does not read (an unknown keyword or target name, a slot > 4, a depth out of range) locks the file:
// what it can read is loaded, the file is never written again this session (1042), locked() says so.
#if CORE_MSAMP
void want_files();                  // the project's 129 paths into the coming store fill
void rekey(const char* name);       // the setup in memory is now name's (a rename moved the files)
#endif
void load();                        // the kit's file, else the project's, else none (the store only)
bool save();                        // idle_save: true if it wrote (or settled a locked file)
void kit_changed(int kit);          // the Rytm's active kit (0..127): released, the old kit's edits saved, the new loaded
void project_changed();             // released, the old project's (kit's) edits saved (queued), the new loaded
void save_due();                    // the unsaved edits due now (a rename's flush, a restart)
bool dirty();
bool locked();
int kit();                          // the kit the setup is of, -1 none named
void reset();                       // tests: no slots, HOLD off, nothing touching

}  // namespace xypad
}  // namespace core
