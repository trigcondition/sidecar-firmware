// core/mod/rytm_out.h - the Rytm's own scene, PERF macros and mutes set from the Sidecar (spec-050 2.7, 5.2): one sender
// for the layer's kRytm writer (RYTM:SCN, RYTM:PERF1..12 locked in a scene) and the looper's auto-mute (Phase 4). Never
// the kit: a scene switch, a macro's value, a mute, as the Rytm's own pads set them. Two paths:
//   the mod command (the 0.5.0 flash; the hello's caps say so): F0 7D 52 54 <kHostCmd> 01 <op> ... F7, ops
//     01 MUTE <set lo7> <set hi5> <clr lo7> <clr hi5>, 02 SCENE <n 0..12>, 03 PERF <macro 1..12> <value 0..127>;
//     the mod answers 2D 01 10 <op> <status> (0 done, 1 refused: the CCs from then on, until the next hello). The mod
//     runs each in the Rytm's MIDI task by the stock setters a CC 94 / 92 / 35.. reaches (mute_set, scene_set, the
//     macro's amount: rytm-mods 0026 design.md "0.5.0"), whatever the MIDI settings; a MUTE sets, it never toggles
//   else CCs, as the Rytm receives them (MIDI CONFIG > PORT CONFIG: receive CC on): the scene CC 92 = n on the auto
//     channel, a macro kPerfCc[m - 1] on the PERF channel, a mute CC 94 = 1 / 0 on the track's channel
//     (core/song/song.h MidiConfig, from the Rytm's GLOBAL dump, else its defaults: auto 14, PERF 15, tracks 1..12).
// On change only; the scene and each macro at most every kSpacingMs, the latest value kept and sent when its time comes
// (tick). Control side (core 3 on the Pi), cold.
#pragma once
#include <stdint.h>

namespace core {
namespace mod {

constexpr uint8_t kHostCmd = 0x5A;    // sim/protocol.py HOST_CMD (free in the registry and rytm-mods' router: Task 47)
constexpr uint8_t kHostAck = 0x10;    // 2D 01 10 <op> <status>: the mod's answer (core/seq/seq.h kStateCmd kind)
constexpr int kCapsHostCmd = 16;      // the hello's caps bit "the mod command" (rytm-mods 0026 0.5.0: caps 31)
enum : uint8_t { kOpMute = 1, kOpScene = 2, kOpPerf = 3 };
constexpr uint8_t kSceneCc = 92, kMuteCc = 94;
constexpr uint8_t kPerfCc[12] = {35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 46, 47};   // hardware 2026-11-05: no CC 38
constexpr uint32_t kSpacingMs = 5;

void rytm_scene(int n);                          // the Rytm's scene 0..12 (0 off)
void rytm_perf(int macro, int v);                // macro 1..12 to 0..127
void rytm_mute(uint16_t set, uint16_t clr);      // the tracks in set muted, those in clr unmuted (bit k: track k + 1)
void rytm_host_cmd(bool on);                     // the hello's caps: the mod command there (a refusal is forgotten);
                                                 // a hello is a new link: every value sent is forgotten
void rytm_ack(int op, int status);               // 2D 01 10 <op> <status>
void rytm_out_forget(int slot);                  // 0 the scene, 1..12 a macro: the value sent is no longer known
void rytm_out_tick(uint32_t ms);                 // mod::tick: the values whose kSpacingMs has passed
void rytm_out_reset();                           // tests (mod::reset): nothing sent, the CCs
struct RytmOutStats { uint32_t cmds, ccs, refused, held; };   // held: a value replaced before its time came
const RytmOutStats& rytm_out_stats();

}  // namespace mod
}  // namespace core
