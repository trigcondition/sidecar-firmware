// core/mod/scene.h - morph scenes (spec-050 section 2): twelve scenes a kit (the A4: a project), each up to 64 locks of
// any modulation target (core/mod/target.h) at an absolute value, as the Octatrack's; two of them, A and B, on the
// ends of one fader 0..16383. Each tick the morph hands the layer (mod::morph) one value per lock of A and B:
//   in A and B   a + (b - a) x f            A only  a + (base - a) x f        B only  base + (b - base) x f
// with f the smoothed fader / 16383 and base what the target plays without the morph (its knob, a p-lock, the Rytm's
// own scene underneath). A stepped target (a step-mode kind, a built-in page's list knob) takes one side whole: it
// switches at halfway, up at f >= 0.5 + 1/64 and down at f < 0.5 - 1/64. The morph is never written anywhere but the
// layer's writers: no NRPN, no 0x03, no kit, no pattern field (spec 2.3). Control side (core 3 on the Pi), cold.
#pragma once
#include <stdint.h>

#include "mod/target.h"

namespace core {
namespace scene {

constexpr int kScenes = 12, kLocks = 64;
constexpr uint16_t kFaderMax = 16383;
constexpr uint32_t kSmoothMs = 20;   // the played fader reaches a new value in this long (linear)
struct Lock {
  mod::Tid t;
  uint16_t v;   // the value x 128 (0..16383): 0021's encoding, as the sequencer's locks
};
struct Scene {
  uint8_t n;    // locks in use, l[0..n)
  Lock l[kLocks];
};
struct State {
  Scene s[kScenes];
  int8_t a, b;       // 0..11, -1 none
  uint16_t fader;    // 0..16383 as set (A at 0); fader_played() is the smoothed one the morph uses
  int8_t sel;        // the scene the page shows and edits, 0..11
};
// the fader's sources (spec 2.4): whichever moved last wins
enum Source : uint8_t { kPage = 1, kTouch, kWeb, kLearn, kQperf, kConsole };

const State& state();
bool lock(int n, mod::Tid t, uint16_t v);   // scene n (0..11) locks t at v (replaces its lock); false: full, or bad
bool unlock(int n, mod::Tid t);             // false: no such lock
void clear(int n);
void copy(int from, int to);
void set_ab(int a, int b);                  // -1 none; the fader does not move
void set_sel(int n);
void set_fader(uint16_t v, int source);     // 0..16383, absolute; a change frees every held target (mod::fader_moved)
uint16_t fader_played();
void tick(uint32_t ms);                     // mod::tick, once a millisecond at most: the morph's values to the layer
void reset();                               // tests: no locks, no A / B, the fader at 0

}  // namespace scene
}  // namespace core
