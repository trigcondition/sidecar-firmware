// pi/engine/tohost_q.h - the Sidecar gadget's MIDI queue toward the computer, gated on the gadget being configured.
// Portable: Mac-tested (tests/test_tohost_q.cpp).
//
// No computer on the USB-C port (the GeeekPi case wires it to the display input: the gadget gets no IRQ and is never
// configured): Push () returns kIdle at once, nothing is queued and no counter moves per message. Up () (the host
// configured the MIDI IN endpoint) empties the queue first, so the stream starts with the next message and not with a
// backlog; Down () (reset, deconfigured, suspend torn down) empties it again and goes back to idle.
// Not thread-safe by itself: Up / Down / Push / Pop all run on core 0, the gadget IRQ or with IRQs masked (the caller's
// rule, as for the queue it replaces).
#pragma once
#include <stdint.h>
#include <string.h>

template <unsigned N>
class ToHostQueue {
 public:
  enum Result { kQueued, kIdle, kFull };

  Result Push(const uint8_t* p) {
    if (!up_) return kIdle;
    if (head_ - tail_ >= N) return kFull;
    memcpy(&q_[head_ % N], p, 4);
    head_++;
    return kQueued;
  }
  // up to nmax bytes (whole 4-byte packets) into out; returns the bytes
  unsigned Pop(uint8_t* out, unsigned nmax) {
    unsigned n = 0;
    while (n + 4 <= nmax && tail_ != head_) {
      memcpy(out + n, &q_[tail_ % N], 4);
      tail_++;
      n += 4;
    }
    return n;
  }
  void Up() {
    head_ = tail_ = 0;
    if (!up_) ups_++;
    up_ = true;
  }
  void Down() {
    up_ = false;
    head_ = tail_ = 0;
  }
  bool IsUp() const { return up_; }
  unsigned Ups() const { return ups_; }     // times it went up: bounded by plug-ins, not by messages
  unsigned Size() const { return head_ - tail_; }

 private:
  uint32_t q_[N];
  unsigned head_ = 0, tail_ = 0, ups_ = 0;
  bool up_ = false;
};
