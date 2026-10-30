// core/fx/amp.h - INSERT TYPE 9 AMP (docs/INSERT-FX.md "AMP"): a NAM (Neural Amp Modeler) capture on a Sidecar voice's
// INSERT slot, run by TONE3000's NAM A2-Lite engine (third_party/nam/embedded, MIT: the Cortex-A76 build, NEON on the
// Pi 4 and an Apple Silicon Mac, its portable path elsewhere). The Pi and the Mac (CORE_POLY builds).
//
// Knobs (P1..P4, 0..127):
//   MODL  the capture: the files of /NAM/ (.namb, or .nam A2-Lite JSON), sorted by name (case ignored), spread over
//         the knob (n files: 128 / n values each); shown by a short name (7 characters: the file's name, upper case,
//         spaces and '_' '-' left out) or, while it cannot play, the reason (LOADING, NO NAM, NOTLITE, BADFILE, NOT 48K,
//         MAX 6); a change loads asynchronously and crossfades in (10 ms)
//   GAIN  the drive into the capture, -18..+18 dB (64 = 0 dB)
//   TONE  a tilt after it around 650 Hz, -6..+6 dB (64 = flat)
//   LEV   the output, -36..+6 dB (109 = 0 dB)
// Only A2-Lite (1,871 weights, 48 kHz) is supported (the user's decision, 2026-10-23): A2-Full, NAM's A1 "Standard" /
// "Lite" / "Feather" WaveNets and LSTMs are refused ("NOTLITE"); a capture at another rate is refused ("NOT 48K": no
// resampler). The voice chains only (V1..V8): a bus chain does not take AMP. A stereo voice chain (OUT PRE / POST)
// runs the capture once on (L + R) / 2 and puts it on both sides.
//
// Threads and memory. A fixed pool of kInstances engines (weights + instance + ring state, ~92 KB each), placed with
// the INSERT chains' heap buffers (ins::heap_place) or static on the Mac: nothing is allocated after boot.
//   - the control core (the Pi's core 3; ins::set_* and step()): the /NAM/ listing (a cardq kListReq the card core
//     runs, into the load buffer), the load job (the file's open, reads and close posted to the card core through
//     cardq::async: core 3 never touches the card for either), the parse (a .namb at once; a .nam's JSON kParsePer bytes a step), the engine's prewarm
//     (kPrewarmPer samples a step), then the ready instance offered to the slot;
//   - the render core of the voice (ins::Engine::Run -> run()): takes an offer at the next chunk and crossfades from
//     what played (another capture, or dry) over kXfadeFrames; the old instance goes back to the control core.
// The cap: kMaxRunning AMP slots play at once (6 x 3.98 % of a Pi 4 core [measured, the boot bench 2026-10-23: worst
// block 5.5 %], at most 2 a voice: 8 % of the 20 % a voice may take); a slot past it plays dry and shows MAX 6.
#pragma once
#include <stdint.h>

#ifndef NAM_A2LITE_MAX_BUFFER_SIZE
#define NAM_A2LITE_MAX_BUFFER_SIZE 16   // ins::kChunk: the largest piece Engine::Run passes
#endif
#include "../../third_party/nam/embedded/engines/cortex-a76/a2lite/nam_a2lite.h"

namespace core {
namespace amp {

constexpr int kVoices = 8, kSlots = 2;
constexpr int kMaxRunning = 6;              // AMP slots playing at once (the CPU cap; fix1049: 8 -> 6)
constexpr int kInstances = kMaxRunning + 1; // + one for a capture change's crossfade
constexpr int kWeights = 1871;              // A2-Lite's weight count (the only shape it runs)
constexpr int kMaxModels = 64;              // /NAM/'s files listed at most
constexpr int kNameMax = 48;                // a file's name (longer: left out of the list)
constexpr int kShort = 8;                   // the knob's text: 7 characters + NUL
constexpr int kXfadeFrames = 480;           // 10 ms
constexpr uint32_t kFileMax = 512 * 1024;   // larger: NOTLITE (an A2-Lite .nam ~40-60 KB; TONE3000's Lite + Full ~300 KB)
constexpr uint32_t kReadPiece = 16384;      // bytes a card read
constexpr uint32_t kParsePer = 4096;        // JSON bytes a step
constexpr int kPrewarmPer = 512;            // prewarm samples a step (~0.3 ms on a Pi 4 [inferred])
constexpr const char* kFolder = "/NAM";

enum Status : uint8_t {
  kIdle = 0,    // not an AMP slot
  kPlaying,     // the capture plays
  kLoading,     // listing / reading / parsing / prewarming
  kNoFiles,     // /NAM/ has no .nam / .namb
  kNotLite,     // a NAM model, but not A2-Lite
  kBadFile,     // not a NAM model, damaged, truncated, a weight not finite
  kNot48k,      // a capture at another sample rate
  kCap,         // kMaxRunning AMP slots play already
  kNoMem,       // the pool was not placed
  kStatuses
};
const char* status_short(int s);   // "LOADING" .. (<= 7 characters); "" for kIdle / kPlaying
const char* status_long(int s);    // "not an A2-Lite model: LITE MODELS ONLY" ..

// ---- the loader (pure: any core; tests)
enum Parse : int8_t { kOk = 0, kErrFormat, kErrTruncated, kErrCrc, kErrNotLite, kErrRate, kErrNotFinite };
Status parse_status(Parse p);
// A .namb (TONE3000's binary NAM, format 1): checked (container, CRC, the A2-Lite shape, 48 kHz, finite weights),
// its kWeights weights copied into w. data 4-byte aligned.
Parse parse_namb(const uint8_t* data, uint32_t size, float* w);
// A .nam (JSON): "architecture" "WaveNet", one layer array of 3 channels, no condition DSP, kWeights finite
// "weights", "sample_rate" 48000 (or none); or TONE3000's A2 file, a "SlimmableContainer" whose config.submodels hold
// an A2-Lite and an A2-Full WaveNet: the first A2-Lite submodel is taken. Stepped: begin, then step(budget) until it
// returns != 1.
struct JsonScan {
  void begin(const char* text, uint32_t n, float* w);
  int step(uint32_t budget);   // 1 more to do, 0 done (result()), -1 failed (result())
  Parse result() const { return res_; }
  int weights() const { return wn_; }

 private:
  void value(const char* tok, int len, bool str);
  bool open(char c);
  Parse verdict() const;
  void close_model();
  const char* p_ = nullptr;
  uint32_t n_ = 0, i_ = 0;
  float* w_ = nullptr;
  int wn_ = 0, depth_ = 0, layers_ = 0, channels_ = -1, top_ = 0;
  char stack_[24];
  bool want_key_[24];
  char key_[24][16];
  char tok_[48];
  int tl_ = 0;
  bool in_str_ = false, esc_ = false, in_tok_ = false, wavenet_ = false, other_arch_ = false, cond_ = false,
       rate_bad_ = false, finite_ = true, bad_ = false, failed_ = false, slim_ = false, locked_ = false;
  int root_ = 1, subs_ = 0;
  Parse sub_res_ = kErrFormat;
  Parse res_ = kErrFormat;
};
bool has_ext(const char* name);    // .nam / .namb (any case)
void short_name(const char* file, char* out);   // "Plexi Crunch.namb" -> "PLEXICR"

// ---- the engine pool (placement as the DELAY lines: insert.h heap_bytes / heap_place)
unsigned long heap_bytes();
void heap_place(void* mem);        // zeroed by the caller; before any run()
bool placed();

// ---- the audio side (the voice's render core): slot (c 0..7, s 0..1) over m <= 16 frames. x / r the input (r null:
// mono), y0 / y1 the output (y1 used when r). p: the smoothed params 0..1.
void run(int c, int s, const float* p, const float* x, const float* r, float* y0, float* y1, int m);
void fresh(int c, int s);          // the slot starts AMP from clean (its tone / DC state; not its capture)
void service(int vi);              // voice_out's, every block: offers to voice vi's slots that did not run AMP

// ---- the control side (core 3)
void begin(bool card_ok);          // /NAM/ listed at the next steps
bool step();                       // one bounded piece of work; true: did something
void rescan();                     // list /NAM/ again (amp=rescan; the files changed)
int models();                      // files listed
const char* model_file(int i);     // "Plexi Crunch.namb"
const char* model_short(int i);    // "PLEXICR"
int model_for_knob(int v);         // P1 0..127 -> the file, -1 none
int knob_for_model(int i);         // the middle of the file's range
int find_model(const char* file);  // by name (case ignored), -1 none
Status status(int c, int s);
// P1's text for slot (c, s) with the knob at v: the file's short name, or the reason it does not play
void knob_text(int c, int s, int v, char* b, int cap);
// The card's AMP line ("AMP V1 A Plexi Crunch.namb"): the slot's capture by name, resolved once /NAM/ is listed
void want_file(int c, int s, const char* file);
const char* slot_file(int c, int s);   // the file the slot's knob picks ("" none)
int running();                     // AMP slots with a capture (playing or fading)
// The console: "amp" (the state), "amp=rescan"; false: not an amp command
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);      // "\"amp\":{...}"
struct Stats { uint32_t loads, reuses, refused, offers, taken, xfades, lists; };
const Stats& stats();
void reset();                      // tests: every slot dry, the pool free, the list kept

// ---- the benches / tests: a synthetic A2-Lite capture (deterministic small random weights: not an amp, a test signal
// path), and one instance's cost over 1 s of audio in kBlock pieces (the Pi's boot bench; core::cycles() ticks)
void synthetic_weights(float* w, uint32_t seed);
uint64_t bench(uint32_t* worst_block);

}  // namespace amp
}  // namespace core
