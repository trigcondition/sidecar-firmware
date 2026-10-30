// pi/engine/usbmidi_merge.h - what goes to the Rytm's USB MIDI OUT endpoint, in which order, each Poll of core 0's
// loop (CObUsbTransport::Poll, ob_usb.cpp, is this with Circle behind Io). Portable: tests/test_midibridge.cpp runs
// this same code with a fake endpoint, including a send that fails.
//
// THE ORDER. 1 the Pi's own keyboards' notes (core/keys.h), 2 the computer's MIDI for the Rytm (the Sidecar gadget,
// filtered there), 3 the engine's records (g_ToHost: SysEx, pieces of a long SysEx, LFO CCs), 4 the Rytm MIDI bridge's
// (midibridge.h), paced at midibridge::kRate. USB MIDI has one cable, so a SysEx that is half sent keeps everything
// else back: the computer's (an early return), the engine's (nothing else goes while a piece of the engine's SysEx is
// out without its F7 - a song dump in 3069-byte pieces - for kEngineStall Polls with no piece at most) and the
// bridge's (keys, computer and engine wait while it is open: kMaxHoldMs at most).
//
// REALTIME. The computer's realtime bytes (F8..FF: the DAW's clock and transport) never wait behind a hold: they go
// as CIN F packets inside any SysEx, in the Poll they arrive; the computer's other packets wait their turn in order. Once
// something waits, only the clock (F8) and active sensing (FE) still overtake it: start, continue and stop keep their
// place, so a Song Position Pointer then Continue reach the Rytm in that order.
//
// A FAILED SEND. Circle's bulk transfer blocks until the Rytm takes it, 3 s at most, then fails. Any failed send - the
// engine's, the bridge's, and the keys' or the computer's too - turns all sending off for kRetryUs (5 s, doubling to
// 60 s while it keeps failing) or until a new umidi1 (rytm_new()), and ends that Poll: a Rytm that is enumerated but
// not reading costs one 3 s stall per retry, never one per chunk or per Poll (the watchdog is 15 s), and a Rytm that was
// only busy gets the Sidecar's own link back by itself. The bridge's queue is dropped with it, the half-sent SysEx
// too: the rest of a broken SysEx is never sent with a hole in it. What the computer had deferred is dropped and
// counted at a failure and at a new umidi1 (stale notes, not played seconds late).
#pragma once
#include <stdint.h>
#include <string.h>

#include "midibridge.h"
#include "usbmidi_pack.h"

class UsbMidiMerge {
 public:
  struct Io {
    virtual bool send(const uint8_t* p, unsigned n) = 0;	// one bulk transfer (n <= kChunk), false: failed
    virtual bool key(uint32_t* w) = 0;				// the next keys' packet
    virtual bool has_computer() = 0;				// a Sidecar gadget (USB-C) exists
    virtual void computer_connected(bool up) = 0;		// tell it whether the Rytm can take its MIDI
    virtual bool computer(uint32_t* w) = 0;			// its next packet for the Rytm
    virtual bool computer_in_sysex() = 0;			// a SysEx of its is half sent
    virtual const uint8_t* engine_next(int* len) = 0;		// the next whole record, 0 none
    virtual unsigned engine_drain() = 0;			// drop every record, their bytes
    virtual void warn(const char* what) = 0;			// a failed send turned the sending off (log it)
  };
  static constexpr unsigned kChunk = 512;		// one bulk transfer: the endpoint's max packet (high speed)
  static constexpr unsigned kEngineBudget = 4096;	// engine bytes (and computer packets x 4) a Poll at most
  static constexpr unsigned kEngineStall = 250;		// Polls an engine SysEx may stay open with no piece of it

  // rytm: umidi1 is there; send: the engine's replies are on (tohost=); bridge / pace: 0 without a bridge
  void poll(Io& io, bool rytm, bool send, midibridge::ToRytm* bridge, midibridge::Pacer* pace, uint32_t now_us) {
    now_ = now_us;
    if (failed_ && rytm && now_us - fail_at_ >= retry_us_) {	// a Rytm that was only busy: try again, backing off
      failed_ = false;
      retrying_ = true;
      retries_++;
      retry_us_ = retry_us_ * 2 > kRetryMaxUs ? kRetryMaxUs : retry_us_ * 2;
      io.warn("to-host: trying the Rytm's USB MIDI again after a failed send");
    }
    bool open = bridge != nullptr && bridge->open();
    bool up = rytm && !failed_;
    if (engine_open_ && ++engine_idle_ > kEngineStall) {	// its next piece never came: the others go again
      engine_open_ = false;
      engine_stalls_++;
    }
    bool hold = open || engine_open_;			// a SysEx of the bridge's or the engine's is half sent
    // 1 the keys: between whole messages
    if (up) {
      if (!io.computer_in_sysex() && !hold) {
        unsigned n = 0;
        uint32_t w;
        while (n + 4 <= kChunk && io.key(&w)) {
          memcpy(keys_ + n, &w, 4);
          n += 4;
        }
        if (n != 0 && !io.send(keys_, n)) {
          mac_failed_++;
          fail(io, "to-host: a USB MIDI send of the keys' failed: sending off for a while (then tried again)");
          return;					// one 3 s stall a Poll at most (the watchdog is 15 s)
        }
      }
    } else {
      uint32_t w;
      while (io.key(&w)) {}				// no Rytm: nowhere to play them
    }
    // 2 the computer's. During a hold its realtime bytes (clock, start, stop, continue, active sensing) still go at once:
    // USB MIDI 1.0 lets a CIN F single byte sit inside a SysEx on the same cable. The rest waits in order in def_.
    if (io.has_computer()) {
      io.computer_connected(up);
      if (up) {
        unsigned n = 0;
        uint32_t w;
        if (!hold) {
          for (; dn_ > 0 && !failed_; dn_--, dh_ = (dh_ + 1) % kDefer) put_mac(io, def_[dh_], &n);	// what waited, first
          for (unsigned k = 0; k < kEngineBudget / 4 && !failed_ && io.computer(&w); k++) put_mac(io, w, &n);
        } else {
          // only the clock (F8) and active sensing (FE) overtake what waits: start / continue / stop (FA FB FC) keep
          // their place behind it (a Song Position Pointer then Continue must reach the Rytm in that order)
          for (unsigned k = 0; k < kEngineBudget / 4 && dn_ < kDefer && !failed_ && io.computer(&w); k++) {
            uint8_t st = (uint8_t)(w >> 8);
            if (realtime(w) && (dn_ == 0 || st == 0xF8 || st == 0xFE)) put_mac(io, w, &n);
            else def_[(dh_ + dn_++) % kDefer] = w;
          }
        }
        if (!failed_ && n != 0 && !io.send(mac_, n)) mac_fail(io);
        if (failed_) return;				// a failed send ends the Poll
        if (!hold && io.computer_in_sysex()) return;
      }
    }
    // 3 the engine's
    if (!send || failed_ || !rytm) {
      dropped_ += io.engine_drain();			// whole records
      engine_open_ = false;
    } else if (!open) {
      engine(io);
    }
    // 4 the bridge's
    if (bridge == nullptr) return;
    if (!rytm || failed_) {
      if (bridge->pending()) bridge->flush(false);	// nowhere to send them: dropped, counted
      return;
    }
    if (engine_open_) return;
    unsigned budget = pace != nullptr ? pace->avail(now_us) : midibridge::kPollBytes;
    if (budget > midibridge::kPollBytes) budget = midibridge::kPollBytes;
    uint64_t before = bridge->stats().sent;
    unsigned n = bridge->take(bridge_, sizeof bridge_, budget);
    if (pace != nullptr) pace->spend((unsigned)(bridge->stats().sent - before));
    for (unsigned i = 0; i < n; i += kChunk)
      if (!io.send(bridge_ + i, n - i < kChunk ? n - i : kChunk)) {
        bridge_failed_++;
        fail();
        bridge->flush(false);				// the rest of a broken SysEx is not sent with a hole in it
        io.warn("a USB MIDI send of the Rytm MIDI bridge's failed: sending off for a while (then tried again)");
        break;
      }
    if (n != 0 && !failed_) sent_ok();
  }

  static constexpr uint32_t kRetryUs = 5000000, kRetryMaxUs = 60000000;	// after a failed send: 5 s, doubling to 60
  static constexpr unsigned kDefer = 1024;		// the computer's packets that wait out a hold (its realtime does not)
  // a new umidi1 (or none)
  void rytm_new() { failed_ = false; engine_open_ = false; retry_us_ = kRetryUs; retrying_ = false; drop_deferred(); }
  bool failed() const { return failed_; }
  bool engine_open() const { return engine_open_; }
  unsigned dropped() const { return dropped_; }
  unsigned sent() const { return sent_; }
  unsigned msgs() const { return msgs_; }
  unsigned failures() const { return failures_; }
  unsigned mac_failed() const { return mac_failed_; }
  unsigned bridge_failed() const { return bridge_failed_; }
  unsigned engine_stalls() const { return engine_stalls_; }
  unsigned retries() const { return retries_; }
  unsigned deferred() const { return dn_; }
  unsigned deferred_dropped() const { return deferred_dropped_; }

 private:
  void engine(Io& io) {
    unsigned budget = kEngineBudget;
    int len;
    const uint8_t* rec;
    while (budget > 0 && (rec = io.engine_next(&len)) != nullptr) {
      budget = budget > (unsigned)len ? budget - len : 0;
      int npk = usbmidi_pack(rec, len, pkt_);		// a SysEx, a piece of one, or a channel / realtime message
      if (npk == 0 || failed_) {
        dropped_ += len;
        continue;
      }
      bool ok = true;
      for (int i = 0; ok && i < npk; i += kChunk) ok = io.send(pkt_ + i, (unsigned)npk - i < kChunk ? npk - i : kChunk);
      if (!ok) {
        dropped_ += len;
        failures_++;
        fail();
        io.warn("to-host: a USB MIDI send failed: sending off for a while (then tried again)");
        continue;
      }
      sent_ok();
      sent_ += len;
      msgs_++;
      engine_idle_ = 0;
      if (rec[0] == 0xF0 || !(rec[0] & 0x80)) engine_open_ = rec[len - 1] != 0xF7;	// a SysEx or a piece of one
    }
    if (failed_) engine_open_ = false;
  }
  static bool realtime(uint32_t w) { return (w & 15) == 0x0F && ((w >> 8) & 0xFF) >= 0xF8; }
  void put_mac(Io& io, uint32_t w, unsigned* n) {
    memcpy(mac_ + *n, &w, 4);
    *n += 4;
    if (*n == kChunk) {
      if (!io.send(mac_, *n)) mac_fail(io);
      *n = 0;
    }
  }
  void mac_fail(Io& io) {
    mac_failed_++;
    fail(io, "to-host: a USB MIDI send of the computer's failed: sending off for a while (then tried again)");
  }
  void fail(Io& io, const char* why) {
    fail();
    io.warn(why);
  }
  void fail() {							// what waited is stale by the retry: dropped, counted
    failed_ = true;
    fail_at_ = now_;
    drop_deferred();
  }
  void drop_deferred() {
    deferred_dropped_ += dn_;
    dn_ = dh_ = 0;
  }
  void sent_ok() {						// a send went through after a retry: the back-off starts over
    if (retrying_) { retrying_ = false; retry_us_ = kRetryUs; }
  }
  bool failed_ = false, engine_open_ = false, retrying_ = false;
  uint32_t now_ = 0, fail_at_ = 0, retry_us_ = kRetryUs;
  unsigned retries_ = 0, deferred_dropped_ = 0;
  uint32_t def_[kDefer];
  unsigned dh_ = 0, dn_ = 0;
  unsigned engine_idle_ = 0, engine_stalls_ = 0;
  unsigned dropped_ = 0, sent_ = 0, msgs_ = 0, failures_ = 0, mac_failed_ = 0, bridge_failed_ = 0;
  uint8_t keys_[kChunk], mac_[kChunk];
  uint8_t pkt_[4 * ((4096 + 2) / 3)];			// RecordReader's kMax bytes, packed
  uint8_t bridge_[4 * ((midibridge::kPollBytes + 2) / 3) + 4];
};
