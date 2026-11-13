// core/seq/rytm_dump.h - the Rytm's own pattern for the touch Rytm view: the stock SysEx pattern dump of its working
// buffer, requested and read back here (sim/protocol.py RYTM_DUMP_REQ / rytm_dump_parse, the reference; the format
// verified in rytm-emu on OS 1.73: rytm-mods tools/emu/sq_emu.py "fetch"). Pi and Mac (CORE_POLY builds).
//
//   request  F0 00 20 3C 07 00 64 01 01 <n> 00 00 00 05 F7          pattern n; the ACTIVE pattern's answer is its working
//            buffer as edited, saved or not [measured: an unsaved trig placed in GRID REC is in it]. 0x80 | n (the
//            community's "working buffer") answers the same, but 0x80 is not a MIDI data byte: a byte-stream MIDI path
//            (rytm-emu's link, strict host stacks) cuts the SysEx there [measured]. So n = the active pattern (2D 02);
//            0x80 only while it is not known (a Rytm without the mod).
//   answer   F0 00 20 3C 07 00 54 01 01 <pattern> <the 13101-B record 7-bit packed from here: groups of 8, a byte of
//            the next 7 bytes' top bits, then those 7; so 00 <00 00 00 05: the record's header> ..: 14973 B>
//            <checksum hi lo: sum of the bytes after <pattern> to it & 0x3FFF> <length hi lo: size - 10> F7   (14988 B)
//            [measured on a real Rytm MKII: patterns 1 and 13 answer sums that leave their number out; pattern 0 hid it]
//
// The record (13101 B) is what pi/ui/rytm_pattern.h's rytm_pattern_decode takes; this module only fetches and checks
// it (no layout knowledge), so the decoder stays the touch UI's (read-only here).
//
// When: the platform's consumer turns it on (set_page: the PAT page, or the VOICE page, which only needs the track's
// p-locks), then a fetch goes out at once; and on want(). A 15 KB answer is costly for the Rytm (its UI slows while it
// builds one, worst while it plays and records), so everything else is paced:
//   pattern change (seq's 2D 02)  kPatternMs after the last one (a quick run of switches: one fetch, the final pattern)
//   DIRTY (the mod's 2D 0B, any    stopped: kQuietMs after the last one (trailing), at most kStaleMs after the first
//   pattern write, <= 5/s)         playing: at most one every kPlayGapMs; the VOICE page none while playing (after STOP)
//   song take (set_hold)          recording / armed: no DIRTY fetch (once after it ends); a pattern change only on the
//                                  PAT page; arming / restoring / replaying (the song's own dumps): nothing at all
// One request at a time; a lost answer times out (kTimeoutMs) and is asked again once, then waits for the next trigger.
// The Rytm's MIDI reaches byte() as it comes (a tap beside the platform's own parser: the answer is longer than its
// SysEx buffer). Measured rates: docs/SONG.md "Traffic to the Rytm" (core/tests/test_traffic.cpp).
#pragma once
#include <stdint.h>

namespace core {
namespace rytm_dump {

constexpr int kRecord = 13101;
constexpr int kAnswer = 14988;
constexpr uint32_t kDirtyMs = 300, kTimeoutMs = 1500;
constexpr uint32_t kPatternMs = 250, kQuietMs = 1000, kStaleMs = 4000, kPlayGapMs = 4000;

int request(uint8_t out[15], int pattern);          // the request's bytes (15): pattern 0..127, else 0x80
// An answer's bytes -> the record (kRecord bytes) and its pattern; false: not a pattern dump, or its checksum / length
// wrong (pure: the tests and the platform's own parsers).
bool parse(const uint8_t* m, int n, uint8_t* record, int* pattern);
// 0.5.0 FILL (core/seq/patwrite.h): the same for another dump id with this framing (0x5A: pattern X, the working buffer's
// answer to 0x6A), and the way back: answer m (kAnswer bytes, as parse took it) carrying `record` instead - its 7-bit
// payload packed again (groups from byte 10), its checksum computed again (as the Rytm's own answers), every other byte kept.
bool parse_id(const uint8_t* m, int n, uint8_t id, uint8_t* record, int* pattern);
void repack(uint8_t* m, const uint8_t* record);

#if CORE_POLY
enum : uint8_t { kOff = 0, kPagePattern = 1, kPageVoice = 2 };
void set_page(int page);                            // who wants the pattern (a fetch at once when it turns on)
void set_active(bool on);                           // set_page(kPagePattern / kOff)
bool active();
int page();
// seq: the Rytm's transport plays; song: 1 a take armed / recording, 2 the song's own dumps or writes (arming,
// restoring, replaying), 0 none
void set_playing(bool on);
bool is_playing();                                  // the transport plays (the pools' P3 guard)
void set_hold(int level);
bool pending();                                     // a fetch due or its answer awaited
// The shown record is older than an edit: 0 no, 1 a fetch follows once the edits settle, 2 held until STOP / the take
int stale();
void want(uint32_t delay_ms = 0);                   // a fetch after delay_ms
void dirty();                                       // seq: 2D 0B, the pattern was written
void pattern_changed(int pattern);                  // seq: 2D 02's active pattern moved
int active_pattern();                               // the last 2D 02's active pattern (0..127), -1 none seen yet
void forget_pattern();                              // seq: a Rytm hello (it rebooted): -1 until the resync's 2D 02
void byte(uint8_t b);                               // the Rytm -> host MIDI stream, every byte (realtime ones ignored)
void tick(uint32_t ms);                             // the control loop: sends the request when due, times out
// The last good record (null: none yet), its pattern and a count that moves with every new one.
const uint8_t* record(int* pattern, uint32_t* gen);
void on_record(void (*fn)(const uint8_t* rec, int n, int pattern));   // called from byte() with each good answer
struct Stats {
  uint32_t requests, answers, bad, timeouts, bytes, dirties;
};
const Stats& stats();
// 0.5.0 FILL (core/seq/patwrite.h): the pattern writer's guard - the DIRTYs (2D 0B) and pattern changes seen so far, and
// its own hold (no fetch of ours while it reads, writes and verifies; the song's set_hold is apart)
uint32_t dirty_count();
uint32_t switch_count();
int hold_level();                                   // set_hold's
void set_fill_hold(bool on);
void reset();                                       // tests
bool command(const char* p, char* out, int cap);    // "rytmpat" (state), "rytmpat=on|voice|off|fetch"
#endif

}  // namespace rytm_dump
}  // namespace core
