// core/samp/preview.h - the sample browser's dry preview voice (0.5.0; FUNC + YES on a file in the bank list's browser,
// core/ui/page_wavegrain.cpp, and its file menu's PREVIEW): one file of the card played once, up to kMaxSeconds, on the
// POST pair (host -> Rytm 0/1, core/fx/out.h) after everything else - no track voice, no INSERT, no send FX, no pool
// row. Any file the WAV reader takes (core/wavetable/wav_read.h: WAV / AIFF, PCM 8..32-bit or float, any rate, mono
// on both sides, the first two channels of more).
//
// Two sides, no locks, no allocation:
//   - the control core (core 3): start() / stop() / info(); step() (from the idle chain, core/engine.cpp) posts one card
//     request a call through cardq (the file opened, its head read - a WAV's chunks walked on past it when its data
//     starts later -, then kPiece pieces converted to int16 L R into a buffer of kBufFrames frames at the file's own
//     rate, taken once at boot from the board's high heap: begin()). It says false while a request waits for the card
//     (the rest of the idle chain runs meanwhile), and loads nothing over the buffer before the audio side has left the
//     previous preview (or none runs);
//   - the audio side (the Pi's output fill on core 0, ob_usb.cpp): mix() adds what is loaded to the POST pair at
//     kLevel, resampled to 48 kHz (linear), 5 ms in, 5 ms out on a stop. It never waits: past what is loaded it holds
//     the last frame, fades it out (counted as an underrun) and fades in again when more is there.
// The level (0.5.0, the owner's hardware test: -18 dB was too quiet): PREVIEW LEVEL, knob H in the browser, kLevelMinDb ..
// kLevelMaxDb in 1 dB steps, kLevelDefDb at first; a Sidecar setting (/.rytm/preview.txt "level=-9", not per kit). The
// output stage limits the POST pair to -12 dBFS (core/fx/out.h kLimitPost) and a send FX return on it to -12 dBFS too,
// before this is added: at kLevelDefDb (0.35, 3 dB over a POST voice at its limit) a full-scale file stays under full
// scale with POST's voices and a send FX return at their limits (0.25 + 0.25 + 0.35) - not counting the computer's USB
// OUT, the master FX return or take playback, which also land on 0/1; at kLevelMaxDb (0.5) the sum can reach full scale
// with both at their ceilings. Either way the add saturates (add_sat), never wraps.
// The audio side ramps to a new level over a block.
// sounding() needs no audio side: a stopped preview is over kQuietMs later, an unstopped one past its own length.
// The rules (the browser's): one preview at a time; a new start() fades the old one out first; stop() on any cursor
// move, a load, the browser closing, FUNC + YES's YES released (0.5.0: played while held, as the stock browser).
// Pi and Mac only (CORE_MSAMP).
#pragma once
#include <stdint.h>

#include "../platform.h"

#if CORE_MSAMP

namespace core {
namespace preview {

constexpr uint32_t kOutRate = 48000;
constexpr uint32_t kMaxSeconds = 10;
constexpr uint32_t kBufFrames = kMaxSeconds * kOutRate;   // 1.92 MB of int16 L R (a 96 kHz file: 5 s)
constexpr uint32_t kFade = 240;                           // 5 ms
constexpr uint32_t kStartFrames = 8192;                   // loaded before the sound starts (or the whole file)
constexpr int kLevelMinDb = -40, kLevelMaxDb = -6, kLevelDefDb = -9;   // PREVIEW LEVEL (above: after the POST limit)

enum State : uint8_t { kIdle, kOpening, kHeader, kLoading, kDone, kFailed };   // the control side's job

void begin();            // boot (core/engine.cpp): the buffer from the board's high heap (no buffer: every start refused)
// start: path's preview (any core path, "/Samples/x.wav"); the one sounding fades out first. info: the header only
// (no sound). -1 refused (a virtual "@" item, a path too long), else 0.
int start(const char* path);
int info(const char* path);
void stop();             // fades out (5 ms); the file closed
bool sounding();         // started and not yet faded out / ended (the control core's view)
bool step();             // one bounded piece of the job; true: it did work

struct Status {
  State st;
  bool play;             // a preview (false: an INFO read)
  uint32_t rate, frames; // the file's rate and frames (0 until its head is read)
  uint16_t ch, bits;
  uint32_t loaded;       // frames in the buffer
  uint32_t size;         // the file's bytes
  char path[160];
};
const Status& status();
// The audio side's: adds the preview to out[f][post L / R] (int32 full scale: the output stage's), n frames
void mix(int32_t (*out)[12], unsigned n, int left, int right);
struct Stats { uint32_t starts, ends, underruns, played; };   // played: output frames
const Stats& stats();
// PREVIEW LEVEL: set_level_db clamps to kLevelMinDb .. kLevelMaxDb (a change: saved at the next save()); level() the
// gain the audio side ramps to. load(): boot, from /.rytm/preview.txt (absent: kLevelDefDb); save(): the idle chain's
// posted settings write (fmt.h), true when it wrote.
void set_level_db(int db);
int level_db();
float level();
void load();
bool save();
void reset_for_tests();

}  // namespace preview
}  // namespace core

#endif
