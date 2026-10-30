// core/msamp/msamp.h - the MULTISAMPLE machine (docs/MULTISAMPLE.md): one machine plays an instrument of many samples
// mapped over keys and velocities (an SFZ file, sfz.h), polyphonic, with poly FM's chord params on P13..P16
// (core/poly.h: CHD INV SPR VOI, the same table and expansion). Pi and Mac only (CORE_MSAMP); the Teensy has none.
//
//   card     INST (P1, the 14-bit word bank * 128 + row) is a row of the project's INSTRUMENTS pool (core/lib/pools.h,
//            docs/POOLS.md): an .sfz (resolved by its hash) with its samples (WAV or AIFF: 8/16/24/32-bit integer or
//            32-bit float, mono or stereo, any rate) where the .sfz says - each a member of the pool entry, resolved by
//            its own hash (a member moved on the card plays from where it is now). OFF, empty and missing rows are
//            silent. kDir is where the auto-sampler writes its instruments.
//   memory   STREAM (the default) or RAM, per instrument. STREAM: each sample's first kHeadMs (`msamp=head:MS`, 500
//            by default) is preloaded (its "head"; a sample not much longer is held whole), and a loop that lies
//            past the head is preloaded too (up to kLoopMax frames); the rest streams from the card into the note's
//            ring (kRing frames) while it plays. Opening an instrument reads only the heads. RAM: every sample whole
//            (`<control> sidecar_load=ram` in the .sfz, or automatically for an instrument of kAutoRam bytes or
//            less); streamed instead when it needs more than half the pool RAM. Samples are held as int16 mono at
//            their own rate (stereo summed: the voice's channel is mono). The heads and RAM samples live in the pool
//            RAM's shared cache (core/pool/pcache.h: one copy a content, shared with GRANULAR and DSP SAMPLE; the
//            budget `poolram=MB`), the loop parts in private blocks of it. An instrument stays in its slot until the
//            slot is needed (least recently used, not selected, not playing); its heads stay cached until evicted.
//   loading  between blocks (step(): one open, one header or one read of up to kStreamChunk a call; the .sfz's text
//            in kChunk reads), a voice's wanted instrument first. A trig
//            before its instrument is ready plays when it is, if that is within kLateMs.
//   streaming between blocks too, before loading: one read of up to kStreamChunk a call for the note with the least
//            audio ahead, while it has less than the read-ahead (`ahead_ms`: 300 ms, or the SD card test's, sdtest.h).
//            At most `max_streams` notes stream at once (32, or the SD card test's): a new one beyond takes the oldest
//            released streaming note's place. A note that runs dry (an underrun) fades out within 1 ms and in again
//            when its data comes (counted); a note whose read-ahead falls under kCriticalMs takes the oldest released
//            streaming note's bandwidth (killed, counted). The render cores never touch the card.
//   notes    per voice kPerVoice notes (no voice steals another's: a note's render state is touched by one render
//            core only), kTotal at most in all. A note = one region's playback: its sample at the key's pitch
//            (Hermite interpolation, the rate folded into the step), its own AHDSR (ampeg_* scaled by ATK DEC SUS
//            REL), its loop. A trig releases the voice's earlier trig notes (their tails go on) and starts the
//            chord's notes (one note for CHD OFF), each the regions its key and velocity select (layers: up to
//            kLayers; round robin by seq_length/seq_position, random by lorand/hirand). 0x11 releases them and, with
//            RTRG on, starts the release regions (trigger=release) of the released keys.
//   cores    report / note_off / silence / step / command: the control core. render_add: the voice's render core.
//            A note's start, release and kill go to the render core through a sequence-locked mailbox; the render
//            core says when a note is done. No allocation after begin().
//
// The params (P1..P16 = sound words 9..16, 17..24):
//   P1 INST  P2 VEL (velocity sensitivity, x amp_veltrack)  P3 LEV (100 = unity)  P4 RTRG (>= 64: release regions)
//   P5 ATK  P6 DEC  P7 SUS  P8 REL (64 = the instrument's ampeg_*; times x 2^((v - 64) / 16), SUS below 64 scaled
//   down, above toward 100 %)
//   P9 TUNE (semitones, 64 = 0)  P10 FINE (64 = 0, +-1 semitone)  P11 STRT (0 = the region's offset; up to 127/128 of
//   the way to its end; a streamed sample's start stays inside its head)  P12 LOOP (zones: 0..42 AUTO = the instrument's, 43..85 OFF, 86..127 ON)
//   P13..P16 CHD INV SPR VOI (core/poly.h)
// Values outside a param's range clamp. The Rytm side (the machine, its labels) is a rytm-mods 0021 change; until it
// exists `msamp=voice:V` makes voice V play MULTISAMPLE whatever built-in machine the Rytm reports for it (the PoC:
// on a 6-OP FM track, whose page 2 knobs E..H the poly-chord mod already labels CHD INV SPR VOI).
#pragma once
#include <stdint.h>

#include "sfz.h"

namespace core {
namespace msamp {

constexpr int kSlots = 8;          // instruments resident at once
constexpr int kList = 1024;        // list entries (the INSTRUMENTS pool's codes)
constexpr int kPerVoice = 16, kTotal = 64, kLayers = 4;
constexpr int kMaxRegions = 1024, kMaxSamples = 512;
constexpr uint32_t kSfzMax = 256 * 1024;   // an .sfz's text at most
constexpr uint32_t kChunk = 8192;          // a card read while loading
constexpr uint32_t kStreamChunk = 32768;   // a card read while streaming
constexpr uint32_t kRing = 65536;          // frames a note's stream ring holds (128 KB; 0.68 s at 2x 48 kHz)
constexpr uint32_t kHeadMs = 500;
constexpr uint32_t kLoopMax = 1u << 20;    // a loop past the head preloaded up to this many frames
constexpr uint64_t kAutoRam = 8u << 20;    // an instrument this small (as int16 mono) loads whole
constexpr uint32_t kCriticalMs = 30;
constexpr uint32_t kLateMs = 250;
constexpr const char* kDir = "/MULTISAMPLE";

#if CORE_MSAMP
// Boot (engine_begin, after the card): the pool RAM (the first machine takes it), the slots, the list composed.
void begin(bool card);
bool step();                          // one bounded card job; true when it did one
void rescan();                        // the card's /MULTISAMPLE changed: listed again, unused instruments dropped
bool forced(int vi);                  // msamp=voice:V
// voices.cpp's hooks for a MULTISAMPLE voice vi (note -1: a 0x03 PARAMS)
void report(int vi, int note, int vel, const float* p);
void note_off(int vi);
void silence(int vi);                 // CC 120 / left the machine / the link reset: every note of the voice gone now
void render_add(int vi, float* out, int n);   // the render core's: the voice's notes added into out
void render_add2(int vi, float* l, float* r, int n);   // OUT PRE / POST: the pair (docs/STEREO.md)
// (gain staging as poly FM's, core/poly.h render_add: one key plays as before, a chord's keys at 1/sqrt of their
// envelopes' sum, smoothed; a trig chord's notes at 1 each, a live pad's at 0.6)
float headroom(int vi);               // tests: the voice's headroom gain now
// Live pad notes (core/poly.h's MIDI binding, a channel bound to this voice): a held note on key (the bound
// channel's ch / mnote), not released by a trig or 0x11; live_off releases the channel's mnote (-1: all of them; with
// RTRG on, their release regions start).
void live_on(int vi, int key, int vel, int ch, int mnote);
void live_off(int vi, int ch, int mnote);
int voice_notes(int vi);              // the voice's notes sounding now (the remote UI's chord view)
int voice_keys(int vi, uint8_t* keys, int max);   // their keys (release regions' notes left out; the Pi's VOICE page)
const char* entry_name(int inst);     // INST's list entry's name ("" none: no instruments)
// The INST list as the displays show it: entry e = code e of the INSTRUMENTS pool. As wt's.
int entries();
const char* entry_group(int e);
const char* entry_path(int e);
int entry_lib(int e);
int entry_state(int e);               // 0 none 1 card 2 loading 3 resident 5 failed
const char* entry_note(int e, char* b, int cap);
uint32_t list_gen();
int src_entry(int vi, int v);
bool pick(int vi, int e);             // a choice: INST written on the Rytm (48: the kit keeps it)
bool voice_picked(int vi);            // (always false)
bool command(const char* p, char* out, int cap);   // "msamp", "msamp=rescan", "msamp=voice:V[:off]", "msamp=list",
                                                   // "msamp=head:MS", "msamp=ahead:MS", "msamp=streams:N",
                                                   // "msamp=pick:V:ROW"
// The SD card test's (sdtest.h): the read-ahead and the streaming notes at most
void set_stream_limits(uint32_t ahead_ms, int max_streams);

struct Stats {
  int listed, resident, active, peak;
  uint32_t notes, steals, trigs, late, waits, loads, load_fails, left_out;
  int streams, streams_peak;          // notes streaming now, most at once
  uint32_t underruns;                 // note-blocks that ran dry
  uint32_t bw_steals;                 // notes killed for bandwidth (a full stream count, a note near dry)
  uint32_t stream_reads, loops_dropped;
  uint64_t stream_bytes;
  uint64_t arena, slot_cap;
};
const Stats& stats();
// tests: the list entry k's path ("" none); the instrument a voice plays now (-1 none)
const char* list_path(int k);
int voice_slot(int vi);
// tests: a slot's description (nullptr: not resident or loading)
const Desc* slot_desc(int slot);
uint64_t slot_bytes(int slot);
bool slot_ram(int slot);              // tests: the instrument is held whole
void set_head_ms(uint32_t ms);
void reset();                         // tests: every note free, the list and slots kept
#endif

}  // namespace msamp
}  // namespace core
