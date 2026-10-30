// core/fx/out_measure.h - `stereo=measure`: the OUT route's measurements on the real Rytm (docs/STEREO.md
// "Measurements"; the stereo / POST design's phase 0), run by the engine itself once the Rytm is connected: test signals
// out on the POST pair (host -> Rytm 0/1), the PRE pair (2/3) and, optionally, one voice's TRK channel, read back from
// the Rytm's MAIN L/R (Overbridge IN 0/1, the analog main mix digitised again):
//   1. silence (the noise floor of MAIN IN)
//   2. a 1 kHz tone on POST, then on PRE, then (with trk) on voice V's track channel: each one's level in MAIN IN
//      (Goertzel at 1 kHz over the tone's last half second) -> the path's gain in dB, and whether it reaches MAIN IN at
//      all (20 dB over the floor): the design's open questions 2 (do the USB IN settings scale 0/1 and 2/3?) and 3 (does
//      MAIN IN carry the POST return?)
//   3. an impulse on POST, then (with trk) on the track: the frames from each impulse out to its arrival in MAIN IN; the
//      difference is the PRE / POST vs TRK alignment (the design's 1.3: <= one Rytm block, 32 frames, inferred)
// The result: the gains, the delays, and the trims that make LEV 100 on PRE / POST as loud in MAIN as the voice on TRK
// (stereo=trim:pre|post:DB10). The TRK phase needs the track's VCA open (the Rytm plays a voice input only while its
// AMP envelope is up: a trig with AMP HOLD at its maximum, FLTR open, OVER 0, VOL 100), so it is optional.
//
// Core 0 on the Pi (the OUT fill and the IN completion: never at once); start / json / command on the control core
// (a flag core 0 takes). Portable: the Mac test runs it against a model of the Rytm.
#pragma once
#include <stdint.h>

namespace core {
namespace out {
namespace meas {

constexpr float kTone = 1000.0f;        // Hz
constexpr float kLevel = 0.1f;          // the test signal's amplitude (-20 dBFS: below every limit's knee)
constexpr uint32_t kNoise = 24000, kToneLen = 48000, kImpLen = 24000;   // frames a phase

enum Phase : uint8_t { kIdle = 0, kFloor, kTonePost, kTonePre, kToneTrk, kImpPost, kImpTrk, kDone };

struct Result {
  bool done;
  float floor_db;                       // MAIN IN's 1 kHz floor, dBFS
  float gain_db[3];                     // POST, PRE, TRK: MAIN IN's 1 kHz level minus the tone's (dB)
  bool heard[3];                        // 20 dB over the floor
  int32_t delay[2];                     // POST, TRK: frames from the impulse out to MAIN IN (-1 not found)
  bool trk;                             // the TRK phases ran
};

// control core: start (trk: the voice's track channel too, voice 0..7); stop
void start(bool trk, int voice = 0);
void stop();
Phase phase();
const Result& result();
// core 0, the OUT fill: per frame, what the test adds to the PRE pair, the POST pair and voice trk_voice's channel
// (the output stage adds them before its limits); false while idle (nothing to add)
bool out_frame(float* pre, float* post, float* trk, int* trk_voice);
// core 0, the IN completion: per frame, MAIN L / R (int32 full scale)
void in_frame(int32_t l, int32_t r);
bool active();                          // a measurement runs (core 0 may skip the calls else)
// "stereo=measure[:trk[:V]]", "stereo=measure:stop" are out.cpp's console; the result's text for the log / console
int text(char* out, int cap);
int json(char* out, int cap);           // "\"measure\":{...}"
void reset();                           // tests

}  // namespace meas
}  // namespace out
}  // namespace core
