// core/midim/midim.h - the MIDI machine (backlog 7o; Rytm machine 50, rytm-mods 0021 / 0026; docs/MIDI-MACHINE.md): a machine
// that sends MIDI instead of playing. No audio, no render time, no card: a track's trigs, notes and locks become MIDI on a
// chosen destination and channel. Reached through the note machines' entries (core/nmach.h), so a trig, a seq poly track's
// notes, the pads' and the keyboards' notes all arrive here as they reach MULTISAMPLE.
//
// P1..P16 (the Rytm's SRC and SMPL pages; every one a sound word: kits, sounds, p-locks, LFOs):
//   P1 DST  4 zones: OFF DIN USBC USBA        P2 CHAN 16 zones: 1..16
//   P3 BANK 0 OFF, n: CC 0 = n - 1            P4 SBNK 0 OFF, n: CC 32 = n - 1       P5 PROG 0 OFF, n: program n - 1
//   P6 PB   0 OFF, 1..127: 64 centre, 14 bits with the word's fraction
//   P7 AT   P8 MW (CC 1)  0 OFF, 1..127: 0..127
//   P9..P12 VAL1..4: 0..127   P13..P16 SEL1..4: 0 OFF, n: VALk sends on CC n - 1
//
// Sent: on a trig everything wanted that differs from what was last sent, BANK SBNK PROG (the three together: a bank needs its
// program) PB AT MW CCs, then the note; else what changed, under the byte budget (the newest value wins; notes never wait).
// Each voice's sent values are per destination and channel: a change of either sends all again.
// Held notes: a table of (destination, channel, note, voice, source). The off goes where the on went, whatever DST and CHAN say
// now, and only when no other holder of that note is left. A trig is mono on its voice. Nothing hangs: 0x11, the next trig,
// stop, machine or kit change, the link's hello each end what they own.
// DIN: F0 7D 52 54 4F 01 <status & 7F> <d1> [<d2>] F7 to the Rytm (rytm-mods 0026), which sends it on its MIDI OUT.
// A note-off a USB destination could not take (the platform's queue full) is tried again each tick for kOffRetryMs, unless the note was
// played again. A device that echoes (a DAW's MIDI thru, a device with its own thru) would bring each sent note back in through core/keys.h
// and out again for ever: echo() drops a note-on from a destination's own input that matches one sent there in the last kEchoMs, each sent
// note once. A loop slower than that is not caught; a fixed-velocity keyboard on the destination's port, on CHAN, repeating one key faster
// than that loses those repeats.
// LOST ON THE WAY. A DIN note-off is a record on the way to the Rytm: the merge drops every record while a failed send has the sending off (5-60 s,
// pi/engine/usbmidi_merge.h), a queue on the way can be full, and emit cannot tell. So DIN's note-offs are remembered, a set of (channel, note) kept
// kOffKeepMs of a link that is up, 128 notes on 16 channels with nothing to overflow. The platform calls resync() when records were lost and sending is on
// again (DropResync, the remote UI's resync: on the Pi, at most once a second): each remembered off goes again, under the byte budget, unless the note
// is held now. An off that did arrive and goes twice does nothing. link_state(false) stops the clock, so an off survives a link off for any time.
// The control side (the Pi's core 3): no card, no waiting, bounded work; between render blocks.
#pragma once
#include <stdint.h>

namespace core {
namespace midim {

enum Dest : uint8_t { kOff = 0, kDin = 1, kUsbC = 2, kUsbA = 3, kDests = 4 };
constexpr int kCcs = 4;                 // CC slots (P9..P16)
constexpr int kMaxHeld = 64;            // notes held at once, every voice and destination
constexpr int kDinBytesPerSec = 2400;   // non-note data on DIN (31250 baud carries 3125)
constexpr int kUsbBytesPerSec = 8000;   // on a USB destination
constexpr uint8_t kDinCmd = 0x4F;       // host -> Rytm
constexpr uint32_t kOffRetryMs = 1000;  // a note-off the destination refused: tried this long
constexpr uint32_t kEchoMs = 200;       // a sent note coming back in this soon is its echo
constexpr uint32_t kFreshHeldMs = 100;  // kit_switched: 0026's UI tick and the way
constexpr uint32_t kOffKeepMs = 140000; // a DIN note-off is remembered between this and half of it: past the merge's 60 s off and its 3 s stall, twice
constexpr int kTrigSrc = 0xFF;          // a held note's source: the Rytm's trig (else a live channel: poly.h's kSeqCh, a pad's, keys')

// The platform's USB destinations (the Pi: cores.cpp): one channel message, or one realtime byte. false: not sent.
typedef bool (*Out)(int dest, const uint8_t* m, int n);
void set_out(Out f);

// The voice's report: P1..P16; note >= 0 a trig (mono on the voice), -1 parameters only
void report(int vi, int note, int vel, const float* p);
void note_off(int vi);                                         // the Rytm's 0x11: the trig's note
void live_on(int vi, int note, int vel, int ch, int mnote);    // a live note: (ch, mnote) names it for live_off
void live_off(int vi, int ch, int mnote);                      // mnote -1: every note of ch on vi
int voice_notes(int vi);
void silence(int vi);                                          // every note of the voice off now
void leave(int vi);                                            // the voice left the machine: silence, its state forgotten
void kit_changed(int slot = -1);                               // every note off; what was sent forgotten (the new kit's PRG goes out). slot >= 0: that box's voices only
void kit_switched(int slot = -1);                              // a box's 3B named another kit: as kit_changed, but a trig of the last 100 ms stays (the new kit's own step: the 3B comes after it); slot: its voices only
void link_lost();                                              // the Rytm's USB gone (no hello to come): as kit_changed
void resync();                                                 // records for the Rytm were lost, sending is on again: DIN's recent note-offs go again
void link_state(bool up);                                      // each control pass: records can reach the Rytm
bool echo(int dest, int ch, int note, int vel);                // a note-on came in from dest's port: true when it is our own, drop it
void stop(int slot = -1);                                      // slot's transport left PLAY: its voices' trig and sequencer notes off (-1 every voice's)
// Box slot's F8 FA FB FC: to USB-A while that box's voices, and no other box's, send there (decided 2026-11-26: with
// both boxes on USB-A no clock goes). realtime(status) is slot 0's.
void realtime(int slot, uint8_t status);
void realtime(uint8_t status);
int usba_slots();                                              // bit s: a voice of box slot s sends to USB-A
void tick(uint32_t ms);                                        // every control pass: what the budget held back

struct Stats {
  uint32_t ons, offs, values, din, lost, steals;   // lost: a destination's message not sent
  uint32_t retried, echoes;                        // note-offs that went on a later try; note-ons dropped as echoes
  uint32_t din_again;                              // DIN note-offs sent again after a loss of records
  int held, peak;
};
const Stats& stats();
int dest_of(int vi);                                           // the voice's destination now (kOff none), for tests and status
bool command(const char* p, char* out, int cap);               // the console's "midim"
void reset();                                                  // tests

}  // namespace midim
}  // namespace core
