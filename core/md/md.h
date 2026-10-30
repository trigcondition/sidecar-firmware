// core/md/md.h - the built-in MD: voice byte 0x40 (machines.h kMd) plays the combined mdkit drum machine, the same
// voices, knobs and code as the MD user machine (plugins/mdkit: mdk.c compiled into the core through
// core/engines/setup_engines.sh's MDKit/mdk_builtin.c; mdkit vendored in third_party/mdkit). It replaces DIGIDRUM
// (removed 2026-10-29) on the same flag. Pi and Mac only (CORE_MD, with CORE_MSAMP): a PI voice's state (~26 KB) is
// more than the Teensy has, so there 0x40 stays silent.
//
// The Rytm interface (rytm-mods 0021's machine 42; the spec: docs/MD-BUILTIN.md): a report tagged kLayoutTag reads
// P1..P8 the TYPE's controls, P9 ENGINE, P10 TYPE, P11 LEV. Any other report on 0x40 is DIGIDRUM's old page: P1 its
// ENG (11 zones) mapped onto ENGINE / TYPE (legacy_map), P2..P8 kept where the controls are the same, P9..P16 ignored.
// The labels reach the Rytm through the user slots' 19 / 1A / 1B with the slot byte 0x40 + ENGINE (on_ask).
//
// Threading: report / trig / params on the control core, render on the voice's render core, never at once for one
// voice (the core's between-blocks rule, as every machine).
#pragma once
#include <stdint.h>

#ifndef CORE_MD   // 1 with CORE_MSAMP (the Pi, the Mac), else 0 (the Teensy): always defined, also used in expressions
#if CORE_MSAMP
#define CORE_MD 1
#else
#define CORE_MD 0
#endif
#endif

namespace core {
namespace md {

constexpr int kLayoutTag = 0x224D;   // the sound tag 0021 puts on machine 42's reports with MD's page ("DM")
constexpr int kAskSlot = 0x40;       // 19 / 1A / 1B slot bytes 0x40..0x42: MD's labels for ENGINE 0..2
constexpr int kTableVersion = 0;     // moves the label sets' generations when the tables change (0..3)
constexpr int kSets = 30;            // TYPEs over all ENGINEs: FM 0..7, PI 8..16, TRX 17..29

#if CORE_MD
void init();                          // boot (voices_init): every voice's state set up (FM BD)
void enter(int vi);                   // voice vi starts playing MD (from another machine): a fresh state
// a report's P1..P16 (as x.p holds them, np of them) and its sound tag: the machine's knobs; MD's own layout with
// kLayoutTag, DIGIDRUM's otherwise (remembered for params())
void report(int vi, const float* p, int np, int tag);
void params(int vi, const float* p);  // x.p changed on the control side (LFOs, step locks): in the voice's last layout
void trig(int vi, int vel);           // a hit, velocity 1..127
void render(int vi, float* out, int n);   // n <= 12 samples, -1..1 nominal
bool on_ask(const uint8_t* d, int len);   // a 19 for slots 0x40..0x42: answered (1A / 1B), true; else false
// P1..P16 of the layout given -> the MD knobs (q[0..10]: P1..P8, ENGINE, TYPE, LEV); the set (0..29) they select
void knobs_of(const float* p, int np, bool md_layout, float q[16]);
int set_of(const float q[16]);
int voice_set(int vi);                // the set voice vi plays (tests, the log)
const char* set_name(int set);        // "FM BD" ... "TRX B2"
uint32_t state_bytes();
// The control core's cost (review M4: /status "md", and the boot bench's line): ENGINE / TYPE changes (a params()
// that changes the set, then the trig that renders the old voice's 2 ms tail and sets the new one up) and the most
// one took; the most any other call took.
struct Cost { uint32_t changes, change_max_us, call_max_us; };
Cost cost();
void cost_reset();
int json(char* out, int cap);   // "\"md\":{\"ok\":..,\"stateBytes\":..,\"changes\":..,\"changeMaxUs\":..,\"callMaxUs\":..}"
#endif

}  // namespace md
}  // namespace core
