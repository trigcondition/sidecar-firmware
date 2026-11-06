// pi/engine/midi_filter.h - a SysEx filter on a stream of USB MIDI 1.0 event packets (4 bytes: cable/CIN, 3 MIDI
// bytes), for the Sidecar's MIDI pass-through between the Rytm and the computer (sidecar_gadget.h). Packets go
// through in order and unchanged. A SysEx is held back (at most 5 packets, 13 bytes) until its head decides; then it
// passes or is dropped whole, up to its end. Non-SysEx packets go through at once, including real-time bytes (CIN
// 0xF) inside a SysEx.
//
// What is dropped (the predicates below, by direction):
//   both ways      the engine's host-link SysEx F0 7D 52 54 ... (the Rytm and the engine talk to each other only)
//                  MIDI-CI, universal SysEx F0 7E|7F <dev> 0D ... (macOS 26 sends discovery on every port open;
//                  an echoed CI message wedged CoreMIDI's port, measured in P1)
//   to the Rytm    an Elektron OS upgrade, os_upgrade::any below - the one rule the Rytm MIDI bridge uses too
//                  (midibridge.h classify): the .syx image's messages F0 00 20 3C <dev> 00 7E|7F (7E data, 7F
//                  start/end; rytm-emu fw/*.syx, measured) whatever the device byte, and the MIDI RPC's
//                  OsUpgradeStart / Write / End (command 50..52: elkflash, elektroid). Off, this drops at least what
//                  each path dropped before (the union). So other Elektron SysEx whose payload starts 7E|7F (a test
//                  build's 7D, an RPC's 10) is dropped too: a known limit. SetPassOsUpdate(true) (usbmidi=os:on|emu)
//                  lets through only os_upgrade::pass: the image on the stock files' device byte 0C, and the RPC's
//                  50..52; the image shape on any other device byte stays dropped, also then.
//                  the empty SysEx F0 F7 (macOS sends one on a port open, measured; it means nothing)
// Portable: Mac-tested (tests/test_midi_filter.cpp).
#pragma once
#include <stdint.h>
#include <string.h>

// What counts as an Elektron OS upgrade: one rule for the USB-C port's filter (MidiFilter, below) and the Rytm MIDI
// bridge (midibridge.h classify and its os switch). m: a SysEx's first n bytes, F0 first; kDecide bytes decide.
namespace os_upgrade {
constexpr int kDecide = 13;                    // the RPC's command is m[11]: 13 bytes (as the bridge always required)
constexpr uint8_t kImageDevice = 0x0C;         // the device byte of the stock OS .syx files' messages
inline bool elektron(const uint8_t* m, int n) {
  return n >= 6 && m[0] == 0xF0 && m[1] == 0x00 && m[2] == 0x20 && m[3] == 0x3C && m[5] == 0x00;
}
// the .syx image's messages (7E data, 7F start/end), whatever the device byte
inline bool image(const uint8_t* m, int n) { return n >= 7 && elektron(m, n) && (m[6] == 0x7E || m[6] == 0x7F); }
// the MIDI RPC's command, or -1 when it is no RPC: 8-to-7, m[6] holds the first group's top bits, m[7..13] its bytes
// (seq hi, seq lo, resp hi, resp lo, command; a reply's bit refused all the same). Not RPC: analoghealth's channel
// (device 7D) and the Rytm's own dumps and requests (device 07, id 0x50..0x6F at m[6])
inline int rpc_command(const uint8_t* m, int n) {
  if (n < kDecide || !elektron(m, n) || image(m, n)) return -1;
  if (m[4] == 0x7D || (m[4] == 0x07 && m[6] >= 0x50 && m[6] <= 0x6F)) return -1;
  return (uint8_t)((m[11] | ((m[6] & (0x40 >> 4)) ? 0x80 : 0)) & 0x7F);
}
// the RPC's OsUpgradeStart / Write / End
inline bool rpc(const uint8_t* m, int n) {
  int c = rpc_command(m, n);
  return c >= 0x50 && c <= 0x52;
}
// with the setting off: dropped (the union of what the filter and the bridge dropped before)
inline bool any(const uint8_t* m, int n) { return image(m, n) || rpc(m, n); }
// a new upgrade's first message: the image's start (7F 01; its end is 7F 02 - the first and last message of every local
// .syx: Rytm MKII OS 1.72 1.73 1.74, Rytm MKI 1.74, A4 MKII 1.56, Digitakt 1.54, Syntakt 1.42) or the RPC's
// OsUpgradeStart (50)
inline bool start(const uint8_t* m, int n) {
  return (image(m, n) && n >= 8 && m[6] == 0x7F && m[7] == 0x01) || rpc_command(m, n) == 0x50;
}
// with the setting on: let through (the rest of any() stays dropped)
inline bool pass(const uint8_t* m, int n) { return (image(m, n) && m[4] == kImageDevice) || rpc(m, n); }
}  // namespace os_upgrade

class MidiFilter {
 public:
  static const int kHead = os_upgrade::kDecide;      // bytes of a SysEx's head that decide
  static const int kHoldPackets = (kHead + 2) / 3;   // packets held back at most (5), then emitted at once
  static const uint8_t kOsUpdateDevice = os_upgrade::kImageDevice;

  explicit MidiFilter(bool to_rytm) : to_rytm_(to_rytm) {}
  // to the Rytm: OS upgrades (os_upgrade::pass) go through (sidecar_gadget: usbmidi=os:)
  void SetPassOsUpdate(bool pass) { pass_os_update_ = pass; }
  bool PassOsUpdate() const { return pass_os_update_; }
  // after an aborted upgrade (usbmidi_os_path.h): with the pass on, OS messages stay dropped until a start
  // (os_upgrade::start) passes, so the tail of the aborted image never goes through on its own
  void AwaitStart(bool on) { await_start_ = on; }
  bool AwaitingStart() const { return await_start_; }
  void Reset() { held_ = 0; head_n_ = 0; state_ = kIdle; }        // a new source: nothing of the old one's SysEx stays
  // a new destination: the SysEx under way (held or passing) is dropped up to its end, so its rest never goes out as
  // a continuation without a start (which Feed would pass); the next SysEx is decided afresh
  void Cut() {
    if (state_ == kHolding || state_ == kPassing) {
      dropped_ += held_;
      dropped_msgs_++;
      state_ = kDropping;
    }
    held_ = 0;
  }

  // One packet in. emit(const uint8_t p[4]) is called for every packet that goes through (0..4 per call).
  template <class Emit>
  void Feed(const uint8_t* p, Emit emit) {
    uint8_t cin = p[0] & 0x0F;
    bool sysex_part = cin == 0x4 || cin == 0x6 || cin == 0x7 || (cin == 0x5 && (p[1] == 0xF7 || state_ != kIdle));
    if (!sysex_part) {                   // channel, system common, real time: never part of a SysEx
      // passes at once, also inside a SysEx (USB MIDI packets are whole; a SysEx being dropped stays dropped)
      emit(p);
      return;
    }
    bool starts = p[1] == 0xF0;
    bool ends = cin != 0x4;
    if (starts && state_ != kIdle) Abort(emit);   // a new SysEx without the old one's end
    if (state_ == kIdle) {
      if (!starts) {                     // a continuation without a start: pass it, as received
        emit(p);
        return;
      }
      state_ = kHolding;
      held_ = 0;
      head_n_ = 0;
    }
    if (state_ == kPassing) {
      emit(p);
      if (ends) state_ = kIdle;
      return;
    }
    if (state_ == kDropping) {
      dropped_++;
      if (ends) state_ = kIdle;
      return;
    }
    // holding: keep the packet and its bytes, decide when the head is long enough or the SysEx ends
    memcpy(hold_[held_++], p, 4);
    int n = ends ? (cin == 0x5 ? 1 : cin == 0x6 ? 2 : 3) : 3;
    for (int i = 0; i < n && head_n_ < kHead; i++) head_[head_n_++] = p[1 + i];
    if (!ends && head_n_ < kHead) return;
    if (to_rytm_ && OsUpdate(head_, head_n_)) os_msgs_++;
    if (Drop(head_, head_n_)) {
      dropped_ += held_;
      dropped_msgs_++;
      NoteDropped();
      state_ = ends ? kIdle : kDropping;
    } else {
      if (await_start_ && OsUpdate(head_, head_n_)) await_start_ = false;   // the start that ends the wait
      for (int i = 0; i < held_; i++) emit(hold_[i]);
      state_ = ends ? kIdle : kPassing;
    }
    held_ = 0;
  }

  uint32_t DroppedPackets() const { return dropped_; }
  uint32_t DroppedMessages() const { return dropped_msgs_; }
  bool InSysEx() const { return state_ != kIdle; }
  // toward the Rytm: the OS-upgrade messages seen (passed or dropped), for usbmidi_os_path.h's OsGate
  uint32_t OsMessages() const { return os_msgs_; }
  // the head (up to kHead bytes, F0 first) of the last SysEx dropped by a rule, for the log: which rule it was is read
  // off its bytes (sidecar_gadget's mirror line). 0 when none yet. Not for a Cut (that one was not dropped by a rule).
  int LastDropped(uint8_t* out) const {
    memcpy(out, last_, last_n_);
    return last_n_;
  }

  // The predicates (public: the tests use them).
  static bool HostLink(const uint8_t* h, int n) { return n >= 4 && h[1] == 0x7D && h[2] == 0x52 && h[3] == 0x54; }
  static bool MidiCi(const uint8_t* h, int n) { return n >= 4 && (h[1] == 0x7E || h[1] == 0x7F) && h[3] == 0x0D; }
  static bool Empty(const uint8_t* h, int n) { return n == 2 && h[1] == 0xF7; }
  // dropped toward the Rytm while the pass is off, let through while it is on (os_upgrade, above)
  static bool OsUpdate(const uint8_t* h, int n) { return os_upgrade::any(h, n); }
  static bool OsUpdatePass(const uint8_t* h, int n) { return os_upgrade::pass(h, n); }

 private:
  enum State { kIdle, kHolding, kPassing, kDropping };

  bool Drop(const uint8_t* h, int n) const {
    return HostLink(h, n) || MidiCi(h, n) ||
           (to_rytm_ && ((OsUpdate(h, n) && !(pass_os_update_ && OsUpdatePass(h, n) &&
                                              (!await_start_ || os_upgrade::start(h, n)))) || Empty(h, n)));
  }
  void NoteDropped() {
    memcpy(last_, head_, head_n_);
    last_n_ = head_n_;
  }
  // A held SysEx cut short by a status byte or a new F0: its packets go through (undecided: a short one).
  template <class Emit>
  void Abort(Emit emit) {
    if (state_ == kHolding) {
      if (to_rytm_ && OsUpdate(head_, head_n_)) os_msgs_++;
      if (Drop(head_, head_n_)) {
        dropped_ += held_;
        dropped_msgs_++;
        NoteDropped();
      } else {
        if (await_start_ && OsUpdate(head_, head_n_)) await_start_ = false;
        for (int i = 0; i < held_; i++) emit(hold_[i]);
      }
    }
    held_ = 0;
    state_ = kIdle;
  }

  bool to_rytm_;
  bool pass_os_update_ = false, await_start_ = false;
  State state_ = kIdle;
  uint8_t hold_[kHoldPackets][4];
  int held_ = 0;
  uint8_t head_[kHead];
  int head_n_ = 0;
  uint8_t last_[kHead];
  int last_n_ = 0;
  uint32_t dropped_ = 0, dropped_msgs_ = 0, os_msgs_ = 0;
};
