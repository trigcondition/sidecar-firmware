// core/poly.h - chords and polyphony on the 6-OP FM machine (docs/POLYPHONY.md "6. Built: poly FM").
//
// The chord parameters are the FM machine's page 2 (SMPL), P13..P16 = sound words 21..24 (P9..P16 were unused by
// 6-OP FM): CHD INV SPR VOI, each its 7-bit value as a direct index. A value beyond the range reads as the first
// entry, so an old sound's stock sample words there (STA 0, END 120, LOP 0, LEV 100) read OFF / 0 / 0 / AUTO.
//   CHD  0 OFF (mono, as before) 1 MAJ 2 MIN 3 SUS2 4 SUS4 5 7 6 MAJ7 7 MIN7 8 DIM 9 AUG 10 6 11 MIN6 12 9 13 ADD9
//        14 MIN9 15 5 (power)
//   INV  0..3: the inversion (k of the chord's lowest notes an octave up)
//   SPR  0..2: the spread, octaves the voicing opens over (note k of m raised 12 * (k * (SPR + 1) / m))
//   VOI  0 AUTO (the chord's own notes), 1..6 notes (fewer: the top dropped; more: the chord's notes again, an
//        octave up each round)
// A trig's note (its NOTE) is the root. sim/protocol.py has the Python twin (CHORD_NAMES, chord_expand).
//
// The pool (CORE_POLY builds only: the Pi and the Mac; the Teensy, frozen at 0.1.0, has none): per voice up to
// kPerVoice notes, kTotal in all, each its own Plaits voice + rytm::FmMachine (its own envelopes, its own LEN gate),
// summed into the voice's channel after the voice's mono note. Steal: the same pitch on the voice, then a free slot,
// then the voice's oldest (at its cap), then the oldest released anywhere, then the oldest. A 0x04 with CHD on starts
// a chord (the voice's earlier chord and mono note released, their tails go on); 0x11 releases the trig notes; a slot
// is free again once released and quiet.
//
// Live chords (the host half of live chord recording): the Rytm's pads send every note-on / note-off on USB MIDI (on
// the auto channel in CHRO), while its own voice plays one note. A channel is bound to an FM voice (or a MULTISAMPLE
// one: its notes then are msamp's live notes, msamp.h live_on / live_off; test-1031) when two of its
// note-ons each come within kBindMs of that voice's 0x04 with the same note offset (the 0x04's note - the MIDI note).
// A bound channel's notes play on the voice's pool at once (monitoring), as held; while it is active (a note held, or
// one within kActiveMs) the voice's own 0x04s are swallowed when a matching note-on came (or comes within kHoldMs:
// held that long, then played). The note-ons that start within kWindowMs of a group's first are one chord: when the
// window closes, 2+ notes are detected (detect()) and written back to the Rytm for the step it just recorded:
//   host -> Rytm  28 01 <track 0..11> <root> <CHD> <INV> <SPR> <VOI>    (13 bytes; any other length refused)
//   Rytm -> host  28 02 <track> <step | 7F> <status>    0 ok, 1 no recent step, 2 lock slots full, 3 not recording /
//                 not a Sidecar track / track > 11 (rytm-mods poly-chord; 0x30..0x3F are all 0026's, 0022 routes 28 to it)
// The Rytm writes it only while it records; the host sends it for every chord (it does not know RECORD).
#pragma once
#include <stdint.h>

namespace core {
namespace poly {

constexpr int kChords = 16, kInvs = 4, kSprs = 3, kVoiMax = 6;
constexpr int kMaxNotes = kVoiMax;   // a chord's notes at most (VOI 6)
constexpr int kPerVoice = 6, kTotal = 24;
constexpr uint8_t kChordCmd = 0x28, kChordWrite = 0x01, kChordReply = 0x02;
constexpr uint32_t kWindowMs = 60, kBindMs = 30, kHoldMs = 8, kActiveMs = 1500;
// The Rytm's pads in CHRO are MONO per track [measured 2026-10-20, rui1031]: a second pad sends the first one's note-off
// right after its own note-on (on A, on B, off A, on C, off B, ... and only the last pad's real off). On a channel not
// bound by poly=bind (a keyboard's offs are real), a note-off for the channel's previous note within kStealMs after
// another note's note-on, when that note-on came within kGestureMs of the previous one, is the Rytm's steal: the note
// is kept (sounding, in the chord); the next real note-off releases the group (the notes of its gesture, the pads
// played each within kGestureMs of the one before). A non-overlapping melody sends real offs (before the next on):
// never a steal. A chord's window closes kGestureMs after its last note-on (a strum of pads 80 ms apart is one chord).
constexpr uint32_t kStealMs = 5, kGestureMs = 100;

extern const char* const kChordNames[kChords];   // "OFF" "MAJ" ... (the Rytm's CHD value texts)

struct Chord {
  int chd, inv, spr, voi;
};
// The chord params from P1..P16 (p[12..15]), each read as above.
Chord chord_of(const float* p);
// The chord's notes on root, ascending and distinct, in 0..127 (others dropped); the count (1 for OFF).
int expand(int root, const Chord& c, uint8_t* out);
// Held notes (any order, repeats ignored; at most kMaxNotes kept, the lowest) -> the chord expand() plays from root.
// exact: expand(root, chord) gives exactly those notes. One note (or none): OFF, VOI 0. A set no chord reproduces: the
// chord whose pitch classes are nearest (Jaccard > 1/2; INV from the bass, SPR from the span, VOI the count), else
// OFF on the lowest note.
struct Detected {
  int root;
  Chord c;
  bool exact;
};
Detected detect(const uint8_t* notes, int n);
// The value text of chord param k (0 CHD, 1 INV, 2 SPR, 3 VOI) at value v: "MAJ7", "INV2", "SPR1", "AUTO" / "4".
const char* value_text(int k, int v, char* buf, int cap);
// 28 01 into m (13 bytes); the length.
int sysex_chord_write(uint8_t* m, int track, int root, const Chord& c);

#if CORE_POLY
void init();                                  // boot (voices_init): the slots (heap), every one free
// voices.cpp's hooks for an FM voice vi
void set_voice(int vi, const uint8_t* dx7);   // the DX7 voice its PAT picked (128 B, copied) or nullptr (silent)
void knobs(int vi, const float* p);           // P1..P8 changed: its sounding notes take them
bool take_trig(int vi, int note, int vel, const float* p);   // a 0x04: true when the pool took it (no mono note)
// The live binding's part of take_trig, for MULTISAMPLE too (voices.cpp: an MS voice's 0x04): true when a bound pad
// plays it (swallowed, or held kHoldMs for its note-on; a held one that gets none plays through msamp::report).
bool take_live(int vi, int note, int vel);
// The remote UI's chord view (core/ui/page_gfx.cpp) of voice vi: the last trig's root (60 before any), or, when the
// pads played since (a live binding), the chord detected from them (live: c is that chord, else read c from P13..P16).
struct View {
  int root;
  Chord c;
  bool live;
};
View view(int vi);
int voice_notes(int vi);                      // vi's notes sounding now: the pool's (6-OP FM) or MULTISAMPLE's
void note_off(int vi);                        // 0x11: its trig notes released
void silence(int vi);                         // CC 120 / left FM / the link reset: every note of the voice gone
bool active(int vi);                          // the voice has a sounding note in the pool
// The render core's: the voice's pool notes added into out (n samples, -1..1 before the 0.7 headroom); stage as
// FmMachine::Render's. Gain staging: each note at its own gain (a trig's 1, a live pad's 0.6, as one note always
// was), their sum times the voice's headroom gain 1/sqrt(max(1, E)), E the notes' weights (1 a held note, a released
// one its block peak over its peak while held), smoothed (kHeadAtkMs down, kHeadRelMs up): one note plays as before,
// a chord of n held notes at 1/sqrt(n) each; voices.cpp's soft limiter (soft_limit) takes the overs that are left.
constexpr float kHeadAtkMs = 3.0f, kHeadRelMs = 15.0f;
void render_add(int vi, float* out, int n, uint8_t* stage);
float headroom(int vi);                         // tests: the voice's pool headroom gain now
int held_notes(int vi);                         // tests: the voice's pool notes held (not released) now
// Every note message poly::midi takes, as the voice hears it (a mono steal's note-off left out, the group's note-offs
// at its release): for the Sidecar sequencer's recorder (seq-mvp). nullptr: none.
void set_note_sink(void (*f)(uint8_t st, uint8_t d1, uint8_t d2));
// The control side's
void midi(uint8_t st, uint8_t d1, uint8_t d2);   // a channel message from the Rytm (its pads' notes)
void tick(uint32_t ms);                          // every pass: a held 0x04's time, a chord window's end
bool reply(const uint8_t* d, int len);           // a whole 28 02 from the Rytm (true: it was one)
// The Sidecar sequencer's notes (core/seq/seq.h) on voice vi: 6-OP FM's pool or MULTISAMPLE's live notes (channel
// kSeqCh), held until seq_off (note -1: every seq note of the voice). Other machines: nothing (false).
constexpr int kSeqCh = 16;
bool seq_on(int vi, int note, int vel);
void seq_off(int vi, int note);
int bound_voice(int ch, int* off);               // the voice MIDI channel ch (0..15) plays live (-1 none), its note offset
// The Pi's own keyboards (core/keys.h): note on voice vi from keys channel kc (kKeysCh0.., one per source and MIDI
// channel), held until key_off (note -1: every note of kc on vi): 6-OP FM's pool (at a live pad's gain) or MULTISAMPLE's
// live notes. Other machines: nothing (false). keys_view: the chord view shows the keys' held notes (as a pad chord's).
constexpr int kKeysCh0 = 32;
bool plays_poly(int vi);                         // vi plays 6-OP FM or MULTISAMPLE now (the keys' notes go to the pool)
bool key_on(int vi, int note, int vel, int kc);
void key_off(int vi, int note, int kc);
Detected keys_view(int vi, const uint8_t* notes, int n);
struct Stats {
  int active, peak;   // notes sounding now, most at once
  uint32_t notes, steals, chords, live_notes, swallowed, held, writes, replies_ok, replies_err;
  uint32_t mono_steals;   // the Rytm's mono note-offs kept (kStealMs)
  uint32_t seq_pads;      // fix1040b: a seq track's own pad played its voice (the normal pad mode)
  uint32_t trk_pads;      // 1042: a pad's note with the Rytm's TRK held (a track selection): no note
  int bound[16];      // channel -> voice (-1 none)
};
const Stats& stats();
bool command(const char* p, char* out, int cap);   // the console's "poly": the stats line
void reset();                                      // tests: every note free, no binding, stats cleared
// rui1040: the Rytm's selected track moved to a track on voice vi (-1 none): a channel the pads bound (votes, not
// poly=bind) to another voice is unbound - the auto channel's CHRO notes are the selected track's, so a binding learned
// on the seq track must not play its voice for the next track's chromatic pads
void selected_voice(int vi);
// 1042: the Rytm's TRK key held / released (the remote page's 31; core/ui/remote.cpp, cleared when the page closes): a
// note on the auto channel while it is held is TRK + pad's (a track selection), not a note
void rytm_trk(bool held);
// fix1040b: a seq track's own pad in the normal pad mode (its trigger note 0..11 on the auto channel) still plays its
// Sidecar voice as any Sidecar track's pad does. voices.cpp asks seq_pad for a 0x04 on a voice a seq track owns: true -
// that track's pad was pressed just now (kBindMs): play it as a trig; false - held kBindMs for the pad's note-on (which
// then plays it here), else dropped (the Rytm's own trig on a seq track makes no note). The pad's release ends it.
bool seq_pad(int vi, int note, int vel);
// fix1052: a seq track's own pad on voice vi is held now (seq_pad / pad_trigger played it: the voice's own machine,
// which the pool's activity does not show). seq.cpp's silence counts it, so a pinned VCA opens under the pad's note.
bool pad_sounding(int vi);
#endif

}  // namespace poly
}  // namespace core
