// core/fx/out.h - the OUT route of a Sidecar voice (docs/STEREO.md; the stereo / POST design 2.3): where the voice goes
// into the Rytm, per voice, stored per kit with the INSERT settings (core/fx/insert_card.cpp: "OUT V3 POST 100 64 64 0").
//
//   TRK  (the default; as before) the voice's own track channel (host -> Rytm 4..11): the Rytm's track FLTR, AMP, OVER,
//        VOL, PAN, its DEL / REV sends and the master FX apply. Mono.
//   PRE  host -> Rytm 2/3 (MAIN FX: the delay / reverb return buffers, before the master distortion + compressor).
//        Stereo; the Rytm's track chain skipped.
//   POST host -> Rytm 0/1 (MAIN: after them, nothing else applies). Stereo.
// The names are the send FX's (core/fx/send_fx.h P1 ROUTE): POST means 0/1 everywhere in the Sidecar.
//
// On PRE / POST the voice renders a stereo pair (core/voices.cpp: the sample machines in stereo, the others L = R) and
// its INSERT chain runs in stereo; the output stage (the Pi's pi/engine/output.cpp, core 0) applies, per voice:
//   LEV  0..127, 100 = unity (the voice's level as on TRK, before the Rytm's own VOL)
//   PAN  0..127, 64 = centre: a stereo voice's balance, a mono one's equal-power pan (normalised: centre = unity)
//   WID  0..127, 64 = as rendered, 0 = mono, 127 = the side x 2 (M/S)
//   the mute gate: the voice's track muted on the Rytm (2D 01 05 / CC 94: the Rytm's MUTE only stops trigs, a loop on
//        POST would keep playing) -> a 5 ms fade out, unmuted -> 5 ms in. A shared voice (RS/CP, MT/HT, CH/OH, CY/CB)
//        follows the track that last triggered it when that is known (the Sidecar sequencer's trigs), else it is gated
//        when both of its tracks are muted (the Rytm's trig report does not name the track).
// then sums the PRE voices onto one pair and the POST voices onto another, each soft-limited (linear to 70 % of the
// send FX returns' ceilings, a tanh knee above: kLimitPre on 2/3, kLimitPost on 0/1) and scaled by the pair's trim (the calibration of the real Rytm's USB
// IN levels, docs/STEREO.md "Measurements": 0 dB until measured), added before the send FX returns and take playback.
// An OUT change crossfades over kXfadeFrames (8 ms, equal power) from the old destination to the new. OUT is not
// p-lockable (a structural change); LEV / PAN / WID are LFO targets (O1..O8, params 0..2).
//   DEC  DSP SAMPLE on PRE / POST (no Rytm AMP to shape it): 0 OFF (to the sample's end or the note-off, a 2 ms release),
//        1..127 an exponential decay from the trig, 20 ms .. 10 s (the time to -60 dB). Other machines have their own
//        envelopes.
// The send FX: the voice's Rytm stem (IN 2..9) is silent off TRK, so the output stage hands the voice's mono sum x LEV x
// the gate to the send FX in that stem's place (sendfx_pi FromVoices / FromRytm): the track's FX1 / FX2 send levels keep
// working, with the same lag as the stems.
//
// Two halves, as insert.h: out::Control (bytes the render cores and core 0 read; written on the control core by
// out.cpp's settings) and out::Mixer (out_mix.h: the DSP, header-only and allocation-free, the output stage's).
// (No libm here: the Pi's Circle side includes this through pi/engine/output.h. The DSP is out_mix.h.)
#pragma once
#include <stdint.h>

namespace core {
namespace out {

constexpr int kVoices = 8;
enum Route : uint8_t { kTrk = 0, kPre = 1, kPost = 2, kRoutes };
constexpr int kXfadeFrames = 384;    // 8 ms: an OUT change (the send FX route's kPanFrames)
constexpr int kGateFrames = 240;     // 5 ms: the mute gate
constexpr float kLimitPre = 0.5f;    // -6 dBFS on 2/3 (send_fx.h kLimitFx)
constexpr float kLimitPost = 0.25f;  // -12 dBFS on 0/1 (send_fx.h kLimitMain, core::MainOut::kLimit)
constexpr float kSmoothMs = 3.0f;    // LEV / PAN / WID smoothing
// The device the voices go into, as a table (device-agnostic host side: a future device - an Analog Four MKII - adds
// its own row and its firmware mod, nothing else here changes): its host -> device channels for the main return
// (`post`, after its master FX), the FX return (`pre`, before them), each voice's own channel (`voice`), the index of
// the voice's stem in the device -> host stems (`stem`: the send FX's input the voice stands in for on PRE / POST),
// the tracks that play each voice (a shared voice: two; -1 none) for the mute gate, the device -> host channels of its
// AUDIO IN (`audio_in`: what LIVE IN plays, core/livein/livein.h) and its own OUT -> IN loop (`loop_frames`: host -> device
// frames back in the device -> host stream, the converters and the device's buffers; LIVE IN's latency estimate). The only
// device so far: the Analog Rytm MKII over Overbridge (device-specific: docs/STEREO.md "Device-specific").
struct Device {
  const char* name;
  uint8_t channels;          // host -> device channels a frame
  uint8_t post[2], pre[2];   // the returns' L / R channels
  uint8_t voice[kVoices];    // each voice's own channel (TRK)
  uint8_t stem[kVoices];     // each voice's stem (0.. in the device's stem order)
  int8_t tracks[kVoices][2];
  uint8_t audio_in[2];       // device -> host: AUDIO IN L / R
  uint16_t loop_frames;      // host -> device -> host, frames (0: not measured)
};
// the Rytm: voices BD BT SD LT CH/OH MT/HT CY/CB RS/CP (core/ui/remote.cpp kTrackVoice), channels 4..11 / stems
// IN 2..9 in the order BD SD RS/CP BT LT MT/HT CH/OH CY/CB, tracks BD SD RS CP BT LT MT HT CH OH CY CB; AUDIO IN L / R on
// IN 10 / 11 (ob_block.h); the loop 289 frames [measured 2026-10-15 on the Rytm, docs/pi-engine.md: a voice's onset as
// sent in OUT -> back in MAIN on IN, FIFO not included]
constexpr Device kRytmMk2 = {"Analog Rytm MKII", 12, {0, 1}, {2, 3}, {4, 7, 5, 8, 10, 9, 11, 6}, {0, 3, 1, 4, 6, 5, 7, 2},
                             {{0, -1}, {4, -1}, {1, -1}, {5, -1}, {8, 9}, {6, 7}, {10, 11}, {2, 3}}, {10, 11}, 289};
// the device connected (the only one so far)
inline const Device& device() { return kRytmMk2; }

struct Control {
  volatile uint8_t route[kVoices];   // Route (the knob: what the render and the output stage follow)
  volatile uint8_t lev[kVoices];     // as they play (the knob + the LFO's offset), 0..127
  volatile uint8_t pan[kVoices];
  volatile uint8_t wid[kVoices];
  volatile uint8_t dec[kVoices];     // DSP SAMPLE's DEC: 0 OFF, else 1..127
  volatile uint8_t open[kVoices];    // the mute gate's target: 1 sounding, 0 the voice's track muted
  volatile int16_t trim10[2];        // the PRE / POST pair's trim, tenths of a dB (the calibration; 0 by default)
};
extern Control control;
inline void defaults(Control& c) {
  for (int v = 0; v < kVoices; ++v) c.route[v] = kTrk, c.lev[v] = 100, c.pan[v] = 64, c.wid[v] = 64, c.dec[v] = 0, c.open[v] = 1;
  c.trim10[0] = c.trim10[1] = 0;
}

inline bool stereo(int vi) { return control.route[vi & 7] != kTrk; }
inline const char* route_name(int r) { return r == kPre ? "PRE" : r == kPost ? "POST" : "TRK"; }
int route_from_name(const char* s);          // "trk" / "pre" / "post" (any case), -1 neither
// DEC's decay a sample for t60 at value v (1..127: 20 ms .. 10 s, exponential), 1 for OFF
float dec_coef(int v);
// DEC's text: "OFF", "20MS" .. "10S"
void dec_text(int v, char* b, int cap);
// LEV's / PAN's / WID's texts ("100", "L32" "C" "R12", "64")
void value_text(int k, int v, char* b, int cap);

// ---- the settings (the control core's: the remote page, the touchscreen, the console, the card)
void set_route(int vi, int r);
void set_param(int vi, int k, int v);        // k 0 LEV, 1 PAN, 2 WID, 3 DEC
int param(int vi, int k);                    // the knob
void lfo_offset(int vi, int k, int off);     // the LFOs' targets O1..O8 (k 0..2: LEV PAN WID), -127..127
void set_mutes(uint16_t mask);               // the Rytm's 12 tracks muted (seq: 2D 01 05 / CC 94)
void trig_track(int vi, int track);          // the track that triggered voice vi (the Sidecar sequencer's), -1 unknown
bool any_stereo();                           // a voice is routed PRE or POST
void set_trim(int pair, int db10);           // stereo=trim:pre|post:DB10
// The kit file's line ("OUT V3 POST 100 64 64 0"): from_line takes one (false: not an OUT line it reads); line(vi)
// writes the voice's when it is not the default (0 none)
bool from_line(const char* const* w, int n);
int line(int vi, char* b, int cap);
void clear();                                // every voice TRK, LEV 100, PAN 64, WID 64, DEC OFF (a new kit file)
// The console: "out=V3:post", "out=V3:lev:100", "out=V3:pan:64", "out=V3:wid:64", "out=V3:dec:off|N", "out=V3"
// (that voice), "out=list"; "stereo" the routes and the calibration; "stereo=trim:pre|post:DB10";
// "stereo=measure[:trk[:V]]" / "stereo=measure:stop" (core/fx/out_measure.h, docs/STEREO.md "Measurements"). False: not an out / stereo command (out=main and
// out=tracks are the output stage's own: not taken here).
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);                // "\"out\":[{...}, ...]" (/status "ins")
struct Stats { uint32_t sets, xfades; };
const Stats& stats();
// The settings changed since the card file was written (the INSERT file holds them: insert.cpp's dirty mark)
extern void (*on_change)();
void reset();                                // tests

}  // namespace out
}  // namespace core
