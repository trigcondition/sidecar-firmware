// core/mod/rytm_param.h - the Rytm's own sound parameters modulated from the Sidecar (0.5.0; the binding design and
// contract: notes/design rytm-param-mod.md, sections 1, 2 and 5; docs/RYTM-PARAMS.md). One sender for the remote target
// kinds (core/mod/target.h kRs kRa kRfx: RT1:FRQ, RT2:VEL, RFX:TIM): per (track, word) entry the host wants on the
// Rytm an OFFSET (the sum of the layer's sources: the LFOs, the XY pad, MIDI learn; in steps) and an OVERRIDE (a seq
// p-lock on a poly track: the value, mix 1, a jump at the step's time; the morph: a value and a mix, ramped). The
// Rytm (rytm-mods 0026, always in from 0.5.0) adds them in its audio interrupt after its own modulation and never writes
// the kit. Wire: the 0.5.0 host command F0 7D 52 54 5A 01 <op> ... F7 (core/mod/rytm_out.h kHostCmd):
//   04 OFFSET   <kit> <ramp> <n 1..28> {<t> <w> <hi7> <lo7>} x n     v 14-bit two's complement, 1/64 step
//   05 OVERRIDE <kit> <ramp> <n 1..22> {<t> <w | jump << 6> <hi7> <lo7> <mix 0..127>} x n    v 1/128 step, mix 0 releases
//   06 CLEAR    <kit> <ramp> <mask lo7> <mask hi6>                 every entry of the tracks in the 13-bit mask
//   07 STATUS                                                      answered 2D 01 11 <slots> <rd hi> <lo> <kd hi> <lo>
//                                                                  <timeouts> <flags>
// t 0..12 (12 the FX track), w 1..41 the sound words, 48..51 VM1..4, 52..55 AT1..4, 56 VEL; kit the Rytm's active kit
// as its 3B said (a message for another kit is dropped by the Rytm); ramp in Rytm blocks (32 frames, 0.667 ms).
// Policy (section 2.4):
// - At most one 04 and one 05 every kGridUs; they take turns going first, so neither starves the other under the cap.
// - Only the records that changed (an offset by >= 1/16 step, an override's value or mix; a release first), oldest
//   first when more than a message holds; every live record again every kRefreshMs (the Rytm's 250 ms timeout never
//   fires while the host lives).
// - The whole feature at most kBytesPerS on average: the grid stretches.
// - The ramp: until the record's next update (the grid, the cap's wait, the rounds of a full table, a slow source's
//   pace). The wire has one ramp per message, so records of different paces in one message share the shortest pace,
//   and an op that waits for the other's turn under the cap ramps over its own wait only (up to 2 x short).
// - A p-lock's override at its due time (the step's event + the lead), outside the grid; its bytes a debt of at most
//   100 ms of the cap.
// - After a kit / project change, no grid send until the layer applied the sources' switch (rp_layer_done).
// - 0.5.0 (LFO FINAL, the owner): releases and p-locks keep priority over the LFOs' updates. A p-lock is due outside
//   the grid (above); a release - an override's mix to 0, an offset back to 0 - goes at its op's next grid slot even
//   while the cap's credit is spent (its bytes a debt, as a p-lock's), and first within its message (an offset's
//   passes: 1 a release, 2 a change, 3 a refresh). No cap on the sources: when the offsets that changed wait (the
//   credit spent at their grid slot, or more than a message holds) for kSatMs on end, the sender is saturated
//   (rp_saturated, kSatHoldMs after the last such slot): the LFO screens say "RYTM LINK FULL: LFOs update slower".
// - Nothing while the Rytm's hello lacks caps bit kCapsParamMod, before its 3B named a kit, or after its USB went
//   (until the next hello). A new hello forgets what was sent (the Rytm cleared everything): the next tick sends it
//   all again. A kit change or a project change sends 06 for every track.
// - A refusal (2D 01 10 <op 4..7> <status>) is counted and logged, never the 0.5.0 commands' CC fallback (rytm_out.h
//   rytm_ack). A message with a byte above 0x7f is a bug: refused here (bad), nothing booked as sent.
// Exclusions (rytm_active): words 9..24 of a track playing a Sidecar machine (they are its P1..P16), a machine's empty
// synth slots ("--"; all of 9..16 on DISABLE, BT IMPROVED, FUNC. GEN.). The FX track's compressor words are live (M0b).
// Control side (core 3 on the Pi), cold; CORE_POLY builds only.
#pragma once
#include <stdint.h>

#include "mod/target.h"

namespace core {
namespace mod {

constexpr int kCapsParamMod = 64;     // the hello's caps bit: the Rytm has ops 04..07 (rytm-param-mod 2.3)
enum : uint8_t { kOpOffset = 4, kOpOverride = 5, kOpClear = 6, kOpStatus = 7 };
constexpr uint8_t kPmStatus = 0x11;   // 2D 01 11 ...: 07's answer (core/seq/seq.h kStateCmd kinds)
constexpr int kRpEntries = 128;       // live (track, word) pairs (the Rytm's slots)
constexpr int kRpMaxOffsets = 28, kRpMaxOverrides = 22;   // records a message (<= 123 / 121 B)
constexpr uint32_t kGridUs = 5000;    // one 04 and one 05 at most this often
constexpr uint32_t kRefreshMs = 100;  // every live record again (the keepalive)
constexpr uint32_t kBytesPerS = 16384;   // the feature's cap on the link
constexpr int kOffMinMove = 4;        // 1/64 steps: an offset that moved less is not sent (1/16 step)
// a p-lock's override goes out this long after its step's event is applied (the Pi -> Rytm stream latency minus the
// SysEx path's): a start value, tuned on hardware (rp=lead:US)
constexpr uint32_t kRpLeadUs = 2000;
constexpr int kRpTracks = 13;
constexpr uint32_t kSatMs = 100, kSatHoldMs = 1000;   // rp_saturated: a backlog this long on end; held this long after

// ---- the layer's and the sequencer's side
void rp_offset(Tid t, float steps);    // the summed offset of t this tick (0: none; mod's put / restore)
// the layer let go of t (mod's restore: no source refers to it): its offset back to 0 is a release (LFO FINAL: before
// the changes, past a spent credit), not a source crossing 0
void rp_release(Tid t);
// an override of (track, word): v 0..16383 (1/128 step), mix 0..127 (0 releases), jump (a p-lock: lands at once),
// due_us (now_us()-based; 0 at the next send). Records of seq p-locks: rp_lock, which composes with the morph's
void rp_lock(int track, int word, int v, uint32_t due_us);   // a seq p-lock on (track, word), v 0..16383
void rp_unlock(int track, int word, uint32_t due_us);        // its step has none: the Rytm's own value back (jump)
uint32_t rp_lead_us();                                       // kRpLeadUs, or rp=lead:US
// the morph's parts (rytm-param-mod 5.3): a and b the scenes' values in steps (has_a / has_b: the side locks it), f
// the fader 0..1, step a list word (the side's whole value, switched at halfway: upper). Each tick the morph holds t
void rp_morph(Tid t, bool has_a, float a, bool has_b, float b, float f, bool step, bool upper);
void rp_morph_off(Tid t);                                    // the morph left t: its override released (ramped)
// ---- the link
void rp_hello(int caps);               // the Rytm's hello: caps bit kCapsParamMod; everything sent is forgotten
void rp_kit(int kit);                  // the Rytm's 3B: its active kit 0..127 (a change: 06 for every track)
void rp_project();                     // a project change: 06 for every track
void rp_link_lost();                   // its USB gone: nothing sent until the next hello
void rp_clear_all();                   // 06 for every track now (the layer restored: a stop, a reset), entries forgotten
void rp_ack(int op, int status);       // 2D 01 10 <op 4..7> <status>: 1 refused, 2 the Rytm's slots full
void rp_status(const uint8_t* b, int n);   // 2D 01 11's bytes after the kind
void rp_tick(uint32_t us);             // mod::tick, every pass: due p-locks, the grid, the refresh, the cap
void rp_layer_done();                  // mod::tick after its end_tick: a kit / project change's sources applied
// ---- availability (rytm-param-mod 5.1: a target stays valid, files keep it; an inactive one is not sent)
bool rp_on();                          // the Rytm said caps bit 64 since its last hello, and its USB is there
bool rp_active(Tid t);                 // rp_on and the target allowed on its track now (the machine, the probes)
bool rp_word_active(int track, int word);
// the sender saturated (above: offsets that changed waiting kSatMs on end, held kSatHoldMs): the LFOs update slower
bool rp_saturated();
// a Rytm machine's (the 33's machine number 0..52; rytm-param-mod M0d) synth word 9..16 label ("TUN"), "" an empty slot
// ("--": not sent), null out of the table (a later OS's machine: P1..P8 shown, all allowed)
const char* rp_machine_label(int machine, int word);
// t's name for a page's parameter column: the RT kinds' own ("FRQ"), P1..P8 the track's machine's label ("TUN") when
// known, else "P2"; into b, returns b
const char* rp_label(Tid t, char* b, int cap);
// ---- the console: "rp" (the state: entries, bytes/s, refusals, bad, the Rytm's 07 status), "rp=status" (asks 07),
// "rp=clear" (06 every track, every entry forgotten: a source holding still sends again only when it moves),
// "rp=lead:US", "rp=list" (every live entry). False: not "rp..."
bool rp_command(const char* p, char* out, int cap);
// "\"rp\":{\"on\":1,\"live\":N,\"bps\":B,\"refused\":R,\"full\":F,\"bad\":X,\"sent\":S,\"sat\":0|1}"
int rp_json(char* out, int cap);
// sat_ms: the time saturated since the reset (ms, counted at the grid's slots); rel_cut: releases sent past the credit
struct RpStats { uint32_t offsets, overrides, clears, msgs, bytes, refused, full, dropped, held, bad, sat_ms, rel_cut; };
const RpStats& rp_stats();
// tests: an entry's state (false: none): the offset wanted / sent (1/64 step, INT16_MIN none), the override
struct RpView { int16_t off_sent; uint16_t ovr; uint8_t mix; bool locked, morph; };
bool rp_view(int track, int word, RpView* out);
int rp_live();
void rp_reset();                       // tests (mod::reset): nothing sent, no entries, caps and kit unknown

}  // namespace mod
}  // namespace core
