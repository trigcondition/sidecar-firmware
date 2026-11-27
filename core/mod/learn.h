// core/mod/learn.h - MIDI learn (spec-050 2.4, 2.10): a CC from the Rytm's USB MIDI (port 0) or a USB-A MIDI device on
// the Pi (ports 1..4, by their order: the Devine EZ-Creator Fade's crossfader) moves the scenes' fader, picks scene A
// or B, or adds a bipolar offset to any modulation target. Arm a route (learn=fader|a|b|NAME[:DEPTH]): the next CC heard
// is its port, channel and number. 7-bit CCs, or 14-bit once CC n (0..31) has been followed by CC n + 32 on the same
// port and channel (the LSB; an MSB alone then plays with the last LSB). A fader route takes the fader from another
// source (or a held edit) only on a change of 2 CC steps / 256 (14 bit) from its last value, then follows every step
// (0.5.0 v2 Task 6). A target route is a source of the layer (8..39): each tick mod::offset(8 + i, t, depth,
// (v - 63.5) / 63.5) once a value came. A box setting, not a kit's: /.rytm/learn.txt, read at boot, written by
// idle_save. Control side (core 3 on the Pi), cold.
#pragma once
#include <stdint.h>

#include "mod/target.h"

namespace core {
namespace learn {

constexpr int kRoutes = 32;          // the layer's sources 26..57
constexpr int kFirstSource = 26;
enum What : uint8_t { kFader = 0, kA, kB, kTarget };
struct Route {
  uint8_t port, ch, cc, what;   // port 0 the Rytm, 1..4 USB-A; ch 0..15; cc 0..127; What
  mod::Tid t;                   // kTarget: the target
  int8_t depth;                 // kTarget: -64..63, the offset at the CC's ends
};

// a channel message from port (0 the Rytm, 1..4 a USB-A device): true when a route (or the armed learn) took it
bool midi(int port, uint8_t st, uint8_t d1, uint8_t d2);
bool arm(const char* what, bool only = false);   // "fader", "a", "b", a target name with ":DEPTH" (default 32);
                                     // false: no such. only: the learned route replaces every route of its What (the MIDI
                                     // page's LEARN rows: one fader, one A, one B)
void disarm();
int armed();                         // -1 none, else the What it waits for
int last();                          // the index of the route the last learn made or replaced, -1 none
void tick();                         // mod::tick: the target routes' offsets
int routes(const Route** out);       // the routes in use (*out: the table, n of them first)
bool clear(int i);                   // route i (0-based) dropped
// the console: "learn" (the routes, one per line), "learn=fader|a|b|NAME[:DEPTH]", "learn=clear:N" (1-based),
// "learn=off". False: not "learn..."
bool command(const char* p, char* out, int cap);
// the file: "#sidecar learn 1", "QP off|always" (scene's QPERF FADER when not the default MUTED ONLY, v2 Task 6), then
// "R <port> <ch 1..16> <cc> <what> [depth]", what fader / a / b / a target name. A line this build does not read locks
// the file (read what can be read, never written: the 1042 rule). A QPERF FADER change makes it dirty.
void load();                         // boot (the card: a box setting, as keys.txt)
bool save();                         // idle_save: true if it wrote
bool dirty();
bool locked();
void reset();                        // tests: no routes, nothing armed, the values forgotten

}  // namespace learn
}  // namespace core
