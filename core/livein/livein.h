// core/livein/livein.h - LIVE IN (docs/LIVE-IN.md): a Sidecar voice that plays the device's AUDIO IN (on the Rytm
// MKII: Overbridge IN 10 / 11, core/fx/out.h Device::audio_in) through the voice's own INSERT chain (core/fx/insert.h:
// AMP, DRIVE, EQ, ...), its sends and its OUT route (TRK / PRE / POST, docs/STEREO.md). Pi and Mac only (CORE_MSAMP):
// the Sidecar Mini has no INSERT chains. Machine flag 0x78 (core/machines.h kLiveMachine); until the Rytm's machine
// list has it, `livein=voice:V` plays it on voice V whatever built-in machine the Rytm reports (as wt= / gr=).
//
// The knobs (the voice's reported P1..P9; the remote UI's LIVE IN page draws them with these labels):
//   P1 INPT  L / R / L+R (mono: (L + R) / 2) / STR (stereo: L and R through a PRE / POST route; mono on TRK)
//   P2 GAIN  the input's level, -24..+24 dB (64 = 0 dB)
//   P3 GATE  a noise gate: 0 OFF, 1..127 the threshold -80..-20 dBFS (after GAIN; it closes 3 dB under it)
//   P4 HOLD  the gate's hold after the input falls under it, 0..1 s; it then closes over HOLD / 4 + 5 ms
//   P5 MON   ALW: the input passes all the time (an effects lane) / TRIG: a trig opens an envelope on it (ATK DEC SUS
//            REL): rhythmic gating, as an Octatrack thru machine. Velocity is not used.
//   P6 ATK   TRIG's attack 0.5 ms..2 s;  P7 DEC  its decay 5 ms..5 s;  P8 SUS  the level it decays to (0: a one-shot,
//            no note-off needed);  P9 REL (page 2) the release after the note-off, 5 ms..5 s
// The output level is the route's (the Rytm's AMP / VOL on TRK, OUT LEV on PRE / POST, the INSERT AMP's LEV).
//
// Threads. in_block: the IN side's one writer (the Pi's IN completion on core 0): a ring of float pairs, the write count
// stored with release. render / post: the voice's render core (each voice its own read point, its own state). The rest:
// the control core. Nothing allocates; no card I/O anywhere here.
//
// The read point (render): a voice starting LIVE IN reads the newest block; a short read (the stream behind: an
// underrun) pads silence, counts it, and leaves the read point where the stream is (the delay grows only as far as the
// stream's jitter needs); once a second a backlog that never fell under kSlack frames is cut back to the newest block
// (counted as a skip). No stream at all: silence and the underrun counter.
//
// The feedback guard (post, after the INSERT chain): a hard limiter at -1 dBFS on the voice always; and when the
// input rises window over window (or sits near full scale) while the voice sounds, a probe: the voice's output dips
// 6 dB for 30 ms, and if the input dips and recovers with it (the input follows the voice: a loop) the voice is muted
// for 2 s, ramps back over 0.5 s, and the log says why. A probe that finds no loop leaves the voice alone for 3 s.
#pragma once
#include <stdint.h>

namespace core {
namespace livein {

constexpr int kVoices = 8;
constexpr int kRing = 2048;   // frames of AUDIO IN held (42 ms at 48 kHz)
constexpr int kSlack = 24;    // a backlog over this for a whole second is cut back
enum { kInL = 0, kInR, kInMono, kInStereo, kInputs };   // P1 INPT's zones
enum { kMonAlways = 0, kMonTrig, kMons };              // P5 MON's zones
int input_of(float p1);       // P1 -> kIn*
int mon_of(float p5);         // P5 -> kMon*
float gain_db(float p2);      // P2 -> -24..+24
float gate_db(float p3);      // P3 -> the threshold, dBFS; 0 OFF (returns 0)
float hold_ms(float p4);
float atk_ms(float p6);
float dec_ms(float p7);       // also REL's (P9)
// the knobs' texts for the pages ("L+R", "+6DB", "OFF", "-48DB", "120MS", "ALW", "TRIG", ...); k 0..8 (P1..P9)
void value_text(int k, float p, char* b, int cap);
const char* label(int k);     // "INPT" "GAIN" "GATE" "HOLD" "MON" "ATK" "DEC" "SUS" "REL"

// ---- the IN side (one writer)
void in_block(const int32_t* f, int n, int stride);    // n frames, channel c of frame i at f[i * stride + c]
void in_frames(const float* l, const float* r, int n); // the same from floats -1..1 (tests, demos; r null: l on both)
bool wanted();                                          // a voice plays LIVE IN (else the IN side may skip in_block)

// ---- the control core
bool forced(int vi);                 // livein=voice:V
void force(int vi, bool on);
void trig(int vi);                   // a trig or a sequencer note on a LIVE IN voice (MON TRIG's envelope)
void release(int vi);                // its note-off
void silence(int vi);                // the voice left LIVE IN: its envelope and guard state cleared at its next block
void set_fifo(int frames);           // the platform's output FIFO target (the latency estimate; default 96)
int fifo();                          // that target (CHOP's LAZY latency estimate too)
void step();                         // between blocks: the guard's and the measure's news into the log
// "livein" (the voices' state, levels, latency, counters), "livein=voice:V[:off]" (V 0..7), "livein=measure[:V]" (the
// round trip: patch the device's main out -> its AUDIO IN first), "livein=measure:stop". False: not a livein command.
bool command(const char* p, char* out, int cap);
// The STATUS words for voice vi (<= 10 characters): "LIVE -18DB", "LIVE GATED", "LIVE NO IN", "LIVE MUTED"
int status_short(int vi, char* b, int cap);
// The round trip AUDIO IN -> the voice -> the device's output, ms x 10: the measured one if livein=measure found it
// (*measured true), else the estimate: the device's own loop + the FIFO target + the read point's mean backlog.
int latency_ms10(int vi, bool* measured);

// ---- the render core (the voice's)
// One block (n <= 12 frames): the input as the knobs p (P1..P16) say into l (and r when stereo: the voice's OUT is PRE
// / POST; else mono into l).
void render(int vi, const float* p, float* l, float* r, bool stereo, int n);
// After the voice's INSERT chain: the guard (probe / mute gain), the limiter, the measure's clicks; the rows as
// render_voices writes them (ring_r null: mono)
void post(int vi, int16_t (*ring)[8], int16_t (*ring_r)[8], const int* row, int n);

struct Stats {
  uint32_t frames_in;          // frames the IN side wrote (all voices share them)
  uint32_t blocks, underruns, skips, limited, probes, trips;
  int16_t in_db10;             // the input's peak after GAIN over the last ~0.3 s, dBFS x 10 (-1000: silent)
  uint8_t gate_open, muted, stream;   // stream: frames came within the last 0.2 s
  uint16_t backlog;            // the read point's mean backlog over the last second, frames
  int32_t measured;            // the measured round trip, frames (-1 none)
  uint8_t meas_state;          // 0 idle, 1 running, 2 done, 3 failed
  int16_t meas_found;          // clicks found of the last measure
};
Stats stats(int vi);
void reset();   // tests: every voice off, the ring empty, the counters 0

}  // namespace livein
}  // namespace core
