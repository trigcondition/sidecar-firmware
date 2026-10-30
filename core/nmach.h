// core/nmach.h - the note machines: MULTISAMPLE (msamp/msamp.h), WAVETABLE (wavetable/wavetable.h), GRANULAR
// (granular/granular.h), CHOP (chop/chop.h: its notes are slices; no chord params). Each plays its own notes (a pool per voice, the chord params on P13..P16) behind the same
// hooks; voices.cpp and poly.cpp reach them through these. Pi and Mac only (CORE_MSAMP); inline, cold.
#pragma once
#include "voices.h"

#if CORE_MSAMP
#include "chop/chop.h"
#include "granular/granular.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "seq/rytm_dump.h"
#include "msamp/msamp.h"
#include "pool/pool.h"
#include "wavetable/wavetable.h"

namespace core {
namespace nm {

// The machine a console override forces on voice vi (msamp= / wt= / gr=voice:V), 0 none.
inline int forced(int vi) {
  return msamp::forced(vi) ? kMultiMachine : wt::forced(vi) ? kWaveMachine : gr::forced(vi) ? kGranMachine
       : chop::forced(vi) ? kChopMachine : 0;
}
// The note machine voice vi plays: its state, else a forced one before the voice's first report; 0 none.
inline int of(int vi) {
  const V& x = v[vi & 7];
  if (x.ms) return kMultiMachine;
  if (x.wt) return kWaveMachine;
  if (x.gr) return kGranMachine;
  if (x.cp) return kChopMachine;
  return forced(vi & 7);
}
inline bool is(int vi) { return of(vi) != 0; }

inline void report(int vi, int note, int vel, const float* p) {
  switch (of(vi)) {
    case kMultiMachine: msamp::report(vi, note, vel, p); break;
    case kWaveMachine: wt::report(vi, note, vel, p); break;
    case kGranMachine: gr::report(vi, note, vel, p); break;
    case kChopMachine: chop::report(vi, note, vel, p); break;
    default: break;
  }
}
inline void note_off(int vi) {
  switch (of(vi)) {
    case kMultiMachine: msamp::note_off(vi); break;
    case kWaveMachine: wt::note_off(vi); break;
    case kGranMachine: gr::note_off(vi); break;
    case kChopMachine: chop::note_off(vi); break;
    default: break;
  }
}
inline void live_on(int vi, int key, int vel, int ch, int mnote) {
  switch (of(vi)) {
    case kMultiMachine: msamp::live_on(vi, key, vel, ch, mnote); break;
    case kWaveMachine: wt::live_on(vi, key, vel, ch, mnote); break;
    case kGranMachine: gr::live_on(vi, key, vel, ch, mnote); break;
    case kChopMachine: chop::live_on(vi, key, vel, ch, mnote); break;
    default: break;
  }
}
inline void live_off(int vi, int ch, int mnote) {
  switch (of(vi)) {
    case kMultiMachine: msamp::live_off(vi, ch, mnote); break;
    case kWaveMachine: wt::live_off(vi, ch, mnote); break;
    case kGranMachine: gr::live_off(vi, ch, mnote); break;
    case kChopMachine: chop::live_off(vi, ch, mnote); break;
    default: break;
  }
}
inline int voice_notes(int vi) {
  switch (of(vi)) {
    case kMultiMachine: return msamp::voice_notes(vi);
    case kWaveMachine: return wt::voice_notes(vi);
    case kGranMachine: return gr::voice_notes(vi);
    case kChopMachine: return chop::voice_notes(vi);
    default: return 0;
  }
}
// CC 120 / the link reset: every note machine's notes of voice vi gone (whichever it plays)
inline void silence_all(int vi) {
  if (v[vi & 7].ms) msamp::silence(vi & 7);
  if (v[vi & 7].wt) wt::silence(vi & 7);
  if (v[vi & 7].gr) gr::silence(vi & 7);
  if (v[vi & 7].cp) chop::silence(vi & 7);
}
// The render core's, a voice on OUT PRE / POST: the notes' pair added into l / r (MULTISAMPLE, GRANULAR: true;
// WAVETABLE has no stereo of its own: false, the caller renders it mono)
inline bool render_add2(int vi, float* l, float* r, int n) {
  const V& x = v[vi];
  if (x.ms) msamp::render_add2(vi, l, r, n);
  else if (x.gr) gr::render_add2(vi, l, r, n);
  else if (x.cp) chop::render_add2(vi, l, r, n);
  else return false;
  return true;
}
// The render core's: the voice's notes added into out (it plays a note machine: true)
inline bool render_add(int vi, float* out, int n) {
  const V& x = v[vi];
  if (x.ms) msamp::render_add(vi, out, n);
  else if (x.wt) wt::render_add(vi, out, n);
  else if (x.gr) gr::render_add(vi, out, n);
  else if (x.cp) chop::render_add(vi, out, n);
  else return false;
  return true;
}
inline bool command(const char* p, char* out, int cap) {
  return wt::command(p, out, cap) || gr::command(p, out, cap) || chop::command(p, out, cap) || pools::command(p, out, cap) ||
         srclib::command(p, out, cap);
}
// engine_begin / engine_idle: the new machines' boot and card steps (msamp's are engine.cpp's own)
inline void begin(bool card) {
  srclib::begin(card);   // the browsers (core/lib/srclib.h)
  pools::set_playing_fn(rytm_dump::is_playing);   // P3: a row in use is not replaced while the Rytm plays
  pools::begin(card);    // the project's POOL lists (core/lib/pools.h), before the machines compose their lists
  wt::begin(card);
  gr::begin(card);
  chop::begin(card);
}
inline bool step() {   // they take turns (a long load or a folder's listing does not hold the others)
  static int t = 0;
  t = t == 3 ? 0 : t + 1;
  if (pools::step()) return true;   // (its card work is posted to the card core: a step never waits on the card)
  pool_sync();                      // DSP SAMPLE's slots follow the SAMPLES pool (rui1045: all eight banks)
  return t == 0 ? (wt::step() || gr::step() || chop::step() || srclib::step())
       : t == 1 ? (gr::step() || chop::step() || srclib::step() || wt::step())
       : t == 2 ? (chop::step() || srclib::step() || wt::step() || gr::step())
                : (srclib::step() || wt::step() || gr::step() || chop::step());
}

}  // namespace nm
}  // namespace core
#endif
