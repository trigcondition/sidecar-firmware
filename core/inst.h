// core/inst.h - E0 (docs/SEQ-IDEAS.md, docs/POLYPHONY.md "E0"): every Sidecar machine on a sequencer poly track. The
// sequencer's notes for voice vi come here (core/seq/seq.cpp) with their track's poly config (seq.h PolyCfg) and go:
//   - a machine with its own note pool (6-OP FM, MULTISAMPLE, WAVETABLE, GRANULAR): to that pool (core/poly.h seq_on),
//     VOICES capping the notes held at once (the oldest held one released for a new one; AUTO: no cap, as before E0);
//   - one instance resolved (VOICES 1, DSP SAMPLE, a mono user machine): the voice's own machine, as a Rytm trig plays
//     it (voices.h voice_play): mono, a new note retriggers / chokes the old;
//   - more: the instance pool below - for PLAITS, DAISYSP, JUNO and user machines that opt in (their
//     package's RMP_HF_INSTANCES): kTotal instances shared by the voices, each with its own engine state, allocated by
//     the track's ALLOCATION mode and stacked by its UNISON (core/inst_alloc.h), rendered into the voice's channel on
//     its render core (its INSERT chain and the guard's per-voice budget after it, as the voice's own sound).
// The note's pitch per machine: inst_alloc.h Decl.pitch. CPU and RAM per instance: stat() / json() (STATUS, /status),
// the declared cost checked against the voice budget when the config resolves (inst_alloc.h resolve).
// Control core (the Pi's core 3) between render blocks; render_add a render core; no card work anywhere here.
#pragma once
#include <stdint.h>

#include "inst_alloc.h"
#include "machines.h"

namespace core {
namespace inst {

constexpr int kTotal = 32;   // instances shared by the voices (4 seq tracks x 8)

#if CORE_POLY
void init();    // boot (voices_init): the instances' engines (heap)
void reset();   // tests: every instance free, the counters cleared
// The sequencer's note on / off on voice vi (note: as the voice plays it) with its track's config; seq_off note -1: all
// the voice's sequencer notes. seq_on false: nothing plays it (no voice machine that takes notes).
bool seq_on(int vi, int note, int vel, const seq::PolyCfg& cfg);
void seq_off(int vi, int note);
bool active(int vi);       // vi has instances sounding (render_add has work)
bool seq_mono(int vi);     // vi plays a sequencer note on its own machine now (the mono path): the Rytm's 0x11 leaves it
void silence(int vi);      // CC 120, the link reset, the voice's machine changed: its instances gone at once
void knobs(int vi);        // the voice's P1..P16 changed (a report, the LFOs): its instances take them (JUNO, user)
// The render core's: vi's instances (kBlock frames) added into l (and r: a stereo voice, OUT PRE / POST, each instance
// at its UNISON pan), in the voice's own units (+-1 nominal, before voices.cpp's 0.7 and soft limiter), at the pool's
// headroom gain (1 / sqrt of the sounding weight, as core/poly.h's)
void render_add(int vi, float* l, float* r, bool stereo);
int voice_notes(int vi);   // vi's instances sounding
bool juno_patch_is(int vi, const char* name);   // tests: vi's sounding JUNO instances (one at least) play that patch
// What cfg resolves to on voice vi's machine now (the console, the pages): false when vi is no voice
bool resolve_for(int vi, const seq::PolyCfg& cfg, Resolved* r, int* machine);
struct VoiceStat {
  int machine;             // the voice's machine (voice byte & 0x78), -1 none
  int path;                // 0 none yet, 1 its own pool, 2 mono (its own machine), 3 instances
  int instances, lanes, unison, sounding;   // sounding: instances (inst), the note held (mono), the notes held (pool)
  bool clamped;            // the budget or the machine's max cut the config
  int cpu100;              // the instances' declared cost (% of a core x 100)
  int meas100;             // measured: the instances' render time, % of a block x 100 (0 before any)
  uint32_t ram;            // bytes the instances hold
  uint32_t notes, steals;
};
VoiceStat stat(int vi);
uint32_t inst_bytes();     // one instance's RAM
int json(char* out, int cap);                      // "\"inst\":[{...} x 8]" for /status
bool command(const char* p, char* out, int cap);   // the console's "inst": per voice what it plays and costs
#endif

}  // namespace inst
}  // namespace core
