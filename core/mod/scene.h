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
int fader_source();                         // the source that set the fader last (Source), 0 none since the reset

// ---- the Rytm's QUICK PERF knob as the fader (spec-050 2.4.5, 5.1; plan-050 Task 47): the mod's 2D 01 0E report.
// Absolute, left end = A, no pickup; ignored until the knob has sent since the Rytm booted (kQpMoved: before, its
// position reads 0 whatever the knob says). Smoothed over the measured interval between reports (at least kSmoothMs,
// at most kQpSmoothMaxMs; a first report after a pause: kSmoothMs), so a turn does not stair-step.
constexpr uint8_t kQpFunc = 1, kQpMoved = 2, kQpMuted = 4;   // flags: FUNC held, moved since boot, macros muted
constexpr uint32_t kQpSmoothMaxMs = 100;
void qperf(int pos, uint16_t mask, uint8_t flags);   // pos 0..255, mask the PERF macros on the knob (bit m - 1)
uint16_t qperf_mask();                      // those macros while the knob is the fader's source (PERF follow skips
                                            // them: the Rytm moves them itself, spec 2.7), else 0
// the Rytm's own scene as the mod reports it (2D 01 0F, on any change: its pads, CC 92, the host command, a kit load):
void rytm_scene_seen(int n);                // 1..12, 0 none
int rytm_scene();                           // the last one seen, -1 none since the reset (an older mod: never)
void tick(uint32_t ms);                     // mod::tick, once a millisecond at most: the morph's values to the layer
void reset();                               // tests: no locks, no A / B, the fader at 0

// ---- the files (spec-050 2.8), as the LFOs' (core/lfo.h): per kit /.rytm/scene/<project>/kNNN.txt (kit 1..128), the
// project's default /.rytm/scene/<project>.txt (a kit without its own file, and the scenes while the Rytm has named no
// kit; the A4's only one: its state folder makes it /.a4/scene/<project>.txt). Read from the RAM store only (never the
// card on the control core), written by idle_save a second after the last edit as one posted atomic write (queued when
// over fmt::kPostMax). Text, one item a line, '#' comments:
//   #sidecar scene 1
//   AB 3 7          A, B (1..12, 0 none)
//   F 8192          the fader (its last position, written with the next edit's save: a fader move alone is no edit)
//   S 3             the selected scene
//   L 3 V1:P3 6400  scene 3 locks V1's P3 at 50.0 (the value x 128)
// A line this build does not read (an unknown name or keyword, a value out of range, a scene > 12, a 65th lock) locks
// the file: what it can read is loaded, the file is never written again this session (1042), locked() says so.
#if CORE_MSAMP
void want_files();                          // the project's 129 paths into the coming store fill
void rekey(const char* name);               // the scenes in memory are now name's (a rename moved the files)
#endif
void load();                                // the kit's file, else the project's, else none (the store only)
bool save();                                // idle_save: true if it wrote (or settled a locked file)
void kit_changed(int kit);                  // the Rytm's active kit (0..127): the old kit's edits saved, the new loaded
void project_changed();                     // the old project's (kit's) edits saved (queued), the new loaded
void save_due();                            // the unsaved edits due now (a rename's flush, a restart)
bool dirty();
bool locked();
int kit();                                  // the kit the scenes are of, -1 none named

// ---- the MORPH page's EDIT (spec-050 2.6: hold a scene key and turn a knob on any Sidecar page locks it; Phase 6)
void set_edit(bool on);
bool edit();
// ---- the console and /status (spec-050 2.10): "scene" (the state), "scene=ab:A:B" (1..12, 0 none), "scene=fader:V"
// (0..16383), "scene=sel:N", "scene=lock:N:NAME:V" (V the target's value, 0..127.99, kept x 128), "scene=unlock:N:NAME",
// "scene=clear:N", "scene=copy:N:M", "scene=edit:on|off"; each edit answers with the state. False: not "scene..."
bool command(const char* p, char* out, int cap);
// "\"scene\":{\"a\":A,\"b\":B,\"fader\":F,\"played\":P,\"sel\":S,\"locked\":0|1,\"edit\":0|1,\"s\":[{\"n\":N,\"l\":[[\"V1:P3\",6400],...]},...],\"src\":SRC,\"rscn\":R}"
// (SRC the fader's last source, 0 none; R the Rytm's own scene 1..12, 0 none, -1 not reported)
// (A, B, S, N 1-based, 0 none; only the scenes with locks): its length, cap when it did not fit (a 768-lock state fits 16 KB)
int json(char* out, int cap);

}  // namespace scene
}  // namespace core
