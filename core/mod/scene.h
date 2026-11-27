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
constexpr uint32_t kHandoverMs = 200;   // ... and in this long when the fader's source changes (v2 Task 6, UX 2b)
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
bool lock(int n, mod::Tid t, uint16_t v);   // scene n (0..11) locks t at v (replaces its lock); false: full, or bad, or
                                            // kRytm (0.5.0 v2, S7: the Rytm's scene / PERF are no scene targets)
bool unlock(int n, mod::Tid t);             // false: no such lock
void clear(int n);
void copy(int from, int to);
void set_ab(int a, int b);                  // -1 none; the fader does not move
void set_sel(int n);
void set_fader(uint16_t v, int source);     // 0..16383, absolute; a change frees every held target (mod::fader_moved);
                                            // a source other than the last one glides over kHandoverMs
uint16_t fader_played();
int fader_source();                         // the source that set the fader last (Source), 0 none since the reset

// ---- the Rytm's QUICK PERF knob as the fader (spec-050 2.4.5, 5.1; plan-050 Task 47): the mod's 2D 01 0E report.
// Absolute, left end = A, no pickup; ignored until the knob has sent since the Rytm booted (kQpMoved: before, its
// position reads 0 whatever the knob says). The first position after the link comes up (qperf_rebase: a hello, a USB
// edge) is a baseline only: the knob takes the fader when its position changes from it (Phase 7 review M2), so the
// hello's report never moves the fader nor overrides a kit file's F. While another source has the fader, or a held
// manual edit plays, the knob takes it on a change of kQpTakeSteps positions from the one it acted on last (M1: a
// pot's 1-step flicker does not steal the fader or let a held edit go); once it has the fader it follows every step. Smoothed over the measured interval between reports (at least kSmoothMs,
// at most kQpSmoothMaxMs; a first report after a pause: kSmoothMs), so a turn does not stair-step.
constexpr uint8_t kQpFunc = 1, kQpMoved = 2, kQpMuted = 4;   // flags: FUNC held, moved since boot, macros muted
constexpr uint32_t kQpSmoothMaxMs = 100;
constexpr int kQpTakeSteps = 2;
// QPERF FADER (0.5.0 v2 Task 6, the MIDI page's row, learn.txt's "QP" line): MUTED ONLY (the default) takes the
// knob's position only while the Rytm's macros are muted (FUNC + QPER: flags bit 2), OFF never, ALWAYS as above. Bit 2
// is trusted only from a mod whose hello's caps2 has ui::kCaps2QpMute; without it MUTED ONLY acts as OFF. A change of
// bit 2 (a mute edge) makes the next position a baseline; the first take after a baseline glides over kHandoverMs.
enum QpMode : uint8_t { kQpMutedOnly, kQpOff, kQpAlways };
void set_qp_mode(int m);                    // QpMode; anything else: MUTED ONLY
int qp_mode();
const char* qp_mode_name(int m);            // "muted", "off", "always"
void qperf(int pos, uint16_t mask, uint8_t flags);   // pos 0..255, mask the PERF macros on the knob (bit m - 1)
void qperf_rebase();                        // the link came up (host_link's link_reset): the next position a baseline
// the screens' QPERF readout (v2 Task 7): the knob's last position 0..255 (-1 none since the reset), whether it has sent
// since the Rytm booted, the macros' mute (trusted: kCaps2QpMute), when a move was last ignored for the macros playing
// (MUTED ONLY; 0 never); qp_wants_knob: ALWAYS, or MUTED ONLY from a mod that reports the mute
struct QpView {
  int pos;
  bool moved, muted;
  uint32_t ignored_ms;
};
QpView qperf_view();
bool qp_wants_knob();
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
//   #sidecar scene 2 (0.5.0 v2: a v1 file, without RYTM: locks since, reads as a newer one: loaded, locked)
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
int rytm_kit();                             // the Rytm's active kit 0..127 as it last said (kit_changed), -1 none
int kit_note(uint32_t now_ms);              // the Rytm's kit 0..127 when it changed in the last 2 s (the MORPH header), -1
void kit_changed(int kit);                  // the Rytm's active kit (0..127): the old kit's edits saved, the new loaded;
                                            // a held EDIT ends
void project_changed();                     // the old project's (kit's) edits saved (queued), the new loaded
void save_due();                            // the unsaved edits due now (a rename's flush, a restart)
bool dirty();
bool locked();
int kit();                                  // the kit the scenes are of, -1 none named

// ---- EDIT (0.5.0 v2, the owner 2026-11-14: momentary, Octatrack-style): PAGE + T15 held = scene A, PAGE + T16 = scene
// B; while that trig is held a knob turned on a Sidecar page locks into that side's scene, and T1..T12 make scene n that
// side. The host's key handling (ui/remote.cpp) sets it; the trig's release, a hello or a kit change ends it.
void set_edit_side(int side);               // 0 A, 1 B, -1 none
int edit_side();
inline bool edit() { return edit_side() >= 0; }
int side_scene(int side);                   // A's (0) or B's (1) scene 0..11, -1 none
bool lock_side(int side, mod::Tid t, uint16_t v);   // lock() into that side's scene; false also when the side has none
// ---- EDIT on the Rytm's own pages (0.5.0 v2 P2, rytm-mods 0026 "ms", the hello's caps2 ui::kCaps2Capture): the mod
// catches PAGE + T15 / T16 there and reports the gesture (2D 01 13: stock_edit) and each turn of a lockable parameter
// under it (2D 01 12: capture) instead of writing the kit. stock_edit: ev 1 the side (0 A, 1 B) held, 0 let go, 2 scene
// n (0..11) picked for it; each answers with a readout on the Rytm's screen (mod::rytm_readout). capture: track 0..11
// (12 the FX track), the Rytm's sound word 1..41, delta in detents (FUNC: fine), base the kit's value x 128 (0..16383).
// The target: RTn / RFX (mod::rytm_tid), a Sidecar machine's words 9..24 its voice's P1..P16 (kV). The side's lock moves
// from its value, else from base: delta x 128 (FUNC x 8), a stepped target one entry a detent. Refused (a readout, no
// lock) while the side is not held (a kit change ended it), its scene unset (SET A FIRST), or the scene full. True: locked.
enum : uint8_t { kEditLetGo = 0, kEditHeld = 1, kEditPicked = 2 };
void stock_edit(int ev, int side, int n);
bool capture(int side, int track, int word, int delta, bool func, uint16_t base);
// 0.5.2: a DATA ENTRY knob pressed under the same gesture (2D 01 14 <side> <t> <w>; the Rytm eats the press, as it does the
// turns, so stock's p-lock clear does not run) removes that parameter's lock from that side's scene - only that entry,
// nothing else: the target is the turn's (the same track / word -> Tid rule). A readout like "A FRQ CLR"; "A FRQ NONE"
// when the scene had none. Refused (a readout, nothing removed) as a turn is: the gesture ended, the side's scene unset,
// a word with no target. True: a lock removed. An older host never sees the op's effect: it ignores the unknown 2D 01 14
// (the press is eaten on the Rytm and nothing else happens).
bool capture_clear(int side, int track, int word);
// ---- the console and /status (spec-050 2.10): "scene" (the state), "scene=ab:A:B" (1..12, 0 none), "scene=fader:V"
// (0..16383), "scene=sel:N", "scene=lock:N:NAME:V" (V the target's value, 0..127.99, kept x 128), "scene=unlock:N:NAME",
// "scene=clear:N", "scene=copy:N:M"; each edit answers with the state. "scene=qperf:muted|off|always" (QPERF FADER)
// answers "scene: qperf MODE". False: not "scene..."
bool command(const char* p, char* out, int cap);
// "\"scene\":{\"a\":A,\"b\":B,\"fader\":F,\"played\":P,\"sel\":S,\"locked\":0|1,\"edit\":0|1|2,\"s\":[{\"n\":N,\"l\":[[\"V1:P3\",6400],...]},...],\"src\":SRC,\"rscn\":R}"
// (edit: 0 none, 1 A held, 2 B held; SRC the fader's last source, 0 none; R the Rytm's own scene 1..12, 0 none, -1 not
// reported; on the Pi also "pl", "vl" and "gone": the locked targets that no longer resolve, v2 Task 7)
// (A, B, S, N 1-based, 0 none; only the scenes with locks): its length, cap when it did not fit (a 768-lock state fits 16 KB)
int json(char* out, int cap);

}  // namespace scene
}  // namespace core
