// core/loop/loop.h - the master looper (spec-050 section 3): the Rytm's MAIN L / R is kept in a ring, always (loop_ring.h,
// written by core 0 in the Overbridge IN completion); one press (grab) takes the last 1/2 .. 16 bars ending on the last
// bar line and plays them as a loop on the Rytm's MAIN after its FX (the POST return), from the next bar line. The ring
// never stops: a loop plays with the live Rytm under it, the ring hears both, so a grab while a loop plays is a resample.
// No overdub. Up to 8 grabs stay in a per-board store, the oldest dropped; KEEP writes one to the card as a take.
//
// Threads: the state machine, the grabs and the store are core 3's (tick, from the control loop). Core 0 has the two
// audio hooks, in_block (the IN completion: the ring) and out_mix (FillOut: the loop's frames onto MAIN, after the master
// FX); core 3 hands core 0 what to play through one published command (a generation counter; core 0 takes it at the next
// block). Times are the host's render frames (seq::frames()); a bar's time is seq::beat_map's, `lat` early.
//
// Two boxes (one looper per box, decided 2026-11-27): each slot (0, 1) has a looper of its own: its own ring of its box's MAIN,
// its own grabs and store, its bars from its own clock, its loop.txt in its own box's folder and its loop on its own box's
// returns (set_returns). Nothing is shared and slot 0 and slot 1 are the same. The core 0 hooks, init and KEEP take the slot;
// the controls, the settings (load / save in the slot's Scope), the console and json act on the slot in hand (set_slot_fn: the
// pages', the console's); tick runs every slot in its own Scope. Auto-mute and LIVE VIA move a Rytm's tracks and macros: a
// Rytm's looper only (another box's: a named gap).
#pragma once
#include <stdint.h>

#include "loop/loop_ring.h"

namespace core {
namespace loop {

enum Len { kHalf = 1, k1 = 2, k2 = 4, k4 = 8, k8 = 16, k16 = 32 };   // a grab's length in half bars
enum Err { kOk = 0, kErrStopped = 1, kErrTooLong, kErrNoRoom, kErrOff, kErrBadArg };
enum State { kRing = 0, kWait, kPlaying, kFading };   // no loop / its start is ahead / playing / stop scheduled or fading
constexpr int kMaxGrabs = 8;
constexpr uint32_t kPieceBytes = 64 * 1024;    // one tick's copy at most (with its peak scan and the limiter: ~0.1 ms on a Pi 4)
constexpr double kPieceFrames = 48;            // the frames one control pass takes (1 ms): what the press rule counts a piece as
constexpr int kSeam = 64;                      // frames: the loop seam, a swap, a start
constexpr int kCutFrames = 480;                // 10 ms
constexpr int kPre = kSeam;                    // frames kept before a grab's first (the seam's other side)
constexpr int kSlots = 2;                      // loopers: one a box
constexpr uint8_t kNoReturn = 0xFF;            // set_returns: the box has no such return

struct Grab {
  uint32_t id;       // counts up, never reused
  uint32_t frames;   // the loop's length (pre not counted)
  uint8_t half_bars;
  float bpm;
  uint64_t start;    // the host render frame of its first sample's source (arrival frame of the grab's start)
  uint8_t kept, gap; // KEEP wrote it; the ring had a gap in it
  uint8_t copied;    // the copy into the store is done (until then it plays from the ring)
  uint32_t peak;     // the largest |sample| (L or R, 2^31 = full scale): of what is copied so far (not limited yet), once `copied` of the
                     // loop as played through the grab limiter's envelope (loop.cpp: at most 6 dB off the peaks; the store's audio itself is untouched)
  int32_t* pcm;      // the store's copy (frame 0; kPre frames before it), null while not copied; as grabbed, never changed
  const uint16_t* env;   // the limiter's gain envelope for it (uint16 every 16 frames, 32768 = 1), null while not copied
};

// the time and the Rytm as the looper sees them (the sequencer's, by default; tests put theirs in)
struct Env {
  bool (*beat_map)(double* origin, double* per16, int* lat) = nullptr;
  uint64_t (*frames)() = nullptr;
  uint16_t (*muted)() = nullptr;
  bool (*clock)(float* ticks, float* tick_us, bool* run) = nullptr;   // the MIDI clock's, the fallback for the bars
};
void set_env(const Env* e);                 // null: the defaults (the slot in hand's)
// which box: the slot the controls act on (null: 0); a slot's returns, POST and PRE (L / R channels of out_mix's F, kNoReturn
// none: OUT = PRE is refused on a box without one, and a PRE kept from before plays on POST)
void set_slot_fn(int (*fn)());
int slot_in_hand();
void set_returns(int slot, const uint8_t post[2], const uint8_t pre[2]);
bool has_pre(int slot);
void init(int slot, int32_t* ring, uint32_t ring_frames, uint8_t* store, uint64_t store_bytes);
void init(int32_t* ring, uint32_t ring_frames, uint8_t* store, uint64_t store_bytes);   // null ring: the looper is off
void reset();                               // tests: everything stopped, the settings default
bool on();
bool on(int slot);

// ---- the controls (core 3)
int grab(int half_bars);                    // Err; a grab while a loop plays is a resample (swapped in at the next bar line)
bool play(int g);                           // grab g (0 newest) from the next bar line; false: none, or no bar lines
void stop(bool cut, bool now);              // cut 10 ms / the FADE time; at the next bar line unless now
bool keep(int g, char* msg, int cap);       // ask core 0 to write grab g as a take (the Pi: loop_pi); false: none / off
void tick(uint32_t us);                     // the control loop's every pass
const char* err_text(int err, int half_bars, char* b, int cap);   // "PLAY FIRST", "16 BARS NEEDS 47 BPM", "NO ROOM"

int grabs();
const Grab* grab_at(int i);                 // 0 newest
int grab_index(uint32_t id);                // the index of the grab with that id (it moves as newer ones land), -1 none
State state();
int playing();                              // the grab playing (0 newest), -1 none / the loop is gone from the store
bool playing_info(uint64_t* start, uint32_t* len);   // the loop's sample 0 (host render frame) and length now; false: none
bool pending();                             // a grab is waiting for its bar's audio
int ring_seconds();                         // how much the ring holds now (s, up to its size)
bool bar_position(float* bar, float* of_bars);   // the loop's position in bars and its length in bars, false: none

// ---- settings
enum { kFadeQuarter = 0, kFadeHalf, kFade1, kFade2, kFade4, kFades };   // FADE: 1/4 .. 4 bars
void set_bal(int v);                        // 0..127 (64: both full)
void set_lev(int v);                        // 0..127 (100 unity)
void set_fade(int f);
void set_auto_mute(bool on);
void set_live_via(int perf);                // 0 off, 1..12
enum { kOutPost = 0, kOutPre = 1 };         // OUT: the loop onto the Rytm's POST return (F 0/1) or its PRE return (F 2/3)
void set_out(int o);
int out();
int bal();
int lev();
int fade();
bool auto_mute();
int live_via();
// the modulation layer's writers (LOOP:BAL, LOOP:LEV): a value replaces the knob until released (-1)
void set_bal_mod(int v);
void set_lev_mod(int v);
int bal_eff();
int lev_eff();
float own_gain(int bal);                    // the Sidecar's own output at BAL (spec 3.8)
float loop_gain(int bal);                   // the loop's
int live_perf(int bal);                     // LIVE VIA's macro value: 127 down to 0 over BAL 64..127

// ---- the audio hooks (core 0)
void in_block(int slot, const int32_t* lr, int n, uint64_t arrival, unsigned lost);   // MAIN L / R of slot's box's IN block (interleaved)
void out_mix(int slot, int32_t (*F)[12], int n, uint64_t render_frame);              // slot's loop onto its box's F (its returns), the rest by BAL
void in_block(const int32_t* lr, int n, uint64_t arrival, unsigned lost);   // (tests: the slot in hand's)
void out_mix(int32_t (*F)[12], int n, uint64_t render_frame);
// The loop's make-up gain onto F[.][0..1] (default 1). The Rytm's MAIN arrives over USB at its own level, but what the Pi
// puts on POST (0/1) reaches the Rytm's main 10.9 dB down (masterfx_pi.h, measured 2026-11-01): the Pi sets 3.508 so a
// loop plays back at the level it was heard. Any thread; out_mix reads it once a block. POST tops out about 10.9 dB under
// the Rytm's full scale, so each loop's make-up is capped from its own peak (grab.peak): the loop alone stays under
// kLoopCeil of full scale, a quiet loop gets the whole make-up, a hot one less. Several loops summed still meet the soft
// knee (out_mix) instead of clipping.
void set_out_gain(float g);
void set_out_gain(int slot, float g);
float out_gain();
// OUT = PRE: the same for the PRE return (F 2/3), which reaches the main 2.0 dB down (the Pi sets kStemOnPost / kPreOnPost)
void set_pre_gain(float g);
void set_pre_gain(int slot, float g);
float pre_gain();
// the measured POST make-up (loop=cal: click out / peak back; loop.txt `postgain`): in 1.5 .. 6.0 it replaces out_gain on POST,
// anything else is none (0)
void set_postgain(float g);
float postgain();
float makeup();                             // what the loop is multiplied by now (POST: postgain, else out_gain; PRE: pre_gain)
constexpr float kLoopCeil = 0.98f;          // the loop alone is capped to this part of full scale (also the soft knee's start)
bool wants_in();                            // the ring exists
bool wants_out();                           // a loop is on its way or plays (out_mix does something)
bool wants_in(int slot);
bool wants_out(int slot);

// ---- POST latency and level calibration (loop=cal): clicks on POST 0 / 1 (the OUT hook), found again in MAIN IN (the IN hook):
// when they come back, the round trip (post_lat) and each click's peak back (the return gain, postgain). POST only
void cal_start();
bool cal_wanted();                          // clicks are on their way
bool cal_info(uint64_t* first_click, int* n, uint32_t* gap);   // the render frames of the clicks (tests, the OUT hook)
int post_lat();                             // frames the POST path takes beyond the voice path's latency (0 default)
void set_post_lat(int f);

// ---- settings on the card (/.rytm/loop.txt in the slot's Scope: its box's folder, a box setting) and the console / status
void load();                                // the slot in hand's
void take_text(const char* b, int n);       // a read loop.txt into the slot in hand's settings (another slot's at its attach)
bool save();                                // the slot in hand's; true: a posted write was queued
bool dirty();
bool command(const char* p, char* out, int cap);   // "loop", "loop=grab:L", ... (spec 3.12): false when not "loop..."
int json(char* out, int cap);               // "\"loop\":{...}"
int json(int slot, char* out, int cap);
const char* last_message();                 // the last refusal / result text the pages show ("PLAY FIRST"), "" none
uint32_t message_gen();                     // counts up with each one

// ---- KEEP: core 3 asks, core 0 (loop_pi, its idle loop) takes the job, writes the grab's store copy as a take in pieces and
// says when it is done. The grab stays in the store until then.
struct KeepJob {
  uint32_t id, frames;
  const int32_t* pcm;   // the store's copy, L R interleaved
  float bpm;
  int half_bars;
  uint64_t start;       // the grab's first frame's host render frame
};
bool keep_take(int slot, KeepJob* j);       // core 0: slot's next job (false: none)
void keep_finished(int slot, uint32_t id, bool ok, const char* take_name);
bool keep_take(KeepJob* j);                 // (the slot in hand's)
void keep_finished(uint32_t id, bool ok, const char* take_name);
struct Stats {
  uint32_t grabs, refused, resamples, copies, abandoned, dropped, cal_ok, cal_fail, kept;
  uint32_t worst_copy_bytes;
};
const Stats& stats();
const Stats& stats(int slot);

}  // namespace loop
}  // namespace core
