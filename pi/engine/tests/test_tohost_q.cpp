// pi/engine/tests/test_tohost_q.cpp - the Sidecar's MIDI queue toward the computer (tohost_q.h): nothing queues, counts or
// flushes while no computer has the gadget configured; configured it flows; both transitions start clean.
#include "check.h"
#include "midi_filter.h"
#include "tohost_q.h"

static const unsigned N = 8;
using Q = ToHostQueue<N>;

static void pkt(uint8_t* p, uint8_t v) { p[0] = 0x09; p[1] = 0x90; p[2] = v; p[3] = 100; }

// the gadget's FromRytmMIDI / ToHost, as sidecar_gadget.cpp does them: counters, skip rule, filter, queue
struct Sim {
  Q q;
  MidiFilter f{false};
  unsigned queued = 0, full = 0, idle_calls = 0;
  void FromRytm(const uint8_t* p) {
    if (!q.IsUp() && !f.InSysEx()) { idle_calls++; return; }
    f.Feed(p, [this](const uint8_t* e) {
      switch (q.Push(e)) {
        case Q::kQueued: queued++; break;
        case Q::kFull: full++; break;
        default: break;
      }
    });
  }
};

int main() {
  uint8_t p[4], out[64];
  pkt(p, 60);

  // never configured: 1e6 messages, nothing queued, no counter moves
  Sim s;
  for (int i = 0; i < 1000000; i++) s.FromRytm(p);
  CHECK(s.queued == 0 && s.full == 0 && s.q.Size() == 0 && s.q.Ups() == 0 && !s.q.IsUp());
  CHECK(s.q.Push(p) == Q::kIdle && s.q.Pop(out, sizeof out) == 0);

  // configured: messages flow in order
  s.q.Up();
  CHECK(s.q.IsUp() && s.q.Ups() == 1);
  for (uint8_t v = 0; v < 5; v++) { pkt(p, v); s.FromRytm(p); }
  CHECK(s.queued == 5 && s.q.Size() == 5);
  CHECK(s.q.Pop(out, 8) == 8 && out[2] == 0 && out[6] == 1);          // whole packets, nmax honoured
  CHECK(s.q.Pop(out, sizeof out) == 12 && out[2] == 2 && out[10] == 4);
  CHECK(s.q.Size() == 0);

  // configured but the computer is not reading: full is counted (the visible fault), bounded by the ring
  for (int i = 0; i < 20; i++) s.FromRytm(p);
  CHECK(s.q.Size() == N && s.full == 12);

  // unplug: back to idle, backlog gone, counters stop
  s.q.Down();
  CHECK(!s.q.IsUp() && s.q.Size() == 0);
  unsigned q0 = s.queued, f0 = s.full;
  for (int i = 0; i < 1000; i++) s.FromRytm(p);
  CHECK(s.queued == q0 && s.full == f0 && s.q.Pop(out, sizeof out) == 0);

  // plug in again with a backlog's worth of idle traffic behind: nothing stale flushes at the new computer
  s.q.Up();
  CHECK(s.q.Ups() == 2 && s.q.Size() == 0 && s.q.Pop(out, sizeof out) == 0);
  pkt(p, 99);
  s.FromRytm(p);
  CHECK(s.q.Pop(out, sizeof out) == 4 && out[2] == 99);

  // plug-in with stale data queued (configured twice without a Down, or a Down missed): Up empties it
  for (int i = 0; i < 3; i++) s.FromRytm(p);
  s.q.Up();
  CHECK(s.q.Size() == 0 && s.q.Ups() == 2);                          // already up: not a new plug-in
  s.q.Down();
  s.q.Down();                                                        // twice: harmless
  CHECK(!s.q.IsUp());

  // a SysEx in flight when the computer goes: the filter keeps being fed until it ends (stays in step), nothing queues
  Sim t;
  t.q.Up();
  uint8_t a[4] = {0x04, 0xF0, 0x7D, 0x01}, b[4] = {0x04, 2, 3, 4}, c[4] = {0x05, 0xF7, 0, 0};
  t.FromRytm(a);
  t.q.Down();
  unsigned before = t.idle_calls;
  t.FromRytm(b);
  t.FromRytm(c);                                                     // fed (InSysEx), but Push says kIdle
  CHECK(t.idle_calls == before && !t.f.InSysEx() && t.q.Size() == 0);
  t.FromRytm(c);                                                     // idle again: skipped
  CHECK(t.idle_calls == before + 1);

  printf("test_tohost_q: PASS\n");
  return 0;
}
