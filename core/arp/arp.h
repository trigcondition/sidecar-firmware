// core/arp/arp.h - the arpeggiator (backlog 7ae; docs/ARP.md): a track's held or sequenced chord played as a pattern, in time with the
// Rytm. Any Sidecar machine, the MIDI machine (core/midim/midim.h) too: what a voice would have played goes into the voice's held
// notes here, and one note at a time comes out through inst::play_on (core/inst.h), the path every machine takes from the sequencer.
//
// In: a Rytm trig (held until its 0x11, its CHD chord on the machines with chord words), the sequencer's notes (inst::seq_on: held
// until their own off), the pads' and keyboards' live notes (poly.cpp live_start / live_release). With a voice's mode OFF nothing is
// taken and the voice plays as before. An off for a note not held here passes on: a note started before the arp came on ends as it would.
//
// Settings: per sound, a 10-bit code in the Rytm's sound (rytm-mods 0021, stored slot 46, live +0x6d and the two bits above the tag), so
// they are saved with sounds and kits and an old sound reads 0, OFF. The Rytm treats it as opaque: the reading of it is here.
//   code = mode + 6 x (speed + 6 x (range + 3 x (gate + 3 x swing))); fields stored so 0 is the default, values above kCodes - 1 read 0
//   Rytm -> host  45 01 <track> <code & 7F> <code >> 7>   after a hello, and when the active kit's sound changes (a kit, a load, a paste)
//                 ... and from ext_render just before a trig whose kit's code it has not said: a new kit's first step plays by its own code
//   host -> Rytm  45 01 <track> <code & 7F> <code >> 7>   the page's edit: the active kit's live sound and the project copy
// A voice shared by two tracks (RS / CP ...) takes the first of them with its mode on.
//
// Time: grid point n of interval g (1/16ths) falls at origin + n g + (n odd ? swing g : 0), from the sequencer's step clock (seq::beat_map),
// `latency` early as its own notes. Stopped, or with no ticks yet: free running from the first note at the last tempo reported. Each
// render block (seq::pre_block): the note's off, then what is due. A chord that starts just after a grid point (a trig always does:
// its report comes after its step) plays at once; else at the next point.
//
// Ending: the held notes empty: the note plays out its gate; a hold that ended before its first note (off the grid, short) plays that note then. STOP or pause: a trig's and the sequencer's notes go,
// keys and pads that are down run on, free. Another kit (a 3B naming the same kit is an open's: nothing), machine change, a voice's silence, the link gone, a mode set OFF:
// everything of the voice ends now, but a trig just made (the new kit's own step, reported before its 3B).
//
// Locks (docs/SEQUENCER.md "Arp locks"): a sequencer step locks any of the five settings (seq.h kLockArp), on a seq track only. lock() lays them over the
// voice's settings from the step until the next step, or until another step's set says otherwise; a voice with no lock runs the code above and nothing else.
// The code's values stay as they were. MODE locked on, the sound's OFF: the step's notes are held here, and end with the step. MODE locked OFF, the sound's on: the
// step's own notes play as if no arp, what was held rests and runs on after it. A note sounding keeps its own pitch for its off, whatever the lock does to the pattern.
//
// The control side (the Pi's core 3): between render blocks, no card, bounded work: pre_block does nothing while no voice runs,
// else at most kVoices clock reads, a sort of kHeld notes at each note.
#pragma once
#include <stdint.h>

#include "seq/seq.h"   // PolyCfg

namespace core {
namespace arp {

enum Mode : uint8_t { kOff = 0, kUp, kDown, kUpDown, kRandom, kPlayed, kModes };
constexpr int kSpeeds = 6, kRanges = 3, kGates = 3, kSwings = 3;   // SPD 1/4 1/8 1/8T 1/16 1/16T 1/32, RNG 1..3 octaves, GATE 25 50 100 %, SWNG 50 58 66 %
constexpr int kCodes = kModes * kSpeeds * kRanges * kGates * kSwings;
constexpr int kHeld = 16;           // notes held on a voice: the oldest gives way
constexpr uint8_t kCmd = 0x45;
constexpr uint32_t kFreshMs = 100;  // 0026's UI tick (50 ms) and the way
constexpr uint8_t kLiveSeq = 0xFE;  // a held note's source: the sequencer
constexpr uint8_t kLiveTrig = 0xFF; // ... a Rytm trig; else a live channel (poly.h's)

struct Cfg {
  uint8_t mode, speed, range, gate, swing;   // indices into the tables above, in the knob's order (speed 3: 1/16)
};
Cfg decode(int code);
int encode(const Cfg& c);
const char* mode_name(int m);    // "OFF" "UP" "DOWN" "UPDN" "RND" "PLAY"
const char* speed_name(int s);   // "1/4" .. "1/32"
int gate_pct(int g);
int swing_pct(int s);

bool sysex(const uint8_t* d, int len);      // a whole 45 from the Rytm: true when it was one
void set(int track, const Cfg& c);          // an edit (the ARP page, the console): played from now, sent to the Rytm
Cfg get(int track);                         // track 0..11's settings
bool on(int vi);                            // voice vi arpeggiates (its machine a Sidecar one, its mode on)

bool seq_on(int vi, int note, int vel, const seq::PolyCfg& cfg);   // the sequencer's note: true when held here
bool seq_off(int vi, int note);             // true: it was; note -1: every sequencer note, false (the caller ends its own too)
void plain_off(int vi, int note);           // the sequencer's off of a pitch still sounding on another of its notes: a note played past a lock of MODE OFF ends
bool live_on(int vi, int note, int vel, int ch, int key);          // a pad's or keyboard's note; key names it for live_off
bool live_off(int vi, int ch, int key);     // true: it was; key -1: every note of ch, false
bool trig(int vi, int note, int vel, const float* p);              // a Rytm trig (mono on the voice): true when held here
bool trig_off(int vi);                      // its 0x11: true when it ended the voice's trig here

// The sequencer's step: fields of mask (bit i: val[i], in Cfg's order) laid over the voice's settings until host frame until; mask 0: none now. track: the step's
void lock(int vi, int track, int mask, const uint8_t* val, int64_t until);
int locked(int vi);                         // the mask in force, 0 none

void block(int64_t h);                      // seq::pre_block, each render block: h its first frame
void stop();                                // the transport left PLAY
void kit_changed();                         // another active kit: every voice's notes end, but a trig of the last kFreshMs (the new kit's own: its 3B comes after it)
void link_lost();                           // the Rytm's USB gone: every note ends, no 0x11 will come
void end(int vi);                           // vi's notes end now (a machine change, a silence)
void machines();                            // the Rytm's 33: a voice whose machine changed ends
int held(int vi);                           // tests: the notes the arp holds on voice vi
bool busy(int vi);                          // holding notes or sounding one (the pin's silence, seq.cpp)
void tick(uint32_t ms);                     // every control pass: what the sequencer must clock and pin

struct Stats {
  uint32_t notes, late, chords, reports, edits, released;
  uint32_t worst_us;   // block()'s slowest pass, its notes' starts included ("arp" on the console: the Pi's real number)
  int running;
};
const Stats& stats();
bool command(const char* p, char* out, int cap);   // the console's "arp", "arp=T:mode=up,spd=3,rng=1,gate=1,swng=0" (T 1..12)
void reset();                               // tests: nothing running, every code 0

}  // namespace arp
}  // namespace core
